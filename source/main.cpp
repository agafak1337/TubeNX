// main.cpp — точка входа NRO: libnx, framebuffer, ввод, главный цикл.
#include <switch.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/canvas.hpp"
#include "core/font.hpp"
#include "dpi/dpi.hpp"
#include "md3/theme.hpp"
#include "md3/ui.hpp"
#include "net/http.hpp"
#include "net/images.hpp"
#include "platform.hpp"
#include "player/player.hpp"
#include "ui/app.hpp"

namespace {

Framebuffer g_fb;
NWindow* g_win = nullptr;
tnx::app::App* g_app = nullptr;
volatile bool g_quit = false;
AppletHookCookie g_hookCookie;

// --- повтор клавиш (D-pad) ---
struct Repeat {
    u64 mask = 0;
    float timer = 0.f;
    float delay = 0.f;
};


void onExit(AppletHookType type, void*) {
    if (type == AppletHookType_OnExitRequest) g_quit = true;
}

inline float deadzone(float v, float dz = 0.22f) {
    if (std::fabs(v) < dz) return 0.f;
    float s = (v - (v > 0 ? dz : -dz)) / (1.f - dz);
    return std::clamp(s, -1.f, 1.f);
}

} // namespace

namespace tnx::platform {

void bootLog(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // Лог пишут и фоновые потоки (ретранслятор, сеть) — без мьютекса строки
    // могли перемешиваться или дважды открывать файл.
    static std::mutex logMtx;
    std::lock_guard<std::mutex> lk(logMtx);

    printf("[TubeNX] %s\n", buf);
    fflush(stdout);

    // Первые строки пишутся до fsdevMountSdmc — файл тогда ещё не открыть.
    // Копим их в памяти и выливаем, как только SD станет доступна.
    static FILE* log = nullptr;
    static std::string pending;
    if (!log) {
        pending += buf;
        pending += '\n';
        log = fopen("sdmc:/switch/TubeNX-boot.log", "w");
        if (!log) log = fopen("sdmc:/TubeNX-boot.log", "w");
        if (!log) {
            if (pending.size() > (1u << 16)) pending.erase(0, pending.size() - (1u << 15));
            return;
        }
        fputs(pending.c_str(), log);
        pending.clear();
        fflush(log);
        return;
    }
    fprintf(log, "%s\n", buf);
    fflush(log);
}

bool keyboard(std::string& io, const char* guide, const char* header, bool password) {
    SwkbdConfig cfg;
    if (R_FAILED(swkbdCreate(&cfg, 0))) return false;
    swkbdConfigMakePresetDefault(&cfg);
    if (header && *header) swkbdConfigSetHeaderText(&cfg, header);
    if (guide && *guide) swkbdConfigSetGuideText(&cfg, guide);
    if (password) swkbdConfigSetPasswordFlag(&cfg, true);
    swkbdConfigSetInitialText(&cfg, io.c_str());
    char out[1024] = {0};
    Result rc = swkbdShow(&cfg, out, sizeof(out));
    swkbdClose(&cfg);
    if (R_FAILED(rc)) return false;
    io = out;
    return true;
}

void requestExit() { g_quit = true; }

Info info() {
    Info i;
    SetSysFirmwareVersion fw{};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%u.%u.%u", fw.major, fw.minor, fw.micro);
        i.firmware = buf;
    }
    i.docked = appletGetOperationMode() == AppletOperationMode_Console;
    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
        NifmInternetConnectionType type;
        u32 strength;
        NifmInternetConnectionStatus status;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)))
            i.wifi = status == NifmInternetConnectionStatus_Connected;
        nifmExit();
    }
    if (R_SUCCEEDED(psmInitialize())) {
        u32 charge = 0;
        psmGetBatteryChargePercentage(&charge);
        i.battery = (int)charge;
        PsmChargerType ct = PsmChargerType_Unconnected;
        if (R_SUCCEEDED(psmGetChargerType(&ct)))
            i.charging = ct == PsmChargerType_EnoughPower || ct == PsmChargerType_LowPower;
        psmExit();
    }
    i.model = "Nintendo Switch";
    return i;
}

} // namespace tnx::platform

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    using tnx::platform::bootLog;
    bootLog("=== TubeNX 1.0 ===");
    // Метка сборки: по логу сразу видно, запущена ли свежая версия.
    bootLog("build: %s %s (SNI-обход: регистр + подмена, SOCKS 16)",
            __DATE__, __TIME__);
    bootLog("argv0=%s", (argv && argv[0]) ? argv[0] : "(null)");
    bootLog("mode=%s", appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");

    // --- базовые сервисы (каждый шаг проверяем и логируем) ---
    Result rc = fsInitialize();
    bootLog("fsInitialize: 0x%x", rc);
    rc = fsdevMountSdmc();
    bootLog("fsdevMountSdmc: 0x%x", rc);
    rc = romfsInit();
    bootLog("romfsInit: 0x%x", rc);
    rc = socketInitializeDefault();
    bootLog("socketInitializeDefault: 0x%x", rc);
    rc = nifmInitialize(NifmServiceType_User);
    bootLog("nifmInitialize: 0x%x", rc);
    if (R_SUCCEEDED(rc)) {
        NifmInternetConnectionType wtype;
        u32 wstrength = 0;
        NifmInternetConnectionStatus wstatus;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&wtype, &wstrength, &wstatus))) {
            // 0-3: по этому числу сразу видно, не в слабом ли Wi-Fi дело.
            bootLog("wifi: тип=%d сигнал=%u/3 статус=%d", (int)wtype, wstrength, (int)wstatus);
        }
    }

    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    bootLog("hidInitializeTouchScreen: ok");

    appletSetMediaPlaybackState(true);
    appletHook(&g_hookCookie, onExit, nullptr);
    // На Switch 2 / новых прошивках даём приложению больше CPU-времени на декод.
    rc = appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    bootLog("appletSetCpuBoostMode: 0x%x", rc);

    // --- экран: пробуем 720p x2, затем 720p x1, затем 540p ---
    g_win = nwindowGetDefault();
    nwindowSetDimensions(g_win, 1280, 720);
    rc = framebufferCreate(&g_fb, g_win, 1280, 720, PIXEL_FORMAT_RGBA_8888, 2);
    bootLog("framebufferCreate 1280x720x2: 0x%x", rc);
    if (R_FAILED(rc)) {
        rc = framebufferCreate(&g_fb, g_win, 1280, 720, PIXEL_FORMAT_RGBA_8888, 1);
        bootLog("framebufferCreate 1280x720x1: 0x%x", rc);
    }
    if (R_FAILED(rc)) {
        bootLog("НЕТ ФРЕЙМБУФЕРА — выходим");
        return 1;
    }
    framebufferMakeLinear(&g_fb);

    // Визуальная метка «NRO загрузилось»: заливаем экран тёмным фоном и
    // показываем его до app.init(). Если инициализация упадёт, на экране
    // останется этот цвет, а не чёрный экран — сразу видно, что виноват init.
    {
        u32 stride = 0;
        u32* px = (u32*)framebufferBegin(&g_fb, &stride);
        if (px) {
            const u32 bg = 0xFF14121E;   // RGBA(0x1E,0x12,0x14)
            for (int y = 0; y < 720; y++) {
                u32* row = px + (size_t)y * (stride / 4);
                for (int x = 0; x < 1280; x++) row[x] = bg;
            }
            framebufferEnd(&g_fb);
            bootLog("boot screen: painted");
        } else {
            bootLog("boot screen: framebufferBegin failed");
        }
    }

    tnx::Canvas canvas;
    static tnx::app::App app;
    // Кадр рисуем в обычную (кэшируемую) память, а в framebuffer копируем
    // один раз за кадр. Причина: framebuffer — некэшируемая память, и каждый
    // блендинг с чтением пикселя стоил ~100 тактов; из-за этого кадр рисовался
    // ~110 мс (8 fps). Своя память + один проход memcpy дают кратное ускорение.
    static std::vector<uint32_t> frameBuf((size_t)1280 * 720, 0xFF000000u);
    g_app = &app;
    bootLog("app.init() ...");
    app.init();
    bootLog("app.init() done");

    tnx::ui::Ui u;
    u64 lastTick = armGetSystemTick();
    float time = 0.f;
    bool firstFrame = true;
    int frameNo = 0;

    // Диагностика производительности: раз в 120 кадров пишем средние и
    // максимальные времена этапов. Сразу видно, что тормозит — логика
    // (update), отрисовка (draw) или обмен с фреймбуфером (flush).
    double perfUpd = 0, perfUpdMax = 0, perfDraw = 0, perfDrawMax = 0;
    double perfFlush = 0, perfFlushMax = 0;
    int perfFrames = 0, perfSlowFrames = 0;
    u64 perfWallStart = armGetSystemTick();

    auto msSince = [](u64 a, u64 b) { return (double)armTicksToNs(b - a) / 1e6; };

    while (appletMainLoop() && !g_quit) {
        frameNo++;
        // --- время ---
        u64 now = armGetSystemTick();
        float dt = (float)armTicksToNs(now - lastTick) / 1e9f;
        lastTick = now;
        dt = std::clamp(dt, 1.f / 240.f, 0.1f);
        time += dt;

        // --- ввод ---
        padUpdate(&pad);
        u64 btns = padGetButtons(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down) tnx::platform::bootLog("input: btns=%llx", (unsigned long long)down);
        (void)padGetButtonsUp(&pad);

        HidAnalogStickState ls = padGetStickPos(&pad, 0);
        HidAnalogStickState rs = padGetStickPos(&pad, 1);
        float lx = deadzone(ls.x / 32767.f), ly = deadzone(ls.y / 32767.f);
        float rx = deadzone(rs.x / 32767.f), ry = deadzone(rs.y / 32767.f);

        // Повтор для стика (навигация по фокусу).
        static Repeat repX, repY;
        auto axisRepeat = [&](float v, Repeat& r, bool& neg, bool& pos) {
            neg = pos = false;
            if (v < -0.5f) {
                if (r.mask != 1) { r.mask = 1; r.timer = 0.f; r.delay = 0.42f; neg = true; }
                else {
                    r.timer += dt;
                    if (r.timer >= r.delay) { r.timer = 0.f; r.delay = 0.13f; neg = true; }
                }
            } else if (v > 0.5f) {
                if (r.mask != 2) { r.mask = 2; r.timer = 0.f; r.delay = 0.42f; pos = true; }
                else {
                    r.timer += dt;
                    if (r.timer >= r.delay) { r.timer = 0.f; r.delay = 0.13f; pos = true; }
                }
            } else {
                r.mask = 0;
                r.timer = 0.f;
            }
        };
        bool lxNeg, lxPos, lyNeg, lyPos;
        axisRepeat(lx, repX, lxNeg, lxPos);
        axisRepeat(ly, repY, lyNeg, lyPos);

        bool dpadUp = (down & HidNpadButton_Up) || (down & HidNpadButton_StickLUp) || lyPos;
        bool dpadDown = (down & HidNpadButton_Down) || (down & HidNpadButton_StickLDown) || lyNeg;
        bool dpadLeft = (down & HidNpadButton_Left) || (down & HidNpadButton_StickLLeft) || lxNeg;
        bool dpadRight = (down & HidNpadButton_Right) || (down & HidNpadButton_StickLRight) || lxPos;

        // Тач + режим курсора (ZR + левый стик).
        HidTouchScreenState touch{};
        hidGetTouchScreenStates(&touch, 1);
        bool touching = touch.count > 0;

        static tnx::Vec2 cursor{640.f, 360.f};
        static bool cursorVisible = false;
        bool mouseMode = (btns & HidNpadButton_ZR) != 0;

        bool pressed = false, released = false;
        bool held = false;

        // ВАЖНО: prevTouch объявлен один раз на всю итерацию. Раньше в ветках
        // if/else было по своему static — отпускание пальца не замечалось
        // вовсе, поэтому тапы не активировали кнопки (нажатие было, клика нет).
        static bool prevTouch = false;
        // Свайп: запоминаем точку касания и суммарный путь. Пока путь меньше
        // порога — это тап (кнопки работают), дальше — перетаскивание списка.
        static tnx::Vec2 touchStart{};
        static tnx::Vec2 touchLast{};
        static float touchTravel = 0.f;
        float dragY = 0.f;
        bool dragging = false;
        bool dragged = false;
        if (touching) {
            cursor = {(float)touch.touches[0].x, (float)touch.touches[0].y};
            cursorVisible = true;
            held = true;
            pressed = !prevTouch;
            if (pressed) {
                touchStart = cursor;
                touchLast = cursor;
                touchTravel = 0.f;
                tnx::platform::bootLog("input: touch %.0f,%.0f", cursor.x, cursor.y);
            } else {
                dragY = cursor.y - touchLast.y;
                touchLast = cursor;
                float dx = cursor.x - touchStart.x, dy = cursor.y - touchStart.y;
                touchTravel = std::max(touchTravel, std::sqrt(dx * dx + dy * dy));
            }
            dragging = touchTravel > 12.f;
        } else if (prevTouch) {
            released = true;
            dragged = touchTravel > 12.f;
            tnx::platform::bootLog("input: touch release%s", dragged ? " (свайп)" : "");
        }
        prevTouch = touching;
        if (mouseMode) {
            cursorVisible = true;
            cursor.x += lx * 1500.f * dt;
            cursor.y -= ly * 1500.f * dt;
            cursor.x = std::clamp(cursor.x, 0.f, 1279.f);
            cursor.y = std::clamp(cursor.y, 0.f, 719.f);
            bool zr = (btns & HidNpadButton_ZR) != 0;
            static bool prevZR = false;
            if (zr && !prevZR) {
                pressed = true;
                held = true;
                tnx::platform::bootLog("input: ZR click %.0f,%.0f", cursor.x, cursor.y);
            }
            if (!zr && prevZR) released = true;
            prevZR = zr;
            if (prevZR) held = true;
        }
        if (!touching && (std::fabs(rx) > 0.1f || std::fabs(ry) > 0.1f)) cursorVisible = false;

        tnx::ui::Input in;
        in.pointer = cursor;
        in.pointerVisible = cursorVisible;
        in.down = held;
        in.pressed = pressed;
        in.released = released;
        in.accept = (down & HidNpadButton_A) != 0;
        in.back = (down & HidNpadButton_B) != 0;
        in.menu = (down & HidNpadButton_Plus) != 0;
        in.x = (down & HidNpadButton_X) != 0;
        in.y = (down & HidNpadButton_Y) != 0;
        in.bumpL = (down & HidNpadButton_L) != 0;
        in.bumpR = (down & HidNpadButton_R) != 0;
        in.up = dpadUp;
        in.downEdge = dpadDown;
        in.left = dpadLeft;
        in.right = dpadRight;
        in.scrollY = -ry * 1400.f * dt;
        in.dragY = dragY;
        in.dragging = dragging;
        in.dragged = dragged;
        in.dt = dt;
        in.time = time;

        // --- обновление логики ---
        u64 tUpd0 = armGetSystemTick();
        app.update(dt);
        u64 tUpd1 = armGetSystemTick();

        // --- рендер ---
        u32 stride = 0;
        u32* fb = (u32*)framebufferBegin(&g_fb, &stride);
        double drawMs = 0.0, flushMs = 0.0;
        if (fb) {
            canvas.attach(frameBuf.data(), 1280, 720, 1280);
            app.fonts.setCanvas(&canvas);
            u.beginFrame(canvas, app.theme, app.fonts, in);
            app.draw(u);
            u.endFrame();
            u64 tDraw1 = armGetSystemTick();
            // Быстрая линейная копия в framebuffer (по строке, stride может
            // отличаться от ширины).
            const int fbStride = (int)stride / 4;
            for (int y = 0; y < 720; y++)
                memcpy(fb + (size_t)y * fbStride, frameBuf.data() + (size_t)y * 1280, 1280 * 4);
            framebufferEnd(&g_fb);
            u64 tFlush1 = armGetSystemTick();
            drawMs = msSince(tUpd1, tDraw1);
            flushMs = msSince(tDraw1, tFlush1);
            if (firstFrame) {
                firstFrame = false;
                tnx::platform::bootLog("first frame rendered: ok");
            } else if (frameNo == 120 || frameNo == 600 || frameNo % 3600 == 0) {
                tnx::platform::bootLog("alive: frames=%d screen=%d", frameNo, (int)app.screen);
            }
        }

        // --- статистика кадра ---
        {
            double updMs = msSince(tUpd0, tUpd1);
            perfUpd += updMs; perfUpdMax = std::max(perfUpdMax, updMs);
            perfDraw += drawMs; perfDrawMax = std::max(perfDrawMax, drawMs);
            perfFlush += flushMs; perfFlushMax = std::max(perfFlushMax, flushMs);
            perfFrames++;
            if (drawMs + updMs + flushMs > 45.0) perfSlowFrames++;
            if (perfFrames >= 120) {
                double wall = (double)armTicksToNs(armGetSystemTick() - perfWallStart) / 1e9;
                double n = (double)perfFrames;
                tnx::platform::bootLog(
                    "perf: screen=%d upd=%.1f/%.1f draw=%.1f/%.1f flush=%.1f/%.1f ms "
                    "fps=%.1f slow=%d",
                    (int)app.screen, perfUpd / n, perfUpdMax, perfDraw / n, perfDrawMax,
                    perfFlush / n, perfFlushMax, wall > 0.001 ? n / wall : 0.0, perfSlowFrames);
                perfUpd = perfDraw = perfFlush = 0.0;
                perfUpdMax = perfDrawMax = perfFlushMax = 0.0;
                perfFrames = 0;
                perfSlowFrames = 0;
                perfWallStart = armGetSystemTick();
            }
        }

        // --- 60 fps ---
        float target = 1.f / 60.f;
        float elapsed = (float)armTicksToNs(armGetSystemTick() - now) / 1e9f;
        if (elapsed < target) svcSleepThread((s64)((target - elapsed) * 1e9f));
    }

    bootLog("main loop exit: frames=%d screen=%d", frameNo, (int)app.screen);
    app.shutdown();
    tnx::player::instance().shutdown();
    tnx::dpi::stopRelay();
    tnx::dpi::stopSocks();
    tnx::net::shutdown();

    framebufferClose(&g_fb);
    fsdevUnmountAll();
    fsExit();
    nifmExit();
    socketExit();
    romfsExit();
    return 0;
}