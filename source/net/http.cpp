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
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
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
    int outLines = 0;   // сколько строк исходящего запроса уже залогировано
};
int debugCb(CURL*, curl_infotype type, char* data, size_t size, void* user) {
    // CURLINFO_HEADER_OUT — ровно те строки, что ушли на сервер. Нужно, когда
    // ответ не сходится с ожиданием (например, 404 обложек на консоли при
    // рабочем URL с ПК): в логе видно запрошенный путь, Host и заголовки.
    // Включается только в verbose-режиме (первые запросы за запуск).
    const bool out = (type == CURLINFO_HEADER_OUT);
    if ((!out && type != CURLINFO_TEXT) || !user) return 0;
    DebugCtx* d = static_cast<DebugCtx*>(user);
    std::string line(data, size);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) return 0;
    if (out) {
        if (d->outLines >= 12) return 0;
        ++d->outLines;
        // LOG-06/SEC-03: значения Cookie в лог не попадают.
        static const char kCookie[] = "cookie:";
        bool isCookie = line.size() >= sizeof(kCookie) - 1;
        for (size_t i = 0; isCookie && i < sizeof(kCookie) - 1; ++i) {
            if (std::tolower(static_cast<unsigned char>(line[i])) != kCookie[i])
                isCookie = false;
        }
        if (isCookie) line = "Cookie: <скрыто>";
    }
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - d->t0)
                             .count();
    platform::bootLog("curl +%lld мс: %s%s", ms, out ? "-> " : "", line.c_str());
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
// Поэтому число живых хэндлов теперь ограничивает SessionBudget (NET-09):
// по умолчанию 2 сессии зарезервированы за плеером, остальные 2 — под UI
// (api + images), при этом api имеет приоритет над images.
struct HandlePool {
    explicit HandlePool(SessionClass c) : cls(c) {}
    std::mutex m;
    std::condition_variable cv;
    std::vector<CURL*> idle;
    int live = 0;
    SessionClass cls;
};

HandlePool g_apiPool{SessionClass::Api};
HandlePool g_imgPool{SessionClass::Images};

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
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(p.m);
            if (!p.idle.empty()) {
                CURL* h = p.idle.back();
                p.idle.pop_back();
                return h; // сессия уже учтена в бюджете
            }
        }
        // Нет свободного хэндла — пробуем занять слот SSL-бюджета (NET-09).
        if (SessionBudget::shared().acquire(p.cls)) {
            CURL* h = curl_easy_init();
            if (!h) {
                SessionBudget::shared().release(p.cls);
                return nullptr;
            }
            std::lock_guard<std::mutex> lk(p.m);
            ++p.live;
            return h;
        }
        // Слотов нет: освобождаем простаивающие UI-сессии и ждём.
        closeIdleConnections();
        std::unique_lock<std::mutex> lk(p.m);
        p.cv.wait_for(lk, std::chrono::milliseconds(100));
    }
}

void releaseHandle(HandlePool& p, CURL* h) {
    if (!h) return;
    // Указатели на заголовки/отладку/прогресс живут на стеке вызывающего —
    // сбрасываем, иначе следующий запрос уйдёт с висячим указателем.
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(h, CURLOPT_DEBUGDATA, nullptr);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, nullptr);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, nullptr);
    // NET-11: прогресс/отмена не должны «течь» в переиспользуемый хэндл.
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, nullptr);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, nullptr);
    std::lock_guard<std::mutex> lk(p.m);
    p.idle.push_back(h);
    p.cv.notify_one();
}

// Действующая конфигурация сети — один неизменяемый снапшот (NET-10).
// Все запросы читают configSnapshot(); set*/reconfigure() под g_mu заменяют
// указатель целиком. Гонок на полях нет: объект под shared_ptr<const> после
// публикации не меняется, а начатые запросы дорабатывают на своей копии.
std::shared_ptr<const NetConfig> g_config; // под g_mu
bool g_inited = false;
std::atomic<bool> g_dead{false};   // shutdown уже начался

// NET-14: любой отход от Full должен быть виден в логе, один раз на смену
// режима. «Тихого» перехода в None/ChainOnly быть не должно.
std::atomic<int> g_verifyLogged{-1};
void noteVerifyDowngrade(Verify v) {
    if (v == Verify::Full) return;
    const int want = static_cast<int>(v);
    if (g_verifyLogged.exchange(want) == want) return;
    if (v == Verify::ChainOnly) {
        platform::bootLog("tls: проверка понижена до ChainOnly (имя хоста не сверяется) "
                          "— применено по явному запросу");
    } else {
        platform::bootLog("tls: ВНИМАНИЕ, проверка сертификата ОТКЛЮЧЕНА (None) — "
                          "только по явному запросу вызывающего");
    }
}

// Человекочитаемая версия HTTP (NET-01/NET-17). В релизе используется только
// диагностикой, поэтому помечена как возможно неиспользуемая.
[[maybe_unused]] const char* httpVerName(long v) {
    switch (v) {
        case CURL_HTTP_VERSION_1_0: return "1.0";
        case CURL_HTTP_VERSION_1_1: return "1.1";
        case CURL_HTTP_VERSION_2_0: return "2";
        case CURL_HTTP_VERSION_2TLS: return "2TLS";
#ifdef CURL_HTTP_VERSION_3
        case CURL_HTTP_VERSION_3: return "3";
#endif
        default: return "?";
    }
}

#ifdef TNX_DIAG

// Собрать строку отправленных заголовков (без cookie/authorization).
std::string sentHeadersToLog(const Headers& sent) {
    std::string out;
    for (const auto& h : redactHeaders(sent)) {
        if (!out.empty()) out += "; ";
        out += buildHeaderLine(h.first, h.second);
    }
    return out;
}

// Счётчики, чтобы DIAG-лог не превращался в новую лавину строк.
std::atomic<int> g_diagReqLogged{0};
std::atomic<int> g_diagBodyLogged{0};
#endif

// NET-06: суффикс «+N в очереди» формируется formatQueueSuffix() ниже
// (публичная функция, тестируемая на host). Раньше строка с литералом "%lld"
// уходила в bootLog как %s, и в логе появлялось «+%lld в очереди».

