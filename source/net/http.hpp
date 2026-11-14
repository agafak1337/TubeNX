// net/http.hpp — HTTPS-клиент на libcurl (mbedtls), с опциональным SOCKS5-обходом DPI.
#pragma once

#include "net/timeouts.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tnx::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

// Переопределение глобального конфига для одного запроса. Нужно пробам:
// они не должны менять net::setProxy()/setVerify() под работающими потоками.
struct Overrides {
    const char* proxy = nullptr; // nullptr = глобальный; "" = принудительно напрямую
    int verify = -1;             // -1 = глобальный, иначе (int)Verify
};

// Проверка TLS. Full — как в браузере (цепочка + имя хоста). ChainOnly —
// только цепочка: нужно, если сервис консоли не принимает имя хоста в
// верхнем регистре (наш обход SNI-блокировки). None — без проверок (диагностика).
enum class Verify { Full = 0, ChainOnly = 1, None = 2 };

struct Response {
    long status = 0;
    std::string body;
    Headers headers;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
    std::string header(const std::string& name) const;
};

bool init();
void shutdown();

// Закрыть простаивающие keep-alive соединения (освобождает SSL-сессии
// сервиса ssl перед запуском видео: их всего 4 на всю систему).
void closeIdleConnections();

// --- Неизменяемый снапшот сетевой конфигурации (NET-10) ---------------------
// После старта запросов настройки не читаются из глобальных переменных:
// каждый запрос берёт shared_ptr<const NetConfig>. Смена настроек создаёт
// НОВЫЙ снапшот (reconfigure() — барьер): начатые запросы доработают на
// прежнем (он неизменяем), новые увидят новый. Гонок на полях нет.
const char* defaultUserAgent();

struct NetConfig {
    std::string proxy;                                  // "" — ходим напрямую
    std::string caBundle = "romfs:/cacert-trim.pem";
    std::string userAgent = defaultUserAgent();
    Verify verify = Verify::Full;
};

// Текущий снапшот. Возвращённый объект менять нельзя (const).
std::shared_ptr<const NetConfig> configSnapshot();
// Атомарно заменить снапшот (барьер). false — неверный конфиг/идёт shutdown.
bool reconfigure(const NetConfig& cfg);

// Обёртки над reconfigure(): оставлены для совместимости с вызывающими.
// Каждый set* делает полный барьер (копия + swap), поэтому безопасны и после
// init(); «тихой» частичной записи под работающими запросами нет.
void setProxy(const std::string& url);   // "socks5h://127.0.0.1:1080" или "" — напрямую
std::string proxy();
void setCaBundlePath(const std::string& path);
void setUserAgent(const std::string& ua);
void setVerify(Verify v);
Verify verify();
// Имя режима для логов: "Full" / "ChainOnly" / "None" (NET-14).
const char* verifyName(Verify v);

// --- Бюджеты запросов (NET-03) ---------------------------------------------
// Совместимость: канонические значения теперь в net/timeouts.hpp (NET-12).
// bootstrap GET / — 10 с, InnerTube browse/search/next — 20 с, обложки — 8 с.
constexpr int kTimeoutBootstrapMs = defaults::kBootstrapMs;
constexpr int kTimeoutBrowseMs = defaults::kApiMs;
constexpr int kTimeoutImagesMs = defaults::kImagesMs;
// 0 — у URL нет отдельного бюджета (используем timeoutMs вызывающего).
int budgetMsFor(const std::string& url);

// --- Лимиты ответа (NET-13) -------------------------------------------------
// Жёсткий потолок размера тела для класса URL. 0 — класс не задан.
size_t maxBytesForUrl(const std::string& url);
// Итоговый лимит: min(запрошенный, потолок класса). 0 у запрошенного = «не задан».
size_t effectiveMaxBytes(const std::string& url, size_t requested);

