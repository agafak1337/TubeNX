// core/canvas.cpp
#include "core/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tnx {

namespace {
inline uint32_t packRGBA(const Color& c) {
    // libnx framebuffer RGBA_8888: байты в памяти R,G,B,A (little-endian u32).
    return (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | ((uint32_t)c.a << 24);
}
inline uint32_t packRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

// Цвет трёхточечного градиента (t в 0..1).
inline Color grad3(const Color& a, const Color& b, const Color& c, float t) {
    return t < 0.5f ? Color::lerp(a, b, t * 2.f) : Color::lerp(b, c, (t - 0.5f) * 2.f);
}

// Целочисленный диапазон x по пикселям, где cov == 1 (с запасом в полпикселя).
// Выводится из sdRoundRect: при |dx| <= w/2 - maxRad получаем qx <= -0.5.
inline void coreXRange(const Rect& r, float maxRad, int& xa, int& xb) {
    xa = (int)std::ceil(r.x + maxRad);
    xb = (int)std::floor(r.r() - maxRad - 1.f) + 1;
}
inline void coreYRange(const Rect& r, float maxRad, int& ya, int& yb) {
    ya = (int)std::ceil(r.y + maxRad);
    yb = (int)std::floor(r.b() - maxRad - 1.f) + 1;
}

// Заливка горизонтального отрезка полупрозрачным цветом: cov == 1 для всех
// пикселей отрезка. Целочисленное смешивание, как в Canvas::put, но без
// вызова put на каждый пиксель (это заметная часть кадра: чипы, кнопки).
inline void blendSpan(uint32_t* row, int x0, int x1, const Color& c) {
    const uint32_t a = c.a;
    if (a == 0) return;
    const uint32_t ia = 255u - a;
    const uint32_t cr = (uint32_t)c.r * a, cg = (uint32_t)c.g * a, cb = (uint32_t)c.b * a;
    for (int x = x0; x < x1; x++) {
        const uint32_t d = row[x];
        const uint32_t r = ((cr + (d & 0xFFu) * ia) * 257u + 32768u) >> 16;
        const uint32_t g = ((cg + ((d >> 8) & 0xFFu) * ia) * 257u + 32768u) >> 16;
        const uint32_t b = ((cb + ((d >> 16) & 0xFFu) * ia) * 257u + 32768u) >> 16;
        row[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
}
} // namespace

Canvas::Canvas() = default;
Canvas::~Canvas() = default;

void Canvas::attach(uint32_t* fb, int width, int height, int stridePx) {
    fb_ = fb;
    w_ = width;
    h_ = height;
    stride_ = stridePx > 0 ? stridePx : width;
    clipStack_.clear();
}

void Canvas::fill(const Color& c) {
    if (!fb_) return;
    const uint32_t v = packRGBA(c);
    // Одна строка — один std::fill_n (компилятор разворачивает в широкие записи).
    for (int y = 0; y < h_; y++) std::fill_n(fb_ + (size_t)y * stride_, w_, v);
}

void Canvas::pushClip(const Rect& r) {
    Rect nr = r.intersect(clip());
    clipStack_.push_back(nr);
}
void Canvas::popClip() {
    if (!clipStack_.empty()) clipStack_.pop_back();
}

inline void Canvas::put(int x, int y, const Color& c, float cov) {
    if (cov <= 0.003f) return;

    // Смешивание в целых числах: раньше на каждый канал приходились float-
    // умножения и lround (это дорого: ~40 нс на пиксель, а пикселей в кадре
    // сотни тысяч). Формула (c*a + d*(255-a))/255 через *257>>16 — тот же
    // результат с точностью 1/255.
    int cov8 = (int)(cov * 255.f + 0.5f);
    if (cov8 > 255) cov8 = 255;
    int a = (cov8 * c.a * 257) >> 16;   // = cov8 * c.a / 255
    if (a <= 0) return;
    uint32_t* p = fb_ + (size_t)y * stride_ + x;
    if (a >= 255) {
        *p = packRGBA(c);
        return;
    }
    const uint32_t d = *p;
    const uint32_t ia = (uint32_t)(255 - a);
    const uint32_t r = (((uint32_t)c.r * a + (d & 0xFF) * ia) * 257u + 32768u) >> 16;
    const uint32_t g = (((uint32_t)c.g * a + ((d >> 8) & 0xFF) * ia) * 257u + 32768u) >> 16;
    const uint32_t b = (((uint32_t)c.b * a + ((d >> 16) & 0xFF) * ia) * 257u + 32768u) >> 16;
    uint32_t oa = ((255u * a + ((d >> 24) & 0xFF) * ia) * 257u + 32768u) >> 16;
    if (oa > 255u) oa = 255u;
    *p = r | (g << 8) | (b << 16) | (oa << 24);
}

void Canvas::blendPixel(int x, int y, const Color& c, float coverage) {
    if (!fb_) return;
    Rect cl = clip();
    if (x < (int)cl.x || y < (int)cl.y || x >= (int)cl.r() || y >= (int)cl.b()) return;
    put(x, y, c, coverage);
}

float Canvas::sdRoundRect(float px, float py, const Rect& r, const CornerRadii& cr) {
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    float dx = px - cx, dy = py - cy;
    float rad;
    if (dy < 0) rad = (dx < 0) ? cr.tl : cr.tr;
    else        rad = (dx < 0) ? cr.bl : cr.br;
    float maxRad = std::min(r.w, r.h) * 0.5f;
    rad = std::clamp(rad, 0.f, maxRad);
    float qx = std::fabs(dx) - (r.w * 0.5f - rad);
    float qy = std::fabs(dy) - (r.h * 0.5f - rad);
    float ax = std::max(qx, 0.f), ay = std::max(qy, 0.f);
    float outside = std::sqrt(ax * ax + ay * ay);
    float inside = std::min(std::max(qx, qy), 0.f);
    return outside + inside - rad;
}

void Canvas::fillRect(const Rect& r, const Color& c) {
    fillRoundRect(r, CornerRadii(0), c);
}

void Canvas::fillRoundRect(const Rect& rIn, const CornerRadii& cr, const Color& c) {
    if (!fb_ || c.a == 0) return;
    Rect r = rIn.intersect(clip());
    if (r.empty()) return;
    int x0 = (int)std::floor(r.x), x1 = (int)std::ceil(r.r());
    int y0 = (int)std::floor(r.y), y1 = (int)std::ceil(r.b());
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, w_); y1 = std::min(y1, h_);
    if (x0 >= x1 || y0 >= y1) return;

    // Радиус ограничиваем так же, как sdRoundRect (иначе «ядро» может быть больше фигуры).
    const float maxRad = std::clamp(std::max(std::max(cr.tl, cr.tr), std::max(cr.br, cr.bl)), 0.f,
                                    std::min(rIn.w, rIn.h) * 0.5f);
    const Rect core = rIn.inset(maxRad + 1.f);
    const uint32_t v = packRGBA(c);
    const bool opaque = (c.a == 255);

    for (int y = y0; y < y1; y++) {
        const float py = y + 0.5f;
        uint32_t* row = fb_ + (size_t)y * stride_;
        // Строка целиком в «ядре» по Y — SDF не нужен вообще.
        if (py >= core.y && py <= core.b()) {
            if (opaque) {
                std::fill_n(row + x0, x1 - x0, v);
            } else {
                blendSpan(row, x0, x1, c);
            }
            continue;
        }

        // Иначе ищем В ЭТОЙ СТРОКЕ границы полного покрытия: у скруглённого
        // прямоугольника cov монотонно растёт от краёв к середине, поэтому
        // достаточно пройти кромки, а середину залить без SDF. Раньше для
        // «таблеток» (радиус = половина высоты, например чипы) ядра по Y не
        // было вовсе, и SDF считался на каждый пиксель — это была самая
        // дорогая операция кадра.
        int lo = x0, hi = x1 - 1;
        while (lo <= hi) {
            float cov = 0.5f - sdRoundRect(lo + 0.5f, py, rIn, cr);
            if (cov >= 1.f) break;
            if (cov > 0.f) put(lo, y, c, cov);
            ++lo;
        }
        while (hi >= lo) {
            float cov = 0.5f - sdRoundRect(hi + 0.5f, py, rIn, cr);
            if (cov >= 1.f) break;
            if (cov > 0.f) put(hi, y, c, cov);
            --hi;
        }
        if (lo <= hi) {
            if (opaque) std::fill_n(row + lo, hi - lo + 1, v);
            else blendSpan(row, lo, hi + 1, c);
        }
    }
}

void Canvas::strokeRoundRect(const Rect& rIn, const CornerRadii& cr, float width, const Color& c) {
    if (!fb_ || c.a == 0 || width <= 0.f) return;
    Rect r = rIn.inset(-width).intersect(clip());
    if (r.empty()) return;
    int x0 = std::max(0, (int)std::floor(r.x)), x1 = std::min(w_, (int)std::ceil(r.r()));
    int y0 = std::max(0, (int)std::floor(r.y)), y1 = std::min(h_, (int)std::ceil(r.b()));
    float half = width * 0.5f;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float d = sdRoundRect(x + 0.5f, y + 0.5f, rIn, cr);
            float cov = std::clamp(half + 0.5f - std::fabs(d), 0.f, 1.f);
            if (cov > 0.f) put(x, y, c, cov);
        }
    }
}

