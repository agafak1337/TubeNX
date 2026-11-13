// ui/screens.cpp — отрисовка экранов в стиле Material 3 (Expressive).
#include "ui/app.hpp"

#include "core/canvas.hpp"
#include "dpi/dpi.hpp"
#include "md3/icons.hpp"
#include "md3/widgets.hpp"
#include "net/http.hpp"
#include "platform.hpp"

#include <cstdio>
#include <cstring>

namespace tnx::app {

namespace {

constexpr float kW = 1280.f;
constexpr float kH = 720.f;

std::string firstUtf8Char(const std::string& s) {
    if (s.empty()) return "?";
    unsigned char c = s[0];
    int n = 1;
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    return s.substr(0, std::min<size_t>(n, s.size()));
}

std::string timeFmt(double t) {
    if (!(t > 0.0)) t = 0.0;
    int s = (int)t;
    int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

uint32_t seedList[] = {0xFFB3261E, 0xFF6750A4, 0xFF006A6A, 0xFF386A20, 0xFF7D5260, 0xFF3F5F90};
const char* seedNames[] = {"YouTube", "Purple", "Teal", "Green", "Rose", "Blue"};

} // namespace

// ---------------------------------------------------------------- helpers
void App::drawGrid(ui::Ui& u, uint64_t id, const Rect& area, const yt::Feed& f, bool loading,
                   const std::string& err, const char* emptyText, float scroll, float contentH) {
    auto& col = u.theme().colors;
    auto m = metricsFor(area.w);
    (void)contentH;

    if (!err.empty()) {
        Rect card{area.x + area.w * 0.5f - 300.f, area.y + 120.f, 600.f, 268.f};
        md3::surfaceCard(u, card, md3::CardKind::Filled, 24.f);
        md3::iconCentered(u, "cloud_off", Rect(card.x + 24, card.y + 28, 28, 28), 28, col.error);
        u.fonts().draw(1, 20, card.x + 64, card.y + 26, "Не удалось загрузить", col.onSurface);
        u.fonts().drawWrapped(0, 14, Rect(card.x + 24, card.y + 72, card.w - 48, 76), err, col.onSurfaceVariant,
                              3);

        // Что именно включено — чтобы не гадать по надписи «прямое соединение».
        {
            bool proxy = !net::proxy().empty();
            char diag[192];
            snprintf(diag, sizeof(diag), "SNI-обход: вкл  •  %s",
                     proxy ? net::proxy().c_str() : "прямое соединение");
            u.fonts().draw(0, 12, card.x + 24, card.y + 158, diag, col.onSurfaceVariant.withAlpha(0.8f));
        }

        Rect btn{card.x + 24, card.b() - 68, 200, 44};
        // Если фокус ещё нигде (геймпад, без курсора) — сразу на «Повторить»,
        // чтобы A работала без слепого блуждания по D-pad.
        if (!u.hasFocus()) u.focus(id ^ 0x51);
        if (md3::button(u, id ^ 0x51, btn, "Повторить", {})) {
            toast("Повторная загрузка…");
            loadHome(homeIsTrending);
        }
        Rect btn2{card.x + 240, card.b() - 68, 200, 44};
        if (md3::button(u, id ^ 0x52, btn2, "Настройки", {md3::ButtonKind::Outlined}))
            go(Screen::Settings);
        return;
    }

    auto items = flatItems(f);
    if (items.empty() && !loading) {
        u.fonts().drawCentered(0, 18, area, emptyText ? emptyText : "Здесь пока пусто", col.onSurfaceVariant);
        return;
    }

    int cols = m.cols;
    // Отсекаем карточки вне экрана. Раньше нижняя граница бралась по высоте
    // всего контента (area.h == gridH), поэтому все строки ниже вьюпорта
    // рисовались впустую — заметный вклад в лаги ленты.
    Rect view = u.canvas().clip();
    for (size_t i = 0; i < items.size(); i++) {
        int row = (int)i / cols, colIdx = (int)i % cols;
        Rect card{area.x + colIdx * (m.cardW + m.gap), area.y + row * (m.cardH + m.gap), m.cardW, m.cardH};
        if (card.b() < view.y - 40.f || card.y > view.b() + 40.f) continue;

        const yt::VideoItem& v = *items[i];
        md3::VideoTileData d;
        d.thumb = images.get(v.thumbnailUrl);
        d.title = v.title;
        d.meta = v.channel.empty() ? v.meta : (v.channel + "  •  " + v.meta);
        d.duration = v.live ? "LIVE" : v.duration;
        d.live = v.live;
        d.avatars = true;
        d.channelInitial = firstUtf8Char(v.channel);
        if (md3::videoTile(u, id ^ (uint64_t)(i + 1) * 1099511628211ull, card, d)) openVideo(v);
    }

    if (loading) {
        Rect box{area.x, area.y + (float)((items.size() + cols - 1) / cols) * (m.cardH + m.gap), area.w, 80};
        md3::circularProgress(u, {box.center().x, box.y + 40}, 14.f, -1.f, 3.f);
    }
}


// ---------------------------------------------------------------- Continue strip
void App::drawContinueStrip(ui::Ui& u, const Rect& r, const std::vector<const HistoryEntry*>& items) {
    auto& col = u.theme().colors;
    if (items.empty()) return;

    u.fonts().draw(1, 15, r.x, r.y, "Продолжить просмотр", col.onSurface);

    const int n = (int)std::min<size_t>(items.size(), 5);
    const float gap = 12.f;
    const float cardW = (r.w - (n - 1) * gap) / (float)n;
    for (int i = 0; i < n; i++) {
        const HistoryEntry* e = items[(size_t)i];
        Rect card{r.x + i * (cardW + gap), r.y + 32.f, cardW, 100.f};

        md3::VideoTileData d;
        d.thumb = images.get(yt::thumbUrl(e->id));
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

        if (md3::videoTileCompact(u, u.id("home.cont", i), card, d)) {
            yt::VideoItem v;
            v.id = e->id;
            v.title = e->title;
            v.channel = e->channel;
            openVideo(v);
            return;
        }
    }
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
        bg.fillRadialGradient({0, 0, kW, kH}, {kW * 0.5f, 250.f}, 620.f,
                              col.primary.withAlpha(0.17f), col.primary.withAlpha(0.f));
        bg.fillRadialGradient({0, 0, kW, kH}, {kW * 0.12f, kH * 0.95f}, 620.f,
                              col.secondary.withAlpha(0.10f), col.secondary.withAlpha(0.f));
        splashBgReady = true;
    }
    c.image(splashBg, Rect(0, 0, kW, kH), 1.f, CornerRadii(0));

