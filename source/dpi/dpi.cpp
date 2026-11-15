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

// DPI-06: во время видео режим не меняем. Флаг ставят плеер/UI.
std::atomic<bool> g_videoPlaying{false};
// Кол-во ошибок на момент последнего удачного автоподбора (для перепроверки).
uint64_t g_autoBaselineErrors = 0;
// DPI-15: host-тест отключает реальное подключение к цели в SOCKS-рукопожатии,
// чтобы fuzz битого ввода не уходил в сеть.
std::atomic<bool> g_testNoUpstream{false};

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

// DPI-14: ждём флаг завершения потока не дольше ms. Нужно потому, что
// pthread_timedjoin_np есть не везде (в newlib/libnx — нет), а остановка
// обязана укладываться в разумное время, а не висеть на pthread_join().
bool waitFlag(const std::atomic<bool>& flag, int ms) {
    const long long deadline = nowMs() + ms;
    while (!flag.load()) {
        if (nowMs() >= deadline) return false;
        sleepMs(5);
    }
    return true;
}

int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ---------------------------------------------------------------------------
// Настройка сокетов (DPI-01/DPI-02): буферы, таймауты, неблокирующий режим.
// ---------------------------------------------------------------------------

// Таймауты на чтение/запись. На блокирующем сокете SO_RCVTIMEO/SO_SNDTIMEO
// превращают «вечное» ожидание в EAGAIN/EWOULDBLOCK, после которого слот
// освобождается. 15 с — компромисс между паузой плеера и «мёртвым» клиентом.
void setSocketTimeouts(int fd, int sec) {
    if (sec <= 0) return;
    struct timeval tv{};
    tv.tv_sec = sec;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Большие буферы ядра уменьшают число переключений контекста на потоке видео
// (DPI-01). Значения — подсказка ядру; фактический размер ядро может удвоить.
void setSocketBuffers(int fd, int bytes) {
    if (bytes <= 0) return;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

bool setNonBlocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
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

// DPI-03: бюджет upstream-сессий ретранслятора. Видео и аудио получают по
// одной сессии (итого две — столько SSL-сессий и переварит консоль), «прочее»
// (манифесты и прочие ответы через ретранслятор) — до двух. Всего не больше 4.
// Раздельные лимиты не дают одному видео вытеснить аудио и наоборот.
class SessionBudget {
public:
    enum class Kind { Video, Audio, Other };

    // Классификация по URL googlevideo: mime=video / mime=audio / иначе other.
    static Kind classify(const std::string& url) {
        if (url.find("mime=video") != std::string::npos) return Kind::Video;
        if (url.find("mime=audio") != std::string::npos) return Kind::Audio;
        return Kind::Other;
    }

    static const char* kindName(Kind k) {
        switch (k) {
        case Kind::Video: return "video";
        case Kind::Audio: return "audio";
        default: return "other";
        }
    }

    // Ждём слот нужного класса не дольше timeoutMs (плеер повторит Range).
    bool acquire(Kind k, const std::atomic<bool>& run, int timeoutMs) {
        std::unique_lock<std::mutex> lk(mu_);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        for (;;) {
            int& used = usedOf(k);
            if (used < limitOf(k) && total_ < kTotal) {
                ++used;
                ++total_;
                return true;
            }
            if (!run.load()) return false;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            cv_.wait_for(lk, std::chrono::milliseconds(50));
        }
    }

    void release(Kind k) {
        std::lock_guard<std::mutex> lk(mu_);
        int& used = usedOf(k);
        if (used > 0) --used;
        if (total_ > 0) --total_;
        cv_.notify_all();
    }

    void snapshot(int& v, int& a, int& o) {
        std::lock_guard<std::mutex> lk(mu_);
        v = video_;
        a = audio_;
        o = other_;
    }

private:
    static constexpr int kTotal = 4;
    static int limitOf(Kind k) {
        return k == Kind::Video ? 1 : (k == Kind::Audio ? 1 : 2);
    }
    int& usedOf(Kind k) {
        return k == Kind::Video ? video_ : (k == Kind::Audio ? audio_ : other_);
    }

    std::mutex mu_;
    std::condition_variable cv_;
    int video_ = 0;
    int audio_ = 0;
    int other_ = 0;
    int total_ = 0;
};

// ---------------------------------------------------------------------------
// TCP-подключение к цели (IPv4/IPv6, с таймаутом)
// ---------------------------------------------------------------------------

void socksLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Бюджеты логов обхода (определены ниже, нужны уже в connectTcp).
bool socksLogOn();
bool socksLogErrOn();

// Кэш DNS. На Switch libnx-getaddrinfo на этой сети иногда думает 3-9 секунд,
// а новое TCP-соединение открывается на каждый запрос (curl, обложки, API).
// Без кэша каждый запрос начинается с многогоризонтального ожидания.
struct DnsEntry {
    long long ts = 0;
    std::vector<sockaddr_in> addrs;  // только A-записи (IPv4), порт не заполнен
    int good = -1;                   // индекс последнего удачного адреса
    long long resolveMs = 0;         // сколько занял последний реальный resolve
};

std::mutex g_dnsMu;
std::map<std::string, DnsEntry> g_dns;
// NET-16: TTL 120 с. Раньше кэш жил 10 минут — за это время у провайдера
// успевал «умереть» выбранный IP, и приложение держалось за него. Две минуты
// — компромисс: getaddrinfo не дёргается на каждый запрос (на консоли он
// иногда думает секунды), но и мёртвый адрес не залипает надолго.
constexpr long long kDnsTtlMs = 120 * 1000;

// Резолвит host в СПИСОК адресов (с кэшем). Раньше брали только первый —
// и если провайдер режет именно этот IP (RST за 200 мс), все запросы к хосту
// падали, пока не истечёт TTL кэша: «нихуя не грузится».
bool resolveAll(const std::string& host, uint16_t port, std::vector<sockaddr_in>& out,
                bool forceFresh, long long* resolveMsOut = nullptr, bool* fromCacheOut = nullptr) {
    {
        std::lock_guard<std::mutex> lk(g_dnsMu);
        auto it = g_dns.find(host);
        if (!forceFresh && it != g_dns.end() && nowMs() - it->second.ts < kDnsTtlMs &&
            !it->second.addrs.empty()) {
            out.clear();
            for (const auto& a : it->second.addrs) {
                // NET-16: IPv6 в кэш не попадает и из кэша не отдаётся — на этой
                // сети его нет, а попытка подключения только съедает таймаут.
                if (a.sin_family != AF_INET) continue;
                out.push_back(a);
            }
            if (out.empty()) {
                g_dns.erase(it);
            } else {
                for (auto& a : out) a.sin_port = htons(port);
                if (resolveMsOut) *resolveMsOut = it->second.resolveMs;
                if (fromCacheOut) *fromCacheOut = true;
                return true;
            }
        }
    }
    if (fromCacheOut) *fromCacheOut = false;

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
    if (resolveMsOut) *resolveMsOut = dnsMs;

    std::vector<sockaddr_in> list;
    if (gai == 0 && res) {
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            // NET-16: кэшируем только IPv4.
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
        char ip[20] = {0};
        inet_ntop(AF_INET, &list.front().sin_addr, ip, sizeof(ip));
        socksLog("dns: %s -> %zu адрес(ов) за %lld мс, первый %s", host.c_str(), list.size(),
                 dnsMs, ip);
    }

    {
        std::lock_guard<std::mutex> lk(g_dnsMu);
        DnsEntry& e = g_dns[host];
        // Сохраняем «хороший» адрес, только если тот же IP остался в новом
        // списке: после смены выдачи индекс указывал бы на другой адрес.
        in_addr prevGood{};
        bool hadGood = false;
        if (e.good >= 0 && static_cast<size_t>(e.good) < e.addrs.size()) {
            prevGood = e.addrs[(size_t)e.good].sin_addr;
            hadGood = true;
        }
        e.ts = nowMs();
        e.addrs = list;
        e.resolveMs = dnsMs;
        e.good = -1;
        if (hadGood) {
            for (size_t i = 0; i < e.addrs.size(); ++i) {
                if (e.addrs[i].sin_addr.s_addr == prevGood.s_addr) {
                    e.good = (int)i;
                    break;
                }
            }
        }
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

// NET-16: «хороший» адрес перестал подключаться — забываем его. Иначе каждое
// новое соединение снова начиналось бы с мёртвого IP и платило perAddr
// миллисекунд на таймаут (заметно, когда провайдер режет один из адресов).
void clearGoodAddress(const std::string& host) {
    std::lock_guard<std::mutex> lk(g_dnsMu);
    auto it = g_dns.find(host);
    if (it != g_dns.end() && it->second.good >= 0) it->second.good = -1;
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
    const long long started = nowMs();
    const long long deadline = started + (timeoutMs > 0 ? timeoutMs : 15000);
    const int perAddr = timeoutMs > 0 && timeoutMs < 2500 ? timeoutMs : 2500;

    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<sockaddr_in> addrs;
        long long dnsMs = 0;
        bool cached = false;
        if (!resolveAll(host, port, addrs, attempt > 0, &dnsMs, &cached)) return -1;

        const int good = goodAddressIndex(host);
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < addrs.size(); ++i) {
                const bool isGood = (static_cast<int>(i) == good);
                if ((pass == 0) != isGood) continue;  // pass 0 — сначала «хороший»
                int fd = connectOne(addrs[i], perAddr);
                if (fd >= 0) {
                    markGoodAddress(host, addrs[i]);
                    // NET-16: выбранный IP и время resolve/connect. Пишем не
                    // каждое соединение (иначе 80 строк бюджета уходят за
                    // минуту), а когда есть что показать: свежий DNS, долгий
                    // resolve или переход на резервный адрес.
                    if (!cached || dnsMs >= 150 || !isGood) {
                        if (socksLogOn()) {
                            char ip[20] = {0};
                            inet_ntop(AF_INET, &addrs[i].sin_addr, ip, sizeof(ip));
                            socksLog("socks: %s:%d -> %s (адрес %zu/%zu%s, dns %lld мс%s, "
                                     "всего %lld мс)",
                                     host.c_str(), port, ip, i + 1, addrs.size(),
                                     isGood ? ", last-good" : "", dnsMs,
                                     cached ? " из кэша" : "", nowMs() - started);
                        }
                    }
                    return fd;
                }
                // NET-16: таймаут/отказ именно на «хорошем» адресе — сбрасываем.
                if (isGood) {
                    clearGoodAddress(host);
                    if (socksLogErrOn()) {
                        char ip[20] = {0};
                        inet_ntop(AF_INET, &addrs[i].sin_addr, ip, sizeof(ip));
                        socksLog("socks: %s:%d last-good %s не отвечает — сброшен, "
                                 "перебираем остальные", host.c_str(), port, ip);
                    }
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

// Строит план фрагментации первого пакета. Чистая функция (без сокетов и
// пауз): её же использует host-тест DPI-05, поэтому итоговые байты обязаны
// совпадать с исходными при любом режиме. Публичная обёртка — ниже, за
// пределами анонимного пространства имён (planFragmentsImpl).
FragmentPlan planFragmentsImpl(const char* data, size_t len, const Config& cfg) {
    FragmentPlan plan;
    if (len == 0) return plan;

    const Mode mode = cfg.mode;
    const int delay = clampInt(cfg.delayMs, 0, 1000);
    const int fragSize = clampInt(cfg.fragmentSize, 1, 16);

    auto whole = [&]() {
        plan.chunks.emplace_back(data, len);
        plan.delaysMs.push_back(0);
        plan.applied = false;
    };
    auto push = [&](const char* p, size_t n, int delayBefore) {
        plan.chunks.emplace_back(p, n);
        plan.delaysMs.push_back(delayBefore);
    };

    if (mode == Mode::Off) {
        whole();
        return plan;
    }

    if (mode == Mode::Split) {
        size_t first = static_cast<size_t>(fragSize);
        if (len <= first) {
            whole();
            return plan;
        }
        plan.applied = true;
        push(data, first, 0);
        push(data + first, len - first, delay);
        return plan;
    }

    if (mode == Mode::MultiSplit) {
        int nf = clampInt(cfg.fragments, 2, 32);
        size_t first = static_cast<size_t>(fragSize);
        if (len <= first + 1 || nf < 2) {
            whole();
            return plan;
        }
        plan.applied = true;
        push(data, first, 0);

        size_t rest = len - first;
        size_t parts = static_cast<size_t>(nf - 1);
        size_t base = rest / parts;
        size_t extra = rest % parts; // лишние байты достаются первым кускам
        size_t off = first;
        for (size_t i = 0; i < parts && off < len; ++i) {
            size_t chunk = base + (i < extra ? 1 : 0);
            if (chunk == 0) continue;
            if (chunk > len - off) chunk = len - off;
            push(data + off, chunk, delay);
            off += chunk;
        }
        if (off < len) push(data + off, len - off, delay);
        return plan;
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
        if (len < 4 || k == 0) {
            whole();
            return plan;
        }
        plan.applied = true;
        push(data, k, 0);
        push(data + k, len - k, delay * 3);
        return plan;
    }

    if (mode == Mode::TlsRecordSplit) {
        // Режем строго по границам TLS-записей: 5-байтовый заголовок
        // (type, version[2], length[2]), затем payload. Каждая запись —
        // отдельный send с паузой. ClientHello — обычно одна запись, поэтому
        // режим полезен, когда следом идёт вторая (например, CCS): тогда DPI
        // видит разрыв между записями.
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
            push(data + off, total, records > 0 ? delay : 0);
            off += total;
            ++records;
            plan.applied = true;
        }
        if (off < len) {
            push(data + off, len - off, records > 0 ? delay : 0);
        }
        return plan;
    }

    whole();
    return plan;
}

// Отправляет первый пакет согласно плану фрагментации. Возвращает true, если
// всё ушло. fragmentationApplied выставляется в true только если реально было
// >1 send. fragmentCount (если задан) — сколько сегментов ушло.
bool sendFirstPacket(int fd, const char* data, size_t len, const Config& cfg,
                     bool& fragmentationApplied, size_t* fragmentCount = nullptr) {
    FragmentPlan plan = planFragments(data, len, cfg);
    fragmentationApplied = plan.applied;
    if (fragmentCount) *fragmentCount = plan.chunks.size();
    for (size_t i = 0; i < plan.chunks.size(); ++i) {
        if (i > 0 && plan.delaysMs[i] > 0) sleepMs(plan.delaysMs[i]);
        if (!writeAll(fd, plan.chunks[i].data(), plan.chunks[i].size())) return false;
    }
    return true;
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
    bool threadStarted = false;
    std::atomic<bool> loopExited{false}; // DPI-14: accept-loop завершился
};

RelayServer g_relay;
int g_relayWakeWrite = -1;

#ifndef TNX_HOST
// Диагностика ретранслятора: в лог идут только первые запросы плеера, чтобы
// было видно, доходит ли mpv до ретранслятора и что отвечает googlevideo.
std::atomic<int> g_relayLogLines{0};

// DPI-03: бюджет upstream-сессий (video/audio/other) и его периодический лог.
SessionBudget g_relaySessions;
std::atomic<long long> g_relaySessionsLog{0};

// Логирует занятость бюджета не чаще раза в 5 с, чтобы не забивать SD.
void relayLogSessions(const char* tag) {
    const long long now = nowMs();
    long long last = g_relaySessionsLog.load();
    if (last != 0 && now - last < 5000) return;
    if (!g_relaySessionsLog.compare_exchange_strong(last, now)) return;
    int v = 0, a = 0, o = 0;
    g_relaySessions.snapshot(v, a, o);
    platform::bootLog("relay: sessions video=%d audio=%d other=%d (%s)", v, a, o, tag);
}

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

// DPI-04: человекочитаемое имя кода REP из SOCKS5-ответа.
const char* socksRepText(unsigned char rep) {
    switch (rep) {
    case 0x00: return "succeeded";
    case 0x01: return "general failure";
    case 0x02: return "not allowed";
    case 0x03: return "network unreachable";
    case 0x04: return "host unreachable";
    case 0x05: return "connection refused";
    case 0x06: return "TTL expired";
    case 0x07: return "command not supported";
    case 0x08: return "address type not supported";
    default: return "unknown";
    }
}

// LOG-04 + DPI-04: агрегат рукопожатий. Успешный handshake больше не пишется
// в лог построчно — только одна сводка не чаще раза в 60 с. Счётчики
// атомарные, «гонка» на границе окна безвредна.
struct SocksAgg {
    std::atomic<uint64_t> ok{0};
    std::atomic<uint64_t> fail{0};
    std::atomic<uint64_t> fragmented{0};
    std::atomic<long long> lastLog{0};
};
SocksAgg g_socksAgg;

void socksAggTick(bool ok, bool fragmented) {
    if (ok) g_socksAgg.ok.fetch_add(1, std::memory_order_relaxed);
    else g_socksAgg.fail.fetch_add(1, std::memory_order_relaxed);
    if (fragmented) g_socksAgg.fragmented.fetch_add(1, std::memory_order_relaxed);

    const long long now = nowMs();
    long long last = g_socksAgg.lastLog.load(std::memory_order_relaxed);
    if (last == 0) {
        g_socksAgg.lastLog.store(now, std::memory_order_relaxed);
        return; // первое рукопожатие открывает окно, но не пишет строку
    }
    if (now - last < 60000) return;
    if (!g_socksAgg.lastLog.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return; // окно уже закрыл другой поток
    }
    const uint64_t o = g_socksAgg.ok.exchange(0, std::memory_order_relaxed);
    const uint64_t f = g_socksAgg.fail.exchange(0, std::memory_order_relaxed);
    const uint64_t fr = g_socksAgg.fragmented.exchange(0, std::memory_order_relaxed);
    socksLog("socks: handshakes за %lld с: ok=%llu fail=%llu fragmented=%llu",
             (now - last) / 1000, static_cast<unsigned long long>(o),
             static_cast<unsigned long long>(f),
             static_cast<unsigned long long>(fr));
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
// fragmentedOut (если задан) — фрагментировался ли ClientHello (для агрегата).
int socksHandshake(int client, bool* fragmentedOut = nullptr) {
    if (fragmentedOut) *fragmentedOut = false;
    // DPI-04: отметки этапов greeting -> auth -> CONNECT -> upstream -> reply.
    const long long tStart = nowMs();
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
    // Авторизацию не поддерживаем, поэтому отдельного этапа auth нет — выбор
    // метода уходит вместе с greeting. Отметка нужна для раскладки по этапам.
    const long long tGreeting = nowMs();

    unsigned char req[4];
    if (!readExact(client, req, 4, left())) return -1;
    if (req[0] != 0x05) return -1;
    if (req[1] != 0x01) { // только CONNECT
        if (socksLogErrOn()) {
            socksLog("socks: CONNECT cmd=0x%02x не поддержан (этап CONNECT, "
                     "%lld мс), rep=0x07 (%s)",
                     req[1], nowMs() - tGreeting, socksRepText(0x07));
        }
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
        if (socksLogErrOn()) {
            socksLog("socks: CONNECT atyp=0x%02x не поддержан, rep=0x08 (%s)",
                     atyp, socksRepText(0x08));
        }
        sendSocksReply(client, 0x08); // address type not supported
        return -1;
    }

    unsigned char pb[2];
    if (!readExact(client, pb, 2, left())) return -1;
    uint16_t port = static_cast<uint16_t>((pb[0] << 8) | pb[1]);
    const long long tReq = nowMs();

    // DPI-04: одна строка с этапами — только при ошибке или заметной задержке.
    // Успешные рукопожатия идут в агрегат (LOG-04), а не построчно.
    auto logStages = [&](unsigned char rep, const char* why, long long dUpstream,
                         long long dReply) {
        if (!socksLogErrOn()) return;
        socksLog("socks: %s:%u этапы greeting=%lld auth=0 connect=%lld upstream=%lld "
                 "reply=%lld rep=0x%02x (%s): %s",
                 host.c_str(), static_cast<unsigned>(port), tGreeting - tStart,
                 tReq - tGreeting, dUpstream, dReply, rep, socksRepText(rep), why);
    };

    if (port == 0) {
        logStages(0x01, "port=0", 0, 0);
        sendSocksReply(client, 0x01);
        return -1;
    }

    // DPI-15: host-тест может отключить реальное подключение к цели — тогда
    // fuzz битого ввода не уходит в сеть, но весь разбор до этой точки
    // прогоняется. Для прода флаг всегда false.
    if (g_testNoUpstream.load()) {
        sendSocksReply(client, 0x05);
        return -1;
    }

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
        logStages(0x05, "upstream connect failed", nowMs() - tReq, 0);
        sendSocksReply(client, 0x05); // connection refused / general failure
        return -1;
    }

    const long long tUpstream = nowMs();
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
        if (socksLogErrOn()) {
            socksLog("socks: %s:%u -> клиент молчит после CONNECT (%lld мс, "
                     "upstream=%lld мс)",
                     host.c_str(), static_cast<unsigned>(port), nowMs() - tReq,
                     tUpstream - tReq);
        }
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
    size_t fragCount = 1;
    bool okSend = true;
    if (wantFragment) {
        okSend = sendFirstPacket(upstream, first.data(), first.size(), cfg, applied,
                                 &fragCount);
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
        g_stats.lastHandshakeMs = static_cast<float>(nowMs() - tStart);
    }
    if (fragmentedOut) *fragmentedOut = applied;

    // LOG-04: успешный handshake НЕ пишем построчно. Единственное исключение —
    // заметно медленное рукопожатие (полезно для диагноза) и DPI-05-маркер
    // fragmented=N. Всё остальное уходит в агрегат socksAggTick().
    const long long totalMs = nowMs() - tStart;
    if (totalMs >= 1500 && socksLogOn()) {
        socksLog("socks: %s:%u готово за %lld мс fragmented=%zu", host.c_str(),
                 static_cast<unsigned>(port), totalMs, fragCount);
    }

    return upstream;
}

// Ретрансляция клиент <-> цель до закрытия любой из сторон.
//
// DPI-01: буфер 64 КиБ, неблокирующие сокеты и чтение в цикле до EAGAIN —
// за одну итерацию poll() выгребаем весь доступный буфер ядра, а не 8 КБ.
// poll() остаётся только как «спячка», а не как арбитр каждого send.
// DPI-02: таймауты/буферы сокетов выставлены в socksClientThread, записи
// ограничены EAGAIN+ожиданием; «мёртвый» клиент не держит слот.
void socksRelay(int client, int upstream, uint64_t* bytesUp, uint64_t* bytesDown) {
    std::vector<char> buf(64 * 1024);
    bool clientOpen = true;
    bool serverOpen = true;
    bool fatal = false;
    const long long t0 = nowMs();
    long long lastData = t0;
    // 10 минут тишины: короче — рвём keep-alive curl и платим за TLS заново;
    // длиннее — «мёртвые» туннели держат слоты. Поток видео всегда с данными,
    // поэтому большой лимит времени не мешает многочасовому фильму.
    constexpr long long kIdleMs = 10LL * 60000;
    constexpr long long kMaxMs = 6LL * 3600 * 1000;
    // DPI-01: раз в 5 с пишем фактическую скорость и число recv/send.
    constexpr long long kRateMs = 5000;

    uint64_t winRecv = 0, winSend = 0;       // recv/send за текущее окно 5 с
    uint64_t winBytes = 0;                   // байт в обе стороны за окно
    long long lastRate = t0;

    // Неблокирующий режим: recv возвращает EAGAIN, когда данных больше нет,
    // и мы сразу отдаём управление poll(). writeAll() умеет ждать POLLOUT.
    setNonBlocking(client);
    setNonBlocking(upstream);

    // Читает из `from` в цикле (до EAGAIN), отдаёт всё в `to`.
    // Ok — данных пока нет; PeerClosed — from закрыт; Fatal — запись не удалась.
    enum class Pump { Ok, PeerClosed, Fatal };
    auto pumpDir = [&](int from, int to, uint64_t& bytes) -> Pump {
        for (;;) {
            ssize_t n = recv(from, buf.data(), buf.size(), 0);
            if (n > 0) {
                lastData = nowMs();
                ++winRecv;
                if (!writeAll(to, buf.data(), static_cast<size_t>(n))) return Pump::Fatal;
                ++winSend;
                bytes += static_cast<uint64_t>(n);
                winBytes += static_cast<uint64_t>(n);
                {
                    std::lock_guard<std::mutex> lk(g_dataMu);
                    g_stats.relayedBytes += static_cast<uint64_t>(n);
                }
                continue; // возможно, в буфере ядра есть ещё — читаем дальше
            }
            if (n == 0) return Pump::PeerClosed;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return Pump::Ok;
            return Pump::Fatal;
        }
    };

    while (clientOpen || serverOpen) {
        pollfd pfd[2];
        pfd[0].fd = clientOpen ? client : -1;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = serverOpen ? upstream : -1;
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;

        int pr = poll(pfd, 2, 1000);
        const long long now = nowMs();
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr > 0) {
            // Клиент -> upstream. Важно проверять HUP/ERR: на Switch poll
            // может не выставить POLLIN при разрыве.
            if (clientOpen && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                Pump r = pumpDir(client, upstream, *bytesUp);
                if (r == Pump::PeerClosed) {
                    clientOpen = false;
                    shutdown(upstream, SHUT_WR); // полузакрытие: ответ ещё придёт
                } else if (r == Pump::Fatal) {
                    std::lock_guard<std::mutex> lk(g_dataMu);
                    ++g_stats.errors;
                    fatal = true;
                }
            }

            if (!fatal && serverOpen &&
                (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                Pump r = pumpDir(upstream, client, *bytesDown);
                if (r == Pump::PeerClosed) {
                    serverOpen = false;
                    shutdown(client, SHUT_WR);
                } else if (r == Pump::Fatal) {
                    std::lock_guard<std::mutex> lk(g_dataMu);
                    ++g_stats.errors;
                    serverOpen = false;
                }
            }
        } else {
            if (now - lastData > kIdleMs || now - t0 > kMaxMs) break;
        }

        // DPI-01: не чаще раза в 5 с — скорость и число recv/send за окно.
        if (now - lastRate >= kRateMs) {
            const double secs = static_cast<double>(now - lastRate) / 1000.0;
            const double kbs = winBytes / 1024.0 / (secs > 0 ? secs : 1.0);
            socksLog("relay: rate=%.1f KB/s recv=%llu send=%llu up=%llu down=%llu",
                     kbs, static_cast<unsigned long long>(winRecv),
                     static_cast<unsigned long long>(winSend),
                     static_cast<unsigned long long>(*bytesUp),
                     static_cast<unsigned long long>(*bytesDown));
            lastRate = now;
            winBytes = winRecv = winSend = 0;
        }

        if (fatal) break;
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

    // DPI-02: явные таймауты чтения/записи и большие буферы ядра.
    setSocketTimeouts(client, 15);
    setSocketBuffers(client, 256 * 1024);

    bool fragmented = false;
    int upstream = socksHandshake(client, &fragmented);
    if (upstream >= 0) {
        setSocketTimeouts(upstream, 15);
        setSocketBuffers(upstream, 256 * 1024);
        uint64_t up = 0, down = 0;
        socksRelay(client, upstream, &up, &down);
        // LOG-04: закрытие каждого туннеля больше не пишем — только агрегат.
        close(upstream);
    } else {
        std::lock_guard<std::mutex> lk(g_dataMu);
        ++g_stats.errors;
    }
    socksAggTick(upstream >= 0, fragmented);
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
// DPI-09..DPI-12, DPI-16: HTTP-помощники ретранслятора.
//
// Намеренно НЕ зависят от libcurl: собираются и под -DTNX_HOST, а значит
// проверяются host-тестом (DPI-15). Здесь вся «чистая» логика ретранслятора:
// percent-декодирование URL, разбор HTTP-запроса, коды ошибок curl -> HTTP,
// ответы, валидация Range и текст /healthz.
// ---------------------------------------------------------------------------

// openListener определён ниже (в host- и device-ветке). Нужен DPI-13.
bool openListener(int& listenFd, int port);

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// DPI-09: percent-декодирование. '+' остаётся как есть (это не пробел в
// RFC 3986); неполный/битый '%xx' сохраняется буквально, чтобы не терять байты.
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
        out.push_back(s[i]);
    }
    return out;
}

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

// DPI-15: чистая часть readHttpRequest — разбор уже прочитанного буфера.
// Никогда не падает и не читает за пределами строки.
bool parseHttpRequest(const std::string& buf, HttpRequest& rq) {
    rq = HttpRequest{};
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
            rq.headers.emplace_back(line.substr(0, colon),
                                    trimCrlf(line.substr(colon + 1)));
        }
        pos = eol + 2;
    }
    return true;
}

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
    return parseHttpRequest(buf, rq);
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
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "OK";
    }
}

// DPI-11/DPI-12: ответ с дополнительными заголовками (X-TubeNX-Error,
// Retry-After). Content-Length всегда согласован с телом: «200 с пустым телом»
// как маркер ошибки здесь невозможен.
bool sendResponse(int fd, int code, const char* contentType, const std::string& body,
                  const std::string& extraHeaders = std::string()) {
    std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reasonPhrase(code) + "\r\n";
    out += "Content-Type: ";
    out += contentType;
    out += "\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
    if (!extraHeaders.empty()) out += extraHeaders;
    out += "Connection: close\r\n\r\n";
    out += body;
    return writeAll(fd, out.data(), out.size());
}

// Используется только ретранслятором (device-ветка); в host-сборке остаётся
// объявленной, но не вызываемой — не считаем это предупреждением.
[[maybe_unused]] bool sendSimple(int fd, int code, const char* contentType,
                                 const std::string& body) {
    return sendResponse(fd, code, contentType, body);
}

// DPI-11: числовые коды curl (без включения curl/curl.h — функция нужна и на
// хосте). Значения совпадают с CURLE_* из curl.h.
enum CurlCode {
    kCurlOk = 0,
    kCurlCouldntResolveHost = 6,
    kCurlCouldntConnect = 7,
    kCurlPartialFile = 18,
    kCurlOperationTimedout = 28,
    kCurlSslConnectError = 35,
    kCurlTooManyRedirects = 47,
    kCurlGotNothing = 52,
    kCurlSendError = 55,
    kCurlRecvError = 56,
};

// Маппинг curl -> HTTP. Таймаут — 504, всё остальное — 502.
int curlCodeToHttp(int curlCode) {
    if (curlCode == kCurlOk) return 0;
    if (curlCode == kCurlOperationTimedout) return 504;
    return 502;
}

std::string httpErrorHeaders(int curlCode, int retryAfterSec) {
    std::string h;
    if (curlCode > 0) h += "X-TubeNX-Error: curl " + std::to_string(curlCode) + "\r\n";
    if (retryAfterSec > 0) h += "Retry-After: " + std::to_string(retryAfterSec) + "\r\n";
    return h;
}

[[maybe_unused]] std::string httpErrorBody(int code, int curlCode, const std::string& detail) {
    std::string b = "relay error " + std::to_string(code);
    if (curlCode > 0) b += " (curl " + std::to_string(curlCode) + ")";
    b += ": ";
    b += detail.empty() ? reasonPhrase(code) : detail;
    b += "\n";
    return b;
}

// DPI-09: URL длиннее 8 КБ ретранслятор не принимает (414).
bool relayUrlTooLong(const std::string& url) {
    return url.size() > kMaxRelayUrl;
}

// DPI-09: из path вида /u?u=<urlencoded> достаёт upstream. false — параметра u
// нет или он пуст.
bool parseRelayUpstream(const std::string& path, std::string& upstream) {
    upstream.clear();
    size_t q = path.find('?');
    if (q == std::string::npos) return false;
    std::string query = path.substr(q + 1);
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair =
            query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == "u") {
            upstream = pctDecode(pair.substr(eq + 1));
            return !upstream.empty();
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return false;
}

// DPI-10: проверка байтового Range. Формы: "bytes=a-b", "bytes=a-", "bytes=-n".
// Пустая строка/без префикса — не Range (true только для пустой).
bool validByteRange(const std::string& value) {
    if (value.empty()) return true;
    if (value.rfind("bytes=", 0) != 0) return false;
    std::string r = value.substr(6);
    size_t dash = r.find('-');
    if (dash == std::string::npos) return false;
    std::string a = r.substr(0, dash);
    std::string b = r.substr(dash + 1);
    auto digits = [](const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    };
    if (dash == 0) return digits(b);            // "-n"
    if (b.empty()) return digits(a);            // "n-"
    return digits(a) && digits(b);              // "n-m"
}

// Контекст одного запроса к upstream: сюда curl кладёт заголовки, отсюда
// байты уходят прямо в сокет клиента (без буферизации всего видео в памяти).
struct RelayCtx {
    int fd = -1;
    bool isHead = false;
    bool headSent = false;
    bool failed = false;
    bool clientGone = false;
    bool timedOut = false; // запись в клиент упёрлась в таймаут (DPI-02)
    bool rangeRequested = false; // клиент просил Range (DPI-10)
    bool rangeIgnored = false;   // upstream ответил 200, проигнорировав Range
    long code = 0;
    std::string reason;
    std::string contentType;
    std::string contentLength;
    std::string acceptRanges;
    std::string contentRange;
    bool chunked = false; // upstream отдаёт chunked: длину клиенту не сообщаем
    uint64_t bodyBytes = 0;
    // DPI-18 (диагностика): подробный лог SOCKS/TLS первых upstream-запросов
    // и текст ошибки libcurl — без него видно только «Couldn't connect».
    // Только на устройстве: в host-сборке curl.h не подключается.
#ifndef TNX_HOST
    int debugLines = 0;
    char errbuf[CURL_ERROR_SIZE] = {0};
#endif
    // DPI-19: поток склеен из нескольких upstream-запросов фиксированного
    // размера (googlevideo не отдаёт открытый Range). Клиенту уходит один
    // непрерывный 206 без Content-Length/Content-Range.
    bool aggregate = false;
    uint64_t chunkBytes = 0; // сколько принёс текущий кусок

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

[[maybe_unused]] bool sendRelayHead(RelayCtx* c) {
    if (c->headSent) return true;
    long code = c->code ? c->code : 200;
    std::string out = "HTTP/1.1 " + std::to_string(code) + " ";
    out += c->reason.empty() ? reasonPhrase(code) : c->reason;
    out += "\r\n";
    if (!c->contentType.empty()) out += "Content-Type: " + c->contentType + "\r\n";
    // При склейке кусков upstream-длины и Content-Range относятся к одному
    // куску, а клиенту уходит непрерывный поток до закрытия — их не отдаём.
    // В обычном режиме при chunked длина после раскодирования неизвестна:
    // отдаём тело до закрытия соединения (Connection: close).
    if (!c->aggregate && !c->contentLength.empty() && !c->chunked) {
        out += "Content-Length: " + c->contentLength + "\r\n";
    }
    // Не выдумываем Accept-Ranges: если upstream его не прислал и это не 206,
    // каждый Range вернётся полным 200, и seek будет перезапускать поток.
    if (!c->acceptRanges.empty()) {
        out += "Accept-Ranges: " + c->acceptRanges + "\r\n";
    } else if (code == 206) {
        out += "Accept-Ranges: bytes\r\n";
    }
    if (!c->aggregate && !c->contentRange.empty()) out += "Content-Range: " + c->contentRange + "\r\n";
    if (c->rangeIgnored) out += "X-TubeNX-Range-Ignored: 1\r\n";
    out += "Connection: close\r\n\r\n";
    // Даже частичная запись означает «заголовки начаты»: второй статус-строки
    // в потоке быть не должно.
    c->headSent = true;
    return writeAll(c->fd, out.data(), out.size());
}

// DPI-16: /healthz. Валидный текст, цифры меняются вместе со статистикой.
std::string healthzText(const Stats& s, int video, int audio, int other, int slots) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "ok connections=%llu fragmented=%llu relayed=%llu errors=%llu "
                  "rate=%.1fKB/s active=%d sslSessions=%llu sessions video=%d "
                  "audio=%d other=%d slots=%d lastError=%s\n",
                  static_cast<unsigned long long>(s.connections),
                  static_cast<unsigned long long>(s.fragmented),
                  static_cast<unsigned long long>(s.relayedBytes),
                  static_cast<unsigned long long>(s.errors),
                  static_cast<double>(s.rateKbps), static_cast<int>(s.active),
                  static_cast<unsigned long long>(s.sslSessions), video, audio, other,
                  slots, s.lastError.empty() ? "-" : s.lastError.c_str());
    return std::string(buf);
}

