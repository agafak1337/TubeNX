// dpi/dpi.cpp — обход DPI в духе ByeByeDPI.
//
// Что здесь есть:
//   1) Локальный SOCKS5-прокси (RFC 1928, только CONNECT). curl ходит через него
//      как socks5h://127.0.0.1:1080. Прокси сам открывает TCP к цели и
//      ретранслирует байты, но самый первый пакет клиента (TLS ClientHello)
//      отправляет upstream-серверу ФРАГМЕНТАМИ. DPI, который ищет сигнатуру SNI
//      в одном TCP-сегменте (или в первых байтах потока), ломается, а сервер
//      спокойно собирает ClientHello из нескольких сегментов.
//   2) Локальный HTTP-ретранслятор для mpv: http://127.0.0.1:8899/u?u=<url>.
//      ffmpeg в этом порте собран без TLS, поэтому HTTPS-видео качаем сами
//      через libcurl (с нашим же SOCKS5), а плееру отдаём обычный HTTP.
//
// Честное ограничение: raw-сокеты на Horizon OS (Switch) обычному
// homebrew-процессу недоступны, поэтому режимы Fake / OOB / desync из
// ByeByeDPI и zapret здесь НЕ реализованы. Есть только фрагментация
// (split / multi-split / split-disorder / tls-record-split) — она не требует
// raw-сокетов и работает через обычный TCP-соединение. Настоящий SplitDisorder
// (перестановка сегментов) возможен только на уровне IP, поэтому здесь он
// эмулируется безопасным разбиением ClientHello пополам (см. sendFirstPacket).
//
// Код намеренно написан на чистом POSIX (socket/poll/pthread), без libnx,
// чтобы его можно было собрать и прогнать на хосте с -DTNX_HOST.
// В host-сборке отключается только часть, которой нужны заголовки libcurl
// (HTTP-ретранслятор и probe через net::get): на обычном Linux-хосте
// curl-заголовков может не быть, а SOCKS5-ядро от них не зависит.

#include "dpi/dpi.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h> // fd_set: требуется заголовками curl/multi.h
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef TNX_HOST
#include "net/http.hpp"
#include "platform.hpp"

#include <curl/curl.h>
#endif

