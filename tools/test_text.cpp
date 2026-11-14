// tools/test_text.cpp — host-тесты текста/кэша глифов (RD-02, RD-03, RD-04).
//
// Запуск:  TUBENX_FONTS=resources/fonts ./test_text
// Собирается без консоли: нужны только core/font.cpp, core/canvas.cpp и
// tools/host_stubs.cpp. Возврат 0 — зелёно.
#include "core/canvas.hpp"
#include "core/font.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace tnx;

static int checks = 0, fails = 0;
static void check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        fails++;
        printf("   FAIL: %s\n", what);
    }
}

static std::string utf8(int cp) {
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

int main() {
    Fonts f;
    if (!f.loadDefaults()) {
        printf("нет шрифтов (TUBENX_FONTS) — SKIP\n");
        return 2;
    }

    // --- RD-02: лимит кэша глифов ------------------------------------------
    // 10 000 уникальных строк при жёстком лимите 256 КБ — память должна
    // остаться в бюджете, а вытеснения — произойти.
    const size_t limit = 256u << 10;
    f.setGlyphCacheLimit(limit);
    for (int i = 0; i < 10000; i++) {
        int cp = 0x100 + (i % 0xF00);
        f.measure(0, 16 + (i % 24), utf8(cp), 0.f);
    }
    check(f.glyphCacheBytes() <= limit, "RD-02: память кэша в пределах лимита");
    check(f.glyphCacheEntries() > 0, "RD-02: кэш не пуст");
    check(f.glyphCacheEvictions() > 0, "RD-02: вытеснения произошли");
    printf("   RD-02: bytes=%zu entries=%zu evictions=%zu\n", f.glyphCacheBytes(),
           f.glyphCacheEntries(), f.glyphCacheEvictions());

    // Текст не «пропадает» после вытеснения: кириллица снова измерима.
    float w = f.measure(0, 18, "Привет, мир!", 0.f);
    check(w > 10.f, "RD-02/RD-03: текст меряется после вытеснения");

    // --- RD-04: отсутствующие глифы ----------------------------------------
    size_t before = f.missingGlyphCount();
    float w2 = f.measure(0, 18, utf8(0x4E2D) /* 中 — нет в Google Sans */, 0.f);
    check(f.missingGlyphCount() > before, "RD-04: отсутствующий глиф зафиксирован");
    check(f.firstMissingGlyph() >= 0, "RD-04: firstMissingGlyph задан");
    check(w2 > 0.f, "RD-04: плейсхолдер имеет ненулевой advance");
    printf("   RD-04: firstMissing=U+%04X count=%zu\n", f.firstMissingGlyph(), f.missingGlyphCount());

    // Иконка отсутствует (Material Symbols, role 3) — advance есть, «тофу» нет.
    float wi = f.measure(3, 24, utf8(0xE000) /* private use — нет */, 0.f);
    check(wi > 0.f, "RD-04: отсутствующая иконка имеет advance");

    // --- RD-02: прогрев базовых кеглей -------------------------------------
    f.setGlyphCacheLimit(6u << 20);
    size_t e0 = f.glyphCacheEntries();
    f.preloadCommon(0, {14, 16, 22, 28});
    check(f.glyphCacheEntries() > e0, "RD-02: preloadCommon прогрел кэш");
    printf("   RD-02: после прогрева entries=%zu bytes=%zu\n", f.glyphCacheEntries(), f.glyphCacheBytes());

    printf("\n== %s: %d проверок, %d провалов ==\n", fails ? "ЕСТЬ ОШИБКИ" : "ЗЕЛЕНО", checks, fails);
    return fails ? 1 : 0;
}