void Canvas::fillCircle(float cx, float cy, float radius, const Color& c) {
    if (!fb_ || c.a == 0 || radius <= 0.f) return;
    Rect clipR = clip();
    int x0 = std::max((int)clipR.x, (int)std::floor(cx - radius - 1));
    int x1 = std::min((int)clipR.r(), (int)std::ceil(cx + radius + 1));
    int y0 = std::max((int)clipR.y, (int)std::floor(cy - radius - 1));
    int y1 = std::min((int)clipR.b(), (int)std::ceil(cy + radius + 1));
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = std::sqrt(dx * dx + dy * dy) - radius;
            float cov = std::clamp(0.5f - d, 0.f, 1.f);
            if (cov > 0.f) put(x, y, c, cov);
        }
    }
}

void Canvas::fillLinearGradient(const Rect& rIn, const CornerRadii& cr, const Color& from, const Color& to,
                                bool horizontal) {
    if (!fb_ || (from.a == 0 && to.a == 0)) return;
    Rect r = rIn.intersect(clip());
    if (r.empty()) return;
    int x0 = std::max(0, (int)std::floor(r.x)), x1 = std::min(w_, (int)std::ceil(r.r()));
    int y0 = std::max(0, (int)std::floor(r.y)), y1 = std::min(h_, (int)std::ceil(r.b()));
    if (x0 >= x1 || y0 >= y1) return;

    const float maxRad = std::clamp(std::max(std::max(cr.tl, cr.tr), std::max(cr.br, cr.bl)), 0.f,
                                    std::min(rIn.w, rIn.h) * 0.5f);
    int ca, cb, ya, yb;
    coreXRange(rIn, maxRad, ca, cb);
    coreYRange(rIn, maxRad, ya, yb);
    ca = std::clamp(ca, x0, x1); cb = std::clamp(cb, x0, x1);
    if (cb < ca) cb = ca; // «ядро» пустое — кромки не должны пересекаться
    ya = std::clamp(ya, y0, y1); yb = std::clamp(yb, y0, y1);

    const float gw = std::max(1.f, rIn.w), gh = std::max(1.f, rIn.h);

    // Горизонтальный градиент: цвет зависит только от X — считаем один раз на столбец
    // и переиспользуем во всех строках. Буфер на стеке (ширина экрана <= 2048).
    static constexpr int kMaxCols = 2048;
    Color colBuf[kMaxCols];
    const bool colsReady = horizontal && (x1 - x0) <= kMaxCols;
    if (colsReady) {
        for (int x = x0; x < x1; x++)
            colBuf[x - x0] = Color::lerp(from, to, clamp01(((x + 0.5f) - rIn.x) / gw));
    }

    if (horizontal && !colsReady) {
        // Редкий случай (прямоугольник шире буфера) — прежний попиксельный путь.
        for (int y = y0; y < y1; y++) {
            const float py = y + 0.5f;
            for (int x = x0; x < x1; x++) {
                float d = sdRoundRect(x + 0.5f, py, rIn, cr);
                float cov = std::clamp(0.5f - d, 0.f, 1.f);
                if (cov <= 0.f) continue;
                put(x, y, Color::lerp(from, to, clamp01(((x + 0.5f) - rIn.x) / gw)), cov);
            }
        }
        return;
    }

    for (int y = y0; y < y1; y++) {
        const float py = y + 0.5f;
        const bool rowCore = (y >= ya && y < yb); // строка в «ядре» по Y
        uint32_t* row = fb_ + (size_t)y * stride_;

        // Вертикальный градиент: цвет один на всю строку.
        Color rowCol;
        if (!horizontal) rowCol = Color::lerp(from, to, clamp01((py - rIn.y) / gh));

        if (rowCore) {
            // Кромки: cov считаем как раньше, цвет уже готов.
            for (int x = x0; x < ca; x++) {
                float cov = std::clamp(0.5f - sdRoundRect(x + 0.5f, py, rIn, cr), 0.f, 1.f);
                if (cov > 0.f) put(x, y, horizontal ? colBuf[x - x0] : rowCol, cov);
            }
            // Ядро: cov == 1, SDF не нужен.
            for (int x = ca; x < cb; x++) {
                const Color& col = horizontal ? colBuf[x - x0] : rowCol;
                if (col.a == 255) row[x] = packRGBA(col);
                else if (col.a != 0) put(x, y, col, 1.f);
            }
            for (int x = cb; x < x1; x++) {
                float cov = std::clamp(0.5f - sdRoundRect(x + 0.5f, py, rIn, cr), 0.f, 1.f);
                if (cov > 0.f) put(x, y, horizontal ? colBuf[x - x0] : rowCol, cov);
            }
            continue;
        }
        for (int x = x0; x < x1; x++) {
            float cov = std::clamp(0.5f - sdRoundRect(x + 0.5f, py, rIn, cr), 0.f, 1.f);
            if (cov <= 0.f) continue;
            const Color& col = horizontal ? colBuf[x - x0] : rowCol;
            if (cov >= 1.f && col.a == 255) row[x] = packRGBA(col);
            else put(x, y, col, cov);
        }
    }
}

