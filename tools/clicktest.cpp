// tools/clicktest.cpp — проверка, что кнопки реально ловят тап (press+release).
// Собирается как preview.sh; печатает результаты проверок в stdout.
#include "core/canvas.hpp"
#include "md3/ui.hpp"
#include "ui/app.hpp"

#include <cstdio>
#include <vector>

using namespace tnx;
using tnx::app::Screen;

static std::vector<uint32_t> g_buf(1280 * 720);

// Отрисовывает кадр с заданным вводом и возвращает «как есть» после draw.
// Важно: один Ui на все кадры — как в main.cpp (иначе activeId_ не переживёт кадр).
static void frame(ui::Ui& u, app::App& app, const ui::Input& in) {
    static Canvas canvas;
    canvas.attach(g_buf.data(), 1280, 720, 1280);
    app.fonts.setCanvas(&canvas);
    u.beginFrame(canvas, app.theme, app.fonts, in);
    app.draw(u);
    u.endFrame();
}

struct TapResult {
    bool took = false;
    std::string what;
};

// Тап по точке: hover → press → release.
static void tap(ui::Ui& u, app::App& app, Vec2 p) {
    ui::Input in;
    in.pointerVisible = true;
    in.pointer = p;

    in.time = 1.00f;
    frame(u, app, in);
    in.time = 1.02f;
    in.down = true;
    in.pressed = true;
    frame(u, app, in);
    in.time = 1.04f;
    in.pressed = false;
    in.released = true;
    in.down = false;
    frame(u, app, in);
}

int main() {
    app::App app;
    app.init();
    ui::Ui ui;
    app.applyTheme();
    app.bootDone = true;
    app.screen = Screen::Home;
    app.feedOk = false;
    app.feedLoading = false;
    app.feedError = "Сеть недоступна. Проверьте обход DPI.";

    // Кнопка «Повторить»: карточка ошибки area={24,136}, h=268, btn.y=card.b()-68=456.
    tap(ui, app, {464.f, 478.f});
    printf("retry: feedLoading=%d feedError='%s'\n", app.feedLoading ? 1 : 0, app.feedError.c_str());
    printf("retry works: %s\n", app.feedLoading ? "YES" : "NO");

    // Кнопка «Настройки» в той же карточке.
    app.feedLoading = false;
    app.feedError = "Сеть недоступна. Проверьте обход DPI.";
    app.screen = Screen::Home;
    tap(ui, app, {680.f, 478.f});
    printf("settings btn: screen=%d (%s)\n", (int)app.screen,
           app.screen == Screen::Settings ? "OK" : "FAIL");

    // Настройки: тап по переключателю «Обход DPI».
    app.screen = Screen::Settings;
    bool before = app.cfg.dpiEnabled;
    tap(ui, app, {540.f, 300.f});
    printf("settings tap: dpi %d -> %d\n", (int)before, (int)app.cfg.dpiEnabled);

    // Геймпад без курсора: A должна сразу попадать в «Повторить».
    app.screen = Screen::Home;
    app.feedLoading = false;
    app.feedOk = false;
    app.feedError = "Сеть недоступна. Проверьте обход DPI.";
    ui::Ui ui2;
    ui::Input gp;
    gp.pointerVisible = false;
    gp.time = 2.00f;
    frame(ui2, app, gp);
    gp.time = 2.02f;
    gp.accept = true;
    frame(ui2, app, gp);
    printf("gamepad A: feedLoading=%d (%s)\n", app.feedLoading ? 1 : 0,
           app.feedLoading ? "OK" : "FAIL");
    return 0;
}