// ui/proxy.hpp — разбор и проверка строки внешнего прокси (UX-09).
//
// Вынесено из app.cpp отдельным header-only модулем, чтобы валидацию можно
// было прогнать host-тестом без линковки всего приложения. Функции чистые:
// ничего не пишут в конфиг и не трогают транспорт — это делает App::setProxy.
#pragma once

#include <string>

namespace tnx::app {

// Приводим строку прокси к виду scheme://host:port (пустая — отключить).
// Без схемы считаем SOCKS5 с разрешением имён на стороне прокси (socks5h).
inline std::string normalizeProxy(std::string p) {
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
    while (!p.empty() && (p.back() == ' ' || p.back() == '\t' || p.back() == '\n' || p.back() == '\r'))
        p.pop_back();
    if (p.empty()) return p;
    if (p.find("://") == std::string::npos) p = "socks5h://" + p;
    return p;
}

// UX-09: проверка строки прокси. Пустая — валидное «отключить». Иначе схема
// socks5h/socks5/http/https, непустой хост и порт 1..65535. `out` —
// нормализованная строка, `err` — понятная причина отказа.
inline bool validateProxy(const std::string& raw, std::string& out, std::string& err) {
    out.clear();
    err.clear();
    std::string p = normalizeProxy(raw);
    if (p.empty()) return true;   // отключение внешнего прокси

    const size_t sep = p.find("://");
    if (sep == std::string::npos) {
        err = "Нужен адрес вида host:port.";
        return false;
    }
    const std::string scheme = p.substr(0, sep);
    if (scheme != "socks5h" && scheme != "socks5" && scheme != "http" && scheme != "https") {
        err = "Неизвестная схема «" + scheme + "». Поддерживаются socks5h, socks5, http, https.";
        return false;
    }

    std::string rest = p.substr(sep + 3);
    if (rest.empty()) {
        err = "Укажите адрес в виде host:port.";
        return false;
    }

    std::string host, portStr;
    if (rest.front() == '[') {   // IPv6-литерал: [addr]:port
        const size_t close = rest.find(']');
        if (close == std::string::npos) {
            err = "Не закрыта скобка в адресе IPv6.";
            return false;
        }
        host = rest.substr(1, close - 1);
        const std::string tail = rest.substr(close + 1);
        if (tail.size() < 2 || tail.front() != ':') {
            err = "После IPv6-адреса нужен порт: [адрес]:порт.";
            return false;
        }
        portStr = tail.substr(1);
    } else {
        const size_t colon = rest.rfind(':');
        if (colon == std::string::npos) {
            err = "Не указан порт. Нужно host:port.";
            return false;
        }
        host = rest.substr(0, colon);
        portStr = rest.substr(colon + 1);
    }

    if (host.empty()) {
        err = "Пустой адрес прокси.";
        return false;
    }
    for (char ch : host) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_' || ch == ':';
        if (!ok) {
            err = std::string("Недопустимый символ в адресе: «") + ch + "».";
            return false;
        }
    }
    if (portStr.empty() || portStr.size() > 5) {
        err = "Порт должен быть числом 1..65535.";
        return false;
    }
    int port = 0;
    for (char ch : portStr) {
        if (ch < '0' || ch > '9') {
            err = "Порт должен быть числом 1..65535.";
            return false;
        }
        port = port * 10 + (ch - '0');
    }
    if (port < 1 || port > 65535) {
        err = "Порт вне диапазона 1..65535.";
        return false;
    }
    out = p;
    return true;
}

} // namespace tnx::app