void Canvas::fillLinearGradient3(const Rect& rIn, const CornerRadii& cr, const Color& a, const Color& b,
                                 const Color& c, bool horizontal) {
    if (!fb_ || (a.a == 0 && b.a == 0 && c.a == 0)) return;
    Rect r = rIn.intersect(clip());
    if (r.empty()) return;
    int x0 = std::max(0, (int)std::floor(r.x)), x1 = std::min(w_, (int)std::ceil(r.r()));
    int y0 = std::max(0, (int)std::floor(r.y)), y1 = std::min(h_, (int)std::ceil(r.b()));
    if (x0 >= x1 || y0 >= y1) return;

    const float maxRad = std::clamp(std::max(std::max(cr.tl, cr.tr), std::max(cr.br, cr.bl)), 0.f,
                                    std::min(rIn.w, rIn.h) * 0.5f);
    int ca, cb, ya, yb;
    coreXRange(rIn, maxRad, ca, cb);
    coreYRange(rIn, maxRad, ya, yb);
    ca = std::clamp(ca, x0, x1); cb = std::clamp(cb, x0, x1);
    if (cb < ca) cb = ca;
    ya = std::clamp(ya, y0, y1); yb = std::clamp(yb, y0, y1);

    const float gw = std::max(1.f, rIn.w), gh = std::max(1.f, rIn.h);

    static constexpr int kMaxCols = 2048;
    Color colBuf[kMaxCols];
    const bool colsReady = horizontal && (x1 - x0) <= kMaxCols;
    if (colsReady) {
        for (int x = x0; x < x1; x++)
            colBuf[x - x0] = grad3(a, b, c, clamp01(((x + 0.5f) - rIn.x) / gw));
    }

    if (horizontal && !colsReady) {
        for (int y = y0; y < y1; y++) {
            const float py = y + 0.5f;
            for (int x = x0; x < x1; x++) {
                float d = sdRoundRect(x + 0.5f, py, rIn, cr);
                float cov = std::clamp(0.5f - d, 0.f, 1.f);
                if (cov <= 0.f) continue;
                put(x, y, grad3(a, b, c, clamp01(((x + 0.5f) - rIn.x) / gw)), cov);
            }
        }
        return;
    }

    for (int y = y0; y < y1; y++) {
        const float py = y + 0.5f;
        const bool rowCore = (y >= ya && y < yb);
        uint32_t* row = fb_ + (size_t)y * stride_;

        // Вертикальный градиент: цвет один на всю строку.
        Color rowCol;
        if (!horizontal) rowCol = grad3(a, b, c, clamp01((py - rIn.y) / gh));

        if (rowCore) {
            for (int x = x0; x < ca; x++) {
                float cov = std::clamp(0.5f - sdRoundRect(x + 0.5f, py, rIn, cr), 0.f, 1.f);
                if (cov > 0.f) put(x, y, horizontal ? colBuf[x - x0] : rowCol, cov);
            }
            for (int x = ca; x < cb; x++) {
                const Color& col = horizontal ? colBuf[x - x0] : rowCol;
                if (col.a == 255) row[x] = packRGBA(col);
                else if (col.a != 0) put(x, y, col, 1.f);
            }
            for (int x = cb; x < x1; x++) {
                float cov = std::clamp(0.5f - sdRoundRect(x + 0.5f, py, rIn, cr), 0.f, 1.f);
                if (cov > 0.f) put(x, y, horizontal ? colBuf[x - x0] : rowCol, cov);
            }
            continue;
        }
        for (int x = x0; x < x1; x++) {
            float cov = std::clamp(0.5f - sdRoundRect(x + 0.5f, py, rIn, cr), 0.f, 1.f);
            if (cov <= 0.f) continue;
            const Color& col = horizontal ? colBuf[x - x0] : rowCol;
            if (cov >= 1.f && col.a == 255) row[x] = packRGBA(col);
            else put(x, y, col, cov);
        }
    }
}

