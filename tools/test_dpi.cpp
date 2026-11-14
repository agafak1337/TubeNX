// tools/test_dpi.cpp — host-тест обхода DPI (SOCKS5 + фрагментация).
//
// Сборка/запуск (host, БЕЗ сети), одна строка:
//   g++ -std=c++17 -O2 -DTNX_HOST -Isource -Itools tools/test_dpi.cpp source/dpi/dpi.cpp -o /tmp/test_dpi -lpthread
//   /tmp/test_dpi
//
// ВАЖНО: -DTNX_HOST обязателен. Без него dpi.cpp тянет заголовки libcurl
// (HTTP-ретранслятор), которых на чистом хосте нет. С -DTNX_HOST остаётся
// чистое POSIX-ядро SOCKS5/фрагментации — ровно то, что этот тест проверяет.
//
// Что проверяем:
//   1) DPI-05: чистая фрагментация. Эталонный ClientHello прогоняется через
//      ВСЕ режимы (Split/MultiSplit/SplitDisorder/TlsRecordSplit/Off),
//      fragmentSize=1..16, fragments=2..32; склейка фрагментов обязана быть
//      побайтово равна исходнику. Счётчик реально порезанных случаев —
//      `fragmented=N`.
//   2) DPI-01: пропускная способность SOCKS-туннеля на localhost. Без
//      внешней сети: поднимаем локальный «bulk»-сервер и качаем через
//      startSocks(), меряем МБ/с. Это тот же socksRelay, что и на консоли.
//   3) Идемпотентность start/stop SOCKS.
//   4) Опционально (TNX_TEST_NET=1): реальный curl через прокси, все режимы
//      и probe() — нужен интернет, по умолчанию пропускается.
//
// Ограничение: HTTP-ретранслятор (libcurl) в host-сборке вырезан, поэтому
// DPI-03 (бюджет сессий) и путь curl->relay здесь НЕ проверяются — это
// отмечено как непроверенное в отчёте.

#include "dpi/dpi.hpp"

#if !defined(TNX_HOST)
#warning "test_dpi.cpp: для host-сборки нужен -DTNX_HOST, иначе dpi.cpp требует libcurl"
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* kProxy = "127.0.0.1:11080";
int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("   %s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool netEnabled() {
    const char* e = std::getenv("TNX_TEST_NET");
    return e && e[0] == '1';
}

// ---------------------------------------------------------------------------
// DPI-05: эталонный ClientHello и проверка фрагментации
// ---------------------------------------------------------------------------

// Тело ClientHello с расширением SNI (www.youtube.com) и supported_versions.
std::vector<unsigned char> buildClientHelloBody() {
    std::vector<unsigned char> ext;
    auto push16 = [](std::vector<unsigned char>& v, unsigned x) {
        v.push_back(static_cast<unsigned char>(x >> 8));
        v.push_back(static_cast<unsigned char>(x));
    };

    // server_name (type 0x0000)
    const char host[] = "www.youtube.com";
    const size_t hl = sizeof(host) - 1;
    std::vector<unsigned char> sn;
    sn.push_back(0x00); // host_name
    push16(sn, static_cast<unsigned>(hl));
    sn.insert(sn.end(), host, host + hl);
    std::vector<unsigned char> sniExt;
    push16(sniExt, 0x0000);
    push16(sniExt, static_cast<unsigned>(2 + sn.size()));
    push16(sniExt, static_cast<unsigned>(sn.size()));
    sniExt.insert(sniExt.end(), sn.begin(), sn.end());
    ext.insert(ext.end(), sniExt.begin(), sniExt.end());

    // supported_versions (type 0x002b) = TLS 1.3
    const unsigned char sv[] = {0x00, 0x2b, 0x00, 0x03, 0x02, 0x03, 0x04};
    ext.insert(ext.end(), sv, sv + sizeof(sv));

    std::vector<unsigned char> b;
    push16(b, 0x0303); // client_version TLS 1.2
    for (int i = 0; i < 32; ++i) b.push_back(static_cast<unsigned char>(i)); // random
    b.push_back(0x00); // session_id len
    push16(b, 2);      // cipher suites len
    push16(b, 0x1301); // TLS_AES_128_GCM_SHA256
    b.push_back(1);
    b.push_back(0);    // compression: null
    push16(b, static_cast<unsigned>(ext.size()));
    b.insert(b.end(), ext.begin(), ext.end());
    return b;
}

std::vector<unsigned char> tlsRecord(unsigned char type,
                                     const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> rec;
    rec.push_back(type);
    rec.push_back(0x03);
    rec.push_back(0x01);
    const unsigned n = static_cast<unsigned>(payload.size());
    rec.push_back(static_cast<unsigned char>(n >> 8));
    rec.push_back(static_cast<unsigned char>(n));
    rec.insert(rec.end(), payload.begin(), payload.end());
    return rec;
}

