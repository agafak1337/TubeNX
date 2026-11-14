// core/canvas.hpp — программный рендерер (RGBA8888) с антиалиасингом.
// Никаких зависимостей от GPU/SDL: всё рисуется в буфер libnx framebuffer.
#pragma once

#include "core/types.hpp"
#include <vector>

namespace tnx {

// Простая RGBA-картинка (используется для обложек и иконок).
struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px; // 0xAABBGGRR в памяти (little-endian RGBA)
    // RD-05: стабильный идентификатор кадра (например, из ImageCache — URL).
    // Если задан, кэш масштабированных копий ключуется им, а не адресом
    // буфера: адрес может переиспользоваться после выгрузки из кэша.
    uint64_t id = 0;
    bool valid() const { return w > 0 && h > 0 && !px.empty(); }
};

struct CornerRadii {
    float tl = 0, tr = 0, br = 0, bl = 0;
    CornerRadii() = default;
    explicit CornerRadii(float r) : tl(r), tr(r), br(r), bl(r) {}
    CornerRadii(float a, float b, float c, float d) : tl(a), tr(b), br(c), bl(d) {}
    CornerRadii scaled(float s) const { return {tl * s, tr * s, br * s, bl * s}; }
};

// Безопасные поля в логических px. Handheld — нули, dock — 64/36
// (docs/impl-tz.md §5.1, §5.9).
struct Insets {
    float l = 0.f, t = 0.f, r = 0.f, b = 0.f;
};

class Canvas {
public:
    Canvas();
    ~Canvas();

    // Привязка к буферу libnx (stride в пикселях).
    void attach(uint32_t* fb, int width, int height, int stridePx);
    void fill(const Color& c);

    int width() const { return w_; }
    int height() const { return h_; }

    // --- dock / масштаб логического канваса ---
    // Все примитивы ниже принимают координаты в ЛОГИЧЕСКОМ канвасе 1280×720,
    // а рисуются в физических пикселях буфера: внутри координаты умножаются
    // на scale(). При scale = 1 (handheld, host-предпросмотр) поведение
    // прежнее — существующие вызовы работают без правок.
    void setScale(float s);
    float scale() const { return scale_; }
    // Логический размер канваса (1280×720 при любом масштабе).
    float logicalWidth() const { return scale_ > 0.f ? (float)w_ / scale_ : (float)w_; }
    float logicalHeight() const { return scale_ > 0.f ? (float)h_ / scale_ : (float)h_; }

    void setSafeInsets(const Insets& in) { safe_ = in; }
    const Insets& safeInsets() const { return safe_; }

    // RD-05: пределы кэша масштабированных обложек + метрики (для теста/DIAG).
    static constexpr size_t kScaledCacheLimitBytes = 16u << 20;
    static constexpr size_t kScaledCacheLimitEntries = 48;
    size_t scaledCacheEntries() const { return scaled_.size(); }
    size_t scaledCacheBytes() const { return scaledBytes_; }
    size_t scaledCacheComputes() const { return scaledComputes_; }
    void clearScaledCache() { scaled_.clear(); scaledBytes_ = 0; }

    void pushClip(const Rect& r);
    void popClip();
    // Текущий клип в логических координатах (экраны читают его для раскладки).
    Rect clip() const { return clipStack_.empty() ? Rect(0, 0, logicalWidth(), logicalHeight()) : clipStack_.back(); }

