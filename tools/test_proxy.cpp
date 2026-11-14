// tools/test_proxy.cpp — host-тест UX-09 (проверка строки внешнего прокси).
// Header-only модуль ui/proxy.hpp, линковки не требует.
#include "ui/proxy.hpp"

#include <cstdio>
#include <string>

using namespace tnx::app;

static int checks = 0, fails = 0;
static void check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        fails++;
        printf("   FAIL: %s\n", what);
    }
}

// Удобная обёртка: ожидаем валидность и (если валидно) нормализованный вид.
static bool valid(const std::string& in, const std::string& wantNorm) {
    std::string out, err;
    if (!validateProxy(in, out, err)) return false;
    return wantNorm.empty() || out == wantNorm;
}
static bool invalid(const std::string& in) {
    std::string out, err;
    if (validateProxy(in, out, err)) return false;
    return !err.empty();   // у отказа всегда есть понятная причина
}

int main() {
    // Пусто — валидное «отключить».
    check(valid("", ""), "пустая строка — отключение");
    check(valid("   ", ""), "пробелы — отключение");

    // Без схемы добавляется socks5h.
    check(valid("127.0.0.1:1080", "socks5h://127.0.0.1:1080"), "host:port → socks5h://");
    check(valid("proxy.local:9050", "socks5h://proxy.local:9050"), "доменное имя + порт");

    // Все поддерживаемые схемы.
    check(valid("socks5h://10.0.0.2:1080", "socks5h://10.0.0.2:1080"), "socks5h");
    check(valid("socks5://10.0.0.2:1080", "socks5://10.0.0.2:1080"), "socks5");
    check(valid("http://example.com:8080", "http://example.com:8080"), "http");
    check(valid("https://example.com:443", "https://example.com:443"), "https");

    // Обрезка пробелов.
    check(valid("  127.0.0.1:1080  ", "socks5h://127.0.0.1:1080"), "обрезка пробелов");

    // IPv6-литерал.
    check(valid("[::1]:1080", "socks5h://[::1]:1080"), "IPv6 [::1]:1080");
    check(valid("socks5://[2001:db8::1]:9050", "socks5://[2001:db8::1]:9050"), "IPv6 полный");

    // Отказы.
    check(invalid("foo://1.2.3.4:80"), "неизвестная схема");
    check(invalid("127.0.0.1"), "нет порта");
    check(invalid(":1080"), "пустой хост");
    check(invalid("127.0.0.1:"), "пустой порт");
    check(invalid("127.0.0.1:0"), "порт 0");
    check(invalid("127.0.0.1:65536"), "порт 65536");
    check(invalid("127.0.0.1:99999"), "порт слишком длинный");
    check(invalid("127.0.0.1:abc"), "порт не число");
    check(invalid("bad host:1080"), "пробел в адресе");
    check(invalid("ho/st:1080"), "слэш в адресе");
    check(invalid("[::1]"), "IPv6 без порта");
    check(invalid("[::1:1080"), "IPv6 без закрывающей скобки");

    // Границы порта проходят.
    check(valid("127.0.0.1:1", "socks5h://127.0.0.1:1"), "порт 1");
    check(valid("127.0.0.1:65535", "socks5h://127.0.0.1:65535"), "порт 65535");

    printf("\n== %s: %d проверок, %d провалов ==\n", fails ? "ЕСТЬ ОШИБКИ" : "ЗЕЛЕНО", checks,
           fails);
    return fails ? 1 : 0;
}