void Canvas::fillRadialGradient(const Rect& rIn, Vec2 center, float radius, const Color& inner,
                                const Color& outer) {
    if (!fb_ || radius <= 0.f) return;
    Rect r = rIn.intersect(clip());
    if (r.empty()) return;
    int x0 = std::max(0, (int)std::floor(r.x)), x1 = std::min(w_, (int)std::ceil(r.r()));
    int y0 = std::max(0, (int)std::floor(r.y)), y1 = std::min(h_, (int)std::ceil(r.b()));
    float inv = 1.f / radius;
    for (int y = y0; y < y1; y++) {
        float dy = (y + 0.5f) - center.y;
        for (int x = x0; x < x1; x++) {
            float dx = (x + 0.5f) - center.x;
            float t = std::sqrt(dx * dx + dy * dy) * inv;
            if (t >= 1.f) continue;
            // smoothstep — плавное затухание без видимой границы круга.
            float s = t * t * (3.f - 2.f * t);
            Color col = Color::lerp(inner, outer, s);
            if (col.a == 0) continue;
            put(x, y, col, 1.f);
        }
    }
}

void Canvas::shadow(const Rect& rIn, const CornerRadii& cr, float blur, float dy, const Color& c) {
    if (!fb_ || c.a == 0 || blur <= 0.f) return;
    Rect shape = {rIn.x, rIn.y + dy, rIn.w, rIn.h};
    Rect bbox = {shape.x - blur * 2.f, shape.y - blur * 2.f, shape.w + blur * 4.f, shape.h + blur * 4.f};
    Rect r = bbox.intersect(clip());
    if (r.empty()) return;
    int x0 = std::max(0, (int)std::floor(r.x)), x1 = std::min(w_, (int)std::ceil(r.r()));
    int y0 = std::max(0, (int)std::floor(r.y)), y1 = std::min(h_, (int)std::ceil(r.b()));
    const float inv = 1.f / (blur * 1.35f);

    // Тень рисуется на КАЖДОЙ карточке, а раньше на каждый пиксель приходились
    // sdRoundRect (sqrt) и exp — именно это съедало ~100 мс на кадр.
    // Для прямоугольной формы гауссово размытие распадается на произведение
    // двух одномерных профилей (а на углах exp(a)*exp(b) = exp(a+b) в точности
    // совпадает с радиальной формулой). Поэтому считаем exp один раз на
    // столбец и один раз на строку.
    static float prof[2048];
    const int span = x1 - x0;
    if (span <= 0) return;
    if (span <= (int)(sizeof(prof) / sizeof(prof[0]))) {
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f;
            float d = 0.f;
            if (px < shape.x) d = shape.x - px;
            else if (px > shape.r()) d = px - shape.r();
            float t = d * inv;
            prof[x - x0] = std::exp(-t * t * 1.6f);
        }
    } else {
        return;
    }

    // Внутренняя часть (под самой карточкой) не видна — её не считаем.
    const float maxRad = std::clamp(std::max(std::max(cr.tl, cr.tr), std::max(cr.br, cr.bl)), 0.f,
                                    std::min(shape.w, shape.h) * 0.5f);
    const int ix0 = std::max(x0, (int)std::ceil(shape.x + maxRad));
    const int ix1 = std::min(x1, (int)std::floor(shape.r() - maxRad));
    const int iy0 = std::max(y0, (int)std::ceil(shape.y + maxRad));
    const int iy1 = std::min(y1, (int)std::floor(shape.b() - maxRad));

    for (int y = y0; y < y1; y++) {
        float py = y + 0.5f;
        float d = 0.f;
        if (py < shape.y) d = shape.y - py;
        else if (py > shape.b()) d = py - shape.b();
        float t = d * inv;
        const float ay = std::exp(-t * t * 1.6f);
        if (ay < 0.004f) continue;

        const bool midRow = (y >= iy0 && y < iy1);
        if (midRow) {
            // Средняя полоса: только левая и правая кромки.
            for (int x = x0; x < std::min(ix0, x1); x++) {
                float a = prof[x - x0] * ay;
                if (a >= 0.004f) put(x, y, c, a);
            }
            for (int x = std::max(ix1, x0); x < x1; x++) {
                float a = prof[x - x0] * ay;
                if (a >= 0.004f) put(x, y, c, a);
            }
        } else {
            for (int x = x0; x < x1; x++) {
                float a = prof[x - x0] * ay;
                if (a >= 0.004f) put(x, y, c, a);
            }
        }
    }
}