    float t = splashT;
    float appear = std::min(1.f, t / 0.5f);
    float scale = 0.9f + 0.1f * (1.f - (1.f - appear) * (1.f - appear));

    Rect mark{kW * 0.5f - 56 * scale, 210 - 56 * (scale - 1.f), 112 * scale, 112 * scale};
    c.shadow(mark, CornerRadii(32 * scale), 28, 12, col.shadow.withAlpha(0.45f));
    c.fillRoundRect(mark, 32 * scale, col.primary);
    md3::iconCentered(u, "play_arrow", mark, 64 * scale, col.onPrimary);

    u.fonts().drawCentered(2, 46, Rect(0, 350, kW, 60), "TubeNX", col.onSurface);
    u.fonts().drawCentered(0, 17, Rect(0, 404, kW, 26), "YouTube для Nintendo Switch", col.onSurfaceVariant);

    // Статус
    float y = 470;
    if (!bootDone) {
        md3::circularProgress(u, {kW * 0.5f - 90, y + 12}, 9.f, -1.f, 2.5f);
        u.fonts().draw(0, 15, kW * 0.5f - 70, y, splashStatus, col.onSurfaceVariant);
    } else {
        const char* st = feedOk ? "Лента загружена" : "Загрузка ленты…";
        md3::iconCentered(u, feedOk ? "check" : "sync", Rect(kW * 0.5f - 96, y + 2, 20, 20), 20,
                          feedOk ? col.primary : col.onSurfaceVariant);
        u.fonts().draw(0, 15, kW * 0.5f - 68, y, st, col.onSurfaceVariant);
    }

    // Статус обхода DPI
    Rect chip{kW * 0.5f - 210, 520, 420, 56};
    c.fillRoundRect(chip, 28, col.surfaceContainerHigh);
    Color dot = probeMs > 0 ? Color::rgb(0x4CAF50) : (probeOk ? col.primary : col.error);
    c.fillCircle(chip.x + 28, chip.center().y, 5, dot);
    char buf[160];
    if (probeMs > 0)
        snprintf(buf, sizeof(buf), "ByeByeDPI активен • YouTube %d мс", probeMs);
    else if (probeOk)
        snprintf(buf, sizeof(buf), "ByeByeDPI активен • соединение есть");
    else
        snprintf(buf, sizeof(buf), "Проверка соединения…");
    u.fonts().draw(0, 14, chip.x + 44, chip.y + 18, buf, col.onSurfaceVariant);

    // Линия прогресса загрузки
    Rect bar{0, kH - 4, kW, 4};
    c.fillRect(bar, col.surfaceContainerHigh);
    float p = bootDone ? (feedOk ? 1.f : 0.7f) : std::min(0.65f, t * 0.4f);
    c.fillRect({0, bar.y, kW * p, 4}, col.primary);

