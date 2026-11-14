// md3/widgets.cpp — библиотека компонентов Material 3 (Expressive) на программном канвасе.
// Всё рисуется только через Canvas/Fonts: ни SDL, ни GPU, ни глобальных картинок.
//
// Соглашения:
//  * геометрия/типографика берётся из u.theme(), числа ниже — дефолты M3;
//  * интерактивность: u.interact() (hover/press/focus + регистрация в D-pad навигации),
//    поверх формы — u.stateLayer() и u.ripple();
//  * анимации, которым нужен Spring, живут в локальных кэшах по id (Ui не отдаёт NodeState).

#include "md3/widgets.hpp"

#include "md3/icons.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tnx::md3 {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// Material Symbols грузится в отдельный слот Fonts (роль 3), см. FontRole::Symbols.
constexpr int kIconFont = 3;

float clamp01f(float v) { return std::clamp(v, 0.f, 1.f); }

// Стабильное смешивание id: виджет получает собственные id для подэлементов.
uint64_t mixId(uint64_t base, uint64_t salt) {
    uint64_t h = base ^ (salt + 0x9e3779b97f4a7c15ull + (base << 6) + (base >> 2));
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h ? h : 1;
}

// Кодпоинт → UTF-8 (иконки рисуются как текст Material Symbols).
std::string utf8Encode(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

// Обрезка ASCII-пробелов: строки из сети приходят с лишними пробелами.
std::string trimAscii(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// Spring-кэши по id. Ограничиваем размер, чтобы не расти бесконечно на длинных сессиях.
std::unordered_map<uint64_t, Spring> g_switchAnim;
std::unordered_map<uint64_t, Spring> g_navAnim;
std::unordered_map<uint64_t, Spring> g_segAnim;
std::unordered_map<uint64_t, Spring> g_chipScroll;
uint64_t g_sliderActive = 0;

Spring& springFor(std::unordered_map<uint64_t, Spring>& cache, uint64_t id, float initial) {
    if (cache.size() > 4096) cache.clear();
    auto it = cache.find(id);
    if (it == cache.end()) {
        Spring s;
        // M3-08: параметры пружины берём из токенов движения.
        s.stiffness = motion.springStiffness;
        s.damping = motion.springDamping;
        s.setImmediate(initial);
        it = cache.emplace(id, s).first;
    }
    return it->second;
}

// ---------- мелкие хелперы рисования ----------

float textLH(ui::Ui& u, const TextStyle& s) {
    return u.fonts().lineHeight((int)s.font, s.size);
}

void textAt(ui::Ui& u, const TextStyle& s, float x, float y, const std::string& t, const Color& c) {
    u.fonts().draw((int)s.font, s.size, x, y, t, c, s.letterSpacing);
}

void textVCenter(ui::Ui& u, const TextStyle& s, float x, const Rect& box, const std::string& t,
                 const Color& c) {
    textAt(u, s, x, box.y + (box.h - textLH(u, s)) * 0.5f, t, c);
}

void textCenter(ui::Ui& u, const TextStyle& s, const Rect& box, const std::string& t, const Color& c) {
    u.fonts().drawCentered((int)s.font, s.size, box, t, c, s.letterSpacing);
}

// Перенос по словам с фиксированным числом строк: интерлиньяж задаёт макет
// (например, 44 px на две строки), а не метрики шрифта.
std::vector<std::string> wrapLines(ui::Ui& u, const TextStyle& s, float maxW, const std::string& text,
                                   int maxLines, bool* truncated) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto flush = [&]() {
        if (word.empty()) return;
        std::string probe = cur.empty() ? word : cur + " " + word;
        if (cur.empty() || u.fonts().measure((int)s.font, s.size, probe, s.letterSpacing) <= maxW)
            cur = probe;
        else {
            lines.push_back(cur);
            cur = word;
        }
        word.clear();
    };
    for (int cp : utf8Decode(text)) {
        if (cp == '\n') {
            flush();
            lines.push_back(cur);
            cur.clear();
            continue;
        }
        if (cp == ' ') {
            flush();
            continue;
        }
        char buf[5] = {0};
        if (cp < 0x80) buf[0] = (char)cp;
        else if (cp < 0x800) {
            buf[0] = (char)(0xC0 | (cp >> 6));
            buf[1] = (char)(0x80 | (cp & 0x3F));
        } else {
            buf[0] = (char)(0xE0 | (cp >> 12));
            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (cp & 0x3F));
        }
        word += buf;
    }
    flush();
    if (!cur.empty()) lines.push_back(cur);
    bool tr = (int)lines.size() > maxLines;
    if (tr) lines.resize((size_t)maxLines);
    if (truncated) *truncated = tr;
    return lines;
}

void drawLines(ui::Ui& u, const TextStyle& s, float x, float y, float maxW, float lh,
               const std::vector<std::string>& lines, bool truncated, const Color& c) {
    (void)truncated; // каждая строка обрезается по ширине — длинное слово не вылезет
    for (size_t i = 0; i < lines.size(); i++) {
        float ly = y + (float)i * lh;
        u.fonts().drawEllipsized((int)s.font, s.size, x, ly, maxW, lines[i], c, s.letterSpacing);
    }
}

void drawShadow(Canvas& cv, const Theme& th, const Rect& r, const CornerRadii& cr, int level) {
    Elevation e = elevationFor(level);
    if (e.blur <= 0.f) return;
    cv.shadow(r, cr, e.blur, e.dy, th.colors.shadow.withAlpha(e.alpha));
}

// State layer + мягкая волна. base — цвет поверхности под виджетом (волна подмешивается к нему).
void overlay(ui::Ui& u, uint64_t id, const Rect& shape, const CornerRadii& cr, const Color& tint,
             const Color& base) {
    u.stateLayer(id, shape, cr, tint, 0.f);
    u.ripple(id, shape, cr, Color::lerp(base, tint, 0.12f));
}

// 3dp-кольцо фокуса снаружи формы (M3 focus indicator): зазор 2 px.
void focusRing(ui::Ui& u, uint64_t id, const Rect& r, const CornerRadii& cr, const Color& c) {
    float f = u.focusAmount(id);
    if (f < 0.02f) return;
    CornerRadii o(cr.tl + 2.f, cr.tr + 2.f, cr.br + 2.f, cr.bl + 2.f);
    u.canvas().strokeRoundRect(r.inset(-2.f), o, 3.f, c.withAlpha(f));
}

// Кольцо фокуса внутрь строки меню/списка (offset −3, Р-11).
void focusRingInside(ui::Ui& u, uint64_t id, const Rect& r, const CornerRadii& cr, const Color& c) {
    float f = u.focusAmount(id);
    if (f < 0.02f) return;
    CornerRadii o(std::max(0.f, cr.tl - 3.f), std::max(0.f, cr.tr - 3.f),
                  std::max(0.f, cr.br - 3.f), std::max(0.f, cr.bl - 3.f));
    u.canvas().strokeRoundRect(r.inset(3.f), o, 3.f, c.withAlpha(f));
}

// Фокус поверх видео (Р-05): кольцо primary + внешняя тёмная обводка 7 px.
void focusRingOnVideo(ui::Ui& u, uint64_t id, const Rect& r, const CornerRadii& cr, const Color& c) {
    float f = u.focusAmount(id);
    if (f < 0.02f) return;
    CornerRadii dark(cr.tl + 5.5f, cr.tr + 5.5f, cr.br + 5.5f, cr.bl + 5.5f);
    u.canvas().strokeRoundRect(r.inset(-5.5f), dark, 7.f, Color(0, 0, 0, (uint8_t)std::lround(0.7f * f * 255.f)));
    CornerRadii o(cr.tl + 2.f, cr.tr + 2.f, cr.br + 2.f, cr.bl + 2.f);
    u.canvas().strokeRoundRect(r.inset(-2.f), o, 3.f, c.withAlpha(f));
}

