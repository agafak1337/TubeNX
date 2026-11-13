// net/http.cpp — тонкая обёртка над libcurl easy API для TubeNX.
//
// Особенности:
//   * каждый запрос создаёт свой CURL* (curl_easy_init) — так безопасно
//     звать get/post из разных потоков (обложки, InnerTube, плеер);
//   * глобальные настройки (прокси, CA-бандл, User-Agent) лежат под мьютексом;
//   * curl_global_init вызывается один раз в init();
//   * CURLOPT_ACCEPT_ENCODING "" — libcurl сам попросит gzip/deflate и сам
//     распакует ответ, нам не нужно возиться с Content-Encoding;
//   * CURLOPT_NOSIGNAL обязателен: libcurl не должен трогать SIGALRM в
//     многопоточном homebrew-процессе на Switch.

#include "net/http.hpp"

#include "platform.hpp"

#include <sys/select.h> // fd_set: требуется заголовками curl/multi.h
#include <unistd.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace tnx::net {
namespace {

std::mutex g_mu;
// Диагностика: для первых запросов включаем «verbose» libcurl и пишем его
// текстовые сообщения в лог. Это единственный способ увидеть, на каком шаге
// (прокси-туннель, TLS, HTTP) запрос завис на консоли.
std::atomic<int> g_verboseLeft{14};
struct DebugCtx {
    std::chrono::steady_clock::time_point t0;
};
int debugCb(CURL*, curl_infotype type, char* data, size_t size, void* user) {
    if (type != CURLINFO_TEXT || !user) return 0;
    DebugCtx* d = static_cast<DebugCtx*>(user);
    std::string line(data, size);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) return 0;
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - d->t0)
                             .count();
    platform::bootLog("curl +%lld мс: %s", ms, line.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Пул easy-хэндлов: keep-alive между запросами.
//
// Рукопожатие через сервис ssl консоли занимает 5–9 секунд, а новый коннект
// на каждый запрос означал, что лента и обложки «грузятся вечно». libcurl
// хранит соединение внутри easy-хэндла, поэтому хэндлы не уничтожаем, а
// переиспользуем: первое обращение к хосту платит за TLS, остальные идут по
// уже открытому соединению (десятки миллисекунд).
// ВАЖНО: сервис ssl на Horizon OS даёт максимум 4 сессии (sslInitialize 1..4),
// и каждая ЖИВАЯ keep-alive связка держит одну сессию. Семи хэндлам сессий
// физически не хватало: лишние рукопожатия ждали по 8–15 с, а curl в это
// время молчал после SOCKS CONNECT — отсюда «клиент молчит» и таймауты.
// Поэтому UI-пул = 2 соединения (API + обложки), а остальные две сессии
// остаются ретранслятору плеера (видео + звук).
constexpr int kApiHandles = 1;   // InnerTube, страницы, поиск
constexpr int kImgHandles = 1;   // обложки (последовательно, но по keep-alive)

struct HandlePool {
    explicit HandlePool(int maxHandles) : maxLive(maxHandles) {}
    std::mutex m;
    std::condition_variable cv;
    std::vector<CURL*> idle;
    int live = 0;
    int maxLive = 0;
};

HandlePool g_apiPool{kApiHandles};
HandlePool g_imgPool{kImgHandles};

// Обложки не должны выедать слоты API. С одним пулом три воркера картинок
// занимали половину из шести хэндлов, и /player ждал чужие таймауты по 30 с.
HandlePool& poolFor(const std::string& url) {
    if (url.find("ytimg.com") != std::string::npos ||
        url.find("ggpht.com") != std::string::npos ||
        url.find("googleusercontent.com") != std::string::npos) {
        return g_imgPool;
    }
    return g_apiPool;
}

CURL* acquireHandle(HandlePool& p) {
    std::unique_lock<std::mutex> lk(p.m);
    for (;;) {
        if (!p.idle.empty()) {
            CURL* h = p.idle.back();
            p.idle.pop_back();
            return h;
        }
        if (p.live < p.maxLive) {
            ++p.live;
            lk.unlock();
            CURL* h = curl_easy_init();
            if (!h) {
                lk.lock();
                --p.live;
                p.cv.notify_one();
            }
            return h;
        }
        p.cv.wait_for(lk, std::chrono::milliseconds(200));
    }
}

void releaseHandle(HandlePool& p, CURL* h) {
    if (!h) return;
    // Указатели на заголовки/отладку живут на стеке вызывающего — сбрасываем,
    // иначе следующий запрос уйдёт с висячим указателем.
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(h, CURLOPT_DEBUGDATA, nullptr);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, nullptr);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, nullptr);
    std::lock_guard<std::mutex> lk(p.m);
    p.idle.push_back(h);
    p.cv.notify_one();
}

