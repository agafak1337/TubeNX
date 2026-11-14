// net/sni.cpp — обход SNI-блокировки YouTube.
//
// Провайдерские DPI (в т.ч. РКН) вырезают TLS-рукопожатие, если в ClientHello
// встречается заблокированное имя — сравнение при этом регистрозависимое.
// Проверено на реальной сети: `www.youtube.com` -> таймаут, `WWW.YOUTUBE.COM`
// -> нормальный TLS 1.3 и ответ сервера. По RFC 6066 SNI регистронезависим,
// поэтому верхний регистр не ломает ни TLS, ни проверку сертификата.
//
// libcurl и ffmpeg в devkitPro-порту ходят в TLS через системный сервис
// `ssl` (libnx), а не через mbedTLS: имя хоста задаётся одним вызовом
// sslConnectionSetHostName(). Перехватываем его линкером (--wrap) и
// поднимаем регистр — этого достаточно и для API, и для видео.
//
// DPI-07: сервис ssl задаёт ОДНОЙ строкой и байты SNI, и имя для проверки
// сертификата; разделить их API не позволяет. Поэтому в лог пишем фактически
// отправленное имя (sni=) и исходное (orig=), а также результат рукопожатия —
// по логу boot видно, проходит ли `Full` с именем в верхнем регистре. Если
// нет — приложение откатывается на ChainOnly не молча (NET-14 логирует
// причину). Нижний регистр для `youtube.com`/`ytimg.com` на этой сети не
// работает (провайдер режет), поэтому подъём регистра остаётся обязательным.
#include <switch.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>

#include "net/http.hpp" // DPI-07/DPI-08: единая SNI-политика (sniSpoofName/...)
#include "platform.hpp"

namespace {

// DPI-08: строгий suffix-match и список хостов вынесены в net/http.hpp
// (hostMatchesDomain/sniSpoofName/sniNeedsUpper) — один источник истины,
// который проверяется host-тестом. Раньше здесь был нестрогий endsWith,
// из-за которого подмена могла сработать для evilgooglevideo.com.

// Имя можно менять на время одного соединения: ретранслятор перебирает
// кандидатов, если провайдер начнёт резать и первое. Переменная thread_local:
// libcurl работает в отдельном потоке на каждое соединение, так что гонок нет.
thread_local const char* g_spoofOverride = nullptr;

// DPI-07: что реально ушло в sslConnectionSetHostName и что было запрошено.
// Заполняется в обёртке setHostName и попадает в строку результата handshake.
thread_local char g_sniSent[256] = {0};
thread_local char g_sniOrig[256] = {0};

void rememberSni(const char* orig, const char* sent) {
    const auto copy = [](char* dst, size_t cap, const char* src) {
        if (!src) {
            dst[0] = 0;
            return;
        }
        size_t n = strlen(src);
        if (n >= cap) n = cap - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    };
    copy(g_sniOrig, sizeof(g_sniOrig), orig);
    copy(g_sniSent, sizeof(g_sniSent), sent);
}

} // namespace

extern "C" Result __real_sslConnectionSetHostName(SslConnection* c, const char* str, u32 str_bufsize);

// Выбор имени для следующих рукопожатий в текущем потоке. nullptr — умолчание.
extern "C" void tubeSpoofSetName(const char* name) {
    g_spoofOverride = name;
}

// LOG-04/NET-05: пишем не больше 8 строк sni за запуск и только когда имя
// реально меняется (подмена/регистр). Иначе «без изменений» засоряет лог.
std::atomic<int> g_sniLogged{0};
bool shouldLogSni(const char* from, const char* to) {
    static thread_local std::string lastFrom, lastTo;
    const std::string f = from ? from : "";
    const std::string t = to ? to : "";
    if (f == lastFrom && t == lastTo) return false;
    lastFrom = f;
    lastTo = t;
    if (g_sniLogged.fetch_add(1) >= 8) return false;
    return true;
}

