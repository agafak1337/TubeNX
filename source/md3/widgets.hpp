// md3/widgets.hpp — компоненты Material 3 (Expressive) на нашем канвасе.
#pragma once

#include "core/canvas.hpp"
#include "md3/theme.hpp"
#include "md3/ui.hpp"

#include <string>
#include <vector>

namespace tnx::md3 {

// ---------- иконки (Material Symbols, romfs-сабсет) ----------
void icon(ui::Ui& u, const char* name, float x, float y, float size, const Color& c);
void iconCentered(ui::Ui& u, const char* name, const Rect& box, float size, const Color& c);
// 40dp state-layer для иконок-кнопок + ripple
bool iconButton(ui::Ui& u, uint64_t id, const Rect& r, const char* iconName, const Color& tint,
                float iconSize = 24.f, bool toggleOn = false, const Color* toggleBg = nullptr);

// ---------- кнопки ----------
enum class ButtonKind { Filled, Tonal, Elevated, Outlined, Text };
struct ButtonOpts {
    ButtonKind kind = ButtonKind::Filled;
    bool enabled = true;
    const char* iconName = nullptr;
    float height = 40.f;
    bool tonalIcon = false;
};
bool button(ui::Ui& u, uint64_t id, const Rect& r, const std::string& label, const ButtonOpts& o = {});

enum class FabSize { Small, Regular, Large };
bool fab(ui::Ui& u, uint64_t id, Vec2 center, const char* iconName, FabSize size = FabSize::Regular,
         const std::string& label = "");

// ---------- поверхности ----------
enum class CardKind { Filled, Elevated, Outlined };
void surfaceCard(ui::Ui& u, const Rect& r, CardKind kind, float radius = -1.f, bool interactive = false);
void divider(ui::Ui& u, const Rect& r);
void scrim(ui::Ui& u, float opacity);

// ---------- списки / чипы ----------
bool listItem(ui::Ui& u, uint64_t id, const Rect& r, const char* leadingIcon, const std::string& headline,
              const std::string& supporting, bool selected = false, const char* trailingIcon = nullptr);
bool chip(ui::Ui& u, uint64_t id, Rect& rInOut, const std::string& label, bool selected,
          const char* iconName = nullptr, bool assist = false);
// Горизонтальный ряд чипов с прокруткой; возвращает выбранный индекс (-1 если нет нажатия).
int chipRow(ui::Ui& u, uint64_t id, const Rect& r, const std::vector<std::string>& labels,
            const std::vector<bool>& selected, int* scrollIndex = nullptr);

// ---------- навигация ----------
void topBar(ui::Ui& u, uint64_t id, const Rect& r, const std::string& title, bool big,
            bool* backPressed, const char* actionIcon = nullptr, bool* actionPressed = nullptr,
            const char* secondActionIcon = nullptr, bool* secondActionPressed = nullptr);
bool navBar(ui::Ui& u, uint64_t id, const Rect& r, const std::vector<std::string>& labels,
            const std::vector<std::string>& icons, int& selected);
bool segmented(ui::Ui& u, uint64_t id, const Rect& r, const std::vector<std::string>& labels, int& selected);

// ---------- индикаторы ----------
void linearProgress(ui::Ui& u, const Rect& r, float progress); // <0 — бесконечный
void circularProgress(ui::Ui& u, Vec2 center, float radius, float progress, float stroke = 4.f);

// ---------- контролы ----------
bool switchWidget(ui::Ui& u, uint64_t id, const Rect& r, bool& value);
bool slider(ui::Ui& u, uint64_t id, const Rect& r, float& value, float min = 0.f, float max = 1.f);
void snackbar(ui::Ui& u, const std::string& text, float appear, const char* action = nullptr,
              bool* actionPressed = nullptr);
// Возвращает 0 — ничего, 1 — подтвердить, 2 — отменить.
int dialog(ui::Ui& u, uint64_t id, const Rect& r, const std::string& title, const std::string& body,
           const char* confirm, const char* dismiss);

// ---------- карточка видео ----------
struct VideoTileData {
    const Image* thumb = nullptr;   // обложка (может быть nullptr → скелетон)
    std::string title;
    std::string meta;               // "Канал • 1,2 млн просмотров • 3 дня назад"
    std::string duration;           // "12:34"
    float watchProgress = -1.f;     // 0..1 — красная полоска
    bool live = false;
    bool avatars = false;           // показывать круглый аватар канала
    const Image* avatar = nullptr;
    std::string channelInitial;     // если аватара нет
};
// Полноширинная карточка (как в ленте YouTube).
bool videoTile(ui::Ui& u, uint64_t id, const Rect& r, const VideoTileData& d);
// Компактная широкая карточка (для очереди/похожих).
bool videoTileCompact(ui::Ui& u, uint64_t id, const Rect& r, const VideoTileData& d);

} // namespace tnx::md3