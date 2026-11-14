// platform.hpp — платформенные функции libnx, доступные UI-коду.
#pragma once

#include <string>

namespace tnx::platform {

// --- Логирование (docs/impl-tz.md §11) -------------------------------------
// Уровни: ERROR/WARN/INFO/DEBUG. В релизе — INFO, DEBUG пишется только в
// диагностической сборке. Формат строки: "<uptime_ms> <LEVEL> <сообщение>".
enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void setLogLevel(LogLevel level);
LogLevel logLevel();

// Единая точка записи. Одинаковые подряд сообщения в пределах 500 мс
// схлопываются, при смене — строка "(повторено N раз)".
void logWrite(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

void logError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logWarn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logInfo(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logDebug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Дешёвая проверка «нужно ли собирать аргументы» для DEBUG-строк.
bool debugEnabled();

// Совместимость: bootLog — это logInfo. Пишет в stdout и в файл на SD
// (sdmc:/switch/TubeNX/logs/tubenx-<дата>.log + sdmc:/switch/TubeNX-boot.log),
// с немедленным flush; предыдущий лог сохраняется как *.prev.log.
void bootLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// LOG-05: аварийный лог. Ставится один раз после инициализации логгера:
// при SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE пишет sdmc:/switch/TubeNX/logs/crash.log
// (версия, сигнал и хвост ring-буфера 64 КБ) и передаёт сигнал дальше.
void installCrashHandler();

// Экранная клавиатура Switch. Возвращает false, если пользователь отменил.
bool keyboard(std::string& io, const char* guide = nullptr, const char* header = nullptr,
              bool password = false);

// Завершение приложения (возврат в hbmenu).
void requestExit();

// PL-12: медиа-состояние для hbmenu. true — только пока реально идёт видео,
// false — пауза/выход из плеера/сон.
void setMediaPlaybackState(bool playing);

// Информация о железе/сети для экрана «О приложении».
struct Info {
    std::string firmware;
    std::string model;
    bool docked = false;
    bool wifi = false;
    int battery = -1;
    bool charging = false;
};
Info info();

} // namespace tnx::platform
