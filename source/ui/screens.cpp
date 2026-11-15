// ui/screens.cpp — отрисовка экранов в стиле Material 3 (Expressive).
// Геометрия — по docs/impl-tz.md §5; dock/handheld различия читаются из
// ScreenLayout (core/layout.hpp), внутри экранов нет проверок «docked».
#include "ui/app.hpp"

#include "core/canvas.hpp"
#include "core/layout.hpp"
#include "dpi/dpi.hpp"
#include "md3/icons.hpp"
#include "md3/widgets.hpp"
#include "net/http.hpp"
#include "platform.hpp"
#include "ui/strings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tnx::app {

namespace {

constexpr float kW = 1280.f;
constexpr float kH = 720.f;
// Резерв снизу под снэкбар (y=636…684) — сохраняется и без нижней навигации.
constexpr float kBottomReserve = 88.f;

std::string timeFmt(double t) {
    if (!(t > 0.0)) t = 0.0;
    int s = (int)t;
    int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

// Палитра seed-цветов — docs/impl-tz.md §5.2.
struct SeedColor {
    uint32_t argb;
    const char* name;
};
const SeedColor kSeeds[] = {
    {0xFFC3002F, "Красный"},  {0xFF1A73E8, "Синий"},      {0xFF1E8E3E, "Зелёный"},
    {0xFF7E57C2, "Фиолетовый"}, {0xFFE8710A, "Оранжевый"}, {0xFFD81B60, "Розовый"},
};
constexpr int kSeedCount = 6;

// Состояние открытых меню и диалогов экранов (в md3/ui нет хранения
// выпадающих поверхностей — держим его локально в screens.cpp).
float g_continueScroll = 0.f;
bool g_setDialog = false;
bool g_licDialog = false;
bool g_resetDialog = false;   // CFG-07: подтверждение сброса настроек

} // namespace

// ---------------------------------------------------------------- helpers
void App::drawGrid(ui::Ui& u, uint64_t id, const Rect& area, const yt::Feed& f, bool loading,
                   const std::string& err, const char* emptyText, float scroll, float contentH) {
    auto& th = u.theme();
    auto& col = th.colors;
    auto& c = u.canvas();
    auto m = metricsFor(area.w);
    (void)contentH;
    (void)scroll;

    if (!err.empty()) {
        // Карточка ошибки: 560 по центру, errorContainer/onErrorContainer (§5.5).
        float cx = area.x + area.w * 0.5f;
        Rect card{cx - 280.f, area.y + 148.f, 560.f, 268.f};
        c.fillRoundRect(card, 28.f, col.errorContainer);
        md3::iconCentered(u, "cloud_off", Rect(card.x + 24, card.y + 24, 32, 32), 32, col.error);
        const auto& ts = th.type.titleLarge;
        u.fonts().draw((int)ts.font, ts.size, card.x + 68, card.y + 24, "Не удалось загрузить ленту",
                       col.onErrorContainer, ts.letterSpacing);
        const auto& bs = th.type.bodyMedium;
        const std::string msg = humanError(err);
        u.fonts().drawWrapped((int)bs.font, bs.size, Rect(card.x + 24, card.y + 76, card.w - 48, 60), msg,
                              col.onErrorContainer.withAlpha(0.9f), 2, bs.letterSpacing);

        // Что именно включено — чтобы не гадать по надписи «прямое соединение».
        {
            bool proxy = !net::proxy().empty();
            char diag[192];
            snprintf(diag, sizeof(diag), "SNI-обход: вкл  •  %s",
                     proxy ? net::redactProxy(net::proxy()).c_str()
                           : "прямое соединение");
            u.fonts().draw(0, 12, card.x + 24, card.y + 150, diag, col.onErrorContainer.withAlpha(0.75f));
        }

        float by = card.b() - 64.f;
        if (!u.hasFocus()) u.focus(id ^ 0x51);
        if (md3::button(u, id ^ 0x51, Rect(card.r() - 24.f - 200.f, by, 200.f, 44.f), "Повторить",
                        {md3::ButtonKind::Filled})) {
            toast("Повторная загрузка…");
            loadHome(homeIsTrending);
        }
        if (md3::button(u, id ^ 0x52, Rect(card.r() - 24.f - 200.f - 8.f - 180.f, by, 180.f, 44.f),
                        "Настройки", {md3::ButtonKind::Text}))
            go(Screen::Settings);
        return;
    }

    auto items = flatItems(f);
    if (items.empty() && !loading) {
        float cx = area.x + area.w * 0.5f;
        md3::iconCentered(u, "search", Rect(cx - 32.f, area.y + 120.f, 64.f, 64.f), 64.f, col.outline);
        const auto& hs = th.type.headlineSmall;
        u.fonts().drawCentered((int)hs.font, hs.size, Rect(area.x, area.y + 200.f, area.w, 36.f),
                               emptyText ? emptyText : "Пока ничего нет", col.onSurface, hs.letterSpacing);
        return;
    }

    int cols = std::max(1, m.cols);
    // Отсекаем карточки вне экрана, чтобы не тратить время на нижние ряды.
    Rect view = u.canvas().clip();
    for (size_t i = 0; i < items.size(); i++) {
        int row = (int)i / cols, colIdx = (int)i % cols;
        Rect card{area.x + colIdx * (m.cardW + m.gap), area.y + row * (m.cardH + m.rowGap), m.cardW, m.cardH};
        if (card.b() < view.y - 40.f || card.y > view.b() + 40.f) continue;

        const yt::VideoItem& v = *items[i];
        md3::VideoTileData d;
        d.thumb = images.get(v.thumbnailUrl);
        if (!d.thumb) d.thumbPlaceholder = images.placeholder(v.thumbnailUrl) || images.failed(v.thumbnailUrl);
        d.title = v.title;
        d.channel = v.channel;
        d.meta = v.meta;
        d.duration = v.live ? "LIVE" : v.duration;
        d.live = v.live;
        if (md3::videoTile(u, id ^ (uint64_t)(i + 1) * 1099511628211ull, card, d)) openVideo(v);
    }

    if (loading) {
        Rect box{area.x, area.y + (float)((items.size() + cols - 1) / cols) * (m.cardH + m.rowGap), area.w, 80};
        md3::circularProgress(u, {box.center().x, box.y + 40}, 14.f, -1.f, 3.f);
    } else if (!items.empty() && f.continuation.empty()) {
        // UX-03: явный конец ленты — иначе непонятно, кончилось или зависло.
        Rect box{area.x, area.y + (float)((items.size() + cols - 1) / cols) * (m.cardH + m.rowGap),
                 area.w, 56};
        const auto& ls = th.type.bodyMedium;
        u.fonts().drawCentered((int)ls.font, ls.size, box, "Больше нет",
                               col.onSurfaceVariant.withAlpha(0.7f), ls.letterSpacing);
    }
}

// ---------------------------------------------------------------- Continue strip
void App::drawContinueStrip(ui::Ui& u, const Rect& r, const std::vector<const HistoryEntry*>& items) {
    auto& col = u.theme().colors;
    if (items.empty()) return;

    const auto& ts = u.theme().type.titleSmall;
    u.fonts().draw((int)ts.font, ts.size, r.x, r.y, "Продолжить просмотр", col.onSurface, ts.letterSpacing);

    // Карточка фиксирована: 240×135, название поверх обложки (videoTileCompact).
    const int n = (int)std::min<size_t>(items.size(), 12);
    const float gap = 12.f, cardW = 240.f, cardH = 135.f;
    const float top = r.y + 28.f;

    float total = n * cardW + (n - 1) * gap;
    float maxOff = std::max(0.f, total - r.w);
    // Горизонтальный сдвиг полосы — от фокуса (Ui не умеет горизонтальный скролл).
    for (int i = 0; i < n; i++) {
        if (u.isFocused(u.id("home.cont", i))) {
            float px = i * (cardW + gap);
            if (px < g_continueScroll) g_continueScroll = px;
            else if (px + cardW > g_continueScroll + r.w) g_continueScroll = px + cardW - r.w;
        }
    }
    g_continueScroll = std::clamp(g_continueScroll, 0.f, maxOff);

    u.canvas().pushClip(Rect(r.x, top - 6.f, r.w, cardH + 12.f));
    for (int i = 0; i < n; i++) {
        const HistoryEntry* e = items[(size_t)i];
        Rect card{r.x - g_continueScroll + i * (cardW + gap), top, cardW, cardH};

        md3::VideoTileData d;
        d.thumb = images.get(yt::thumbUrl(e->id));
        if (!d.thumb) d.thumbPlaceholder = images.placeholder(yt::thumbUrl(e->id)) || images.failed(yt::thumbUrl(e->id));
        d.title = e->title;
        if (e->duration > 0.0 && e->time > 0.0) {
            int left = (int)std::max(0.0, e->duration - e->time);
            char buf[64];
            if (left >= 3600) snprintf(buf, sizeof(buf), "Осталось %d ч %d мин", left / 3600, (left % 3600) / 60);
            else snprintf(buf, sizeof(buf), "Осталось %d мин", std::max(1, left / 60));
            d.meta = buf;
        } else {
            d.meta = e->channel;
        }
        d.watchProgress = e->duration > 0.0 ? (float)(e->time / e->duration) : -1.f;

        const uint64_t tid = u.id("home.cont", i);
        const uint64_t rid = u.id("home.cont.rm", i);
        const bool tileFocused = u.isFocused(tid);
        const bool rmFocused = u.isFocused(rid);
        const bool opened = md3::videoTileCompact(u, tid, card, d);
        // UX-10: «×» убрать запись — видна у карточки в фокусе (на геймпаде:
        // фокус на «×» или на самой карточке). Убрать не открывает видео.
        if (tileFocused || rmFocused || opened) {
            Rect xr{card.r() - 30.f, card.y + 6.f, 26.f, 26.f};
            u.canvas().fillRoundRect(xr, 13.f, Color::rgb(0x000000).withAlpha(0.55f));
            if (md3::iconButton(u, rid, xr, "close", Color::rgb(0xFFFFFF), 18.f, false, nullptr, true)) {
                removeHistory(e->id);
                return;
            }
        }
        if (opened) {
            yt::VideoItem v;
            v.id = e->id;
            v.title = e->title;
            v.channel = e->channel;
            openVideo(v);
            return;
        }
    }
    u.canvas().popClip();
}

// ---------------------------------------------------------------- Splash
void App::drawSplash(ui::Ui& u) {
    auto& c = u.canvas();
    auto& col = u.theme().colors;

    // Фон: глубокая поверхность + мягкий «aurora»-ореол вокруг знака (M3 Expressive).
    // Градиенты считаются один раз в Image и дальше копируются как есть: иначе
    // даже сплэш занимал ~160 мс на кадр (6 fps) и мешал сетевому потоку.
    if (!splashBgReady) {
        splashBg.w = kW;
        splashBg.h = kH;
        splashBg.px.assign(static_cast<size_t>(kW) * static_cast<size_t>(kH), 0u);
        Canvas bg;
        bg.attach(splashBg.px.data(), kW, kH, kW);
        bg.fillLinearGradient({0, 0, kW, kH}, CornerRadii(0), col.surfaceContainerLowest, col.surface,
                              false);
        bg.fillRadialGradient({0, 0, kW, kH}, {kW * 0.5f, 260.f}, 620.f, col.primary.withAlpha(0.17f),
                              col.primary.withAlpha(0.f));
        bg.fillRadialGradient({0, 0, kW, kH}, {kW * 0.12f, kH * 0.95f}, 620.f,
                              col.secondary.withAlpha(0.10f), col.secondary.withAlpha(0.f));
        splashBgReady = true;
    }
    c.image(splashBg, Rect(0, 0, kW, kH), 1.f, CornerRadii(0));

    // Марка 96×96: scale 0.92→1.0 за 400 мс.
    float appear = std::min(1.f, splashT / 0.4f);
    float scale = 0.92f + 0.08f * (1.f - (1.f - appear) * (1.f - appear));
    float ms = 96.f * scale;
    Rect mark{kW * 0.5f - ms * 0.5f, 200.f - (ms - 96.f) * 0.5f, ms, ms};
    c.shadow(mark, CornerRadii(30.f * scale), 28, 12, col.shadow.withAlpha(0.45f));
    // Марка: squircle primary + треугольник onPrimary (design .mark.xl,
    // компоненты.css 146) — в тёмной теме это розовый квадрат, как в макете.
    c.fillRoundRect(mark, 30.f * scale, col.primary);
    md3::iconCentered(u, "play_arrow", mark, 64.f * scale, col.onPrimary);

    float textA = std::min(1.f, splashT / 0.25f);
    const auto& ds = u.theme().type.displaySmall;
    u.fonts().drawCentered((int)ds.font, ds.size, Rect(0, 320, kW, 48), "TubeNX",
                           col.onSurface.fade(textA), ds.letterSpacing);
    const auto& bm = u.theme().type.bodyMedium;
    u.fonts().drawCentered((int)bm.font, bm.size, Rect(0, 372, kW, 26), "YouTube для Nintendo Switch",
                           col.onSurfaceVariant.fade(textA), bm.letterSpacing);

    // UX-14: первый запуск — короткий онбординг прямо на сплэше. Сеть не
    // блокирует: «Продолжить» доступно сразу, проверка и прокси — опционально.
    if (cfg.firstRun) {
        // Фокус по умолчанию — на «Продолжить»: онбординг проходится одним A.
        if (!u.hasFocus()) u.focus(u.id("splash.start"));
        Rect card{kW * 0.5f - 300.f, 404.f, 600.f, 208.f};
        c.shadow(card, CornerRadii(24.f), 16, 3, col.shadow.withAlpha(0.35f));
        c.fillRoundRect(card, 24.f, col.surfaceContainerHigh);
        const float px = card.x + 24.f;
        float cy = card.y + 22.f;

        // Строка 1: состояние обхода DPI.
        md3::iconCentered(u, cfg.dpiEnabled ? "gpp_good" : "wifi_off", Rect(px, cy, 24, 24), 22,
                          cfg.dpiEnabled ? col.tertiary : col.error);
        u.fonts().draw(1, 16, px + 34.f, cy,
                       cfg.dpiEnabled ? "Обход DPI включён" : "Обход DPI выключен", col.onSurface);
        cy += 30.f;

        // Строка 2: результат проверки соединения (может ещё идти).
        std::string st;
        Color stc = col.onSurfaceVariant;
        if (!bootDone) {
            st = "Проверяем соединение…";
        } else if (probeOk) {
            st = probeMs > 0 ? ("Соединение есть • YouTube " + std::to_string(probeMs) + " мс")
                             : std::string("Соединение есть");
            stc = col.tertiary;
        } else {
            st = "Сеть недоступна — можно настроить позже";
            stc = col.error;
        }
        u.fonts().drawEllipsized(0, 14, px + 34.f, cy, card.w - 58.f, st, stc);
        cy += 26.f;
        u.fonts().drawEllipsized(0, 12, px, cy, card.w - 48.f,
                                 "Сеть не блокирует: можно продолжить сразу.",
                                 col.onSurfaceVariant.withAlpha(0.8f));

        // Кнопки: проверка (tonal), прокси (text), «Продолжить» (filled).
        const float by = card.y + card.h - 24.f - 40.f;
        if (md3::button(u, u.id("splash.probe"), Rect(px, by, 216.f, 40.f), "Проверить соединение",
                        {md3::ButtonKind::Tonal, true, "bolt"}))
            retryBoot();
        if (md3::button(u, u.id("splash.proxy"), Rect(px + 224.f, by, 156.f, 40.f), "Указать прокси",
                        {md3::ButtonKind::Text, true})) {
            std::string pval = cfg.proxy;
            if (platform::keyboard(pval, "Прокси: host:port или socks5h://host:port",
                                   "Внешний прокси (пусто — отключить)")) {
                std::string perr;
                if (!setProxy(pval, perr)) toast(perr);
            }
        }
        if (md3::button(u, u.id("splash.start"), Rect(card.r() - 24.f - 148.f, by, 148.f, 40.f),
                        "Продолжить", {md3::ButtonKind::Filled})) {
            cfg.firstRun = false;
            saveConfig();
            platform::bootLog("UX-14: онбординг пройден");
            go(Screen::Home);
            return;
        }
    } else {
        // Индикатор состояния (§5.4): спиннер 22 + текст, ниже подписи на 48.
        float y = 444.f;
        // Офлайн (§5.4) — проба сети не прошла. Если сеть есть, а лента не
        // загрузилась, сплэш уходит на Home с карточкой ошибки (там есть
        // «Настройки» и «Повторить»), а не запирает на этом экране.
        bool offline = bootDone && !feedOk && !probeOk;
        if (!bootDone) {
            md3::circularProgress(u, {kW * 0.5f - 100, y + 11}, 11.f, -1.f, 2.5f);
            u.fonts().draw(0, 16, kW * 0.5f - 78, y, "Проверяем соединение…",
                           col.onSurfaceVariant.fade(textA));
        } else if (feedOk) {
            md3::iconCentered(u, "check", Rect(kW * 0.5f - 96, y + 2, 20, 20), 20, col.tertiary);
            u.fonts().draw(0, 16, kW * 0.5f - 68, y, "Готово", col.onSurfaceVariant.fade(textA));
        } else {
            // Офлайн (splash-offline): wifi_off + «Нет сети» в роли error.
            md3::iconCentered(u, "wifi_off", Rect(kW * 0.5f - 88, y + 2, 20, 20), 20, col.error);
            u.fonts().draw(0, 16, kW * 0.5f - 60, y, "Нет сети", col.error.fade(textA));
        }

        if (offline) {
            // Офлайн: кнопки «Настроить» (tonal) → Settings и «Повторить» (text)
            // повторяют проверку соединения (§5.4).
            const float bh = 40.f, by = 520.f, gap = 12.f;
            const auto& ls = u.theme().type.labelLarge;
            float w1 = std::max(112.f, u.fonts().measure((int)ls.font, ls.size, "Настроить", ls.letterSpacing) + 48.f);
            float w2 = std::max(112.f, u.fonts().measure((int)ls.font, ls.size, "Повторить", ls.letterSpacing) + 48.f);
            float x0 = kW * 0.5f - (w1 + gap + w2) * 0.5f;
            if (md3::button(u, u.id("splash.settings"), Rect(x0, by, w1, bh), "Настроить",
                            {md3::ButtonKind::Tonal}))
                go(Screen::Settings);
            if (md3::button(u, u.id("splash.retry"), Rect(x0 + w1 + gap, by, w2, bh), "Повторить",
                            {md3::ButtonKind::Text}))
                retryBoot();
        } else {
            // Статус обхода DPI: чип с точкой роли tertiary (как точка сети в Home).
            Rect chip{kW * 0.5f - 210, 520, 420, 56};
            c.fillRoundRect(chip, 28, col.surfaceContainerHigh);
            Color dot = probeMs > 0 ? col.tertiary : (probeOk ? col.primary : col.error);
            c.fillCircle(chip.x + 28, chip.center().y, 5, dot);
            char buf[160];
            if (probeMs > 0)
                snprintf(buf, sizeof(buf), "ByeByeDPI активен • YouTube %d мс", probeMs);
            else if (probeOk)
                snprintf(buf, sizeof(buf), "ByeByeDPI активен • соединение есть");
            else
                snprintf(buf, sizeof(buf), "Проверка соединения…");
            u.fonts().draw(0, 14, chip.x + 44, chip.y + 18, buf, col.onSurfaceVariant);
        }
    }

    // Линия прогресса загрузки
    Rect bar{0, kH - 4, kW, 4};
    c.fillRect(bar, col.surfaceContainerHigh);
    float p = bootDone ? (feedOk ? 1.f : 0.7f) : std::min(0.65f, splashT * 0.4f);
    c.fillRect({0, bar.y, kW * p, 4}, col.primary);

    u.fonts().draw(0, 12, 24, kH - 32, "Material 3 Expressive • Google Sans",
                   col.onSurfaceVariant.withAlpha(0.7f));
    u.fonts().drawEllipsized(0, 12, kW - 24 - 300, kH - 32, 300, "v1.0 • devkitA64 / libnx",
                             col.onSurfaceVariant.withAlpha(0.7f));
}

// ---------------------------------------------------------------- Home
void App::drawHome(ui::Ui& u) {
    auto& c = u.canvas();
    auto& th = u.theme();
    auto& col = th.colors;
    c.fillRect({0, 0, kW, kH}, col.surface);

    auto L = tnx::ScreenLayout::forCanvas(c);
    const float gw = kW - 2.f * L.margin;   // ширина сетки/полосы

    // ---- верхняя панель (0…72) ----
    Rect tb{0, 0, kW, 72};
    c.fillRect(tb, col.surface);
    Rect mark{24, 20, 32, 32};
    c.fillRoundRect(mark, 10, col.primary);
    md3::iconCentered(u, "play_arrow", mark, 22, col.onPrimary);
    const auto& tl = th.type.titleLarge;
    u.fonts().draw((int)tl.font, tl.size, 68, 22, "TubeNX", col.onSurface, tl.letterSpacing);

    if (md3::iconButton(u, u.id("home.search"), Rect(kW - 116, 12, 48, 48), "search", col.onSurfaceVariant))
        go(Screen::Search);
    if (md3::iconButton(u, u.id("home.settings"), Rect(kW - 64, 12, 48, 48), "settings", col.onSurfaceVariant))
        go(Screen::Settings);
    if (probeMs > 0) c.fillCircle(kW - 122, 36, 3.5f, col.tertiary);
    if (u.input().x) go(Screen::Search);

    // ---- чипы (y=76, высота 32, слева 24) ----
    std::vector<bool> sel(chips.size(), false);
    if (!chips.empty()) sel[(size_t)std::clamp(homeChip, 0, (int)chips.size() - 1)] = true;
    Rect chipRowRect{L.margin, 76, gw, 48};
    int clicked = md3::chipRow(u, u.id("home.chips"), chipRowRect, chips, sel, &homeChip, 32.f);
    if (clicked >= 0 && clicked != homeChip) {
        homeChip = clicked;
        cfg.homeChip = clicked;   // UX-01: выбор чипа переживает перезапуск
        saveConfig();
        if (clicked == 0) loadHome(false);
        else if (clicked == 1) loadHome(true);
        else {
            // Честные подписи «Поиск: …» — ищем по слову без префикса.
            std::string q = chips[(size_t)clicked];
            const std::string pfx = "Поиск: ";
            if (q.rfind(pfx, 0) == 0) q = q.substr(pfx.size());
            startSearch(q);
        }
    }

    // ---- «Продолжить просмотр» + лента ----
    auto cont = continueWatching();
    // Вариант A (как в макете home-default): заголовок 20 + 8 + карточка 135,
    // плюс запас = 187; сетка начинается с y=323.
    const float stripH = cont.empty() ? 0.f : 187.f;

    Rect vp{0, 124, kW, kH - 124 - kBottomReserve};   // 124…632
    float gridH = gridContentHeight(feed, gw);
    float contentH = 12.f + stripH + gridH + 24.f;
    u.pushScroll(u.id("home.scroll"));
    bool nearEnd = false;
    float off = u.scrollArea(u.id("home.scroll"), vp, contentH, &nearEnd);
    c.pushClip(vp);
    float top = vp.y - off + 12.f;
    if (stripH > 0.f) drawContinueStrip(u, Rect{L.margin, top, gw, stripH}, cont);
    drawGrid(u, u.id("home.grid"), Rect{L.margin, top + stripH, gw, gridH}, feed,
             feedLoading && feed.sections.empty(), feedError, "Пока ничего нет", off, contentH);
    c.popClip();
    u.popScroll();
    if (nearEnd && feedCont.size() && !feedLoading) loadMore();
}

// ---------------------------------------------------------------- Search
void App::drawSearch(ui::Ui& u) {
    auto& c = u.canvas();
    auto& th = u.theme();
    auto& col = th.colors;
    c.fillRect({0, 0, kW, kH}, col.surface);

    auto L = tnx::ScreenLayout::forCanvas(c);
    const float gw = kW - 2.f * L.margin;

    if (md3::iconButton(u, u.id("search.back"), Rect(18, 12, 48, 48), "arrow_back", col.onSurface)) back();

    // Поле поиска: 52, r26, x=76…1256.
    Rect field{76, 14, kW - 76 - 24, 52};
    md3::SearchBarOpts sbo;
    if (!query.empty()) sbo.trailingIcon = "close";
    md3::SearchBarResult res = md3::searchBar(u, u.id("search.field"), field, query, "Поиск на YouTube", sbo);
    if (res.trailing) {
        query.clear();
        showSuggestions = false;
        searchFeed = yt::Feed{};
    } else if (res.activated) {
        showSuggestions = false;
        std::string q = query;
        if (platform::keyboard(q, "Например: lo-fi hip hop", "Поиск на YouTube") && !q.empty()) {
            query = q;
            startSearch(q);
        }
    }

    // Подсказки: карточка x=76, y=66, ширина 1180, до 6 строк по 48 (§5.6).
    if (showSuggestions && !suggestions.empty()) {
        int cnt = std::min(6, (int)suggestions.size());
        Rect list{76, 66, 1180, cnt * 48.f + 16.f};
        c.shadow(list, CornerRadii(16), 10, 2, col.shadow.withAlpha(0.32f));
        c.fillRoundRect(list, 16, col.surfaceContainer);
        for (int i = 0; i < cnt; i++) {
            Rect row{list.x, list.y + 8 + (float)i * 48.f, list.w, 48.f};
            if (u.interact(u.id("sugg", i), row)) {
                query = suggestions[(size_t)i];
                startSearch(query);
                break;
            }
            md3::iconCentered(u, "history", Rect(row.x + 16, row.y + 12, 24, 24), 22, col.onSurfaceVariant);
            u.fonts().drawEllipsized(0, 16, row.x + 56, row.y + 12, row.w - 112, suggestions[(size_t)i],
                                     col.onSurface);
            md3::iconCentered(u, "arrow_forward", Rect(row.r() - 40, row.y + 12, 24, 24), 22,
                              col.onSurfaceVariant);
        }
        return;
    }

    if (lastQuery.empty()) {
        // Пустое состояние: иконка search 64 outline, центр y=200 (§5.6).
        md3::iconCentered(u, "search", Rect(kW * 0.5f - 32, 168, 64, 64), 64, col.outline);
        const auto& hs = th.type.headlineSmall;
        u.fonts().drawCentered((int)hs.font, hs.size, Rect(0, 248, kW, 36), "Найдите что посмотреть",
                               col.onSurface, hs.letterSpacing);
        const auto& bm = th.type.bodyMedium;
        u.fonts().drawCentered((int)bm.font, bm.size, Rect(0, 288, kW, 24),
                               "Нажмите на поле — откроется клавиатура Switch", col.onSurfaceVariant,
                               bm.letterSpacing);

        // «Недавние»: колонка 720 по центру (x=280), y=344 (§5.6).
        float y = 344.f;
        if (!history.empty()) {
            const auto& sts = th.type.titleSmall;
            u.fonts().draw((int)sts.font, sts.size, 296, y, "Недавние", col.onSurface, sts.letterSpacing);
            y += 28.f;
            for (size_t i = 0; i < history.size() && i < 4; i++) {
                Rect row{280, y, 720, 64};
                md3::iconCentered(u, "history", Rect(row.x + 16, row.y + 20, 24, 24), 24, col.onSurfaceVariant);
                u.fonts().drawEllipsized(0, 16, row.x + 56, row.y + 10, row.w - 72, history[i].title,
                                         col.onSurface);
                u.fonts().drawEllipsized(0, 12, row.x + 56, row.y + 36, row.w - 72, history[i].channel,
                                         col.onSurfaceVariant);
                if (u.interact(u.id("hist", (int)i), row)) {
                    yt::VideoItem v;
                    v.id = history[i].id;
                    v.title = history[i].title;
                    v.channel = history[i].channel;
                    openVideo(v);
                    return;
                }
                y += 64;
            }
        }
        return;
    }

    // Результаты: сетка Home, y=88.
    Rect vp{0, 88, kW, kH - 88 - kBottomReserve};
    float contentH = gridContentHeight(searchFeed, gw);
    u.pushScroll(u.id("search.scroll"));
    float off = u.scrollArea(u.id("search.scroll"), vp, contentH, nullptr);
    c.pushClip(vp);
    drawGrid(u, u.id("search.grid"), Rect{L.margin, vp.y - off, gw, contentH}, searchFeed, searchLoading,
             searchError, "Ничего не найдено", off, contentH);
    c.popClip();
    u.popScroll();
}

// ---------------------------------------------------------------- Player
void App::drawPlayer(ui::Ui& u) {
    auto& c = u.canvas();
    auto& th = u.theme();
    auto& col = th.colors;
    auto& p = player::instance();

    c.fillRect({0, 0, kW, kH}, Color::rgb(0x000000));
    drawVideo(u, {0, 0, kW, kH});

    bool ended = p.status().eof && !pl.loading;
    bool show = pl.controls;
    // UX-05: сообщаем update(), что открыто меню/диалог — контролы не прятать.
    pl.menusOpen = pl.qualityMenuOpen || pl.moreMenuOpen || pl.closeDialogOpen;

    // UX-06: относительная перемотка с удержанием цели (прогресс не откатывается).
    auto relSeek = [&](double d) {
        double t = std::max(0.0, p.position() + d);
        const double d2 = p.duration();
        if (d2 > 0.0) t = std::min(t, d2);
        p.seekRelative(d);
        pl.seekTarget = t;
        pl.seekHoldT = 0.f;
    };
    bool hasRelated = ended && show && !pl.details.related.empty();
    // Конец + «Похожие»: кадр затемняется на 55 % (макет player-ended-related),
    // иначе белые подписи карточек не читаются на светлом видео. Рисуем до
    // контролов, чтобы затемнить только видео.
    if (hasRelated) c.fillRect({0, 0, kW, kH}, Color(0, 0, 0).withAlpha(0.55f));

    bool paused = p.paused() || !p.playing();

    // Загрузка
    if (pl.loading || (!plFrame.valid() && pl.error.empty())) {
        md3::circularProgress(u, {kW * 0.5f, 349.f}, 22.f, -1.f, 3.f);
        const auto& bm = th.type.bodyMedium;
        u.fonts().drawCentered((int)bm.font, bm.size, Rect(0, 381, kW, 24), "Получаем потоки…",
                               Color::rgb(0xFFFFFF), bm.letterSpacing);
    }

    // Ошибка: карточка 640×260 в (320, 230), r28.
    if (!pl.error.empty()) {
        // YT-05: по playabilityStatus даём понятный заголовок и действие;
        // для «нужен вход/возраст» повтор бессмысленен — только «Назад».
        const std::string& st = pl.details.playabilityStatus;
        std::string title = "Не удалось воспроизвести";
        const char* icon = "error";
        bool retryable = true;
        auto startsWith = [](const std::string& s, const char* p) {
            return s.rfind(p, 0) == 0;
        };
        if (st == "LOGIN_REQUIRED" || startsWith(st, "AGE_") || st == "CONTENT_CHECK_REQUIRED") {
            title = "Требуется вход";
            icon = "lock";
            retryable = false;
        } else if (startsWith(st, "GEO_BLOCK")) {
            title = "Заблокировано в регионе";
            icon = "lock";
            retryable = false;
        } else if (st == "LIVE_STREAM_OFFLINE") {
            title = "Эфир не идёт";
            icon = "schedule";
            retryable = false;
        }
        Rect card{320, 230, 640, 260};
        c.fillRoundRect(card, 28, col.surfaceContainerHighest);
        md3::iconCentered(u, icon, Rect(card.x + 24, card.y + 24, 32, 32), 32, col.error);
        const auto& tl = th.type.titleLarge;
        u.fonts().draw((int)tl.font, tl.size, card.x + 68, card.y + 24, title, col.onSurface,
                       tl.letterSpacing);
        const auto& bm = th.type.bodyMedium;
        const std::string pmsg = humanError(pl.error);
        u.fonts().drawWrapped((int)bm.font, bm.size, Rect(card.x + 24, card.y + 76, card.w - 48, 70), pmsg,
                              col.onSurfaceVariant, 3, bm.letterSpacing);
        float bx = card.x + 24;
        if (retryable) {
            if (md3::button(u, u.id("pl.retry"), Rect(bx, card.b() - 64, 180, 44), "Повторить",
                            {md3::ButtonKind::Tonal}))
                openVideo(pl.item);
            bx += 188;
        }
        if (md3::button(u, u.id("pl.errback"), Rect(bx, card.b() - 64, 160, 44), "Назад",
                        {md3::ButtonKind::Text}))
            closePlayer();
        if (u.input().back) closePlayer();
        return;
    }

    bool dlgWasOpen = pl.closeDialogOpen;

    if (show) {
        // Пауза: затемнение 12 % (§5.7). На конце видео затемнения нет.
        if (paused && !ended) c.fillRect({0, 0, kW, kH}, Color::rgb(0x000000).withAlpha(0.12f));

        // Скримы: верх 190 (72 %→0), низ 240 (0→92 %).
        c.fillLinearGradient({0, 0, kW, 190}, CornerRadii(0), Color::rgb(0x000000).withAlpha(0.72f),
                             Color::rgb(0x000000).withAlpha(0.f), false);
        c.fillLinearGradient({0, kH - 240, kW, 240}, CornerRadii(0), Color::rgb(0x000000).withAlpha(0.f),
                             Color::rgb(0x000000).withAlpha(0.92f), false);

        // Верхняя панель
        if (md3::iconButton(u, u.id("pl.back"), Rect(20, 20, 48, 48), "arrow_back", Color::rgb(0xFFFFFF),
                            24.f, false, nullptr, true))
            closePlayer();
        std::string title = pl.details.title.empty() ? pl.item.title : pl.details.title;
        const auto& tl = th.type.titleLarge;
        u.fonts().drawEllipsized((int)tl.font, tl.size, 76, 20, 904, title, Color::rgb(0xFFFFFF),
                                 tl.letterSpacing);
        std::string sub = pl.details.channel.empty() ? pl.item.channel : pl.details.channel;
        if (!pl.qualityLabel.empty()) sub += "  •  " + pl.qualityLabel;
        const auto& bsm = th.type.bodySmall;
        u.fonts().drawEllipsized((int)bsm.font, bsm.size, 76, 52, 904, sub,
                                 Color::rgb(0xFFFFFF).withAlpha(0.9f), bsm.letterSpacing);

        // Центральная кнопка: круг 84, чёрный 45 %, центр кадра (Р-08).
        // На конце видео её нет — вместо неё кнопка «Смотреть снова» (§5.7).
        if (!ended) {
            Rect play{kW * 0.5f - 42, kH * 0.5f - 42, 84, 84};
            c.fillCircle(play.center().x, play.center().y, 42, Color::rgb(0x000000).withAlpha(0.45f));
            if (md3::iconButton(u, u.id("pl.play"), play.inset(12), paused ? "play_arrow" : "pause",
                                Color::rgb(0xFFFFFF), 40.f, false, nullptr, true)) {
                p.togglePause();
                pl.idle = 0.f;
            }
        }

        // Таймкоды: labelMedium на плашке чёрный 60 %, y=608.
        double dur = p.duration();
        double pos = pl.seeking ? (pl.seekPos * dur)
                                : (pl.seekTarget >= 0.0 ? pl.seekTarget : p.position());
        std::string posS = timeFmt(pos);
        std::string durS = timeFmt(dur);
        const auto& lm = th.type.labelMedium;
        auto capsule = [&](float x, bool rightAlign, const std::string& txt) {
            float tw = u.fonts().measure((int)lm.font, lm.size, txt, lm.letterSpacing);
            Rect r{rightAlign ? x - (tw + 12) : x, 608, tw + 12, 20};
            c.fillRoundRect(r, 6, Color::rgb(0x000000).withAlpha(0.6f));
            u.fonts().draw((int)lm.font, lm.size, r.x + 6, r.y + 2, txt, Color::rgb(0xFFFFFF),
                           lm.letterSpacing);
        };
        capsule(24, false, posS);
        capsule(kW - 24, true, durS);

        // Дорожка: x=96, y=596, 1088×44; буфер белый 30 %, прогресс primary.
        Rect seekRect{96, 596, 1088, 44};
        auto st = p.status();
        float buf = (st.cacheMs > 0 && dur > 0.0) ? (float)std::clamp(st.cacheMs / 1000.0 / dur, 0.0, 1.0)
                                                  : -1.f;
        float val = dur > 0 ? (float)std::clamp(pos / dur, 0.0, 1.0) : 0.f;
        md3::SliderOpts so;
        so.onVideo = true;
        so.buffer = buf;
        if (md3::slider(u, u.id("pl.seek"), seekRect, val, 0.f, 1.f, so)) {
            pl.seeking = u.input().down;
            pl.seekPos = val;
            if (!u.input().down) {
                const double target = (double)val * dur;
                p.seek(target);
                pl.seekTarget = target;   // UX-06: держим позицию, пока mpv догоняет
                pl.seekHoldT = 0.f;
                pl.seeking = false;
            }
        }
        pl.idle = 0.f;

        // Ряд кнопок: y=648, высота 48, зазор 8, поля 24.
        const float ry = 648.f;
        if (md3::iconButton(u, u.id("pl.rw"), Rect(24, ry, 48, 48), "replay_10", Color::rgb(0xFFFFFF), 24.f,
                            false, nullptr, true))
            relSeek(-10.0);
        if (md3::iconButton(u, u.id("pl.ff"), Rect(80, ry, 48, 48), "forward_10", Color::rgb(0xFFFFFF), 24.f,
                            false, nullptr, true))
            relSeek(10.0);

        int vol = p.volume();
        const bool mutedNow = p.muted();
        // PLR-09: mute — отдельное свойство, громкость при этом не сбрасывается.
        if (md3::iconButton(u, u.id("pl.mute"), Rect(136, ry, 48, 48),
                            (mutedNow || vol == 0) ? "volume_off" : "volume_up", Color::rgb(0xFFFFFF),
                            24.f, mutedNow, nullptr, true))
            p.setMuted(!mutedNow);
        float vv = std::clamp(vol / 130.f, 0.f, 1.f);
        md3::SliderOpts vo;
        vo.onVideo = true;
        vo.volume = true;
        if (md3::slider(u, u.id("pl.vol"), Rect(192, ry, 140, 48), vv, 0.f, 1.f, vo)) {
            p.setVolume((int)(vv * 130.f));
            if (mutedNow) p.setMuted(false);   // движение ползунка снимает mute
        }

        // Правый ряд: лайк, чип качества, чип обхода, more_vert (§5.7).
        bool proxyOn = !net::proxy().empty();
        float rx = kW - 24.f;
        Rect more{rx - 48.f, ry, 48.f, 48.f};
        rx -= 48.f + 8.f;
        if (md3::iconButton(u, u.id("pl.more"), more, "more_vert", Color::rgb(0xFFFFFF), 24.f,
                            pl.moreMenuOpen, nullptr, true))
            pl.moreMenuOpen = !pl.moreMenuOpen;
        rx -= 0.f;

        std::string stText = proxyOn ? "Обход DPI: прокси" : "SNI-обход";
        float bw = u.fonts().measure(1, 14, stText) + 58.f;   // 16+18+8+label+16 (иконка gpp_good)
        Rect bypass{rx - bw, ry + 6, bw, 36};
        md3::ChipOpts bo;
        bo.onVideo = true;
        bo.off = !proxyOn;
        md3::chip(u, u.id("pl.bypass"), bypass, stText, false, "gpp_good", false, bo);
        rx -= bw + 8.f;

        std::string qs = pl.qualityLabel.empty() ? "Авто" : pl.qualityLabel;
        float qw = u.fonts().measure(1, 14, qs) + 32.f;
        Rect qchip{rx - qw, ry + 6, qw, 36};
        md3::ChipOpts qo;
        qo.onVideo = true;
        if (md3::chip(u, u.id("pl.quality"), qchip, qs, false, nullptr, false, qo)) pl.qualityMenuOpen = !pl.qualityMenuOpen;
        rx -= qw + 8.f;

        bool liked = isLiked(pl.item.id);
        Color likeBg = col.primary.withAlpha(0.26f);
        if (md3::iconButton(u, u.id("pl.like"), Rect(rx - 48.f, ry, 48, 48), "thumb_up",
                            liked ? col.primary : Color::rgb(0xFFFFFF), 24.f, liked, &likeBg, true))
            toggleLike(pl.item.id);

        // Меню: низ y=600 (на 8 px выше плашки таймкода), пункты 48, поверх видео.
        if (pl.qualityMenuOpen) {
            std::vector<std::string> qi = {"Авто", "720p", "480p", "360p"};
            int selQ = std::clamp(cfg.quality, 0, (int)qi.size() - 1);
            Rect menu{qchip.x, 600.f - (qi.size() * 48.f + 16.f), 200.f, qi.size() * 48.f + 16.f};
            c.shadow(menu, CornerRadii(16), 10, 2, col.shadow.withAlpha(0.32f));
            c.fillRoundRect(menu, 16, col.surfaceContainer);
            for (size_t i = 0; i < qi.size(); i++) {
                Rect row{menu.x, menu.y + 8 + (float)i * 48.f, menu.w, 48.f};
                if ((int)i == selQ) c.fillRoundRect(row.inset(8.f, 0.f), 12, col.secondaryContainer);
                if ((int)i == selQ) md3::iconCentered(u, "check", Rect(row.x + 8, row.y + 12, 24, 24), 22, col.onSurface);
                u.fonts().drawEllipsized(0, 16, row.x + 44, row.y + 12, row.w - 60, qi[i], col.onSurface);
                if (u.interact(u.id("pl.q.item", (int)i), row)) {
                    pl.qualityMenuOpen = false;
                    switchPlayerQuality((int)i);   // PLR-14: поток на лету
                }
            }
        }
        if (pl.moreMenuOpen) {
            const char* mi[3] = {"Скорость", "Субтитры", "Сдвиг звука"};
            const char* mv[3] = {"Обычная", "Выкл", "0 мс"};
            const char* mc[3] = {"speed", "subtitles", "sync"};
            Rect menu{more.r() - 304.f, 600.f - (3 * 48.f + 16.f), 304.f, 3 * 48.f + 16.f};
            c.shadow(menu, CornerRadii(16), 10, 2, col.shadow.withAlpha(0.32f));
            c.fillRoundRect(menu, 16, col.surfaceContainer);
            for (int i = 0; i < 3; i++) {
                Rect row{menu.x, menu.y + 8 + (float)i * 48.f, menu.w, 48.f};
                if (u.interact(u.id("pl.m.item", i), row)) {
                    pl.moreMenuOpen = false;
                    toast(std::string(mi[i]) + " — в разработке");
                }
                // Пункт 48: иконка 24 onSurfaceVariant, название bodyLarge,
                // текущее значение bodyMedium справа (макет player-more-menu).
                md3::iconCentered(u, mc[i], Rect(row.x + 16, row.y + 12, 24, 24), 24, col.onSurfaceVariant);
                u.fonts().drawEllipsized(0, 16, row.x + 52, row.y + 12, row.w - 52 - 120, mi[i], col.onSurface);
                float vw = u.fonts().measure(0, 14, mv[i]);
                u.fonts().draw(0, 14, row.r() - 16 - vw, row.y + 14, mv[i], col.onSurfaceVariant);
            }
        }

        // Буферизация: капсула по центру y=120.
        if (p.buffering()) {
            Rect bc{kW * 0.5f - 110, 120, 220, 40};
            c.fillRoundRect(bc, 20, Color::rgb(0x000000).withAlpha(0.6f));
            md3::circularProgress(u, {bc.x + 26, bc.center().y}, 11, -1.f, 2.f);
            u.fonts().draw(0, 14, bc.x + 48, bc.y + 11, "Буферизация…", Color::rgb(0xFFFFFF));
        }
        // PLR-13: играет тихий null-AO или в потоке нет аудиодорожки — не молчим.
        const std::string aw = p.audioWarning();
        if (!aw.empty()) {
            const float ww = u.fonts().measure(0, 14, aw) + 40.f;
            Rect wc{kW * 0.5f - ww * 0.5f, 172, ww, 36};
            c.fillRoundRect(wc, 18, Color::rgb(0x000000).withAlpha(0.6f));
            md3::iconCentered(u, "volume_off", Rect(wc.x + 10, wc.y + 8, 20, 20), 20,
                              Color::rgb(0xFFD54F));
            u.fonts().draw(0, 14, wc.x + 38, wc.y + 10, aw, Color::rgb(0xFFFFFF));
        }
    } else {
        // Свёрнутые контролы: верхний скрим 24 (чёрный 40 %), полоса 3 px (§5.7).
        c.fillLinearGradient({0, 0, kW, 24}, CornerRadii(0), Color::rgb(0x000000).withAlpha(0.4f),
                             Color::rgb(0x000000).withAlpha(0.f), false);
        double dur = p.duration(), pos = p.position();
        if (dur > 0) {
            float p01 = (float)std::clamp(pos / dur, 0.0, 1.0);
            c.fillRect({0, 0, kW, 3}, Color::rgb(0xFFFFFF).withAlpha(0.3f));
            c.fillRect({0, 0, kW * p01, 3}, col.primary);
        }
        if (u.input().accept || u.input().pressed) pl.controls = true;
        pl.idle = 0.f;
    }

    // Ended: «Смотреть снова» 48 px по центру (336…384), inverseSurface (§5.7).
    if (ended) {
        float tw = u.fonts().measure(1, 16, "Смотреть снова");
        float bw = 28.f + 20.f + 8.f + tw + 28.f;   // .btn.l: padding 28, icon 20, gap 8
        Rect again{kW * 0.5f - bw * 0.5f, 336, bw, 48};
        c.fillRoundRect(again, 24, col.inverseSurface);
        md3::iconCentered(u, "replay", Rect(again.x + 28, again.y + 14, 20, 20), 20, col.inverseOnSurface);
        u.fonts().draw(1, 16, again.x + 56, again.y + 13, "Смотреть снова", col.inverseOnSurface);
        if (u.interact(u.id("pl.again"), again)) {
            p.seek(0);
            p.setPaused(false);
        }
    }

    // «Похожие» на конце: заголовок y=412 titleSmall, карточки 176×99 r12,
    // зазор 12, y=440, название 12/16 одной строкой (§5.7).
    if (hasRelated) {
        const auto& ts = th.type.titleSmall;
        u.fonts().draw((int)ts.font, ts.size, 24, 412, "Похожие видео", Color::rgb(0xFFFFFF), ts.letterSpacing);
        int n = (int)std::min<size_t>(pl.details.related.size(), 4);
        for (int i = 0; i < n; i++) {
            const yt::VideoItem& v = pl.details.related[(size_t)i];
            Rect card{24.f + (float)i * (176.f + 12.f), 440.f, 176.f, 99.f};
            if (u.interact(u.id("pl.rel", i), Rect(card.x, card.y, card.w, card.h + 22.f))) openVideo(v);
            auto thumb = images.get(v.thumbnailUrl);
            if (thumb && thumb->valid()) c.image(*thumb, card, 1.f, CornerRadii(12.f));
            else c.fillRoundRect(card, 12.f, col.surfaceContainerHigh);
            u.fonts().drawEllipsized(1, 12, card.x, card.b() + 6.f, card.w, v.title, Color::rgb(0xFFFFFF), 0.3f);
        }
    }

    // Диалог закрытия: показывать только если назад нажали после 30 с (§5.7).
    if (dlgWasOpen) {
        int r = md3::dialog(u, u.id("pl.close"), Rect{440, 264, 400, 220}, "Закрыть видео?",
                            "Воспроизведение остановится.", "Закрыть", "Отмена");
        if (r == 1) {
            pl.closeDialogOpen = false;
            closePlayer();
        } else if (r == 2) {
            pl.closeDialogOpen = false;
        }
    } else if (u.input().back) {
        if (p.position() > 30.0) pl.closeDialogOpen = true;
        else if (screen == Screen::Player) closePlayer();
    }

    // Горячие клавиши геймпада
    if (u.input().left) relSeek(-10.0);
    if (u.input().right) relSeek(10.0);
    if (u.input().bumpL) relSeek(-30.0);
    if (u.input().bumpR) relSeek(30.0);
    if (u.input().y) p.togglePause();
}

// ---------------------------------------------------------------- Settings
void App::drawSettings(ui::Ui& u) {
    auto& c = u.canvas();
    auto& th = u.theme();
    auto& col = th.colors;
    c.fillRect({0, 0, kW, kH}, col.surface);

    if (md3::iconButton(u, u.id("set.back"), Rect(18, 12, 48, 48), "arrow_back", col.onSurface)) back();
    const auto& hs = th.type.headlineSmall;
    u.fonts().draw((int)hs.font, hs.size, 76, 20, "Настройки", col.onSurface, hs.letterSpacing);

    // Две колонки 560, x=56 / x=640, зазор 24; верх 72, padding 8/36 (§5.8, Р-09).
    Rect vp{0, 72, kW, kH - 72 - kBottomReserve};
    const float colX0 = 56.f, colX1 = 640.f, colW = 560.f;
    // CFG-06: баннер о недоступной записи занимает строку над колонками.
    const float bannerH = storageReadOnly ? 72.f : 0.f;
    // Высота контента: 8 сверху + 5 секций правой колонки (28 overline +
    // card + 20) + 8 снизу. Правая колонка выше левой (1270 px до низа
    // последней карточки). Карточка «Сеть» выросла на кнопку SEC-01.
    const float contentH = 1278.f + bannerH;
    // UX-13: виджеты должны знать, к какому скролл-контейнеру относятся —
    // иначе стик/D-pad не прокручивают настройки, а фокус не докручивает
    // карточку в видимую область (как на «Главной» и в поиске).
    u.pushScroll(u.id("set.scroll"));
    float off = u.scrollArea(u.id("set.scroll"), vp, contentH);
    c.pushClip(vp);

    if (storageReadOnly) {
        Rect b{colX0, vp.y + 8.f - off, kW - 2.f * colX0, 56.f};
        c.fillRoundRect(b, 16.f, col.errorContainer);
        md3::iconCentered(u, "cloud_off", Rect(b.x + 16.f, b.y + 16.f, 24.f, 24.f), 24.f,
                          col.onErrorContainer);
        u.fonts().draw(1, 14, b.x + 52.f, b.y + 8.f, "SD только для чтения", col.onErrorContainer);
        u.fonts().drawEllipsized(0, 12, b.x + 52.f, b.y + 30.f, b.w - 68.f,
                                 "Настройки и история работают, но не сохранятся на карту",
                                 col.onErrorContainer.withAlpha(0.9f));
    }

    float yL = vp.y + 8.f - off + bannerH;
    float yR = vp.y + 8.f - off + bannerH;

    auto overline = [&](float x, float& y, const char* t) {
        const auto& s = th.type.titleSmall;
        u.fonts().draw((int)s.font, s.size, x + 20.f, y, t, col.primary, 0.5f);
        y += 20.f + 8.f;
    };
    auto cardBg = [&](float x, float y, float h) {
        Rect r{x, y, colW, h};
        c.fillRoundRect(r, 20.f, col.surfaceContainerLow);
        return r;
    };

    // ================= Левая колонка =================
    // ---------- Обход DPI ----------
    overline(colX0, yL, "ОБХОД DPI  •  BYEBYEDPI");
    {
        Rect card = cardBg(colX0, yL, 428.f);
        float px = card.x + 20.f, pw = card.w - 40.f, cy = card.y + 20.f;
        u.fonts().draw(1, 16, px, cy + 12, "Обход блокировок", col.onSurface);
        bool en = cfg.dpiEnabled;
        if (md3::switchWidget(u, u.id("set.dpi"), Rect(card.r() - 20 - 52, cy + 8, 52, 32), en))
            setDpiEnabled(en);
        cy += 48.f;
        u.fonts().drawWrapped(0, 14, Rect(px, cy, pw, 40),
                              "Фрагментация TLS ClientHello против DPI-троттлинга YouTube",
                              col.onSurfaceVariant, 2);
        cy += 40.f + 16.f;
        const char* modes[] = {"Split", "Multi", "Disorder", "TLS"};
        std::vector<std::string> ml(modes, modes + 4);
        // cfg.dpiMode хранит dpi::Mode (1..4); в сегментах — индекс 0..3.
        int mi = cfg.dpiMode - 1;
        if (mi < 0 || mi > 3) mi = 1;
        if (md3::segmented(u, u.id("set.dpimode"), Rect(px, cy, pw, 40), ml, mi)) {
            cfg.dpiMode = mi + 1;
            dpi::setMode((dpi::Mode)cfg.dpiMode);
            saveConfig();
        }
        cy += 40.f + 12.f;
        if (md3::button(u, u.id("set.probe"), Rect(px, cy, 200, 40),
                        probing ? "Проверяем…" : "Проверить доступ",
                        {md3::ButtonKind::Tonal, !probing, "wifi"}))
            probe(true);
        if (probeMs > 0) {
            char buf[96];
            snprintf(buf, sizeof(buf), "YouTube: %d мс", probeMs);
            md3::iconCentered(u, "check", Rect(px + 216, cy + 10, 20, 20), 20, col.tertiary);
            u.fonts().draw(1, 14, px + 242, cy + 10, buf, col.tertiary);
        } else {
            md3::iconCentered(u, "wifi_off", Rect(px + 216, cy + 10, 20, 20), 20, col.error);
            u.fonts().draw(1, 14, px + 242, cy + 10, "нет соединения", col.error);
        }
        cy += 48.f + 12.f;
        // DPI-06: автоподбор режима — проба generate_204 по всем режимам,
        // запоминается первый рабочий. Во время видео режим не меняется.
        if (md3::button(u, u.id("set.dpiauto"), Rect(px, cy, 260, 40),
                        dpiAutoSelecting ? "Подбираем режим…" : "Подобрать автоматически",
                        {md3::ButtonKind::Tonal, !dpiAutoSelecting && !probing, "bolt"}))
            autoSelectDpi();
        cy += 40.f + 12.f;
        // DPI-16: снимок статистики (потокобезопасно) — соединения, скорость,
        // активные сессии ретранслятора, SSL-сессии и последняя ошибка.
        auto st = dpi::statsSnapshot();
        char sbuf[192];
        snprintf(sbuf, sizeof(sbuf), "Соединений: %llu   •   фрагментов: %llu   •   трафик: %.1f МБ",
                 (unsigned long long)st.connections, (unsigned long long)st.fragmented,
                 st.relayedBytes / 1048576.0);
        u.fonts().draw(0, 14, px, cy, sbuf, col.onSurfaceVariant);
        cy += 20.f;
        char mbuf[224];
        snprintf(mbuf, sizeof(mbuf), "%.0f КБ/с   •   активных: %d   •   SSL-сессий: %llu", st.rateKbps,
                 st.activeSessions, (unsigned long long)st.sslSessions);
        u.fonts().draw(0, 14, px, cy, mbuf, col.onSurfaceVariant);
        cy += 20.f;
        if (!st.lastError.empty()) {
            u.fonts().drawEllipsized(0, 12, px, cy, pw, "DPI: " + st.lastError, col.error);
            cy += 18.f;
        }
// UX-07: внешний прокси и параметры TLS вынесены в «Дополнительно»
        // (правая колонка) — карточка DPI больше не перегружена.
        u.fonts().draw(0, 14, px, cy,
                       std::string("SNI-обход: вкл  •  ") +
                           (net::verify() == net::Verify::Full
                                ? "TLS: имя + цепочка"
                                : (net::verify() == net::Verify::ChainOnly ? "TLS: только цепочка"
                                                                          : "TLS: без проверок")),
                       col.tertiary);
        cy += 20.f;
        if (!feedError.empty())
            u.fonts().drawEllipsized(0, 14, px, cy, pw, "Последняя ошибка: " + feedError, col.error);
    }
    yL += 404.f + 20.f;

    // ---------- Диагностика DPI (UX-08) ----------
    overline(colX0, yL, "ДИАГНОСТИКА  •  DPI");
    {
        const float cardH = 268.f;
        Rect card = cardBg(colX0, yL, cardH);
        float px = card.x + 20.f, pw = card.w - 40.f, cy = card.y + 20.f;

        char l1[192];
        snprintf(l1, sizeof(l1), "Режим: %s   •   SOCKS: %d   •   ретранслятор: %d",
                 dpi::modeName(dpi::config().mode), dpi::socksPort(), dpi::relayPort());
        u.fonts().drawEllipsized(0, 14, px, cy, pw, l1, col.onSurface);
        cy += 22.f;

        const std::string rtt = probeMs > 0 ? ("YouTube: " + std::to_string(probeMs) + " мс")
                                            : std::string("YouTube: нет ответа");
        const std::string boot =
            !bootDone ? std::string("идёт") : (feedOk ? "лента загружена" : "лента с ошибкой");
        u.fonts().drawEllipsized(0, 14, px, cy, pw,
                                 "Проба: " + rtt + "   •   Bootstrap: " + boot,
                                 probeOk ? col.onSurfaceVariant : col.error);
        cy += 22.f;

        u.fonts().draw(1, 13, px, cy, "Последние ошибки", col.onSurfaceVariant);
        cy += 20.f;
        if (diagErrors.empty()) {
            u.fonts().draw(0, 12, px, cy, "Нет — всё чисто", col.onSurfaceVariant.withAlpha(0.7f));
        } else {
            for (auto it = diagErrors.rbegin(); it != diagErrors.rend(); ++it) {
                u.fonts().drawEllipsized(0, 12, px, cy, pw, "• " + *it, col.error.withAlpha(0.9f));
                cy += 16.f;
            }
        }

        if (md3::button(u, u.id("set.diag.copy"), Rect(px, card.y + cardH - 56.f, 240.f, 40.f),
                        "Скопировать лог в файл", {md3::ButtonKind::Tonal, true, "download"}))
            exportDiagnostics();
    }
    yL += 268.f + 20.f;

    // ---------- Видео ----------
    overline(colX0, yL, "ВИДЕО");
    {
        Rect card = cardBg(colX0, yL, 190.f);
        float px = card.x + 20.f, pw = card.w - 40.f, cy = card.y + 20.f;
        u.fonts().draw(1, 16, px, cy, "Качество по умолчанию", col.onSurface);
        cy += 24.f + 8.f;
        std::vector<std::string> q = {"Авто", "720p", "480p", "360p"};
        int qi = std::clamp(cfg.quality, 0, 3);
        if (md3::segmented(u, u.id("set.q"), Rect(px, cy, pw, 40), q, qi)) {
            cfg.quality = qi;
            saveConfig();
        }
        cy += 40.f + 12.f;
        u.fonts().draw(1, 16, px, cy + 12, "Автовоспроизведение следующего видео", col.onSurface);
        bool ap = cfg.autoplay;
        if (md3::switchWidget(u, u.id("set.autoplay"), Rect(card.r() - 20 - 52, cy + 8, 52, 32), ap)) {
            cfg.autoplay = ap;
            saveConfig();
        }
    }
    yL += 190.f + 20.f;

    // ---------- Звук ----------
    overline(colX0, yL, "ЗВУК  •  СИНХРОНИЗАЦИЯ С ВИДЕО");
    {
        Rect card = cardBg(colX0, yL, 170.f);
        float px = card.x + 20.f, cy = card.y + 20.f;
        u.fonts().draw(1, 16, px, cy + 12, "Сдвиг звука", col.onSurface);
        float rr = card.r() - 20.f;
        Rect reset{rr - 130.f, cy, 130.f, 40.f};
        rr -= 130.f + 8.f;
        Rect plus{rr - 64.f, cy, 64.f, 40.f};
        rr -= 64.f + 8.f;
        Rect vz{rr - 88.f, cy, 88.f, 40.f};
        rr -= 88.f + 8.f;
        Rect minus{rr - 64.f, cy, 64.f, 40.f};
        int ad = cfg.audioDelayMs;
        if (md3::button(u, u.id("set.adelay.minus"), minus, "-50", {md3::ButtonKind::Tonal})) ad -= 50;
        if (md3::button(u, u.id("set.adelay.plus"), plus, "+50", {md3::ButtonKind::Tonal})) ad += 50;
        if (md3::button(u, u.id("set.adelay.reset"), reset, "Сброс",
                        {md3::ButtonKind::Text, true, "restart_alt"}))
            ad = 0;
        char val[48];
        snprintf(val, sizeof(val), "%+d мс", cfg.audioDelayMs);
        u.fonts().drawCentered(1, 22, vz, val, col.primary);
        if (ad != cfg.audioDelayMs) {
            cfg.audioDelayMs = std::clamp(ad, -1000, 1000);
            player::instance().setAudioDelay(cfg.audioDelayMs / 1000.0);
            saveConfig();
            toast("Сдвиг звука: " + std::to_string(cfg.audioDelayMs) + " мс");
        }
        u.fonts().drawWrapped(0, 14, Rect(px, cy + 56, card.w - 40, 40),
                              "Если звук отстаёт от картинки — сдвиньте в плюс", col.onSurfaceVariant, 2);
    }
    yL += 170.f + 20.f;

    // ================= Правая колонка =================
    // ---------- Оформление ----------
    overline(colX1, yR, "ОФОРМЛЕНИЕ  •  MATERIAL 3");
    {
        Rect card = cardBg(colX1, yR, 230.f);
        float px = card.x + 20.f, cy = card.y + 20.f;
        u.fonts().draw(1, 16, px, cy, "Тёмная тема", col.onSurface);
        u.fonts().draw(0, 14, px, cy + 22, "M3 tonal palette, автоматически", col.onSurfaceVariant);
        bool dark = cfg.dark;
        if (md3::switchWidget(u, u.id("set.dark"), Rect(card.r() - 20 - 52, cy + 8, 52, 32), dark)) {
            cfg.dark = dark;
            applyTheme();
            saveConfig();
        }
        cy += 48.f + 16.f;
        u.fonts().draw(1, 16, px, cy, "Палитра", col.onSurface);
        cy += 24.f + 4.f;
        for (int i = 0; i < kSeedCount; i++) {
            Rect zone{px - 8.f + i * 56.f, cy - 8.f, 48.f, 48.f};
            Rect sw{zone.x + 8.f, zone.y + 8.f, 32.f, 32.f};
            bool active = cfg.seed == kSeeds[i].argb;
            c.fillRoundRect(sw, 16.f, Color::rgb(kSeeds[i].argb & 0xFFFFFF));
            if (active) {
                c.strokeRoundRect(sw.inset(-3.f), CornerRadii(19.f), 2.f, col.primary);
                md3::iconCentered(u, "check", sw, 20, Color::rgb(0xFFFFFF));
            }
            if (u.interact(u.id("set.seed", i), zone)) {
                cfg.seed = kSeeds[i].argb;
                applyTheme();
                saveConfig();
                toast(std::string("Тема: ") + kSeeds[i].name);
            }
        }
    }
    yR += 230.f + 20.f;

    // ---------- История и рекомендации ----------
    overline(colX1, yR, "ИСТОРИЯ И РЕКОМЕНДАЦИИ");
    {
        const float cardH = 200.f;
        Rect card = cardBg(colX1, yR, cardH);
        float px = card.x + 20.f, cy = card.y + 20.f;
        u.fonts().draw(1, 16, px, cy, "Локальные рекомендации", col.onSurface);
        u.fonts().drawWrapped(0, 14, Rect(px, cy + 22, card.w - 140, 20), "История хранится только на этой консоли",
                              col.onSurfaceVariant, 1);
        bool reco = cfg.localReco;
        if (md3::switchWidget(u, u.id("set.reco"), Rect(card.r() - 20 - 52, cy + 8, 52, 32), reco)) {
            cfg.localReco = reco;
            saveConfig();
            rankFeed();
            toast(reco ? "Лента учитывает историю" : "Лента без персонализации");
        }
        cy += 48.f + 12.f;
        if (md3::button(u, u.id("set.clear"), Rect(px - 12.f, cy, 210.f, 40.f), "Очистить историю",
                        {md3::ButtonKind::Text, true, "delete"}))
            g_setDialog = true;
        // CFG-05: сброс рекомендаций — снимаем выученные веса каналов и
        // отключаем персонализацию; сама история (для «Продолжить») остаётся.
        if (md3::button(u, u.id("set.reco.reset"), Rect(px + 208.f, cy, 244.f, 40.f),
                        "Сбросить рекомендации", {md3::ButtonKind::Text, true, "refresh"})) {
            cfg.localReco = false;
            affinity_.clear();
            rankFeed();
            saveConfig();
            toast("Рекомендации сброшены — лента без персонализации");
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "Сигналов: %d каналов  •  %d видео в истории", recoSignals(),
                 (int)history.size());
        u.fonts().draw(0, 12, px, cy + 52, buf, col.onSurfaceVariant.withAlpha(0.85f));
        u.fonts().drawEllipsized(0, 12, px, cy + 70, card.w - 40.f,
                                 "Данные остаются на этой консоли и не уходят в Google",
                                 col.onSurfaceVariant.withAlpha(0.7f));
        // YT-11: объяснимость — 2 главные причины текущего порядка ленты.
        if (cfg.localReco) {
            auto top = topChannels(2);
            if (!top.empty()) {
                std::string why = "Чаще показываем: ";
                for (size_t i = 0; i < top.size(); ++i) {
                    if (i) why += " • ";
                    why += top[i].first;
                }
                u.fonts().drawEllipsized(0, 12, px, cy + 88, card.w - 40, why,
                                         col.onSurfaceVariant.withAlpha(0.85f));
            }
        }
    }
    yR += 200.f + 20.f;

    // ---------- Дополнительно (UX-07): прокси и TLS/CA ----------
    overline(colX1, yR, "ДОПОЛНИТЕЛЬНО  •  СЕТЬ");
    {
        const float cardH = 256.f;
        Rect card = cardBg(colX1, yR, cardH);
        float px = card.x + 20.f, pw = card.w - 40.f, cy = card.y + 20.f;
        bool proxyOn = !cfg.proxy.empty();
        u.fonts().draw(1, 16, px, cy, "Внешний прокси", col.onSurface);
        cy += 26.f;
        u.fonts().drawEllipsized(0, 14, px, cy, pw,
                                 proxyOn ? cfg.proxy : std::string("Отключён — используется обход DPI"),
                                 proxyOn ? col.primary : col.onSurfaceVariant);
        cy += 26.f;
        if (md3::button(u, u.id("set.proxy.edit"), Rect(px - 12.f, cy, 168.f, 40.f),
                        proxyOn ? "Изменить" : "Задать прокси",
                        {md3::ButtonKind::Tonal, true, "tune"})) {
            std::string pval = cfg.proxy;
            if (platform::keyboard(pval, "Прокси: host:port или socks5h://host:port",
                                   "Внешний прокси (пусто — отключить)")) {
                std::string perr;
                if (!setProxy(pval, perr)) toast(perr);
            }
        }
        if (proxyOn) {
            if (md3::button(u, u.id("set.proxy.reset"), Rect(px + 164.f, cy, 140.f, 40.f), "Сбросить",
                            {md3::ButtonKind::Text, true, "close"})) {
                std::string perr;
                setProxy("", perr);
            }
        }
        cy += 48.f;
        c.fillRect(Rect(px, cy, card.w - 40, 1), col.outlineVariant);
        cy += 12.f;
        u.fonts().drawEllipsized(0, 12, px, cy, pw,
                                 std::string("TLS: ") +
                                     (net::verify() == net::Verify::Full
                                          ? "имя + цепочка"
                                          : (net::verify() == net::Verify::ChainOnly
                                                 ? "только цепочка"
                                                 : "без проверок")),
                                 col.onSurfaceVariant);
        cy += 18.f;
        u.fonts().drawEllipsized(0, 12, px, cy, pw, "Сертификаты: встроенный cacert-trim.pem",
                                 col.onSurfaceVariant.withAlpha(0.85f));
        cy += 24.f;
        // SEC-01: понижение проверки TLS не должно быть «молчаливым». Кнопка
        // возвращает полную проверку имени и цепочки; при блокировке SNI
        // запросы могут перестать проходить до перезапуска — это осознанный
        // выбор пользователя, поэтому действие видимое и логируемое.
        if (net::verify() != net::Verify::Full &&
            md3::button(u, u.id("set.tls.verify"), Rect(px - 12.f, cy, 268.f, 40.f),
                        "Вернуть проверку TLS", {md3::ButtonKind::Text, true, "lock"})) {
            net::setVerify(net::Verify::Full);
            platform::bootLog("SEC-01: пользователь вернул полную проверку TLS (Full)");
            toast("Проверка TLS: имя + цепочка");
        }
    }
    yR += 256.f + 20.f;

    // ---------- О приложении ----------
    overline(colX1, yR, "О ПРИЛОЖЕНИИ");
    {
        Rect card = cardBg(colX1, yR, 200.f);
        float px = card.x + 20.f, cy = card.y + 20.f;
        u.fonts().draw(1, 18, px, cy, "TubeNX 1.0", col.onSurface);
        u.fonts().draw(0, 14, px, cy + 28, "Material 3 Expressive • Google Sans", col.onSurfaceVariant);
        u.fonts().draw(0, 12, px, cy + 52, "Сборка: 2026.11.13 • devkitA64 / libnx", col.onSurfaceVariant);
        if (md3::button(u, u.id("set.lic"), Rect(px - 12.f, cy + 76, 260, 40), "Лицензии open source",
                        {md3::ButtonKind::Text, true, "chevron_right"}))
            g_licDialog = true;
        c.fillRect(Rect(px, cy + 128, card.w - 40, 1), col.outlineVariant);
        u.fonts().draw(0, 12, px, cy + 140, "Кнопки: A — выбрать • B — назад", col.onSurfaceVariant);
        u.fonts().draw(0, 12, px, cy + 160, "В плеере: Y — пауза • L/R — ±30 с • ←/→ — ±10 с",
                       col.onSurfaceVariant);
    }
    yR += 200.f + 20.f;

    // ---------- Система (UX-07): опасные действия отдельно ----------
    overline(colX1, yR, "СИСТЕМА");
    {
        Rect card = cardBg(colX1, yR, 156.f);
        float px = card.x + 20.f, cy = card.y + 20.f;
        u.fonts().draw(1, 16, px, cy, "Завершить TubeNX", col.onSurface);
        u.fonts().draw(0, 13, px, cy + 24, "Вернётесь в hbmenu, видео остановится",
                       col.onSurfaceVariant);
        if (md3::button(u, u.id("set.exit"), Rect(px - 12.f, cy + 46, 200.f, 40.f), "Выход",
                        {md3::ButtonKind::Tonal, true, "logout"}))
            platform::requestExit();
        // CFG-07: сброс настроек к дефолтам (история не стирается).
        if (md3::button(u, u.id("set.reset"), Rect(px + 200.f, cy + 46, 244.f, 40.f),
                        "Сбросить настройки", {md3::ButtonKind::Text, true, "restart_alt"}))
            g_resetDialog = true;
        u.fonts().draw(0, 12, px, cy + 96, "Сброс вернёт тему, качество, DPI и прокси к значениям по умолчанию.",
                       col.onSurfaceVariant.withAlpha(0.8f));
    }
    yR += 156.f + 20.f;

    c.popClip();
    u.popScroll();

    // Диалог очистки истории: опасное действие только через подтверждение (§5.8).
    if (g_setDialog) {
        int r = md3::dialog(u, u.id("set.dialog"), Rect{440, 252, 400, 220}, "Очистить историю?",
                            "История просмотров и локальные рекомендации будут удалены с этой консоли.",
                            "Очистить", "Отмена");
        if (r == 1) {
            history.clear();
            rebuildAffinity();
            saveConfig();
            toast("История очищена");
            g_setDialog = false;
        } else if (r == 2) {
            g_setDialog = false;
        }
    }

    // Лицензии open source: список зависимостей и ссылка на тексты (§5.8).
    if (g_licDialog) {
        const char* body =
            "TubeNX — MIT\n"
            "libnx — MIT\n"
            "stb (image / image_write) — public domain / MIT\n"
            "Google Sans — SIL OFL 1.1\n"
            "Material Symbols — Apache 2.0\n"
            "libmpv, FFmpeg, devkitPro — лицензии проектов\n"
            "\n"
            "Полные тексты: LICENSE, THIRD_PARTY.md, NOTICE,\n"
            "resources/fonts/LICENSE-OFL.txt, resources/fonts/LICENSE.";
        int r = md3::dialog(u, u.id("set.lic.dialog"), Rect{320, 180, 640, 340},
                            "Лицензии open source", body, "Закрыть", nullptr);
        if (r != 0) g_licDialog = false;
    }

    // CFG-07: сброс настроек — через подтверждение, история не стирается.
    if (g_resetDialog) {
        int r = md3::dialog(u, u.id("set.reset.dialog"), Rect{440, 236, 400, 248},
                            "Сбросить настройки?",
                            "Тема, качество, скорость, режим DPI и прокси вернутся к значениям по умолчанию. История просмотров сохранится.",
                            "Сбросить", "Отмена");
        if (r == 1) {
            resetSettings();
            g_resetDialog = false;
        } else if (r == 2) {
            g_resetDialog = false;
        }
    }
}

void App::drawToasts(ui::Ui& u) {
    if (toastT <= 0.f || toastText.empty()) return;
    // UX-12: при ошибке загрузки на экране уже есть свой блок — тост не
    // наслаиваем. На плеере — аналогично карточке ошибки.
    if ((screen == Screen::Home && !feedError.empty()) ||
        (screen == Screen::Search && !searchError.empty()) ||
        (screen == Screen::Player && !pl.error.empty()))
        return;
    const float appear = toastTotal > 0.f ? std::min(1.f, (toastTotal - toastT) * 4.f) : 1.f;
    md3::snackbar(u, toastText, appear);
}

// ---------------------------------------------------------------- draw
void App::draw(ui::Ui& u) {
    // UX-11: B — назад. Плеер обрабатывает B сам (диалог закрытия после 30 с),
    // «Главная» — корень, там B ничего не делает.
    if (u.input().back && (screen == Screen::Settings || screen == Screen::Search)) back();

    switch (screen) {
    case Screen::Splash: drawSplash(u); break;
    case Screen::Home: drawHome(u); break;
    case Screen::Search: drawSearch(u); break;
    case Screen::Player: drawPlayer(u); break;
    case Screen::Settings: drawSettings(u); break;
    }
    drawToasts(u);
}

} // namespace tnx::app