// --- SSL-бюджет (NET-09) ----------------------------------------------------
// Сервис ssl libnx даёт всего 4 сессии. Приоритет: player > api > images.
// Менеджер только считает слоты; интеграцию с плеером делает вызывающий
// (net::closeIdleConnections() перед стартом видео уже есть).
constexpr int kSslSessions = 4;
enum class SessionClass { Player = 0, Api = 1, Images = 2 };

class SessionBudget {
public:
    static SessionBudget& shared();

    // Занять слот класса. false — свободных слотов нет: вызывающий должен
    // освободить простаивающие соединения и повторить попытку.
    bool acquire(SessionClass c);
    void release(SessionClass c);

    // Резерв под плеер: player держит эти слоты, UI (api/images) их не занимает.
    // По умолчанию зарезервировано 2 сессии (см. docs/impl-tz.md §7.2).
    void reserveForPlayer(int sessions);
    int playerReserve() const;

    // Обнулить счётчики занятых слотов (при shutdown/init). Резерв сохраняется.
    void reset();

    int total() const { return kSslSessions; }
    int used() const;
    int freeSlots() const;

    struct Stats {
        int total = 0, player = 0, api = 0, images = 0, used = 0;
    };
    Stats stats() const;

private:
    SessionBudget() = default;
    mutable std::mutex mu_;
    int player_ = 0, api_ = 0, images_ = 0;
    int playerReserve_ = 2;
};

