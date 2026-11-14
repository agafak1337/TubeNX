// tools/png2jpg.cpp — конвертер PNG → baseline JPEG для иконки NACP/hbmenu.
// hbmenu декодирует иконку NRO как JPEG, поэтому в .nro кладём именно JPEG.
// Собирается на хосте: g++ -std=c++17 -I source/third_party tools/png2jpg.cpp -o png2jpg
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: png2jpg <in.png> <out.jpg> [quality]\n");
        return 2;
    }
    int quality = (argc > 3) ? atoi(argv[3]) : 92;
    int w = 0, h = 0, comp = 0;
    // Грузим в RGB (alpha не нужна: иконка полнослойная, но на всякий случай
    // прозрачность уже слита с фоном в исходном PNG).
    unsigned char* px = stbi_load(argv[1], &w, &h, &comp, 3);
    if (!px) {
        fprintf(stderr, "png2jpg: не удалось прочитать %s\n", argv[1]);
        return 1;
    }
    if (!stbi_write_jpg(argv[2], w, h, 3, px, quality)) {
        fprintf(stderr, "png2jpg: не удалось записать %s\n", argv[2]);
        stbi_image_free(px);
        return 1;
    }
    printf("png2jpg: %s -> %s  %dx%d q=%d\n", argv[1], argv[2], w, h, quality);
    stbi_image_free(px);
    return 0;
}
