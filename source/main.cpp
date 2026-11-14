// main.cpp — точка входа NRO: libnx, framebuffer, ввод, главный цикл.
#include <switch.h>

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "core/canvas.hpp"
#include "core/font.hpp"
#include "core/layout.hpp"
#include "dpi/dpi.hpp"
#include "md3/theme.hpp"
#include "md3/ui.hpp"
#include "net/http.hpp"
#include "net/images.hpp"
#include "platform.hpp"
#include "player/player.hpp"
#include "ui/app.hpp"

// Идентификация сборки (LOG-03, RB-02). Задаётся Makefile; здесь — fallback.
#ifndef TNX_VERSION
#define TNX_VERSION "dev"
#endif
#ifndef TNX_BUILD_SHA
#define TNX_BUILD_SHA "unknown"
#endif
// RB-02: дата сборки не должна попадать в бинарь сама по себе (__DATE__/__TIME__
// ломают воспроизводимость). Передаётся только явным -DTNX_BUILD_DATE=...
#ifndef TNX_BUILD_DATE
#define TNX_BUILD_DATE "unknown"
#endif

namespace {

Framebuffer g_fb;
NWindow* g_win = nullptr;
tnx::app::App* g_app = nullptr;
volatile bool g_quit = false;
volatile bool g_modeDirty = false;   // получен AppletHookType_OnOperationMode
volatile bool g_focusDirty = false;  // получен OnFocusState/OnResume (PL-03)
AppletHookCookie g_hookCookie;

// --- режим вывода (docs/impl-tz.md §5.1, §5.9) ---
// Логический канвас всегда 1280×720; dock рисует его с масштабом 1,5 в буфер
// 1920×1080. Буфер кадра — в обычной кэшируемой памяти, физического размера.
tnx::DisplayLayout g_layout;
bool g_fbReady = false;
bool g_fbLinear = false;   // RD-01: framebuffer в линейном режиме (shadow-буфер один)
int g_fbW = 1280, g_fbH = 720;
std::vector<uint32_t> g_frameBuf;
// RD-01: содержимое framebuffer после прошлого кадра. Нужно, чтобы копировать
// в него только изменившиеся строки. Сбрасывается при пересоздании
// framebuffer'а (смена режима dock/handheld, загрузочная заливка).
bool g_flushValid = false;

// --- повтор клавиш (D-pad) ---
struct Repeat {
    u64 mask = 0;
    float timer = 0.f;
    float delay = 0.f;
};


void onAppletHook(AppletHookType type, void*) {
    if (type == AppletHookType_OnExitRequest) g_quit = true;
    // Переключение dock/handheld: в колбэке только флаг. Пересоздание
    // framebuffer'а делает главный цикл — там кадр ещё не начат.
    if (type == AppletHookType_OnOperationMode) g_modeDirty = true;
    // PL-03: фон/сон (FocusState_Background) и пробуждение. В колбэке нельзя
    // трогать mpv — только флаг, решение принимает главный цикл.
    if (type == AppletHookType_OnFocusState || type == AppletHookType_OnResume) g_focusDirty = true;
}

// --- framebuffer ---
bool fbCreate(int w, int h) {
    nwindowSetDimensions(g_win, w, h);
    Result rc = framebufferCreate(&g_fb, g_win, w, h, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) rc = framebufferCreate(&g_fb, g_win, w, h, PIXEL_FORMAT_RGBA_8888, 1);
    if (R_FAILED(rc)) return false;
    g_fbLinear = R_SUCCEEDED(framebufferMakeLinear(&g_fb));
    g_fbW = w;
    g_fbH = h;
    g_fbReady = true;
    return true;
}

void fbDestroy() {
    if (g_fbReady) {
        framebufferClose(&g_fb);
        g_fbReady = false;
    }
}

// Применяет раскладку: framebuffer, масштаб и safe insets канваса, буфер кадра.
// Буфер кадра перевыделяется ТОЛЬКО при смене размера, не на кадр.
bool applyLayout(const tnx::DisplayLayout& l, tnx::Canvas* canvas) {
    fbDestroy();
    if (!fbCreate(l.fbWidth, l.fbHeight)) return false;
    g_layout = l;
    if (canvas) {
        canvas->setScale(l.scale);
        canvas->setSafeInsets(l.safe);
    }
    const size_t need = (size_t)g_fbW * (size_t)g_fbH;
    if (g_frameBuf.size() != need) g_frameBuf.assign(need, 0xFF000000u);
    g_flushValid = false;   // RD-01: framebuffer пересоздан — копируем кадр целиком
    return true;
}

// Выбор режима по appletGetOperationMode() + fallback и одна строка в лог.
bool configureDisplay(bool dockRequested, tnx::Canvas* canvas) {
    using tnx::platform::bootLog;
    if (dockRequested) {
        if (applyLayout(tnx::DisplayLayout::dockLayout(), canvas)) {
            bootLog("display: dock 1920x1080 scale=1.5");
            return true;
        }
        bootLog("display: dock 1920x1080 недоступен");
        if (applyLayout(tnx::DisplayLayout::dockFallback(), canvas)) {
            bootLog("display: dock fallback 1280x720");
            return true;
        }
        bootLog("display: dock fallback: нет фреймбуфера");
        return false;
    }
    if (applyLayout(tnx::DisplayLayout::handheld(), canvas)) {
        bootLog("display: handheld 1280x720 scale=1.0");
        return true;
    }
    bootLog("display: handheld: нет фреймбуфера");
    return false;
}

// Заливка экрана до app.init(): визуальная метка «NRO загрузилось».
void paintBootScreen() {
    u32 stride = 0;
    u32* px = (u32*)framebufferBegin(&g_fb, &stride);
    if (!px) {
        tnx::platform::bootLog("boot screen: framebufferBegin failed");
        return;
    }
    const u32 bg = 0xFF14121E;   // RGBA(0x1E,0x12,0x14)
    for (int y = 0; y < g_fbH; y++) {
        u32* row = px + (size_t)y * (stride / 4);
        for (int x = 0; x < g_fbW; x++) row[x] = bg;
    }
    framebufferEnd(&g_fb);
    g_flushValid = false;   // RD-01: в framebuffer теперь заливка, а не кадр
    tnx::platform::bootLog("boot screen: painted %dx%d", g_fbW, g_fbH);
}

inline float deadzone(float v, float dz = 0.22f) {
    if (std::fabs(v) < dz) return 0.f;
    float s = (v - (v > 0 ? dz : -dz)) / (1.f - dz);
    return std::clamp(s, -1.f, 1.f);
}

} // namespace