extern "C" Result __wrap_sslConnectionSetHostName(SslConnection* c, const char* str, u32 str_bufsize) {
    if (!str || !*str) {
        rememberSni(str, str);
        return __real_sslConnectionSetHostName(c, str, str_bufsize);
    }

    // DPI-08: подмена SNI только для строго совпадающих видео-хостов
    // (*.googlevideo.com). example.com и evilgooglevideo.com сюда не попадают.
    const char* spoof = tnx::net::sniSpoofName(str);
    if (spoof) {
        const char* name = g_spoofOverride ? g_spoofOverride : spoof;
        const size_t n = strlen(name);
        u32 sz = str_bufsize ? str_bufsize : (u32)n;
        if (sz > n) sz = (u32)n;
        if (sz > 0xff) sz = 0xff;
        rememberSni(str, name);
        if (shouldLogSni(str, name)) {
            // DPI-08: в логе ровно то, что нужно для приёмки.
            tnx::platform::bootLog("sni: spoof=%s host=%s (подмена SNI)", name, str);
        }
        return __real_sslConnectionSetHostName(c, name, sz);
    }

    // DPI-07: для youtube.com/ytimg.com поднимаем регистр SNI (обход DPI,
    // проверено на сети). Проверку сертификата для них не отключаем — режим
    // (Full/ChainOnly) задаёт вызывающий через NET-14.
    if (!tnx::net::sniNeedsUpper(str)) {
        rememberSni(str, str);
        return __real_sslConnectionSetHostName(c, str, str_bufsize);
    }

    char buf[256];
    size_t n = strlen(str);
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    for (size_t i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)str[i]);
    buf[n] = 0;

    // str_bufsize у сервиса ограничен 0xff — не выходим за него.
    u32 sz = str_bufsize ? str_bufsize : (u32)n;
    if (sz > n) sz = (u32)n;
    if (sz > 0xff) sz = 0xff;
    rememberSni(str, buf);
    if (shouldLogSni(str, buf)) {
        tnx::platform::bootLog("sni: host=%s -> %s (регистр)", str, buf);
    }
    return __real_sslConnectionSetHostName(c, buf, sz);
}
// ---------------------------------------------------------------------------
// Диагностика TLS-стека.
//
// На консоли рукопожатие иногда занимает секунды, и тогда curl упирается в
// connect-таймаут. Чтобы понять, кто именно тормозит — сервис ssl, импорт
// CA-бандла (у нас 148 сертификатов!) или сеть — измеряем каждый вызов.
// ---------------------------------------------------------------------------
namespace {

long long msNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

std::atomic<int> g_slowLogged{0};

void noteSlow(const char* what, long long ms, long rc) {
    if (ms < 250 && rc == 0) return;
    if (g_slowLogged.fetch_add(1) >= 60) return;
    // NET-04: rc знаковый — иначе -1 превращался в 0xffffffff и «пугал».
    if (rc < 0) {
        tnx::platform::bootLog("ssl: %s %lld мс, rc=%ld (ошибка)", what, ms, rc);
    } else {
        tnx::platform::bootLog("ssl: %s %lld мс, rc=0x%lx", what, ms, (unsigned long)rc);
    }
}

std::atomic<int> g_pkiCalls{0};
std::atomic<long long> g_pkiMs{0};

} // namespace

extern "C" Result __real_sslCreateContext(SslContext* c, u32 ssl_version);
extern "C" Result __wrap_sslCreateContext(SslContext* c, u32 ssl_version) {
    const long long t0 = msNow();
    Result rc = __real_sslCreateContext(c, ssl_version);
    noteSlow("CreateContext", msNow() - t0, rc);
    return rc;
}

extern "C" Result __real_sslContextCreateConnection(SslContext* c, SslConnection* conn);
extern "C" Result __wrap_sslContextCreateConnection(SslContext* c, SslConnection* conn) {
    const long long t0 = msNow();
    Result rc = __real_sslContextCreateConnection(c, conn);
    noteSlow("CreateConnection", msNow() - t0, rc);
    return rc;
}