std::string g_proxy;                      // "" — ходим напрямую
// Короткий бандл (13 корней вместо 148): на консоли каждый корневой
// сертификат импортируется в сервис ssl, и полный набор заметно тормозит
// рукопожатие. Полный лежит рядом как запасной — см. App::init().
std::string g_caBundle = "romfs:/cacert-trim.pem";
std::string g_userAgent =
    "Mozilla/5.0 (Nintendo Switch; TubeNX) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
Verify g_verify = Verify::Full;
bool g_inited = false;
std::atomic<bool> g_dead{false};   // shutdown уже начался

// Общий кэш DNS и переиспользуемых соединений между запросами. Без него каждый
// curl_easy_init() начинает с нуля: на Switch это лишние 2-9 секунд на resolve
// и новое TLS-рукопожатие на каждый запрос (обложки, лента, API).
CURLSH* g_share = nullptr;
std::mutex g_shareMu;

void shareLock(CURL*, curl_lock_data, curl_lock_access, void*) { g_shareMu.lock(); }
void shareUnlock(CURL*, curl_lock_data, void*) { g_shareMu.unlock(); }

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string lowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Копия настроек под мьютексом: дальше работаем со снимком, не держа лок
// во время сетевого запроса.
struct Snapshot {
    std::string proxy;
    std::string ca;
    std::string ua;
    Verify verify = Verify::Full;
    CURL* share = nullptr;
};

Snapshot snapshot() {
    std::lock_guard<std::mutex> lk(g_mu);
    return Snapshot{g_proxy, g_caBundle, g_userAgent, g_verify, g_share};
}

// Общие опции для любого запроса.
void applyCommon(CURL* eh, const Snapshot& s) {
    curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(eh, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(eh, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(eh, CURLOPT_ACCEPT_ENCODING, ""); // libcurl сам распакует
    curl_easy_setopt(eh, CURLOPT_USERAGENT, s.ua.c_str());
    curl_easy_setopt(eh, CURLOPT_TCP_NODELAY, 1L);

    // Прокси: socks5h:// — DNS на стороне прокси (важно для обхода DPI).
    //
    // Ставим ВСЕГДА, в том числе пустой: хэндл переиспользуется, и раньше
    // после переключения пробы «прокси -> напрямую» в нём оставался старый
    // CURLOPT_PROXY. Следующий запрос шёл на уже закрытый 127.0.0.1:1080 и
    // падал с curl 7 «Couldn't connect to server» за 90 мс.
    curl_easy_setopt(eh, CURLOPT_PROXY, s.proxy.empty() ? nullptr : s.proxy.c_str());
    if (s.proxy.rfind("socks5h://", 0) == 0) {
        curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
    } else if (s.proxy.rfind("socks5://", 0) == 0) {
        curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
    } else if (s.proxy.rfind("socks4a://", 0) == 0) {
        curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4A);
    } else if (s.proxy.rfind("socks4://", 0) == 0) {
        curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
    } else if (s.proxy.rfind("http://", 0) == 0 ||
               s.proxy.rfind("https://", 0) == 0) {
        curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
    }

    // CA-бандл: ставим только если файл реально существует. Если нет —
    // сбрасываем путь, иначе переиспользованный хэндл останется с прошлым.
    if (!s.ca.empty() && access(s.ca.c_str(), R_OK) == 0) {
        curl_easy_setopt(eh, CURLOPT_CAINFO, s.ca.c_str());
    } else {
        curl_easy_setopt(eh, CURLOPT_CAINFO, nullptr);
    }
    switch (s.verify) {
        case Verify::Full:
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, 2L);
            break;
        case Verify::ChainOnly:
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, 0L);
            break;
        case Verify::None:
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, 0L);
            break;
    }
}