void Canvas::ripple(const Rect& shape, const CornerRadii& cr, Vec2 origin, float radius, const Color& c) {
    if (!fb_ || c.a == 0 || radius <= 0.f) return;
    Rect r = shape.intersect(clip());
    if (r.empty()) return;
    int x0 = std::max(0, (int)std::floor(r.x)), x1 = std::min(w_, (int)std::ceil(r.r()));
    int y0 = std::max(0, (int)std::floor(r.y)), y1 = std::min(h_, (int)std::ceil(r.b()));
    // Дополнительно ограничиваем bbox окружностью волны: вне radius+0.5 вклад нулевой.
    x0 = std::max(x0, (int)std::floor(origin.x - radius - 1.f));
    x1 = std::min(x1, (int)std::ceil(origin.x + radius + 1.f));
    y0 = std::max(y0, (int)std::floor(origin.y - radius - 1.f));
    y1 = std::min(y1, (int)std::ceil(origin.y + radius + 1.f));
    if (x0 >= x1 || y0 >= y1) return;

    // «Ядро» формы: здесь shapeCov == 1, SDF можно не считать.
    const float maxRad = std::clamp(std::max(std::max(cr.tl, cr.tr), std::max(cr.br, cr.bl)), 0.f,
                                    std::min(shape.w, shape.h) * 0.5f);
    const float ix0 = shape.x + maxRad + 0.5f, ix1 = shape.r() - maxRad - 0.5f;
    const float iy0 = shape.y + maxRad + 0.5f, iy1 = shape.b() - maxRad - 0.5f;
    // Квадраты радиусов для проверки «заведомо внутри/вне» волны без sqrt.
    const float inner = radius - 0.5f;
    const float inner2 = inner > 0.f ? inner * inner : -1.f;
    const float outer = radius + 0.5f, outer2 = outer * outer;

    for (int y = y0; y < y1; y++) {
        const float py = y + 0.5f;
        const bool yCore = (py >= iy0 && py <= iy1);
        for (int x = x0; x < x1; x++) {
            const float px = x + 0.5f;
            float shapeCov = 1.f;
            if (!(yCore && px >= ix0 && px <= ix1)) {
                float d = sdRoundRect(px, py, shape, cr);
                shapeCov = std::clamp(0.5f - d, 0.f, 1.f);
                if (shapeCov <= 0.f) continue;
            }
            const float dx = px - origin.x, dy = py - origin.y;
            const float d2 = dx * dx + dy * dy;
            float cov;
            if (d2 <= inner2) cov = 1.f;            // заведомо внутри волны
            else if (d2 >= outer2) continue;        // заведомо вне волны
            else cov = std::clamp(radius - std::sqrt(d2) + 0.5f, 0.f, 1.f);
            if (cov <= 0.f) continue;
            put(x, y, c, cov * shapeCov);
        }
    }
}

void Canvas::scrimV(const Rect& rIn, const Color& top, const Color& bottom) {
    fillLinearGradient(rIn, CornerRadii(0), top, bottom, false);
}

// ------------------------------------------------------------------ кэш миниатюр
namespace {
inline uint64_t hashImage(const Image& img) {
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 0x100000001b3ull;
    };
    mix((uint64_t)img.w);
    mix((uint64_t)img.h);
    mix((uint64_t)(uintptr_t)img.px.data());
    // Адрес может переиспользоваться после выгрузки из ImageCache, поэтому
    // подмешиваем ещё и содержимое (32 точки по всей картинке). Совпадение
    // адреса И этих точек практически исключено.
    const size_t n = img.px.size();
    const size_t step = std::max<size_t>(1, n / 32);
    for (size_t i = 0; i < n; i += step) mix(img.px[i]);
    return h;
}