// DPI-13: занимаем локальный порт; занят — берём случайный свободный.
bool bindLocalListener(int wantPort, int& outFd, int& outPort) {
    int fd = -1;
    if (openListener(fd, wantPort)) {
        sockaddr_in a{};
        socklen_t l = sizeof(a);
        int p = wantPort;
        // Порт 0 означает «выбери сам»: узнаём фактический через getsockname.
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l) == 0) {
            p = ntohs(a.sin_port);
        }
        outFd = fd;
        outPort = p;
        return true;
    }
    if (wantPort == 0) return false;
    if (openListener(fd, 0)) {
        sockaddr_in a{};
        socklen_t l = sizeof(a);
        int p = 0;
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l) == 0) {
            p = ntohs(a.sin_port);
        }
        outFd = fd;
        outPort = p;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// HTTP-ретранслятор для плеера (нужен libcurl, поэтому в host-сборке вырезан)
// ---------------------------------------------------------------------------

#ifndef TNX_HOST

// DPI-18: подробный upstream-лог (SOCKS/TLS) для первых relay-запросов.
// Нужен, когда curl возвращает «Couldn't connect» без единой подсказки:
// здесь видно, дошёл ли он до прокси, что ответил SOCKS5 и где оборвалось.
// CURLINFO_TEXT — служебные строки libcurl, подписанных query-параметров в них
// нет; HEADER_OUT (строки запроса) намеренно не логируем.
int relayDebugCb(CURL*, curl_infotype type, char* data, size_t size, void* user) {
    if (type != CURLINFO_TEXT || !user || !data) return 0;
    RelayCtx* c = static_cast<RelayCtx*>(user);
    if (c->debugLines >= 40) return 0;
    ++c->debugLines;
    std::string line(data, size);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) return 0;
    if (line.size() > 200) line.resize(200);
    platform::bootLog("relay curl: %s", line.c_str());
    return 0;
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
            // SO_SNDTIMEO уже вернул EAGAIN (до 15 с). Ждём ещё до 10 с: если
            // клиент так и не читает, это таймаут (DPI-02) — рвём поток, и
            // слот освобождается быстрее 30 с (15 + 10).
            if (poll(&pfd, 1, 10000) <= 0) {
                c->timedOut = true;
                break;
            }
            continue;
        }
        break;
    }
    if (off < len) {
        c->clientGone = true;
        if (c->timedOut && g_relayLogLines.fetch_add(1) < 400) {
            platform::bootLog("relay: таймаут записи в клиент (mpv не читает), обрыв");
        }
        return 0;
    }

    c->bodyBytes += len;
    c->chunkBytes += len;
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
        // DPI-10: если клиент просил Range, а upstream ответил 200 — сервер
        // проигнорировал диапазон. Не ломаемся: отдаём полный ответ, но
        // помечаем это заголовком X-TubeNX-Range-Ignored.
        if (c->code == 200 && c->rangeRequested) c->rangeIgnored = true;
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
            // DPI-12: все слоты заняты. Раньше был просто close() с
            // непрочитанным запросом -> RST -> curl 7. Теперь честный
            // 503 + Retry-After: 1 и вычитка, чтобы не было RST. Это НЕ
            // фатальная ошибка: плеер повторит Range.
            if (g_relayLogLines.fetch_add(1) < 400) {
                platform::bootLog("relay: slots full (%d/%d), 503 Retry-After: 1",
                                  g_relay.slots.used(), 4);
            }
            sendResponse(c, 503, "text/plain; charset=utf-8",
                         "relay busy: slots full\n", "Retry-After: 1\r\n");
            drainAndClose(c);
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
    g_relay.loopExited.store(true); // DPI-14
    return nullptr;
}