struct WriteCtx {
    std::string* out = nullptr;
    size_t maxBytes = 0;
    bool overflow = false;
};

size_t writeToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    WriteCtx* c = static_cast<WriteCtx*>(userdata);
    size_t len = size * nmemb;
    if (c->out->size() + len > c->maxBytes) {
        c->overflow = true;
        return 0; // прерываем передачу: ответ больше лимита
    }
    c->out->append(ptr, len);
    return len;
}

size_t headerToVector(char* buffer, size_t size, size_t nitems, void* userdata) {
    Headers* h = static_cast<Headers*>(userdata);
    size_t len = size * nitems;
    std::string line(buffer, len);

    // Новая строка статуса (в т.ч. после редиректа) — начинаем список заново,
    // чтобы вызывающий видел заголовки финального ответа.
    if (line.size() >= 5 && line.compare(0, 5, "HTTP/") == 0) {
        h->clear();
        return len;
    }
    if (line == "\r\n" || line == "\n") return len;

    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        h->emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    return len;
}

bool hasHeader(const Headers& h, const char* name) {
    std::string want = lowerAscii(name);
    for (const auto& it : h) {
        if (lowerAscii(it.first) == want) return true;
    }
    return false;
}

// Единая реализация запроса. body == nullptr — GET, иначе POST.
Response performOnce(const char* method, const std::string& url, const std::string* body,
                     const Headers& headers, int timeoutMs, size_t maxBytes, CURLcode* outRc,
                     bool fresh, const Overrides* ov) {
    if (outRc) *outRc = CURLE_OK;
    Response r;
    if (url.empty()) {
        r.error = "empty url";
        return r;
    }

    init();
    if (g_dead.load()) {
        r.error = "net shutting down";
        return r;
    }
    Snapshot s = snapshot();
    // Пробы не должны менять глобальные настройки под работающими потоками.
    if (ov) {
        if (ov->proxy) s.proxy = ov->proxy;
        if (ov->verify >= 0) s.verify = static_cast<Verify>(ov->verify);
    }

    // Отдельно ждём слот пула: это время не должно попадать в «медленный
    // ответ» — иначе в логе 17 с у обложки, из которых 16 с она стояла в очереди.
    const auto queued = std::chrono::steady_clock::now();
    HandlePool& pool = poolFor(url);
    CURL* eh = acquireHandle(pool);
    if (!eh) {
        r.error = "curl_easy_init failed";
        return r;
    }
    const auto started = std::chrono::steady_clock::now();
    DebugCtx dbg{started};
    const bool verbose = g_verboseLeft.fetch_sub(1) > 0;

    WriteCtx wc;
    wc.out = &r.body;
    wc.maxBytes = maxBytes;

    curl_easy_setopt(eh, CURLOPT_URL, url.c_str());
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(eh, CURLOPT_WRITEDATA, &wc);
    curl_easy_setopt(eh, CURLOPT_HEADERFUNCTION, headerToVector);
    curl_easy_setopt(eh, CURLOPT_HEADERDATA, &r.headers);

    int t = timeoutMs > 0 ? timeoutMs : 30000;
    curl_easy_setopt(eh, CURLOPT_TIMEOUT_MS, static_cast<long>(t));
    // Первое рукопожатие на консоли идёт 5–12 с. Соединению отдаём не больше
    // 3/4 общего бюджета, иначе на тело ответа не остаётся времени.
    int connectMs = std::min(t * 3 / 4, 25000);
    if (connectMs < 3000) connectMs = 3000;
    curl_easy_setopt(eh, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(connectMs));
    // 120 с: один устаревший A-рекорд не должен жить в общем кэше все 10 минут.
    curl_easy_setopt(eh, CURLOPT_DNS_CACHE_TIMEOUT, 120L);
    // IPv6 в сети нет, а попытка AAAA на консоли стоит секунд ожидания.
    curl_easy_setopt(eh, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(eh, CURLOPT_TCP_KEEPALIVE, 1L);
    // Переиспользованное соединение может быть уже закрыто сервером: не тратим
    // на него попытку, если это повтор, и не держим слишком старые.
    curl_easy_setopt(eh, CURLOPT_FRESH_CONNECT, fresh ? 1L : 0L);
    curl_easy_setopt(eh, CURLOPT_MAXAGE_CONN, 120L);
    curl_easy_setopt(eh, CURLOPT_SHARE, s.share);
    if (verbose) {
        curl_easy_setopt(eh, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(eh, CURLOPT_DEBUGFUNCTION, debugCb);
        curl_easy_setopt(eh, CURLOPT_DEBUGDATA, &dbg);
    } else {
        // Хэндл мог остаться в verbose от предыдущего запроса.
        curl_easy_setopt(eh, CURLOPT_VERBOSE, 0L);
    }

    applyCommon(eh, s);

    // Заголовки запроса + User-Agent по умолчанию, если не задан явно.
    curl_slist* list = nullptr;
    if (!hasHeader(headers, "User-Agent")) {
        list = curl_slist_append(list, ("User-Agent: " + s.ua).c_str());
    }
    for (const auto& h : headers) {
        std::string line = h.first + ": " + h.second;
        list = curl_slist_append(list, line.c_str());
    }

    if (body) {
        if (!hasHeader(headers, "Content-Type")) {
            // InnerTube всегда шлёт JSON; если вызывающий не задал тип — считаем
            // JSON разумным значением по умолчанию.
            list = curl_slist_append(list, "Content-Type: application/json");
        }
        curl_easy_setopt(eh, CURLOPT_POST, 1L);
        curl_easy_setopt(eh, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(eh, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body->size()));
    } else {
        curl_easy_setopt(eh, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(eh, CURLOPT_POSTFIELDS, nullptr);
        curl_easy_setopt(eh, CURLOPT_POSTFIELDSIZE, 0L);
    }

    if (list) curl_easy_setopt(eh, CURLOPT_HTTPHEADER, list);

    CURLcode rc = curl_easy_perform(eh);
    if (outRc) *outRc = rc;

    long status = 0;
    curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &status);
    r.status = status;
    const size_t gotBytes = r.body.size();

    if (rc != CURLE_OK) {
        if (wc.overflow) {
            r.error = "response too large";
        } else {
            // Код curl в тексте: без него «Timeout was reached» не отличить от
            // обрыва TLS и от ошибки сертификата.
            char buf[160];
            snprintf(buf, sizeof(buf), "curl %d: %s", (int)rc, curl_easy_strerror(rc));
            r.error = buf;
        }
        r.body.clear();
    } else if (wc.overflow) {
        r.error = "response too large";
        r.body.clear();
    }

    if (list) curl_slist_free_all(list);
    releaseHandle(pool, eh);

    // Диагностика: какие именно URL падают и с каким кодом curl. Полный URL
    // бывает длинным, в лог пишем начало. Время и объём важны: по ним видно,
    // это «медленно» или «вообще не соединилось».
    {
        std::string u = url.size() > 110 ? url.substr(0, 110) + "…" : url;
        const auto ended = std::chrono::steady_clock::now();
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                 ended - started).count();
        const long long qms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                  started - queued).count();
        const char* q = qms >= 50 ? " +%lld в очереди" : "";
        if (!r.error.empty()) {
            platform::bootLog("http: %s %s -> %s (%lld мс%s, %zu байт, прокси='%s')", method,
                              u.c_str(), r.error.c_str(), ms, q, gotBytes, s.proxy.c_str());
        } else {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 40) {
                platform::bootLog("http: %s %s -> %ld (%lld мс%s, %zu байт)", method, u.c_str(),
                                  r.status, ms, q, gotBytes);
            }
        }
    }
    (void)method;
    return r;
}