namespace tnx::dpi {
namespace {

// net/sni.cpp: выбор имени для подмены SNI в текущем потоке.
extern "C" void tubeSpoofSetName(const char* name);

// ---------------------------------------------------------------------------
// Общие настройки и статистика
// ---------------------------------------------------------------------------

std::mutex g_dataMu;          // защищает config() и stats()
Config g_cfg;
Stats g_stats;

// Отдельный мьютекс жизненного цикла: start/stop не должны держать g_dataMu,
// пока join-ят поток (иначе поток-акцептор может ждать g_dataMu и получится
// взаимная блокировка).
std::mutex g_lifeMu;

// SIGPIPE: send() в закрытый сокет не должен убивать процесс.
void ignoreSigpipe() {
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
}

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// usleep есть и в libnx, и на хосте; отдельная обёртка для читаемости.
void sleepMs(int ms) {
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ---------------------------------------------------------------------------
// Процентное кодирование (RFC 3986): пробел — %20, а не '+'.
// ---------------------------------------------------------------------------

std::string pctEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                          c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// send() может записать меньше, чем просили (особенно на Switch с маленьким
// SO_SNDBUF), поэтому всегда отправляем циклом. MSG_NOSIGNAL есть не везде.
#ifdef MSG_NOSIGNAL
const int kSendFlags = MSG_NOSIGNAL;
#else
const int kSendFlags = 0;
#endif

bool writeAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    // Общий предел, а не 5 с на каждую итерацию: иначе медленный получатель
    // может держать поток в writeAll практически бесконечно.
    const long long deadline = nowMs() + 30000;
    while (len > 0) {
        ssize_t n = send(fd, p, len, kSendFlags);
        if (n > 0) {
            p += n;
            len -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (nowMs() > deadline) return false;
            pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, 5000) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

// Прочитать ровно n байт с таймаутом.
bool readExact(int fd, void* buf, size_t n, int timeoutMs) {
    char* p = static_cast<char*>(buf);
    long long deadline = nowMs() + timeoutMs;
    while (n > 0) {
        long long left = deadline - nowMs();
        if (left <= 0) return false;
        pollfd pfd{fd, POLLIN, 0};
        int pr = poll(&pfd, 1, static_cast<int>(left));
        if (pr == 0) return false;
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// Лимит одновременных соединений для каждого сервера (8 штук).
// Реализован на мьютексе + condition_variable, чтобы не тащить sem_t.
class SlotLimiter {
public:
    explicit SlotLimiter(int maxSlots) : max_(maxSlots) {}

    // Ждём свободный слот, но регулярно проверяем флаг run: остановка сервера
    // не должна зависать навсегда в ожидании слота.
    bool acquire(const std::atomic<bool>& run) {
        std::unique_lock<std::mutex> lk(mu_);
        while (used_ >= max_) {
            if (!run.load()) return false;
            cv_.wait_for(lk, std::chrono::milliseconds(100));
        }
        ++used_;
        return true;
    }

    void release() {
        std::lock_guard<std::mutex> lk(mu_);
        if (used_ <= 0) return;  // двойной release не должен расширять лимит
        --used_;
        cv_.notify_one();
    }

    int used() {
        std::lock_guard<std::mutex> lk(mu_);
        return used_;
    }

    // Ждём слот не дольше ms: если очередь не рассосалась, лучше отказать
    // клиенту сразу (он повторит запрос), чем навсегда остановить accept-loop
    // и получить у всех новых соединений таймаут вместо быстрой ошибки.
    bool acquireFor(const std::atomic<bool>& run, int ms) {
        std::unique_lock<std::mutex> lk(mu_);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (used_ >= max_) {
            if (!run.load()) return false;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            cv_.wait_for(lk, std::chrono::milliseconds(50));
        }
        ++used_;
        return true;
    }

private:
    int max_;
    int used_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
};

// ---------------------------------------------------------------------------
// TCP-подключение к цели (IPv4/IPv6, с таймаутом)
// ---------------------------------------------------------------------------

void socksLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Кэш DNS. На Switch libnx-getaddrinfo на этой сети иногда думает 3-9 секунд,
// а новое TCP-соединение открывается на каждый запрос (curl, обложки, API).
// Без кэша каждый запрос начинается с многогоризонтального ожидания.
struct DnsEntry {
    long long ts = 0;
    std::vector<sockaddr_in> addrs;  // все A-записи (порт не заполнен)
    int good = -1;                   // индекс последнего удачного адреса
};

std::mutex g_dnsMu;
std::map<std::string, DnsEntry> g_dns;
constexpr long long kDnsTtlMs = 10 * 60 * 1000;

// Резолвит host в СПИСОК адресов (с кэшем). Раньше брали только первый —
// и если провайдер режет именно этот IP (RST за 200 мс), все запросы к хосту
// падали, пока не истечёт TTL кэша: «нихуя не грузится».
bool resolveAll(const std::string& host, uint16_t port, std::vector<sockaddr_in>& out,
                bool forceFresh) {
    {
        std::lock_guard<std::mutex> lk(g_dnsMu);
        auto it = g_dns.find(host);
        if (!forceFresh && it != g_dns.end() && nowMs() - it->second.ts < kDnsTtlMs &&
            !it->second.addrs.empty()) {
            out = it->second.addrs;
            for (auto& a : out) a.sin_port = htons(port);
            return true;
        }
    }

    addrinfo hints{};
    // Только IPv4: на этой сети IPv6 нет, а запрос AAAA на консоли может
    // висеть до таймаута — именно из-за него getaddrinfo занимал секунды.
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

    // Тайминг: если резолв снова займёт секунды, в логе будет видно, что дело
    // именно в DNS, а не в TCP/TLS.
    const long long td0 = nowMs();
    addrinfo* res = nullptr;
    const int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
    const long long dnsMs = nowMs() - td0;

    std::vector<sockaddr_in> list;
    if (gai == 0 && res) {
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            if (ai->ai_family != AF_INET || ai->ai_addrlen != sizeof(sockaddr_in)) continue;
            sockaddr_in a{};
            std::memcpy(&a, ai->ai_addr, sizeof(a));
            a.sin_port = 0;
            bool dup = false;
            for (const auto& e : list)
                if (e.sin_addr.s_addr == a.sin_addr.s_addr) dup = true;
            if (!dup) list.push_back(a);
        }
        freeaddrinfo(res);
    }
    if (list.empty()) {
        socksLog("dns: %s -> ОШИБКА за %lld мс", host.c_str(), dnsMs);
        return false;
    }
    if (dnsMs >= 400 || list.size() > 1) {
        socksLog("dns: %s -> %zu адрес(ов) за %lld мс", host.c_str(), list.size(), dnsMs);
    }

    {
        std::lock_guard<std::mutex> lk(g_dnsMu);
        DnsEntry& e = g_dns[host];
        e.ts = nowMs();
        e.addrs = list;
        if (e.addrs.empty()) e.good = -1;
    }
    out = list;
    for (auto& a : out) a.sin_port = htons(port);
    return true;
}

void markGoodAddress(const std::string& host, const sockaddr_in& addr) {
    std::lock_guard<std::mutex> lk(g_dnsMu);
    auto it = g_dns.find(host);
    if (it == g_dns.end()) return;
    for (size_t i = 0; i < it->second.addrs.size(); ++i) {
        if (it->second.addrs[i].sin_addr.s_addr == addr.sin_addr.s_addr) {
            it->second.good = (int)i;
            return;
        }
    }
}

int goodAddressIndex(const std::string& host) {
    std::lock_guard<std::mutex> lk(g_dnsMu);
    auto it = g_dns.find(host);
    return it == g_dns.end() ? -1 : it->second.good;
}

// Одно неблокирующее подключение с таймаутом.
int connectOne(const sockaddr_in& addr, int timeoutMs) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    int rc = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno == EINPROGRESS) {
        pollfd pfd{fd, POLLOUT, 0};
        int pr = poll(&pfd, 1, timeoutMs);
        int err = 0;
        socklen_t el = sizeof(err);
        if (pr > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
            rc = 0;
        } else {
            rc = -1;
        }
    } else if (rc == 0) {
        rc = 0;
    }

    if (rc != 0) {
        close(fd);
        return -1;
    }

    if (fl >= 0) fcntl(fd, F_SETFL, fl);  // обратно в блокирующий режим
    // Держим NAT-таблицу провайдера живой на долгих TLS-паузах консоли.
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    return fd;
}

int connectTcp(const std::string& host, uint16_t port, int timeoutMs) {
    // Перебираем ВСЕ адреса хоста: часть IP у провайдера заблокирована и
    // отвечает RST, но соседние работают. Последний удачный адрес запоминаем
    // и в следующий раз пробуем первым — тогда «холодный» перебор не нужен.
    const long long deadline = nowMs() + (timeoutMs > 0 ? timeoutMs : 15000);
    const int perAddr = timeoutMs > 0 && timeoutMs < 2500 ? timeoutMs : 2500;

    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<sockaddr_in> addrs;
        if (!resolveAll(host, port, addrs, attempt > 0)) return -1;

        const int good = goodAddressIndex(host);
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < addrs.size(); ++i) {
                const bool isGood = (static_cast<int>(i) == good);
                if ((pass == 0) != isGood) continue;  // pass 0 — сначала «хороший»
                int fd = connectOne(addrs[i], perAddr);
                if (fd >= 0) {
                    markGoodAddress(host, addrs[i]);
                    if (!isGood && addrs.size() > 1) {
                        char ip[20] = {0};
                        inet_ntop(AF_INET, &addrs[i].sin_addr, ip, sizeof(ip));
                        socksLog("socks: %s:%d -> адрес %zu/%zu (%s)", host.c_str(), port, i + 1,
                                 addrs.size(), ip);
                    }
                    return fd;
                }
                if (nowMs() > deadline) return -1;
            }
        }
        if (nowMs() > deadline) return -1;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Фрагментация первого пакета (TLS ClientHello)
// ---------------------------------------------------------------------------

// Похоже ли начало буфера на TLS-запись типа Handshake (0x16) с версией 3.x?
bool looksLikeTls(const char* data, size_t len) {
    if (len < 3) return false;
    unsigned char b0 = static_cast<unsigned char>(data[0]);
    unsigned char b1 = static_cast<unsigned char>(data[1]);
    return b0 == 0x16 && b1 == 0x03;
}

// Похоже ли на plain-HTTP запрос (для splitHttpHost).
bool looksLikeHttp(const char* data, size_t len) {
    static const char* kMethods[] = {"GET ", "POST ", "HEAD ", "PUT ", "OPTIONS "};
    for (const char* m : kMethods) {
        size_t ml = std::strlen(m);
        if (len >= ml && std::memcmp(data, m, ml) == 0) return true;
    }
    return false;
}

// Отправляет первый пакет согласно режиму. Возвращает true, если всё ушло.
// fragmentationApplied выставляется в true только если реально было >1 send.
bool sendFirstPacket(int fd, const char* data, size_t len, const Config& cfg,
                     bool& fragmentationApplied) {
    fragmentationApplied = false;
    if (len == 0) return true;

    const Mode mode = cfg.mode;
    const int delay = clampInt(cfg.delayMs, 0, 1000);
    const int fragSize = clampInt(cfg.fragmentSize, 1, 16);

    if (mode == Mode::Off) {
        return writeAll(fd, data, len);
    }

    if (mode == Mode::Split) {
        size_t first = static_cast<size_t>(fragSize);
        if (len <= first) return writeAll(fd, data, len);
        fragmentationApplied = true;
        if (!writeAll(fd, data, first)) return false;
        sleepMs(delay);
        return writeAll(fd, data + first, len - first);
    }

    if (mode == Mode::MultiSplit) {
        int nf = clampInt(cfg.fragments, 2, 32);
        size_t first = static_cast<size_t>(fragSize);
        if (len <= first + 1 || nf < 2) return writeAll(fd, data, len);

        fragmentationApplied = true;
        if (!writeAll(fd, data, first)) return false;

        size_t rest = len - first;
        size_t parts = static_cast<size_t>(nf - 1);
        size_t base = rest / parts;
        size_t extra = rest % parts; // лишние байты достаются первым кускам
        size_t off = first;
        for (size_t i = 0; i < parts && off < len; ++i) {
            size_t chunk = base + (i < extra ? 1 : 0);
            if (chunk == 0) continue;
            if (chunk > len - off) chunk = len - off;
            sleepMs(delay);
            if (!writeAll(fd, data + off, chunk)) return false;
            off += chunk;
        }
        if (off < len) {
            sleepMs(delay);
            return writeAll(fd, data + off, len - off);
        }
        return true;
    }

    if (mode == Mode::SplitDisorder) {
        // ВАЖНО. Настоящий disorder (отправить хвост раньше головы, чтобы DPI
        // склеил поток не в том порядке) работает только через raw-сокеты:
        // там можно самому назначить sequence number. В обычном TCP номер
        // последовательности выдаёт ядро в порядке вызовов send(), поэтому
        // буквальная перестановка просто испортила бы поток (TLS-сервер
        // ответил бы «wrong version number»), а не обманула бы DPI.
        //
        // Поэтому здесь честная эмуляция: режем ClientHello примерно пополам
        // и отправляем первую половину отдельным сегментом с увеличенной
        // паузой. Для DPI это такой же разрыв сигнатуры, но байтовый поток
        // остаётся корректным.
        size_t k = len / 2;
        if (len < 4 || k == 0) return writeAll(fd, data, len);
        fragmentationApplied = true;
        if (!writeAll(fd, data, k)) return false;
        sleepMs(delay * 3);
        return writeAll(fd, data + k, len - k);
    }

    if (mode == Mode::TlsRecordSplit) {
        // Режем строго по границам TLS-записей: 5-байтовый заголовок
        // (type, version[2], length[2]), затем payload. Каждая запись —
        // отдельный send с паузой.
        size_t off = 0;
        int records = 0;
        while (off + 5 <= len) {
            unsigned char type = static_cast<unsigned char>(data[off]);
            size_t recLen = (static_cast<unsigned char>(data[off + 3]) << 8) |
                            static_cast<unsigned char>(data[off + 4]);
            // Защита от мусора: тип записи TLS — 20..24.
            if (type < 20 || type > 24 || recLen > 16384 + 2048) break;
            size_t total = 5 + recLen;
            if (total > len - off) total = len - off; // неполная запись
            if (records > 0) sleepMs(delay);
            if (!writeAll(fd, data + off, total)) return false;
            off += total;
            ++records;
            fragmentationApplied = true;
        }
        if (off < len) {
            if (records > 0) sleepMs(delay);
            if (!writeAll(fd, data + off, len - off)) return false;
        }
        return true;
    }

    return writeAll(fd, data, len);
}

// ---------------------------------------------------------------------------
// SOCKS5-сервер
// ---------------------------------------------------------------------------

// По умолчанию стек pthread в newlib невелик; для сетевых потоков (TLS,
// DNS, разбор ответов) берём 512 КиБ, иначе возможен тихий stack overflow.
static int createThreadWithStack(pthread_t* th, void* (*fn)(void*), void* arg, size_t stack) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return pthread_create(th, nullptr, fn, arg);
    pthread_attr_setstacksize(&attr, (int)stack);
    int rc = pthread_create(th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

constexpr size_t kNetStack = 512u * 1024u;

struct SocksServer {
    int listenFd = -1;
    int wakeFd = -1;
    std::atomic<int> port{0};
    std::atomic<bool> run{false};
    // 8 было мало: при пакетной загрузке обложек соединения занимают слоты на
    // всё время рукопожатия (до 10 с каждое), accept-loop вставал в очередь,
    // и новые подключения от curl падали с «Couldn't connect to server».
    SlotLimiter slots{16};
    pthread_t thread{};
    bool threadStarted = false;
    // Живые клиентские fd: stopSocks() должен разбудить их, иначе туннели
    // продолжат работать после выключения обхода и держать слоты.
    std::mutex liveMu;
    std::vector<int> live;
    std::atomic<bool> loopExited{false};
};

SocksServer g_socks;
int g_socksWakeWrite = -1;

// Состояние HTTP-ретранслятора. Определено всегда, чтобы host-сборка могла
// собрать relayPort()/stopRelay() без libcurl-заголовков.
struct RelayServer {
    int listenFd = -1;
    int wakeFd = -1;
    std::atomic<int> port{0};
    std::atomic<bool> run{false};
    // 4 = максимум SSL-сессий консоли. Больше одновременных curl-потоков
    // всё равно не смогут рукопожаться и будут ждать, занимая слоты.
    SlotLimiter slots{4};
    pthread_t thread{};
};

RelayServer g_relay;
int g_relayWakeWrite = -1;

#ifndef TNX_HOST
// Диагностика ретранслятора: в лог идут только первые запросы плеера, чтобы
// было видно, доходит ли mpv до ретранслятора и что отвечает googlevideo.
std::atomic<int> g_relayLogLines{0};

// Общий кэш DNS для relay-потоков: иначе каждый Range-запрос резолвит
// googlevideo заново (на Switch это 2–9 с, см. net/http.cpp).
CURLSH* g_relayShare = nullptr;
std::mutex g_relayShareMu;
void relayShareLock(CURL*, curl_lock_data, curl_lock_access, void*) { g_relayShareMu.lock(); }
void relayShareUnlock(CURL*, curl_lock_data, void*) { g_relayShareMu.unlock(); }
CURLSH* relayShare() {
    std::lock_guard<std::mutex> lk(g_relayShareMu);
    if (!g_relayShare) {
        g_relayShare = curl_share_init();
        if (g_relayShare) {
            curl_share_setopt(g_relayShare, CURLSHOPT_LOCKFUNC, relayShareLock);
            curl_share_setopt(g_relayShare, CURLSHOPT_UNLOCKFUNC, relayShareUnlock);
            curl_share_setopt(g_relayShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        }
    }
    return g_relayShare;
}

// Хост из URL — без query: в нём подписанные токены.
std::string urlHostOnly(const std::string& u) {
    size_t p = u.find("://");
    size_t s = (p == std::string::npos) ? 0 : p + 3;
    size_t e = u.find_first_of("/?#", s);
    return u.substr(s, (e == std::string::npos ? u.size() : e) - s);
}
#endif

// Логи обхода: пишем только первые события, чтобы не забивать SD-карту
// и не тормозить сеть на каждом соединении.
std::atomic<int> g_socksLog{0};
std::atomic<int> g_socksErrLog{0};
bool socksLogOn() {
    return g_socksLog.fetch_add(1) < 80;
}
// Ошибки не должны конкурировать с «шумными» строками об успехе: раньше
// общий счётчик на 80 съедался строками «готово за…», и «нет соединения»
// переставало писаться уже через ~40 успешных туннелей.
bool socksLogErrOn() {
    return g_socksErrLog.fetch_add(1) < 300;
}

void socksLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void socksLog(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifndef TNX_HOST
    platform::bootLog("%s", buf);
#else
    std::fprintf(stderr, "%s\n", buf);
#endif
}

// Собирает стандартный ответ SOCKS5 (REP, ATYP=1) с реальным bound-адресом.
bool sendSocksReply(int fd, unsigned char rep) {
    unsigned char out[10] = {0x05, rep, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    sockaddr_storage ss{};
    socklen_t sl = sizeof(ss);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &sl) == 0 &&
        ss.ss_family == AF_INET) {
        auto* in4 = reinterpret_cast<sockaddr_in*>(&ss);
        std::memcpy(out + 4, &in4->sin_addr, 4);
        std::memcpy(out + 8, &in4->sin_port, 2);
    }
    return writeAll(fd, out, sizeof(out));
}

// Закрыть без RST: RST выбрасывает уже отправленный SOCKS-ответ, и клиент
// видит «Connection reset by peer» вместо честного кода ошибки.
void drainAndClose(int fd) {
    shutdown(fd, SHUT_WR);
    char tmp[512];
    for (int i = 0; i < 20; ++i) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) break;
        if (recv(fd, tmp, sizeof(tmp), 0) <= 0) break;
    }
    close(fd);
}

// Полное SOCKS5-рукопожатие. При успехе возвращает fd соединения с целью,
// иначе -1 (клиенту уже отправлен код ошибки, если это уместно).
int socksHandshake(int client) {
    // Один дедлайн на весь handshake, а не 10 с на каждую из шести read:
    // иначе медленный клиент держит слот до минуты, ничего не отправив.
    // 20 с = худший перебор адресов (15 с) + ClientHello после CONNECT.
    const long long hsDeadline = nowMs() + 20000;
    auto left = [&]() {
        const long long l = hsDeadline - nowMs();
        return l > 0 ? static_cast<int>(l) : 1;
    };

    unsigned char head[2];
    if (!readExact(client, head, 2, left()) || head[0] != 0x05) {
        return -1;
    }
    int nmethods = head[1];
    if (nmethods <= 0 || nmethods > 255) return -1;

    std::vector<unsigned char> methods(static_cast<size_t>(nmethods));
    if (!readExact(client, methods.data(), methods.size(), left())) return -1;

    bool hasNoAuth = false;
    for (unsigned char m : methods) {
        if (m == 0x00) hasNoAuth = true;
    }
    if (!hasNoAuth) {
        // Авторизацию не поддерживаем — честно отказываем.
        unsigned char no[2] = {0x05, 0xFF};
        writeAll(client, no, sizeof(no));
        return -1;
    }
    unsigned char ok[2] = {0x05, 0x00};
    if (!writeAll(client, ok, sizeof(ok))) return -1;

    unsigned char req[4];
    if (!readExact(client, req, 4, left())) return -1;
    if (req[0] != 0x05) return -1;
    if (req[1] != 0x01) { // только CONNECT
        sendSocksReply(client, 0x07); // command not supported
        return -1;
    }

    unsigned char atyp = req[3];
    std::string host;
    unsigned char raw4[4] = {0, 0, 0, 0};
    sockaddr_storage target{};
    socklen_t targetLen = 0;
    bool useSockaddr = false;

    if (atyp == 0x01) { // IPv4
        if (!readExact(client, raw4, 4, left())) return -1;
        char txt[16] = {0};
        inet_ntop(AF_INET, raw4, txt, sizeof(txt));
        host = txt;

        auto* in4 = reinterpret_cast<sockaddr_in*>(&target);
        in4->sin_family = AF_INET;
        std::memcpy(&in4->sin_addr, raw4, 4);
        targetLen = sizeof(sockaddr_in);
        useSockaddr = true;
    } else if (atyp == 0x03) { // доменное имя
        unsigned char hl = 0;
        if (!readExact(client, &hl, 1, left()) || hl == 0) return -1;
        std::vector<char> name(static_cast<size_t>(hl) + 1, 0);
        if (!readExact(client, name.data(), hl, left())) return -1;
        host.assign(name.data(), hl);
    } else if (atyp == 0x04) { // IPv6 (бонусом, стоит почти ничего)
        unsigned char raw16[16];
        if (!readExact(client, raw16, 16, left())) return -1;
        char txt[46] = {0};
        inet_ntop(AF_INET6, raw16, txt, sizeof(txt));
        host = txt;
    } else {
        sendSocksReply(client, 0x08); // address type not supported
        return -1;
    }

    unsigned char pb[2];
    if (!readExact(client, pb, 2, left())) return -1;
    uint16_t port = static_cast<uint16_t>((pb[0] << 8) | pb[1]);
    if (port == 0) {
        sendSocksReply(client, 0x01);
        return -1;
    }

    long long t0 = nowMs();
    int upstream = -1;
    if (useSockaddr) {
        auto* in4 = reinterpret_cast<sockaddr_in*>(&target);
        in4->sin_port = htons(port);
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd >= 0) {
            int fl = fcntl(fd, F_GETFL, 0);
            if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int rc = connect(fd, reinterpret_cast<sockaddr*>(in4), targetLen);
            if (rc < 0 && errno == EINPROGRESS) {
                pollfd pfd{fd, POLLOUT, 0};
                int pr = poll(&pfd, 1, left());
                int err = 0;
                socklen_t el = sizeof(err);
                if (pr > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 &&
                    err == 0) {
                    rc = 0;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0) {
                if (fl >= 0) fcntl(fd, F_SETFL, fl);
                upstream = fd;
            } else {
                close(fd);
            }
        }
    } else {
        upstream = connectTcp(host, port, left());
    }

    if (upstream < 0) {
        if (socksLogErrOn()) socksLog("socks: %s:%d -> нет соединения (%lld мс)", host.c_str(),
                                      port, nowMs() - t0);
        sendSocksReply(client, 0x05); // connection refused / general failure
        return -1;
    }

    // Клиент мог закрыть соединение, пока мы устанавливали upstream: не
    // держим слот ещё 10 с на мёртвом fd.
    if (!sendSocksReply(client, 0x00)) {
        close(upstream);
        return -1;
    }
    const long long tReply = nowMs();

    // ---- Первый пакет клиента: именно его и фрагментируем ----
    pollfd pfd{client, POLLIN, 0};
    // Слот занят, пока ждём первый пакет: 10 с вместо 15, чтобы очередь
    // из обложек/API не выедала все слоты на «мёртвых» соединениях.
    int pr = poll(&pfd, 1, left());
    if (pr <= 0) {
        if (socksLogErrOn()) socksLog("socks: %s:%d -> клиент молчит (%lld мс)", host.c_str(),
                                      port, nowMs() - t0);
        close(upstream);
        return -1;
    }

    std::vector<char> first(16384); // ClientHello обычно < 1.5 КБ, 16 КБ — с запасом
    ssize_t got = recv(client, first.data(), first.size(), 0);
    if (got <= 0) {
        close(upstream);
        return -1;
    }
    first.resize(static_cast<size_t>(got));

    // Если клиент молчал после CONNECT — это задержка TLS-стека консоли, а не
    // сети. Пишем только заметные паузы, чтобы не забивать лог.
    if (nowMs() - tReply >= 700 && socksLogOn()) {
        socksLog("socks: %s:%d клиент начал TLS через %lld мс после CONNECT", host.c_str(), port,
                 nowMs() - tReply);
    }

    Config cfg;
    {
        std::lock_guard<std::mutex> lk(g_dataMu);
        cfg = g_cfg;
    }

    bool isTls = looksLikeTls(first.data(), first.size());
    bool isHttp = looksLikeHttp(first.data(), first.size());
    bool wantFragment = cfg.enabled && (cfg.mode != Mode::Off) &&
                        ((isTls) || (isHttp && cfg.splitHttpHost));

    bool applied = false;
    bool okSend = true;
    if (wantFragment) {
        okSend = sendFirstPacket(upstream, first.data(), first.size(), cfg, applied);
    } else {
        okSend = writeAll(upstream, first.data(), first.size());
    }

    if (!okSend) {
        close(upstream);
        return -1;
    }

    {
        std::lock_guard<std::mutex> lk(g_dataMu);
        g_stats.relayedBytes += first.size();
        if (applied) ++g_stats.fragmented;
        g_stats.lastHandshakeMs = static_cast<float>(nowMs() - t0);
    }

    if (socksLogOn()) {
        socksLog("socks: %s:%d готово за %lld мс%s", host.c_str(), port, nowMs() - t0,
                 applied ? ", ClientHello раздроблен" : "");
    }

    return upstream;
}

// Ретрансляция клиент <-> цель до закрытия любой из сторон.
void socksRelay(int client, int upstream, uint64_t* bytesUp, uint64_t* bytesDown) {
    char buf[8192];
    bool clientOpen = true;
    bool serverOpen = true;
    // «Тихий» keep-alive туннель иначе живёт вечно и держит слот: 16 таких —
    // и все новые подключения получают отказ.
    const long long t0 = nowMs();
    long long lastData = t0;
    // 10 минут тишины: короче — рвём keep-alive curl и платим за TLS заново;
    // длиннее — «мёртвые» туннели держат слоты. Поток видео всегда с данными,
    // поэтому большой лимит времени не мешает многочасовому фильму.
    constexpr long long kIdleMs = 10LL * 60000;
    constexpr long long kMaxMs = 6LL * 3600 * 1000;

    while (clientOpen || serverOpen) {
        pollfd pfd[2];
        pfd[0].fd = clientOpen ? client : -1;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = serverOpen ? upstream : -1;
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;

        int pr = poll(pfd, 2, 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            const long long now = nowMs();
            if (now - lastData > kIdleMs || now - t0 > kMaxMs) break;
            continue;
        }

        // Клиент -> upstream. Важно проверять HUP/ERR: на Switch poll может
        // не выставить POLLIN при разрыве.
        if (clientOpen && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = recv(client, buf, sizeof(buf), 0);
            if (n > 0) {
                lastData = nowMs();
                if (!writeAll(upstream, buf, static_cast<size_t>(n))) {
                    std::lock_guard<std::mutex> lk(g_dataMu);
                    ++g_stats.errors;
                    break;
                }
                *bytesUp += static_cast<uint64_t>(n);
                std::lock_guard<std::mutex> lk(g_dataMu);
                g_stats.relayedBytes += static_cast<uint64_t>(n);
            } else if (n == 0) {
                clientOpen = false;
                shutdown(upstream, SHUT_WR); // полузакрытие: ответ ещё придёт
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                clientOpen = false;
                break;
            }
        }

        if (serverOpen && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = recv(upstream, buf, sizeof(buf), 0);
            if (n > 0) {
                lastData = nowMs();
                if (!writeAll(client, buf, static_cast<size_t>(n))) {
                    std::lock_guard<std::mutex> lk(g_dataMu);
                    ++g_stats.errors;
                    break;
                }
                *bytesDown += static_cast<uint64_t>(n);
                std::lock_guard<std::mutex> lk(g_dataMu);
                g_stats.relayedBytes += static_cast<uint64_t>(n);
            } else if (n == 0) {
                serverOpen = false;
                shutdown(client, SHUT_WR);
                break; // сервер закрыл поток — дальше данных не будет
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                serverOpen = false;
                break;
            }
        }

        if (!serverOpen) break;
    }
}

struct SocksJob {
    int client;
};

void* socksClientThread(void* arg) {
    SocksJob* job = static_cast<SocksJob*>(arg);
    int client = job->client;
    delete job;

    {
        std::lock_guard<std::mutex> lk(g_socks.liveMu);
        g_socks.live.push_back(client);
    }

    int upstream = socksHandshake(client);
    if (upstream >= 0) {
        uint64_t up = 0, down = 0;
        const long long t0 = nowMs();
        socksRelay(client, upstream, &up, &down);
        if (socksLogOn()) {
            socksLog("socks: соединение закрыто, up=%llu down=%llu, %lld мс",
                     (unsigned long long)up, (unsigned long long)down, nowMs() - t0);
        }
        close(upstream);
    } else {
        std::lock_guard<std::mutex> lk(g_dataMu);
        ++g_stats.errors;
    }
    {
        std::lock_guard<std::mutex> lk(g_socks.liveMu);
        auto& v = g_socks.live;
        v.erase(std::remove(v.begin(), v.end(), client), v.end());
    }
    close(client);
    g_socks.slots.release();
    return nullptr;
}

void* socksAcceptLoop(void* arg) {
    (void)arg;
    while (g_socks.run.load()) {
        pollfd pfd[2];
        pfd[0].fd = g_socks.listenFd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = g_socks.wakeFd;
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;

        int pr = poll(pfd, 2, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            // Выход отсюда = прокси мёртв до перезапуска приложения, потому что
            // startSocks() считает запущенным любой живой поток. Продолжаем.
            if (socksLogErrOn()) {
                socksLog("socks: poll: %s — продолжаем", std::strerror(errno));
            }
            sleepMs(100);
            continue;
        }
        if (pr == 0) continue;
        if (pfd[1].revents & POLLIN) break; // попросили остановиться
        if (pfd[0].revents & POLLNVAL) {
            sleepMs(100);
            continue;
        }
        if (!(pfd[0].revents & POLLIN)) continue;

        int c = accept(g_socks.listenFd, nullptr, nullptr);
        if (c < 0) {
            const int e = errno;
            if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK ||
                e == ECONNABORTED || e == EPROTO) {
                continue;
            }
            // EMFILE/ENFILE/ENOBUFS: не выходим, иначе прокси умирает молча.
            static std::atomic<int> acceptErrs{0};
            if (acceptErrs.fetch_add(1) < 40) {
                socksLog("socks: accept: %s — пауза", std::strerror(e));
            }
            sleepMs(200);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(g_dataMu);
            ++g_stats.connections;
        }

        if (!g_socks.slots.acquireFor(g_socks.run, 3000)) {
            // Все слоты заняты (обычно «мёртвые» соединения). Раньше здесь
            // был просто close() с непрочитанным greeting -> RST -> curl 7
            // без единой строки в логе. Теперь честный REP=0x01 и вычитка.
            if (socksLogErrOn()) {
                socksLog("socks: все слоты заняты %d/16 дольше 3 с, отказ",
                         g_socks.slots.used());
            }
            sendSocksReply(c, 0x01);
            drainAndClose(c);
            continue;
        }

        SocksJob* job = new SocksJob{c};
        pthread_t th;
        if (createThreadWithStack(&th, socksClientThread, job, kNetStack) != 0) {
            delete job;
            g_socks.slots.release();
            close(c);
            std::lock_guard<std::mutex> lk(g_dataMu);
            ++g_stats.errors;
            continue;
        }
        pthread_detach(th);
    }
    g_socks.loopExited.store(true);
    return nullptr;
}

