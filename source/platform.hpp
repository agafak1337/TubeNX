// platform.hpp — платформенные функции libnx, доступные UI-коду.
#pragma once

#include <string>

namespace tnx::platform {

// Экранная клавиатура Switch. Возвращает false, если пользователь отменил.
bool keyboard(std::string& io, const char* guide = nullptr, const char* header = nullptr,
              bool password = false);

// Завершение приложения (возврат в hbmenu).
void requestExit();

// Диагностика загрузки: пишет строку в stdout и в sdmc:/switch/TubeNX-boot.log
// (с немедленным flush — если приложение упадёт, лог останется на карте).
void bootLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

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