    u.fonts().draw(0, 11, 24, kH - 32, "Material 3 Expressive • Google Sans", col.onSurfaceVariant.withAlpha(0.7f));
    u.fonts().drawEllipsized(0, 11, kW - 24 - 300, kH - 32, 300, "v1.0 • devkitA64 / libnx",
                             col.onSurfaceVariant.withAlpha(0.7f));
}

// ---------------------------------------------------------------- Home
void App::drawHome(ui::Ui& u) {
    auto& c = u.canvas();
    auto& th = u.theme();
    auto& col = th.colors;
    c.fillRect({0, 0, kW, kH}, col.surface);

    // ---- верхняя панель ----
    Rect tb{0, 0, kW, 72};
    c.fillRect(tb, col.surface);
    Rect mark{24, 20, 32, 32};
    c.fillRoundRect(mark, 10, col.primary);
    md3::iconCentered(u, "play_arrow", mark, 22, col.onPrimary);
    u.fonts().draw(2, 21, 68, 26, "TubeNX", col.onSurface);

    if (md3::iconButton(u, u.id("home.search"), Rect(kW - 112, 12, 44, 44), "search", col.onSurfaceVariant))
        go(Screen::Search);
    if (md3::iconButton(u, u.id("home.settings"), Rect(kW - 60, 12, 44, 44), "settings", col.onSurfaceVariant))
        go(Screen::Settings);
    if (probeMs > 0)
        c.fillCircle(kW - 122, 26, 3.5f, Color::rgb(0x4CAF50));
    if (u.input().x) go(Screen::Search);

    // ---- чипы ----
    std::vector<bool> sel(chips.size(), false);
    sel[(size_t)homeChip] = true;
    Rect chipRowRect{0, 76, kW, 48};
    int clicked = md3::chipRow(u, u.id("home.chips"), chipRowRect, chips, sel, &homeChip);
    if (clicked >= 0 && clicked != homeChip) {
        homeChip = clicked;
        if (clicked == 0) loadHome(false);
        else if (clicked == 1) loadHome(true);
        else startSearch(chips[(size_t)clicked]);
    }

    // ---- «Продолжить просмотр» + лента ----
    auto cont = continueWatching();
    const float stripH = cont.empty() ? 0.f : 148.f;

    Rect vp{24, 136, kW - 48, kH - 136 - 88};
    float gridH = gridContentHeight(feed, vp.w);
    float contentH = stripH + gridH;
    u.pushScroll(u.id("home.scroll"));
    bool nearEnd = false;
    float off = u.scrollArea(u.id("home.scroll"), vp, contentH, &nearEnd);
    c.pushClip(vp);
    if (stripH > 0.f) drawContinueStrip(u, Rect{vp.x, vp.y - off, vp.w, stripH}, cont);
    drawGrid(u, u.id("home.grid"), Rect{vp.x, vp.y - off + stripH, vp.w, gridH}, feed,
             feedLoading && feed.sections.empty(), feedError, "Лента пуста. Проверьте обход DPI.", off, contentH);
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

    if (md3::iconButton(u, u.id("search.back"), Rect(16, 16, 44, 44), "arrow_back", col.onSurface))
        back();

    // Поле поиска — Material 3 search bar
    Rect field{68, 14, kW - 68 - 24, 52};
    c.fillRoundRect(field, 26, col.surfaceContainerHigh);
    md3::iconCentered(u, "search", Rect(field.x + 16, field.y + 14, 24, 24), 24, col.onSurfaceVariant);
    if (query.empty())
        u.fonts().draw(0, 17, field.x + 52, field.y + 15, "Поиск на YouTube", col.onSurfaceVariant);
    else
        u.fonts().drawEllipsized(0, 17, field.x + 52, field.y + 15, field.w - 110, query, col.onSurface);
    if (!query.empty()) {
        if (md3::iconButton(u, u.id("search.clear"), Rect(field.r() - 48, field.y + 4, 44, 44), "close",
                            col.onSurfaceVariant)) {
            query.clear();
            showSuggestions = false;
            searchFeed = yt::Feed{};
        }
    }
    if (u.interact(u.id("search.field"), field)) {
        showSuggestions = false;
        std::string q = query;
        if (platform::keyboard(q, "Например: lo-fi hip hop", "Поиск на YouTube") && !q.empty()) {
            query = q;
            startSearch(q);
        }
    }

    float contentY = 84.f;
    if (showSuggestions && !suggestions.empty()) {
        float h = std::min(6, (int)suggestions.size()) * 48.f + 16.f;
        Rect list{68, contentY, kW - 92, h};
        c.shadow(list, CornerRadii(16), 24, 6, col.shadow.withAlpha(0.18f));
        c.fillRoundRect(list, 16, col.surfaceContainerHigh);
        for (size_t i = 0; i < suggestions.size() && i < 6; i++) {
            Rect row{list.x, list.y + 8 + (float)i * 48.f, list.w, 48.f};
            if (u.interact(u.id("sugg", (int)i), row)) {
                query = suggestions[i];
                startSearch(query);
                break;
            }
            md3::iconCentered(u, "history", Rect(row.x + 16, row.y + 12, 24, 24), 22, col.onSurfaceVariant);
            u.fonts().drawEllipsized(0, 16, row.x + 56, row.y + 13, row.w - 80, suggestions[i], col.onSurface);
        }
        return;
    }

    if (lastQuery.empty()) {
        // Подсказка-пустое состояние
        Rect hint{0, 200, kW, 200};
        md3::iconCentered(u, "search", Rect(kW * 0.5f - 32, hint.y, 64, 64), 64, col.surfaceVariant);
        u.fonts().drawCentered(1, 20, Rect(0, hint.y + 84, kW, 30), "Найдите что посмотреть", col.onSurface);
        u.fonts().drawCentered(0, 15, Rect(0, hint.y + 118, kW, 24),
                               "Нажмите на поле поиска — откроется клавиатура Switch", col.onSurfaceVariant);

        // История просмотров
        float y = 380.f;
        if (!history.empty()) {
            u.fonts().draw(1, 16, 68, y, "Недавние", col.onSurface);
            y += 34;
            for (size_t i = 0; i < history.size() && i < 4; i++) {
                Rect row{56, y, kW - 112, 56};
                md3::iconCentered(u, "history", Rect(row.x + 8, row.y + 14, 24, 24), 24, col.onSurfaceVariant);
                u.fonts().drawEllipsized(0, 15, row.x + 48, row.y + 8, row.w - 120, history[i].title, col.onSurface);
                u.fonts().drawEllipsized(0, 12, row.x + 48, row.y + 30, row.w - 120, history[i].channel,
                                         col.onSurfaceVariant);
                if (u.interact(u.id("hist", (int)i), row)) {
                    yt::VideoItem v;
                    v.id = history[i].id;
                    v.title = history[i].title;
                    v.channel = history[i].channel;
                    openVideo(v);
                    return;
                }
                y += 58;
            }
        }
        return;
    }

    // Результаты
    Rect vp{24, contentY, kW - 48, kH - contentY - 88};
    float contentH = gridContentHeight(searchFeed, vp.w);
    u.pushScroll(u.id("search.scroll"));
    float off = u.scrollArea(u.id("search.scroll"), vp, contentH, nullptr);
    c.pushClip(vp);
    drawGrid(u, u.id("search.grid"), Rect{vp.x, vp.y - off, vp.w, contentH}, searchFeed, searchLoading,
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

    // Пустое состояние / загрузка
    if (pl.loading || (!plFrame.valid() && pl.error.empty())) {
        md3::circularProgress(u, {kW * 0.5f, kH * 0.5f}, 22.f, -1.f, 3.f);
        u.fonts().drawCentered(0, 15, Rect(0, kH * 0.5f + 40, kW, 24), "Получаем потоки…", col.onSurfaceVariant);
    }

    if (!pl.error.empty()) {
        Rect card{kW * 0.5f - 320, kH * 0.5f - 130, 640, 260};
        c.fillRoundRect(card, 28, col.surfaceContainerHighest);
        md3::iconCentered(u, "error", Rect(card.x + 24, card.y + 24, 32, 32), 32, col.error);
        u.fonts().draw(1, 20, card.x + 68, card.y + 26, "Не удалось воспроизвести", col.onSurface);
        u.fonts().drawWrapped(0, 14, Rect(card.x + 24, card.y + 78, card.w - 48, 70), pl.error,
                              col.onSurfaceVariant, 3);
        if (md3::button(u, u.id("pl.retry"), Rect(card.x + 24, card.b() - 68, 180, 44), "Повторить",
                        {md3::ButtonKind::Tonal}))
            openVideo(pl.item);
        if (md3::button(u, u.id("pl.errback"), Rect(card.x + 220, card.b() - 68, 160, 44), "Назад",
                        {md3::ButtonKind::Text}))
            closePlayer();
        if (u.input().back) closePlayer();
        return;
    }

    bool show = pl.controls;

    if (show) {
        // Скримы
        c.fillLinearGradient({0, 0, kW, 190}, CornerRadii(0), Color::rgb(0x000000).withAlpha(0.72f),
                             Color::rgb(0x000000).withAlpha(0.f), false);
        c.fillLinearGradient({0, kH - 240, kW, 240}, CornerRadii(0), Color::rgb(0x000000).withAlpha(0.f),
                             Color::rgb(0x000000).withAlpha(0.86f), false);

        // Верхняя панель
        if (md3::iconButton(u, u.id("pl.back"), Rect(16, 16, 48, 48), "arrow_back", Color::rgb(0xFFFFFF)))
            closePlayer();
        std::string title = pl.details.title.empty() ? pl.item.title : pl.details.title;
        u.fonts().drawEllipsized(1, 19, 76, 20, kW - 300, title, Color::rgb(0xFFFFFF));
        std::string sub = pl.details.channel.empty() ? pl.item.channel : pl.details.channel;
        if (!pl.qualityLabel.empty()) sub += "  •  " + pl.qualityLabel;
        u.fonts().drawEllipsized(0, 13, 76, 46, kW - 300, sub, Color::rgb(0xFFFFFF).withAlpha(0.78f));

        // Центральная кнопка
        Rect play{0, 0, 84, 84};
        play.x = kW * 0.5f - 42;
        play.y = kH * 0.5f - 60;
        c.fillCircle(play.center().x, play.center().y, 42, Color::rgb(0x000000).withAlpha(0.35f));
        bool paused = p.paused() || !p.playing();
        if (md3::iconButton(u, u.id("pl.play"), play.inset(12), paused ? "play_arrow" : "pause",
                            Color::rgb(0xFFFFFF), 40.f)) {
            p.togglePause();
            pl.idle = 0.f;
        }

        // Нижние контролы
        double dur = p.duration();
        double pos = pl.seeking ? (pl.seekPos * dur) : p.position();
        float barY = kH - 124.f;

        std::string posS = timeFmt(pos);
        std::string durS = timeFmt(dur);
        float tw = u.fonts().measure(1, 13, "00:00");
        Rect seekRect{24 + tw + 12, barY, kW - 2 * (24 + tw + 12), 44};
        // Время по краям — на одной линии с дорожкой.
        u.fonts().draw(1, 13, 24, barY + 14, posS, Color::rgb(0xFFFFFF));
        u.fonts().drawEllipsized(1, 13, kW - 24 - tw, barY + 14, tw, durS, Color::rgb(0xFFFFFF));

        float val = dur > 0 ? (float)(pos / dur) : 0.f;
        val = std::clamp(val, 0.f, 1.f);
        if (md3::slider(u, u.id("pl.seek"), seekRect, val, 0.f, 1.f)) {
            pl.seeking = u.input().down;
            pl.seekPos = val;
            if (!u.input().down) {
                p.seek((double)val * dur);
                pl.seeking = false;
            }
        }
        pl.idle = 0.f;

        // Ряд кнопок
        float by = kH - 72.f;
        if (md3::iconButton(u, u.id("pl.rw"), Rect(20, by, 44, 44), "replay_10", Color::rgb(0xFFFFFF)))
            p.seekRelative(-10.0);
        if (md3::iconButton(u, u.id("pl.ff"), Rect(68, by, 44, 44), "forward_10", Color::rgb(0xFFFFFF)))
            p.seekRelative(10.0);

        // Громкость
        int vol = p.volume();
        if (md3::iconButton(u, u.id("pl.mute"), Rect(132, by, 44, 44), vol == 0 ? "volume_off" : "volume_up",
                            Color::rgb(0xFFFFFF)))
            p.setVolume(vol == 0 ? 100 : 0);
        float vv = std::clamp(vol / 130.f, 0.f, 1.f);
        if (md3::slider(u, u.id("pl.vol"), Rect(178, by, 140, 44), vv, 0.f, 1.f))
            p.setVolume((int)(vv * 130.f));

        // Качество и состояние обхода — справа, перед «ещё».
        float moreX = kW - 20.f - 44.f;
        bool proxyOn = !net::proxy().empty();
        const char* stText = proxyOn ? "Обход DPI: прокси" : "SNI-обход";
        float dw = u.fonts().measure(1, 13, stText) + 54.f;
        Rect dchip{moreX - 8.f - dw, by + 4, dw, 36};
        c.fillRoundRect(dchip, 18, Color::rgb(0xFFFFFF).withAlpha(0.14f));
        c.fillCircle(dchip.x + 18, dchip.center().y, 4,
                     proxyOn ? Color::rgb(0x66E08A) : Color::rgb(0xFFFFFF).withAlpha(0.5f));
        u.fonts().draw(1, 13, dchip.x + 32, dchip.y + 9, stText, Color::rgb(0xFFFFFF));

        std::string qs = pl.qualityLabel.empty() ? "auto" : pl.qualityLabel;
        float qw = u.fonts().measure(1, 13, qs) + 44.f;
        Rect qchip{dchip.x - 8.f - qw, by + 4, qw, 36};
        c.fillRoundRect(qchip, 18, Color::rgb(0xFFFFFF).withAlpha(0.14f));
        md3::iconCentered(u, "high_quality", Rect(qchip.x + 10, qchip.y + 8, 20, 20), 20, Color::rgb(0xFFFFFF));
        u.fonts().draw(1, 13, qchip.x + 34, qchip.y + 9, qs, Color::rgb(0xFFFFFF));

        // Локальный «лайк» — усиливает канал в рекомендациях (никуда не отправляется).
        bool liked = isLiked(pl.item.id);
        Color likeBg = col.primary.withAlpha(0.26f);
        if (md3::iconButton(u, u.id("pl.like"), Rect(qchip.x - 52.f, by, 44, 44), "thumb_up",
                            liked ? col.primary : Color::rgb(0xFFFFFF), 24.f, liked, &likeBg))
            toggleLike(pl.item.id);

        if (md3::iconButton(u, u.id("pl.more"), Rect(moreX, by, 44, 44), "more_vert", Color::rgb(0xFFFFFF)))
            toast("Похожие видео — в разработке");

        // Информация о буферизации
        if (p.buffering()) {
            Rect bc{kW * 0.5f - 110, 120, 220, 36};
            c.fillRoundRect(bc, 18, Color::rgb(0x000000).withAlpha(0.55f));
            md3::circularProgress(u, {bc.x + 26, bc.center().y}, 8, -1.f, 2.f);
            u.fonts().draw(0, 13, bc.x + 44, bc.y + 9, "Буферизация…", Color::rgb(0xFFFFFF));
        }
    } else {
        // Свёрнутые контролы: только тонкая полоса прогресса сверху
        double dur = p.duration(), pos = p.position();
        if (dur > 0) {
            float p01 = (float)std::clamp(pos / dur, 0.0, 1.0);
            c.fillRect({0, 0, kW, 3}, Color::rgb(0xFFFFFF).withAlpha(0.18f));
            c.fillRect({0, 0, kW * p01, 3}, col.primary);
        }
        if (u.input().accept || u.input().pressed) pl.controls = true;
        pl.idle = 0.f;
    }

    // Горячие клавиши геймпада
    if (u.input().left) p.seekRelative(-10.0);
    if (u.input().right) p.seekRelative(10.0);
    if (u.input().bumpL) p.seekRelative(-30.0);
    if (u.input().bumpR) p.seekRelative(30.0);
    if (u.input().y) p.togglePause();
    if (u.input().back) closePlayer();
}

// ---------------------------------------------------------------- Settings
void App::drawSettings(ui::Ui& u) {
    auto& c = u.canvas();
    auto& th = u.theme();
    auto& col = th.colors;
    c.fillRect({0, 0, kW, kH}, col.surface);

    if (md3::iconButton(u, u.id("set.back"), Rect(16, 16, 44, 44), "arrow_back", col.onSurface)) back();
    u.fonts().draw(2, 24, 72, 18, "Настройки", col.onSurface);

    Rect vp{0, 72, kW, kH - 72 - 88};
    float pad = 24.f;
    float colW = 560.f;
    float colX[2] = {pad, pad + colW + 24.f};

    // Содержимое в две колонки; высота больше вьюпорта — поэтому скроллим.
    const float contentH = 846.f;
    float off = u.scrollArea(u.id("set.scroll"), vp, contentH);
    float y[2] = {vp.y + 8 - off, vp.y + 8 - off};
    c.pushClip(vp);

    auto section = [&](int colIdx, const char* title) {
        u.fonts().draw(1, 13, colX[colIdx], y[colIdx], title, col.primary);
        y[colIdx] += 22;
    };

    // ---------- Обход DPI ----------
    int k = 0;
    section(k, "ОБХОД DPI  •  BYEBYEDPI");
    {
        Rect card{colX[k], y[k], colW, 292};
        md3::surfaceCard(u, card, md3::CardKind::Filled, 20.f);
        u.fonts().draw(1, 16, card.x + 20, card.y + 16, "Обход блокировок", col.onSurface);
        u.fonts().drawWrapped(0, 13, Rect(card.x + 20, card.y + 42, card.w - 100, 40),
                              "Фрагментация TLS ClientHello против DPI-троттлинга YouTube",
                              col.onSurfaceVariant, 2);
        bool en = cfg.dpiEnabled;
        if (md3::switchWidget(u, u.id("set.dpi"), Rect(card.r() - 76, card.y + 16, 52, 32), en))
            setDpiEnabled(en);

        const char* modes[] = {"Split", "Multi", "Disorder", "TLS"};
        std::vector<std::string> ml(modes, modes + 4);
        Rect seg{card.x + 20, card.y + 92, card.w - 40, 40};
        int mi = cfg.dpiMode;
        if (mi < 0 || mi > 3) mi = 1;
        if (md3::segmented(u, u.id("set.dpimode"), seg, ml, mi)) {
            cfg.dpiMode = mi;
            dpi::setMode((dpi::Mode)mi);
            saveConfig();
        }

        Rect probeBtn{card.x + 20, card.y + 148, 200, 44};
        if (md3::button(u, u.id("set.probe"), probeBtn, probing ? "Проверяем…" : "Проверить доступ",
                        {md3::ButtonKind::Tonal, !probing, "wifi"}))
            probe(true);

        if (probeMs > 0) {
            char buf[96];
            snprintf(buf, sizeof(buf), "YouTube: %d мс", probeMs);
            u.fonts().draw(1, 14, card.x + 236, card.y + 164, buf, Color::rgb(0x4CAF50));
        } else {
            u.fonts().draw(0, 13, card.x + 236, card.y + 164, "нет соединения", col.error);
        }

        auto& st = dpi::stats();
        char buf[192];
        snprintf(buf, sizeof(buf), "Соединений: %llu   •   фрагментов: %llu   •   трафик: %.1f МБ",
                 (unsigned long long)st.connections, (unsigned long long)st.fragmented,
                 st.relayedBytes / 1048576.0);
        u.fonts().draw(0, 12, card.x + 20, card.y + 208, buf, col.onSurfaceVariant);
        bool proxyOn = !net::proxy().empty();
        u.fonts().draw(0, 12, card.x + 20, card.y + 228,
                       proxyOn ? ("Прокси: " + net::proxy()) : std::string("Прокси отключён"),
                       col.onSurfaceVariant.withAlpha(0.8f));
        u.fonts().draw(0, 12, card.x + 20, card.y + 250,
                       std::string("SNI-обход: вкл  •  ") + (net::verify() == net::Verify::Full
                                                                    ? "TLS: имя + цепочка"
                                                                    : (net::verify() == net::Verify::ChainOnly
                                                                           ? "TLS: только цепочка"
                                                                           : "TLS: без проверок")),
                       Color::rgb(0x4CAF50));
        if (!feedError.empty())
            u.fonts().drawWrapped(0, 12, Rect(card.x + 20, card.y + 268, card.w - 40, 20),
                                  "Последняя ошибка: " + feedError, col.error, 1);
    }
    y[k] += 308;

    // ---------- Оформление ----------
    section(k, "ОФОРМЛЕНИЕ  •  MATERIAL 3");
    {
        Rect card{colX[k], y[k], colW, 200};
        md3::surfaceCard(u, card, md3::CardKind::Filled, 20.f);
        u.fonts().draw(1, 16, card.x + 20, card.y + 16, "Тёмная тема", col.onSurface);
        u.fonts().draw(0, 13, card.x + 20, card.y + 42, "M3 tonal palette, автоматически", col.onSurfaceVariant);
        bool dark = cfg.dark;
        if (md3::switchWidget(u, u.id("set.dark"), Rect(card.r() - 76, card.y + 16, 52, 32), dark)) {
            cfg.dark = dark;
            applyTheme();
            saveConfig();
        }

        u.fonts().draw(1, 13, card.x + 20, card.y + 78, "Цвет-источник (Material You)", col.onSurface);
        for (int i = 0; i < 6; i++) {
            Rect sw{card.x + 20 + i * 48.f, card.y + 102, 36, 36};
            bool active = cfg.seed == seedList[i];
            c.fillRoundRect(sw, 18, Color::rgb(seedList[i] & 0xFFFFFF));
            if (active) {
                c.strokeRoundRect(sw.inset(-3), CornerRadii(21), 2.5f, col.onSurface);
                md3::iconCentered(u, "check", sw, 20, Color::rgb(0xFFFFFF));
            }
            if (u.interact(u.id("set.seed", i), sw)) {
                cfg.seed = seedList[i];
                applyTheme();
                saveConfig();
                toast(std::string("Тема: ") + seedNames[i]);
            }
        }
        u.fonts().draw(0, 12, card.x + 20, card.y + 152,
                       "Google Sans • M3 Expressive shapes • 60 fps software UI", col.onSurfaceVariant);
    }
    y[k] += 216;

    // ---------- Локальные рекомендации ----------
    section(k, "РЕКОМЕНДАЦИИ  •  ТОЛЬКО НА КОНСОЛИ");
    {
        Rect card{colX[k], y[k], colW, 132};
        md3::surfaceCard(u, card, md3::CardKind::Filled, 20.f);
        u.fonts().draw(1, 16, card.x + 20, card.y + 16, "Локальные рекомендации", col.onSurface);
        bool reco = cfg.localReco;
        if (md3::switchWidget(u, u.id("set.reco"), Rect(card.r() - 76, card.y + 16, 52, 32), reco)) {
            cfg.localReco = reco;
            saveConfig();
            rankFeed();
            toast(reco ? "Лента учитывает историю" : "Лента без персонализации");
        }
        u.fonts().drawWrapped(0, 13, Rect(card.x + 20, card.y + 44, card.w - 140, 40),
                              "Лента сортируется по вашей истории на этой консоли. "
                              "Данные не отправляются в Google.",
                              col.onSurfaceVariant, 2);
        char buf[128];
        snprintf(buf, sizeof(buf), "Сигналов: %d каналов  •  %d видео в истории", recoSignals(),
                 (int)history.size());
        u.fonts().draw(0, 12, card.x + 20, card.y + 104, buf, col.onSurfaceVariant.withAlpha(0.8f));
    }
    y[k] += 148;

    // ---------- Плеер ----------
    int k2 = 1;
    section(k2, "ПЛЕЕР");
    {
        Rect card{colX[k2], y[k2], colW, 176};
        md3::surfaceCard(u, card, md3::CardKind::Filled, 20.f);
        u.fonts().draw(1, 16, card.x + 20, card.y + 16, "Качество видео", col.onSurface);
        std::vector<std::string> q = {"Авто", "720p", "480p", "360p"};
        int qi = std::clamp(cfg.quality, 0, 3);
        if (md3::segmented(u, u.id("set.q"), Rect(card.x + 20, card.y + 44, card.w - 40, 40), q, qi)) {
            cfg.quality = qi;
            saveConfig();
        }
        u.fonts().draw(1, 16, card.x + 20, card.y + 100, "Скорость", col.onSurface);
        std::vector<std::string> sp = {"0.75x", "1x", "1.25x", "1.5x"};
        int si = cfg.speed == 75 ? 0 : (cfg.speed == 100 ? 1 : (cfg.speed == 125 ? 2 : 3));
        if (md3::segmented(u, u.id("set.speed"), Rect(card.x + 20, card.y + 128, card.w - 40, 36), sp, si)) {
            cfg.speed = si == 0 ? 75 : (si == 1 ? 100 : (si == 2 ? 125 : 150));
            player::instance().setSpeed(cfg.speed / 100.0);
            saveConfig();
        }
    }
    y[k2] += 192;

    // ---------- Звук ----------
    section(k2, "ЗВУК  •  СИНХРОНИЗАЦИЯ С ВИДЕО");
    {
        Rect card{colX[k2], y[k2], colW, 132};
        md3::surfaceCard(u, card, md3::CardKind::Filled, 20.f);
        u.fonts().draw(1, 16, card.x + 20, card.y + 14, "Сдвиг звука", col.onSurface);
        char val[64];
        snprintf(val, sizeof(val), "%+d мс", cfg.audioDelayMs);
        u.fonts().draw(1, 14, card.r() - 120, card.y + 16, val, col.primary);
        int ad = cfg.audioDelayMs;
        if (md3::button(u, u.id("set.adelay.minus"), Rect(card.x + 20, card.y + 48, 64, 40), "-50",
                        {md3::ButtonKind::Tonal}))
            ad -= 50;
        if (md3::button(u, u.id("set.adelay.plus"), Rect(card.x + 92, card.y + 48, 64, 40), "+50",
                        {md3::ButtonKind::Tonal}))
            ad += 50;
        if (md3::button(u, u.id("set.adelay.reset"), Rect(card.x + 164, card.y + 48, 128, 40), "Сброс",
                        {md3::ButtonKind::Text}))
            ad = 0;
        if (ad != cfg.audioDelayMs) {
            cfg.audioDelayMs = std::clamp(ad, -1000, 1000);
            player::instance().setAudioDelay(cfg.audioDelayMs / 1000.0);
            saveConfig();
            toast("Сдвиг звука: " + std::to_string(cfg.audioDelayMs) + " мс");
        }
        u.fonts().drawWrapped(0, 12, Rect(card.x + 20, card.y + 96, card.w - 40, 30),
                              "Голос отстаёт от картинки — увеличьте, спешит — уменьшите. "
                              "Применяется сразу, шаг 50 мс.",
                              col.onSurfaceVariant, 2);
    }
    y[k2] += 148;

    // ---------- Прокси ----------
    section(k2, "СЕТЬ  •  ПРОКСИ ДЛЯ ВИДЕО");
    {
        Rect card{colX[k2], y[k2], colW, 184};
        md3::surfaceCard(u, card, md3::CardKind::Filled, 20.f);
        u.fonts().draw(1, 16, card.x + 20, card.y + 16, "Внешний прокси", col.onSurface);
        std::string shown = cfg.proxy.empty() ? "не задан — только обход по SNI" : cfg.proxy;
        u.fonts().drawEllipsized(0, 14, card.x + 20, card.y + 46, card.w - 40, shown,
                                 cfg.proxy.empty() ? col.onSurfaceVariant : col.primary);
        u.fonts().drawWrapped(0, 12, Rect(card.x + 20, card.y + 70, card.w - 40, 32),
                              "Нужен, если провайдер блокирует сам CDN видео (googlevideo) по IP: "
                              "без прокси такие ссылки не открываются вообще.",
                              col.onSurfaceVariant, 2);
        Rect b1{card.x + 20, card.y + 116, 170, 40};
        if (md3::button(u, u.id("set.proxy"), b1, cfg.proxy.empty() ? "Указать" : "Изменить",
                        {md3::ButtonKind::Filled}))
        {
            std::string p = cfg.proxy.empty() ? std::string("socks5h://192.0.2.10:1080") : cfg.proxy;
            if (platform::keyboard(p, "socks5h://host:port", "Прокси для видео")) {
                cfg.proxy = normalizeProxy(p);
                saveConfig();
                // Применяем сразу — и для API, и для ретранслятора видео.
                net::setProxy(cfg.proxy);
                if (cfg.proxy.empty() && cfg.dpiEnabled && dpi::socksPort() > 0)
                    net::setProxy("socks5h://127.0.0.1:" + std::to_string(dpi::socksPort()));
                toast(cfg.proxy.empty() ? "Прокси отключён" : "Прокси сохранён");
            }
        }
        if (!cfg.proxy.empty()) {
            Rect b2{card.x + 202, card.y + 116, 160, 40};
            if (md3::button(u, u.id("set.proxy.clear"), b2, "Сбросить", {md3::ButtonKind::Text, true, "close"})) {
                cfg.proxy.clear();
                saveConfig();
                net::setProxy("");
                if (cfg.dpiEnabled && dpi::socksPort() > 0)
                    net::setProxy("socks5h://127.0.0.1:" + std::to_string(dpi::socksPort()));
                toast("Прокси отключён");
            }
        }
        u.fonts().draw(0, 12, card.x + 20, card.y + 162,
                       "Подойдёт любой SOCKS5/HTTP-прокси (например, ПК с VPN в той же сети)",
                       col.onSurfaceVariant.withAlpha(0.75f));
    }
    y[k2] += 200;

    // ---------- Система ----------
    section(k2, "СИСТЕМА");
    {
        auto inf = platform::info();
        Rect card{colX[k2], y[k2], colW, 132};
        md3::surfaceCard(u, card, md3::CardKind::Filled, 20.f);
        char buf[128];
        snprintf(buf, sizeof(buf), "Прошивка %s  •  %s", inf.firmware.empty() ? "?" : inf.firmware.c_str(),
                 inf.docked ? "Docked" : "Handheld");
        u.fonts().draw(0, 14, card.x + 20, card.y + 18, buf, col.onSurface);
        snprintf(buf, sizeof(buf), "Сеть: %s", inf.wifi ? "подключена" : "нет");
        u.fonts().draw(0, 14, card.x + 20, card.y + 44, buf, col.onSurfaceVariant);
        if (inf.battery >= 0) {
            snprintf(buf, sizeof(buf), "Батарея: %d%%%s", inf.battery, inf.charging ? "  (заряжается)" : "");
            u.fonts().draw(0, 14, card.x + 20, card.y + 70, buf, col.onSurfaceVariant);
        }
        u.fonts().draw(0, 12, card.x + 20, card.y + 100, "libnx • devkitA64 • libmpv (software render)",
                       col.onSurfaceVariant.withAlpha(0.75f));
    }
    y[k2] += 148;

    // ---------- О приложении ----------
    section(k2, "О ПРИЛОЖЕНИИ");
    {
        Rect card{colX[k2], y[k2], colW, 120};
        md3::surfaceCard(u, card, md3::CardKind::Filled, 20.f);
        u.fonts().draw(2, 18, card.x + 20, card.y + 16, "TubeNX 1.0", col.onSurface);
        u.fonts().draw(0, 13, card.x + 20, card.y + 44,
                       "YouTube-клиент для Nintendo Switch. Не является официальным приложением YouTube.",
                       col.onSurfaceVariant);
        if (md3::button(u, u.id("set.exit"), Rect(card.x + 20, card.y + 68, 180, 40), "Выход",
                        {md3::ButtonKind::Text, true, "logout"}))
            platform::requestExit();
    }

    c.popClip();
}

// ---------------------------------------------------------------- Chrome
void App::drawChrome(ui::Ui& u) {
    auto& c = u.canvas();
    auto& th = u.theme();
    auto& col = th.colors;
    if (screen == Screen::Splash || screen == Screen::Player) return;

    Rect bar{0, kH - 88, kW, 88};
    // Мягкое затухание контента к панели навигации (иначе карточки «режутся» линией).
    c.fillLinearGradient({0, bar.y - 40, kW, 40}, CornerRadii(0), col.surface.withAlpha(0.f),
                         col.surface.withAlpha(1.f), false);
    c.fillRect(bar, col.surfaceContainer);
    c.fillRect({0, bar.y, kW, 1}, col.outlineVariant.withAlpha(0.5f));

    std::vector<std::string> labels = {"Главная", "Поиск", "Настройки"};
    std::vector<std::string> icons = {"home", "search", "settings"};
    int sel = screen == Screen::Home ? 0 : (screen == Screen::Search ? 1 : 2);
    int picked = sel;
    navIndex = sel;
    if (md3::navBar(u, u.id("nav"), Rect(bar.x, bar.y + 4, bar.w, 76), labels, icons, picked)) {
        if (picked != sel) {
            if (picked == 0) go(Screen::Home);
            else if (picked == 1) go(Screen::Search);
            else go(Screen::Settings);
        }
    }
}

void App::drawToasts(ui::Ui& u) {
    if (toastT <= 0.f || toastText.empty()) return;
    md3::snackbar(u, toastText, std::min(1.f, (2.6f - toastT) * 4.f));
}

// ---------------------------------------------------------------- draw
void App::draw(ui::Ui& u) {
    switch (screen) {
    case Screen::Splash: drawSplash(u); break;
    case Screen::Home: drawHome(u); break;
    case Screen::Search: drawSearch(u); break;
    case Screen::Player: drawPlayer(u); break;
    case Screen::Settings: drawSettings(u); break;
    }
    drawChrome(u);
    drawToasts(u);
}

} // namespace tnx::app