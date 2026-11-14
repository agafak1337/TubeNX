// core/font.cpp
#include "core/font.hpp"

#include "platform.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

#include <cstdlib>
#include <cstdint>
#include <list>

namespace tnx {

namespace {
// Физический кегль глифа: кэш ключуется именно им, а не логическим px,
// поэтому на dock (масштаб 1,5) текст растрируется в 1,5 раза крупнее и
// остаётся резким, а не растянутым.
inline int physPx(float px, float scale) {
    int v = (int)std::lround(px * scale);
    return v < 1 ? 1 : v;
}
} // namespace

struct Fonts::Impl {
    stbtt_fontinfo info[4];
    std::vector<uint8_t> data[4];
    bool ok[4] = {false, false, false, false};

    // RD-02: кэш глифов с LRU-вытеснением по байтам. Список даёт стабильные
    // адреса элементов (ссылка, отданная наружу, остаётся валидной, пока глиф
    // не вытеснен), индекс — доступ за O(1) по ключу (font, физический px, cp).
    struct Entry {
        Glyph g;
        size_t bytes = 0;
    };
    mutable std::list<std::pair<Key, Entry>> lru;                 // front = свежий
    mutable std::unordered_map<Key, std::list<std::pair<Key, Entry>>::iterator, KeyHash> index;
    mutable size_t bytes = 0;
    mutable size_t limit = 6u << 20;                              // 6 МБ
    mutable size_t evictions = 0;
    mutable uint64_t hits = 0;
    mutable uint64_t misses = 0;

    // RD-04: диагностика отсутствующих глифов.
    mutable int firstMissing = -1;
    mutable size_t missingCount = 0;

    void clearCache() {
        lru.clear();
        index.clear();
        bytes = 0;
        evictions = 0;
        hits = misses = 0;
    }
};

void Fonts::ensureImpl() {
    if (!impl_) impl_ = new Impl();
}

bool Fonts::load(int role, const void* data, size_t size) {
    ensureImpl();
    if (role < 0 || role > 3) return false;
    const uint8_t* p = (const uint8_t*)data;
    impl_->data[role].assign(p, p + size);
    int off = stbtt_GetFontOffsetForIndex(impl_->data[role].data(), 0);
    if (off < 0) return false;
    if (!stbtt_InitFont(&impl_->info[role], impl_->data[role].data(), off)) return false;
    impl_->ok[role] = true;
    impl_->clearCache();
    return true;
}

static std::vector<uint8_t> readFile(const char* path) {
    std::vector<uint8_t> out;
    FILE* f = fopen(path, "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize((size_t)n);
        if (fread(out.data(), 1, (size_t)n, f) != (size_t)n) out.clear();
    }
    fclose(f);
    return out;
}

bool Fonts::loadDefaults() {
    bool any = false;
    // На хосте (offline-предпросмотр) путь можно переопределить через TUBENX_FONTS.
    std::string dir = "romfs:/fonts/";
    if (const char* env = getenv("TUBENX_FONTS")) dir = std::string(env) + "/";
    const struct { int role; const char* name; } files[] = {
        {0, "GoogleSans-Regular.ttf"},
        {1, "GoogleSans-Medium.ttf"},
        {2, "GoogleSans-Bold.ttf"},
        {3, "MaterialSymbols.ttf"},
    };
    for (auto& f : files) {
        auto d = readFile((dir + f.name).c_str());
        if (!d.empty()) any |= load(f.role, d.data(), d.size());
    }
    if (!any) return false;
    // Фолбэки, если какой-то вес не найден.
    for (int r = 0; r < 3; r++) {
        if (!impl_->ok[r]) {
            int src = r == 2 ? 1 : 0;
            for (int i = 0; i < 3; i++)
                if (impl_->ok[i]) src = i;
            impl_->info[r] = impl_->info[src];
            impl_->ok[r] = true;
        }
    }
    return true;
}

std::vector<int> utf8Decode(const std::string& s) {
    std::vector<int> out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        int cp = c;
        int n = 0;
        if (c >= 0xF0) { cp = c & 0x07; n = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; n = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 1; }
        for (int k = 0; k < n; k++) {
            if (i + 1 + k >= s.size()) break;
            cp = (cp << 6) | (s[i + 1 + k] & 0x3F);
        }
        out.push_back(cp);
        i += 1 + n;
    }
    return out;
}