// DPI-19: googlevideo с `rqh=1` отдаёт 403 и на запрос без Range, и на
// открытый `Range: 0-` (проверено с ПК через тот же прокси: 403), а закрытый
// диапазон работает (1–3 МБ — 206, 4 МБ и больше — снова 403). Поэтому
// ретранслятор сам запрашивает поток кусками и склеивает их в один ответ.
constexpr uint64_t kRelayChunk = 2ull * 1024 * 1024; // 2 МБ — с запасом до 3 МБ
// Часть видео на узле кэширована не целиком: кусок с концом за ~3 МиБ
// получает 403. Тогда уменьшаем окно и дотягиваем до предела.
constexpr uint64_t kRelayChunkSmall = 256ull * 1024;
constexpr int kRelayMaxChunks = 512; // предохранитель от цикла

// Открытый Range — `0-` или пустой (mpv по умолчанию просит весь файл).
bool rangeOpenEnded(const std::string& r) { return r.empty() || r.back() == '-'; }

// Начало диапазона из `N-…` (только цифры до дефиса).
bool rangeStartOffset(const std::string& r, uint64_t& out) {
    out = 0;
    if (r.empty()) return true;
    size_t i = 0;
    uint64_t v = 0;
    while (i < r.size() && r[i] >= '0' && r[i] <= '9') {
        if (v > (UINT64_MAX - 9) / 10) return false;
        v = v * 10 + (uint64_t)(r[i] - '0');
        ++i;
    }
    if (i == 0) return false;
    out = v;
    return true;
}