// ---------------------------------------------------------------------------
// HTTP-ретранслятор для плеера (нужен libcurl, поэтому в host-сборке вырезан)
// ---------------------------------------------------------------------------

#ifndef TNX_HOST

// Декодирование нужно только ретранслятору (разбор /u?u=...).
int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string pctDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hexVal(s[i + 1]);
            int l = hexVal(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>((h << 4) | l));
                i += 2;
                continue;
            }
        }
        // '+' по RFC 3986 не пробел: оставляем как есть.
        out.push_back(s[i]);
    }
    return out;
}

// Локальные вспомогательные функции для разбора HTTP-запроса клиента.
std::string lowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string trimCrlf(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == '\r' || s[e - 1] == '\n' || s[e - 1] == ' ' ||
                     s[e - 1] == '\t')) {
        --e;
    }
    return s.substr(b, e - b);
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;

    std::string header(const char* name) const {
        std::string want = lowerAscii(name);
        for (const auto& h : headers) {
            if (lowerAscii(h.first) == want) return h.second;
        }
        return std::string();
    }
};

bool readHttpRequest(int fd, HttpRequest& rq) {
    std::string buf;
    char tmp[2048];
    while (buf.size() < 16384) {
        pollfd pfd{fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 5000);
        if (pr <= 0) return false;
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.find("\r\n\r\n") != std::string::npos) break;
    }

    size_t lineEnd = buf.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    std::string firstLine = buf.substr(0, lineEnd);
    size_t s1 = firstLine.find(' ');
    if (s1 == std::string::npos) return false;
    size_t s2 = firstLine.find(' ', s1 + 1);
    if (s2 == std::string::npos) s2 = firstLine.size();
    rq.method = firstLine.substr(0, s1);
    rq.path = firstLine.substr(s1 + 1, s2 - s1 - 1);
    if (rq.method.empty() || rq.path.empty()) return false;

    size_t pos = lineEnd + 2;
    while (pos < buf.size()) {
        size_t eol = buf.find("\r\n", pos);
        if (eol == std::string::npos || eol == pos) break;
        std::string line = buf.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            rq.headers.emplace_back(line.substr(0, colon), trimCrlf(line.substr(colon + 1)));
        }
        pos = eol + 2;
    }
    return true;
}