// Дуга из маленьких кружков: сглаживается AA-канвасом, концы автоматически скруглены.
void arcDots(Canvas& cv, Vec2 center, float radius, float a0, float a1, float stroke,
             const Color& col, bool taper) {
    float arc = std::fabs(a1 - a0);
    float rr = stroke * 0.5f;
    int steps = std::max(2, (int)std::ceil(arc * std::max(1.f, radius)));
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float a = a0 + (a1 - a0) * t;
        float r = radius;
        float rad = rr;
        if (taper) {
            // Хвост индетерминантного индикатора сужается (без альфа-наложений).
            rad = rr * (0.25f + 0.75f * t);
            r = radius - (rr - rad) * 0.5f;
        }
        cv.fillCircle(center.x + std::cos(a) * r, center.y + std::sin(a) * r, rad, col);
    }
}

} // namespace

Rect hitRect(const Rect& r, float minW, float minH) {
    Rect h = r;
    if (h.w < minW) {
        h.x = r.center().x - minW * 0.5f;
        h.w = minW;
    }
    if (h.h < minH) {
        h.y = r.center().y - minH * 0.5f;
        h.h = minH;
    }
    return h;
}

// ======================================================================== иконки

void icon(ui::Ui& u, const char* name, float x, float y, float size, const Color& c) {
    uint32_t cp = iconCodepoint(name);
    if (cp == 0 || size <= 0.f || c.a == 0) return;
    std::string glyph = utf8Encode(cp);
    u.fonts().drawCentered(kIconFont, size, Rect(x, y, size, size), glyph, c);
}

void iconCentered(ui::Ui& u, const char* name, const Rect& box, float size, const Color& c) {
    if (size <= 0.f) return;
    Vec2 ctr = box.center();
    icon(u, name, ctr.x - size * 0.5f, ctr.y - size * 0.5f, size, c);
}

bool iconButton(ui::Ui& u, uint64_t id, const Rect& r, const char* iconName, const Color& tint,
                float iconSize, bool toggleOn, const Color* toggleBg, bool onVideo) {
    const Theme& th = u.theme();
    Canvas& cv = u.canvas();

    // State layer/ripple — по всему визуальному кругу (40/44/48), hit — ≥ 48×48.
    Rect hit = hitRect(r);
    bool act = u.interact(id, hit);

    float layer = std::min(r.w, r.h);
    Rect shape{r.center().x - layer * 0.5f, r.center().y - layer * 0.5f, layer, layer};
    CornerRadii cr(layer * 0.5f);

    if (toggleOn) {
        Color bg = toggleBg ? *toggleBg : tint.withAlpha(0.12f);
        cv.fillRoundRect(shape, cr, bg);
    }
    overlay(u, id, shape, cr, tint, th.colors.surface);
    if (onVideo)
        focusRingOnVideo(u, id, shape, cr, th.colors.primary);
    else
        focusRing(u, id, shape, cr, th.colors.primary);
    iconCentered(u, iconName, r, iconSize, tint);
    return act;
}

// ======================================================================== кнопки

bool button(ui::Ui& u, uint64_t id, const Rect& r, const std::string& label, const ButtonOpts& o) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();

    float h = o.height > 0.f ? o.height : 40.f;
    Rect box{r.x, r.y, r.w, h};
    if (r.h > h) box.y = r.y + (r.h - h) * 0.5f;
    CornerRadii cr(th.shapes.full);

    Color bg = c.primary, fg = c.onPrimary, border = c.outline, tint = c.onPrimary;
    Color base = c.primary;
    bool hasBorder = false, hasShadow = false;
    switch (o.kind) {
        case ButtonKind::Filled:
            bg = c.primary; fg = c.onPrimary; tint = c.onPrimary; base = c.primary;
            break;
        case ButtonKind::Tonal:
            bg = c.secondaryContainer; fg = c.onSecondaryContainer;
            tint = c.onSecondaryContainer; base = c.secondaryContainer;
            break;
        case ButtonKind::Elevated:
            bg = c.surfaceContainerLow; fg = c.primary; tint = c.primary;
            base = c.surfaceContainerLow; hasShadow = true;
            break;
        case ButtonKind::Outlined:
            bg = Color(0, 0, 0, 0); fg = c.primary; tint = c.primary; base = c.surface;
            hasBorder = true;
            break;
        case ButtonKind::Text:
            bg = Color(0, 0, 0, 0); fg = c.primary; tint = c.primary; base = c.surface;
            break;
    }
    if (!o.enabled) {
        fg = c.onSurface.withAlpha(0.38f);
        bg = (o.kind == ButtonKind::Text) ? Color(0, 0, 0, 0) : c.onSurface.withAlpha(0.12f);
        border = c.onSurface.withAlpha(0.12f);
        tint = c.onSurface;
        base = c.surface;
        hasShadow = false;
    }

    Rect hit = hitRect(box);
    bool act = o.enabled ? u.interact(id, hit) : false;
    if (hasShadow && bg.a > 0) drawShadow(cv, th, box, cr, 1);
    if (bg.a > 0) cv.fillRoundRect(box, cr, bg);
    if (hasBorder) cv.strokeRoundRect(box, cr, 1.f, border);
    if (o.enabled) {
        overlay(u, id, box, cr, tint, base);
        if (o.onVideo)
            focusRingOnVideo(u, id, box, cr, c.primary);
        else
            focusRing(u, id, box, cr, c.primary);
    }

    // Содержимое: [иконка 18 + 8] + labelLarge, по центру.
    const TextStyle& s = th.type.labelLarge;
    bool hasIcon = o.iconName != nullptr && o.iconName[0] != '\0';
    float iconSz = 18.f, gap = 8.f;
    float tw = u.fonts().measure((int)s.font, s.size, label, s.letterSpacing);
    float total = (hasIcon ? iconSz + gap : 0.f) + tw;
    float cx = box.x + (box.w - total) * 0.5f;
    if (cx < box.x + 12.f) cx = box.x + 12.f;
    if (hasIcon) icon(u, o.iconName, cx, box.y + (box.h - iconSz) * 0.5f, iconSz, fg);
    float tx = cx + (hasIcon ? iconSz + gap : 0.f);
    float ty = box.y + (box.h - textLH(u, s)) * 0.5f;
    float maxW = std::max(0.f, box.r() - 12.f - tx);
    if (tw <= maxW) textAt(u, s, tx, ty, label, fg);
    else u.fonts().drawEllipsized((int)s.font, s.size, tx, ty, maxW, label, fg, s.letterSpacing);
    return act;
}

bool fab(ui::Ui& u, uint64_t id, Vec2 center, const char* iconName, FabSize size,
         const std::string& label) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();

    float w = 56.f, h = 56.f, radius = th.shapes.large;
    float iconSz = 24.f;
    if (size == FabSize::Small) {
        w = h = 40.f;
        radius = th.shapes.large;
        iconSz = 24.f;
    } else if (size == FabSize::Large) {
        w = h = 96.f;
        radius = th.shapes.extraLarge;
        iconSz = 36.f;
    }
    bool extended = !label.empty();
    if (extended) {
        const TextStyle& s = th.type.labelLarge;
        float tw = u.fonts().measure((int)s.font, s.size, label, s.letterSpacing);
        w = 20.f + 24.f + 12.f + tw + 20.f;
        h = 56.f;
        radius = th.shapes.large;
        iconSz = 24.f;
    }
    Rect box{center.x - w * 0.5f, center.y - h * 0.5f, w, h};
    CornerRadii cr(radius);

    bool act = u.interact(id, box);
    float hv = clamp01f(u.hoverAmount(id));

    // Тень уровня 3; при наведении / нажатии плавно уходит к уровню 4.
    Elevation e3 = elevationFor(3);
    Elevation e4 = elevationFor(4);
    float k = clamp01f(hv + u.pressAmount(id) * 0.5f);
    cv.shadow(box, cr, e3.blur + (e4.blur - e3.blur) * k, e3.dy + (e4.dy - e3.dy) * k,
              c.shadow.withAlpha(e3.alpha + (e4.alpha - e3.alpha) * k));
    cv.fillRoundRect(box, cr, c.primaryContainer);
    overlay(u, id, box, cr, c.onPrimaryContainer, c.primaryContainer);
    focusRing(u, id, box, cr, c.primary);

    if (extended) {
        const TextStyle& s = th.type.labelLarge;
        icon(u, iconName, box.x + 20.f, box.y + (h - 24.f) * 0.5f, 24.f, c.onPrimaryContainer);
        textVCenter(u, s, box.x + 20.f + 24.f + 12.f, box, label, c.onPrimaryContainer);
    } else {
        iconCentered(u, iconName, box, iconSz, c.onPrimaryContainer);
    }
    return act;
}