// Собирает эталонный ClientHello. extraCcs добавляет вторую TLS-запись
// (ChangeCipherSpec), чтобы TlsRecordSplit реально резал по границам записей.
std::vector<unsigned char> makeClientHello(bool extraCcs) {
    std::vector<unsigned char> body = buildClientHelloBody();
    std::vector<unsigned char> hs;
    hs.push_back(0x01); // Handshake: ClientHello
    const unsigned bl = static_cast<unsigned>(body.size());
    hs.push_back(static_cast<unsigned char>(bl >> 16));
    hs.push_back(static_cast<unsigned char>(bl >> 8));
    hs.push_back(static_cast<unsigned char>(bl));
    hs.insert(hs.end(), body.begin(), body.end());

    std::vector<unsigned char> out = tlsRecord(0x16, hs);
    if (extraCcs) {
        std::vector<unsigned char> ccs(1, 0x01);
        std::vector<unsigned char> r = tlsRecord(0x14, ccs);
        out.insert(out.end(), r.begin(), r.end());
    }
    return out;
}

// Прогоняет все режимы/параметры и проверяет, что склейка == исходник.
bool fragTest(const std::vector<unsigned char>& src, const char* label,
              uint64_t& cases, uint64_t& fragmented) {
    using tnx::dpi::Config;
    using tnx::dpi::Mode;
    const Mode modes[] = {Mode::Split, Mode::MultiSplit, Mode::SplitDisorder,
                          Mode::TlsRecordSplit, Mode::Off};
    bool ok = true;
    for (Mode m : modes) {
        for (int fs = 1; fs <= 16; ++fs) {
            for (int fr = 2; fr <= 32; ++fr) {
                Config cfg{};
                cfg.mode = m;
                cfg.fragmentSize = fs;
                cfg.fragments = fr;
                cfg.delayMs = 0; // паузы к байтам отношения не имеют
                cfg.enabled = (m != Mode::Off);

                tnx::dpi::FragmentPlan plan = tnx::dpi::planFragments(
                    reinterpret_cast<const char*>(src.data()), src.size(), cfg);

                std::vector<unsigned char> rebuilt;
                rebuilt.reserve(src.size());
                for (const std::string& ch : plan.chunks) {
                    rebuilt.insert(rebuilt.end(), ch.begin(), ch.end());
                }
                ++cases;
                if (plan.chunks.size() > 1) ++fragmented;

                if (rebuilt != src) {
                    ok = false;
                    std::printf("   FAIL: mode=%d fs=%d fr=%d: got %zu != %zu байт\n",
                                static_cast<int>(m), fs, fr, rebuilt.size(),
                                src.size());
                }
                // applied обязан совпадать с «было >1 фрагмента», кроме
                // Off (никогда) — это упрощает диагностику на консоли.
                if (m == Mode::Off && plan.applied) {
                    ok = false;
                    std::printf("   FAIL: Off выставил applied\n");
                }
            }
        }
    }
    std::printf("   %s: %s (%llu конфигураций, из них порезано %llu)\n", label,
                ok ? "OK" : "FAIL",
                static_cast<unsigned long long>(cases),
                static_cast<unsigned long long>(fragmented));
    return ok;
}