const char* reasonPhrase(long code) {
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

bool sendSimple(int fd, int code, const char* contentType, const std::string& body) {
    std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reasonPhrase(code) + "\r\n";
    out += "Content-Type: ";
    out += contentType;
    out += "\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\nConnection: close\r\n\r\n";
    out += body;
    return writeAll(fd, out.data(), out.size());
}

// Контекст одного запроса к upstream: сюда curl кладёт заголовки, отсюда
// байты уходят прямо в сокет клиента (без буферизации всего видео в памяти).
struct RelayCtx {
    int fd = -1;
    bool isHead = false;
    bool headSent = false;
    bool failed = false;
    bool clientGone = false;
    long code = 0;
    std::string reason;
    std::string contentType;
    std::string contentLength;
    std::string acceptRanges;
    std::string contentRange;
    bool chunked = false; // upstream отдаёт chunked: длину клиенту не сообщаем
    uint64_t bodyBytes = 0;

    void resetHeaders() {
        code = 0;
        reason.clear();
        contentType.clear();
        contentLength.clear();
        acceptRanges.clear();
        contentRange.clear();
        chunked = false;
    }
};

bool sendRelayHead(RelayCtx* c) {
    if (c->headSent) return true;
    long code = c->code ? c->code : 200;
    std::string out = "HTTP/1.1 " + std::to_string(code) + " ";
    out += c->reason.empty() ? reasonPhrase(code) : c->reason;
    out += "\r\n";
    if (!c->contentType.empty()) out += "Content-Type: " + c->contentType + "\r\n";
    // При chunked у upstream длина после раскодирования неизвестна: отдаём
    // тело до закрытия соединения (Connection: close), иначе клиент зависнет.
    if (!c->contentLength.empty() && !c->chunked) {
        out += "Content-Length: " + c->contentLength + "\r\n";
    }
    // Не выдумываем Accept-Ranges: если upstream его не прислал и это не 206,
    // каждый Range вернётся полным 200, и seek будет перезапускать поток.
    if (!c->acceptRanges.empty()) {
        out += "Accept-Ranges: " + c->acceptRanges + "\r\n";
    } else if (code == 206) {
        out += "Accept-Ranges: bytes\r\n";
    }
    if (!c->contentRange.empty()) out += "Content-Range: " + c->contentRange + "\r\n";
    out += "Connection: close\r\n\r\n";
    // Даже частичная запись означает «заголовки начаты»: второй статус-строки
    // в потоке быть не должно.
    c->headSent = true;
    return writeAll(c->fd, out.data(), out.size());
}

size_t relayWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    RelayCtx* c = static_cast<RelayCtx*>(userdata);
    size_t len = size * nmemb;
    if (len == 0) return 0;

    if (!c->headSent && !sendRelayHead(c)) {
        c->failed = true;
        return 0; // обрываем загрузку: клиент ушёл
    }

    size_t off = 0;
    while (off < len) {
        ssize_t n = send(c->fd, ptr + off, len - off, kSendFlags);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{c->fd, POLLOUT, 0};
            if (poll(&pfd, 1, 5000) <= 0) break;
            continue;
        }
        break;
    }
    if (off < len) {
        c->clientGone = true;
        return 0;
    }

    c->bodyBytes += len;
    {
        std::lock_guard<std::mutex> lk(g_dataMu);
        // savedBytes — сколько байт плеер получил через обход (без ретранслятора
        // он бы их вообще не скачал, т.к. ffmpeg без TLS).
        g_stats.savedBytes += len;
    }
    return len;
}