// Уменьшение усреднением по площади: для каждого пикселя назначения берём
// прямоугольник источника и считаем среднее. Это и быстрее bilinear'а, и
// качественнее при уменьшении (нет «звона» и потерянных пикселей).
void scaleDownArea(const Image& src, int dw, int dh, std::vector<uint32_t>& out) {
    const int iw = src.w, ih = src.h;
    out.resize((size_t)dw * dh);
    const uint32_t* s = src.px.data();

    for (int dy = 0; dy < dh; dy++) {
        const int sy0 = (int)((int64_t)dy * ih / dh);
        int sy1 = (int)((int64_t)(dy + 1) * ih / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        const int rows = sy1 - sy0;
        uint32_t* o = out.data() + (size_t)dy * dw;
        for (int dx = 0; dx < dw; dx++) {
            const int sx0 = (int)((int64_t)dx * iw / dw);
            int sx1 = (int)((int64_t)(dx + 1) * iw / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t ar = 0, ag = 0, ab = 0, aa = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint32_t* row = s + (size_t)sy * iw;
                for (int sx = sx0; sx < sx1; sx++) {
                    const uint32_t p = row[sx];
                    ar += p & 0xFFu;
                    ag += (p >> 8) & 0xFFu;
                    ab += (p >> 16) & 0xFFu;
                    aa += (p >> 24) & 0xFFu;
                }
            }
            const uint32_t cnt = (uint32_t)((sx1 - sx0) * rows);
            const uint32_t half = cnt / 2;
            o[dx] = ((ar + half) / cnt) | (((ag + half) / cnt) << 8) | (((ab + half) / cnt) << 16) |
                    (((aa + half) / cnt) << 24);
        }
    }
}
} // namespace

const Canvas::ScaledImg* Canvas::scaledFor(const Image& img, int dw, int dh) {
    if (dw <= 0 || dh <= 0) return nullptr;
    const uint64_t key = hashImage(img) ^ ((uint64_t)(uint32_t)dw << 32) ^ (uint64_t)(uint32_t)dh;
    const uint64_t tick = ++scaleTick_;
    for (auto& e : scaled_) {
        if (e.key == key && e.w == dw && e.h == dh) {
            e.used = tick;
            return &e;
        }
    }

    // Не раздуваем память: держим не больше 16 МиБ и не больше 48 записей.
    const size_t need = (size_t)dw * dh * 4;
    auto evictOne = [&]() -> bool {
        size_t best = scaled_.size();
        uint64_t bestUsed = UINT64_MAX;
        for (size_t i = 0; i < scaled_.size(); i++)
            if (scaled_[i].used < bestUsed) {
                bestUsed = scaled_[i].used;
                best = i;
            }
        if (best == scaled_.size()) return false;
        scaledBytes_ -= scaled_[best].px.size() * 4;
        scaled_.erase(scaled_.begin() + (long)best);
        return true;
    };
    while (!scaled_.empty() && (scaledBytes_ + need > (size_t)16 * 1024 * 1024 || scaled_.size() >= 48))
        if (!evictOne()) break;

    ScaledImg e;
    e.w = dw;
    e.h = dh;
    e.key = key;
    e.used = tick;
    scaleDownArea(img, dw, dh, e.px);
    scaledBytes_ += e.px.size() * 4;
    scaled_.push_back(std::move(e));
    return &scaled_.back();
}

void Canvas::blit1to1(const uint32_t* sp, int sw, int sh, const Rect& dstIn, const CornerRadii& cr,
                      float opacity) {
    // Округляем положение до целого пикселя: картинка уже 1:1, субпиксельный
    // сдвиг только размывал бы её.
    Rect dst = {std::round(dstIn.x), std::round(dstIn.y), (float)sw, (float)sh};
    Rect r = dst.intersect(clip());
    if (r.empty()) return;
    int x0 = std::max(0, (int)dst.x), x1 = std::min(w_, (int)(dst.x + dst.w));
    int y0 = std::max(0, (int)dst.y), y1 = std::min(h_, (int)(dst.y + dst.h));
    if (x0 >= x1 || y0 >= y1) return;

    const bool rounded = (cr.tl + cr.tr + cr.br + cr.bl) > 0.01f;
    const float maxRad = std::clamp(std::max(std::max(cr.tl, cr.tr), std::max(cr.br, cr.bl)), 0.f,
                                    std::min(dst.w, dst.h) * 0.5f);
    int ca = x0, cb = x1, ya = y0, yb = y1;
    if (rounded) {
        coreXRange(dst, maxRad, ca, cb);
        coreYRange(dst, maxRad, ya, yb);
        ca = std::clamp(ca, x0, x1); cb = std::clamp(cb, x0, x1);
        if (cb < ca) cb = ca;
        ya = std::clamp(ya, y0, y1); yb = std::clamp(yb, y0, y1);
    }
    const bool opaqueDraw = (opacity >= 1.f);

    for (int y = y0; y < y1; y++) {
        const uint32_t* srow = sp + (size_t)(y - (int)dst.y) * sw;
        const int sx = (int)dst.x;
        uint32_t* row = fb_ + (size_t)y * stride_;
        const bool rowCore = (y >= ya && y < yb);
        if (rowCore && !rounded && opaqueDraw) {
            // Самая частая ситуация: непрозрачная картинка без скругления.
            // Копируем построчно (memcpy), отдельно обрабатывая пиксели с
            // прозрачностью — PNG с альфой встречаются (аватарки каналов).
            int x = x0;
            while (x < x1) {
                if ((srow[x - sx] >> 24) == 255) {
                    int run = x + 1;
                    while (run < x1 && (srow[run - sx] >> 24) == 255) ++run;
                    memcpy(row + x, srow + (x - sx), (size_t)(run - x) * 4);
                    x = run;
                } else {
                    const uint32_t p = srow[x - sx];
                    const uint32_t a = p >> 24;
                    if (a) put(x, y, Color((uint8_t)(p & 0xFF), (uint8_t)((p >> 8) & 0xFF),
                                          (uint8_t)((p >> 16) & 0xFF), (uint8_t)a),
                               opacity);
                    ++x;
                }
            }
            continue;
        }
        const float py = y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float cov = 1.f;
            if (rounded && !(rowCore && x >= ca && x < cb)) {
                const float d = sdRoundRect(x + 0.5f, py, dst, cr);
                cov = std::clamp(0.5f - d, 0.f, 1.f);
                if (cov <= 0.f) continue;
            }
            const uint32_t p = srow[x - sx];
            const uint32_t a = p >> 24;
            if (a == 255) {
                if (cov >= 1.f && opaqueDraw) {
                    row[x] = p;
                } else {
                    // Полупрозрачность рисуем как обычно, но без выборки:
                    // цвет уже в нужном масштабе.
                    put(x, y, Color((uint8_t)(p & 0xFF), (uint8_t)((p >> 8) & 0xFF),
                                    (uint8_t)((p >> 16) & 0xFF), 255),
                        cov * opacity);
                }
            } else if (a > 0) {
                put(x, y, Color((uint8_t)(p & 0xFF), (uint8_t)((p >> 8) & 0xFF),
                                (uint8_t)((p >> 16) & 0xFF), (uint8_t)a),
                    cov * opacity);
            }
        }
    }
}