extern "C" int __real_socketSslConnectionSetSocketDescriptor(SslConnection* c, int sockfd);

// NET-04: отказ сервиса ssl принять сокет делает рукопожатие бессмысленным —
// но отказом считается только rc<0 с errno != ENOENT.
//
// libnx (switch/runtime/devices/socket.h, про socketSslConnectionSetSocketDescriptor)
// пишет дословно: «Returns the output sockfd on success and -1 on error.
// errno==ENOENT indicates that no sockfd was returned, this error must be
// ignored.» То есть rc==-1 при errno==ENOENT — штатный исход: сокет сервисом
// принят, новый fd просто не возвращён. Раньше мы считали это отказом,
// из-за чего handshake пропускался на каждом соединении и весь HTTPS падал
// с `curl 35: SSL connect error`.
//
// Флаг читает обёртка DoHandshake: она не продолжит handshake, а вернёт ошибку.
thread_local bool g_badSocket = false;
thread_local int g_badSocketRc = 0;

extern "C" int __wrap_socketSslConnectionSetSocketDescriptor(SslConnection* c, int sockfd) {
    const long long t0 = msNow();
    errno = 0;
    int rc = __real_socketSslConnectionSetSocketDescriptor(c, sockfd);
    const int err = errno;
    if (rc < 0 && err == ENOENT) {
        // Штатный исход libnx: сокет принят, fd не возвращён. Handshake нужен.
        g_badSocket = false;
        g_badSocketRc = 0;
        static std::atomic<bool> enoentLogged{false};
        if (!enoentLogged.exchange(true)) {
            tnx::platform::bootLog(
                "ssl: SetSocketDescriptor(sockfd=%d) rc=-1/ENOENT — норма (fd не возвращён, "
                "сокет принят)",
                sockfd);
        }
        noteSlow("SetSocketDescriptor", msNow() - t0, rc);
    } else if (rc < 0) {
        g_badSocket = true;
        g_badSocketRc = rc;
        tnx::platform::bootLog(
            "ssl: SetSocketDescriptor(sockfd=%d) -> rc=%d, errno=%d (%s): сокет не принят "
            "сервисом ssl, рукопожатие невозможно",
            sockfd, rc, err, std::strerror(err));
    } else {
        g_badSocket = false;
        // Одна строка об успехе — чтобы в логе было видно rc=0, а не только ошибки.
        static std::atomic<bool> okLogged{false};
        if (!okLogged.exchange(true)) {
            tnx::platform::bootLog("ssl: SetSocketDescriptor rc=0 (успех)");
        }
        noteSlow("SetSocketDescriptor", msNow() - t0, rc);
    }
    // libcurl (Curl_libnx_connect) проверяет не только rc==-1, но и errno==ENOENT:
    // лог и noteSlow могут затереть errno, поэтому возвращаем его как было.
    errno = err;
    return rc;
}

extern "C" Result __real_sslContextImportServerPki(SslContext* c, const void* buffer, u32 size,
                                                   SslCertificateFormat format, u64* id);
extern "C" Result __wrap_sslContextImportServerPki(SslContext* c, const void* buffer, u32 size,
                                                   SslCertificateFormat format, u64* id) {
    const long long t0 = msNow();
    Result rc = __real_sslContextImportServerPki(c, buffer, size, format, id);
    const long long dt = msNow() - t0;
    const int n = g_pkiCalls.fetch_add(1) + 1;
    const long long total = g_pkiMs.fetch_add(dt) + dt;
    // Бандл целиком — 148 сертификатов. Если импорт идёт на каждое соединение,
    // в логе будет видно «148 вызовов» подряд.
    if (n % 40 == 0 || dt >= 100) {
        tnx::platform::bootLog("ssl: ImportServerPki #%d, %lld мс (всего %lld мс), size=%u",
                               n, dt, total, (unsigned)size);
    }
    return rc;
}