namespace tnx::platform {

namespace {

// --- Состояние логгера (LOG-01..03, docs/impl-tz.md §11) -------------------
// Логгер один на процесс: строки пишут и фоновые потоки (сеть, ретранслятор).
struct LogState {
    LogLevel level = LogLevel::Info;
    std::mutex mtx;
    FILE* file = nullptr;       // sdmc:/switch/TubeNX/logs/tubenx-<ts>.log
    FILE* boot = nullptr;       // sdmc:/switch/TubeNX-boot.log (совместимость)
    std::string pending;        // строки до готовности SD
    uint64_t baseTick = 0;
    bool baseSet = false;
    std::string prevRaw;        // предыдущее сообщение — схлопывание повторов
    uint64_t prevMs = 0;
    int suppressed = 0;
    long bytes = 0;             // размер основного файла
    int flushCount = 0;
    // LOG-05: кольцо последних строк для аварийного дампа (64 КБ).
    char ring[64 * 1024];
    size_t ringPos = 0;         // куда писать следующую строку
    size_t ringLen = 0;         // сколько занято (не больше размера кольца)
};

LogState& logState() { static LogState s; return s; }

const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Info: return "INFO";
        default: return "DEBUG";
    }
}

uint64_t uptimeMs() {
    LogState& s = logState();
    if (!s.baseSet) { s.baseTick = armGetSystemTick(); s.baseSet = true; }
    return armTicksToNs(armGetSystemTick() - s.baseTick) / 1000000ull;
}

// Оставляет в каталоге не больше keep новейших файлов tubenx-*.log.
void pruneLogs(const char* dir, int keep) {
    DIR* d = opendir(dir);
    if (!d) return;
    std::vector<std::string> names;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n.rfind("tubenx-", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".log")
            names.push_back(n);
    }
    closedir(d);
    if ((int)names.size() <= keep) return;
    std::sort(names.begin(), names.end());  // имя = метка времени, сортируется лексикографически
    for (int i = 0; i + keep < (int)names.size(); i++) {
        std::string p = std::string(dir) + "/" + names[i];
        remove(p.c_str());
    }
}

