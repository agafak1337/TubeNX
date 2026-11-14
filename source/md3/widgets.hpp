// md3/widgets.hpp — компоненты Material 3 (Expressive) на нашем канвасе.
#pragma once

#include "core/canvas.hpp"
#include "md3/theme.hpp"
#include "md3/ui.hpp"

#include <string>
#include <vector>

namespace tnx::md3 {

// Хит-зона: если визуальный rect меньше minW×minH, возвращает расширенный
// прямоугольник (Р-06). Отрисовка при этом не меняется.
Rect hitRect(const Rect& r, float minW = 48.f, float minH = 48.f);

// ---------- иконки (Material Symbols, romfs-сабсет) ----------
void icon(ui::Ui& u, const char* name, float x, float y, float size, const Color& c);
void iconCentered(ui::Ui& u, const char* name, const Rect& box, float size, const Color& c);
// 40dp state-layer для иконок-кнопок + ripple; зона нажатия ≥ 48×48.
// onVideo — вариант поверх видео: кольцо фокуса primary + тёмная обводка 7 px.
bool iconButton(ui::Ui& u, uint64_t id, const Rect& r, const char* iconName, const Color& tint,
                float iconSize = 24.f, bool toggleOn = false, const Color* toggleBg = nullptr,
                bool onVideo = false);

// ---------- кнопки ----------
enum class ButtonKind { Filled, Tonal, Elevated, Outlined, Text };
struct ButtonOpts {
    ButtonKind kind = ButtonKind::Filled;
    bool enabled = true;
    const char* iconName = nullptr;
    float height = 40.f;
    bool tonalIcon = false;
    bool onVideo = false;   // тёмная обводка фокуса 7 px (Р-05)
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
// ringInside — кольцо фокуса внутрь (offset −3) для строк меню/списков (Р-11).
bool listItem(ui::Ui& u, uint64_t id, const Rect& r, const char* leadingIcon, const std::string& headline,
              const std::string& supporting, bool selected = false, const char* trailingIcon = nullptr,
              bool ringInside = true);

struct ChipOpts {
    bool onVideo = false;   // поверх видео: белый текст, высота 36
    bool off = false;       // видео-чип выключен: прозрачный + обводка белый 50 %
    bool enabled = true;
    float height = 0.f;     // 0 — 32 (обычный) / 36 (on-video)
};
bool chip(ui::Ui& u, uint64_t id, Rect& rInOut, const std::string& label, bool selected,
          const char* iconName = nullptr, bool assist = false, const ChipOpts& o = {});
// Горизонтальный ряд чипов с прокруткой; возвращает выбранный индекс (-1 если нет нажатия).
int chipRow(ui::Ui& u, uint64_t id, const Rect& r, const std::vector<std::string>& labels,
            const std::vector<bool>& selected, int* scrollIndex = nullptr, float chipHeight = 32.f);

// ---------- навигация ----------
void topBar(ui::Ui& u, uint64_t id, const Rect& r, const std::string& title, bool big,
            bool* backPressed, const char* actionIcon = nullptr, bool* actionPressed = nullptr,
            const char* secondActionIcon = nullptr, bool* secondActionPressed = nullptr);
bool navBar(ui::Ui& u, uint64_t id, const Rect& r, const std::vector<std::string>& labels,
            const std::vector<std::string>& icons, int& selected);
// Сегменты: высота 40, на всю ширину, кольцо фокуса внутрь.
bool segmented(ui::Ui& u, uint64_t id, const Rect& r, const std::vector<std::string>& labels, int& selected,
               float height = 40.f);

// ---------- индикаторы ----------
void linearProgress(ui::Ui& u, const Rect& r, float progress); // <0 — бесконечный
void circularProgress(ui::Ui& u, Vec2 center, float radius, float progress, float stroke = 4.f);

// ---------- контролы ----------
bool switchWidget(ui::Ui& u, uint64_t id, const Rect& r, bool& value);

struct SliderOpts {
    bool onVideo = false;   // поверх видео: дорожка белый 18 %, буфер 30 %, белое кольцо
    bool volume = false;    // громкость: белая заливка и ползунок (140×48)
    float buffer = -1.f;    // 0..1 — буфер (только на видео)
    float track = 4.f, focusTrack = 6.f;
    float thumb = 14.f, focusThumb = 20.f;
};
bool slider(ui::Ui& u, uint64_t id, const Rect& r, float& value, float min = 0.f, float max = 1.f,
            const SliderOpts& o = {});

// Поле поиска 52 px, r26, иконка search 20, placeholder bodyLarge.
struct SearchBarOpts {
    bool focused = false;
    bool enabled = true;
    const char* leadingIcon = "search";
    const char* trailingIcon = nullptr;   // close
    float height = 52.f;
};
struct SearchBarResult {
    bool activated = false;   // нажатие по полю
    bool trailing = false;    // нажатие по кнопке close
};
SearchBarResult searchBar(ui::Ui& u, uint64_t id, const Rect& r, const std::string& text,
                          const std::string& placeholder = "Поиск на YouTube",
                          const SearchBarOpts& o = {});

void snackbar(ui::Ui& u, const std::string& text, float appear, const char* action = nullptr,
              bool* actionPressed = nullptr);
// Возвращает 0 — ничего, 1 — подтвердить, 2 — отменить.
int dialog(ui::Ui& u, uint64_t id, const Rect& r, const std::string& title, const std::string& body,
           const char* confirm, const char* dismiss);

// ---------- карточка видео ----------
struct VideoTileData {
    const Image* thumb = nullptr;   // обложка (может быть nullptr → скелетон)
    // NET-02: обложки нет (все размеры 404 / ошибка сети) — рисовать статичный
    // плейсхолдер, а не «загрузку». Игнорируется, если thumb валиден.
    bool thumbPlaceholder = false;
    std::string title;
    std::string channel;            // канал — bodySmall 12/16, 1 строка
    std::string meta;               // «1,2 млн просмотров • 3 дня назад»; старый
                                    // формат «канал • мета» тоже разбирается
    std::string duration;           // "12:34"
    float watchProgress = -1.f;     // 0..1 — полоска просмотра; <0 — нет
    bool live = false;
    // Устарело: аватар и more_vert из карточки убраны. Поля оставлены,
    // чтобы не ломать вызовы screens.cpp.
    bool avatars = false;
    const Image* avatar = nullptr;
    std::string channelInitial;
};
// Карточка видео: ширина снаружи (296 handheld / 276 dock), высота = thumb + 90.
bool videoTile(ui::Ui& u, uint64_t id, const Rect& r, const VideoTileData& d);
// «Продолжить просмотр»: фиксированная карточка 240×135 + строка 14/20.
bool videoTileCompact(ui::Ui& u, uint64_t id, const Rect& r, const VideoTileData& d);

} // namespace tnx::md3