// ======================================================================== поверхности

void surfaceCard(ui::Ui& u, const Rect& r, CardKind kind, float radius, bool interactive) {
    const Theme& th = u.theme();
    Canvas& cv = u.canvas();
    // Без id интерактивность карточки невозможна — оставляем статичную поверхность.
    (void)interactive;
    float rad = radius >= 0.f ? radius : th.shapes.medium;
    CornerRadii cr(rad);

    if (kind == CardKind::Elevated) drawShadow(cv, th, r, cr, 1);
    Color bg = th.colors.surface;
    if (kind == CardKind::Filled) bg = th.colors.surfaceContainerHighest;
    else if (kind == CardKind::Elevated) bg = th.colors.surfaceContainerLow;
    cv.fillRoundRect(r, cr, bg);
    if (kind == CardKind::Outlined) cv.strokeRoundRect(r, cr, 1.f, th.colors.outlineVariant);
}

void divider(ui::Ui& u, const Rect& r) {
    Canvas& cv = u.canvas();
    float y = r.y + r.h * 0.5f - 0.5f;
    cv.fillRect(Rect(r.x, y, r.w, 1.f), u.theme().colors.outlineVariant);
}

void scrim(ui::Ui& u, float opacity) {
    Canvas& cv = u.canvas();
    Color c = u.theme().colors.scrim.withAlpha(clamp01f(opacity));
    cv.fillRect(Rect(0.f, 0.f, (float)cv.width(), (float)cv.height()), c);
}

// ======================================================================== списки / чипы

bool listItem(ui::Ui& u, uint64_t id, const Rect& r, const char* leadingIcon,
              const std::string& headline, const std::string& supporting, bool selected,
              const char* trailingIcon, bool ringInside) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();

    bool act = u.interact(id, r);
    float h = r.h > 0.f ? std::clamp(r.h, 56.f, 72.f) : 56.f;
    Rect box{r.x, r.y, r.w, h};
    CornerRadii cr(th.shapes.small);

    if (selected) cv.fillRoundRect(box, cr, c.secondaryContainer);
    overlay(u, id, box, cr, selected ? c.onSecondaryContainer : c.onSurface,
            selected ? c.secondaryContainer : c.surface);
    if (ringInside)
        focusRingInside(u, id, box, cr, c.primary);
    else
        focusRing(u, id, box, cr, c.primary);

    float x = box.x + 16.f;
    Color lead = selected ? c.onSecondaryContainer : c.onSurfaceVariant;
    if (leadingIcon && leadingIcon[0]) {
        iconCentered(u, leadingIcon, Rect(x, box.y + (h - 24.f) * 0.5f, 24.f, 24.f), 24.f, lead);
        x += 24.f + 16.f;
    }
    float right = box.r() - 16.f;
    if (trailingIcon && trailingIcon[0]) {
        Rect tr{box.r() - 16.f - 24.f, box.y + (h - 24.f) * 0.5f, 24.f, 24.f};
        iconCentered(u, trailingIcon, tr, 24.f, lead);
        right = tr.x - 8.f;
    }

    const TextStyle& hs = th.type.bodyLarge;
    const TextStyle& ss = th.type.bodyMedium;
    float hh = textLH(u, hs);
    float sh = supporting.empty() ? 0.f : textLH(u, ss);
    float y = box.y + (h - (hh + sh)) * 0.5f;
    float tw = std::max(0.f, right - x);
    Color hc = selected ? c.onSecondaryContainer : c.onSurface;
    Color sc = selected ? c.onSecondaryContainer.withAlpha(0.8f) : c.onSurfaceVariant;
    u.fonts().drawEllipsized((int)hs.font, hs.size, x, y, tw, headline, hc, hs.letterSpacing);
    if (!supporting.empty())
        u.fonts().drawEllipsized((int)ss.font, ss.size, x, y + hh, tw, supporting, sc, ss.letterSpacing);
    return act;
}

bool chip(ui::Ui& u, uint64_t id, Rect& rInOut, const std::string& label, bool selected,
          const char* iconName, bool assist, const ChipOpts& o) {
    (void)assist; // фильтр/assist различаются только иконкой вызывающего
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();
    const TextStyle& s = th.type.labelLarge;

    float h = o.height > 0.f ? o.height : (o.onVideo ? 36.f : 32.f);
    bool hasIcon = iconName != nullptr && iconName[0] != '\0';
    float iconSz = 18.f;
    float leading = hasIcon ? 8.f : 16.f;
    float gap = hasIcon ? 8.f : 0.f;
    float labelW = u.fonts().measure((int)s.font, s.size, label, s.letterSpacing);
    float w = leading + (hasIcon ? iconSz : 0.f) + gap + labelW + 16.f;

    Rect box{rInOut.x, rInOut.y, w, h};
    // В ряду высотой 48 чип центрируется по вертикали (низ — зона нажатия).
    if (rInOut.h > h + 0.5f) box.y = rInOut.y + (rInOut.h - h) * 0.5f;
    rInOut = box;
    CornerRadii cr(h * 0.5f);

    bool act = o.enabled ? u.interact(id, hitRect(box)) : false;
    float f = o.enabled ? clamp01f(u.focusAmount(id)) : 0.f;

    Color bg(0, 0, 0, 0), fg = c.onSurfaceVariant, border = c.outlineVariant, tint = c.onSurfaceVariant;
    Color iconCol = c.primary;
    bool drawBorder = true;
    if (o.onVideo) {
        tint = Color(255, 255, 255, 255);
        if (selected) {
            bg = Color(255, 255, 255, 255);
            fg = Color(0x10, 0x14, 0x18);
            drawBorder = false;
        } else if (o.off) {
            // Выключенный чип на видео: прозрачный + обводка белый 50 %.
            fg = Color(255, 255, 255).withAlpha(0.86f);
            border = Color(255, 255, 255).withAlpha(0.5f);
        } else {
            bg = Color(255, 255, 255).withAlpha(0.14f);
            fg = Color(255, 255, 255);
            drawBorder = false;
        }
        iconCol = fg;
    } else if (selected) {
        bg = c.secondaryContainer;
        fg = c.onSecondaryContainer;
        tint = c.onSecondaryContainer;
        iconCol = c.onSecondaryContainer;
        drawBorder = false;
    }

    // Фокус: заливка primaryContainer + текст onPrimaryContainer (Р-05/§5.5).
    if (f > 0.002f) {
        bg = Color::lerp(bg, c.primaryContainer, f);
        fg = Color::lerp(fg, c.onPrimaryContainer, f);
        iconCol = Color::lerp(iconCol, c.onPrimaryContainer, f);
    }
    if (!o.enabled) {
        fg = fg.withAlpha(0.38f);
        iconCol = iconCol.withAlpha(0.38f);
        bg = bg.withAlpha(bg.a * 0.38f / 255.f);
    }

    if (bg.a > 0) cv.fillRoundRect(box, cr, bg);
    if (drawBorder && border.a > 0 && o.enabled) cv.strokeRoundRect(box, cr, 1.f, border);
    if (o.enabled) {
        u.stateLayer(id, box, cr, tint, 0.f);
        u.ripple(id, box, cr, Color::lerp(bg, tint, 0.12f));
        if (o.onVideo)
            focusRingOnVideo(u, id, box, cr, c.primary);
        else
            focusRing(u, id, box, cr, c.primary);
    }

    if (hasIcon) {
        Rect ib{box.x + leading, box.y + (h - iconSz) * 0.5f, iconSz, iconSz};
        iconCentered(u, iconName, ib, iconSz, iconCol);
    }
    textVCenter(u, s, box.x + leading + (hasIcon ? iconSz + gap : 0.f), box, label, fg);
    return act;
}