// Общий кэш DNS и переиспользуемых соединений между запросами. Без него каждый
// curl_easy_init() начинает с нуля: на Switch это лишние 2-9 секунд на resolve
// и новое TLS-рукопожатие на каждый запрос (обложки, лента, API).
CURLSH* g_share = nullptr;
std::mutex g_shareMu;

// --- NET-17: HTTP-версия и переиспользование соединений --------------------
// Версия выбирается один раз при init(): HTTP/2 over TLS, если libcurl собран
// с nghttp2, иначе честный HTTP/1.1. Явный CURLOPT_HTTP_VERSION нужен, потому
// что хэндл живёт между запросами и не должен наследовать чужой выбор.
long g_httpVersion = CURL_HTTP_VERSION_1_1;
bool g_http2 = false;

// Сводка по 10 запросам: сколько реально было новых рукопожатий, а сколько
// запросов ушло в уже открытое keep-alive-соединение, и какая версия HTTP
// согласовалась. В релизе это единственная строка про соединения — по ней
// видно, работает ли переиспользование (иначе каждое TLS = 2-9 с на консоли).
struct HttpConnAgg {
    std::mutex m;
    int req = 0;
    int h10 = 0, h11 = 0, h2 = 0, h3 = 0;
    int newConn = 0, reused = 0;
    long long tlsUs = 0;
    int tlsN = 0;
};
HttpConnAgg g_connAgg;

void noteHttpStats(long ver, long numConnects, long long tlsUs) {
    std::string line;
    {
        std::lock_guard<std::mutex> lk(g_connAgg.m);
        ++g_connAgg.req;
        switch (ver) {
            case CURL_HTTP_VERSION_1_0: ++g_connAgg.h10; break;
            case CURL_HTTP_VERSION_2_0:
            case CURL_HTTP_VERSION_2TLS: ++g_connAgg.h2; break;
#ifdef CURL_HTTP_VERSION_3
            case CURL_HTTP_VERSION_3: ++g_connAgg.h3; break;
#endif
            default: ++g_connAgg.h11; break;
        }
        if (numConnects > 0) {
            ++g_connAgg.newConn;
            if (tlsUs > 0) {
                g_connAgg.tlsUs += tlsUs;
                ++g_connAgg.tlsN;
            }
        } else if (numConnects == 0) {
            ++g_connAgg.reused;
        }
        if (g_connAgg.req < 10) return;

        char b[320];
        long long avgTls = g_connAgg.tlsN > 0 ? g_connAgg.tlsUs / g_connAgg.tlsN / 1000 : 0;
        std::snprintf(b, sizeof(b),
                      "http: 10 запросов: h1.0=%d h1.1=%d h2=%d h3=%d, новых соединений %d "
                      "(tls в среднем %lld мс), переиспользовано %d",
                      g_connAgg.h10, g_connAgg.h11, g_connAgg.h2, g_connAgg.h3,
                      g_connAgg.newConn, avgTls, g_connAgg.reused);
        line = b;
        // Окно закрыто — счётчики с нуля (мьютекс не копируется).
        g_connAgg.req = 0;
        g_connAgg.h10 = g_connAgg.h11 = g_connAgg.h2 = g_connAgg.h3 = 0;
        g_connAgg.newConn = g_connAgg.reused = g_connAgg.tlsN = 0;
        g_connAgg.tlsUs = 0;
    }
    // Лог — вне мьютекса: bootLog может ждать диск.
    if (!line.empty()) platform::bootLog("%s", line.c_str());
}

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

// Эффективные параметры запроса: неизменяемый снапшот конфига (NET-10) плюс
// общий кэш соединений. Держим shared_ptr: пока запрос выполняется, его
// снапшот жив, даже если параллельный reconfigure() заменит g_config.
struct RequestConfig {
    std::shared_ptr<const NetConfig> cfg;
    CURL* share = nullptr;
    std::string proxy;            // с учётом Overrides
    Verify verify = Verify::Full;
};

RequestConfig requestConfig(const Overrides* ov) {
    RequestConfig rc;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        rc.cfg = g_config;
        rc.share = g_share;
    }
    if (!rc.cfg) rc.cfg = std::make_shared<const NetConfig>(); // страховка
    rc.proxy = rc.cfg->proxy;
    rc.verify = rc.cfg->verify;
    // Пробы не должны менять глобальные настройки под работающими потоками.
    if (ov) {
        if (ov->proxy) rc.proxy = ov->proxy;
        if (ov->verify >= 0) rc.verify = static_cast<Verify>(ov->verify);
    }
    return rc;
}