void fragSection() {
    std::printf("== DPI-05: корректность фрагментации ==\n");
    uint64_t cases = 0, fragmented = 0;
    bool ok = true;

    std::vector<unsigned char> hello = makeClientHello(false);
    std::printf("   эталонный ClientHello: %zu байт\n", hello.size());
    ok &= fragTest(hello, "ClientHello (1 запись)", cases, fragmented);

    std::vector<unsigned char> helloCcs = makeClientHello(true);
    std::printf("   ClientHello + CCS: %zu байт\n", helloCcs.size());
    ok &= fragTest(helloCcs, "ClientHello+CCS (2 записи)", cases, fragmented);

    // Граничные длины: ничего не должно теряться/падать.
    for (size_t n = 0; n <= 8; ++n) {
        std::vector<unsigned char> tiny(n);
        for (size_t i = 0; i < n; ++i) tiny[i] = static_cast<unsigned char>(0x40 + i);
        ok &= fragTest(tiny, "граничная длина", cases, fragmented);
    }

    std::printf("   fragmented=%llu\n", static_cast<unsigned long long>(fragmented));
    check(ok, "DPI-05: все режимы сохраняют байты");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// DPI-01: пропускная способность SOCKS-туннеля на localhost
// ---------------------------------------------------------------------------

int connectLocal(uint16_t port, int timeoutMs) {
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7F000001u);
    // Несложный таймаут через SO_SNDTIMEO/RCVTIMEO — на connect не влияет,
    // но защищает последующие блокирующие вызовы.
    struct timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool readN(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t r = ::recv(fd, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

bool writeN(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t r = ::send(fd, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// Локальный «bulk»-сервер: отдаёт total байт и закрывается.
// throttleUs — пауза после каждого 64 КиБ куска (чтобы растянуть поток во
// времени и увидеть периодический `relay: rate=` в логе).
struct BulkServer {
    uint16_t port = 0;
    std::atomic<long long> sent{0};
    std::thread th;

    bool start(size_t total, int throttleUs = 0) {
        int ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls < 0) return false;
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = 0;
        a.sin_addr.s_addr = htonl(0x7F000001u);
        if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
            ::listen(ls, 4) != 0) {
            ::close(ls);
            return false;
        }
        socklen_t al = sizeof(a);
        getsockname(ls, reinterpret_cast<sockaddr*>(&a), &al);
        port = ntohs(a.sin_port);
        th = std::thread([this, ls, total, throttleUs]() {
            int c = ::accept(ls, nullptr, nullptr);
            if (c >= 0) {
                std::vector<char> buf(64 * 1024, 'A');
                size_t left = total;
                while (left > 0) {
                    size_t n = std::min(buf.size(), left);
                    ssize_t w = ::send(c, buf.data(), n, 0);
                    if (w <= 0) break;
                    left -= static_cast<size_t>(w);
                    sent += w;
                    if (throttleUs > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::microseconds(throttleUs));
                    }
                }
                // Вычитываем присланное клиентом (ClientHello). Иначе close()
                // с непрочитанными данными отправит RST и клиент потеряет
                // хвост потока — замер станет заниженным.
                struct timeval tv{};
                tv.tv_usec = 200000; // 200 мс
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                char tmp[8192];
                while (::recv(c, tmp, sizeof(tmp), 0) > 0) {
                }
                ::shutdown(c, SHUT_WR);
                ::close(c);
            }
            ::close(ls);
        });
        return true;
    }
    void join() {
        if (th.joinable()) th.join();
    }
};

// SOCKS5 CONNECT к 127.0.0.1:destPort. Возвращает fd или -1.
int socksConnectIpHack(uint16_t destPort) {
    int fd = connectLocal(static_cast<uint16_t>(tnx::dpi::socksPort()), 5000);
    if (fd < 0) return -1;
    const unsigned char greet[3] = {0x05, 0x01, 0x00};
    unsigned char rep[2] = {0, 0};
    if (!writeN(fd, greet, sizeof(greet)) || !readN(fd, rep, 2) || rep[0] != 0x05 ||
        rep[1] != 0x00) {
        ::close(fd);
        return -1;
    }
    unsigned char req[10] = {0x05, 0x01, 0x00, 0x01, 127, 0, 0, 1, 0, 0};
    req[8] = static_cast<unsigned char>(destPort >> 8);
    req[9] = static_cast<unsigned char>(destPort & 0xFF);
    unsigned char ans[10];
    if (!writeN(fd, req, sizeof(req)) || !readN(fd, ans, sizeof(ans)) || ans[1] != 0x00) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Читает всё до EOF, возвращает число байт и МБ/с.
double drainRate(int fd, size_t& got) {
    std::vector<char> buf(256 * 1024);
    got = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return secs > 0 ? (got / 1048576.0) / secs : 0.0;
}

void throughputSection() {
    std::printf("== DPI-01: пропускная способность SOCKS-туннеля (localhost) ==\n");
    const size_t kTotal = 32u * 1024u * 1024u; // 32 МБ

    // Базовая линия: напрямую с локального сервера.
    BulkServer direct;
    if (!direct.start(kTotal)) {
        check(false, "не удалось поднять bulk-сервер (direct)");
        return;
    }
    int dfd = connectLocal(direct.port, 5000);
    if (dfd >= 0) {
        size_t got = 0;
        double rate = drainRate(dfd, got);
        ::close(dfd);
        std::printf("   напрямую: %zu байт, %.1f МБ/с\n", got, rate);
    }
    direct.join();

    // Через SOCKS5 с фрагментацией ClientHello.
    using namespace tnx::dpi;
    init();
    Config& cfg = config();
    cfg.enabled = true;
    cfg.mode = Mode::MultiSplit;
    cfg.fragmentSize = 4;
    cfg.fragments = 6;
    cfg.delayMs = 0;
    if (!startSocks(11080)) {
        check(false, "startSocks(11080)");
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    BulkServer dist;
    if (!dist.start(kTotal)) {
        check(false, "не удалось поднять bulk-сервер (socks)");
        stopSocks();
        return;
    }
    int sfd = socksConnectIpHack(dist.port);
    bool ok = (sfd >= 0);
    double rate = 0;
    size_t got = 0;
    if (ok) {
        // Первый пакет клиента — эталонный ClientHello: его SOCKS-воркер
        // фрагментирует и только потом начинает ретрансляцию.
        std::vector<unsigned char> hello = makeClientHello(false);
        ok = writeN(sfd, hello.data(), hello.size());
        if (ok) rate = drainRate(sfd, got);
        ::close(sfd);
    }
    dist.join();
    std::printf("   через SOCKS: %zu байт, %.1f МБ/с (сервер отправил %lld)\n", got,
                rate, static_cast<long long>(dist.sent.load()));
    const auto st = tnx::dpi::statsSnapshot(); // копия под мьютексом (DPI-16)
    std::printf("   stats: relayed=%llu fragmented=%llu\n",
                static_cast<unsigned long long>(st.relayedBytes),
                static_cast<unsigned long long>(st.fragmented));
    check(ok && got >= kTotal / 2, "DPI-01: через SOCKS скачано >= половины потока");

    // Растянутый поток (>5 с): проверяем, что периодический `relay: rate=`
    // реально пишется (DPI-01). Строка уходит в stderr через socksLog.
    {
        const size_t slow = 12u * 1024u * 1024u; // 12 МБ
        BulkServer s;
        if (s.start(slow, /*throttleUs=*/30000)) { // ~5.8 с при 64 КиБ кусках
            int fd = socksConnectIpHack(s.port);
            if (fd >= 0) {
                std::vector<unsigned char> hello = makeClientHello(false);
                if (writeN(fd, hello.data(), hello.size())) {
                    size_t g = 0;
                    double r = drainRate(fd, g);
                    std::printf("   throttled: %zu байт, %.2f МБ/с (ожидаем строку "
                                "relay: rate= в stderr)\n",
                                g, r);
                }
                ::close(fd);
            }
            s.join();
        }
    }
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// Опциональная проверка через реальную сеть (TNX_TEST_NET=1)
// ---------------------------------------------------------------------------

long fileSize(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fclose(f);
    return sz;
}

bool fileContains(const char* path, const char* needle) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);
    return all.find(needle) != std::string::npos;
}

int runCurl(const std::string& url, const char* out, int maxTime = 30) {
    std::string cmd = "curl -sS -m " + std::to_string(maxTime) +
                      " --socks5-hostname " + kProxy + " \"" + url + "\" -o " + out +
                      " -w 'http=%{http_code} bytes=%{size_download} "
                      "time=%{time_total}'";
    std::printf("  $ %s\n", cmd.c_str());
    int rc = std::system(cmd.c_str());
    int code = 0;
    if (rc != -1) code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    std::printf("  exit=%d\n", code);
    return code;
}

void dumpStats(const char* tag) {
    const auto s = tnx::dpi::statsSnapshot();
    std::printf("  [%s] connections=%llu fragmented=%llu relayed=%llu errors=%llu "
                "handshake=%.1fms active=%d working=%d\n",
                tag, static_cast<unsigned long long>(s.connections),
                static_cast<unsigned long long>(s.fragmented),
                static_cast<unsigned long long>(s.relayedBytes),
                static_cast<unsigned long long>(s.errors), s.lastHandshakeMs,
                static_cast<int>(s.active), static_cast<int>(s.working));
}

void networkSection() {
    using namespace tnx::dpi;
    std::printf("== Сеть (TNX_TEST_NET=1): curl через SOCKS во всех режимах ==\n");
    if (!startSocks(11080)) {
        check(false, "startSocks");
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    struct Case {
        Mode mode;
        const char* name;
    };
    const Case cases[] = {
        {Mode::Split, "Split"},         {Mode::MultiSplit, "MultiSplit"},
        {Mode::SplitDisorder, "SplitDisorder"}, {Mode::TlsRecordSplit, "TlsRecordSplit"},
        {Mode::Off, "Off"},
    };
    bool ok = true;
    for (const Case& c : cases) {
        setMode(c.mode);
        std::printf("== режим %s ==\n", c.name);
        int rc = runCurl("https://example.com/", "/tmp/tnx_example.html");
        long sz = fileSize("/tmp/tnx_example.html");
        bool body = fileContains("/tmp/tnx_example.html", "Example Domain");
        std::printf("  size=%ld contains='Example Domain'=%d\n", sz,
                    static_cast<int>(body));
        if (rc != 0 || sz <= 0 || !body) ok = false;
        dumpStats(c.name);
    }
    ProbeResult pb = probe("example.com", 443, true);
    std::printf("  probe: ok=%d rtt=%dms status=%ld error='%s'\n",
                static_cast<int>(pb.ok), pb.rttMs, pb.status, pb.error.c_str());
    ok &= pb.ok;
    check(ok, "сеть: все режимы и probe");
    stopSocks();
    std::printf("\n");
}

void lifecycleSection() {
    using namespace tnx::dpi;
    std::printf("== SOCKS: жизненный цикл start/stop ==\n");
    if (!startSocks(11080)) {
        check(false, "повторный startSocks");
        return;
    }
    bool up = (socksPort() == 11080) && stats().active;
    check(up, "SOCKS слушает и active=true");
    stopSocks();
    stopSocks(); // no-op
    check(socksPort() == 0 && !stats().active, "stopSocks идемпотентен");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// DPI-09: кодирование URL ретранслятора
// ---------------------------------------------------------------------------

void urlSection() {
    using namespace tnx::dpi::test;
    std::printf("== DPI-09: кодирование/декодирование URL ретранслятора ==\n");

    // URL из реальных подписанных ссылок googlevideo: есть %, &, +, ?, =, ':'.
    const char* urls[] = {
        "https://rr1---sn-abc.googlevideo.com/videoplayback?expire=1&mn=sn-a"
        "&sparams=ip%2Cid&sig=AbC%2B1%3D&lsparams=abc&plus=a+b",
        "https://example.com/u?x=1&y=%26%3F%25%2B&z=плюс",
        "http://127.0.0.1:9/a%20b%2Fc?q=%7B%7D",
        "https://x/",
    };
    bool ok = true;
    for (const char* u : urls) {
        std::string enc = pctEncodeTest(u);
        std::string dec = pctDecodeTest(enc);
        if (dec != u) {
            ok = false;
            std::printf("   FAIL round-trip: '%s' -> '%s'\n", u, dec.c_str());
        }
    }
    check(ok, "pctEncode/pctDecode: round-trip сохраняет URL");

    // Знаки из ТЗ: %, &, +, ?.
    check(pctDecodeTest("%25") == "%", "pctDecode %25 -> %");
    check(pctDecodeTest("%26") == "&", "pctDecode %26 -> &");
    check(pctDecodeTest("%3F") == "?", "pctDecode %3F -> ? (заглавный hex)");
    check(pctDecodeTest("%3f") == "?", "pctDecode %3f -> ? (строчный hex)");
    check(pctDecodeTest("a+b") == "a+b", "pctDecode: '+' НЕ пробел (RFC 3986)");
    check(pctDecodeTest("%2B") == "+", "pctDecode %2B -> +");
    check(pctDecodeTest("%ZZ") == "%ZZ", "pctDecode: битый %ZZ не теряет байты");
    check(pctDecodeTest("50%") == "50%", "pctDecode: '%' в конце не теряет байт");
    check(pctEncodeTest(" ") == "%20", "pctEncode: пробел -> %20 (не '+')");

    // Извлечение upstream: & и ? внутри значения не ломают разбор.
    std::string up;
    const std::string orig = "https://h/p?a=1&b=2%20c&x=%26";
    bool got = parseRelayUpstreamTest("/u?u=" + pctEncodeTest(orig), up);
    check(got && up == orig, "parseRelayUpstream: & и ? внутри u= не ломают разбор");
    got = parseRelayUpstreamTest("/u?t=1&u=https%3A%2F%2Fh%2F", up);
    check(got && up == "https://h/", "parseRelayUpstream: параметр u не первый");
    got = parseRelayUpstreamTest("/u?t=1", up);
    check(!got, "parseRelayUpstream: без u -> false");

    // Лимит 8 КБ (414).
    const std::string atLimit(8192, 'a');
    const std::string overLimit(8193, 'a');
    check(!relayUrlTooLongTest(atLimit), "URL ровно 8192 байт проходит");
    check(relayUrlTooLongTest(overLimit), "URL 8193 байта -> 414 (> 8 КБ)");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// DPI-10: валидация Range
// ---------------------------------------------------------------------------

void rangeSection() {
    using namespace tnx::dpi::test;
    std::printf("== DPI-10: валидация Range (206/416) ==\n");
    check(validByteRangeTest(""), "пустой Range допустим (Range нет)");
    check(validByteRangeTest("bytes=0-99"), "bytes=0-99");
    check(validByteRangeTest("bytes=100-"), "bytes=100-");
    check(validByteRangeTest("bytes=-500"), "bytes=-500 (суффикс)");
    check(!validByteRangeTest("0-99"), "без 'bytes=' -> невалидный");
    check(!validByteRangeTest("bytes=abc"), "bytes=abc -> невалидный");
    check(!validByteRangeTest("bytes=1-2-3"), "bytes=1-2-3 -> невалидный");
    check(!validByteRangeTest("bytes="), "bytes= -> невалидный");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// DPI-11/DPI-12: маппинг ошибок curl -> HTTP, X-TubeNX-Error, Retry-After, 503
// ---------------------------------------------------------------------------

void recvAll(int fd, std::string& out) {
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
}

bool sendAndRead(int code, const char* ct, const std::string& body,
                 const std::string& extra, std::string& got) {
    int sp[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return false;
    bool ok = tnx::dpi::test::sendResponseTest(sp[0], code, ct, body, extra);
    ::shutdown(sp[0], SHUT_WR);
    got.clear();
    recvAll(sp[1], got);
    ::close(sp[0]);
    ::close(sp[1]);
    return ok;
}

void errorSection() {
    using namespace tnx::dpi::test;
    std::printf("== DPI-11/DPI-12: коды ошибок, X-TubeNX-Error, Retry-After, 503 ==\n");

    check(curlCodeToHttpTest(0) == 0, "curl 0 -> нет ошибки (0)");
    check(curlCodeToHttpTest(28) == 504, "curl 28 (timeout) -> 504");
    check(curlCodeToHttpTest(7) == 502, "curl 7 (нет соединения) -> 502");
    check(curlCodeToHttpTest(6) == 502, "curl 6 (DNS) -> 502");
    check(curlCodeToHttpTest(35) == 502, "curl 35 (SSL) -> 502");
    check(curlCodeToHttpTest(52) == 502, "curl 52 (got nothing) -> 502");

    const std::string h = httpErrorHeadersTest(28, 1);
    check(h.find("X-TubeNX-Error: curl 28") != std::string::npos,
          "заголовок X-TubeNX-Error: curl 28");
    check(h.find("Retry-After: 1") != std::string::npos, "заголовок Retry-After: 1");
    check(httpErrorHeadersTest(0, 0).empty(), "без ошибки/retry заголовков нет");

    // DPI-12: 503 + Retry-After: 1, тело непустое, Content-Length = длине тела.
    const std::string busy = "relay busy: slots full\n";
    std::string got;
    bool sent = sendAndRead(503, "text/plain; charset=utf-8", busy, "Retry-After: 1\r\n", got);
    check(sent, "503 записан клиенту");
    check(got.rfind("HTTP/1.1 503 ", 0) == 0, "ответ начинается с 'HTTP/1.1 503'");
    check(got.find("Retry-After: 1\r\n") != std::string::npos, "503 несёт Retry-After: 1");
    check(got.find("Content-Length: " + std::to_string(busy.size()) + "\r\n") !=
              std::string::npos,
          "503: Content-Length совпадает с телом");
    check(got.find(busy) != std::string::npos, "503: тело непустое и читаемое");

    // DPI-11: 504 (timeout upstream) с X-TubeNX-Error и текстом curl в теле;
    // НИКОГДА не 200 с пустым телом.
    const std::string body = "relay error 504 (curl 28): Operation timed out\n";
    sent = sendAndRead(504, "text/plain; charset=utf-8", body, h, got);
    check(sent && got.rfind("HTTP/1.1 504 ", 0) == 0, "504 выдан на timeout upstream");
    check(got.find("X-TubeNX-Error: curl 28") != std::string::npos, "504 несёт X-TubeNX-Error");
    check(got.find("curl 28") != std::string::npos, "тело 504 содержит 'curl 28'");
    check(got.find("HTTP/1.1 200") == std::string::npos, "ошибка НЕ отдаётся как 200");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// DPI-16: /healthz
// ---------------------------------------------------------------------------

void healthzSection() {
    using namespace tnx::dpi;
    std::printf("== DPI-16: /healthz ==\n");
    Stats s;
    s.connections = 5;
    s.fragmented = 3;
    s.relayedBytes = 12345;
    s.errors = 1;
    s.rateKbps = 2048.5f;
    s.active = true;
    s.sslSessions = 7;
    s.lastError = "curl 7";
    const std::string a = test::healthzTest(s, 1, 1, 0, 2);
    check(a.find("connections=5") != std::string::npos, "healthz: connections");
    check(a.find("relayed=12345") != std::string::npos, "healthz: relayed");
    check(a.find("active=1") != std::string::npos, "healthz: active");
    check(a.find("sslSessions=7") != std::string::npos, "healthz: sslSessions");
    check(a.find("lastError=curl 7") != std::string::npos, "healthz: lastError");
    check(a.find("rate=") != std::string::npos && a.find("slots=2") != std::string::npos,
          "healthz: rate и slots");
    check(!a.empty() && a.back() == '\n', "healthz: валидная строка с \\n на конце");

    s.relayedBytes = 99999;
    s.sslSessions = 8;
    s.errors = 42;
    const std::string b = test::healthzTest(s, 2, 1, 1, 3);
    check(a != b && b.find("relayed=99999") != std::string::npos &&
              b.find("errors=42") != std::string::npos,
          "healthz: цифры меняются вместе со статистикой");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// DPI-06: автовыбор режима и перепроверка при ошибках
// ---------------------------------------------------------------------------

void autoSelectSection() {
    using namespace tnx::dpi;
    std::printf("== DPI-06: автовыбор режима (порядок Off->Split->MultiSplit->TLS) ==\n");
    check(test::selectWorkingModeTest(true, true, true, true) == 0,
          "Off рабочий -> выбран Off (индекс 0)");
    check(test::selectWorkingModeTest(false, true, true, true) == 1,
          "Off не работает -> Split (1)");
    check(test::selectWorkingModeTest(false, false, true, true) == 2,
          "-> MultiSplit (2)");
    check(test::selectWorkingModeTest(false, false, false, true) == 3,
          "-> TlsRecordSplit (3)");
    check(test::selectWorkingModeTest(false, false, false, false) == -1,
          "ничего не работает -> -1");

    Config& cfg = config();
    cfg.modeAutoPicked = true;
    cfg.autoReprobeErrors = 3;
    test::setAutoBaselineErrorsTest(stats().errors);
    setVideoPlaying(false);
    check(!shouldReprobeOnErrors(), "ошибок меньше порога -> не перепроверяем");
    stats().errors += 3;
    check(shouldReprobeOnErrors(), "ошибок >= порога -> пора перепроверить");
    setVideoPlaying(true);
    check(!shouldReprobeOnErrors(), "во время видео перепроверка запрещена");
    setVideoPlaying(false);
    cfg.modeAutoPicked = false;
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// DPI-13: локальность (только 127.0.0.1, случайный порт, недоступность извне)
// ---------------------------------------------------------------------------

void localitySection() {
    using namespace tnx::dpi::test;
    std::printf("== DPI-13: слушаем только 127.0.0.1 ==\n");

    int p1 = 0;
    int fd1 = bindLocalListenerTest(0, p1);
    bool loop = false;
    sockaddr_in a{};
    socklen_t al = sizeof(a);
    if (fd1 >= 0 && getsockname(fd1, reinterpret_cast<sockaddr*>(&a), &al) == 0) {
        loop = (a.sin_family == AF_INET) && ((ntohl(a.sin_addr.s_addr) >> 24) == 127);
    }
    check(fd1 >= 0 && p1 > 0, "слушатель поднят на свободном порту");
    check(loop, "адрес прослушивания — 127.0.0.1 (не 0.0.0.0)");

    // Занятый порт -> другой (случайный) свободный порт.
    int p2 = 0;
    int fd2 = bindLocalListenerTest(p1, p2);
    check(fd2 >= 0 && p2 > 0 && p2 != p1, "занятый порт -> случайный свободный порт");

    // Недоступность с внешнего (не-loopback) адреса.
    bool tested = false, refused = true;
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* it = ifs; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
            auto* in4 = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
            const uint32_t host = ntohl(in4->sin_addr.s_addr);
            if ((host >> 24) == 127 || host == 0) continue;
            int pf = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (pf < 0) break;
            int fl = fcntl(pf, F_GETFL, 0);
            if (fl >= 0) fcntl(pf, F_SETFL, fl | O_NONBLOCK);
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(static_cast<uint16_t>(p1));
            dst.sin_addr = in4->sin_addr;
            tested = true;
            int rc = ::connect(pf, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            if (rc != 0 && errno == EINPROGRESS) {
                pollfd pfd{pf, POLLOUT, 0};
                if (poll(&pfd, 1, 1000) > 0) {
                    int err = 0;
                    socklen_t el = sizeof(err);
                    getsockopt(pf, SOL_SOCKET, SO_ERROR, &err, &el);
                    refused = (err != 0); // соединение не установилось
                }
                // таймаут (>= 1 с) тоже считаем недоступностью
            } else if (rc == 0) {
                refused = false;
            }
            ::close(pf);
            break;
        }
        freeifaddrs(ifs);
    }
    if (fd2 >= 0) ::close(fd2);
    if (fd1 >= 0) ::close(fd1);
    if (tested) {
        check(refused, "с внешнего IP loopback-слушатель недоступен");
    } else {
        std::printf("   SKIP: внешнего IPv4-интерфейса нет\n");
    }
    std::printf("   проверка вручную: ss -ltn | grep -E '127\\.0\\.0\\.1:(1080|8899)'\n");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// DPI-14: 100 циклов start/stop (нет гонок/двойного close, стоп < 1 с)
// ---------------------------------------------------------------------------

void cyclesSection() {
    using namespace tnx::dpi;
    std::printf("== DPI-14: 100 циклов start/stop SOCKS ==\n");
    const int port = 11081;
    bool allOk = true;
    double maxStopMs = 0;
    for (int i = 0; i < 100; ++i) {
        if (!startSocks(port) || socksPort() != port || !stats().active) {
            allOk = false;
            std::printf("   FAIL: start #%d\n", i);
            break;
        }
        const auto t0 = std::chrono::steady_clock::now();
        stopSocks();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        if (ms > maxStopMs) maxStopMs = ms;
        if (socksPort() != 0 || stats().active) {
            allOk = false;
            std::printf("   FAIL: stop #%d\n", i);
            break;
        }
    }
    std::printf("   максимум остановки: %.1f мс\n", maxStopMs);
    check(allOk, "100 циклов start/stop без падений");
    check(maxStopMs < 1000.0, "остановка укладывается в < 1 с");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// DPI-15: fuzz парсеров (фиксированный seed)
// ---------------------------------------------------------------------------

uint64_t g_seed = 0x123456789abcdef0ULL;
uint32_t xorshift() {
    uint64_t x = g_seed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_seed = x;
    return static_cast<uint32_t>(x & 0xFFFFFFFFu);
}

void fuzzSection() {
    using namespace tnx::dpi::test;
    std::printf("== DPI-15: fuzz readHttpRequest/socksHandshake (фикс. seed) ==\n");

    // 1) Чистый разбор HTTP: 1 млн коротких/битых входов, без сокетов.
    //    Половина — случайные байты, половина — порезанные/испорченные
    //    осмысленные запросы (чтобы разбор заголовков тоже прогонялся).
    const std::string seedReq =
        "GET /u?u=https%3A%2F%2Fh%2Fp%3Fa%3D1 HTTP/1.1\r\n"
        "Host: 127.0.0.1:8899\r\nRange: bytes=0-1023\r\nConnection: close\r\n\r\n";
    const int kHttp = 1000000;
    int parsed = 0;
    for (int i = 0; i < kHttp; ++i) {
        std::string s;
        const uint32_t mode = xorshift() % 3;
        if (mode == 0) {
            const size_t len = xorshift() % 64;
            s.reserve(len);
            for (size_t j = 0; j < len; ++j) s.push_back(static_cast<char>(xorshift() & 0xFF));
        } else if (mode == 1) {
            s = seedReq.substr(0, xorshift() % (seedReq.size() + 1)); // обрезка
        } else {
            s = seedReq;
            if (!s.empty()) s[xorshift() % s.size()] = static_cast<char>(xorshift() & 0xFF);
        }
        std::string m, p;
        if (parseHttpRequestTest(s, m, p) >= 0) ++parsed;
    }
    std::printf("   parseHttpRequest: %d входов, разобрано %d, падений 0\n", kHttp, parsed);

    // 2) readHttpRequest и socksHandshake — через socketpair; реальный
    //    upstream-connect отключён, поэтому битый ввод не уходит в сеть.
    setSocksUpstreamDisabledTest(true);
    const int kSock = 20000;
    int hsUpstream = 0;
    for (int i = 0; i < kSock; ++i) {
        const size_t len = xorshift() % 48;
        std::string s;
        s.reserve(len);
        for (size_t j = 0; j < len; ++j) s.push_back(static_cast<char>(xorshift() & 0xFF));

        int sp[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) continue;
        struct timeval tv{};
        tv.tv_usec = 20000; // 20 мс: мёртвый ввод не должен висеть
        setsockopt(sp[0], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (len > 0) writeN(sp[1], s.data(), s.size());
        ::shutdown(sp[1], SHUT_WR);
        std::string m, p;
        readHttpRequestTest(sp[0], m, p);
        ::close(sp[0]);
        ::close(sp[1]);

        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) continue;
        setsockopt(sp[0], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (len > 0) writeN(sp[1], s.data(), s.size());
        ::shutdown(sp[1], SHUT_WR);
        bool frag = false;
        int up = socksHandshakeTest(sp[0], &frag);
        if (up >= 0) {
            ++hsUpstream;
            ::close(up);
        }
        ::close(sp[0]);
        ::close(sp[1]);
    }
    setSocksUpstreamDisabledTest(false);
    std::printf("   readHttpRequest/socksHandshake: %d входов, падений 0 (upstream=%d)\n",
                kSock, hsUpstream);
    check(true, "DPI-15: fuzz без падений");
    std::printf("\n");
}

} // namespace

int main() {
    std::printf("TubeNX · host-тест DPI (SOCKS5 + фрагментация + ретранслятор)\n\n");

    fragSection();
    urlSection();
    rangeSection();
    errorSection();
    healthzSection();
    autoSelectSection();
    localitySection();
    throughputSection();
    lifecycleSection();
    cyclesSection();
    fuzzSection();

    if (netEnabled()) {
        networkSection();
    } else {
        std::printf("== Сеть: ПРОПУЩЕНО (задайте TNX_TEST_NET=1 для проверки "
                    "через интернет) ==\n\n");
    }

    const bool pass = (g_failures == 0);
    std::printf("ИТОГ: %s (провалов %d)\n", pass ? "PASS" : "FAIL", g_failures);
    return pass ? 0 : 1;
}