// --- Отмена устаревших запросов (NET-11) ------------------------------------
// Кооперативный токен: новый поиск отменяет предыдущий; выход с экрана
// отменяет его загрузку. Копии токена разделяют одно состояние — передавай
// CancelToken* в get()/post().
//
// ВАЖНО: player-поток НЕ должен получать этот токен: отмена видео идёт через
// player::close(), а не через транспорт. Токен по умолчанию nullptr — запрос
// не отменяется.
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    CancelToken(const CancelToken&) = default;
    CancelToken& operator=(const CancelToken&) = default;

    void cancel() { flag_->store(true, std::memory_order_relaxed); }
    bool cancelled() const { return flag_->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// --- Матрица повторов (NET-08) ----------------------------------------------
// Класс сетевой ошибки: curl-коды сгруппированы, чтобы политика повторов была
// чистой функцией и тестировалась на host без сети.
enum class NetFail {
    None = 0,  // curl OK: есть HTTP-ответ (смотрим на status)
    Connect,   // connect/resolve/ssl-connect: тело запроса ещё не ушло
    Timeout,   // общий таймаут
    Io,        // recv/send/partial/got-nothing
    Aborted,   // отменён вызывающим (CURLE_ABORTED_BY_CALLBACK)
    Other,     // прочие сетевые ошибки
};

struct RetryOptions {
    bool idempotent = true;      // GET/HEAD/... — повтор безопасен
    bool hasBody = false;        // POST с телом
    bool responseStarted = false; // сервер уже ответил (status != 0)
};

struct RetryDecision {
    bool retry = false;
    int baseDelayMs = 0;      // задержка до jitter
    const char* reason = "";  // для лога reason=
    bool honorRetryAfter = false;
};

// Решение о повторе. attempt — номер уже завершённой попытки (0-based).
RetryDecision decideRetry(NetFail fail, long status, int attempt, int maxAttempts,
                          const RetryOptions& opt);

// Jitter ±20%: base * [0.8, 1.2]. r01 — детерминированное значение [0,1)
// (в бою — из генератора, в тесте — фиксированное).
int jitteredDelayMs(int baseMs, double r01);

// Разбор заголовка Retry-After (секунды). Возвращает мс, но не больше 5 с
// (NET-08); 0 — заголовка нет/не распознан (HTTP-даты игнорируем).
long retryAfterMs(const std::string& value);

// --- Диагностика (NET-01) и редакция секретов (LOG-06/SEC-03) ---------------
// Убрать чувствительные заголовки (Cookie/Authorization/Proxy-Authorization).
Headers redactHeaders(const Headers& h);
// Экранировать тело для лога: первые maxBytes байт, непечатаемые -> \xNN,
// переводы строк -> \n. Безопасно для любой бинарной/UTF-8 строки.
std::string escapeForLog(const std::string& s, size_t maxBytes = 512);
// Скрыть пароль в URL прокси: socks5h://user:***@host:port.
// Inline: функция нужна и там, где http.cpp не линкуется (host-тесты DPI,
// offscreen-предпросмотр UI), а зависимостей у неё нет.
inline std::string redactProxy(const std::string& proxyUrl) {
    const size_t at = proxyUrl.rfind('@');
    if (at == std::string::npos) return proxyUrl;
    const size_t scheme = proxyUrl.find("://");
    const size_t credStart = scheme == std::string::npos ? 0 : scheme + 3;
    if (at <= credStart) return proxyUrl;
    const size_t colon = proxyUrl.find(':', credStart);
    if (colon != std::string::npos && colon < at) {
        return proxyUrl.substr(0, colon + 1) + "***" + proxyUrl.substr(at);
    }
    return proxyUrl.substr(0, credStart) + "***" + proxyUrl.substr(at);
}
// LOG-06: URL для лога — схема, хост и путь как есть, а query только именами
// параметров ("…/player?key,token"). Секреты (sig, pot, n, visitorData) и всё
// остальное содержимое параметров в лог не попадает; такая ссылка нерабочая.
std::string redactUrl(const std::string& url);
// Разбор "Name: value" (края обрезаются). false — строка не заголовок.
bool parseHeaderLine(const std::string& line, std::pair<std::string, std::string>& out);
// Сборка "Name: value".
std::string buildHeaderLine(const std::string& name, const std::string& value);
bool hasHeader(const Headers& h, const char* name);

// " +N в очереди" при qms >= 50, иначе пустая строка (NET-06). Буфер >= 48.
void formatQueueSuffix(char* buf, size_t size, long long qms);

// --- SNI-политика (DPI-07/DPI-08) -------------------------------------------
// Помощники вынесены сюда, чтобы их мог проверить host-тест: sni.cpp под
// Switch включает <switch.h> и на хосте не собирается. Логика используется
// как в net/sni.cpp, так и в tools/test_net.cpp — один источник истины.
//
// Строгий suffix-match: host == suffix ИЛИ host заканчивается на "." + suffix.
// Так "evilgooglevideo.com" НЕ считается поддоменом "googlevideo.com".
bool hostMatchesDomain(const char* host, const char* suffix);
// Имя для подмены SNI на видео-хостах (*.googlevideo.com), иначе nullptr.
const char* sniSpoofName(const char* host);
// Нужно ли поднимать регистр SNI для этого хоста (youtube.com/ytimg.com/...).
bool sniNeedsUpper(const char* host);

// attempts — сколько раз пробовать при «плавающих» сетевых ошибках
// (таймаут, обрыв). Для фоновых проб ставим 1, чтобы не ждать втрое дольше.
// cancel — необязательный токен отмены (NET-11); nullptr значит без отмены.
Response get(const std::string& url, const Headers& h = {},
             int timeoutMs = defaults::kRequestMs,
             size_t maxBytes = limits::kDefault, int attempts = 3,
             const Overrides* ov = nullptr, CancelToken* cancel = nullptr);
Response post(const std::string& url, const std::string& body, const Headers& h = {},
              int timeoutMs = defaults::kRequestMs,
              size_t maxBytes = limits::kDefault, int attempts = 3,
              const Overrides* ov = nullptr, CancelToken* cancel = nullptr);

// Скачивание в файл с прогрессом (для кэша).
bool fetchToFile(const std::string& url, const std::string& path,
                 const std::function<void(long long, long long)>& progress = nullptr);

std::string urlEncode(const std::string& s);
std::string urlDecode(const std::string& s);

} // namespace tnx::net
