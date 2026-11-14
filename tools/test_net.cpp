// tools/test_net.cpp — host-тесты чистой логики сетевого слоя (M1 «Сеть»).
//
// Проверяются только функции, которым не нужна ни консоль, ни реальная сеть:
//   * сборка/разбор заголовков (NET-01);
//   * редакция секретов и экранирование тела для лога (NET-01/LOG-06/SEC-03);
//   * fallback-цепочка качеств обложки (NET-02);
//   * бюджет запросов (NET-03);
//   * менеджер SSL-сессий (NET-09);
//   * qbuf без литерала «%lld» (NET-06);
//   * urlEncode/urlDecode.
//
// M2 «Сеть» (NET-07/08/10/11/12/13, DPI-07/08):
//   * один User-Agent (NET-07) — проверяется косвенно через политику;
//   * матрица повторов decideRetry + jitter + Retry-After (NET-08);
//   * неизменяемый NetConfig-снапшот (NET-10);
//   * CancelToken (NET-11);
//   * именованные таймауты (NET-12);
//   * лимиты ответа maxBytes (NET-13);
//   * строгий suffix-match и правила SNI (DPI-07/DPI-08);
//   * loopback mock-сервер: 500 -> 2 попытки, 400 -> 1, timeout -> 3.
//
// M3 «Рендер» (PERF-03):
//   * кэш обложек: лимит памяти и LRU-вытеснение без сети.
//
// Сборка (см. отчёт): заголовки libcurl берутся из hostdev-сисрута, линкуем
// системные libcurl.so.4/libjansson.so.4 (одной командой):
//   g++ -std=c++17 -O2 -Isource -Itools
//   -I/tmp/opencode/hostdev/usr/include -I/tmp/opencode/hostdev/usr/include/x86_64-linux-gnu
//   tools/test_net.cpp source/net/http.cpp source/net/images.cpp -o /tmp/test_net
//   -lpthread -l:libcurl.so.4 -l:libjansson.so.4
#include "core/threads.hpp"
#include "net/http.hpp"
#include "net/images.hpp"
#include "platform.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace tnx;

// --- Заглушки платформы (host не линкует platform.cpp) ----------------------

namespace tnx::platform {
void setLogLevel(LogLevel) {}
LogLevel logLevel() { return LogLevel::Info; }
bool debugEnabled() { return false; }
void logWrite(LogLevel, const char*, ...) {}
void logError(const char*, ...) {}
void logWarn(const char*, ...) {}
void logInfo(const char*, ...) {}
void logDebug(const char*, ...) {}
void bootLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}
bool keyboard(std::string&, const char*, const char*, bool) { return false; }
void requestExit() {}
Info info() { return Info{}; }
} // namespace tnx::platform

// --- Заглушки потоков (host не линкует core/threads.cpp, NET-02 не стартует
//     воркеры в тесте) ---------------------------------------------------------

namespace tnx::threads {
void spawn(std::function<void()>, size_t) {}
Thread::~Thread() {
    if (task_) { /* detach не нужен: потоки в тесте не запускаются */ }
}
Thread::Thread(Thread&& o) noexcept : task_(o.task_), handle_(o.handle_) {
    o.task_ = nullptr;
    o.handle_ = 0;
}
Thread& Thread::operator=(Thread&& o) noexcept {
    if (this != &o) {
        task_ = o.task_;
        handle_ = o.handle_;
        o.task_ = nullptr;
        o.handle_ = 0;
    }
    return *this;
}
bool Thread::start(std::function<void()>, size_t) { return false; }
void Thread::join() {}
} // namespace tnx::threads

// --- Мини-фреймворк ---------------------------------------------------------

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::fprintf(stderr, "  FAIL: %s (строка %d)\n", (what), __LINE__);  \
        }                                                                        \
    } while (0)

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

