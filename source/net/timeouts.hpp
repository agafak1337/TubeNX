// net/timeouts.hpp — именованные бюджеты времени и лимиты ответа (NET-12, NET-13).
//
// Зачем отдельный файл: до M2 числа 30000/25000/9000/12000/8000/20000 были
// разбросаны по вызовам net::get()/net::post(). Теперь у каждого класса
// запроса есть имя (bootstrap, api, images, probe, player), а «магических»
// чисел в вызовах нет.
//
// Значения можно переопределить в диагностике (env-переменные TNX_T_*) или
// тестом через net::setTimeout(). Переопределение допустимо только до старта
// запросов: см. net::setTimeout() в http.cpp.
#pragma once

#include <cstddef>

namespace tnx::net {

// Значения по умолчанию (мс). Классы — docs/impl-tz.md §7.2, refactor-tz §NET-12.
namespace defaults {
inline constexpr int kBootstrapMs = 10000;   // GET / — ключ и visitorData
inline constexpr int kApiMs = 20000;         // InnerTube browse/search/next/player
inline constexpr int kImagesMs = 8000;       // обложки
inline constexpr int kProbeMs = 9000;        // проверка доступности (generate_204)
inline constexpr int kPlayerMs = 12000;      // вспомогательные запросы плеера/relay

// Общий предел, если у URL нет своего класса.
inline constexpr int kRequestMs = 30000;
// Потолок/пол connect-таймаута: соединению отдаём не больше общего бюджета.
inline constexpr int kConnectCapMs = 25000;
inline constexpr int kConnectMinMs = 3000;
// Служебные значения libcurl (переиспользование/кэш соединений).
inline constexpr int kDnsCacheSec = 120;
inline constexpr int kMaxAgeConnSec = 120;
// Скачивание в файл (кэш): коннект, «мёртвая» скорость.
inline constexpr int kFileConnectMs = 20000;
inline constexpr int kFileLowSpeedBytes = 1;
inline constexpr int kFileLowSpeedSec = 60;
} // namespace defaults

// Жёсткие лимиты размера ответа (байт). Даже если вызывающий передал больше,
// эффективный лимит будет уменьшен (NET-13). Класс URL — как в budgetMsFor().
namespace limits {
inline constexpr size_t kBootstrap = 6u << 20; // GET /, sw.js_data
inline constexpr size_t kApi = 8u << 20;       // /youtubei/v1/browse|search|next
inline constexpr size_t kPlayer = 4u << 20;    // /youtubei/v1/player
inline constexpr size_t kImages = 8u << 20;    // обложки
inline constexpr size_t kDefault = 16u << 20;  // без класса
} // namespace limits

// Текущие (действующие) бюджеты времени. Читаются через timeouts().
struct NetTimeouts {
    int bootstrap = defaults::kBootstrapMs;
    int api = defaults::kApiMs;
    int images = defaults::kImagesMs;
    int probe = defaults::kProbeMs;
    int player = defaults::kPlayerMs;
};

// Единственный экземпляр на процесс (inline-функция с локальным static).
inline NetTimeouts& mutableTimeouts() {
    static NetTimeouts t;
    return t;
}
inline const NetTimeouts& timeouts() { return mutableTimeouts(); }

// Переопределить одно значение по имени: "bootstrap", "api", "images",
// "probe", "player". false — неизвестное имя или значение <= 0.
// Для диагностики/тестов; менять во время работы с запросами нельзя.
bool setTimeout(const char* name, int ms);

} // namespace tnx::net