float Fonts::ascent(int font, float px) const {
    if (!impl_ || !impl_->ok[font]) return px * 0.8f;
    float s = stbtt_ScaleForPixelHeight(&impl_->info[font], px);
    int a, d, g;
    stbtt_GetFontVMetrics(&impl_->info[font], &a, &d, &g);
    return a * s;
}

float Fonts::descent(int font, float px) const {
    if (!impl_ || !impl_->ok[font]) return px * 0.2f;
    float s = stbtt_ScaleForPixelHeight(&impl_->info[font], px);
    int a, d, g;
    stbtt_GetFontVMetrics(&impl_->info[font], &a, &d, &g);
    return -d * s;
}

float Fonts::lineHeight(int font, float px) const {
    return ascent(font, px) + descent(font, px);
}

const Fonts::Glyph& Fonts::glyph(int font, int px, int cp) const {
    Key k{font, px, cp};
    // Шрифты могли не загрузиться (нет romfs/файла) — не падаем, рисуем пусто.
    if (!impl_) {
        static const Glyph kEmpty;
        return kEmpty;
    }
    auto it = impl_->index.find(k);
    if (it != impl_->index.end()) {
        // RD-02: попадание — помечаем запись свежей (LRU).
        impl_->hits++;
        impl_->lru.splice(impl_->lru.begin(), impl_->lru, it->second);
        return it->second->second.g;
    }
    impl_->misses++;

    Glyph g;
    bool missing = false;
    if (impl_->ok[font] && cp >= 0) {
        float s = stbtt_ScaleForPixelHeight(&impl_->info[font], (float)px);
        int gi = stbtt_FindGlyphIndex(&impl_->info[font], cp);
        if (gi == 0 && cp != 32) {
            missing = true;
            // RD-04: текст — подставляем '?'; иконки — пропуск без «тофу».
            if (font != 3) gi = stbtt_FindGlyphIndex(&impl_->info[font], '?');
        }
        if (gi == 0) {
            // Плейсхолдер: только advance, пустой битмап (без .notdef-квадрата).
            g.advance = cp == 32 ? px * 0.26f : px * 0.5f;
        } else {
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&impl_->info[font], gi, &adv, &lsb);
            g.advance = adv * s;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(&impl_->info[font], gi, s, s, &x0, &y0, &x1, &y1);
            g.w = std::max(0, x1 - x0);
            g.h = std::max(0, y1 - y0);
            g.xoff = x0;
            g.yoff = y0;
            if (g.w > 0 && g.h > 0) {
                g.bmp.resize((size_t)g.w * g.h);
                stbtt_MakeGlyphBitmap(&impl_->info[font], g.bmp.data(), g.w, g.h, g.w, s, s, gi);
            }
        }
    } else {
        g.advance = cp == 32 ? px * 0.26f : px * 0.5f;
    }

    if (missing) {
        impl_->missingCount++;
        if (impl_->firstMissing < 0) {
            impl_->firstMissing = cp;
            platform::logWarn("font: нет глифа U+%04X (font %d) — плейсхолдер", cp, font);
        }
    }

    Impl::Entry e{std::move(g), 0};
    e.bytes = sizeof(Impl::Entry) + e.g.bmp.capacity();
    impl_->lru.emplace_front(k, std::move(e));
    impl_->index[k] = impl_->lru.begin();
    impl_->bytes += impl_->lru.front().second.bytes;

    // RD-02: вытесняем самые старые, пока не уложимся в лимит. Только что
    // добавленный глиф (front) не вытесняем — даже если он один больше лимита.
    while (impl_->bytes > impl_->limit && impl_->lru.size() > 1) {
        auto& last = impl_->lru.back();
        impl_->index.erase(last.first);
        impl_->bytes -= last.second.bytes;
        impl_->lru.pop_back();
        impl_->evictions++;
    }
    return impl_->lru.front().second.g;
}

float Fonts::kerning(int font, int px, int a, int b) const {
    if (!impl_ || !impl_->ok[font]) return 0.f;
    float s = stbtt_ScaleForPixelHeight(&impl_->info[font], (float)px);
    int gi1 = stbtt_FindGlyphIndex(&impl_->info[font], a);
    int gi2 = stbtt_FindGlyphIndex(&impl_->info[font], b);
    return stbtt_GetGlyphKernAdvance(&impl_->info[font], gi1, gi2) * s;
}