// Общие опции для любого запроса. userAgent — единственный источник UA на
// запрос (NET-07): либо из заголовка вызывающего, либо глобальный.
void applyCommon(CURL* eh, const RequestConfig& rc, const std::string& userAgent) {
    curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(eh, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(eh, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(eh, CURLOPT_ACCEPT_ENCODING, ""); // libcurl сам распакует
    curl_easy_setopt(eh, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(eh, CURLOPT_TCP_NODELAY, 1L);
    // NET-17: версия HTTP задаётся на каждом запросе — хэндл переиспользуется.
    curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, g_httpVersion);

    // Прокси: socks5h:// — DNS на стороне прокси (важно для обхода DPI).
    //
    // Ставим ВСЕГДА, в том числе пустой: хэндл переиспользуется, и раньше
    // после переключения пробы «прокси -> напрямую» в нём оставался старый
    // CURLOPT_PROXY. Следующий запрос шёл на уже закрытый 127.0.0.1:1080 и
    // падал с curl 7 «Couldn't connect to server» за 90 мс.
    curl_easy_setopt(eh, CURLOPT_PROXY, rc.proxy.empty() ? nullptr : rc.proxy.c_str());
    if (rc.proxy.rfind("socks5h://", 0) == 0) {
        curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
    } else if (rc.proxy.rfind("socks5://", 0) == 0) {
        curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
    } else if (rc.proxy.rfind("socks4a://", 0) == 0) {
        curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4A);
    } else if (rc.proxy.rfind("socks4://", 0) == 0) {
        curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
    } else if (rc.proxy.rfind("http://", 0) == 0 ||
               rc.proxy.rfind("https://", 0) == 0) {
        curl_easy_setopt(eh, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
    }

    // CA-бандл: ставим только если файл реально существует. Если нет —
    // сбрасываем путь, иначе переиспользованный хэндл останется с прошлым.
    const std::string& ca = rc.cfg->caBundle;
    if (!ca.empty() && access(ca.c_str(), R_OK) == 0) {
        curl_easy_setopt(eh, CURLOPT_CAINFO, ca.c_str());
    } else {
        curl_easy_setopt(eh, CURLOPT_CAINFO, nullptr);
    }
    switch (rc.verify) {
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
    // NET-14: отход от Full — не молчаливый, каждый режим логируем один раз.
    noteVerifyDowngrade(rc.verify);
    // NET-01: заголовки запроса не должны «течь» с прошлого использования
    // этого easy-хэндла. releaseHandle() их и так обнуляет; страхуемся здесь,
    // т.к. performOnce ставит свой список уже после applyCommon().
    curl_easy_setopt(eh, CURLOPT_HTTPHEADER, nullptr);
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

    std::pair<std::string, std::string> kv;
    if (parseHeaderLine(line, kv)) h->emplace_back(std::move(kv.first), std::move(kv.second));
    return len;
}

// Контекст отмены для CURLOPT_XFERINFOFUNCTION (NET-11). curl зовёт его
// регулярно; возврат != 0 прерывает передачу (CURLE_ABORTED_BY_CALLBACK).
struct CancelCtx {
    CancelToken* token = nullptr;
};

int xferInfoCb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    CancelCtx* c = static_cast<CancelCtx*>(userdata);
    if (c && c->token && c->token->cancelled()) return 1; // прервать передачу
    return 0;
}

// Единая реализация запроса. body == nullptr — GET, иначе POST.
Response performOnce(const char* method, const std::string& url, const std::string* body,
                     const Headers& headers, int timeoutMs, size_t maxBytes, CURLcode* outRc,
                     bool fresh, const Overrides* ov, CancelToken* cancel) {
    if (outRc) *outRc = CURLE_OK;
    Response r;
    if (url.empty()) {
        r.error = "empty url";
        return r;
    }
    // NET-11: отменённый до старта запрос не занимает слот вовсе.
    if (cancel && cancel->cancelled()) {
        r.error = "cancelled";
        return r;
    }

    init();
    if (g_dead.load()) {
        r.error = "net shutting down";
        return r;
    }
    RequestConfig cfg = requestConfig(ov);

    // NET-07: один User-Agent на запрос. Источник ровно один —
    // CURLOPT_USERAGENT. Если вызывающий задал свой UA, берём его; иначе
    // глобальный. В slist User-Agent НЕ добавляем (иначе два источника).
    const std::string* callerUa = nullptr;
    for (const auto& h : headers) {
        if (lowerAscii(h.first) == "user-agent") {
            callerUa = &h.second;
            break;
        }
    }
    const std::string effectiveUa = callerUa ? *callerUa : cfg.cfg->userAgent;

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
    // NET-13: жёсткий лимит. Даже если вызывающий передал больше, потолок
    // класса URL (browse — 8 МБ и т.п.) уменьшит значение.
    const size_t effMax = effectiveMaxBytes(url, maxBytes);
    wc.maxBytes = effMax;

    curl_easy_setopt(eh, CURLOPT_URL, url.c_str());
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(eh, CURLOPT_WRITEDATA, &wc);
    curl_easy_setopt(eh, CURLOPT_HEADERFUNCTION, headerToVector);
    curl_easy_setopt(eh, CURLOPT_HEADERDATA, &r.headers);

    int t = timeoutMs > 0 ? timeoutMs : timeouts().api;
    // NET-03/NET-12: бюджеты запросов по классам (bootstrap/api/images).
    // Бюджет работает как верхняя граница переданного таймаута: при
    // превышении запрос отменяется, а не висит 30 с.
    const int budget = budgetMsFor(url);
    if (budget > 0 && t > budget) {
#ifdef TNX_DIAG
        platform::bootLog("diag http: бюджет %d мс вместо %d мс для %s", budget, t,
                          redactUrl(url).c_str());
#endif
        t = budget;
    }
    curl_easy_setopt(eh, CURLOPT_TIMEOUT_MS, static_cast<long>(t));
    // Первое рукопожатие на консоли идёт 5–12 с. Соединению отдаём не больше
    // 3/4 общего бюджета, иначе на тело ответа не остаётся времени.
    int connectMs = std::min(t * 3 / 4, defaults::kConnectCapMs);
    if (connectMs < defaults::kConnectMinMs) connectMs = defaults::kConnectMinMs;
    curl_easy_setopt(eh, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(connectMs));
    // 120 с: один устаревший A-рекорд не должен жить в общем кэше все 10 минут.
    curl_easy_setopt(eh, CURLOPT_DNS_CACHE_TIMEOUT, (long)defaults::kDnsCacheSec);
    // IPv6 в сети нет, а попытка AAAA на консоли стоит секунд ожидания.
    curl_easy_setopt(eh, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(eh, CURLOPT_TCP_KEEPALIVE, 1L);
    // Переиспользованное соединение может быть уже закрыто сервером: не тратим
    // на него попытку, если это повтор, и не держим слишком старые.
    curl_easy_setopt(eh, CURLOPT_FRESH_CONNECT, fresh ? 1L : 0L);
    curl_easy_setopt(eh, CURLOPT_MAXAGE_CONN, (long)defaults::kMaxAgeConnSec);
    curl_easy_setopt(eh, CURLOPT_SHARE, cfg.share);
    if (verbose) {
        curl_easy_setopt(eh, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(eh, CURLOPT_DEBUGFUNCTION, debugCb);
        curl_easy_setopt(eh, CURLOPT_DEBUGDATA, &dbg);
    } else {
        // Хэндл мог остаться в verbose от предыдущего запроса.
        curl_easy_setopt(eh, CURLOPT_VERBOSE, 0L);
    }

    applyCommon(eh, cfg, effectiveUa);

    // NET-11: отмена устаревших запросов через прогресс-колбэк. player токен
    // не получает (cancel == nullptr) — его отмена идёт через player::close().
    CancelCtx cc{cancel};
    if (cancel) {
        curl_easy_setopt(eh, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(eh, CURLOPT_XFERINFOFUNCTION, xferInfoCb);
        curl_easy_setopt(eh, CURLOPT_XFERINFODATA, &cc);
    } else {
        curl_easy_setopt(eh, CURLOPT_NOPROGRESS, 1L);
    }

    // Заголовки запроса. NET-07: User-Agent сюда НЕ добавляем — он задан
    // единственным источником через CURLOPT_USERAGENT (см. effectiveUa).
    curl_slist* list = nullptr;
    for (const auto& h : headers) {
        if (lowerAscii(h.first) == "user-agent") continue;
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
        // NET-18: порядок важен! В libcurl установка CURLOPT_POSTFIELDS — даже
        // в nullptr — переключает метод на POST (так в 7.69: `data->set.method
        // = HTTPREQ_POST`). Хэндл переиспользуется между запросами, поэтому
        // после ленты (POST) все GET уходили как `POST /vi/...jpg` с
        // `Content-Length: 0`: обложки получали 404, `www.youtube.com/` — 400,
        // а googlevideo на POST отдавал 404, из-за чего видео не играло.
        // Сначала сбрасываем тело, и только последним — CURLOPT_HTTPGET.
        curl_easy_setopt(eh, CURLOPT_POSTFIELDS, nullptr);
        curl_easy_setopt(eh, CURLOPT_POSTFIELDSIZE, 0L);
        curl_easy_setopt(eh, CURLOPT_HTTPGET, 1L);
    }

    if (list) curl_easy_setopt(eh, CURLOPT_HTTPHEADER, list);

    CURLcode rc = curl_easy_perform(eh);
    if (outRc) *outRc = rc;

    // NET-17: версия, новые соединения и время рукопожатия — в сводку по 10.
    {
        long httpVer = 0;
        curl_easy_getinfo(eh, CURLINFO_HTTP_VERSION, &httpVer);
        long nconn = -1;
        curl_easy_getinfo(eh, CURLINFO_NUM_CONNECTS, &nconn);
        curl_off_t tls = 0;
        curl_easy_getinfo(eh, CURLINFO_APPCONNECT_TIME_T, &tls);
        noteHttpStats(httpVer, nconn, (long long)tls);
    }

    long status = 0;
    curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &status);
    r.status = status;
    const size_t gotBytes = r.body.size();

    if (rc != CURLE_OK) {
        if (wc.overflow) {
            r.error = "response too large";
        } else if (rc == CURLE_ABORTED_BY_CALLBACK) {
            // NET-11: передача прервана вызывающим через CancelToken.
            r.error = "cancelled";
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

    // NET-13: размер ответа (без тела). При превышении лимита — всегда, иначе
    // в диагностике. Тело в лог не попадает ни при каком коде.
    {
        std::string u = redactUrl(url);   // LOG-06: без query-секретов
        if (u.size() > 110) u = u.substr(0, 110) + "…";
        if (wc.overflow) {
            platform::bootLog("http: ответ превысил лимит %zu байт (%zu получено) — "
                              "прервано: %s", effMax, gotBytes, u.c_str());
        }
#ifdef TNX_DIAG
        else {
            platform::bootLog("diag http: размер ответа %zu байт, лимит %zu: %s", gotBytes,
                              effMax, u.c_str());
        }
#endif
    }

#ifdef TNX_DIAG
    // NET-01: подробный разбор запроса/ответа. Только диагностическая сборка.
    {
        long httpVer = 0;
        curl_easy_getinfo(eh, CURLINFO_HTTP_VERSION, &httpVer);
        long nconn = -1;
        curl_easy_getinfo(eh, CURLINFO_NUM_CONNECTS, &nconn);
        curl_off_t tDns = 0, tConn = 0, tTls = 0, tFirst = 0, tTotal = 0;
        curl_easy_getinfo(eh, CURLINFO_NAMELOOKUP_TIME_T, &tDns);
        curl_easy_getinfo(eh, CURLINFO_CONNECT_TIME_T, &tConn);
        curl_easy_getinfo(eh, CURLINFO_APPCONNECT_TIME_T, &tTls);
        curl_easy_getinfo(eh, CURLINFO_STARTTRANSFER_TIME_T, &tFirst);
        curl_easy_getinfo(eh, CURLINFO_TOTAL_TIME_T, &tTotal);
        auto us2ms = [](curl_off_t us) { return (long long)(us / 1000); };
        std::string u = redactUrl(url);
        if (u.size() > 200) u = u.substr(0, 200) + "…";
        // reused=1 и fresh=0 означают keep-alive: заголовки/Connection
        // переиспользуются, что важно для гипотезы H2 (NET-01).
        platform::bootLog("diag http: %s %s HTTP/%s reused=%d fresh=%d status=%ld err='%s'",
                          method, u.c_str(), httpVerName(httpVer), nconn == 0 ? 1 : 0,
                          fresh ? 1 : 0, r.status, r.error.c_str());
        // NET-03: фазы с миллисекундами. SOCKS-этапы при прокси входят в
        // connect (curl не разделяет их), это отмечено в подписи.
        platform::bootLog("diag http: фазы dns=%lld connect+socks=%lld tls=%lld first=%lld "
                          "body=%lld total=%lld мс (proxy='%s')",
                          us2ms(tDns), us2ms(tConn),
                          us2ms(tTls > tConn ? tTls - tConn : 0),
                          us2ms(tFirst > tTls ? tFirst - tTls : 0),
                          us2ms(tTotal > tFirst ? tTotal - tFirst : 0), us2ms(tTotal),
                          redactProxy(cfg.proxy).c_str());
        if (g_diagReqLogged.fetch_add(1) < 60) {
            // NET-01: отправленные заголовки (cookie/authorization вырезаны).
            // NET-07: User-Agent ровно один — из CURLOPT_USERAGENT (effectiveUa).
            Headers sent;
            for (const auto& h : headers) {
                if (lowerAscii(h.first) == "user-agent") continue;
                sent.push_back(h);
            }
            sent.emplace_back("User-Agent", effectiveUa);
            if (callerUa) {
                platform::bootLog("diag http: User-Agent задан вызывающим");
            }
            if (body && !hasHeader(headers, "Content-Type")) {
                sent.emplace_back("Content-Type", "application/json");
            }
            if (body) sent.emplace_back("Content-Length", std::to_string(body->size()));
            sent.emplace_back("Accept-Encoding", "(libcurl: gzip, deflate)");
            platform::bootLog("diag http: заголовки запроса: %s", sentHeadersToLog(sent).c_str());
        }
        if (r.status >= 400 && g_diagBodyLogged.fetch_add(1) < 40) {
            for (const auto& h : redactHeaders(r.headers)) {
                platform::bootLog("diag http: hdr ответа %s: %s", h.first.c_str(), h.second.c_str());
            }
            platform::bootLog("diag http: тело ответа (%zu байт, первые 512): %s", r.body.size(),
                              escapeForLog(r.body, 512).c_str());
        }
    }
#endif

    if (list) curl_slist_free_all(list);
    releaseHandle(pool, eh);

    // Диагностика: какие именно URL падают и с каким кодом curl. Полный URL
    // бывает длинным, в лог пишем начало. Время и объём важны: по ним видно,
    // это «медленно» или «вообще не соединилось».
    {
        std::string u = redactUrl(url);   // LOG-06: без query-секретов
        if (u.size() > 110) u = u.substr(0, 110) + "…";
        const auto ended = std::chrono::steady_clock::now();
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                 ended - started).count();
        const long long qms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                  started - queued).count();
        // NET-06: суффикс очереди формируем в qbuf, а не передаём строку с
        // литералом "%lld" как %s (иначе в логе оставался «+%lld в очереди»).
        char qbuf[48];
        formatQueueSuffix(qbuf, sizeof(qbuf), qms);
        if (!r.error.empty()) {
            platform::bootLog("http: %s %s -> %s (%lld мс%s, %zu байт, прокси='%s')", method,
                              u.c_str(), r.error.c_str(), ms, qbuf, gotBytes,
                              redactProxy(cfg.proxy).c_str());
        } else {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 40) {
                platform::bootLog("http: %s %s -> %ld (%lld мс%s, %zu байт)", method, u.c_str(),
                                  r.status, ms, qbuf, gotBytes);
            }
        }
    }
    (void)method;
    return r;
}

// NET-08: повторы. Отказы на этой сети «плавающие»: тот же запрос через 300 мс
// проходит. Решение о повторе принимает чистая decideRetry() (в публичной
// части, протестирована на host); здесь — применение решения и лог.
bool idempotentMethod(const char* m) {
    return std::strcmp(m, "GET") == 0 || std::strcmp(m, "HEAD") == 0 ||
           std::strcmp(m, "OPTIONS") == 0 || std::strcmp(m, "TRACE") == 0;
}

NetFail classifyFail(CURLcode rc) {
    switch (rc) {
        case CURLE_OK: return NetFail::None;
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_SSL_CONNECT_ERROR:
            return NetFail::Connect;
        case CURLE_OPERATION_TIMEDOUT:
            return NetFail::Timeout;
        case CURLE_GOT_NOTHING:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_PARTIAL_FILE:
            return NetFail::Io;
        case CURLE_ABORTED_BY_CALLBACK:
            return NetFail::Aborted;
        default:
            return NetFail::Other;
    }
}

// ±20% к паузе между попытками. Источник случайности — thread_local, чтобы
// параллельные запросы не толкались за общим генератором.
double rand01() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng);
}

Response perform(const char* method, const std::string& url, const std::string* body,
                 const Headers& headers, int timeoutMs, size_t maxBytes, int attempts,
                 const Overrides* ov, CancelToken* cancel) {
    Response r;
    if (attempts < 1) attempts = 1;
    const bool idem = idempotentMethod(method);
    const bool hasBody = body != nullptr;
    CURLcode rc = CURLE_OK;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        // NET-11: отменённый запрос не начинаем и не повторяем.
        if (cancel && cancel->cancelled()) {
            r = Response();
            r.error = "cancelled";
            return r;
        }
        // На повторе — новое соединение: старое могло быть закрыто сервером
        // ровно в этот момент (тогда FRESH_CONNECT дешевле, чем ещё один RST).
        r = performOnce(method, url, body, headers, timeoutMs, maxBytes, &rc,
                        attempt > 0, ov, cancel);
        if (r.error == "cancelled") return r; // отмена не повторяется

        RetryOptions opt;
        opt.idempotent = idem;
        opt.hasBody = hasBody;
        opt.responseStarted = r.status != 0;
        RetryDecision d = decideRetry(classifyFail(rc), r.status, attempt, attempts, opt);
        if (!d.retry) break;

        int delay = d.baseDelayMs;
        if (d.honorRetryAfter) {
            const long ra = retryAfterMs(r.header("Retry-After"));
            if (ra > 0) delay = static_cast<int>(ra);
        }
        delay = jitteredDelayMs(delay, rand01());
        std::string u = redactUrl(url);
        if (u.size() > 80) u = u.substr(0, 80) + "…";
        platform::bootLog("http: повтор attempt=%d reason=%s задержка=%d мс: %s %s",
                          attempt + 2, d.reason, delay, method, u.c_str());
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
            // NET-09: закрытая keep-alive связка возвращает слот бюджета.
            SessionBudget::shared().release(p->cls);
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
    // NET-10: до первого запроса есть валидный снапшот.
    if (!g_config) g_config = std::make_shared<const NetConfig>();
#ifdef TNX_DIAG
    // NET-12: в диагностике бюджеты времени можно переопределить env-переменными
    // TNX_T_BOOTSTRAP / TNX_T_API / TNX_T_IMAGES / TNX_T_PROBE / TNX_T_PLAYER.
    {
        struct { const char* env; const char* name; } map[] = {
            {"TNX_T_BOOTSTRAP", "bootstrap"}, {"TNX_T_API", "api"},
            {"TNX_T_IMAGES", "images"},       {"TNX_T_PROBE", "probe"},
            {"TNX_T_PLAYER", "player"}};
        for (const auto& m : map) {
            const char* v = std::getenv(m.env);
            if (v && *v) setTimeout(m.name, std::atoi(v));
        }
    }
#endif
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        return false;
    }
    // NET-17: HTTP/2 только если libcurl собран с nghttp2. На Switch портлиба
    // обычно без него — тогда 1.1, и это видно в логе одной строкой.
    {
        const curl_version_info_data* vi = curl_version_info(CURLVERSION_NOW);
        g_http2 = vi && (vi->features & CURL_VERSION_HTTP2);
        g_httpVersion = g_http2 ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1;
        platform::bootLog("http: curl %s, HTTP/2 %s%s%s", vi ? vi->version : "?",
                          g_http2 ? "да" : "нет",
                          (vi && vi->nghttp2_version && *vi->nghttp2_version) ? ", nghttp2 " : "",
                          (vi && vi->nghttp2_version && *vi->nghttp2_version)
                              ? vi->nghttp2_version
                              : "");
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
    // Сбрасываем учёт SSL-бюджета: после shutdown/init счётчики должны быть нулевыми.
    SessionBudget::shared().reset();
    if (g_share) {
        curl_share_cleanup(g_share);
        g_share = nullptr;
    }
    curl_global_cleanup();
    g_inited = false;
    g_dead.store(false);
}

// NET-10: единая точка сборки нового снапшота. Под g_mu копируем текущий
// (или создаём), меняем одно поле и публикуем новый shared_ptr<const>. Это и
// есть «reconfigure() с барьером»: уже выполняющиеся запросы держат прежний
// снапшот (он неизменяем), новые видят новый.
namespace {
void publishMutation(const std::function<void(NetConfig&)>& mutate) {
    std::lock_guard<std::mutex> lk(g_mu);
    NetConfig next = g_config ? *g_config : NetConfig{};
    mutate(next);
    g_config = std::make_shared<const NetConfig>(std::move(next));
}
} // namespace

const char* defaultUserAgent() {
    return "Mozilla/5.0 (Nintendo Switch; TubeNX) AppleWebKit/537.36 "
           "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
}

std::shared_ptr<const NetConfig> configSnapshot() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_config) g_config = std::make_shared<const NetConfig>();
    return g_config;
}