int chipRow(ui::Ui& u, uint64_t id, const Rect& r, const std::vector<std::string>& labels,
            const std::vector<bool>& selected, int* scrollIndex, float chipHeight) {
    const int n = (int)labels.size();
    if (n <= 0) return -1;
    const TextStyle& s = u.theme().type.labelLarge;
    const float spacing = 8.f;

    // Предварительный расчёт ширин: chip() меняет ширину под контент.
    std::vector<float> widths((size_t)n, 0.f);
    std::vector<float> pos((size_t)n, 0.f);
    float total = 0.f;
    for (int i = 0; i < n; i++) {
        float labelW = u.fonts().measure((int)s.font, s.size, labels[(size_t)i], s.letterSpacing);
        widths[(size_t)i] = labelW + 32.f; // 16 + label + 16 (без иконки)
        total += widths[(size_t)i] + (i + 1 < n ? spacing : 0.f);
    }

    int first = scrollIndex ? std::clamp(*scrollIndex, 0, n - 1) : 0;
    float target = 0.f;
    for (int i = 0; i < first; i++) target += widths[(size_t)i] + spacing;
    float maxOff = std::max(0.f, total - r.w);
    target = std::clamp(target, 0.f, maxOff);

    Spring& off = springFor(g_chipScroll, id, target);
    off.target = target;
    off.update(u.dt());
    float ox = std::clamp(off.value, 0.f, maxOff);

    u.canvas().pushClip(Rect(r.x, r.y - 8.f, r.w, r.h + 16.f));
    float x = r.x - ox;
    // Чипы ряда стоят по верхнему краю зоны: на Home это y=76 (высота 32).
    float y = r.y;
    int pressed = -1;
    int focusIdx = -1;
    for (int i = 0; i < n; i++) {
        float w = widths[(size_t)i];
        pos[(size_t)i] = x;
        Rect box{x, y, w, chipHeight};
        // Рисуем все чипы (канвас обрежет по r) — так D-pad может дофокусировать
        // и «доскроллить» ряд до чипа, который сейчас за краем.
        uint64_t cid = mixId(id, (uint64_t)i + 1);
        bool sel = i < (int)selected.size() && selected[(size_t)i];
        Rect in = box;
        ChipOpts co;
        co.height = chipHeight;
        if (chip(u, cid, in, labels[(size_t)i], sel, nullptr, false, co)) pressed = i;
        if (u.isFocused(cid)) focusIdx = i;
        x += in.w + spacing;
    }
    u.canvas().popClip();

    if (scrollIndex) {
        if (pressed >= 0) {
            // Нажатый чип не помещается целиком — доводим ряд до него.
            float px = pos[(size_t)pressed], pw = widths[(size_t)pressed];
            if (px < r.x || px + pw > r.r()) *scrollIndex = pressed;
        } else if (focusIdx >= 0) {
            // Фокус ушёл за край — сдвигаем ряд (плавно через Spring).
            float fx = pos[(size_t)focusIdx];
            if (fx < r.x || fx + widths[(size_t)focusIdx] > r.r()) *scrollIndex = focusIdx;
        }
    }
    return pressed;
}

// ======================================================================== навигация

void topBar(ui::Ui& u, uint64_t id, const Rect& r, const std::string& title, bool big,
            bool* backPressed, const char* actionIcon, bool* actionPressed,
            const char* secondActionIcon, bool* secondActionPressed) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();

    // Крупный вариант рисуется только если вызывающий реально дал 112dp:
    // при сворачивании (например, во время скролла) он просто передаёт меньшую высоту.
    bool bigLayout = big && r.h >= 104.f;
    float h = r.h > 0.f ? r.h : (big ? 112.f : 64.f);
    Rect bar{r.x, r.y, r.w, h};
    cv.fillRect(bar, c.surface);

    float topRowH = bigLayout ? 64.f : h;
    float btnY = bar.y + (topRowH - 48.f) * 0.5f;

    // Экшены справа, в 48dp-боксах.
    float ax = bar.r() - 4.f;
    int actions = 0;
    if (actionIcon && actionIcon[0]) actions++;
    if (secondActionIcon && secondActionIcon[0]) actions++;
    if (actionIcon && actionIcon[0]) {
        ax -= 48.f;
        bool act = iconButton(u, mixId(id, 101), Rect(ax, btnY, 48.f, 48.f), actionIcon, c.onSurface, 24.f);
        if (actionPressed) *actionPressed = act;
    }
    if (secondActionIcon && secondActionIcon[0]) {
        ax -= 48.f;
        bool act = iconButton(u, mixId(id, 102), Rect(ax, btnY, 48.f, 48.f), secondActionIcon,
                              c.onSurface, 24.f);
        if (secondActionPressed) *secondActionPressed = act;
    }
    float titleRight = actions > 0 ? std::max(bar.x + 72.f, ax - 4.f) : bar.r() - 16.f;

    float titleX = bar.x + 16.f;
    if (backPressed) {
        Rect br{bar.x + 4.f, btnY, 48.f, 48.f};
        if (iconButton(u, mixId(id, 103), br, "arrow_back", c.onSurface, 24.f)) *backPressed = true;
        titleX = bar.x + 4.f + 48.f + 8.f;
    }

    if (bigLayout) {
        const TextStyle& s = th.type.headlineMedium;
        float ty = bar.b() - 16.f - textLH(u, s);
        u.fonts().drawEllipsized((int)s.font, s.size, bar.x + 16.f, ty, std::max(0.f, bar.r() - 32.f),
                                 title, c.onSurface, s.letterSpacing);
    } else {
        const TextStyle& s = th.type.titleLarge;
        float ty = bar.y + (h - textLH(u, s)) * 0.5f;
        u.fonts().drawEllipsized((int)s.font, s.size, titleX, ty,
                                 std::max(0.f, titleRight - titleX), title, c.onSurface, s.letterSpacing);
    }
}

bool navBar(ui::Ui& u, uint64_t id, const Rect& r, const std::vector<std::string>& labels,
            const std::vector<std::string>& icons, int& selected) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();

    int n = (int)std::min(labels.size(), icons.size());
    n = std::min(n, 5);
    if (n <= 0) return false;
    selected = std::clamp(selected, 0, n - 1);

    float h = r.h > 0.f ? r.h : 80.f;
    Rect bar{r.x, r.y, r.w, h};
    cv.fillRect(bar, c.surfaceContainer);

    float itemW = bar.w / (float)n;
    const float pillW = 64.f, pillH = 32.f;
    float pillY = bar.y + 12.f;

    // Скользящий индикатор: один Spring на весь ряд.
    Spring& ind = springFor(g_navAnim, id, (float)selected);
    ind.target = (float)selected;
    ind.update(u.dt());
    float pillX = bar.x + (ind.value + 0.5f) * itemW - pillW * 0.5f;
    cv.fillRoundRect(Rect(pillX, pillY, pillW, pillH), pillH * 0.5f, c.secondaryContainer);

    bool changed = false;
    for (int i = 0; i < n; i++) {
        Rect item{bar.x + (float)i * itemW, bar.y, itemW, h};
        uint64_t iid = mixId(id, (uint64_t)i + 1);
        bool act = u.interact(iid, item);

        Rect sl = item.inset(6.f, 8.f);
        overlay(u, iid, sl, CornerRadii(16.f), c.onSurface, c.surfaceContainer);

        float sel = clamp01f(1.f - std::fabs(ind.value - (float)i));
        Color ic = Color::lerp(c.onSurfaceVariant, c.onSecondaryContainer, sel);
        Color lc = Color::lerp(c.onSurfaceVariant, c.secondary, sel);

        Rect ib{item.center().x - 12.f, pillY + (pillH - 24.f) * 0.5f, 24.f, 24.f};
        iconCentered(u, icons[(size_t)i].c_str(), ib, 24.f, ic);
        if (!labels[(size_t)i].empty()) {
            const TextStyle& s = th.type.labelMedium;
            float lh = textLH(u, s);
            u.fonts().drawCentered((int)s.font, s.size,
                                   Rect(item.x, pillY + pillH + 4.f, itemW, lh),
                                   labels[(size_t)i], lc, s.letterSpacing);
        }
        if (act && i != selected) {
            selected = i;
            changed = true;
        }
    }
    if (changed) ind.target = (float)selected;
    return changed;
}