void Canvas::image(const Image& img, const Rect& dst, float opacity, const CornerRadii& cr) {
    if (!fb_ || !img.valid() || opacity <= 0.f) return;

    // Размер совпадает с областью вывода (кадр видео 1280x720 на весь экран):
    // никакого масштабирования не нужно, копируем строки как есть. Раньше
    // такой кадр всё равно проходил через bilinear — это было самой дорогой
    // операцией экрана плеера.
    if (std::abs((float)img.w - dst.w) <= 0.5f && std::abs((float)img.h - dst.h) <= 0.5f) {
        blit1to1(img.px.data(), img.w, img.h, dst, cr, opacity);
        return;
    }

    // Уменьшение заметное (обложка 480x270 → плитка 416x234): берём готовую
    // уменьшенную копию из кэша. Для масштаба ~1:1 (кадр видео) кэш не нужен:
    // содержимое там меняется каждый кадр. Кадры видео к тому же крупные —
    // ограничение по площади страхует от кэширования меняющегося содержимого
    // (ключ кэша проверяет только 32 точки картинки).
    if (img.w * img.h <= 512 * 512 && img.w >= (int)(dst.w * 1.06f) && img.h >= (int)(dst.h * 1.06f)) {
        if (const ScaledImg* sc = scaledFor(img, (int)std::lround(dst.w), (int)std::lround(dst.h))) {
            blit1to1(sc->px.data(), sc->w, sc->h, dst, cr, opacity);
            return;
        }
    }

    Rect r = dst.intersect(clip());
    if (r.empty()) return;
    int x0 = std::max(0, (int)std::floor(r.x)), x1 = std::min(w_, (int)std::ceil(r.r()));
    int y0 = std::max(0, (int)std::floor(r.y)), y1 = std::min(h_, (int)std::ceil(r.b()));
    if (x0 >= x1 || y0 >= y1) return;

    const bool rounded = (cr.tl + cr.tr + cr.br + cr.bl) > 0.01f;
    const float maxRad = std::clamp(std::max(std::max(cr.tl, cr.tr), std::max(cr.br, cr.bl)), 0.f,
                                    std::min(dst.w, dst.h) * 0.5f);
    // «Ядро»: пересечение полос по X и Y, где sdRoundRect даёт d <= -0.5, т.е. cov == 1.
    int ca = x0, cb = x1, ya = y0, yb = y1;
    if (rounded) {
        coreXRange(dst, maxRad, ca, cb);
        coreYRange(dst, maxRad, ya, yb);
        ca = std::clamp(ca, x0, x1); cb = std::clamp(cb, x0, x1);
        if (cb < ca) cb = ca;
        ya = std::clamp(ya, y0, y1); yb = std::clamp(yb, y0, y1);
    }

    const int iw = img.w, ih = img.h;
    const uint32_t* src = img.px.data();
    // Шаг по u в формате 16.16: в горячем цикле нет ни одного деления на пиксель.
    const int64_t duFx = (int64_t)std::llround((double)(iw - 1) / (double)std::max(1.f, dst.w) * 65536.0);
    const int64_t maxUFx = (int64_t)(iw - 1) << 16;
    const int64_t maxVFx = (int64_t)(ih - 1) << 16;
    const float dvS = (float)(ih - 1) / std::max(1.f, dst.h);
    const bool fullOpacity = (opacity >= 1.f);

    // Канал может выйти за 0..255 из-за экстраполяции на дробной кромке — как в оригинале,
    // где Color::fromFloat обрезал значения.
    auto clamp255 = [](int v) -> int { return v < 0 ? 0 : (v > 255 ? 255 : v); };

    for (int y = y0; y < y1; y++) {
        const float py = y + 0.5f;
        // v зависит только от строки — считаем один раз.
        const int64_t vFx = (int64_t)std::llround((double)((py - dst.y) * dvS) * 65536.0);
        int v0, wv;
        if (vFx < 0) {
            // Первая дробная строка: оригинал экстраполировал по отрицательному весу.
            v0 = 0;
            wv = (vFx > -65536) ? (int)vFx : 0;
        } else if (vFx > maxVFx) {
            v0 = ih - 1;
            wv = 0;
        } else {
            v0 = (int)(vFx >> 16);
            wv = (int)(vFx & 0xFFFF); // вес нижней строки, 0..65535
        }
        const int wv1 = 65536 - wv;
        const int v1 = std::min(v0 + 1, ih - 1);
        const uint32_t* s0 = src + (size_t)v0 * iw;
        const uint32_t* s1 = src + (size_t)v1 * iw;
        const bool rowCore = (y >= ya && y < yb);
        uint32_t* row = fb_ + (size_t)y * stride_;

        int64_t uFx = (int64_t)std::llround((double)((x0 + 0.5f) - dst.x) * (double)duFx);

        for (int x = x0; x < x1; x++, uFx += duFx) {
            float cov = 1.f;
            if (rounded && !(rowCore && x >= ca && x < cb)) {
                // Вне ядра — честный SDF (только кромки и углы).
                float d = sdRoundRect(x + 0.5f, py, dst, cr);
                cov = std::clamp(0.5f - d, 0.f, 1.f);
                if (cov <= 0.f) continue;
            }
            int u0, wu;
            const int64_t uf = uFx;
            if (uf < 0) {
                // Левая дробная кромка: отрицательный вес, как в исходной реализации.
                u0 = 0;
                wu = (uf > -65536) ? (int)uf : 0;
            } else if (uf > maxUFx) {
                u0 = iw - 1;
                wu = 0;
            } else {
                u0 = (int)(uf >> 16);
                wu = (int)(uf & 0xFFFF); // вес правого столбца, 0..65535
            }
            const int wu1 = 65536 - wu;
            const int u1 = std::min(u0 + 1, iw - 1);

            // Целочисленный bilinear с весами 16.16: сначала горизонталь по двум строкам,
            // затем вертикаль. Каждый этап с округлением, поэтому ошибка <= 1/255.
            // 255 * 65536 = 16.7M, сумма двух таких — в int не переполняется.
            auto hstep = [&](const uint32_t* s, int sh) -> int {
                return ((int)((s[u0] >> sh) & 0xFF) * wu1 + (int)((s[u1] >> sh) & 0xFF) * wu + 32768) >> 16;
            };
            auto mix = [&](int sh) -> int {
                const int t0 = hstep(s0, sh), t1 = hstep(s1, sh);
                return (t0 * wv1 + t1 * wv + 32768) >> 16;
            };
            const int rr = clamp255(mix(0)), gg = clamp255(mix(8)), bb = clamp255(mix(16)),
                      aa = clamp255(mix(24));

            if (aa == 255 && cov >= 1.f && fullOpacity) {
                // Полностью непрозрачный пиксель — прямая запись в fb.
                row[x] = packRGBA((uint8_t)rr, (uint8_t)gg, (uint8_t)bb, 255);
            } else {
                put(x, y, Color((uint8_t)rr, (uint8_t)gg, (uint8_t)bb, (uint8_t)aa), cov * opacity);
            }
        }
    }
}

