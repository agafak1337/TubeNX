// Бенчмарк отрисовки сплэша на хосте: проверяем, что кэш фона реально
// убирает ~130 мс с кадра.
#include "core/canvas.hpp"
#include "md3/ui.hpp"
#include "ui/app.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace tnx;

int main() {
    static std::vector<uint32_t> buf(1280 * 720);
    Canvas canvas;
    canvas.attach(buf.data(), 1280, 720, 1280);

    app::App app;
    app.fonts.setCanvas(&canvas);
    app.cfg.dark = true;
    app.applyTheme();
    app.screen = app::Screen::Splash;

    ui::Ui u;
    ui::Input in;
    in.pointerVisible = false;

    for (int i = 0; i < 12; i++) {
        in.time = i * 0.016f;
        in.dt = 0.016f;
        auto t0 = std::chrono::steady_clock::now();
        u.beginFrame(canvas, app.theme, app.fonts, in);
        app.draw(u);
        u.endFrame();
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("frame %2d: %.1f ms\n", i, ms);
    }
    return 0;
}