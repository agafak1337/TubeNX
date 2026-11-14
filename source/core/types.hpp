// TubeNX — Material 3 YouTube client for Nintendo Switch
// core/types.hpp — базовые типы (цвет, точка, прямоугольник, анимации)
#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace tnx {

struct Vec2 {
    float x = 0.f, y = 0.f;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
};

struct Rect {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    Rect() = default;
    Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    float r() const { return x + w; }
    float b() const { return y + h; }
    Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < r() && p.y < b(); }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect intersect(const Rect& o) const {
        float nx = std::max(x, o.x), ny = std::max(y, o.y);
        float nr = std::min(r(), o.r()), nb = std::min(b(), o.b());
        return {nx, ny, std::max(0.f, nr - nx), std::max(0.f, nb - ny)};
    }
    bool empty() const { return w <= 0.f || h <= 0.f; }
};

// Цвет в sRGB, 8 бит на канал, non-premultiplied.
struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    constexpr Color() = default;
    constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255)
        : r(r_), g(g_), b(b_), a(a_) {}

    static constexpr Color rgb(uint32_t hex) {
        return Color((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, 255);
    }
    static constexpr Color argb(uint32_t hex) {
        return Color((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, (hex >> 24) & 0xFF);
    }
    // 0..1
    static Color fromFloat(float r_, float g_, float b_, float a_ = 1.f) {
        auto c = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
        return Color(c(r_), c(g_), c(b_), c(a_));
    }

    Color withAlpha(float a_) const { return {r, g, b, (uint8_t)std::lround(std::clamp(a_, 0.f, 1.f) * 255.f)}; }
    Color withAlpha8(uint8_t a_) const { return {r, g, b, a_}; }
    // Умножение альфы на коэффициент (для наложения поверх).
    Color fade(float k) const { return {r, g, b, (uint8_t)std::lround(a * std::clamp(k, 0.f, 1.f))}; }

    static Color lerp(const Color& a, const Color& b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        auto m = [t](uint8_t x, uint8_t y) { return (uint8_t)std::lround(x + (y - x) * t); };
        return Color(m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a));
    }
    // Прозрачность поверх цвета фона — M3 использует "surface tint".
    Color over(const Color& top) const { return lerp(*this, top, top.a / 255.f); }
};

inline float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

// Плавная анимация с пружиной (M3 motion: emphasized decelerate).
struct Spring {
    float value = 0.f;
    float velocity = 0.f;
    float target = 0.f;
    float stiffness = 220.f;
    float damping = 28.f;

    void setImmediate(float v) { value = v; target = v; velocity = 0.f; }

    void update(float dt) {
        // Полунеявный Эйлер, устойчив при 60 fps.
        dt = std::min(dt, 0.05f);
        const int steps = 3;
        float h = dt / steps;
        for (int i = 0; i < steps; i++) {
            float a = (target - value) * stiffness - velocity * damping;
            velocity += a * h;
            value += velocity * h;
        }
        if (std::fabs(target - value) < 0.0005f && std::fabs(velocity) < 0.0005f) {
            value = target;
            velocity = 0.f;
        }
    }
};

// Линейная интерполяция с плавным замедлением, для "одноразовых" анимаций.
struct Tween {
    float t = 0.f;      // 0..1
    float speed = 6.f;  // 1/сек
    bool running = false;

    void restart() { t = 0.f; running = true; }
    void update(float dt) {
        if (!running) return;
        t += dt * speed;
        if (t >= 1.f) { t = 1.f; running = false; }
    }
    float eased() const { return 1.f - (1.f - t) * (1.f - t) * (1.f - t); }
};

} // namespace tnx