// tools/preview.cpp — offscreen-рендер экранов TubeNX в PNG на Linux-хосте.
// Собирается обычным g++ (см. tools/preview.sh). Нужен только для проверки дизайна.
#include "core/canvas.hpp"
#include "md3/ui.hpp"
#include "ui/app.hpp"

#include <cstdio>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

using namespace tnx;

static std::vector<uint32_t> g_buf(1280 * 720);

static void renderFrame(app::App& app, const char* path, const ui::Input& in, int frames = 4) {
    Canvas canvas;
    canvas.attach(g_buf.data(), 1280, 720, 1280);
    app.fonts.setCanvas(&canvas);

    ui::Ui u;
    ui::Input din = in;
    for (int i = 0; i < frames; i++) {
        din.time = i * 0.016f;
        din.dt = 0.016f;
        u.beginFrame(canvas, app.theme, app.fonts, din);
        app.draw(u);
        u.endFrame();
    }
    // RGBA order: пишем как есть (0xAABBGGRR → байты R,G,B,A)
    stbi_write_png(path, 1280, 720, 4, g_buf.data(), 1280 * 4);
    printf("wrote %s\n", path);
}

int main(int argc, char** argv) {
    const char* outDir = argc > 1 ? argv[1] : "/tmp/tubenx";

    app::App app;
    app.init();
    app.feed = app.yt.home();
    app.feedOk = true;
    app.bootDone = true;
    app.probeMs = 96;
    app.probeOk = true;
    app.splashT = 3.2f;
    app.cfg.dark = true;
    app.applyTheme();

    // Демо-история: проверяем «Продолжить просмотр» и локальное ранжирование.
    app.history.push_back({"demo0", "Lo-Fi Hip Hop Radio — beats to relax/study to", "Lofi Girl", 214.0, 754.0, 0.0, true});
    app.history.push_back({"demo3", "Обход DPI в 2026: что реально работает", "Network Notes", 133.0, 640.0, 0.0, false});
    app.history.push_back({"demo6", "Собираем homebrew NRO с devkitPro за 10 минут", "switchbrew", 12.0, 600.0, 0.0, false});
    app.rankFeed();

    ui::Input in;
    in.pointerVisible = false;
    in.time = 1.0f;

    // 1. Splash
    app.screen = app::Screen::Splash;
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/01_splash.png", outDir);
        renderFrame(app, p, in, 30);
    }

    // 2. Home
    app.screen = app::Screen::Home;
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/02_home.png", outDir);
        ui::Input h = in;
        h.scrollY = -140.f;   // немного прокрутим, чтобы видеть два ряда
        renderFrame(app, p, h, 40);
    }

    // 3. Search с результатами
    app.screen = app::Screen::Search;
    app.query = "material you";
    app.lastQuery = "material you";
    app.searchFeed = app.yt.home();
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/03_search.png", outDir);
        renderFrame(app, p, in, 20);
    }

    // 4. Поиск с подсказками
    app.suggestions = app.yt.suggestions("lofi");
    app.showSuggestions = true;
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/04_suggest.png", outDir);
        renderFrame(app, p, in, 10);
    }
    app.showSuggestions = false;

    // 5. Плеер
    app.screen = app::Screen::Player;
    app.pl.visible = true;
    app.pl.loading = false;
    app.pl.controls = true;
    app.pl.qualityLabel = "720p";
    app.pl.details = tnx::yt::VideoDetails{};
    app.pl.details.title = "Lo-Fi Hip Hop Radio — beats to relax/study to";
    app.pl.details.channel = "Lofi Girl";
    app.plFrame = Image{};
    player::instance().update(app.plFrame);
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/05_player.png", outDir);
        renderFrame(app, p, in, 10);
    }

    // 6. Настройки
    app.screen = app::Screen::Settings;
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/06_settings.png", outDir);
        renderFrame(app, p, in, 45);
    }

    // 7. Светлая тема, главная
    app.cfg.dark = false;
    app.applyTheme();
    app.screen = app::Screen::Home;
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/07_home_light.png", outDir);
        renderFrame(app, p, in, 20);
    }

    printf("done\n");
    return 0;
}