// Повторы. На этой сети отказы бывают «плавающими»: тот же запрос через
// 300 мс проходит. Один таймаут раньше срывал загрузку ленты целиком.
bool idempotentMethod(const char* m) {
    return std::strcmp(m, "GET") == 0 || std::strcmp(m, "HEAD") == 0 ||
           std::strcmp(m, "OPTIONS") == 0 || std::strcmp(m, "TRACE") == 0;
}

Response perform(const char* method, const std::string& url, const std::string* body,
                 const Headers& headers, int timeoutMs, size_t maxBytes, int attempts,
                 const Overrides* ov) {
    Response r;
    CURLcode rc = CURLE_OK;
    if (attempts < 1) attempts = 1;
    const bool idem = idempotentMethod(method);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        // На повторе — новое соединение: старое могло быть закрыто сервером
        // ровно в этот момент (тогда FRESH_CONNECT дешевле, чем ещё один RST).
        r = performOnce(method, url, body, headers, timeoutMs, maxBytes, &rc,
                        attempt > 0, ov);
        // CONNECT/RESOLVE/SSL: тело запроса точно не ушло — повтор безопасен
        // для любого метода. TIMEOUT/RECV/SEND/PARTIAL: тело могло уйти,
        // поэтому повторяем только идемпотентные запросы.
        const bool beforeBody = rc == CURLE_COULDNT_CONNECT ||
                                rc == CURLE_COULDNT_RESOLVE_HOST ||
                                rc == CURLE_COULDNT_RESOLVE_PROXY ||
                                rc == CURLE_SSL_CONNECT_ERROR;
        const bool transient = beforeBody ||
                               (idem && (rc == CURLE_OPERATION_TIMEDOUT ||
                                         rc == CURLE_GOT_NOTHING ||
                                         rc == CURLE_RECV_ERROR ||
                                         rc == CURLE_SEND_ERROR ||
                                         rc == CURLE_PARTIAL_FILE));
        if (!transient || attempt + 1 >= attempts) break;
        std::string u = url.size() > 80 ? url.substr(0, 80) + "…" : url;
        platform::bootLog("http: повтор %d: %s %s", attempt + 1, method, u.c_str());
        // 400 / 800 / 1600 мс: не бьём три попытки в одно и то же окно.
        const int delay = std::min(400 * (1 << attempt), 3000);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    return r;
}

} // namespace