bool segmented(ui::Ui& u, uint64_t id, const Rect& r, const std::vector<std::string>& labels,
               int& selected, float height) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();

    int n = (int)labels.size();
    if (n <= 0) return false;
    selected = std::clamp(selected, 0, n - 1);

    // Сегменты: высота 40 на всю ширину (r.h игнорируется — контракт §5.8).
    float h = height > 0.f ? height : 40.f;
    Rect box{r.x, r.y, r.w, h};
    float segW = box.w / (float)n;
    CornerRadii full(h * 0.5f);
    cv.fillRoundRect(box, full, c.surface);

    bool changed = false;
    for (int i = 0; i < n; i++) {
        Rect seg{box.x + (float)i * segW, box.y, segW, h};
        CornerRadii segCr(0.f);
        if (i == 0) { segCr.tl = full.tl; segCr.bl = full.bl; }
        if (i == n - 1) { segCr.tr = full.tr; segCr.br = full.br; }

        uint64_t sid = mixId(id, (uint64_t)i + 1);
        bool act = u.interact(sid, hitRect(seg, seg.w, 48.f));

        Spring& anim = springFor(g_segAnim, sid, i == selected ? 1.f : 0.f);
        anim.target = (i == selected) ? 1.f : 0.f;
        anim.update(u.dt());
        float t = clamp01f(anim.value);

        if (t > 0.002f) cv.fillRoundRect(seg, segCr, c.secondaryContainer.withAlpha(t));

        // Hover/press — onSurface, фокус — primary 10 % (заливка, как в макете).
        u.stateLayer(sid, seg, segCr, c.onSurface, 0.f);
        float f = clamp01f(u.focusAmount(sid));
        Color focusTint = (i == selected) ? c.onSecondaryContainer : c.primary;
        if (f > 0.002f) cv.fillRoundRect(seg, segCr, focusTint.withAlpha(0.10f * f));

        if (i > 0)
            cv.fillRect(Rect(seg.x - 0.5f, seg.y + 8.f, 1.f, std::max(0.f, h - 16.f)), c.outline);

        const TextStyle& s = th.type.labelLarge;
        float labelW = u.fonts().measure((int)s.font, s.size, labels[(size_t)i], s.letterSpacing);
        float checkSz = 18.f, gap = 6.f;
        bool hasCheck = (i == selected);
        float content = (hasCheck ? checkSz + gap : 0.f) + labelW;
        float cx = seg.x + (seg.w - content) * 0.5f;
        if (cx < seg.x + 8.f) cx = seg.x + 8.f;
        Color fg = Color::lerp(c.onSurface, c.onSecondaryContainer, t);
        if (hasCheck) {
            iconCentered(u, "check", Rect(cx, seg.y + (h - checkSz) * 0.5f, checkSz, checkSz),
                         checkSz, c.onSecondaryContainer.withAlpha(clamp01f(t * 1.5f)));
        }
        float tx = cx + (hasCheck ? checkSz + gap : 0.f);
        float maxW = std::max(0.f, seg.r() - 8.f - tx);
        float lh = textLH(u, s);
        float ty = seg.y + (h - lh) * 0.5f;
        if (labelW <= maxW) textAt(u, s, tx, ty, labels[(size_t)i], fg);
        else u.fonts().drawEllipsized((int)s.font, s.size, tx, ty, maxW, labels[(size_t)i], fg,
                                      s.letterSpacing);

        focusRingInside(u, sid, seg, segCr, c.primary);
        if (act && i != selected) {
            selected = i;
            changed = true;
        }
    }
    cv.strokeRoundRect(box, full, 1.f, c.outline);
    return changed;
}

// ======================================================================== индикаторы

void linearProgress(ui::Ui& u, const Rect& r, float progress) {
    const Theme& th = u.theme();
    Canvas& cv = u.canvas();
    float h = r.h > 1.f ? r.h : 4.f;
    float rad = h * 0.5f;
    Rect bar{r.x, r.y, r.w, h};

    cv.fillRoundRect(bar, rad, th.colors.surfaceContainerHighest);
    if (progress >= 0.f) {
        float w = bar.w * clamp01f(progress);
        if (w > 0.f) cv.fillRoundRect(Rect(bar.x, bar.y, w, h), rad, th.colors.primary);
        return;
    }

    // Индетерминантный: два бегущих отрезка с мягким ускорением.
    cv.pushClip(bar);
    auto segment = [&](float phase, float scale) {
        float p = std::fmod(u.time() * 0.55f + phase, 1.f);
        float e = p * p * (3.f - 2.f * p);
        float wf = (0.30f + 0.22f * std::sin(p * kPi)) * scale;
        float x = e * 1.3f - wf - 0.15f;
        Rect s{bar.x + x * bar.w, bar.y, wf * bar.w, h};
        Rect clipped = s.intersect(bar);
        if (clipped.w > 0.5f) cv.fillRoundRect(clipped, rad, th.colors.primary);
    };
    segment(0.f, 1.f);
    segment(0.55f, 0.75f);
    cv.popClip();
}

void circularProgress(ui::Ui& u, Vec2 center, float radius, float progress, float stroke) {
    if (radius <= 0.f) return;
    const Theme& th = u.theme();
    Canvas& cv = u.canvas();
    stroke = stroke > 0.f ? stroke : 4.f;
    float rr = std::max(1.f, radius - stroke * 0.5f);

    if (progress >= 0.f) {
        float p = clamp01f(progress);
        // Лёгкая дорожка помогает читать прогресс на тёмной теме.
        arcDots(cv, center, rr, -kPi * 0.5f, -kPi * 0.5f + kPi * 2.f, stroke,
                th.colors.surfaceContainerHighest, false);
        if (p > 0.001f)
            arcDots(cv, center, rr, -kPi * 0.5f, -kPi * 0.5f + kPi * 2.f * p, stroke,
                    th.colors.primary, false);
    } else {
        // Вращающаяся дуга 270°, хвост сужается.
        float a0 = u.time() * 4.2f;
        arcDots(cv, center, rr, a0, a0 + kPi * 1.5f, stroke, th.colors.primary, true);
    }
}

// ======================================================================== контролы

bool switchWidget(ui::Ui& u, uint64_t id, const Rect& r, bool& value) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();

    // Дорожка 52×32, зона нажатия 52×48 (Р-06).
    const float tw = 52.f, thh = 32.f;
    Rect track{r.x + (r.w - tw) * 0.5f, r.y + (r.h - thh) * 0.5f, tw, thh};
    if (r.w < tw) track = Rect(r.center().x - tw * 0.5f, r.center().y - thh * 0.5f, tw, thh);

    bool act = u.interact(id, hitRect(track, tw, 48.f));

    // Анимация позиции/размера thumb хранится в локальном кэше (Ui не отдаёт NodeState).
    Spring& pos = springFor(g_switchAnim, id, value ? 1.f : 0.f);
    pos.target = value ? 1.f : 0.f;
    pos.update(u.dt());
    float t = clamp01f(pos.value);

    bool toggled = false;
    if (act) {
        value = !value;
        pos.target = value ? 1.f : 0.f;
        toggled = true;
    }

    float hover = clamp01f(u.hoverAmount(id));
    float press = clamp01f(u.pressAmount(id));

    CornerRadii cr(thh * 0.5f);
    // Дорожка: on — primary, off — surfaceContainerHighest + 2dp outline.
    cv.fillRoundRect(track, cr, Color::lerp(c.surfaceContainerHighest, c.primary, t));
    if (t < 0.999f) cv.strokeRoundRect(track, cr, 2.f, c.outline.withAlpha(1.f - t));

    float cx = track.x + 16.f + t * 20.f;
    float cy = track.center().y;
    // Thumb: 16 off → 24 on → 28 pressed (диаметр).
    float thumbR = (16.f + 8.f * t + 4.f * press) * 0.5f;

    // Мягкая тень под thumb.
    Rect tb{cx - thumbR, cy - thumbR, thumbR * 2.f, thumbR * 2.f};
    cv.shadow(tb, CornerRadii(thumbR), 3.f, 1.f, c.shadow.withAlpha(0.25f));

    // Ореол 40 px: onSurface 8 % (off) / primary 12 % (on), усиливается фокусом.
    float halo = 0.08f * hover + 0.08f * clamp01f(u.focusAmount(id));
    Color haloCol = Color::lerp(c.onSurface, c.primary, t);
    if (halo > 0.002f) cv.fillCircle(cx, cy, 20.f, haloCol.withAlpha(halo));
    if (hover > 0.002f && t < 0.5f)
        cv.fillCircle(cx, cy, thumbR + 2.f, c.onSurfaceVariant.withAlpha(0.12f * hover));

    Color thumbCol = Color::lerp(c.outline, c.onPrimary, t);
    if (hover > 0.002f && t < 0.5f) thumbCol = Color::lerp(thumbCol, c.onSurfaceVariant, 0.5f * hover);
    cv.fillCircle(cx, cy, thumbR, thumbCol);

    focusRing(u, id, track, cr, c.primary);
    return toggled;
}