static int countSubstr(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

static void testUrlCodec() {
    using net::urlDecode;
    using net::urlEncode;
    const std::string raw = "a b&c=d/?#+\xff";
    CHECK(urlDecode(urlEncode(raw)) == raw, "urlEncode/urlDecode round-trip");
    CHECK(urlEncode(" ") == "%20", "пробел -> %20");
    CHECK(urlDecode("%2F") == "/", "decode %2F");
    CHECK(urlDecode("a+b") == "a+b", "RFC3986: '+' не пробел");
    CHECK(urlEncode("AZaz09-_.~") == "AZaz09-_.~", "unreserved не кодируются");
    CHECK(urlDecode("%zz") == "%zz", "битый % не падает");
    // YT-10: запрос подсказок с «ё», амперсандом и пробелом уходит корректно.
    CHECK(urlEncode("ё") == "%D1%91", "urlEncode: ё в UTF-8");
    CHECK(urlEncode("a&b") == "a%26b", "urlEncode: & не ломает query");
    CHECK(urlEncode("a b") == "a%20b", "urlEncode: пробел -> %20 в query");
}

static void testHeaders() {
    net::Headers h = {{"Content-Type", "application/json"}, {"X-Test", "1"}};
    CHECK(net::hasHeader(h, "content-type"), "hasHeader без учёта регистра");
    CHECK(!net::hasHeader(h, "User-Agent"), "hasHeader отсутствующего");

    std::pair<std::string, std::string> kv;
    CHECK(net::parseHeaderLine("Name: value", kv) && kv.first == "Name" && kv.second == "value",
          "parseHeaderLine обычный");
    CHECK(net::parseHeaderLine("  A  :  b  ", kv) && kv.first == "A" && kv.second == "b",
          "parseHeaderLine обрезает края");
    CHECK(!net::parseHeaderLine("no-colon", kv), "parseHeaderLine без ':' -> false");
    CHECK(!net::parseHeaderLine(": value", kv), "parseHeaderLine без имени -> false");
    CHECK(net::buildHeaderLine("X-A", "1") == "X-A: 1", "buildHeaderLine");
}

static void testRedact() {
    net::Headers h = {{"Cookie", "SID=secret"},
                      {"Authorization", "Bearer x"},
                      {"Set-Cookie", "a=b"},
                      {"Accept", "text/html"}};
    net::Headers r = net::redactHeaders(h);
    CHECK(r.size() == h.size(), "redactHeaders сохраняет количество");
    CHECK(r[0].second == "<скрыто>", "Cookie скрыт");
    CHECK(r[1].second == "<скрыто>", "Authorization скрыт");
    CHECK(r[2].second == "<скрыто>", "Set-Cookie скрыт");
    CHECK(r[3].second == "text/html", "обычный заголовок сохранён");

    CHECK(net::redactProxy("socks5h://user:pass@127.0.0.1:1080") ==
              "socks5h://user:***@127.0.0.1:1080",
          "redactProxy маскирует пароль");
    CHECK(net::redactProxy("socks5h://127.0.0.1:1080") == "socks5h://127.0.0.1:1080",
          "redactProxy без креденшелов не меняет");

    // LOG-06: в логе остаются схема/хост/путь и только имена параметров.
    const std::string gv =
        "https://rr3---sn-abc.googlevideo.com/videoplayback?expire=1&ei=x&ip=1.2.3.4"
        "&id=o-abc&itag=22&mime=video%2Fmp4&range=0-1000&n=SIGSECRET&pot=TOKEN&sig=ABC";
    const std::string r1 = net::redactUrl(gv);
    CHECK(r1.rfind("https://rr3---sn-abc.googlevideo.com/videoplayback?", 0) == 0,
          "redactUrl сохраняет схему, хост и путь");
    CHECK(r1.find("SIGSECRET") == std::string::npos, "redactUrl убирает значение sig/n");
    CHECK(r1.find("TOKEN") == std::string::npos, "redactUrl убирает pot");
    CHECK(r1.find("sig=") == std::string::npos && r1.find("pot=") == std::string::npos,
          "redactUrl не оставляет sig=/pot= со значениями");
    CHECK(contains(r1, "itag") && contains(r1, "range") && contains(r1, "mime") &&
              contains(r1, "n") && contains(r1, "pot"),
          "redactUrl оставляет имена параметров");
    CHECK(net::redactUrl("https://www.youtube.com/watch?v=abc") ==
              "https://www.youtube.com/watch?v",
          "redactUrl: единственный параметр даёт только имя");
    CHECK(net::redactUrl("https://i.ytimg.com/vi/abc/hq.jpg") ==
              "https://i.ytimg.com/vi/abc/hq.jpg",
          "redactUrl без query не меняет");
    CHECK(net::redactUrl("https://a.b/c#frag?d=1") == "https://a.b/c",
          "redactUrl отрезает фрагмент");
    CHECK(net::redactUrl("") == "", "redactUrl пустой строки");
    CHECK(net::redactUrl("https://a.b/c?").find('?') == std::string::npos,
          "redactUrl с пустым query не оставляет '?'");
}

static void testEscape() {
    const std::string body = std::string("A\x01\nB\\C") + std::string(600, 'z');
    const std::string e = net::escapeForLog(body, 512);
    CHECK(contains(e, "\\x01"), "управляющий байт экранирован");
    CHECK(contains(e, "\\n"), "перевод строки экранирован");
    CHECK(contains(e, "\\\\"), "обратный слэш экранирован");
    CHECK(e.find('\n') == std::string::npos, "реального \\n в выводе нет");
    CHECK(contains(e, "…"), "при превышении лимита добавлено многоточие");
    // Длина: 512 входных байт -> не больше ~2x + многоточие.
    CHECK(e.size() <= 512 * 2 + 4, "экранирование ограничено maxBytes");

    const std::string shortE = net::escapeForLog("ok", 512);
    CHECK(shortE == "ok", "короткое тело без изменений");
}

static void testQueueSuffix() {
    char buf[48];
    net::formatQueueSuffix(buf, sizeof(buf), 10);
    CHECK(std::string(buf).empty(), "малая очередь -> пусто");
    net::formatQueueSuffix(buf, sizeof(buf), 49);
    CHECK(std::string(buf).empty(), "49 мс -> пусто");
    net::formatQueueSuffix(buf, sizeof(buf), 100);
    const std::string s = buf;
    CHECK(contains(s, "100"), "реальное время в суффиксе");
    CHECK(!contains(s, "%lld"), "нет литерала %lld (NET-06)");
    CHECK(contains(s, "в очереди"), "текст суффикса");
}

static void testBudget() {
    CHECK(net::budgetMsFor("https://i.ytimg.com/vi/x/hqdefault.jpg") == net::kTimeoutImagesMs,
          "обложка -> 8 с");
    CHECK(net::budgetMsFor("https://www.youtube.com/") == net::kTimeoutBootstrapMs,
          "bootstrap / -> 10 с");
    CHECK(net::budgetMsFor("https://www.youtube.com/youtubei/v1/browse?key=k") ==
              net::kTimeoutBrowseMs,
          "browse -> 20 с (youtubei важнее bootstrap)");
    CHECK(net::budgetMsFor("https://www.youtube.com/sw.js_data") == net::kTimeoutBootstrapMs,
          "sw.js_data -> bootstrap");
    CHECK(net::budgetMsFor("https://example.com/x") == 0, "чужая сеть -> без бюджета");
}

static void testVerifyPolicy() {
    CHECK(std::string(net::verifyName(net::Verify::Full)) == "Full", "verifyName Full");
    CHECK(std::string(net::verifyName(net::Verify::ChainOnly)) == "ChainOnly", "verifyName ChainOnly");
    CHECK(std::string(net::verifyName(net::Verify::None)) == "None", "verifyName None");
}

static void testSessionBudget() {
    auto& b = net::SessionBudget::shared();
    b.reset();
    b.reserveForPlayer(2);
    CHECK(b.total() == 4, "всего 4 сессии");
    CHECK(b.playerReserve() == 2, "резерв плеера = 2");

    // uiCap = 4 - 2 = 2. api занимает слоты первым.
    CHECK(b.acquire(net::SessionClass::Api), "api #1");
    CHECK(b.acquire(net::SessionClass::Api), "api #2");
    CHECK(!b.acquire(net::SessionClass::Api), "api #3 отклонён (uiCap=2)");
    CHECK(!b.acquire(net::SessionClass::Images), "images уступает api");
    CHECK(b.used() == 2, "занято 2");

    b.release(net::SessionClass::Api);
    b.release(net::SessionClass::Api);
    CHECK(b.used() == 0, "после release занято 0");
    CHECK(b.acquire(net::SessionClass::Images), "images #1");
    CHECK(!b.acquire(net::SessionClass::Images), "images #2 отклонён (резерв под api)");

    // Плеер игнорирует UI-лимит и берёт свободные слоты (в т.ч. сверх своего
    // резерва): images занимает 1 из 4.
    CHECK(b.acquire(net::SessionClass::Player), "player #1");
    CHECK(b.acquire(net::SessionClass::Player), "player #2");
    CHECK(b.acquire(net::SessionClass::Player), "player #3 (1 слот занят images)");
    CHECK(b.used() == 4, "итого 4 занято");
    CHECK(!b.acquire(net::SessionClass::Player), "player #4 отклонён (нет свободных)");

    b.reset();
    b.reserveForPlayer(0);
    CHECK(b.acquire(net::SessionClass::Api), "api без резерва");
    CHECK(b.acquire(net::SessionClass::Images), "images без резерва");
    CHECK(b.acquire(net::SessionClass::Images), "images #2 без резерва");
    b.reset();
    b.reserveForPlayer(2);
}

static void testThumbFallback() {
    const std::string maxres = "https://i.ytimg.com/vi/abc/maxresdefault.jpg";
    auto c = net::thumbnailCandidates(maxres);
    CHECK(c.size() == 5, "maxres -> 5 кандидатов");
    CHECK(c[0] == maxres, "первый — исходный");
    CHECK(contains(c[1], "sddefault"), "далее sddefault");
    CHECK(contains(c[2], "hqdefault"), "далее hqdefault");
    CHECK(contains(c[3], "mqdefault"), "далее mqdefault");
    CHECK(contains(c[4], "default.jpg") && !contains(c[4], "qdefault"), "последний — default");

    const std::string hq = "https://i.ytimg.com/vi/abc/hqdefault.jpg";
    auto c2 = net::thumbnailCandidates(hq);
    CHECK(c2.size() == 3, "hq -> 3 кандидата (вверх не лезем)");
    CHECK(c2[0] == hq, "hq первый");
    CHECK(contains(c2[1], "mqdefault"), "hq -> mq");
    CHECK(contains(c2[2], "default.jpg") && !contains(c2[2], "qdefault"), "hq -> default");

    auto c3 = net::thumbnailCandidates("https://i.ytimg.com/vi/abc/default.jpg");
    CHECK(c3.size() == 1, "default -> один кандидат");

    auto c4 = net::thumbnailCandidates("https://i.ytimg.com/vi_webp/abc/sddefault.webp");
    CHECK(c4.size() == 4, "sddefault.webp -> 4");
    CHECK(contains(c4[0], "sddefault.webp"), "webp расширение сохраняется");
    CHECK(contains(c4[3], "default.webp"), "последний webp");

    auto c5 = net::thumbnailCandidates("https://example.com/cover.jpg");
    CHECK(c5.size() == 1 && c5[0] == "https://example.com/cover.jpg", "не ytimg -> без цепочки");

    CHECK(net::thumbnailAtQuality(hq, "sddefault") ==
              "https://i.ytimg.com/vi/abc/sddefault.jpg",
          "thumbnailAtQuality подменяет качество");
    CHECK(net::thumbnailAtQuality("https://example.com/x.jpg", "sddefault").empty(),
          "thumbnailAtQuality для не-ytimg -> пусто");
}

// --- NET-12: именованные таймауты -------------------------------------------

static void testTimeouts() {
    const net::NetTimeouts saved = net::timeouts();
    CHECK(net::timeouts().bootstrap == 10000, "bootstrap = 10 с");
    CHECK(net::timeouts().api == 20000, "api = 20 с");
    CHECK(net::timeouts().images == 8000, "images = 8 с");
    CHECK(net::timeouts().probe == 9000, "probe = 9 с");
    CHECK(net::timeouts().player == 12000, "player = 12 с");

    // Переопределение влияет и на budgetMsFor (NET-12: без «магических» чисел).
    CHECK(net::setTimeout("images", 1234), "setTimeout images");
    CHECK(net::timeouts().images == 1234, "images переопределён");
    CHECK(net::budgetMsFor("https://i.ytimg.com/vi/x/hqdefault.jpg") == 1234,
          "budgetMsFor читает runtime-таймаут");
    CHECK(net::setTimeout("api", 4321), "setTimeout api");
    CHECK(net::budgetMsFor("https://www.youtube.com/youtubei/v1/browse?key=k") == 4321,
          "browse читает runtime api-таймаут");

    CHECK(!net::setTimeout("nope", 100), "setTimeout неизвестного имени -> false");
    CHECK(!net::setTimeout("api", 0), "setTimeout <= 0 -> false");

    net::mutableTimeouts() = saved; // восстановить
}

// --- NET-13: лимиты ответа ---------------------------------------------------

static void testLimits() {
    const size_t mb = 1u << 20;
    CHECK(net::maxBytesForUrl("https://www.youtube.com/youtubei/v1/browse?key=k") == 8 * mb,
          "browse -> 8 МБ");
    CHECK(net::maxBytesForUrl("https://www.youtube.com/youtubei/v1/search") == 8 * mb,
          "search -> 8 МБ");
    CHECK(net::maxBytesForUrl("https://www.youtube.com/youtubei/v1/player") == 4 * mb,
          "player -> 4 МБ");
    CHECK(net::maxBytesForUrl("https://www.youtube.com/") == 6 * mb, "bootstrap / -> 6 МБ");
    CHECK(net::maxBytesForUrl("https://www.youtube.com/sw.js_data") == 6 * mb,
          "sw.js_data -> 6 МБ");
    CHECK(net::maxBytesForUrl("https://i.ytimg.com/vi/x/hqdefault.jpg") == 8 * mb,
          "обложка -> 8 МБ");
    CHECK(net::maxBytesForUrl("https://example.com/x") == 0, "чужая сеть -> без класса");

    // Жёсткий потолок: даже если вызывающий передал больше, browse урежется.
    CHECK(net::effectiveMaxBytes("https://www.youtube.com/youtubei/v1/browse", 16 * mb) == 8 * mb,
          "browse не больше 8 МБ при запросе 16 МБ");
    CHECK(net::effectiveMaxBytes("https://www.youtube.com/youtubei/v1/browse", 1 * mb) == 1 * mb,
          "меньший запрошенный лимит сохраняется");
    CHECK(net::effectiveMaxBytes("https://example.com/x", 5 * mb) == 5 * mb,
          "без класса — как просили");
    CHECK(net::effectiveMaxBytes("https://example.com/x", 0) == net::limits::kDefault,
          "0 = значение по умолчанию");
}

// --- NET-08: матрица повторов ------------------------------------------------

static void testRetryMatrix() {
    using net::NetFail;
    net::RetryOptions get; // idempotent=true, hasBody=false
    net::RetryOptions post;
    post.idempotent = false;
    post.hasBody = true;

    // 5xx — ровно один повтор (только attempt 0).
    auto d = net::decideRetry(NetFail::None, 500, 0, 3, get);
    CHECK(d.retry && std::string(d.reason) == "http_5xx", "GET 500 -> повтор reason=http_5xx");
    CHECK(!net::decideRetry(NetFail::None, 500, 1, 3, get).retry, "GET 500 второй раз -> нет");
    CHECK(!net::decideRetry(NetFail::None, 503, 0, 1, get).retry, "GET 503 attempts=1 -> нет");

    // 4xx — не повторяем, кроме 408/429.
    CHECK(!net::decideRetry(NetFail::None, 400, 0, 3, get).retry, "GET 400 -> нет");
    CHECK(!net::decideRetry(NetFail::None, 404, 0, 3, get).retry, "GET 404 -> нет");
    d = net::decideRetry(NetFail::None, 429, 0, 3, get);
    CHECK(d.retry && d.honorRetryAfter && std::string(d.reason) == "http_429",
          "GET 429 -> повтор с Retry-After");
    d = net::decideRetry(NetFail::None, 408, 0, 3, get);
    CHECK(d.retry && d.honorRetryAfter && std::string(d.reason) == "http_408",
          "GET 408 -> повтор");

    // Сетевые ошибки: connect безопасен для любого метода.
    CHECK(net::decideRetry(NetFail::Connect, 0, 0, 3, post).retry, "POST connect -> повтор");
    CHECK(net::decideRetry(NetFail::Connect, 0, 0, 3, get).retry, "GET connect -> повтор");

    // Timeout: GET повторяется; POST — только пока ответ не начался.
    CHECK(net::decideRetry(NetFail::Timeout, 0, 0, 3, get).retry, "GET timeout -> повтор");
    CHECK(net::decideRetry(NetFail::Timeout, 0, 0, 3, post).retry,
          "POST timeout без ответа -> повтор");
    post.responseStarted = true;
    CHECK(!net::decideRetry(NetFail::Timeout, 200, 1, 3, post).retry,
          "POST timeout после начала ответа -> нет (не дублировать)");
    post.responseStarted = false;

    // Исчерпание попыток.
    CHECK(!net::decideRetry(NetFail::Timeout, 0, 2, 3, get).retry, "GET timeout attempt=2/3 -> нет");

    // Отмена не повторяется.
    CHECK(!net::decideRetry(NetFail::Aborted, 0, 0, 3, get).retry, "отмена -> нет повтора");

    // POST не повторяем на 5xx (только сетевые ошибки).
    CHECK(!net::decideRetry(NetFail::None, 500, 0, 3, post).retry, "POST 500 -> нет повтора");
    CHECK(!net::decideRetry(NetFail::None, 400, 0, 3, post).retry, "POST 400 -> нет повтора");

    // Jitter ±20%.
    CHECK(net::jitteredDelayMs(1000, 0.0) == 800, "jitter min = -20%");
    CHECK(net::jitteredDelayMs(1000, 0.5) == 1000, "jitter mid = 0%");
    CHECK(net::jitteredDelayMs(1000, 0.999999) <= 1200, "jitter max <= +20%");
    CHECK(net::jitteredDelayMs(0, 0.5) == 0, "jitter нулевой базы = 0");

    // Retry-After не дольше 5 с.
    CHECK(net::retryAfterMs("2") == 2000, "Retry-After: 2 -> 2000 мс");
    CHECK(net::retryAfterMs("10") == 5000, "Retry-After: 10 -> потолок 5 с");
    CHECK(net::retryAfterMs("0") == 0, "Retry-After: 0 -> 0");
    CHECK(net::retryAfterMs("") == 0, "пустой Retry-After -> 0");
    CHECK(net::retryAfterMs("abc") == 0, "нечисловой Retry-After -> 0");
    CHECK(net::retryAfterMs("Wed, 21 Oct 2015 07:28:00 GMT") == 0, "HTTP-дата -> 0");
}

// --- NET-10: неизменяемый снапшот NetConfig ---------------------------------

static void testNetConfig() {
    net::NetConfig c1;
    c1.proxy = "socks5h://127.0.0.1:1111";
    c1.verify = net::Verify::Full;
    CHECK(net::reconfigure(c1), "reconfigure #1");
    auto snapA = net::configSnapshot();
    CHECK(snapA && snapA->proxy == "socks5h://127.0.0.1:1111", "снапшот A виден");

    net::NetConfig c2;
    c2.proxy = "socks5h://127.0.0.1:2222";
    c2.verify = net::Verify::ChainOnly;
    c2.userAgent = "UA-TEST";
    CHECK(net::reconfigure(c2), "reconfigure #2");
    // Старый снапшот неизменяем: его поля не поменялись.
    CHECK(snapA->proxy == "socks5h://127.0.0.1:1111", "старый снапшот не изменился");
    CHECK(net::proxy() == "socks5h://127.0.0.1:2222", "proxy() читает новый снапшот");
    CHECK(net::verify() == net::Verify::ChainOnly, "verify() читает новый снапшот");
    CHECK(net::configSnapshot()->userAgent == "UA-TEST", "userAgent в снапшоте");

    // set* создают новый снапшот, остальные поля переносятся.
    net::setProxy("socks5h://127.0.0.1:3333");
    CHECK(net::proxy() == "socks5h://127.0.0.1:3333", "setProxy -> новый снапшот");
    CHECK(net::configSnapshot()->userAgent == "UA-TEST", "прочие поля сохранены при setProxy");

    net::reconfigure(net::NetConfig{}); // восстановить по умолчанию
    CHECK(net::proxy().empty(), "конфиг восстановлен (без прокси)");
    CHECK(net::verify() == net::Verify::Full, "verify восстановлен в Full");
}

// --- NET-11: CancelToken -----------------------------------------------------

static void testCancelToken() {
    net::CancelToken tok;
    CHECK(!tok.cancelled(), "токен изначально активен");
    net::CancelToken copy = tok;
    copy.cancel();
    CHECK(tok.cancelled(), "копия токена делит состояние");
    CHECK(copy.cancelled(), "отменённый токен");

    // Отменённый до старта запрос не идёт в сеть и возвращает "cancelled".
    net::CancelToken dead;
    dead.cancel();
    net::Response r = net::get("http://127.0.0.1:1/never", {}, 200, 1u << 20, 1, nullptr, &dead);
    CHECK(r.error == "cancelled", "отменённый запрос -> error=cancelled");
    CHECK(r.status == 0, "отменённый запрос без статуса");
}

// --- DPI-07/DPI-08: SNI-политика --------------------------------------------

static void testSniPolicy() {
    CHECK(net::hostMatchesDomain("rr4---sn-abc.googlevideo.com", "googlevideo.com"),
          "строгий suffix: поддомен совпадает");
    CHECK(net::hostMatchesDomain("googlevideo.com", "googlevideo.com"), "точное совпадение");
    CHECK(net::hostMatchesDomain("WWW.YOUTUBE.COM", "youtube.com"), "регистронезависимо");
    CHECK(!net::hostMatchesDomain("evilgooglevideo.com", "googlevideo.com"),
          "evilgooglevideo.com НЕ поддомен (граница метки)");
    CHECK(!net::hostMatchesDomain("googlevideo.com.evil.com", "googlevideo.com"),
          "суффикс не в конце -> нет");
    CHECK(!net::hostMatchesDomain("youtube.com.evil.com", "youtube.com"),
          "youtube.com.evil.com -> нет");

    CHECK(std::string(net::sniSpoofName("rr4---sn-abc.googlevideo.com")) == "www.google.com",
          "googlevideo -> spoof=www.google.com");
    CHECK(net::sniSpoofName("example.com") == nullptr, "example.com -> без подмены");
    CHECK(net::sniSpoofName("evilgooglevideo.com") == nullptr,
          "evilgooglevideo.com -> без подмены");
    CHECK(net::sniSpoofName("www.youtube.com") == nullptr,
          "youtube.com не подменяется (только регистр)");

    CHECK(net::sniNeedsUpper("www.youtube.com"), "youtube.com -> верхний регистр");
    CHECK(net::sniNeedsUpper("i.ytimg.com"), "ytimg.com -> верхний регистр");
    CHECK(!net::sniNeedsUpper("example.com"), "example.com -> без изменений");
    CHECK(net::sniNeedsUpper("googlevideo.com"), "googlevideo всё ещё в списке регистра");
}

// --- NET-08: loopback mock-сервер -------------------------------------------

namespace {

// Мини HTTP-сервер на 127.0.0.1 для проверки числа попыток. Не выходит в сеть:
// слушает только loopback, порт выбирается ядром.
class MockServer {
public:
    // mode: 0=200, 1=500, 2=400, 3=держать соединение (таймаут клиента),
    //       4=404, 5=chunked, 6=gzip, 7=обрыв тела (partial)
    bool start() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
        if (::listen(fd, 32) != 0) return false;
        socklen_t len = sizeof(a);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) return false;
        port_ = ntohs(a.sin_port);
        listenFd_.store(fd);
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        running_ = false;
        const int fd = listenFd_.exchange(-1);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& t : handlers_) {
            if (t.joinable()) t.join();
        }
    }
    int port() const { return port_; }
    int count() const { return count_.load(); }
    void setMode(int m) { mode_.store(m); }

