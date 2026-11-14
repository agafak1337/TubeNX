// tools/test_canvas.cpp — host-тесты канваса: кэш масштаба (RD-05), клипы
// (RD-06), альфа-блендинг (RD-07).
//
// Собирается без консоли: core/canvas.cpp + tools/host_stubs.cpp. Возврат 0 — зелёно.
#include "core/canvas.hpp"

#include <cstdio>
#include <cstdint>
#include <random>
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

static Image makeImage(int w, int h, uint64_t id, uint8_t seed) {
    Image im;
    im.w = w;
    im.h = h;
    im.id = id;
    im.px.resize((size_t)w * h);
    for (size_t i = 0; i < im.px.size(); i++) {
        uint32_t v = (uint32_t)((seed + i) & 0xFF);
        im.px[i] = v | (v << 8) | (v << 16) | 0xFF000000u;
    }
    return im;
}

int main() {
    const int W = 400, H = 300;

    // --- RD-07: альфа-блендинг ---------------------------------------------
    {
        std::vector<uint32_t> buf((size_t)W * H, 0xFF000000u); // чёрный непрозрачный
        Canvas c;
        c.attach(buf.data(), W, H, W);
        c.blendPixel(10, 10, Color(255, 255, 255, 128));       // 50% белый
        uint32_t p = buf[(size_t)10 * W + 10];
        int r = p & 0xFF, a = (p >> 24) & 0xFF;
        check(r >= 126 && r <= 129, "RD-07: 50% белый на чёрном = ~128");
        check(a == 255, "RD-07: непрозрачный фон остаётся непрозрачным");

        // 50% белый на красном.
        std::vector<uint32_t> buf2((size_t)W * H, 0xFF0000FFu); // R=255
        Canvas c2;
        c2.attach(buf2.data(), W, H, W);
        c2.blendPixel(5, 5, Color(255, 255, 255, 128));
        uint32_t q = buf2[(size_t)5 * W + 5];
        check((q & 0xFF) == 255, "RD-07: R остаётся 255 (белый на красном)");
        check(((q >> 8) & 0xFF) >= 126 && ((q >> 8) & 0xFF) <= 129, "RD-07: G ~= 128");
        check(((q >> 16) & 0xFF) >= 126 && ((q >> 16) & 0xFF) <= 129, "RD-07: B ~= 128");

        // Быстрый путь spans (fillRoundRect) == путь put для того же пикселя:
        // заливаем полупрозрачным прямоугольником и сравниваем с одиночным put.
        std::vector<uint32_t> buf3((size_t)W * H, 0xFF102030u);
        Canvas c3;
        c3.attach(buf3.data(), W, H, W);
        Color layer = Color(200, 100, 50, 96);
        c3.fillRoundRect(Rect(0, 0, 100, 50), CornerRadii(0), layer);
        std::vector<uint32_t> buf4((size_t)W * H, 0xFF102030u);
        Canvas c4;
        c4.attach(buf4.data(), W, H, W);
        c4.blendPixel(20, 20, layer);
        check(buf3[(size_t)20 * W + 20] == buf4[(size_t)20 * W + 20],
              "RD-07: spans и put дают одинаковый результат");
    }

    // --- RD-06: корректность клипов (property) -----------------------------
    {
        const uint32_t SENT = 0xFF112233u;
        std::mt19937 rng(1234);
        auto rnd = [&](int lo, int hi) { return lo + (int)(rng() % (unsigned)(hi - lo + 1)); };
        int violations = 0;
        for (int t = 0; t < 60; t++) {
            std::vector<uint32_t> buf((size_t)W * H, SENT);
            Canvas c;
            c.attach(buf.data(), W, H, W);
            int cx = rnd(0, W - 40), cy = rnd(0, H - 40);
            int cw = rnd(20, W - cx), ch = rnd(20, H - cy);
            Rect clip((float)cx, (float)cy, (float)cw, (float)ch);
            c.pushClip(clip);
            // Вложенный клип внутри первого.
            Rect inner((float)rnd(cx, cx + cw - 10), (float)rnd(cy, cy + ch - 10), 0, 0);
            inner.w = (float)rnd(5, (int)clip.r() - (int)inner.x);
            inner.h = (float)rnd(5, (int)clip.b() - (int)inner.y);
            c.pushClip(inner);
            // Фигура заведомо больше клипа, со скруглением.
            c.fillRoundRect(Rect(clip.x - 50, clip.y - 50, clip.w + 100, clip.h + 100),
                            CornerRadii(37.f), Color(255, 0, 0, 200));
            c.fillRect(Rect(-100, -100, 1000, 1000), Color(0, 255, 0, 128));
            c.popClip();
            c.popClip();
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    if (buf[(size_t)y * W + x] == SENT) continue; // не тронут
                    // Тронут: должен быть внутри вложенного клипа (с допуском 1 px
                    // на антиалиасинг кромки скругления).
                    if ((float)x + 0.5f < inner.x - 1.f || (float)x + 0.5f > inner.r() + 1.f ||
                        (float)y + 0.5f < inner.y - 1.f || (float)y + 0.5f > inner.b() + 1.f)
                        violations++;
                }
            }
        }
        check(violations == 0, "RD-06: ни один пиксель не вышел за вложенный клип");
        printf("   RD-06: нарушений=%d за 60 случайных сцен\n", violations);
    }

    // --- RD-05: кэш масштабированных картинок ------------------------------
    {
        std::vector<uint32_t> buf((size_t)W * H, 0xFF000000u);
        Canvas c;
        c.attach(buf.data(), W, H, W);
        // Обложка 480x270 уменьшается до 416x234 (условие кэша соблюдено).
        Image cover = makeImage(480, 270, 0xABCDEFull, 7);
        Rect dst(0, 0, 416, 234);
        c.image(cover, dst, 1.f, CornerRadii(12));
        size_t c1 = c.scaledCacheComputes();
        c.image(cover, dst, 1.f, CornerRadii(12));
        check(c.scaledCacheComputes() == c1, "RD-05: повтор не масштабирует заново");

        // Тот же id, но буфер в другом месте (адрес иной) — тоже попадание.
        Image cover2 = makeImage(480, 270, 0xABCDEFull, 7);
        c.image(cover2, dst, 1.f, CornerRadii(12));
        check(c.scaledCacheComputes() == c1, "RD-05: ключ по id, не по адресу");

        // 60 разных обложек при лимите 48 записей — кэш не превышает лимит.
        for (int i = 0; i < 60; i++) {
            Image im = makeImage(480, 270, 0x100000ull + (uint64_t)i, (uint8_t)(i * 3));
            c.image(im, Rect(0, 0, 416, 234), 1.f, CornerRadii(12));
        }
        check(c.scaledCacheEntries() <= Canvas::kScaledCacheLimitEntries,
              "RD-05: записей не больше лимита");
        check(c.scaledCacheBytes() <= Canvas::kScaledCacheLimitBytes,
              "RD-05: байт не больше лимита");
        printf("   RD-05: entries=%zu bytes=%zu computes=%zu\n", c.scaledCacheEntries(),
               c.scaledCacheBytes(), c.scaledCacheComputes());
    }

    printf("\n== %s: %d проверок, %d провалов ==\n", fails ? "ЕСТЬ ОШИБКИ" : "ЗЕЛЕНО", checks, fails);
    return fails ? 1 : 0;
}