bool slider(ui::Ui& u, uint64_t id, const Rect& r, float& value, float min, float max,
            const SliderOpts& o) {
    const ColorScheme& c = u.theme().colors;
    Canvas& cv = u.canvas();
    const ui::Input& in = u.input();

    float lo = std::min(min, max);
    float hi = std::max(min, max);
    float range = hi - lo;
    if (range <= 1e-6f) range = 1.f;

    // Зона нажатия/фокуса — не меньше 48 по высоте (Р-06).
    Rect hit = hitRect(r, 48.f, 48.f);
    u.interact(id, hit);
    bool changed = false;
    const float pad = 12.f;
    float x0 = r.x + pad;
    float x1 = r.r() - pad;
    if (x1 < x0) x1 = x0;

    auto setFromX = [&](float px) {
        float v = lo + (hi - lo) * clamp01f((px - x0) / std::max(1.f, x1 - x0));
        if (std::fabs(v - value) > 1e-5f) {
            value = v;
            changed = true;
        }
    };

    // Перетаскивание: захват по нажатию внутри расширенной зоны.
    if (in.pressed && hit.contains(in.pointer)) {
        g_sliderActive = id;
        setFromX(in.pointer.x);
    }
    if (g_sliderActive == id) {
        if (in.down) setFromX(in.pointer.x);
        if (in.released || !in.down) g_sliderActive = 0;
    }

    // D-pad: шаг 1/20 диапазона.
    if (u.isFocused(id)) {
        float step = (hi - lo) / 20.f;
        if (in.left) { value = std::clamp(value - step, lo, hi); changed = true; }
        if (in.right) { value = std::clamp(value + step, lo, hi); changed = true; }
    }
    value = std::clamp(value, lo, hi);
    float v01 = (value - lo) / range;

    float f = clamp01f(u.focusAmount(id));
    float hover = clamp01f(u.hoverAmount(id));
    float press = clamp01f(u.pressAmount(id));
    float cy = r.center().y;
    float tx = x0 + v01 * (x1 - x0);

    // Линия 4 → 6 в фокусе, ползунок 14 → 20 в фокусе/нажатии.
    float trackH = o.track + (o.focusTrack - o.track) * f;
    float thumbA = std::max(f, press);
    float thumbR = (o.thumb + (o.focusThumb - o.thumb) * thumbA) * 0.5f;

    Color white(255, 255, 255, 255);
    // На видео прогресс/ползунок primary, у громкости — белые (app.css .vol).
    Color fillCol = o.volume ? white : c.primary;
    Color trackCol = o.onVideo ? white.withAlpha(0.18f) : c.surfaceContainerHighest;

    // Фокус дорожки на видео: подложка primary 12 % на область 44 px (§5.7).
    if (o.onVideo && f > 0.002f) {
        Rect zone{r.x, cy - 22.f, r.w, 44.f};
        cv.fillRoundRect(zone, 22.f, c.primary.withAlpha(0.12f * f));
    }

    Rect trackBg{x0, cy - trackH * 0.5f, std::max(0.f, x1 - x0), trackH};
    float tr = trackH * 0.5f;
    cv.fillRoundRect(trackBg, tr, trackCol);
    if (o.onVideo && o.buffer >= 0.f) {
        float bw = (x1 - x0) * clamp01f(o.buffer);
        if (bw > 0.f) cv.fillRoundRect(Rect(x0, cy - trackH * 0.5f, bw, trackH), tr, white.withAlpha(0.30f));
    }
    if (tx > x0) cv.fillRoundRect(Rect(x0, cy - trackH * 0.5f, tx - x0, trackH), tr, fillCol);

    // Ореол: primary 12 % / белый 12 % на видео.
    float halo = 0.12f * hover + 0.16f * press;
    if (halo > 0.002f)
        cv.fillCircle(tx, cy, thumbR + 9.f, (o.onVideo ? white : c.primary).withAlpha(halo));

    // Кольцо фокуса: 3 px + зазор 2 (Р-04). На видео — белое с тёмным кантом.
    if (f > 0.002f) {
        if (o.onVideo) {
            cv.fillCircle(tx, cy, thumbR + 5.f, Color(0, 0, 0, (uint8_t)std::lround(0.6f * f * 255.f)));
            cv.fillCircle(tx, cy, thumbR + 3.f, white.withAlpha(f));
        } else {
            cv.fillCircle(tx, cy, thumbR + 5.f, c.primary.withAlpha(f));
            cv.fillCircle(tx, cy, thumbR + 2.f, c.surface.withAlpha(f));
        }
    }
    cv.fillCircle(tx, cy, thumbR, fillCol);
    return changed;
}

SearchBarResult searchBar(ui::Ui& u, uint64_t id, const Rect& r, const std::string& text,
                          const std::string& placeholder, const SearchBarOpts& o) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();
    Fonts& f = u.fonts();
    const TextStyle& s = th.type.bodyLarge;
    SearchBarResult res;

    float h = o.height > 0.f ? o.height : 52.f;
    Rect box{r.x, r.y, r.w, h};
    CornerRadii cr(h * 0.5f);

    bool act = o.enabled ? u.interact(id, box) : false;
    res.activated = act;

    float fAmt = clamp01f(u.focusAmount(id));
    float focusK = std::max(fAmt, o.focused ? 1.f : 0.f);

    // Контейнер surfaceContainerHigh; фокус — primary 10 % поверх него.
    Color bg = c.surfaceContainerHigh;
    if (focusK > 0.002f) bg = Color::lerp(bg, c.primary, 0.10f * focusK);

    cv.fillRoundRect(box, cr, bg);
    if (o.enabled) {
        // Hover 8 % / pressed 10 % onSurface (без лишнего фокусного слоя).
        float hv = clamp01f(u.hoverAmount(id)), pk = clamp01f(u.pressAmount(id));
        if (hv > 0.002f) cv.fillRoundRect(box, cr, c.onSurface.withAlpha(0.08f * hv));
        if (pk > 0.002f) cv.fillRoundRect(box, cr, c.onSurface.withAlpha(0.10f * pk));
        u.ripple(id, box, cr, Color::lerp(bg, c.onSurface, 0.12f));
    }
    if (focusK > 0.02f) {
        CornerRadii ringCr(h * 0.5f + 2.f);
        cv.strokeRoundRect(box.inset(-2.f), ringCr, 3.f, c.primary.withAlpha(focusK));
    }

    // Иконка search 20: в фокусе — primary.
    Color iconCol = Color::lerp(c.onSurfaceVariant, c.primary, focusK);
    if (o.leadingIcon && o.leadingIcon[0])
        iconCentered(u, o.leadingIcon, Rect(box.x + 16.f, box.y + (h - 20.f) * 0.5f, 20.f, 20.f), 20.f,
                     iconCol);

    float trailingW = (o.trailingIcon && o.trailingIcon[0]) ? 48.f : 4.f;
    float tx = box.x + 16.f + 20.f + 12.f;
    float maxW = std::max(0.f, box.r() - trailingW - 8.f - tx);
    float ty = box.y + (h - textLH(u, s)) * 0.5f;
    const std::string& shown = text.empty() ? placeholder : text;
    Color textCol = text.empty() ? c.onSurfaceVariant : c.onSurface;
    if (!o.enabled) textCol = textCol.withAlpha(0.38f);
    f.drawEllipsized((int)s.font, s.size, tx, ty, maxW, shown, textCol, s.letterSpacing);

    // Курсор 2×22 primary, мигает.
    if (o.focused && o.enabled) {
        float caretX = tx;
        if (!text.empty()) caretX += f.measure((int)s.font, s.size, text, s.letterSpacing) + 2.f;
        if (std::fmod(u.time(), 1.f) < 0.55f) {
            Rect caret{caretX, box.y + (h - 22.f) * 0.5f, 2.f, 22.f};
            cv.fillRoundRect(caret, 1.f, c.primary);
        }
    }

    if (o.trailingIcon && o.trailingIcon[0]) {
        uint64_t tid = mixId(id, 0x7e11);
        Rect tr{box.r() - 8.f - 40.f, box.y + (h - 40.f) * 0.5f, 40.f, 40.f};
        res.trailing = iconButton(u, tid, tr, o.trailingIcon, c.onSurfaceVariant, 24.f);
    }
    return res;
}