void closeIdleConnections() {
    int closed = 0;
    for (HandlePool* p : {&g_apiPool, &g_imgPool}) {
        std::lock_guard<std::mutex> lk(p->m);
        for (CURL* h : p->idle) {
            curl_easy_cleanup(h);
            ++closed;
        }
        p->live -= static_cast<int>(p->idle.size());
        if (p->live < 0) p->live = 0;
        p->idle.clear();
    }
    if (closed > 0) {
        platform::bootLog("net: освободил %d SSL-сессий для плеера", closed);
    }
}

bool init() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_dead.load()) return false;
    if (g_inited) return true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        return false;
    }
    g_share = curl_share_init();
    if (g_share) {
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, shareLock);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, shareUnlock);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        // CURL_LOCK_DATA_CONNECT в 7.69 экспериментальный: общий кэш соединений
        // может заставлять запросы ждать друг друга. Держим только DNS.
    }
    g_inited = true;
    return true;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_inited) return;
    // Новые запросы выходят сразу. Сначала дожидаемся возврата всех хэндлов,
    // потом рвём их, и только затем share: cleanup share под живым хэндлом
    // оставляет в нём висячий data->share.
    g_dead.store(true);
    for (HandlePool* p : {&g_apiPool, &g_imgPool}) {
        std::unique_lock<std::mutex> plk(p->m);
        p->cv.wait_for(plk, std::chrono::seconds(3),
                       [&] { return p->idle.size() == (size_t)p->live; });
        for (CURL* h : p->idle) curl_easy_cleanup(h);
        p->idle.clear();
        p->live = 0;
    }
    if (g_share) {
        curl_share_cleanup(g_share);
        g_share = nullptr;
    }
    curl_global_cleanup();
    g_inited = false;
    g_dead.store(false);
}