// Открывает файлы. false — SD ещё недоступна (строки копятся в pending).
bool openLogs() {
    LogState& s = logState();
    if (s.file) return true;
    static const char* kDir = "sdmc:/switch/TubeNX/logs";
    mkdir("sdmc:/switch/TubeNX", 0777);
    mkdir(kDir, 0777);
    pruneLogs(kDir, 2);  // два старых + новый = три (ротация, impl-tz §11.3)

    char path[160];
    time_t now = time(nullptr);
    struct tm tmv{};
    localtime_r(&now, &tmv);
    snprintf(path, sizeof(path), "%s/tubenx-%04d%02d%02d-%02d%02d%02d.log", kDir,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min,
             tmv.tm_sec);
    FILE* f = fopen(path, "w");

    // LOG-02: предыдущий журнал сохраняется как .prev.log.
    remove("sdmc:/switch/TubeNX-boot.prev.log");
    rename("sdmc:/switch/TubeNX-boot.log", "sdmc:/switch/TubeNX-boot.prev.log");
    FILE* b = fopen("sdmc:/switch/TubeNX-boot.log", "w");

    if (!f) f = b;
    if (!b) b = f;
    if (!f) return false;
    s.file = f;
    s.boot = b;
    s.bytes = 0;
    return true;
}

void closeLogsNoLock() {
    LogState& s = logState();
    if (s.file) { fflush(s.file); fclose(s.file); }
    if (s.boot && s.boot != s.file) { fflush(s.boot); fclose(s.boot); }
    s.file = nullptr;
    s.boot = nullptr;
}

// Ротация по размеру: 2 МБ на файл (impl-tz §11.3).
void rotateIfNeededBase() {
    LogState& s = logState();
    if (!s.file || s.bytes < 2 * 1024 * 1024) return;
    closeLogsNoLock();
    openLogs();
}

// --- LOG-05: аварийный лог (ring 64 КБ + обработчик сигналов) --------------
constexpr const char* kCrashPath = "sdmc:/switch/TubeNX/logs/crash.log";

// Копирует строку в кольцо. Вызывается под s.mtx из logWrite().
void ringAppendLocked(LogState& s, const char* text, size_t n) {
    // Длинную строку обрезаем: в кольце важно начало сообщения.
    if (n > 400) n = 400;
    for (size_t i = 0; i < n; i++) {
        s.ring[s.ringPos] = text[i];
        s.ringPos = (s.ringPos + 1) % sizeof(s.ring);
    }
    if (s.ringLen + n > sizeof(s.ring)) s.ringLen = sizeof(s.ring);
    else s.ringLen += n;
}

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        default: return "signal";
    }
}

// В обработчике сигнала допустимы только async-signal-safe вызовы:
// open/write/close/signal/raise. Никаких std::string и printf.
void crashHandler(int sig) {
    LogState& s = logState();
    const int fd = open(kCrashPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
                         "TubeNX: аварийное завершение\n"
                         "версия=%s сборка=%s сигнал=%d (%s)\n"
                         "--- хвост лога (%u байт) ---\n",
                         TNX_VERSION, TNX_BUILD_SHA, sig, signalName(sig),
                         (unsigned)s.ringLen);
        if (n > 0) {
            ssize_t wr = write(fd, hdr, (size_t)n);
            (void)wr;
        }
        const size_t len = s.ringLen;
        if (len > 0) {
            const size_t size = sizeof(s.ring);
            const size_t start = (s.ringPos + size - len) % size;
            if (start + len <= size) {
                ssize_t wr = write(fd, s.ring + start, len);
                (void)wr;
            } else {
                const size_t first = size - start;
                ssize_t wr = write(fd, s.ring + start, first);
                (void)wr;
                wr = write(fd, s.ring, len - first);
                (void)wr;
            }
        }
        close(fd);
    }
    // Строка в основной лог: flush не async-signal-safe, но файл уже
    // открыт, и без него последние INFO-строки могут остаться в буфере.
    if (s.file) fflush(s.file);
    signal(sig, SIG_DFL);
    raise(sig);
}

} // namespace

// LOG-05: включаем обработчик. Повторный вызов безопасен.
void installCrashHandler() {
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGBUS, crashHandler);
    signal(SIGILL, crashHandler);
    signal(SIGFPE, crashHandler);
}