bool reconfigure(const NetConfig& cfg) {
    if (g_dead.load()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    g_config = std::make_shared<const NetConfig>(cfg);
    noteVerifyDowngrade(cfg.verify);
    return true;
}

void setProxy(const std::string& url) {
    publishMutation([&](NetConfig& c) { c.proxy = url; });
}

std::string proxy() {
    return configSnapshot()->proxy;
}

bool externalProxySet() {
    const std::string p = configSnapshot()->proxy;
    if (p.empty()) return false;
    // "socks5h://127.0.0.1:1080" — наш локальный SOCKS (фрагментация ClientHello),
    // он DPI не обходит, подмена SNI для него остаётся нужной.
    const size_t scheme = p.find("://");
    const size_t hostStart = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t hostEnd = p.find_first_of(":/@", hostStart);
    std::string host = p.substr(hostStart, hostEnd == std::string::npos
                                              ? std::string::npos
                                              : hostEnd - hostStart);
    for (char& ch : host) ch = (char)std::tolower((unsigned char)ch);
    const bool loopback = host == "127.0.0.1" || host == "localhost" ||
                          host == "::1" || host == "[::1]";
    return !loopback;
}

void setCaBundlePath(const std::string& path) {
    publishMutation([&](NetConfig& c) { c.caBundle = path; });
}

void setUserAgent(const std::string& ua) {
    publishMutation([&](NetConfig& c) { c.userAgent = ua; });
}

void setVerify(Verify v) {
    publishMutation([&](NetConfig& c) { c.verify = v; });
    // NET-14: смена политики проверки — всегда видимая строка, не молча.
    noteVerifyDowngrade(v);
}

Verify verify() {
    return configSnapshot()->verify;
}

const char* verifyName(Verify v) {
    switch (v) {
        case Verify::Full: return "Full";
        case Verify::ChainOnly: return "ChainOnly";
        case Verify::None: return "None";
    }
    return "?";
}

// --- SessionBudget (NET-09) -------------------------------------------------

SessionBudget& SessionBudget::shared() {
    static SessionBudget inst;
    return inst;
}

bool SessionBudget::acquire(SessionClass c) {
    std::lock_guard<std::mutex> lk(mu_);
    const int used = player_ + api_ + images_;
    if (used >= kSslSessions) return false;
    if (c == SessionClass::Player) {
        ++player_;
        return true;
    }
    // UI (api/images) не занимает слоты, зарезервированные под плеер.
    const int uiCap = kSslSessions - playerReserve_;
    const int uiUsed = api_ + images_;
    if (uiUsed >= uiCap) return false;
    if (c == SessionClass::Api) {
        ++api_;
        return true;
    }
    // images уступает api: всегда оставляем api хотя бы один свободный слот.
    constexpr int kApiReserve = 1;
    if (uiUsed + 1 > uiCap - kApiReserve) return false;
    ++images_;
    return true;
}

void SessionBudget::release(SessionClass c) {
    std::lock_guard<std::mutex> lk(mu_);
    int* slot = c == SessionClass::Player ? &player_
                : c == SessionClass::Api  ? &api_
                                          : &images_;
    if (*slot > 0) --*slot;
}

void SessionBudget::reserveForPlayer(int sessions) {
    if (sessions < 0) sessions = 0;
    if (sessions > kSslSessions) sessions = kSslSessions;
    std::lock_guard<std::mutex> lk(mu_);
    playerReserve_ = sessions;
}

int SessionBudget::playerReserve() const {
    std::lock_guard<std::mutex> lk(mu_);
    return playerReserve_;
}

int SessionBudget::used() const {
    std::lock_guard<std::mutex> lk(mu_);
    return player_ + api_ + images_;
}

int SessionBudget::freeSlots() const {
    return kSslSessions - used();
}

SessionBudget::Stats SessionBudget::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    Stats s;
    s.total = kSslSessions;
    s.player = player_;
    s.api = api_;
    s.images = images_;
    s.used = player_ + api_ + images_;
    return s;
}

