// ui/strings.hpp — UX-13 (склонения и единый текст) и UX-15 (человеческие
// сообщения об ошибках вместо технических кодов curl/HTTP).
//
// Строки UI в одном месте; технические детали (коды, URL с токенами) остаются
// в логе/диагностике и не попадают на экран.
#pragma once

#include <string>

namespace tnx::app {

// ---------------- UX-13: русские склонения ----------------
// plural(1, "видео", "видео", "видео") → "1 видео"; (2,…) → "2 видео";
// (5,"минута","минуты","минут") → "5 минут".
inline std::string plural(int n, const char* one, const char* few, const char* many) {
    const int m10 = n % 10, m100 = n % 100;
    const char* w = many;
    if (m10 == 1 && m100 != 11)
        w = one;
    else if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14))
        w = few;
    return std::to_string(n) + " " + w;
}

// ---------------- UX-15: понятный текст ошибки ----------------
namespace detail {
inline bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
} // namespace detail

// Преобразует техническое сообщение (curl 28, HTTP 400, "no upstream headers",
// URL с токенами) в человеческую подсказку. Уже понятные сообщения проходят
// как есть; URL никогда не показывается пользователю.
inline std::string humanError(const std::string& raw) {
    if (raw.empty()) return raw;
    using detail::has;

    // Отмена (NET-11) не показывается как ошибка — экран уже сменён.
    if (has(raw, "отмен")) return "Загрузка отменена.";

    // Уже осмысленные сообщения приложения — не трогаем.
    if (has(raw, "Проверьте обход DPI") || has(raw, "обход DPI включён") ||
        has(raw, "Видео недоступно") || has(raw, "Нет доступных потоков") ||
        has(raw, "конец ленты") || has(raw, "уже идёт"))
        return raw;

    if (has(raw, "curl 28") || has(raw, "timed out") || has(raw, "elapsed") ||
        has(raw, "таймаут") || has(raw, "Timeout"))
        return "YouTube не отвечает. Проверьте соединение и обход DPI.";
    if (has(raw, "curl 6") || has(raw, "resolve"))
        return "Нет связи с YouTube. Проверьте интернет.";
    if (has(raw, "curl 7") || has(raw, "connect"))
        return "Не удалось подключиться. Проверьте обход DPI.";
    if (has(raw, "502") || has(raw, "upstream") || has(raw, "ретранслятор"))
        return "Ретранслятор обхода DPI вернул ошибку. Повторите.";
    if (has(raw, "410"))
        return "Ссылка на поток устарела. Обновляем…";
    if (has(raw, "403"))
        return "Доступ к потоку запрещён. Обновляем ссылку.";
    if (has(raw, "400"))
        return "YouTube отклонил запрос (400). Попробуйте обход DPI.";
    if (has(raw, "HTTP 5"))
        return "YouTube временно недоступен. Повторите позже.";
    if (has(raw, "некорректный JSON") || has(raw, "JSON"))
        return "YouTube вернул неожиданный ответ. Обновите обход DPI.";
    if (has(raw, "Обложка") || has(raw, "thumbnail") || has(raw, "ytimg"))
        return "Обложка недоступна.";

    // Прочее техническое (в т.ч. URL с токенами) — общий текст, детали в логе.
    if (has(raw, "http") || has(raw, "HTTP") || has(raw, "curl"))
        return "Сетевая ошибка. Подробности — в диагностике.";
    return raw;
}

} // namespace tnx::app