float Fonts::measure(int font, float px, const std::string& text) const {
    return measure(font, px, text, 0.f);
}

float Fonts::measure(int font, float px, const std::string& text, float letterSpacing) const {
    if (!impl_ || text.empty()) return 0.f;
    // Меряем в логических px: глифы кэшируются в физическом кегле, поэтому
    // их метрики делим на масштаб канваса. При scale = 1 — как раньше.
    const float sc = (canvas_ && canvas_->scale() > 0.f) ? canvas_->scale() : 1.f;
    const float inv = 1.f / sc;
    int pi = physPx(px, sc);
    auto cps = utf8Decode(text);
    float w = 0.f;
    for (size_t i = 0; i < cps.size(); i++) {
        const Glyph& g = glyph(font, pi, cps[i]);
        w += g.advance * inv + letterSpacing;
        if (i + 1 < cps.size()) w += kerning(font, pi, cps[i], cps[i + 1]) * inv;
    }
    return w;
}

void Fonts::draw(int font, float px, float x, float y, const std::string& text, const Color& c,
                 float letterSpacing) const {
    if (!canvas_ || text.empty() || c.a == 0) return;
    // Все входные координаты — логические; внутри переводим в физические
    // пиксели буфера (dock 1,5×) и рисуем уже готовые глифы физического кегля.
    const float sc = (canvas_->scale() > 0.f) ? canvas_->scale() : 1.f;
    const int pi = physPx(px, sc);
    const float baseline = y * sc + ascent(font, (float)pi);
    const float lsPx = letterSpacing * sc;
    auto cps = utf8Decode(text);
    float penX = x * sc;
    for (size_t i = 0; i < cps.size(); i++) {
        const Glyph& g = glyph(font, pi, cps[i]);
        if (g.w > 0 && g.h > 0) {
            int gx = (int)std::floor(penX + g.xoff);
            int gy = (int)std::floor(baseline + g.yoff);
            for (int row = 0; row < g.h; row++) {
                for (int col = 0; col < g.w; col++) {
                    uint8_t a = g.bmp[(size_t)row * g.w + col];
                    if (!a) continue;
                    canvas_->blendPixelPx(gx + col, gy + row, c, a / 255.f);
                }
            }
        }
        penX += g.advance + lsPx;
        if (i + 1 < cps.size()) penX += kerning(font, pi, cps[i], cps[i + 1]);
    }
}

void Fonts::drawCentered(int font, float px, const Rect& box, const std::string& text, const Color& c,
                         float letterSpacing) const {
    float w = measure(font, px, text, letterSpacing);
    float h = lineHeight(font, px);
    draw(font, px, box.x + (box.w - w) * 0.5f, box.y + (box.h - h) * 0.5f, text, c, letterSpacing);
}

float Fonts::drawEllipsized(int font, float px, float x, float y, float maxWidth, const std::string& text,
                            const Color& c, float letterSpacing) const {
    if (measure(font, px, text, letterSpacing) <= maxWidth) {
        draw(font, px, x, y, text, c, letterSpacing);
        return measure(font, px, text, letterSpacing);
    }
    const std::string dots = "...";
    float dw = measure(font, px, dots, letterSpacing);
    auto cps = utf8Decode(text);
    std::string acc;
    float w = 0.f;
    // Ширины — логические; глиф кэширован в физическом кегле (см. measure).
    const float sc = (canvas_ && canvas_->scale() > 0.f) ? canvas_->scale() : 1.f;
    for (size_t i = 0; i < cps.size(); i++) {
        int cp = cps[i];
        char buf[5] = {0};
        if (cp < 0x80) { buf[0] = (char)cp; }
        else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            buf[0] = (char)(0xE0 | (cp >> 12));
            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (cp & 0x3F));
        } else {
            buf[0] = (char)(0xF0 | (cp >> 18));
            buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[3] = (char)(0x80 | (cp & 0x3F));
        }
        float gw = glyph(font, physPx(px, sc), cp).advance / sc + letterSpacing;
        if (w + gw + dw > maxWidth) break;
        acc += buf;
        w += gw;
    }
    draw(font, px, x, y, acc + dots, c, letterSpacing);
    return w + dw;
}