void SessionBudget::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    player_ = api_ = images_ = 0;
}

// --- Диагностика и редакция (NET-01/LOG-06/SEC-03) --------------------------

Headers redactHeaders(const Headers& h) {
    Headers out;
    out.reserve(h.size());
    for (const auto& kv : h) {
        const std::string name = lowerAscii(kv.first);
        if (name == "cookie" || name == "set-cookie" || name == "authorization" ||
            name == "proxy-authorization") {
            out.emplace_back(kv.first, "<скрыто>");
            continue;
        }
        out.push_back(kv);
    }
    return out;
}

std::string escapeForLog(const std::string& s, size_t maxBytes) {
    std::string out;
    const size_t n = std::min(s.size(), maxBytes);
    out.reserve(n + 16);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\\': out += "\\\\"; break;
            default:
                if (c >= 0x20 && c < 0x7f) {
                    out.push_back(static_cast<char>(c));
                } else {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\x%02x", c);
                    out += b;
                }
        }
    }
    if (s.size() > maxBytes) out += "…";
    return out;
}

// LOG-06: в лог попадают только схема, хост, путь и ИМЕНА query-параметров.
// Зачем: у googlevideo ссылка на поток содержит sig/n/pot и живёт минуты — по
// логу её всё равно не воспроизвести, зато в нём утекали бы токены. Имена
// оставляем: по ним видно, какие параметры запрошены (range, pot, key и т.п.).
std::string redactUrl(const std::string& url) {
    if (url.empty()) return url;
    // Отрезаем фрагмент: он в запрос не уходит.
    std::string u = url;
    const size_t hash = u.find('#');
    if (hash != std::string::npos) u.erase(hash);

    const size_t q = u.find('?');
    if (q == std::string::npos) return u;
    const std::string base = u.substr(0, q);
    const std::string query = u.substr(q + 1);

    std::string names;
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        if (amp > pos) {
            const size_t eq = query.find('=', pos);
            const size_t end = (eq != std::string::npos && eq < amp) ? eq : amp;
            if (!names.empty()) names += ',';
            names.append(query, pos, end - pos);
        }
        if (amp == query.size()) break;
        pos = amp + 1;
    }
    if (names.empty()) return base;
    return base + "?" + names;
}

