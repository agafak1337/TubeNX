// tools/test_m3.cpp — host-тест M3-05 / M3-08 / M3-03 (без Switch и шрифтов).
//
// Проверяет:
//   * типографику: все кегли — целые px и не меньше 12, lineHeight >= size;
//   * motion-токены совпадают с design/tokens.json;
//   * state layer совпадает с design/tokens.json → "state";
//   * формы и elevation не вырождены.
//
// Сборка/запуск:
//   g++ -std=c++17 -O2 -Isource tools/test_m3.cpp source/md3/theme.cpp -o /tmp/test_m3
//   /tmp/test_m3
#include "md3/theme.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace tnx::md3;

static int checks = 0, fails = 0;
static void check(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        fails++;
        printf("   FAIL: %s\n", what.c_str());
    }
}

static bool isInt(float v) { return std::fabs(v - std::round(v)) < 1e-4f; }

int main() {
    const Typography t = Typography::standard();
    const struct {
        const char* name;
        const TextStyle* s;
    } roles[] = {
        {"displayLarge", &t.displayLarge},   {"displayMedium", &t.displayMedium},
        {"displaySmall", &t.displaySmall},   {"headlineLarge", &t.headlineLarge},
        {"headlineMedium", &t.headlineMedium},
        {"headlineSmall", &t.headlineSmall}, {"titleLarge", &t.titleLarge},
        {"titleMedium", &t.titleMedium},     {"titleSmall", &t.titleSmall},
        {"bodyLarge", &t.bodyLarge},         {"bodyMedium", &t.bodyMedium},
        {"bodySmall", &t.bodySmall},         {"labelLarge", &t.labelLarge},
        {"labelMedium", &t.labelMedium},     {"labelSmall", &t.labelSmall},
    };
    for (const auto& r : roles) {
        check(isInt(r.s->size), std::string("M3-05: ") + r.name + " — целый px");
        check(r.s->size >= 12.f, std::string("M3-05: ") + r.name + " >= 12 px");
        check(r.s->lineHeight >= r.s->size,
              std::string("M3-05: ") + r.name + " lineHeight >= size");
    }

    // Токены движения (design/tokens.json → motion).
    check(std::fabs(motion.short2 - 0.100f) < 1e-4f, "M3-08: short2 = 100 мс");
    check(std::fabs(motion.short4 - 0.200f) < 1e-4f, "M3-08: short4 = 200 мс");
    check(std::fabs(motion.medium1 - 0.250f) < 1e-4f, "M3-08: medium1 = 250 мс");
    check(std::fabs(motion.medium2 - 0.300f) < 1e-4f, "M3-08: medium2 = 300 мс");
    check(std::fabs(motion.controls - 0.180f) < 1e-4f, "M3-08: controls = 180 мс");
    check(std::fabs(motion.skeletonPulse - 1.200f) < 1e-4f, "M3-08: skeletonPulse = 1200 мс");
    check(motion.emphasized.x1 == 0.2f && motion.emphasized.y1 == 0.f &&
              motion.emphasized.x2 == 0.f && motion.emphasized.y2 == 1.f,
          "M3-08: emphasized = cubic-bezier(0.2,0,0,1)");
    check(motion.emphasizedDecelerate.x1 == 0.05f && motion.emphasizedDecelerate.y1 == 0.7f,
          "M3-08: emphasizedDecelerate");
    check(motion.springStiffness > 0.f && motion.springDamping > 0.f,
          "M3-08: пружина задана токенами");

    // State layer (design/tokens.json → state).
    check(std::fabs(stateLayerTokens.hover - 0.08f) < 1e-4f, "M3-03: hover = 0.08");
    check(std::fabs(stateLayerTokens.focus - 0.10f) < 1e-4f, "M3-03: focus = 0.10");
    check(std::fabs(stateLayerTokens.pressed - 0.10f) < 1e-4f, "M3-03: pressed = 0.10");
    check(std::fabs(stateLayerTokens.dragged - 0.16f) < 1e-4f, "M3-03: dragged = 0.16");
    check(std::fabs(stateLayerTokens.disabledContent - 0.38f) < 1e-4f, "M3-03: disabled 0.38");
    check(std::fabs(stateLayerTokens.disabledContainer - 0.12f) < 1e-4f, "M3-03: disabled 0.12");

    // Формы и elevation.
    const Shapes sh;
    check(sh.extraSmall == 4.f && sh.small == 8.f && sh.medium == 12.f && sh.large == 16.f,
          "M3-01: формы M3");
    check(sh.largeIncreased == 20.f && sh.extraLarge == 28.f, "M3-01: формы Expressive");
    for (int lv = 0; lv <= 5; lv++) {
        const Elevation e = elevationFor(lv);
        check(e.blur >= 0.f && e.alpha >= 0.f && e.alpha <= 1.f,
              "M3-01: elevation " + std::to_string(lv));
    }

    printf("\n== %s: %d проверок, %d провалов ==\n", fails ? "ЕСТЬ ОШИБКИ" : "ЗЕЛЕНО", checks,
           fails);
    return fails ? 1 : 0;
}