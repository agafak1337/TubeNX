// core/font.hpp — растеризация текста (stb_truetype) с кэшем глифов.
#pragma once

#include "core/canvas.hpp"
#include "md3/theme.hpp"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace tnx {

class Fonts {
public:
    bool loadDefaults();                                     // из romfs (Roboto)
    bool load(int role, const void* data, size_t size);       // 0=Regular 1=Medium 2=Bold
    void setCanvas(Canvas* c) { canvas_ = c; }

    float ascent(int font, float px) const;
    float descent(int font, float px) const;
    float lineHeight(int font, float px) const;
    float measure(int font, float px, const std::string& text) const;
    float measure(int font, float px, const std::string& text, float letterSpacing) const;

    // Рисует строку; y — верх строки (top of line box), baseline = y + ascent.
    void draw(int font, float px, float x, float y, const std::string& text, const Color& c,
              float letterSpacing = 0.f) const;
    void drawCentered(int font, float px, const Rect& box, const std::string& text, const Color& c,
                      float letterSpacing = 0.f) const;

    // Однострочный текст с обрезкой и многоточием. Возвращает реально занятую ширину.
    float drawEllipsized(int font, float px, float x, float y, float maxWidth, const std::string& text,
                         const Color& c, float letterSpacing = 0.f) const;
    // Многострочный текст с переносом по словам. Возвращает высоту.
    float drawWrapped(int font, float px, const Rect& box, const std::string& text, const Color& c,
                      int maxLines = 100, float letterSpacing = 0.f, bool ellipsize = true) const;
    float measureWrapped(int font, float px, float maxWidth, const std::string& text, int maxLines = 100,
                         float letterSpacing = 0.f) const;

    // Простое SDF-подобное сглаживание текста поверх цветного фона (для видео-оверлеев).
    float textWidthForStyle(const md3::TextStyle& s, const std::string& text) const;

    // RD-02: кэш глифов ограничен по памяти (LRU). По умолчанию ~6 МБ.
    // При вытеснении память держится в бюджете, текст не «исчезает».
    void setGlyphCacheLimit(size_t bytes);
    size_t glyphCacheBytes() const;
    size_t glyphCacheEntries() const;
    size_t glyphCacheEvictions() const;
    // RD-02: прогрев ASCII + кириллицы для базовых кеглей (логические px).
    void preloadCommon(int font, const std::vector<int>& pxSizes);

    // RD-04: диагностика отсутствующих глифов (первый codepoint и счётчик).
    int firstMissingGlyph() const;
    size_t missingGlyphCount() const;

private:
    struct Glyph {
        int w = 0, h = 0, xoff = 0, yoff = 0;
        float advance = 0.f;
        std::vector<uint8_t> bmp;
    };
    struct Key {
        int font, px, cp;
        bool operator==(const Key& o) const { return font == o.font && px == o.px && cp == o.cp; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return ((size_t)k.font * 73856093u) ^ ((size_t)k.px * 19349663u) ^ ((size_t)k.cp * 83492791u);
        }
    };

    const Glyph& glyph(int font, int px, int cp) const;
    float kerning(int font, int px, int a, int b) const;

    struct Impl;
    Impl* impl_ = nullptr;
    void ensureImpl();
    Canvas* canvas_ = nullptr;
};

// UTF-8 → codepoints
std::vector<int> utf8Decode(const std::string& s);

} // namespace tnx