size_t relayHeaderCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    RelayCtx* c = static_cast<RelayCtx*>(userdata);
    size_t len = size * nitems;
    std::string line(buffer, len);

    if (line.size() >= 5 && line.compare(0, 5, "HTTP/") == 0) {
        // Новый блок заголовков (в т.ч. после редиректа) — начинаем заново,
        // чтобы клиент получил данные финального ответа.
        c->resetHeaders();
        size_t sp = line.find(' ');
        if (sp != std::string::npos) {
            c->code = std::strtol(line.c_str() + sp + 1, nullptr, 10);
            size_t sp2 = line.find(' ', sp + 1);
            if (sp2 != std::string::npos) {
                c->reason = trimCrlf(line.substr(sp2 + 1));
            }
        }
        return len;
    }

    if (line == "\r\n" || line == "\n") {
        // Редирект: libcurl сам пойдёт по Location, а клиенту промежуточный
        // 302/307 отдавать нельзя — иначе заголовки не совпадут с телом.
        // 100 Continue тоже пропускаем. Всё это — только промежуточные блоки.
        if (c->code == 301 || c->code == 302 || c->code == 303 ||
            c->code == 307 || c->code == 308 || c->code == 100) {
            return len;
        }
        // Заголовки закончились: только теперь знаем Content-Length/Range.
        if (!c->headSent) {
            if (!sendRelayHead(c)) {
                c->failed = true;
                return 0;
            }
        }
        return len;
    }

    size_t colon = line.find(':');
    if (colon == std::string::npos) return len;
    std::string name = lowerAscii(trimCrlf(line.substr(0, colon)));
    std::string value = trimCrlf(line.substr(colon + 1));
    if (name == "content-type") {
        c->contentType = value;
    } else if (name == "content-length") {
        c->contentLength = value;
    } else if (name == "accept-ranges") {
        c->acceptRanges = value;
    } else if (name == "content-range") {
        c->contentRange = value;
    } else if (name == "transfer-encoding") {
        if (lowerAscii(value).find("chunked") != std::string::npos) c->chunked = true;
    }
    return len;
}

// Проверяем CA-бандл: на Switch он лежит в romfs. Если файла нет — не ставим
// CURLOPT_CAINFO, libcurl/mbedtls попробует свои пути по умолчанию.
const char* relayCaPath() {
    static const char* kCandidates[] = {"romfs:/cacert.pem",
                                        "sdmc:/switch/TubeNX/cacert.pem"};
    for (const char* p : kCandidates) {
        if (access(p, R_OK) == 0) return p;
    }
    return nullptr;
}

void* relayClientThread(void* arg);
void* relayAcceptLoop(void* arg);

struct RelayJob {
    int client;
};

void* relayAcceptLoop(void* arg) {
    (void)arg;
    while (g_relay.run.load()) {
        pollfd pfd[2];
        pfd[0].fd = g_relay.listenFd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = g_relay.wakeFd;
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;

        int pr = poll(pfd, 2, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            platform::bootLog("relay: poll: %s — продолжаем", std::strerror(errno));
            sleepMs(100);
            continue;
        }
        if (pr == 0) continue;
        if (pfd[1].revents & POLLIN) break;
        if (!(pfd[0].revents & POLLIN)) continue;

        int c = accept(g_relay.listenFd, nullptr, nullptr);
        if (c < 0) {
            const int e = errno;
            if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK ||
                e == ECONNABORTED || e == EPROTO) {
                continue;
            }
            platform::bootLog("relay: accept: %s — пауза", std::strerror(e));
            sleepMs(200);
            continue;
        }

        // Не блокируем accept-loop дольше 500 мс: mpv сам повторит Range.
        if (!g_relay.slots.acquireFor(g_relay.run, 500)) {
            // Дольше 10 с свободного слота нет (висят «мёртвые» потоки).
            // Рвём запрос: плеер повторит Range, а accept-loop не встанет.
            if (g_relayLogLines.fetch_add(1) < 400) {
                platform::bootLog("relay: нет свободного слота, запрос отклонён");
            }
            close(c);
            continue;
        }

        RelayJob* job = new RelayJob{c};
        pthread_t th;
        if (createThreadWithStack(&th, relayClientThread, job, kNetStack) != 0) {
            delete job;
            g_relay.slots.release();
            close(c);
            continue;
        }
        pthread_detach(th);
    }
    return nullptr;
}