void setLogLevel(LogLevel level) { logState().level = level; }
LogLevel logLevel() { return logState().level; }
bool debugEnabled() { return logState().level >= LogLevel::Debug; }

void logWrite(LogLevel level, const char* fmt, ...) {
    LogState& s = logState();
    if (level > s.level) return;

    char raw[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof(raw), fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(s.mtx);
    const uint64_t now = uptimeMs();

    // Запись строки в файл(ы) с буферизацией до готовности SD.
    auto writeOut = [&](const char* text) {
        // LOG-05: каждая строка идёт и в кольцо аварийного дампа.
        ringAppendLocked(s, text, strlen(text));
        if (!s.file) {
            if (s.pending.empty()) openLogs();
            if (!s.file) {
                s.pending += text; s.pending += '\n';
                if (s.pending.size() > (1u << 16)) s.pending.erase(0, s.pending.size() - (1u << 15));
                return;
            }
        }
        if (!s.pending.empty()) {
            fputs(s.pending.c_str(), s.file);
            if (s.boot && s.boot != s.file) fputs(s.pending.c_str(), s.boot);
            s.bytes += (long)s.pending.size();
            s.pending.clear();
        }
        fprintf(s.file, "%s\n", text);
        if (s.boot && s.boot != s.file) fprintf(s.boot, "%s\n", text);
        s.bytes += (long)strlen(text) + 1;
    };

    // Схлопывание повторов: то же сообщение чаще раза в 500 мс не пишем.
    if (s.prevRaw == raw && now - s.prevMs < 500 && level < LogLevel::Warn) {
        s.suppressed++;
        s.prevMs = now;
        return;
    }
    if (s.suppressed > 0) {
        char note[640];
        snprintf(note, sizeof(note),
                 "%llu INFO log: предыдущее сообщение повторено %d раз(а)",
                 (unsigned long long)now, s.suppressed);
        s.suppressed = 0;
        printf("[TubeNX] %s\n", note);
        writeOut(note);
    }
    s.prevRaw = raw;
    s.prevMs = now;

    char line[640];
    snprintf(line, sizeof(line), "%llu %s %s", (unsigned long long)now, levelName(level), raw);
    printf("[TubeNX] %s\n", line);
    writeOut(line);

    // fflush пачкой: каждые 8 строк и всегда для WARN/ERROR.
    if (++s.flushCount >= 8 || level <= LogLevel::Warn) {
        fflush(s.file);
        if (s.boot && s.boot != s.file) fflush(s.boot);
        s.flushCount = 0;
    }
    rotateIfNeededBase();
}

void logError(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    logWrite(LogLevel::Error, "%s", b);
}
void logWarn(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    logWrite(LogLevel::Warn, "%s", b);
}
void logInfo(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    logWrite(LogLevel::Info, "%s", b);
}
void logDebug(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    logWrite(LogLevel::Debug, "%s", b);
}

void bootLog(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    logWrite(LogLevel::Info, "%s", b);
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

// PL-12: hbmenu показывает «медиа играет» только пока открыт плеер.
void setMediaPlaybackState(bool playing) { appletSetMediaPlaybackState(playing); }

} // namespace tnx::platform

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    using tnx::platform::bootLog;
#ifdef TNX_DIAG
    // Диагностическая сборка: DEBUG включён по умолчанию (LOG-01).
    tnx::platform::setLogLevel(tnx::platform::LogLevel::Debug);
#endif
    // LOG-01: уровень можно переопределить переменной окружения TNX_LOG
    // (error|warn|info|debug) — снять подробный лог без пересборки.
    if (const char* lv = getenv("TNX_LOG")) {
        std::string s;
        for (const char* p = lv; *p; p++) s += (char)tolower((unsigned char)*p);
        using tnx::platform::LogLevel;
        if (s == "error") tnx::platform::setLogLevel(LogLevel::Error);
        else if (s == "warn") tnx::platform::setLogLevel(LogLevel::Warn);
        else if (s == "debug") tnx::platform::setLogLevel(LogLevel::Debug);
        else tnx::platform::setLogLevel(LogLevel::Info);
    }
    // LOG-03: первая строка — версия, метка сборки, дата, режим.
    bootLog("start v=%s build=%s date=%s mode=%s", TNX_VERSION, TNX_BUILD_SHA, TNX_BUILD_DATE,
            appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
    bootLog("argv0=%s", (argv && argv[0]) ? argv[0] : "(null)");
    bootLog("mode=%s", appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
    // LOG-05: аварийный лог. Ставим сразу после первой строки лога — строки,
    // накопленные до готовности SD, лежат в pending и в кольце.
    tnx::platform::installCrashHandler();

    // --- базовые сервисы (каждый шаг проверяем и логируем) ---
    Result rc = fsInitialize();
    bootLog("fsInitialize: 0x%x", rc);
    rc = fsdevMountSdmc();
    bootLog("fsdevMountSdmc: 0x%x", rc);

    // Приёмочная проверка LOG-05: намеренный SIGSEGV, в
    // sdmc:/switch/TubeNX/logs/crash.log должен появиться хвост лога.
    //
    // Проверка стоит ПОСЛЕ fsdevMountSdmc: обработчик сигнала создаёт crash.log
    // на sdmc:, и до монтирования файл просто не открылся бы. Из hbmenu
    // env-переменную задать негде, поэтому кроме TNX_CRASH_TEST=1 есть
    // одноразовый файл-флаг: sdmc:/switch/TubeNX/crash-test (или рядом с логами,
    // sdmc:/switch/TubeNX/logs/crash-test). После срабатывания флаг
    // переименовывается в crash-test.used, чтобы следующий запуск был обычным.
    {
        const char* ct = getenv("TNX_CRASH_TEST");
        const bool byEnv = ct && *ct && *ct != '0';
        const char* flags[2] = {"sdmc:/switch/TubeNX/logs/crash-test",
                                "sdmc:/switch/TubeNX/crash-test"};
        const char* flagUsed = nullptr;
        if (!byEnv) {
            for (const char* path : flags) {
                if (FILE* f = fopen(path, "r")) {
                    fclose(f);
                    flagUsed = path;
                    break;
                }
            }
        }
        if (byEnv || flagUsed) {
            if (flagUsed) {
                std::string used = std::string(flagUsed) + ".used";
                bootLog("LOG-05: тестовый SIGSEGV по файлу-флагу %s", flagUsed);
                if (rename(flagUsed, used.c_str()) != 0)
                    bootLog("LOG-05: флаг не переименован в %s (уже есть?)", used.c_str());
            } else {
                bootLog("LOG-05: тестовый SIGSEGV по TNX_CRASH_TEST=1");
            }
            raise(SIGSEGV);
        }
    }
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

    appletSetMediaPlaybackState(false); // PL-12: включается только на время видео
    appletHook(&g_hookCookie, onAppletHook, nullptr);
    // На Switch 2 / новых прошивках даём приложению больше CPU-времени на декод.
    rc = appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    bootLog("appletSetCpuBoostMode: 0x%x", rc);

    // --- экран: handheld 720p или dock 1080p (с fallback на 720p) ---
    g_win = nwindowGetDefault();
    // PL-02: vsync. В libnx синхронизация с развёрткой — это swap interval
    // окна: 1 = следующая очередь кадра ждёт VBlank (значение по умолчанию,
    // но фиксируем явно и пишем результат в лог).
    {
        Result vrc = nwindowSetSwapInterval(g_win, 1);
        bootLog("PL-02: vsync swap_interval=1 rc=0x%x", vrc);
    }
    const bool startDock = appletGetOperationMode() == AppletOperationMode_Console;
    if (!configureDisplay(startDock, nullptr)) {
        bootLog("НЕТ ФРЕЙМБУФЕРА — выходим");
        return 1;
    }

    paintBootScreen();

    tnx::Canvas canvas;
    canvas.setScale(g_layout.scale);
    canvas.setSafeInsets(g_layout.safe);
    static tnx::app::App app;
    // Кадр рисуем в обычную (кэшируемую) память, а в framebuffer копируем
    // один раз за кадр. Причина: framebuffer — некэшируемая память, и каждый
    // блендинг с чтением пикселя стоил ~100 тактов; из-за этого кадр рисовался
    // ~110 мс (8 fps). Своя память + один проход memcpy дают кратное ускорение.
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
    // PL-02: сколько кадров реально нарисовано, сколько пропущено (статичный
    // экран) и сколько строк перенесено в framebuffer (RD-01).
    int perfDrawn = 0, perfSkipped = 0, perfRows = 0;
    // PL-02: гистограмма draw по 1 мс — из неё считаем p95 (приёмка §5.2).
    int perfDrawHist[32] = {0};
    u64 perfWallStart = armGetSystemTick();

    auto msSince = [](u64 a, u64 b) { return (double)armTicksToNs(b - a) / 1e6; };
    u64 lastInputTick = 0;
    u64 lastDrawTick = armGetSystemTick();

    // RD-01: перенос кадра в framebuffer — только изменившимися строками.
    // При framebufferMakeLinear libnx отдаёт один и тот же линейный
    // shadow-буфер (обычная кэшируемая память), поэтому в нём уже лежит
    // предыдущий кадр: сравниваем с ним. Если контрольные строки изменились
    // почти все (видео, прокрутка) — дешевле скопировать кадр целиком, без
    // 720 построчных сравнений. Возвращает число перенесённых строк.
    auto copyFrame = [](u32* fb, int fbStride) -> int {
        const size_t rowBytes = (size_t)g_fbW * sizeof(uint32_t);
        const u32* src = g_frameBuf.data();
        auto fullCopy = [&]() {
            for (int y = 0; y < g_fbH; y++)
                std::memcpy(fb + (size_t)y * fbStride, src + (size_t)y * g_fbW, rowBytes);
            return g_fbH;
        };
        if (!g_flushValid) return fullCopy();
        // Построчное сравнение верно только в линейном режиме: там все кадры
        // идут в один и тот же shadow-буфер, где уже лежит прошлый кадр.
        if (!g_fbLinear) return fullCopy();
        int probeDiff = 0;
        for (int k = 0; k < 8; k++) {
            const int y = (g_fbH * (2 * k + 1)) / 16;
            if (std::memcmp(fb + (size_t)y * fbStride, src + (size_t)y * g_fbW, rowBytes) != 0)
                probeDiff++;
        }
        if (probeDiff >= 5) return fullCopy();
        int rows = 0;
        for (int y = 0; y < g_fbH; y++) {
            u32* dst = fb + (size_t)y * fbStride;
            const u32* s = src + (size_t)y * g_fbW;
            if (std::memcmp(dst, s, rowBytes) == 0) continue;
            std::memcpy(dst, s, rowBytes);
            rows++;
        }
        return rows;
    };

    while (appletMainLoop() && !g_quit) {
        frameNo++;

        // --- PL-03: фон/сон → пауза плеера, возврат → пробуждение ---
        // Проверяем только после хука OnFocusState/OnResume: appletGetFocusState
        // — IPC-вызов, каждый кадр его дёргать незачем.
        if (g_focusDirty) {
            g_focusDirty = false;
            static bool s_background = false;
            const bool background = appletGetFocusState() == AppletFocusState_Background;
            if (background != s_background) {
                s_background = background;
                if (background) {
                    tnx::player::instance().onSleep();
                    bootLog("PL-03: приложение в фоне/сне — плеер на паузе");
                } else {
                    tnx::player::instance().onWake();
                    bootLog("PL-03: пробуждение — плеер восстановлен");
                }
            }
        }

        // --- смена режима dock ↔ handheld (без перезапуска) ---
        // OnOperationModeChanged приходит из appletMainLoop (главный поток),
        // кадр ещё не начат — framebuffer можно пересоздать. App, ui::Ui и их
        // состояние (экран, скролл, фокус) не пересоздаются. Раз в 30 кадров
        // дополнительно сверяем режим сами: hook есть не во всех средах.
        if (g_modeDirty || frameNo % 30 == 0) {
            const bool dock = appletGetOperationMode() == AppletOperationMode_Console;
            if (g_modeDirty || dock != g_layout.dock) {
                g_modeDirty = false;
                if (configureDisplay(dock, &canvas))
                    bootLog("display: режим переключён, экран=%d", (int)app.screen);
                else
                    bootLog("display: переключение режима не удалось");
            }
        }

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
        // PL-02: рисуем только если что-то изменилось. Ввод и его «хвост»
        // (ripple, пружины фокуса/hover), результаты фоновых задач, анимации
        // (спиннеры, тост, видео-кадр) и догрузка обложек выставляют флаг в
        // App::update(). Плюс страховка: раз в секунду принудительный кадр,
        // чтобы ни одно изменение состояния не осталось незамеченным.
        const bool inputNow = down || btns || pressed || released || touching || dragging ||
                              mouseMode || lx != 0.f || ly != 0.f || rx != 0.f || ry != 0.f;
        const u64 tFrame = armGetSystemTick();
        if (inputNow) lastInputTick = tFrame;
        const bool inputTail = lastInputTick != 0 && msSince(lastInputTick, tFrame) < 600.0;
        const bool watchdog = msSince(lastDrawTick, tFrame) >= 1000.0;
        const bool redraw = firstFrame || inputTail || watchdog || app.takeDirty();

        double drawMs = 0.0, flushMs = 0.0;
        bool didDraw = false;
        if (redraw && g_fbReady) {
            u32 stride = 0;
            u32* fb = (u32*)framebufferBegin(&g_fb, &stride);
            if (fb) {
                canvas.attach(g_frameBuf.data(), g_fbW, g_fbH, g_fbW);
                app.fonts.setCanvas(&canvas);
                u.beginFrame(canvas, app.theme, app.fonts, in);
                app.draw(u);
                u.endFrame();
                u64 tDraw1 = armGetSystemTick();
                // RD-01: копируем в framebuffer только изменившиеся строки;
                // при полной смене кадра (видео/скролл) — целиком.
                const int rows = copyFrame(fb, (int)stride / 4);
                g_flushValid = true;
                framebufferEnd(&g_fb);
                u64 tFlush1 = armGetSystemTick();
                drawMs = msSince(tUpd1, tDraw1);
                flushMs = msSince(tDraw1, tFlush1);
                perfRows += rows;
                didDraw = true;
                lastDrawTick = armGetSystemTick();
                if (firstFrame) {
                    firstFrame = false;
                    tnx::platform::bootLog("first frame rendered: ok");
                } else if (frameNo == 120 || frameNo == 600 || frameNo % 3600 == 0) {
                    tnx::platform::bootLog("alive: frames=%d screen=%d", frameNo, (int)app.screen);
                }
            }
        }

        // --- статистика кадра ---
        {
            double updMs = msSince(tUpd0, tUpd1);
            perfUpd += updMs; perfUpdMax = std::max(perfUpdMax, updMs);
            if (didDraw) {
                perfDrawn++;
                perfDraw += drawMs; perfDrawMax = std::max(perfDrawMax, drawMs);
                perfFlush += flushMs; perfFlushMax = std::max(perfFlushMax, flushMs);
                if (drawMs + updMs + flushMs > 45.0) perfSlowFrames++;
                perfDrawHist[std::clamp((int)drawMs, 0, 31)]++;
            } else {
                perfSkipped++;
            }
            perfFrames++;
            if (perfFrames >= 120) {
                double wall = (double)armTicksToNs(armGetSystemTick() - perfWallStart) / 1e9;
                double n = (double)perfFrames;
                double nd = perfDrawn > 0 ? (double)perfDrawn : 1.0;
                // p95 draw: идём по гистограмме до 95 % нарисованных кадров.
                int p95 = 0;
                if (perfDrawn > 0) {
                    const int need = (int)std::ceil(perfDrawn * 0.95);
                    int acc = 0;
                    for (int i = 0; i < 32; i++) {
                        acc += perfDrawHist[i];
                        if (acc >= need) { p95 = i; break; }
                    }
                }
                tnx::platform::bootLog(
                    "perf: screen=%d upd=%.1f/%.1f draw=%.1f/%.1f(p95=%d) "
                    "flush=%.1f/%.1f ms fps=%.1f drawn=%d skip=%d rows=%d/%d slow=%d",
                    (int)app.screen, perfUpd / n, perfUpdMax, perfDraw / nd, perfDrawMax, p95,
                    perfFlush / nd, perfFlushMax, wall > 0.001 ? n / wall : 0.0, perfDrawn,
                    perfSkipped, perfDrawn > 0 ? perfRows / perfDrawn : 0, g_fbH, perfSlowFrames);
                perfUpd = perfDraw = perfFlush = 0.0;
                perfUpdMax = perfDrawMax = perfFlushMax = 0.0;
                perfFrames = 0;
                perfDrawn = perfSkipped = perfRows = 0;
                perfSlowFrames = 0;
                for (int i = 0; i < 32; i++) perfDrawHist[i] = 0;
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