void relayHandle(int fd) {
    const long long sessionStartMs = nowMs(); // DPI-16: для метрики rate
    HttpRequest rq;
    if (!readHttpRequest(fd, rq)) {
        platform::bootLog("relay: bad/too-long request, dropped");
        return;
    }
    if (rq.method != "GET" && rq.method != "HEAD") {
        sendSimple(fd, 405, "text/plain; charset=utf-8", "method not allowed\n");
        return;
    }

    // /healthz — простая проверка живости для UI/тестов (DPI-16).
    if (rq.path == "/healthz") {
        Stats s;
        {
            std::lock_guard<std::mutex> lk(g_dataMu);
            s = g_stats;
        }
        int v = 0, a = 0, o = 0;
        g_relaySessions.snapshot(v, a, o);
        std::string body = healthzText(s, v, a, o, g_relay.slots.used());
        sendSimple(fd, 200, "text/plain; charset=utf-8", body);
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

    // DPI-09: разбор /u?u=<urlencoded-upstream-url>. Длинные URL не режем —
    // честно отвечаем 414, иначе подписанная ссылка молча исказится.
    std::string upstream;
    const bool hasU = parseRelayUpstream(rq.path, upstream);
    if (relayUrlTooLong(rq.path) || relayUrlTooLong(upstream)) {
        if (g_relayLogLines.fetch_add(1) < 400) {
            platform::bootLog("relay: URL слишком длинный (%zu/%zu байт), 414",
                              rq.path.size(), upstream.size());
        }
        sendSimple(fd, 414, "text/plain; charset=utf-8",
                   "relay error 414: URI too long (limit 8192 bytes)\n");
        return;
    }
    if (!hasU ||
        (upstream.rfind("http://", 0) != 0 && upstream.rfind("https://", 0) != 0)) {
        sendSimple(fd, 400, "text/plain; charset=utf-8",
                   "usage: /u?u=<urlencoded http(s) url>\n");
        return;
    }

    bool isHead = (rq.method == "HEAD");

    RelayCtx ctx;
    ctx.fd = fd;
    ctx.isHead = isHead;

    // DPI-10: Range пробрасываем как есть (без префикса "bytes="), чтобы mpv
    // мог seek-ить. Битый Range не отправляем в curl — отдаём честный 416.
    const std::string rangeRaw = rq.header("Range");
    if (!validByteRange(rangeRaw)) {
        if (g_relayLogLines.fetch_add(1) < 400) {
            platform::bootLog("relay: некорректный Range '%s', 416", rangeRaw.c_str());
        }
        sendResponse(fd, 416, "text/plain; charset=utf-8",
                     "relay error 416: invalid Range\n", "Accept-Ranges: bytes\r\n");
        return;
    }
    std::string range;
    if (rangeRaw.rfind("bytes=", 0) == 0) range = rangeRaw.substr(6);

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
        proxy = "socks5h://127.0.0.1:" + std::to_string(socksPort());
    }
    if (logIt) {
        // LOG-06/SEC-03: прокси в лог — без пароля.
        const std::string proxyLog = proxy.empty() ? std::string("нет") : net::redactProxy(proxy);
        platform::bootLog("relay: %s %s range='%s' proxy=%s", rq.method.c_str(), upHost.c_str(),
                          range.c_str(), proxyLog.c_str());
    }

    // DPI-03: классифицируем сессию (video/audio/other) и укладываемся в бюджет
    // upstream. Видео и аудио получают по слоту, «прочее» — до двух; одному
    // потоку не даём вытеснить другой. Клиент на отказ повторит Range.
    const SessionBudget::Kind kind = SessionBudget::classify(upstream);
    if (!g_relaySessions.acquire(kind, g_relay.run, 3000)) {
        // DPI-12: бюджет занят — 503 + Retry-After: 1, клиент повторит Range.
        // Это не фатальная ошибка.
        if (g_relayLogLines.fetch_add(1) < 400) {
            platform::bootLog("relay: бюджет %s занят, 503 Retry-After: 1 "
                              "(клиент повторит Range)",
                              SessionBudget::kindName(kind));
        }
        sendResponse(fd, 503, "text/plain; charset=utf-8",
                     "relay busy: upstream session budget exhausted\n",
                     "Retry-After: 1\r\n");
        return;
    }
    relayLogSessions("acquire");
    ctx.rangeRequested = !range.empty();
    {
        std::lock_guard<std::mutex> lk(g_dataMu);
        ++g_stats.sslSessions; // DPI-16
        ++g_stats.activeSessions;
    }

    // DPI-08: SNI подменяется только для `*.googlevideo.com` (иначе DPI рвёт
    // рукопожатие), Host остаётся настоящим. Строгая проверка по границе метки:
    // `evilgooglevideo.com` под подмену не попадает (net::hostMatchesDomain).
    //
    // DPI-17: подмена нужна только когда трафик идёт через локальный SOCKS
    // (фрагментация ClientHello) или напрямую. Если задан внешний прокси,
    // DPI уже обойдён туннелем, а подменённый SNI ломает CDN: googlevideo
    // отдаёт 404, потому что TLS-фронтенд выбирается по SNI. В этом случае
    // шлём настоящее имя.
    const bool viaLocalSocks = (proxy == "socks5h://127.0.0.1:" + std::to_string(socksPort()));
    const bool externalProxy = !proxy.empty() && !viaLocalSocks;
    bool spoofedSni = !externalProxy && net::hostMatchesDomain(upHost.c_str(), "googlevideo.com");

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
    // DPI-19: если клиент просит открытый диапазон, upstream отдаёт 403.
    // Идём кусками по kRelayChunk и склеиваем их в непрерывный 206.
    bool aggregate = false;
    uint64_t chunkStart = 0;
    if (!isHead && rangeOpenEnded(range) && rangeStartOffset(range, chunkStart)) {
        aggregate = true;
        ctx.aggregate = true;
    }

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
        // 60 с тишины означало бы «мёртвую сеть», а не паузу плеера: mpv при
        // полном кеше перестаёт читать, но сокет уже оборвётся по таймауту
        // записи (15 с), поэтому LOW_SPEED ловит именно застой upstream.
        curl_easy_setopt(eh, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(eh, CURLOPT_LOW_SPEED_TIME, 30L);
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
        // DPI-18: точный текст ошибки вместо «Couldn't connect to server».
        curl_easy_setopt(eh, CURLOPT_ERRORBUFFER, ctx.errbuf);
        if (logIt && ctx.debugLines < 40) {
            curl_easy_setopt(eh, CURLOPT_VERBOSE, 1L);
            curl_easy_setopt(eh, CURLOPT_DEBUGFUNCTION, relayDebugCb);
            curl_easy_setopt(eh, CURLOPT_DEBUGDATA, &ctx);
        }

        if (isHead) curl_easy_setopt(eh, CURLOPT_NOBODY, 1L);
        if (!aggregate && !range.empty()) curl_easy_setopt(eh, CURLOPT_RANGE, range.c_str());

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
                platform::bootLog("relay: %s -> SNI подменён на %s "
                                  "(verify=chain-only, имя хоста не сверяется)",
                                  upHost.c_str(), kSpoofNames[cand]);
            }
            // DPI-08: имя в SNI подменено, поэтому сверку имени хоста отключаем,
            // но цепочку сертификата проверяем, насколько возможно.
            if (const char* ca = relayCaPath()) curl_easy_setopt(eh, CURLOPT_CAINFO, ca);
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 1L);
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

        if (aggregate) {
            // DPI-19: закрытые куски по kRelayChunk. Хэндл один, поэтому TLS
            // и соединение с CDN переиспользуются — платим только за первый.
            uint64_t off = chunkStart;
            uint64_t chunk = kRelayChunk;
            std::string chunkUrl = upstream;
            for (int ci = 0; ci < kRelayMaxChunks; ci++) {
                const std::string r = std::to_string(off) + "-" +
                                      std::to_string(off + chunk - 1);
                const bool hadHead = ctx.headSent;
                ctx.chunkBytes = 0;
                ctx.errbuf[0] = 0;
                curl_easy_setopt(eh, CURLOPT_RANGE, r.c_str());
                rc = curl_easy_perform(eh);
                if (rc != CURLE_OK) break;
                // Кусок отклонён (обычно 403 на видео, кэшированном частично).
                // Заголовки 206 клиенту уже отданы, поэтому вместо обрыва
                // пробуем то же место окном меньше.
                if (ctx.code != 200 && ctx.code != 206) {
                    if (hadHead && chunk > kRelayChunkSmall) {
                        chunk = kRelayChunkSmall;
                        continue;
                    }
                    platform::bootLog("relay: кусок отклонён http=%ld off=%llu "
                                      "(дальше CDN не отдаёт)",
                                      (long)ctx.code, (unsigned long long)off);
                    break;
                }
                // 200 при off>0 значит upstream проигнорировал Range: тело
                // пойдёт с начала файла — это дубль, останавливаемся.
                if (ctx.code == 200 && off > 0) {
                    platform::bootLog("relay: кусок off=%llu вернул 200 вместо 206",
                                      (unsigned long long)off);
                    break;
                }
                // CDN отвечает 302 на ближайший фронтенд: берём конечный адрес,
                // чтобы следующие куски не платили за ту же цепочку редиректов.
                if (char* eff = nullptr) {
                    if (curl_easy_getinfo(eh, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff &&
                        *eff) {
                        chunkUrl = eff;
                        curl_easy_setopt(eh, CURLOPT_URL, chunkUrl.c_str());
                    }
                }
                if (logIt && ci > 0 && ci % 10 == 0 && ctx.chunkBytes) {
                    platform::bootLog("relay: кусок %d, off=%llu, %llu байт", ci + 1,
                                      (unsigned long long)off,
                                      (unsigned long long)ctx.chunkBytes);
                }
                off += ctx.chunkBytes;
                // Короткий кусок (или пустой) — это конец файла/ошибка.
                if (ctx.chunkBytes < chunk) break;
                if (ctx.failed || ctx.clientGone || ctx.timedOut) break;
            }
        } else {
            rc = curl_easy_perform(eh);
        }

        if (logIt) {
            const std::string err = rc == CURLE_OK
                                        ? std::string()
                                        : (" (" + std::string(curl_easy_strerror(rc)) + ")");
            // DPI-18: error buffer libcurl даёт конкретную причину (SOCKS5 reply,
            // TLS, таймаут фазы), strerror её не содержит.
            if (rc != CURLE_OK && ctx.errbuf[0]) {
                platform::bootLog("relay: %s -> %ld%s, %llu байт; curl: %s", upHost.c_str(),
                                  ctx.code, err.c_str(),
                                  (unsigned long long)ctx.bodyBytes, ctx.errbuf);
            } else {
                platform::bootLog("relay: %s -> %ld%s, %llu байт", upHost.c_str(), ctx.code,
                                  err.c_str(), (unsigned long long)ctx.bodyBytes);
            }
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
        ctx.aggregate = aggregate;
    }

    // Возвращаем умолчание, чтобы состояние потока было предсказуемым.
    tubeSpoofSetName(nullptr);

    // DPI-10: лог Range/Content-Range/206 — видно, что seek идёт диапазоном.
    if (logIt) {
        platform::bootLog("relay: %s -> %ld range='%s' content-range='%s' bytes=%llu%s",
                          upHost.c_str(), ctx.code, range.c_str(),
                          ctx.contentRange.c_str(),
                          (unsigned long long)ctx.bodyBytes,
                          ctx.rangeIgnored ? " (Range проигнорирован upstream)" : "");
    }

    if (rc != CURLE_OK || !ctx.headSent) {
        const int curlCode = static_cast<int>(rc);
        const int httpCode = curlCodeToHttp(curlCode);
        const char* detail = (rc == CURLE_OK) ? "no response headers"
                                              : curl_easy_strerror(rc);
        {
            std::lock_guard<std::mutex> lk(g_dataMu);
            ++g_stats.errors;
            g_stats.lastError = "curl " + std::to_string(curlCode) + " " + detail;
        }
        if (!ctx.headSent) {
            // DPI-11: не отдаём 200 с пустым телом. Понятный код + тело +
            // заголовок X-TubeNX-Error: curl <code>; для 5xx — Retry-After.
            const int code = httpCode > 0 ? httpCode : 502;
            std::string extra = httpErrorHeaders(curlCode, 1);
            sendResponse(fd, code, "text/plain; charset=utf-8",
                         httpErrorBody(code, curlCode, detail), extra);
        }
        // Если заголовки уже ушли — просто закрываем: клиент увидит обрыв,
        // что для видео корректно (mpv сделает повторный Range-запрос).
    }

    // DPI-06: если режим был подобран автопробой и ошибок накопилось слишком
    // много — подсказываем вызывающему повторить подбор (сам режим здесь не
    // меняем: подбором занимается UI/плеер, и только когда видео не играет).
    if (rc != CURLE_OK && shouldReprobeOnErrors() && logIt) {
        platform::bootLog("autoprobe: ошибок больше порога, стоит повторить подбор режима");
    }

    // DPI-16: скорость последней сессии и число активных сессий.
    {
        const long long dt = nowMs() - sessionStartMs;
        std::lock_guard<std::mutex> lk(g_dataMu);
        if (dt > 0) {
            g_stats.rateKbps =
                static_cast<float>(ctx.bodyBytes * 1000.0 / static_cast<double>(dt) / 1024.0);
        }
        if (g_stats.activeSessions > 0) --g_stats.activeSessions;
    }

    // DPI-03: освобождаем upstream-слот сессии.
    g_relaySessions.release(kind);
    relayLogSessions("release");
}