void relayHandle(int fd) {
    HttpRequest rq;
    if (!readHttpRequest(fd, rq)) {
        platform::bootLog("relay: bad/too-long request, dropped");
        return;
    }
    if (rq.method != "GET" && rq.method != "HEAD") {
        sendSimple(fd, 405, "text/plain; charset=utf-8", "method not allowed\n");
        return;
    }

    // /healthz — простая проверка живости для UI/тестов.
    if (rq.path == "/healthz") {
        std::lock_guard<std::mutex> lk(g_dataMu);
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "ok connections=%llu fragmented=%llu relayed=%llu errors=%llu\n",
                      static_cast<unsigned long long>(g_stats.connections),
                      static_cast<unsigned long long>(g_stats.fragmented),
                      static_cast<unsigned long long>(g_stats.relayedBytes),
                      static_cast<unsigned long long>(g_stats.errors));
        sendSimple(fd, 200, "text/plain; charset=utf-8", buf);
        return;
    }

    // /proxy.pac — заглушка-подсказка: полноценным HTTP-прокси ретранслятор
    // не является, он умеет только /u?u=<url>.
    if (rq.path == "/proxy.pac") {
        std::string pac =
            "// TubeNX relay: не является полноценным HTTP-прокси.\n"
            "// Видео запрашивайте как http://127.0.0.1:8899/u?u=<urlencoded-url>\n"
            "function FindProxyForURL(url, host) { return \"DIRECT\"; }\n";
        sendSimple(fd, 200, "application/x-ns-proxy-autoconfig", pac);
        return;
    }

    // Разбор /u?u=<urlencoded-upstream-url>
    std::string upstream;
    size_t q = rq.path.find('?');
    if (q != std::string::npos) {
        std::string query = rq.path.substr(q + 1);
        size_t pos = 0;
        while (pos <= query.size()) {
            size_t amp = query.find('&', pos);
            std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos
                                                                        : amp - pos);
            size_t eq = pair.find('=');
            if (eq != std::string::npos && pair.substr(0, eq) == "u") {
                upstream = pctDecode(pair.substr(eq + 1));
                break;
            }
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
    }

    if (upstream.rfind("http://", 0) != 0 && upstream.rfind("https://", 0) != 0) {
        sendSimple(fd, 400, "text/plain; charset=utf-8",
                   "usage: /u?u=<urlencoded http(s) url>\n");
        return;
    }

    bool isHead = (rq.method == "HEAD");

    RelayCtx ctx;
    ctx.fd = fd;
    ctx.isHead = isHead;

    // Range пробрасываем как есть (без префикса "bytes="), чтобы mpv мог seek-ить.
    std::string range = rq.header("Range");
    if (range.rfind("bytes=", 0) == 0) {
        range = range.substr(6);
    } else {
        range.clear();
    }

    // Диагностика: видно, что mpv реально достучался до ретранслятора и
    // куда мы идём за потоком (без query-токенов).
    const bool logIt = g_relayLogLines.fetch_add(1) < 400;
    const std::string upHost = urlHostOnly(upstream);

    // Весь трафик к YouTube идёт через прокси: либо внешний (задан в
    // настройках), либо наш SOCKS5 (фрагментация + верхний регистр SNI).
    std::string proxy = net::proxy();
    if (proxy.empty() && g_cfg.enabled && g_cfg.relayViaLocalSocks) {
        // Если глобальный прокси почему-то сброшен, ретранслятор всё равно
        // обязан идти через локальный SOCKS — иначе остаётся без обхода.
        proxy = "socks5h://127.0.0.1:1080";
    }
    if (logIt) {
        platform::bootLog("relay: %s %s range='%s' proxy=%s", rq.method.c_str(), upHost.c_str(),
                          range.c_str(), proxy.empty() ? "нет" : proxy.c_str());
    }

    // Для `*.googlevideo.com` в net/sni.cpp SNI подменяется на разрешённое
    // имя (иначе DPI рвёт рукопожатие), Host остаётся настоящим. Сервер
    // отдаёт сертификат на чужое имя, поэтому проверку отключаем. Целостность
    // и раньше не гарантировалась: поток приходит по подписанной ссылке.
    bool spoofedSni = false;
    {
        const size_t n = upHost.size(), m = strlen("googlevideo.com");
        if (n >= m) {
            std::string tail = upHost.substr(n - m);
            for (char& ch : tail) ch = (char)tolower((unsigned char)ch);
            spoofedSni = (tail == "googlevideo.com");
        }
    }

    // Запасные конфиги: провайдер режет само имя в ClientHello, а серверы
    // Google обслуживают URL по Host. Все имена ниже проверены на живой сети
    // (206 video/mp4). Если DPI научится резать первое — берём следующее.
    static const char* const kSpoofNames[] = {
        "www.google.com", "www.gstatic.com", "accounts.google.com",
        "mail.google.com", "google.com",
    };
    constexpr int kSpoofCount = (int)(sizeof(kSpoofNames) / sizeof(kSpoofNames[0]));

    // Удачное имя запоминаем: следующие соединения начинают сразу с него,
    // а не перебирают цепочку заново.
    static std::atomic<int> s_lastGood{-1};

    int order[kSpoofCount];
    int orderLen = 0;
    if (spoofedSni) {
        const int lg = s_lastGood.load();
        if (lg >= 0 && lg < kSpoofCount) order[orderLen++] = lg;
        for (int i = 0; i < kSpoofCount; i++) {
            if (i != lg) order[orderLen++] = i;
        }
    } else {
        order[orderLen++] = -1; // без подмены
    }

    CURLcode rc = CURLE_OK;
    for (int attempt = 0; attempt < orderLen; attempt++) {
        const int cand = order[attempt];
        if (cand >= 0) tubeSpoofSetName(kSpoofNames[cand]);

        CURL* eh = curl_easy_init();
        if (!eh) {
            sendSimple(fd, 500, "text/plain; charset=utf-8", "curl init failed\n");
            break;
        }

        curl_easy_setopt(eh, CURLOPT_URL, upstream.c_str());
        curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(eh, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(eh, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(eh, CURLOPT_CONNECTTIMEOUT, 15L);
        // 60 с тишины — это не мёртвая сеть, а пауза плеера: mpv перестаёт
        // читать при полном кеше. Иначе пауза на минуту рвёт поток посередине
        // тела, хотя Content-Length уже обещан клиенту.
        curl_easy_setopt(eh, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(eh, CURLOPT_LOW_SPEED_TIME, 300L);
        if (CURLSH* sh = relayShare()) curl_easy_setopt(eh, CURLOPT_SHARE, sh);
        // UA должен совпадать с клиентом InnerTube, который выдал ссылку
        // (ANDROID 20.10.38): googlevideo в части случаев проверяет его и
        // отдаёт 403 при чужом.
        curl_easy_setopt(eh, CURLOPT_USERAGENT,
                         "com.google.android.youtube/20.10.38 (Linux; U; Android 12) gzip");
        // Сжатие НЕ просим: тело уходит клиенту как есть, без Content-Encoding.
        curl_easy_setopt(eh, CURLOPT_ACCEPT_ENCODING, nullptr);

        curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, relayWriteCb);
        curl_easy_setopt(eh, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(eh, CURLOPT_HEADERFUNCTION, relayHeaderCb);
        curl_easy_setopt(eh, CURLOPT_HEADERDATA, &ctx);

        if (isHead) curl_easy_setopt(eh, CURLOPT_NOBODY, 1L);
        if (!range.empty()) curl_easy_setopt(eh, CURLOPT_RANGE, range.c_str());

        if (!proxy.empty()) {
            curl_easy_setopt(eh, CURLOPT_PROXY, proxy.c_str());
            if (proxy.rfind("socks5h://", 0) == 0) {
                curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
            } else if (proxy.rfind("socks5://", 0) == 0) {
                curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
            } else if (proxy.rfind("socks4://", 0) == 0) {
                curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
            }
        }

        if (cand >= 0) {
            if (logIt) {
                platform::bootLog("relay: %s -> SNI подменён на %s", upHost.c_str(),
                                  kSpoofNames[cand]);
            }
            if (const char* ca = relayCaPath()) curl_easy_setopt(eh, CURLOPT_CAINFO, ca);
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, 0L);
            // Пока идёт перебор, не сидим по 15 с на каждом «мёртвом» имени.
            curl_easy_setopt(eh, CURLOPT_CONNECTTIMEOUT, 8L);
        } else if (const char* ca = relayCaPath()) {
            curl_easy_setopt(eh, CURLOPT_CAINFO, ca);
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, 2L);
        } else {
            // Бандла нет (например, romfs ещё не смонтирован) — оставляем
            // проверку по умолчанию, но не подсовываем несуществующий путь.
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, 2L);
        }

        rc = curl_easy_perform(eh);

        if (logIt) {
            const std::string err = rc == CURLE_OK
                                        ? std::string()
                                        : (" (" + std::string(curl_easy_strerror(rc)) + ")");
            platform::bootLog("relay: %s -> %ld%s, %llu байт", upHost.c_str(), ctx.code,
                              err.c_str(), (unsigned long long)ctx.bodyBytes);
        }

        curl_easy_cleanup(eh);

        // Соединение не состоялось — можно пробовать другое имя. Если клиенту
        // уже ушли заголовки, повторять нельзя: в поток уедут два ответа.
        const bool connectLevel =
            rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_OPERATION_TIMEDOUT ||
            rc == CURLE_COULDNT_CONNECT || rc == CURLE_GOT_NOTHING ||
            rc == CURLE_SEND_ERROR || rc == CURLE_RECV_ERROR;
        if (rc == CURLE_OK) {
            if (cand >= 0) s_lastGood.store(cand);
        }
        if (rc == CURLE_OK || cand < 0 || !connectLevel || ctx.headSent ||
            attempt + 1 >= orderLen) {
            break;
        }

        platform::bootLog("relay: SNI %s не подошёл (%s), пробуем следующее",
                          kSpoofNames[cand], curl_easy_strerror(rc));
        ctx = RelayCtx{};
        ctx.fd = fd;
        ctx.isHead = isHead;
    }

    // Возвращаем умолчание, чтобы состояние потока было предсказуемым.
    tubeSpoofSetName(nullptr);

    if (rc != CURLE_OK || !ctx.headSent) {
        {
            std::lock_guard<std::mutex> lk(g_dataMu);
            ++g_stats.errors;
        }
        if (!ctx.headSent) {
            // Финальный 3xx без Location, пустой ответ, обрыв до заголовков:
            // клиент не должен получить 0 байт — иначе ошибка не диагностируется.
            std::string msg = std::string("upstream error: ") +
                              (rc == CURLE_OK ? "no response headers"
                                              : curl_easy_strerror(rc)) +
                              "\n";
            sendSimple(fd, 502, "text/plain; charset=utf-8", msg);
        }
        // Если заголовки уже ушли — просто закрываем: клиент увидит обрыв,
        // что для видео корректно (mpv сделает повторный Range-запрос).
    }
}