private:
    void loop() {
        while (running_) {
            int c = ::accept(listenFd_.load(), nullptr, nullptr);
            if (c < 0) break;
            count_.fetch_add(1);
            std::lock_guard<std::mutex> lk(mu_);
            handlers_.emplace_back([this, c] { handle(c); });
        }
    }
    void handle(int c) {
        char buf[2048];
        const ssize_t n = ::recv(c, buf, sizeof(buf), 0); // заголовки запроса
        if (n > 0) {
            std::lock_guard<std::mutex> lk(mu_);
            lastReq_.assign(buf, static_cast<size_t>(n));
        }
        const int m = mode_.load();
        switch (m) {
            case 3:   // дольше клиентского таймаута
                std::this_thread::sleep_for(std::chrono::seconds(3));
                break;
            case 4: {
                const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                   "Connection: close\r\n\r\n";
                ::send(c, resp, std::strlen(resp), 0);
                break;
            }
            case 5: {   // chunked: тело кусками, без Content-Length
                const char* resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                                   "Connection: close\r\n\r\n"
                                   "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
                ::send(c, resp, std::strlen(resp), 0);
                break;
            }
            case 6: {   // gzip("hello"), libcurl распаковывает сам (ACCEPT_ENCODING)
                static const unsigned char gz[] = {
                    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
                    0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x86, 0xa6, 0x10,
                    0x36, 0x05, 0x00, 0x00, 0x00};
                char hdr[160];
                std::snprintf(hdr, sizeof(hdr),
                              "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
                              "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                              sizeof(gz));
                ::send(c, hdr, std::strlen(hdr), 0);
                ::send(c, gz, sizeof(gz), 0);
                break;
            }
            case 7: {   // обещали 10 байт, отдали 3 и закрылись -> partial file
                const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n"
                                   "Connection: close\r\n\r\nabc";
                ::send(c, resp, std::strlen(resp), 0);
                break;
            }
            case 1: {
                const char* resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
                                   "Connection: close\r\n\r\n";
                ::send(c, resp, std::strlen(resp), 0);
                break;
            }
            case 2: {
                const char* resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                                   "Connection: close\r\n\r\n";
                ::send(c, resp, std::strlen(resp), 0);
                break;
            }
            default: {
                const char* resp =
                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
                ::send(c, resp, std::strlen(resp), 0);
                break;
            }
        }
        ::close(c);
    }
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> count_{0};
    std::atomic<int> mode_{0};
    std::atomic<int> listenFd_{-1};
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::thread> handlers_;
    std::string lastReq_;

public:
    // Последний полученный запрос (для проверки заголовков, NET-07).
    std::string lastRequest() {
        std::lock_guard<std::mutex> lk(mu_);
        return lastReq_;
    }
};

} // namespace