void* relayClientThread(void* arg) {
    RelayJob* job = static_cast<RelayJob*>(arg);
    int fd = job->client;
    delete job;
    // DPI-02: явные таймауты чтения/записи. Блокирующий send() без таймаута =
    // вечно висящий поток и занятый слот, когда mpv на паузе не читает. 15 с
    // тишины -> EAGAIN -> poll-путь в relayWriteCb; «мёртвый» клиент
    // освобождает слот < 30 с. Большие буферы — чтобы видео не упиралось в
    // размер SO_SNDBUF (DPI-01).
    setSocketTimeouts(fd, 15);
    setSocketBuffers(fd, 256 * 1024);
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

// DPI-05: публичная обёртка внутреннего плана — чтобы host-тест
// (tools/test_dpi.cpp) проверял ровно тот код, что и отправка.
FragmentPlan planFragments(const char* data, size_t len, const Config& cfg) {
    return planFragmentsImpl(data, len, cfg);
}

Config& config() {
    return g_cfg;
}

Stats& stats() {
    return g_stats;
}

Stats statsSnapshot() {
    std::lock_guard<std::mutex> lk(g_dataMu);
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
            // DPI-14: ждём завершения accept-loop не дольше 1 с, затем join.
            waitFlag(g_socks.loopExited, 1000);
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
        // DPI-14: ждём завершения accept-loop не дольше 1 с, затем join.
        waitFlag(g_socks.loopExited, 1000);
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
            // DPI-14: ждём завершения accept-loop не дольше 1 с, затем join.
            waitFlag(g_relay.loopExited, 1000);
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
    int actualPort = port;
    // DPI-13: если 8899 занят — берём случайный свободный порт (в host-сборке
    // эта ветка вырезана целиком, поэтому тестируется bindLocalListener).
    if (!bindLocalListener(port, lfd, actualPort) || !openWakePipe(rfd, wfd)) {
        if (lfd >= 0) close(lfd);
        return false;
    }

    g_relay.listenFd = lfd;
    g_relay.wakeFd = rfd;
    g_relayWakeWrite = wfd;
    g_relay.port.store(actualPort);
    g_relay.loopExited.store(false);
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
        // DPI-14: ждём завершения accept-loop не дольше 1 с, затем join.
        waitFlag(g_relay.loopExited, 1000);
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

// ---------------------------------------------------------------------------
// DPI-06: автовыбор режима фрагментации.
// ---------------------------------------------------------------------------

// Запрос generate_204. На консоли — настоящий GET через тот же SOCKS/обход
// (он и проверяет, проходит ли фрагментация провайдера). На хосте HTTP-клиента
// нет, поэтому проба вырождается в TCP-connect.
static bool probeGenerate204(int& rttMs, long& status, std::string& error) {
#ifndef TNX_HOST
    // DPI-17: если задан внешний прокси, проба идёт через него: туннель и есть
    // обход, а локальный SOCKS в этом режиме не нужен и даёт ложные «нет сети»
    // (в логе: connect к 127.0.0.1:1080 -> connection refused, 9.5 с ожидания).
    const std::string ext = net::proxy();
    std::string proxy;
    if (!ext.empty() && net::externalProxySet()) {
        proxy = ext;
    } else {
        const int sp = g_socks.port.load();
        if (sp <= 0) {
            error = "SOCKS-прокси не запущен";
            rttMs = 0;
            status = 0;
            return false;
        }
        proxy = "socks5h://127.0.0.1:" + std::to_string(sp);
    }
    net::Overrides ov{proxy.c_str(), -1};
    net::Headers h;
    const long long t0 = nowMs();
    net::Response r =
        net::get("http://connectivitycheck.gstatic.com/generate_204", h, 12000, 1024, 3, &ov);
    rttMs = static_cast<int>(nowMs() - t0);
    status = r.status;
    error = r.error;
    return r.status == 204;
#else
    ProbeResult p = probe("connectivitycheck.gstatic.com", 443, true);
    rttMs = p.rttMs;
    status = p.status;
    error = p.error;
    return p.ok;
#endif
}

ModeProbe probeMode(Mode m) {
    ModeProbe r;
    r.mode = m;
    Mode savedMode;
    bool savedEn;
    {
        std::lock_guard<std::mutex> lk(g_dataMu);
        savedMode = g_cfg.mode;
        savedEn = g_cfg.enabled;
        g_cfg.mode = m;
        g_cfg.enabled = (m != Mode::Off);
    }
    r.ok = probeGenerate204(r.rttMs, r.status, r.error);
    {
        std::lock_guard<std::mutex> lk(g_dataMu);
        g_cfg.mode = savedMode;
        g_cfg.enabled = savedEn;
    }
    return r;
}

AutoSelectResult selectWorkingMode(const std::function<bool(Mode)>& probeFn) {
    AutoSelectResult r;
    // Порядок из ТЗ: Off -> Split -> MultiSplit -> TlsRecordSplit. Если и без
    // фрагментации всё работает — лишний обход не нужен.
    const Mode order[] = {Mode::Off, Mode::Split, Mode::MultiSplit, Mode::TlsRecordSplit};
    for (int i = 0; i < 4; ++i) {
        ++r.tried;
        if (probeFn(order[i])) {
            r.found = true;
            r.mode = order[i];
            r.workingIndex = i;
            break;
        }
    }
    return r;
}

AutoSelectResult autoSelectMode() {
    AutoSelectResult r;
    // DPI-06: во время видео режим не меняем — иначе порвём текущий поток.
    if (g_videoPlaying.load()) {
        r.lastError = "video playing";
        return r;
    }
    std::string lastErr;
    r = selectWorkingMode([&](Mode m) {
        ModeProbe p = probeMode(m);
        if (!p.ok) lastErr = p.error;
        return p.ok;
    });
    if (!r.found) {
        r.lastError = lastErr.empty() ? "нет рабочего режима" : lastErr;
        return r;
    }
    {
        std::lock_guard<std::mutex> lk(g_dataMu);
        g_cfg.mode = r.mode;
        g_cfg.enabled = (r.mode != Mode::Off);
        g_cfg.modeAutoPicked = true;
        g_autoBaselineErrors = g_stats.errors; // точка отсчёта для перепроверки
    }
    return r;
}

void setVideoPlaying(bool playing) {
    g_videoPlaying.store(playing);
}

bool videoPlaying() {
    return g_videoPlaying.load();
}

bool shouldReprobeOnErrors() {
    if (g_videoPlaying.load()) return false;
    std::lock_guard<std::mutex> lk(g_dataMu);
    if (!g_cfg.modeAutoPicked) return false;
    const int threshold = clampInt(g_cfg.autoReprobeErrors, 0, 1000000);
    if (threshold <= 0) return false;
    return (g_stats.errors - g_autoBaselineErrors) >= static_cast<uint64_t>(threshold);
}

// ---------------------------------------------------------------------------
// DPI-15: тонкие обёртки для host-теста/fuzz.
// ---------------------------------------------------------------------------

namespace test {

std::string pctEncodeTest(const std::string& s) { return pctEncode(s); }
std::string pctDecodeTest(const std::string& s) { return pctDecode(s); }

bool parseRelayUpstreamTest(const std::string& path, std::string& upstream) {
    return parseRelayUpstream(path, upstream);
}

bool relayUrlTooLongTest(const std::string& url) { return relayUrlTooLong(url); }

bool validByteRangeTest(const std::string& value) { return validByteRange(value); }

int curlCodeToHttpTest(int curlCode) { return curlCodeToHttp(curlCode); }

std::string httpErrorHeadersTest(int curlCode, int retryAfterSec) {
    return httpErrorHeaders(curlCode, retryAfterSec);
}

bool sendResponseTest(int fd, int code, const char* contentType, const std::string& body,
                      const std::string& extraHeaders) {
    return sendResponse(fd, code, contentType, body, extraHeaders);
}

std::string healthzTest(const Stats& s, int video, int audio, int other, int slots) {
    return healthzText(s, video, audio, other, slots);
}

int parseHttpRequestTest(const std::string& raw, std::string& method, std::string& path) {
    HttpRequest rq;
    if (!parseHttpRequest(raw, rq)) return -1;
    method = rq.method;
    path = rq.path;
    return static_cast<int>(rq.headers.size());
}

int readHttpRequestTest(int fd, std::string& method, std::string& path) {
    HttpRequest rq;
    if (!readHttpRequest(fd, rq)) return -1;
    method = rq.method;
    path = rq.path;
    return static_cast<int>(rq.headers.size());
}

int socksHandshakeTest(int client, bool* fragmentedOut) {
    return socksHandshake(client, fragmentedOut);
}

void setSocksUpstreamDisabledTest(bool disabled) { g_testNoUpstream.store(disabled); }

int bindLocalListenerTest(int wantPort, int& actualPort) {
    int fd = -1;
    int p = 0;
    if (!bindLocalListener(wantPort, fd, p)) return -1;
    actualPort = p;
    return fd;
}

void setAutoBaselineErrorsTest(uint64_t errors) {
    std::lock_guard<std::mutex> lk(g_dataMu);
    g_autoBaselineErrors = errors;
}

uint64_t autoBaselineErrorsTest() {
    std::lock_guard<std::mutex> lk(g_dataMu);
    return g_autoBaselineErrors;
}

// Позволяет host-тесту проверить порядок перебора без сети.
int selectWorkingModeTest(bool offOk, bool splitOk, bool multiOk, bool tlsOk) {
    AutoSelectResult r = selectWorkingMode([&](Mode m) {
        switch (m) {
        case Mode::Off: return offOk;
        case Mode::Split: return splitOk;
        case Mode::MultiSplit: return multiOk;
        case Mode::TlsRecordSplit: return tlsOk;
        default: return false;
        }
    });
    return r.found ? r.workingIndex : -1;
}

} // namespace test

} // namespace tnx::dpi