float Fonts::measureWrapped(int font, float px, float maxWidth, const std::string& text, int maxLines,
                            float letterSpacing) const {
    auto lines = std::vector<std::string>{};
    std::string cur;
    std::string word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        float w = measure(font, px, cur.empty() ? word : cur + " " + word, letterSpacing);
        if (w <= maxWidth || cur.empty()) {
            cur = cur.empty() ? word : cur + " " + word;
        } else {
            lines.push_back(cur);
            cur = word;
        }
        word.clear();
    };
    auto cps = utf8Decode(text);
    for (int cp : cps) {
        if (cp == '\n') {
            flushWord();
            lines.push_back(cur);
            cur.clear();
            continue;
        }
        if (cp == ' ') {
            flushWord();
            continue;
        }
        char buf[5] = {0};
        if (cp < 0x80) buf[0] = (char)cp;
        else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); }
        else {
            buf[0] = (char)(0xE0 | (cp >> 12));
            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (cp & 0x3F));
        }
        word += buf;
    }
    flushWord();
    if (!cur.empty()) lines.push_back(cur);
    if ((int)lines.size() > maxLines) lines.resize(maxLines);
    return (float)lines.size() * lineHeight(font, px);
}

float Fonts::drawWrapped(int font, float px, const Rect& box, const std::string& text, const Color& c,
                         int maxLines, float letterSpacing, bool ellipsize) const {
    std::string cur, word;
    std::vector<std::string> lines;
    auto flushWord = [&]() {
        if (word.empty()) return;
        float w = measure(font, px, cur.empty() ? word : cur + " " + word, letterSpacing);
        if (w <= box.w || cur.empty()) cur = cur.empty() ? word : cur + " " + word;
        else { lines.push_back(cur); cur = word; }
        word.clear();
    };
    for (int cp : utf8Decode(text)) {
        if (cp == '\n') { flushWord(); lines.push_back(cur); cur.clear(); continue; }
        if (cp == ' ') { flushWord(); continue; }
        char buf[5] = {0};
        if (cp < 0x80) buf[0] = (char)cp;
        else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); }
        else {
            buf[0] = (char)(0xE0 | (cp >> 12));
            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (cp & 0x3F));
        }
        word += buf;
    }
    flushWord();
    if (!cur.empty()) lines.push_back(cur);
    bool truncated = (int)lines.size() > maxLines;
    if (truncated) lines.resize(maxLines);

    float lh = lineHeight(font, px);
    float y = box.y;
    for (size_t i = 0; i < lines.size(); i++) {
        bool last = (i + 1 == lines.size());
        if (last && truncated && ellipsize)
            drawEllipsized(font, px, box.x, y, box.w, lines[i], c, letterSpacing);
        else
            draw(font, px, box.x, y, lines[i], c, letterSpacing);
        y += lh;
    }
    return (float)lines.size() * lh;
}

float Fonts::textWidthForStyle(const md3::TextStyle& s, const std::string& text) const {
    return measure((int)s.font, s.size, text, s.letterSpacing);
}

// ------------------------------------------------------------- RD-02 / RD-04
void Fonts::setGlyphCacheLimit(size_t bytes) {
    ensureImpl();
    impl_->limit = bytes ? bytes : 1;
}

size_t Fonts::glyphCacheBytes() const { return impl_ ? impl_->bytes : 0; }
size_t Fonts::glyphCacheEntries() const { return impl_ ? impl_->lru.size() : 0; }
size_t Fonts::glyphCacheEvictions() const { return impl_ ? impl_->evictions : 0; }

void Fonts::preloadCommon(int font, const std::vector<int>& pxSizes) {
    ensureImpl();
    std::vector<int> cps;
    cps.reserve(160);
    for (int c = 32; c < 127; c++) cps.push_back(c);          // ASCII
    for (int c = 0x410; c <= 0x44F; c++) cps.push_back(c);    // А..я
    cps.push_back(0x401);                                     // Ё
    cps.push_back(0x451);                                     // ё
    const float sc = (canvas_ && canvas_->scale() > 0.f) ? canvas_->scale() : 1.f;
    for (int px : pxSizes) {
        int pi = physPx((float)px, sc);
        for (int cp : cps) glyph(font, pi, cp);
    }
}

int Fonts::firstMissingGlyph() const { return impl_ ? impl_->firstMissing : -1; }
size_t Fonts::missingGlyphCount() const { return impl_ ? impl_->missingCount : 0; }

} // namespace tnx