void* relayClientThread(void* arg) {
    RelayJob* job = static_cast<RelayJob*>(arg);
    int fd = job->client;
    delete job;
    // Блокирующий send() без таймаута = вечно висящий поток и занятый слот,
    // когда mpv на паузе не читает. 10 с тишины -> EAGAIN -> poll-путь.
    struct timeval tv{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    relayHandle(fd);
    close(fd);
    g_relay.slots.release();
    return nullptr;
}

// Создаёт слушающий сокет строго на 127.0.0.1:port.
bool openListener(int& listenFd, int port) {
    listenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenFd < 0) return false;

    int one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(0x7F000001u); // только локальная петля (127.0.0.1)

    if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(listenFd);
        listenFd = -1;
        return false;
    }
    // Очередь побольше: пачка обложек + API + видео приходят одновременно.
    if (listen(listenFd, 64) != 0) {
        close(listenFd);
        listenFd = -1;
        return false;
    }
    // Иначе accept() может заблокироваться, если очередь опустела между
    // poll() и accept(); EAGAIN уже обрабатывается в accept-loop.
    int fl = fcntl(listenFd, F_GETFL, 0);
    if (fl >= 0) fcntl(listenFd, F_SETFL, fl | O_NONBLOCK);
    return true;
}

// Создаёт «просыпалку» для poll(): pipe, write-end неблокирующий.
// Horizon OS (newlib) не реализует pipe(), поэтому «просыпалка» для poll() —
// пара loopback-сокетов. Семантика та же: пишем байт в writeFd, читаем из readFd.
bool openWakePipe(int& readFd, int& writeFd) {
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) return false;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(0x7F000001u);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(ls);
        return false;
    }
    if (listen(ls, 1) != 0) {
        close(ls);
        return false;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
        close(ls);
        return false;
    }
    int c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c < 0) {
        close(ls);
        return false;
    }
    if (connect(c, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(c);
        close(ls);
        return false;
    }
    int s = accept(ls, nullptr, nullptr);
    close(ls);
    if (s < 0) {
        close(c);
        return false;
    }
    int fl = fcntl(c, F_GETFL, 0);
    if (fl >= 0) fcntl(c, F_SETFL, fl | O_NONBLOCK);
    readFd = s;
    writeFd = c;
    return true;
}

#else // TNX_HOST

// В host-сборке libcurl-заголовков может не быть, поэтому ретранслятор
// недоступен. Это единственное, что отключено флагом TNX_HOST.

// Создаёт слушающий сокет — он нужен и в host-сборке (SOCKS5-тест).
bool openListener(int& listenFd, int port) {
    listenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenFd < 0) return false;

    int one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(0x7F000001u); // 127.0.0.1

    if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(listenFd);
        listenFd = -1;
        return false;
    }
    // Очередь побольше: пачка обложек + API + видео приходят одновременно.
    if (listen(listenFd, 64) != 0) {
        close(listenFd);
        listenFd = -1;
        return false;
    }
    return true;
}

// Horizon OS (newlib) не реализует pipe(), поэтому «просыпалка» для poll() —
// пара loopback-сокетов. Семантика та же: пишем байт в writeFd, читаем из readFd.
bool openWakePipe(int& readFd, int& writeFd) {
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) return false;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(0x7F000001u);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(ls);
        return false;
    }
    if (listen(ls, 1) != 0) {
        close(ls);
        return false;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
        close(ls);
        return false;
    }
    int c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c < 0) {
        close(ls);
        return false;
    }
    if (connect(c, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(c);
        close(ls);
        return false;
    }
    int s = accept(ls, nullptr, nullptr);
    close(ls);
    if (s < 0) {
        close(c);
        return false;
    }
    int fl = fcntl(c, F_GETFL, 0);
    if (fl >= 0) fcntl(c, F_SETFL, fl | O_NONBLOCK);
    readFd = s;
    writeFd = c;
    return true;
}

#endif // TNX_HOST

// ---------------------------------------------------------------------------
// Публичный API
// ---------------------------------------------------------------------------

} // namespace

Config& config() {
    return g_cfg;
}

Stats& stats() {
    return g_stats;
}

