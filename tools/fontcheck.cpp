// tools/fontcheck.cpp — проверка покрытия глифов в TTF (для проверки сабсетов).
// Использование: fontcheck <font.ttf> 0x410 0xE88A ...
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: fontcheck font.ttf cp1 [cp2 ...]\n");
        return 1;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> data((size_t)n);
    if (fread(data.data(), 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);

    stbtt_fontinfo info;
    int off = stbtt_GetFontOffsetForIndex(data.data(), 0);
    if (!stbtt_InitFont(&info, data.data(), off)) { printf("bad font\n"); return 1; }
    int missing = 0;
    for (int i = 2; i < argc; i++) {
        int cp = (int)strtol(argv[i], nullptr, 0);
        int gi = stbtt_FindGlyphIndex(&info, cp);
        printf("U+%04X -> glyph %d %s\n", cp, gi, gi ? "ok" : "MISSING");
        if (!gi) missing++;
    }
    printf("%d missing of %d\n", missing, argc - 2);
    return missing ? 2 : 0;
}