void Canvas::imageTint(const Image& img, const Rect& dst, const Color& tint, float opacity,
                       const CornerRadii& cr) {
    if (!fb_ || !img.valid()) return;
    Rect r = dst.intersect(clip());
    if (r.empty()) return;
    int x0 = std::max(0, (int)std::floor(r.x)), x1 = std::min(w_, (int)std::ceil(r.r()));
    int y0 = std::max(0, (int)std::floor(r.y)), y1 = std::min(h_, (int)std::ceil(r.b()));
    if (x0 >= x1 || y0 >= y1) return;

    // Флаг скругления выносим из цикла (раньше он проверялся на каждый пиксель).
    const bool rounded = (cr.tl + cr.tr + cr.br + cr.bl) > 0.01f;
    const float maxRad = std::clamp(std::max(std::max(cr.tl, cr.tr), std::max(cr.br, cr.bl)), 0.f,
                                    std::min(dst.w, dst.h) * 0.5f);
    int ca = x0, cb = x1, ya = y0, yb = y1;
    if (rounded) {
        coreXRange(dst, maxRad, ca, cb);
        coreYRange(dst, maxRad, ya, yb);
        ca = std::clamp(ca, x0, x1); cb = std::clamp(cb, x0, x1);
        if (cb < ca) cb = ca;
        ya = std::clamp(ya, y0, y1); yb = std::clamp(yb, y0, y1);
    }

    for (int y = y0; y < y1; y++) {
        float py = y + 0.5f;
        float v = (py - dst.y) / std::max(1.f, dst.h) * (img.h - 1);
        const bool rowCore = (y >= ya && y < yb);
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f;
            float cov = 1.f;
            if (rounded && !(rowCore && x >= ca && x < cb)) {
                float d = sdRoundRect(px, py, dst, cr);
                cov = std::clamp(0.5f - d, 0.f, 1.f);
                if (cov <= 0.f) continue;
            }
            float u = (px - dst.x) / std::max(1.f, dst.w) * (img.w - 1);
            int u0 = std::clamp((int)u, 0, img.w - 1), v0 = std::clamp((int)v, 0, img.h - 1);
            uint32_t p = img.px[(size_t)v0 * img.w + u0];
            float lum = (0.2126f * ((p >> 0) & 0xFF) + 0.7152f * ((p >> 8) & 0xFF) +
                         0.0722f * ((p >> 16) & 0xFF)) / 255.f;
            // Тонирование: иконка/картинка окрашивается в цвет tint с сохранением светимости.
            Color out = Color::lerp(tint, Color::fromFloat(1, 1, 1), lum * 0.35f);
            float a = ((p >> 24) & 0xFF) / 255.f;
            put(x, y, out.withAlpha8((uint8_t)(a * tint.a * opacity)), cov);
        }
    }
}

} // namespace tnx