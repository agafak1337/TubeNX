// net/http.hpp — HTTPS-клиент на libcurl (mbedtls), с опциональным SOCKS5-обходом DPI.
#pragma once

#include <functional>
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

// Прокси, через который ходит весь трафик (наш локальный SOCKS5 с обходом DPI).
void setProxy(const std::string& url);   // "socks5h://127.0.0.1:1080" или "" — напрямую
std::string proxy();
void setCaBundlePath(const std::string& path);
void setUserAgent(const std::string& ua);

// Проверка TLS. Full — как в браузере (цепочка + имя хоста). ChainOnly —
// только цепочка: нужно, если сервис консоли не принимает имя хоста в
// верхнем регистре (наш обход SNI-блокировки). None — без проверок (диагностика).
enum class Verify { Full = 0, ChainOnly = 1, None = 2 };
void setVerify(Verify v);
Verify verify();

// attempts — сколько раз пробовать при «плавающих» сетевых ошибках
// (таймаут, обрыв). Для фоновых проб ставим 1, чтобы не ждать втрое дольше.
Response get(const std::string& url, const Headers& h = {}, int timeoutMs = 30000,
             size_t maxBytes = 16u << 20, int attempts = 3,
             const Overrides* ov = nullptr);
Response post(const std::string& url, const std::string& body, const Headers& h = {},
              int timeoutMs = 30000, size_t maxBytes = 16u << 20, int attempts = 3,
              const Overrides* ov = nullptr);

// Скачивание в файл с прогрессом (для кэша).
bool fetchToFile(const std::string& url, const std::string& path,
                 const std::function<void(long long, long long)>& progress = nullptr);

std::string urlEncode(const std::string& s);
std::string urlDecode(const std::string& s);

} // namespace tnx::net