void setProxy(const std::string& url) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_proxy = url;
}

std::string proxy() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_proxy;
}

void setCaBundlePath(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_caBundle = path;
}

void setUserAgent(const std::string& ua) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_userAgent = ua;
}

void setVerify(Verify v) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_verify = v;
}

Verify verify() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_verify;
}

std::string Response::header(const std::string& name) const {
    std::string want = lowerAscii(name);
    for (const auto& h : headers) {
        if (lowerAscii(h.first) == want) return h.second;
    }
    return std::string();
}

Response get(const std::string& url, const Headers& h, int timeoutMs, size_t maxBytes,
             int attempts, const Overrides* ov) {
    return perform("GET", url, nullptr, h, timeoutMs, maxBytes, attempts, ov);
}

Response post(const std::string& url, const std::string& body, const Headers& h,
              int timeoutMs, size_t maxBytes, int attempts, const Overrides* ov) {
    return perform("POST", url, &body, h, timeoutMs, maxBytes, attempts, ov);
}

namespace {

struct FileCtx {
    FILE* fp = nullptr;
    const std::function<void(long long, long long)>* progress = nullptr;
    bool writeError = false;
};

size_t writeToFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    FileCtx* c = static_cast<FileCtx*>(userdata);
    size_t len = size * nmemb;
    if (len == 0) return 0;
    size_t written = fwrite(ptr, 1, len, c->fp);
    if (written != len) {
        c->writeError = true;
        return 0;
    }
    return len;
}

int progressCb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t,
               curl_off_t) {
    FileCtx* c = static_cast<FileCtx*>(userdata);
    if (c->progress && *c->progress) {
        (*c->progress)(static_cast<long long>(dlnow),
                       static_cast<long long>(dltotal));
    }
    return 0;
}

} // namespace

bool fetchToFile(const std::string& url, const std::string& path,
                 const std::function<void(long long, long long)>& progress) {
    if (url.empty() || path.empty()) return false;

    init();
    Snapshot s = snapshot();

    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    CURL* eh = curl_easy_init();
    if (!eh) {
        std::fclose(fp);
        std::remove(path.c_str());
        return false;
    }

    FileCtx fc;
    fc.fp = fp;
    fc.progress = &progress;

    curl_easy_setopt(eh, CURLOPT_URL, url.c_str());
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(eh, CURLOPT_WRITEDATA, &fc);
    // Для кэша обложек/видео — без общего таймаута, но с контролем «мёртвой»
    // скорости: меньше 1 байта/с в течение 60 секунд считаем обрывом.
    curl_easy_setopt(eh, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(eh, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(eh, CURLOPT_LOW_SPEED_TIME, 60L);
    if (progress) {
        curl_easy_setopt(eh, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(eh, CURLOPT_XFERINFOFUNCTION, progressCb);
        curl_easy_setopt(eh, CURLOPT_XFERINFODATA, &fc);
    }

    applyCommon(eh, s);

    CURLcode rc = curl_easy_perform(eh);

    long status = 0;
    curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &status);

    curl_easy_cleanup(eh);
    bool flushOk = (std::fclose(fp) == 0);

    bool ok = (rc == CURLE_OK) && !fc.writeError && flushOk && status > 0 &&
              status < 400;
    if (!ok) {
        std::remove(path.c_str()); // не оставляем битый файл в кэше
        return false;
    }
    return true;
}

std::string urlEncode(const std::string& s) {
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

std::string urlDecode(const std::string& s) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

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
        // RFC 3986: '+' не является пробелом, оставляем как есть.
        out.push_back(s[i]);
    }
    return out;
}

} // namespace tnx::net