bool parseHeaderLine(const std::string& line, std::pair<std::string, std::string>& out) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    std::string name = trim(line.substr(0, colon));
    if (name.empty()) return false;
    out.first = std::move(name);
    out.second = trim(line.substr(colon + 1));
    return true;
}

std::string buildHeaderLine(const std::string& name, const std::string& value) {
    return name + ": " + value;
}

bool hasHeader(const Headers& h, const char* name) {
    const std::string want = lowerAscii(name);
    for (const auto& it : h) {
        if (lowerAscii(it.first) == want) return true;
    }
    return false;
}

void formatQueueSuffix(char* buf, size_t size, long long qms) {
    if (!buf || size == 0) return;
    buf[0] = '\0';
    if (qms < 50) return;
    std::snprintf(buf, size, " +%lld в очереди", qms);
}

// Бюджеты запросов (NET-03/NET-12). Классификация по URL совпадает с выбором
// пула и с лимитами ответа ниже.
namespace {
bool isImageUrl(const std::string& url) {
    return url.find("ytimg.com") != std::string::npos ||
           url.find("ggpht.com") != std::string::npos ||
           url.find("googleusercontent.com") != std::string::npos;
}
bool isBootstrapUrl(const std::string& url) {
    std::string bare = url;
    const size_t q = bare.find('?');
    if (q != std::string::npos) bare = bare.substr(0, q);
    return bare.rfind("https://www.youtube.com/", 0) == 0 ||
           bare == "https://www.youtube.com";
}
} // namespace