// PERF-03: кэш обложек — лимит памяти и LRU-вытеснение. Сеть не нужна:
// testPut() кладёт изображение прямо в кэш (сборка TNX_HOST).
static void testImageCacheLru() {
    const size_t px100 = 100u * 100u * 4u;   // 100×100 RGBA = 40 000 Б
    const size_t limit = 1024u * 1024u;      // 1 МБ
    const int capacity = static_cast<int>(limit / px100); // 26 записей

    // --- Лимит: 500 вставок по 40 КБ не должны превысить 1 МБ ---
    net::ImageCache cache;
    cache.setMemoryLimitMB(1);
    for (int i = 0; i < 500; ++i) cache.testPut("u" + std::to_string(i), 100, 100);

    const net::ImageCache::Stats st = cache.stats();
    CHECK(st.bytes <= limit, "PERF-03: лимит 1 МБ соблюдён");
    CHECK(st.bytes > limit - px100, "PERF-03: кэш заполнен почти до лимита");
    CHECK(st.entries == capacity, "PERF-03: в кэше ровно 26 записей по 40 КБ");
    CHECK(st.evicted == 500u - (unsigned)capacity, "PERF-03: вытеснено 474 записи");
    CHECK(st.limitMb == 1, "PERF-03: stats отдаёт текущий лимит");
    CHECK(cache.get("u0") == nullptr, "PERF-03: самая старая запись вытеснена");
    CHECK(cache.get("u499") != nullptr, "PERF-03: свежая запись в кэше");

    // --- LRU: тронутая запись переживает вытеснение ---
    net::ImageCache lru;
    lru.setMemoryLimitMB(1);
    for (int i = 0; i < capacity; ++i) lru.testPut("v" + std::to_string(i), 100, 100);
    CHECK(lru.stats().entries == capacity, "PERF-03: кэш ровно на лимите");

    CHECK(lru.get("v0") != nullptr, "PERF-03: v0 в кэше до вытеснения");
    // get() обновляет lastUsed у v0 — теперь самая старая запись это v1.
    lru.testPut("v" + std::to_string(capacity), 100, 100);

    CHECK(lru.get("v0") != nullptr, "PERF-03: тронутая v0 выжила (LRU)");
    CHECK(lru.get("v1") == nullptr, "PERF-03: нетронутая v1 вытеснена первой");
    CHECK(lru.stats().entries == capacity, "PERF-03: после вытеснения лимит соблюдён");
    CHECK(lru.stats().bytes <= limit, "PERF-03: байты в пределах лимита");
}

