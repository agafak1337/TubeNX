// md3/ui.hpp — интерактивный контекст: ввод, фокус (геймпад), скролл, ripple.
#pragma once

#include "core/canvas.hpp"
#include "core/font.hpp"
#include "md3/theme.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace tnx::ui {

// Ввод за кадр. Края (pressed/released/up/...) валидны только в своём кадре.
struct Input {
    Vec2 pointer{640.f, 360.f};   // виртуальный курсор (правый стик/тач)
    bool pointerVisible = false;
    bool down = false;            // кнопка мыши/тач/палец стика зажаты
    bool pressed = false;         // фронт нажатия
    bool released = false;        // фронт отпускания
    bool accept = false;          // A
    bool back = false;            // B
    bool menu = false;            // +
    bool up = false, downEdge = false, left = false, right = false;  // D-pad (фронты)
    bool x = false, y = false;    // X / Y
    bool bumpL = false, bumpR = false;
    float scrollY = 0.f;          // колесо/стик, px за кадр
    // Тач-перетаскивание (свайп). dragY — смещение пальца за кадр,
    // dragging — палец уже сдвинулся дальше порога, dragged — в кадре
    // отпускания: это был свайп, а не тап (активацию виджета гасим).
    float dragY = 0.f;
    bool dragging = false;
    bool dragged = false;
    float dt = 1.f / 60.f;
    float time = 0.f;
};

struct NodeState {
    Spring hover;   // 0..1
    Spring press;   // 0..1
    Spring focus;   // 0..1
    Spring value;   // для переключателей/слайдеров
    uint64_t scrollId = 0;  // к какому scrollArea относится (для стика)
    float rippleStart = -100.f;
    Vec2 rippleOrigin{};
    float scrollVelocity = 0.f;
};

class Ui {
public:
    void beginFrame(Canvas& c, const md3::Theme& th, Fonts& f, const Input& in);
    void endFrame();

    Canvas& canvas() { return *canvas_; }
    Fonts& fonts() { return *fonts_; }
    const md3::Theme& theme() const { return *theme_; }
    const Input& input() const { return in_; }
    float time() const { return in_.time; }
    float dt() const { return in_.dt; }
    Vec2 pointer() const { return in_.pointer; }

    static uint64_t hash(const char* s, uint64_t seed = 1469598103934665603ull);
    uint64_t id(const char* s, int extra = 0) const;
    uint64_t idPtr(const void* p) const;

    // --- взаимодействие ---
    // hover/press-анимации + регистрация в навигации. Возвращает true в кадре активации.
    bool interact(uint64_t id, const Rect& r);
    bool hovered(uint64_t id);
    float hoverAmount(uint64_t id);
    float pressAmount(uint64_t id);
    float focusAmount(uint64_t id);
    bool isFocused(uint64_t id) const { return focusedId_ == id; }
    bool hasFocus() const { return focusedId_ != 0; }
    bool isActive(uint64_t id) const { return activeId_ == id; }
    void focus(uint64_t id) { focusedId_ = id; }
    void clearFocus() { focusedId_ = 0; }

    // Регистрация элемента для D-pad-навигации (вызывать для каждого интерактивного элемента).
    void focusable(uint64_t id, const Rect& r);

    // Ripple поверх формы виджета.
    void ripple(uint64_t id, const Rect& shape, const CornerRadii& cr, const Color& c);
    // Полный стандартный ripple-слой M3 (state layer + волна) — используется виджетами.
    void stateLayer(uint64_t id, const Rect& shape, const CornerRadii& cr, const Color& tint, float baseOpacity);

    // --- скролл ---
    // Возвращает смещение контента (>=0). Управляет инерцией/D-pad-скроллом.
    float scrollArea(uint64_t id, const Rect& viewport, float contentHeight, bool* nearEnd = nullptr);
    void scrollTo(uint64_t id, float offset);
    bool scrolling(uint64_t id) const;

    // Текущий скролл-контекст (для ensureVisible/автоскролла к фокусу).
    void pushScroll(uint64_t id) { scrollStack_.push_back(id); }
    void popScroll() { if (!scrollStack_.empty()) scrollStack_.pop_back(); }

private:
    struct ScrollState {
        Spring offset;
        Rect viewport;
        float maxOffset = 0.f;
        bool dragging = false;
        float dragStartY = 0.f, dragStartOffset = 0.f;
        bool inited = false;
    };
    struct Cand {
        uint64_t id = 0;
        Rect rect;
        uint64_t scrollId = 0;
    };

    Canvas* canvas_ = nullptr;
    const md3::Theme* theme_ = nullptr;
    Fonts* fonts_ = nullptr;
    Input in_;

    std::unordered_map<uint64_t, NodeState> nodes_;
    std::unordered_map<uint64_t, ScrollState> scrolls_;
    std::vector<uint64_t> scrollStack_;
    std::vector<Cand> cands_;

    uint64_t hotId_ = 0, activeId_ = 0, focusedId_ = 0, hoveredId_ = 0;
    uint64_t dragScrollId_ = 0;   // какому scrollArea сейчас принадлежит палец
    bool activated_ = false;
    bool pointerConsumed_ = false;
    float lastScrollConsume_ = 0.f;

    NodeState& node(uint64_t id);
    float viewportTopOf(uint64_t scrollId) const;
    void moveFocus(int dx, int dy);
};

} // namespace tnx::ui