int budgetMsFor(const std::string& url) {
    if (isImageUrl(url)) return timeouts().images;

    // InnerTube API (browse/search/next/player) проверяем ДО bootstrap: у них
    // тот же хост www.youtube.com.
    if (url.find("/youtubei/v1/") != std::string::npos) return timeouts().api;

    // Bootstrap: GET https://www.youtube.com/ и родственные страницы (без query).
    if (isBootstrapUrl(url)) return timeouts().bootstrap;
    return 0;
}

// NET-13: жёсткие лимиты размера тела по классам URL. Имеют приоритет над
// значением вызывающего — именно здесь фиксируется browse ≤ 8 МБ (даже если
// innertube.cpp передал больше или оставил значение по умолчанию).
size_t maxBytesForUrl(const std::string& url) {
    if (isImageUrl(url)) return limits::kImages;
    if (url.find("/youtubei/v1/") != std::string::npos) {
        if (url.find("/youtubei/v1/player") != std::string::npos) return limits::kPlayer;
        return limits::kApi; // browse/search/next
    }
    if (isBootstrapUrl(url)) return limits::kBootstrap;
    return 0; // класс не задан
}

size_t effectiveMaxBytes(const std::string& url, size_t requested) {
    const size_t cap = maxBytesForUrl(url);
    if (requested == 0) return cap > 0 ? cap : limits::kDefault;
    if (cap == 0) return requested;
    return requested < cap ? requested : cap;
}

std::string Response::header(const std::string& name) const {
    std::string want = lowerAscii(name);
    for (const auto& h : headers) {
        if (lowerAscii(h.first) == want) return h.second;
    }
    return std::string();
}

Response get(const std::string& url, const Headers& h, int timeoutMs, size_t maxBytes,
             int attempts, const Overrides* ov, CancelToken* cancel) {
    return perform("GET", url, nullptr, h, timeoutMs, maxBytes, attempts, ov, cancel);
}

Response post(const std::string& url, const std::string& body, const Headers& h,
              int timeoutMs, size_t maxBytes, int attempts, const Overrides* ov,
              CancelToken* cancel) {
    return perform("POST", url, &body, h, timeoutMs, maxBytes, attempts, ov, cancel);
}

// --- Таймауты (NET-12) ------------------------------------------------------

bool setTimeout(const char* name, int ms) {
    if (!name || ms <= 0) return false;
    NetTimeouts& t = mutableTimeouts();
    const std::string n = lowerAscii(name);
    if (n == "bootstrap") t.bootstrap = ms;
    else if (n == "api") t.api = ms;
    else if (n == "images") t.images = ms;
    else if (n == "probe") t.probe = ms;
    else if (n == "player") t.player = ms;
    else return false;
    return true;
}

// --- Матрица повторов (NET-08) ----------------------------------------------