static void testMockRetry() {
    MockServer srv;
    if (!srv.start()) {
        std::fprintf(stderr, "  SKIP: mock-сервер не поднялся\n");
        return;
    }
    const std::string base = "http://127.0.0.1:" + std::to_string(srv.port()) + "/";
    const net::Headers h;

    // 5xx -> 2 попытки.
    srv.setMode(1);
    net::Response r500 = net::get(base + "s500", h, 1500, 1u << 20, 3);
    CHECK(r500.status == 500, "mock: 500 получен");
    const int c500 = srv.count();
    CHECK(c500 == 2, "mock: 500 -> 2 попытки");

    // 4xx -> 1 попытка.
    srv.setMode(2);
    net::Response r400 = net::get(base + "s400", h, 1500, 1u << 20, 3);
    CHECK(r400.status == 400, "mock: 400 получен");
    const int c400 = srv.count() - c500;
    CHECK(c400 == 1, "mock: 400 -> 1 попытка");

    // NET-07: на проводе ровно один User-Agent, и это UA вызывающего.
    srv.setMode(0);
    net::Headers hUa = {{"User-Agent", "UA-CUSTOM/1.0"}};
    net::Response rua = net::get(base + "ua", hUa, 1500, 1u << 20, 1);
    CHECK(rua.status == 200, "mock: 200 для проверки UA");
    const std::string reqUa = srv.lastRequest();
    CHECK(countSubstr(reqUa, "User-Agent:") == 1, "NET-07: ровно один User-Agent");
    CHECK(contains(reqUa, "UA-CUSTOM/1.0"), "NET-07: отправлен UA вызывающего");
    CHECK(!contains(reqUa, "Nintendo Switch"), "NET-07: глобальный UA не продублирован");

    // NET-07: без своего UA — один User-Agent с глобальным значением.
    net::Response rdef = net::get(base + "ua2", {}, 1500, 1u << 20, 1);
    CHECK(rdef.status == 200, "mock: 200 без своего UA");
    const std::string reqDef = srv.lastRequest();
    CHECK(countSubstr(reqDef, "User-Agent:") == 1, "NET-07: один UA по умолчанию");
    CHECK(contains(reqDef, "Nintendo Switch"), "NET-07: отправлен глобальный UA");

    // timeout -> 3 попытки (GET идемпотентен).
    // TEST-02: 404 — один ответ, без повторов (клиентская ошибка).
    srv.setMode(4);
    const int cBefore404 = srv.count();
    net::Response r404 = net::get(base + "s404", h, 1500, 1u << 20, 3);
    CHECK(r404.status == 404, "mock: 404 получен");
    CHECK(srv.count() - cBefore404 == 1, "mock: 404 -> 1 попытка");

    // TEST-02: chunked-ответ собирается в целое тело.
    srv.setMode(5);
    net::Response rChunk = net::get(base + "chunked", h, 1500, 1u << 20, 1);
    CHECK(rChunk.status == 200, "mock: chunked -> 200");
    CHECK(rChunk.body == "hello world", "mock: chunked-тело собрано");

    // TEST-02: gzip распаковывается прозрачно (curl ACCEPT_ENCODING).
    srv.setMode(6);
    net::Response rGz = net::get(base + "gzip", h, 1500, 1u << 20, 1);
    CHECK(rGz.status == 200, "mock: gzip -> 200");
    CHECK(rGz.body == "hello", "mock: gzip-тело распаковано");

    // TEST-02: обрыв тела — ошибка, а не «короткий успешный ответ».
    srv.setMode(7);
    net::Response rPart = net::get(base + "partial", h, 1500, 1u << 20, 1);
    CHECK(!rPart.error.empty(), "mock: обрыв тела -> ошибка");
    CHECK(rPart.body.size() < 10, "mock: обрыв тела не выдаёт неполный ответ за целый");

    // timeout -> 3 попытки (GET идемпотентен).
    srv.setMode(3);
    const int before = srv.count(); // отсчёт ровно перед медленным запросом
    net::Response rto = net::get(base + "slow", h, 300, 1u << 20, 3);
    CHECK(!rto.error.empty(), "mock: timeout -> ошибка");
    for (int i = 0; i < 240 && srv.count() < before + 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(srv.count() - before == 3, "mock: timeout -> 3 попытки");

    srv.stop();
}

int main() {
    testUrlCodec();
    testHeaders();
    testRedact();
    testEscape();
    testQueueSuffix();
    testBudget();
    testVerifyPolicy();
    testSessionBudget();
    testThumbFallback();
    testTimeouts();
    testLimits();
    testRetryMatrix();
    testNetConfig();
    testCancelToken();
    testSniPolicy();
    testImageCacheLru();
    testMockRetry();

    if (g_fail == 0) {
        std::printf("OK (%d проверок)\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "ПРОВАЛЕНО %d из %d проверок\n", g_fail, g_checks);
    return 1;
}