void snackbar(ui::Ui& u, const std::string& text, float appear, const char* action,
              bool* actionPressed) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();

    float a = clamp01f(appear);
    if (actionPressed) *actionPressed = false;
    if (a <= 0.001f) return;

    const TextStyle& ts = th.type.bodyMedium;
    const TextStyle& as = th.type.labelLarge;
    float padding = 16.f;
    bool hasAction = action != nullptr && action[0] != '\0';
    float actionW = hasAction
        ? u.fonts().measure((int)as.font, as.size, action, as.letterSpacing) + 24.f
        : 0.f;
    float textW = u.fonts().measure((int)ts.font, ts.size, text, ts.letterSpacing);
    float maxW = std::min(600.f, (float)cv.width() - 32.f);
    float w = std::min(maxW, padding + textW + (hasAction ? 8.f + actionW : 0.f) + 8.f);

    // Въезд снизу + fade.
    float slide = (1.f - a) * 56.f;
    Rect box{(float)cv.width() * 0.5f - w * 0.5f, (float)cv.height() - 24.f - 48.f + slide, w, 48.f};
    CornerRadii cr(th.shapes.extraSmall);
    drawShadow(cv, th, box, cr, 3);
    cv.fillRoundRect(box, cr, c.inverseSurface.withAlpha(a));

    float maxTextW = box.w - padding - 8.f - (hasAction ? actionW : 0.f);
    float ty = box.y + (box.h - textLH(u, ts)) * 0.5f;
    if (textW <= maxTextW)
        textAt(u, ts, box.x + padding, ty, text, c.inverseOnSurface.fade(a));
    else
        u.fonts().drawEllipsized((int)ts.font, ts.size, box.x + padding, ty,
                                 std::max(0.f, maxTextW), text, c.inverseOnSurface.fade(a),
                                 ts.letterSpacing);

    if (hasAction) {
        Rect ab{box.r() - 8.f - actionW, box.y, actionW, box.h};
        uint64_t aid = mixId(ui::Ui::hash(action), 0x5ca1ab1eull);
        bool act = u.interact(aid, ab);
        u.stateLayer(aid, ab, CornerRadii(th.shapes.extraSmall), c.inversePrimary, 0.f);
        u.ripple(aid, box, cr, Color::lerp(c.inverseSurface, c.inversePrimary, 0.12f));
        textCenter(u, as, ab, action, c.inversePrimary.fade(a));
        if (actionPressed) *actionPressed = act;
    }
}

int dialog(ui::Ui& u, uint64_t id, const Rect& r, const std::string& title, const std::string& body,
           const char* confirm, const char* dismiss) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();

    // Скрим 50% на весь экран (в макете диалог заметно гасит фон).
    scrim(u, 0.50f);

    Rect panel = r;
    if (panel.w < 1.f || panel.h < 1.f) {
        float w = std::min(560.f, (float)cv.width() - 48.f);
        float h = std::min(360.f, (float)cv.height() - 48.f);
        panel = Rect(((float)cv.width() - w) * 0.5f, ((float)cv.height() - h) * 0.5f, w, h);
    }
    CornerRadii cr(th.shapes.extraLarge);
    drawShadow(cv, th, panel, cr, 3);
    cv.fillRoundRect(panel, cr, c.surfaceContainerHigh);

    float pad = 24.f;
    Rect inner = panel.inset(pad);
    float y = inner.y;

    const TextStyle& hs = th.type.headlineSmall;
    if (!title.empty()) {
        u.fonts().drawEllipsized((int)hs.font, hs.size, inner.x, y, inner.w, title, c.onSurface,
                                 hs.letterSpacing);
        y += textLH(u, hs) + 16.f;
    }

    float btnH = 40.f;
    float btnY = inner.b() - btnH;
    float bodyBottom = btnY - 12.f;
    const TextStyle& bs = th.type.bodyMedium;
    if (!body.empty() && bodyBottom > y) {
        Rect bodyBox{inner.x, y, inner.w, bodyBottom - y};
        cv.pushClip(bodyBox);
        u.fonts().drawWrapped((int)bs.font, bs.size, bodyBox, body, c.onSurfaceVariant, 100,
                              bs.letterSpacing, true);
        cv.popClip();
    }

    int result = 0;
    float bx = inner.r();
    if (confirm && confirm[0]) {
        const TextStyle& ls = th.type.labelLarge;
        float bw = std::max(64.f, u.fonts().measure((int)ls.font, ls.size, confirm, ls.letterSpacing) + 32.f);
        bx -= bw;
        ButtonOpts o;
        o.kind = ButtonKind::Text;
        o.height = btnH;
        if (button(u, mixId(id, 201), Rect(bx, btnY, bw, btnH), confirm, o)) result = 1;
        bx -= 8.f;
    }
    if (dismiss && dismiss[0]) {
        const TextStyle& ls = th.type.labelLarge;
        float bw = std::max(64.f, u.fonts().measure((int)ls.font, ls.size, dismiss, ls.letterSpacing) + 32.f);
        bx -= bw;
        ButtonOpts o;
        o.kind = ButtonKind::Text;
        o.height = btnH;
        if (button(u, mixId(id, 202), Rect(bx, btnY, bw, btnH), dismiss, o)) result = 2;
    }

    // B / back закрывает диалог как dismiss.
    if (result == 0 && u.input().back) result = 2;
    return result;
}

// ======================================================================== карточка видео

namespace {

// Канал и метаданные: новый вид (d.channel + d.meta) или старый «канал • мета»,
// который пока кладёт screens.cpp — так экран не ломается до обновления.
void splitMeta(const VideoTileData& d, std::string& channel, std::string& meta) {
    channel = trimAscii(d.channel);
    meta = trimAscii(d.meta);
    if (!channel.empty()) return;
    size_t p = meta.find("•");
    if (p != std::string::npos) {
        channel = trimAscii(meta.substr(0, p));
        meta = trimAscii(meta.substr(p + 3)); // «•» — 3 байта UTF-8
    } else if (!meta.empty()) {
        channel = meta;
        meta.clear();
    }
}

// Обложка-скелетон: тон + полупрозрачный play_arrow.
void skeletonThumb(ui::Ui& u, const Rect& thumb, const CornerRadii& cr) {
    const ColorScheme& c = u.theme().colors;
    u.canvas().fillRoundRect(thumb, cr, c.surfaceContainerHighest);
    float ps = std::min(thumb.w, thumb.h) * 0.25f;
    iconCentered(u, "play_arrow", thumb, ps, c.onSurfaceVariant.withAlpha(0.25f));
}

// NET-02: обложка недоступна (все размеры 404 или ошибка сети). Плоский
// surfaceContainerHigh из макета (.vcard .cover), без «пульсации» скелетона —
// тогда загрузка и «нет обложки» визуально различимы.
void placeholderThumb(ui::Ui& u, const Rect& thumb, const CornerRadii& cr) {
    const ColorScheme& c = u.theme().colors;
    u.canvas().fillRoundRect(thumb, cr, c.surfaceContainerHigh);
    float ps = std::min(thumb.w, thumb.h) * 0.22f;
    iconCentered(u, "play_arrow", thumb, ps, c.onSurfaceVariant.withAlpha(0.16f));
}

} // namespace