// NET-05: 0x1987b — это SSL_ERR_WOULD_BLOCK, нормальное неблокирующее
// ожидание, а не ошибка. Раньше каждая такая итерация писала строку в лог.
constexpr Result kSslWouldBlock = 0x1987b;

// Агрегация одного рукопожатия по указателю соединения: libcurl зовёт
// DoHandshake в цикле, пока не завершит. Пишем ровно одну строку на итог.
thread_local SslConnection* g_hsConn = nullptr;
thread_local long long g_hsStart = 0;
thread_local int g_hsWould = 0;
thread_local int g_hsCalls = 0;

extern "C" Result __real_sslConnectionDoHandshake(SslConnection* c, u32* out_size,
                                                  u32* total_certs, void* server_certbuf,
                                                  u32 server_certbuf_size);
extern "C" Result __wrap_sslConnectionDoHandshake(SslConnection* c, u32* out_size,
                                                  u32* total_certs, void* server_certbuf,
                                                  u32 server_certbuf_size) {
    // NET-04: сокет не приняли — handshake не начинаем.
    if (g_badSocket) {
        const int brc = g_badSocketRc;
        g_badSocket = false;
        tnx::platform::bootLog("ssl: handshake пропущен: сокет не принят (rc=%d)", brc);
        return static_cast<Result>(brc);
    }

    const long long t0 = msNow();
    Result rc = __real_sslConnectionDoHandshake(c, out_size, total_certs, server_certbuf,
                                                server_certbuf_size);

    if (g_hsConn != c) {
        // Другое соединение: предыдущее рукопожатие так и не завершилось —
        // фиксируем его и начинаем новое.
        if (g_hsConn && g_hsWould > 0) {
            tnx::platform::bootLog("ssl: handshake не завершён (ожиданий WOULD_BLOCK %d)", g_hsWould);
        }
        g_hsConn = c;
        g_hsStart = t0;
        g_hsWould = 0;
        g_hsCalls = 0;
    }
    ++g_hsCalls;
    if (rc == kSslWouldBlock) {
        ++g_hsWould; // не ошибка: просто ждём готовности сокета
        return rc;
    }

    const long long dt = msNow() - g_hsStart;
    // DPI-07: в строке результата — какое имя реально ушло в
    // sslConnectionSetHostName и что было запрошено вызывающим.
    const char* sent = g_sniSent[0] ? g_sniSent : "-";
    const char* orig = g_sniOrig[0] ? g_sniOrig : "-";
    if (rc == 0) {
        tnx::platform::bootLog("ssl: handshake ok за %lld мс (sni=%s orig=%s, вызовов %d, "
                               "ожиданий %d)", dt, sent, orig, g_hsCalls, g_hsWould);
    } else if (g_slowLogged.fetch_add(1) < 60) {
        tnx::platform::bootLog("ssl: handshake ошибка rc=0x%lx за %lld мс (sni=%s orig=%s, "
                               "ожиданий %d)", (unsigned long)rc, dt, sent, orig, g_hsWould);
    }
    g_hsConn = nullptr;
    g_hsWould = 0;
    g_hsCalls = 0;
    return rc;
}

// libcurl просит у сервиса ssl 3 сессии (Curl_libnx_init: sslInitialize(3)).
// Мы грузим сеть из нескольких потоков одновременно (обложки, InnerTube,
// ретранслятор видео), поэтому просим максимум — 4. Больше сервис не умеет.
extern "C" Result __real_sslInitialize(u32 num_sessions);
extern "C" Result __wrap_sslInitialize(u32 num_sessions) {
    const u32 want = num_sessions < 4 ? 4 : num_sessions;
    const long long t0 = msNow();
    Result rc = __real_sslInitialize(want);
    tnx::platform::bootLog("ssl: Initialize(%u) -> 0x%x за %lld мс", want, (unsigned)rc,
                           msNow() - t0);
    return rc;
}