void init() {
    std::lock_guard<std::mutex> lk(g_dataMu);
    ignoreSigpipe();
    // Значения по умолчанию уже в Config; сюда попадаем повторно безопасно.
    // Флаг active сохраняем: init() могут позвать уже после startSocks().
    bool running = g_socks.run.load();
    g_stats = Stats{};
    g_stats.active = running;
#ifndef TNX_HOST
    static std::once_flag curlOnce;
    std::call_once(curlOnce, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
#endif
}

const char* modeName(Mode m) {
    switch (m) {
    case Mode::Off: return "Выключено";
    case Mode::Split: return "Split (2 фрагмента)";
    case Mode::MultiSplit: return "Multi-split";
    case Mode::SplitDisorder: return "Split disorder";
    case Mode::TlsRecordSplit: return "TLS record split";
    }
    return "Неизвестно";
}

void setMode(Mode m) {
    std::lock_guard<std::mutex> lk(g_dataMu);
    g_cfg.mode = m;
    g_cfg.enabled = (m != Mode::Off);
}

int socksPort() {
    return g_socks.port.load();
}

bool startSocks(int port) {
    if (port <= 0 || port > 65535) return false;

    std::lock_guard<std::mutex> life(g_lifeMu);
    // loopExited: поток мог выйти по старой ошибке accept/poll, а run остался
    // true. Раньше startSocks() в этом случае возвращал true и не поднимал
    // прокси — все запросы падали до перезапуска приложения.
    if (g_socks.run.load() && !g_socks.loopExited.load() &&
        g_socks.port.load() == port) {
        return true;
    }

    // Если уже запущен (на другом порту или сломан) — аккуратно гасим.
    if (g_socks.run.load() || g_socks.listenFd >= 0) {
        g_socks.run.store(false);
        if (g_socks.listenFd >= 0) shutdown(g_socks.listenFd, SHUT_RDWR);
        if (g_socksWakeWrite >= 0) {
            char b = 1;
            ssize_t n = write(g_socksWakeWrite, &b, 1);
            (void)n;
        }
        if (g_socks.threadStarted) {
            pthread_join(g_socks.thread, nullptr);
            g_socks.threadStarted = false;
        }
        if (g_socks.listenFd >= 0) close(g_socks.listenFd);
        if (g_socks.wakeFd >= 0) close(g_socks.wakeFd);
        if (g_socksWakeWrite >= 0) close(g_socksWakeWrite);
        g_socks.listenFd = g_socks.wakeFd = g_socksWakeWrite = -1;
        g_socks.port.store(0);
        std::lock_guard<std::mutex> lk(g_dataMu);
        g_stats.active = false;
    }

    int lfd = -1, rfd = -1, wfd = -1;
    if (!openListener(lfd, port) || !openWakePipe(rfd, wfd)) {
        if (lfd >= 0) close(lfd);
        return false;
    }

    g_socks.listenFd = lfd;
    g_socks.wakeFd = rfd;
    g_socksWakeWrite = wfd;
    g_socks.port.store(port);
    g_socks.loopExited.store(false);
    g_socks.run.store(true);

    {
        std::lock_guard<std::mutex> lk(g_dataMu);
        g_stats.active = true;
    }

    if (createThreadWithStack(&g_socks.thread, socksAcceptLoop, nullptr, kNetStack) != 0) {
        g_socks.run.store(false);
        close(g_socks.listenFd);
        close(g_socks.wakeFd);
        close(g_socksWakeWrite);
        g_socks.listenFd = g_socks.wakeFd = g_socksWakeWrite = -1;
        g_socks.port.store(0);
        std::lock_guard<std::mutex> lk(g_dataMu);
        g_stats.active = false;
        return false;
    }
    g_socks.threadStarted = true;
    return true;
}

void stopSocks() {
    std::lock_guard<std::mutex> life(g_lifeMu);
    if (!g_socks.run.load() && g_socks.listenFd < 0) return;

    g_socks.run.store(false);

    // Будим живые туннели: иначе они продолжат работать после выключения
    // обхода и держать слоты в том же SlotLimiter.
    {
        std::lock_guard<std::mutex> lk(g_socks.liveMu);
        for (int fd : g_socks.live) shutdown(fd, SHUT_RDWR);
    }

    // shutdown() прослушивающего сокета — самый надёжный способ разбудить
    // accept() на libnx; запись в pipe — страховка для poll().
    if (g_socks.listenFd >= 0) shutdown(g_socks.listenFd, SHUT_RDWR);
    if (g_socksWakeWrite >= 0) {
        char b = 1;
        ssize_t n = write(g_socksWakeWrite, &b, 1);
        (void)n;
    }

    if (g_socks.threadStarted) {
        pthread_join(g_socks.thread, nullptr);
        g_socks.threadStarted = false;
    }

    if (g_socks.listenFd >= 0) close(g_socks.listenFd);
    if (g_socks.wakeFd >= 0) close(g_socks.wakeFd);
    if (g_socksWakeWrite >= 0) close(g_socksWakeWrite);
    g_socks.listenFd = g_socks.wakeFd = g_socksWakeWrite = -1;
    g_socks.port.store(0);

    std::lock_guard<std::mutex> lk(g_dataMu);
    g_stats.active = false;
}

int relayPort() {
    return g_relay.port.load();
}

std::string relayUrl(const std::string& upstreamUrl) {
    if (upstreamUrl.empty()) return std::string();
    int port = g_relay.port.load();
    if (port <= 0) port = 8899;
    return "http://127.0.0.1:" + std::to_string(port) + "/u?u=" +
           pctEncode(upstreamUrl);
}

bool startRelay(int port) {
#ifndef TNX_HOST
    if (port <= 0 || port > 65535) return false;

    std::lock_guard<std::mutex> life(g_lifeMu);
    if (g_relay.run.load() && g_relay.port.load() == port) return true;

    if (g_relay.run.load() || g_relay.listenFd >= 0) {
        g_relay.run.store(false);
        if (g_relay.listenFd >= 0) shutdown(g_relay.listenFd, SHUT_RDWR);
        if (g_relayWakeWrite >= 0) {
            char b = 1;
            ssize_t n = write(g_relayWakeWrite, &b, 1);
            (void)n;
        }
        if (g_relay.thread != 0) {
            pthread_join(g_relay.thread, nullptr);
            g_relay.thread = 0;
        }
        if (g_relay.listenFd >= 0) close(g_relay.listenFd);
        if (g_relay.wakeFd >= 0) close(g_relay.wakeFd);
        if (g_relayWakeWrite >= 0) close(g_relayWakeWrite);
        g_relay.listenFd = g_relay.wakeFd = g_relayWakeWrite = -1;
        g_relay.port.store(0);
    }

    int lfd = -1, rfd = -1, wfd = -1;
    if (!openListener(lfd, port) || !openWakePipe(rfd, wfd)) {
        if (lfd >= 0) close(lfd);
        return false;
    }

    g_relay.listenFd = lfd;
    g_relay.wakeFd = rfd;
    g_relayWakeWrite = wfd;
    g_relay.port.store(port);
    g_relay.run.store(true);

    if (createThreadWithStack(&g_relay.thread, relayAcceptLoop, nullptr, kNetStack) != 0) {
        g_relay.run.store(false);
        close(g_relay.listenFd);
        close(g_relay.wakeFd);
        close(g_relayWakeWrite);
        g_relay.listenFd = g_relay.wakeFd = g_relayWakeWrite = -1;
        g_relay.port.store(0);
        return false;
    }
    return true;
#else
    (void)port;
    return false; // host-сборка: нет заголовков libcurl для стриминга
#endif
}

void stopRelay() {
    std::lock_guard<std::mutex> life(g_lifeMu);
    if (!g_relay.run.load() && g_relay.listenFd < 0) return;

    g_relay.run.store(false);
    if (g_relay.listenFd >= 0) shutdown(g_relay.listenFd, SHUT_RDWR);
    if (g_relayWakeWrite >= 0) {
        char b = 1;
        ssize_t n = write(g_relayWakeWrite, &b, 1);
        (void)n;
    }
    if (g_relay.thread != 0) {
        pthread_join(g_relay.thread, nullptr);
        g_relay.thread = 0;
    }
    if (g_relay.listenFd >= 0) close(g_relay.listenFd);
    if (g_relay.wakeFd >= 0) close(g_relay.wakeFd);
    if (g_relayWakeWrite >= 0) close(g_relayWakeWrite);
    g_relay.listenFd = g_relay.wakeFd = g_relayWakeWrite = -1;
    g_relay.port.store(0);
}

ProbeResult probe(const char* host, int port, bool useBypass) {
    ProbeResult out;
    if (!host || !*host || port <= 0 || port > 65535) {
        out.error = "bad host/port";
        return out;
    }

    long long t0 = nowMs();

#ifndef TNX_HOST
    // На консоли проще и надёжнее померить полный HTTPS-запрос через net::get:
    // он и DNS проверит, и TLS, и наш SOCKS5 с фрагментацией.
    std::string url = "https://" + std::string(host);
    if (port != 443) url += ":" + std::to_string(port);
    url += "/";

    // Прокси задаём только для этого запроса: глобальный setProxy() под
    // работающими потоками — это гонка и потерянное обновление (две пробы
    // восстанавливают устаревшее значение друг друга).
    std::string want;
    if (useBypass) {
        const int sp = g_socks.port.load();
        if (sp <= 0) {
            out.error = "SOCKS-прокси не запущен";
            out.rttMs = static_cast<int>(nowMs() - t0);
            return out;
        }
        want = "socks5h://127.0.0.1:" + std::to_string(sp);
    }
    net::Overrides ov{want.c_str(), -1};

    net::Headers h;
    net::Response r = net::get(url, h, 12000, 8192, 3, &ov);

    out.rttMs = static_cast<int>(nowMs() - t0);
    out.status = r.status;
    out.ok = r.status > 0 && r.status < 500;
    if (!r.error.empty()) out.error = r.error;
#else
    // Host-сборка: меряем TCP-connect (напрямую или через наш же SOCKS5),
    // потому что libcurl-заголовков на хосте может не быть.
    int fd = -1;
    if (useBypass) {
        int sp = g_socks.port.load();
        if (sp <= 0) {
            out.error = "SOCKS-прокси не запущен";
            return out;
        }
        // Простейший SOCKS5 CONNECT через локальный прокси.
        fd = connectTcp("127.0.0.1", static_cast<uint16_t>(sp), 3000);
        if (fd >= 0) {
            unsigned char greet[3] = {0x05, 0x01, 0x00};
            unsigned char rep[2] = {0, 0};
            if (!writeAll(fd, greet, sizeof(greet)) || !readExact(fd, rep, 2, 3000) ||
                rep[0] != 0x05 || rep[1] != 0x00) {
                close(fd);
                fd = -1;
            } else {
                std::string h = host;
                std::vector<unsigned char> req;
                req.push_back(0x05);
                req.push_back(0x01);
                req.push_back(0x00);
                req.push_back(0x03);
                req.push_back(static_cast<unsigned char>(h.size()));
                req.insert(req.end(), h.begin(), h.end());
                req.push_back(static_cast<unsigned char>((port >> 8) & 0xFF));
                req.push_back(static_cast<unsigned char>(port & 0xFF));
                unsigned char head[4];
                if (!writeAll(fd, req.data(), req.size()) ||
                    !readExact(fd, head, 4, 8000) || head[1] != 0x00) {
                    close(fd);
                    fd = -1;
                } else {
                    // Дочитываем BND.ADDR/BND.PORT, иначе оставим мусор в буфере.
                    unsigned char tail[6];
                    if (head[3] == 0x03) {
                        unsigned char hl = 0;
                        if (!readExact(fd, &hl, 1, 3000)) {
                            close(fd);
                            fd = -1;
                        } else {
                            std::vector<unsigned char> skip(hl + 2);
                            if (!readExact(fd, skip.data(), skip.size(), 3000)) {
                                close(fd);
                                fd = -1;
                            }
                        }
                    } else if (head[3] == 0x01) {
                        if (!readExact(fd, tail, 6, 3000)) {
                            close(fd);
                            fd = -1;
                        }
                    } else if (head[3] == 0x04) {
                        unsigned char skip6[18];
                        if (!readExact(fd, skip6, sizeof(skip6), 3000)) {
                            close(fd);
                            fd = -1;
                        }
                    } else {
                        close(fd);
                        fd = -1;
                    }
                }
            }
        }
    } else {
        fd = connectTcp(host, static_cast<uint16_t>(port), 8000);
    }

    // Если соединение установлено — отправляем минимальный тестовый
    // «ClientHello», чтобы SOCKS-воркер не ждал первый пакет 15 секунд.
    if (fd >= 0) {
        unsigned char hello[64] = {0};
        hello[0] = 0x16; // TLS handshake
        hello[1] = 0x03;
        hello[2] = 0x01;
        hello[3] = 0x00;
        hello[4] = static_cast<unsigned char>(sizeof(hello) - 5);
        writeAll(fd, hello, sizeof(hello));
    }

    out.rttMs = static_cast<int>(nowMs() - t0);
    out.ok = (fd >= 0);
    out.status = 0;
    if (!out.ok) out.error = "connect failed";
    if (fd >= 0) close(fd);
#endif

    {
        std::lock_guard<std::mutex> lk(g_dataMu);
        g_stats.working = out.ok;
        g_stats.lastHandshakeMs = static_cast<float>(out.rttMs);
    }
    return out;
}

} // namespace tnx::dpi