bool videoTile(ui::Ui& u, uint64_t id, const Rect& r, const VideoTileData& d) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();
    Fonts& f = u.fonts();

    // Ширина снаружи (296 handheld / 276 dock): обложка floor(w*9/16),
    // высота карточки = обложка + 90 (256 / 245). Карточка не двигается.
    float thumbH = std::floor(r.w * 9.f / 16.f);
    Rect thumb{r.x, r.y, r.w, thumbH};
    Rect card{r.x, r.y, r.w, thumbH + 90.f};
    CornerRadii crThumb(16.f);
    CornerRadii crCard(22.f);

    bool act = u.interact(id, hitRect(card));
    float hover = clamp01f(u.hoverAmount(id));
    float focus = clamp01f(u.focusAmount(id));
    float press = clamp01f(u.pressAmount(id));

    // Фокус: подложка primaryContainer inset −8, r22 (не масштабирует карточку).
    if (focus > 0.02f) cv.fillRoundRect(card.inset(-8.f), crCard, c.primaryContainer.withAlpha(focus));
    // Нажатие: state layer onSurface 10 % по той же подложке.
    if (press > 0.02f) cv.fillRoundRect(card.inset(-8.f), crCard, c.onSurface.withAlpha(0.10f * press));

    // Обложка 16:9, r16.
    if (d.thumb && d.thumb->valid()) cv.image(*d.thumb, thumb, 1.f, crThumb);
    else if (d.thumbPlaceholder) placeholderThumb(u, thumb, crThumb);
    else skeletonThumb(u, thumb, crThumb);

    // Hover/focus — обложка +6 % белого, pressed — +10 %.
    float lit = std::min(0.22f, 0.06f * std::max(hover, focus) + 0.10f * press);
    if (lit > 0.002f) cv.fillRoundRect(thumb, crThumb, Color(255, 255, 255).withAlpha(lit));
    u.ripple(id, thumb, crThumb, Color::lerp(c.surfaceContainerHighest, c.onSurface, 0.10f));

    // Бейдж длительности: правый нижний угол 8, фон #B3000000, r6, padding 6/2.
    const TextStyle& ls = th.type.labelMedium;
    std::string badge = d.live ? std::string("LIVE") : d.duration;
    if (!badge.empty()) {
        float bw = f.measure((int)ls.font, ls.size, badge, ls.letterSpacing) + 12.f;
        Rect br{thumb.r() - 8.f - bw, thumb.b() - 8.f - 20.f, bw, 20.f};
        Color bg = d.live ? c.errorContainer : Color(0, 0, 0, 0xB3);
        Color fg = d.live ? c.onErrorContainer : Color(255, 255, 255, 255);
        cv.fillRoundRect(br, 6.f, bg);
        textCenter(u, ls, br, badge, fg);
    }

    // Прогресс просмотра: линия 3 px внизу обложки, дорожка белый 30 %.
    if (d.watchProgress >= 0.f) {
        float p = clamp01f(d.watchProgress);
        Rect tr{thumb.x, thumb.b() - 3.f, thumb.w, 3.f};
        cv.fillRect(tr, Color(255, 255, 255).withAlpha(0.30f));
        if (p > 0.f) cv.fillRect(Rect(tr.x, tr.y, tr.w * p, 3.f), c.primary);
    }

    // Play 48 на hover/focus: круг чёрный 45 %, белая иконка.
    float playA = std::max(hover, focus);
    if (playA > 0.02f) {
        Vec2 ctr = thumb.center();
        uint8_t bgA = (uint8_t)std::lround(0.45f * playA * 255.f);
        cv.fillCircle(ctr.x, ctr.y, 24.f, Color(0, 0, 0, bgA));
        iconCentered(u, "play_arrow", Rect(ctr.x - 12.f, ctr.y - 12.f, 24.f, 24.f), 24.f,
                     Color(255, 255, 255).withAlpha(playA));
    }

    // Кольцо фокуса вокруг обложки: 3 px primary, offset 2, r18.
    if (focus > 0.02f)
        cv.strokeRoundRect(thumb.inset(-2.f), CornerRadii(18.f), 3.f, c.primary.withAlpha(focus));

    // Текст: +8 заголовок titleMedium 2×22 (44), +4 канал bodySmall, +2 мета.
    const TextStyle& ts = th.type.titleMedium;
    const TextStyle& bs = th.type.bodySmall;
    std::string channel, meta;
    splitMeta(d, channel, meta);

    float titleY = thumb.b() + 8.f;
    bool truncated = false;
    std::vector<std::string> lines = wrapLines(u, ts, card.w, d.title, 2, &truncated);
    Color titleCol = Color::lerp(c.onSurface, c.onPrimaryContainer, focus);
    drawLines(u, ts, card.x, titleY, card.w, 22.f, lines, truncated, titleCol);

    Color subCol = Color::lerp(c.onSurfaceVariant, c.onPrimaryContainer.withAlpha(0.86f), focus);
    float channelY = titleY + 44.f + 4.f;
    if (!channel.empty())
        f.drawEllipsized((int)bs.font, bs.size, card.x, channelY, card.w, channel, subCol, bs.letterSpacing);
    if (!meta.empty())
        f.drawEllipsized((int)bs.font, bs.size, card.x, channelY + textLH(u, bs) + 2.f, card.w, meta, subCol,
                         bs.letterSpacing);
    return act;
}

bool videoTileCompact(ui::Ui& u, uint64_t id, const Rect& r, const VideoTileData& d) {
    const Theme& th = u.theme();
    const ColorScheme& c = th.colors;
    Canvas& cv = u.canvas();
    Fonts& f = u.fonts();

    // «Продолжить просмотр» (вариант A, app.css .ccard.ov): обложка 240×135 r12,
    // название поверх скрима (bottom 11, inset 12), прогресс 3 px внутри обложки.
    const float W = 240.f, H = 135.f;
    Rect thumb{r.x, r.y, W, H};
    Rect card = thumb;
    CornerRadii crThumb(12.f);

    bool act = u.interact(id, hitRect(card));
    float hover = clamp01f(u.hoverAmount(id));
    float focus = clamp01f(u.focusAmount(id));
    float press = clamp01f(u.pressAmount(id));

    // Фокус: подложка primaryContainer inset −6, r18 (app.css .ccard.is-focus).
    if (focus > 0.02f) cv.fillRoundRect(card.inset(-6.f), 18.f, c.primaryContainer.withAlpha(focus));
    if (press > 0.02f) cv.fillRoundRect(card.inset(-6.f), 18.f, c.onSurface.withAlpha(0.10f * press));

    if (d.thumb && d.thumb->valid()) cv.image(*d.thumb, thumb, 1.f, crThumb);
    else if (d.thumbPlaceholder) placeholderThumb(u, thumb, crThumb);
    else skeletonThumb(u, thumb, crThumb);

    float lit = std::min(0.22f, 0.06f * std::max(hover, focus) + 0.10f * press);
    if (lit > 0.002f) cv.fillRoundRect(thumb, crThumb, Color(255, 255, 255).withAlpha(lit));
    u.ripple(id, thumb, crThumb, Color::lerp(c.surfaceContainerHighest, c.onSurface, 0.10f));

    // Скрим под названием: чёрный 0 → 62 % → 80 %, высота 72 (app.css .ccard.ov).
    cv.fillLinearGradient3(Rect{thumb.x, thumb.b() - 72.f, thumb.w, 72.f}, CornerRadii(0.f, 0.f, 12.f, 12.f),
                           Color(0, 0, 0, 0), Color(0, 0, 0, 158), Color(0, 0, 0, 204), false);

    // Название поверх скрима: одна строка с ellipsis, inset 12, bottom 11, белый.
    const TextStyle& ts = th.type.titleSmall;
    f.drawEllipsized((int)ts.font, ts.size, thumb.x + 12.f, thumb.b() - 11.f - textLH(u, ts), W - 24.f, d.title,
                     Color(255, 255, 255), ts.letterSpacing);

    // Прогресс 3 px внутри обложки: дорожка белый 22 %, прогресс primary.
    if (d.watchProgress >= 0.f) {
        float p = clamp01f(d.watchProgress);
        Rect tr{thumb.x, thumb.b() - 3.f, thumb.w, 3.f};
        cv.fillRect(tr, Color(255, 255, 255).withAlpha(0.22f));
        if (p > 0.f) cv.fillRect(Rect(tr.x, tr.y, tr.w * p, 3.f), c.primary);
    }

    // Кольцо фокуса 3 px primary, offset 2, r14.
    if (focus > 0.02f)
        cv.strokeRoundRect(thumb.inset(-2.f), CornerRadii(14.f), 3.f, c.primary.withAlpha(focus));

    return act;
}

} // namespace tnx::md3