RetryDecision decideRetry(NetFail fail, long status, int attempt, int maxAttempts,
                          const RetryOptions& opt) {
    RetryDecision d;
    if (maxAttempts < 1) maxAttempts = 1;
    // Попытки исчерпаны — дальше не повторяем.
    if (attempt + 1 >= maxAttempts) return d;

    // 400 / 800 / 1600 мс (с потолком 3 с): не бьём попытки в одно окно.
    auto netDelay = [attempt]() { return std::min(400 * (1 << attempt), 3000); };

    switch (fail) {
        case NetFail::Aborted:
            return d; // отмена вызывающим не повторяется
        case NetFail::Connect:
            // Тело запроса точно не ушло — повтор безопасен для любого метода.
            d.retry = true;
            d.reason = "network_connect";
            d.baseDelayMs = netDelay();
            return d;
        case NetFail::Timeout:
        case NetFail::Io:
            // Идемпотентные — всегда. POST — только если ответа ещё не было
            // (иначе можно продублировать уже отправленное тело).
            d.baseDelayMs = netDelay();
            if (opt.idempotent) {
                d.retry = true;
                d.reason = (fail == NetFail::Timeout) ? "network_timeout" : "network_io";
            } else if (opt.hasBody && !opt.responseStarted) {
                d.retry = true;
                d.reason = (fail == NetFail::Timeout) ? "network_timeout" : "network_io";
            }
            return d;
        case NetFail::Other:
            d.baseDelayMs = netDelay();
            if (opt.idempotent) {
                d.retry = true;
                d.reason = "network_other";
            }
            return d;
        case NetFail::None:
            break;
    }

    // Есть HTTP-ответ — смотрим на код.
    if (status >= 500 && status <= 599) {
        // 5xx — ровно один повтор, и только для идемпотентных/read-only.
        // POST InnerTube повторяем лишь при сетевой ошибке (см. выше).
        if (opt.idempotent && attempt == 0) {
            d.retry = true;
            d.reason = "http_5xx";
            d.baseDelayMs = netDelay();
        }
        return d;
    }
    if (status == 429 || status == 408) {
        d.retry = true;
        d.reason = (status == 429) ? "http_429" : "http_408";
        d.baseDelayMs = netDelay();
        d.honorRetryAfter = true; // уважаем Retry-After, но ≤ 5 с
        return d;
    }
    return d; // прочие 4xx/3xx/2xx — не повторяем
}

int jitteredDelayMs(int baseMs, double r01) {
    if (baseMs <= 0) return 0;
    if (r01 < 0.0) r01 = 0.0;
    if (r01 >= 1.0) r01 = 0.999999;
    const double factor = 0.8 + 0.4 * r01; // ±20%
    const int v = static_cast<int>(baseMs * factor + 0.5);
    return v < 0 ? 0 : v;
}

long retryAfterMs(const std::string& value) {
    // Только целые секунды; HTTP-даты игнорируем (не наш сценарий).
    const std::string s = trim(value);
    if (s.empty()) return 0;
    long sec = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0; // не число
        sec = sec * 10 + (c - '0');
        if (sec > 1000000) { sec = 1000000; break; }
    }
    long ms = sec * 1000;
    return ms > 5000 ? 5000 : ms; // не дольше 5 с
}

// --- SNI-политика (DPI-07/DPI-08) -------------------------------------------
// Один источник истины для net/sni.cpp и host-теста (sni.cpp включает
// <switch.h> и на хосте не собирается). Строгий suffix-match по границе метки.

bool hostMatchesDomain(const char* host, const char* suffix) {
    if (!host || !suffix || !*host || !*suffix) return false;
    const size_t n = std::strlen(host), m = std::strlen(suffix);
    if (n < m) return false;
    // host == suffix
    if (n == m) {
        for (size_t i = 0; i < m; ++i) {
            unsigned char a = static_cast<unsigned char>(host[i]);
            if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
            if (a != static_cast<unsigned char>(suffix[i])) return false;
        }
        return true;
    }
    // host заканчивается на "." + suffix
    if (host[n - m - 1] != '.') return false;
    const char* tail = host + (n - m);
    for (size_t i = 0; i < m; ++i) {
        unsigned char a = static_cast<unsigned char>(tail[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
        if (a != static_cast<unsigned char>(suffix[i])) return false;
    }
    return true;
}

// Хосты, для которых режем SNI-блокировку; остальные не трогаем (DPI-08).
const char* const kSniUpperSuffixes[] = {
    "youtube.com", "youtube-nocookie.com", "youtu.be", "ytimg.com",
    "googlevideo.com", "ggpht.com", "googleapis.com", "gvt1.com", "gvt2.com",
};
// Подмена SNI для видео: у *.googlevideo.com регистр не помогает, зато серверы
// Google обслуживают URL по заголовку Host.
const char* const kSniSpoofSuffixes[] = {"googlevideo.com"};
// Fallback-имена подмены: первое — основное (DPI-08).
const char* const kSniSpoofNames[] = {"www.google.com"};

const char* sniSpoofName(const char* host) {
    if (!host || !*host) return nullptr;
    for (const char* suf : kSniSpoofSuffixes) {
        if (hostMatchesDomain(host, suf)) return kSniSpoofNames[0];
    }
    return nullptr;
}

bool sniNeedsUpper(const char* host) {
    if (!host || !*host) return false;
    for (const char* suf : kSniUpperSuffixes) {
        if (hostMatchesDomain(host, suf)) return true;
    }
    return false;
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
    RequestConfig cfg = requestConfig(nullptr);

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
    // скорости: меньше 1 байта/с в течение 60 секунд считаем обрывом (NET-12).
    curl_easy_setopt(eh, CURLOPT_CONNECTTIMEOUT, (long)defaults::kFileConnectMs);
    curl_easy_setopt(eh, CURLOPT_LOW_SPEED_LIMIT, (long)defaults::kFileLowSpeedBytes);
    curl_easy_setopt(eh, CURLOPT_LOW_SPEED_TIME, (long)defaults::kFileLowSpeedSec);
    if (progress) {
        curl_easy_setopt(eh, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(eh, CURLOPT_XFERINFOFUNCTION, progressCb);
        curl_easy_setopt(eh, CURLOPT_XFERINFODATA, &fc);
    }

    applyCommon(eh, cfg, cfg.cfg->userAgent);

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