    // Примитивы
    void fillRect(const Rect& r, const Color& c);
    void fillRoundRect(const Rect& r, const CornerRadii& cr, const Color& c);
    void fillRoundRect(const Rect& r, float radius, const Color& c) { fillRoundRect(r, CornerRadii(radius), c); }
    void strokeRoundRect(const Rect& r, const CornerRadii& cr, float width, const Color& c);
    void fillCircle(float cx, float cy, float radius, const Color& c);
    void fillLinearGradient(const Rect& r, const CornerRadii& cr, const Color& from, const Color& to,
                            bool horizontal = false);
    void fillLinearGradient3(const Rect& r, const CornerRadii& cr, const Color& a, const Color& b,
                             const Color& c, bool horizontal);
    // Мягкое радиальное свечение (для M3 Expressive «aurora»-пятен).
    void fillRadialGradient(const Rect& r, Vec2 center, float radius, const Color& inner, const Color& outer);
    // Тень в стиле M3 (без резких краёв).
    void shadow(const Rect& r, const CornerRadii& cr, float blur, float dy, const Color& c);
    // Ripple: круглая волна, обрезанная формой виджета.
    void ripple(const Rect& shape, const CornerRadii& cr, Vec2 origin, float radius, const Color& c);
    // Полупрозрачное затемнение (scrim) с плавным градиентом.
    void scrimV(const Rect& r, const Color& top, const Color& bottom);
    void image(const Image& img, const Rect& dst, float opacity = 1.f, const CornerRadii& cr = CornerRadii(0));
    void imageTint(const Image& img, const Rect& dst, const Color& tint, float opacity = 1.f,
                   const CornerRadii& cr = CornerRadii(0));

    // Пиксель (для отладки/эффектов), логические координаты.
    void blendPixel(int x, int y, const Color& c, float coverage = 1.f);
    // То же в физических пикселях: так текст рисуется уже растрированными
    // в физическом кегле глифами (без повторного умножения на масштаб).
    void blendPixelPx(int x, int y, const Color& c, float coverage = 1.f);

    // Текст
    struct GlyphKey {
        int font = 0, size = 0, glyph = 0;
        bool operator==(const GlyphKey& o) const { return font == o.font && size == o.size && glyph == o.glyph; }
    };

    // SDF-функция со скруглением — нужна и другим модулям (например, для масок).
    static float sdRoundRect(float px, float py, const Rect& r, const CornerRadii& cr);

private:
    uint32_t* fb_ = nullptr;
    int w_ = 0, h_ = 0, stride_ = 0;
    float scale_ = 1.f;      // логические → физические пиксели
    Insets safe_;            // безопасные поля (логические px)
    std::vector<Rect> clipStack_;    // логические
    std::vector<Rect> clipStackPx_;  // физические (для растрирования)

    inline void put(int x, int y, const Color& c, float cov);

    // Перевод логических координат в физические.
    Rect toPx(const Rect& r) const;
    CornerRadii toPx(const CornerRadii& cr) const;
    Vec2 toPx(const Vec2& p) const;
    Rect clipPx() const { return clipStackPx_.empty() ? Rect(0, 0, (float)w_, (float)h_) : clipStackPx_.back(); }

    // Кэш уменьшенных копий картинок. Обложки приходят 480x270 и рисуются в
    // плитках ~416x234; масштабирование bilinear'ом на каждый кадр стоило
    // больше половины времени отрисовки (6 картинок * ~1.3 мс на хосте).
    // Сжимаем один раз (усреднение по площади — качество лучше bilinear при
    // уменьшении) и дальше копируем построчно.
    struct ScaledImg {
        int w = 0, h = 0;
        std::vector<uint32_t> px;
        uint64_t key = 0;
        uint64_t used = 0;
    };
    std::vector<ScaledImg> scaled_;
    uint64_t scaleTick_ = 0;
    size_t scaledBytes_ = 0;
    size_t scaledComputes_ = 0;   // RD-05: сколько раз реально масштабировали
    // Таблица «столбец назначения → столбец источника» для blitNearest.
    // Растёт только при увеличении ширины, на кадр аллокаций не даёт.
    std::vector<int> colIdx_;
    const ScaledImg* scaledFor(const Image& img, int dw, int dh);
    // Копирование картинки без масштабирования (1:1), с учётом скругления,
    // прозрачности и клипа. Именно этот путь работает для кадра видео, когда
    // его размер совпадает с областью вывода.
    void blit1to1(const uint32_t* sp, int sw, int sh, const Rect& dst, const CornerRadii& cr, float opacity);
    // Быстрое увеличение без интерполяции (крупные картинки: кадр видео
    // 1280×720 → 1920×1080, фон сплэша). Билинейная выборка на 2 Мп стоит
    // десятки миллисекунд, а на видео и гладких градиентах разница не видна.
    void blitNearest(const uint32_t* sp, int sw, int sh, const Rect& dst, const CornerRadii& cr, float opacity);
};

} // namespace tnx