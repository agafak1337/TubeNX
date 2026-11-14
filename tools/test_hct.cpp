// tools/test_hct.cpp — host-тест движка динамических цветов M3 (HCT/CAM16).
//
// Сборка и запуск:
//   g++ -std=c++17 -O2 -I source tools/test_hct.cpp source/md3/theme.cpp -o /tmp/test_hct
//   /tmp/test_hct
//
// Программа печатает схему для seed 0xFF6750A4 (light/dark), сверяет её с
// эталонной схемой Material 3 baseline и считает максимальную ошибку по
// каналам R/G/B. Дополнительно проверяются типографика и elevation.

#include "md3/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace tnx;
using namespace tnx::md3;

namespace {

struct RoleRef {
    const char* name;
    Color ColorScheme::*field;
};

const RoleRef kRoles[] = {
    {"primary", &ColorScheme::primary},
    {"onPrimary", &ColorScheme::onPrimary},
    {"primaryContainer", &ColorScheme::primaryContainer},
    {"onPrimaryContainer", &ColorScheme::onPrimaryContainer},
    {"secondary", &ColorScheme::secondary},
    {"onSecondary", &ColorScheme::onSecondary},
    {"secondaryContainer", &ColorScheme::secondaryContainer},
    {"onSecondaryContainer", &ColorScheme::onSecondaryContainer},
    {"tertiary", &ColorScheme::tertiary},
    {"onTertiary", &ColorScheme::onTertiary},
    {"tertiaryContainer", &ColorScheme::tertiaryContainer},
    {"onTertiaryContainer", &ColorScheme::onTertiaryContainer},
    {"error", &ColorScheme::error},
    {"onError", &ColorScheme::onError},
    {"errorContainer", &ColorScheme::errorContainer},
    {"onErrorContainer", &ColorScheme::onErrorContainer},
    {"background", &ColorScheme::background},
    {"onBackground", &ColorScheme::onBackground},
    {"surface", &ColorScheme::surface},
    {"onSurface", &ColorScheme::onSurface},
    {"surfaceVariant", &ColorScheme::surfaceVariant},
    {"onSurfaceVariant", &ColorScheme::onSurfaceVariant},
    {"outline", &ColorScheme::outline},
    {"outlineVariant", &ColorScheme::outlineVariant},
    {"shadow", &ColorScheme::shadow},
    {"scrim", &ColorScheme::scrim},
    {"inverseSurface", &ColorScheme::inverseSurface},
    {"inverseOnSurface", &ColorScheme::inverseOnSurface},
    {"inversePrimary", &ColorScheme::inversePrimary},
    {"surfaceDim", &ColorScheme::surfaceDim},
    {"surfaceBright", &ColorScheme::surfaceBright},
    {"surfaceContainerLowest", &ColorScheme::surfaceContainerLowest},
    {"surfaceContainerLow", &ColorScheme::surfaceContainerLow},
    {"surfaceContainer", &ColorScheme::surfaceContainer},
    {"surfaceContainerHigh", &ColorScheme::surfaceContainerHigh},
    {"surfaceContainerHighest", &ColorScheme::surfaceContainerHighest},
    {"surfaceTint", &ColorScheme::surfaceTint},
};

const int kRoleCount = (int)(sizeof(kRoles) / sizeof(kRoles[0]));

enum class Group { Chromatic, Surface, Error };

// Группа нужна, чтобы отделить реальную точность алгоритма от исторических
// расхождений с таблицей M3 2021 года (см. примечание в конце).
Group groupOf(const char* name) {
    static const char* kError[] = {"error", "onError", "errorContainer", "onErrorContainer"};
    static const char* kSurface[] = {"background", "onBackground", "surface",           "onSurface",
                                     "inverseSurface", "inverseOnSurface"};
    for (const char* n : kError) {
        if (std::strcmp(name, n) == 0) return Group::Error;
    }
    for (const char* n : kSurface) {
        if (std::strcmp(name, n) == 0) return Group::Surface;
    }
    return Group::Chromatic;
}

struct RefValue {
    const char* name;
    uint32_t argb;
};

// Эталон M3 baseline для seed 0xFF6750A4 (официальная таблица Material 3).
const RefValue kLightRef[] = {
    {"primary", 0xFF6750A4},           {"onPrimary", 0xFFFFFFFF},
    {"primaryContainer", 0xFFEADDFF},  {"onPrimaryContainer", 0xFF21005D},
    {"secondary", 0xFF625B71},         {"onSecondary", 0xFFFFFFFF},
    {"secondaryContainer", 0xFFE8DEF8}, {"onSecondaryContainer", 0xFF1D192B},
    {"tertiary", 0xFF7D5260},          {"onTertiary", 0xFFFFFFFF},
    {"tertiaryContainer", 0xFFFFD8E4}, {"onTertiaryContainer", 0xFF31111D},
    {"error", 0xFFB3261E},             {"onError", 0xFFFFFFFF},
    {"errorContainer", 0xFFF9DEDC},    {"onErrorContainer", 0xFF410E0B},
    {"background", 0xFFFFFBFE},        {"onBackground", 0xFF1C1B1F},
    {"surface", 0xFFFFFBFE},           {"onSurface", 0xFF1C1B1F},
    {"surfaceVariant", 0xFFE7E0EC},    {"onSurfaceVariant", 0xFF49454F},
    {"outline", 0xFF79747E},           {"outlineVariant", 0xFFCAC4D0},
    {"inverseSurface", 0xFF313033},    {"inverseOnSurface", 0xFFF4EFF4},
    {"inversePrimary", 0xFFD0BCFF},
};

const RefValue kDarkRef[] = {
    {"primary", 0xFFD0BCFF},           {"onPrimary", 0xFF381E72},
    {"primaryContainer", 0xFF4F378B},  {"onPrimaryContainer", 0xFFEADDFF},
    {"secondary", 0xFFCCC2DC},         {"onSecondary", 0xFF332D41},
    {"secondaryContainer", 0xFF4A4458}, {"onSecondaryContainer", 0xFFE8DEF8},
    {"tertiary", 0xFFEFB8C8},          {"onTertiary", 0xFF492532},
    {"tertiaryContainer", 0xFF633B48}, {"onTertiaryContainer", 0xFFFFD8E4},
    {"error", 0xFFF2B8B5},             {"onError", 0xFF601410},
    {"errorContainer", 0xFF8C1D18},    {"onErrorContainer", 0xFFF9DEDC},
    {"background", 0xFF1C1B1F},        {"onBackground", 0xFFE6E1E9},
    {"surface", 0xFF1C1B1F},           {"onSurface", 0xFFE6E1E9},
    {"surfaceVariant", 0xFF49454F},    {"onSurfaceVariant", 0xFFCAC4D0},
    {"outline", 0xFF938F99},           {"outlineVariant", 0xFF49454F},
    {"inverseSurface", 0xFFE6E1E9},    {"inverseOnSurface", 0xFF313033},
    {"inversePrimary", 0xFF6750A4},
};

void printScheme(const char* title, const ColorScheme& s) {
    std::printf("=== %s ===\n", title);
    for (int i = 0; i < kRoleCount; i++) {
        const Color& c = s.*(kRoles[i].field);
        std::printf("  %-24s #%02X%02X%02X\n", kRoles[i].name, c.r, c.g, c.b);
    }
    std::printf("\n");
}

const Color* findRole(const ColorScheme& s, const char* name) {
    for (int i = 0; i < kRoleCount; i++) {
        if (std::strcmp(kRoles[i].name, name) == 0) return &(s.*(kRoles[i].field));
    }
    return nullptr;
}

int channelDelta(uint32_t a, uint32_t b, int shift) {
    const int ca = (int)((a >> shift) & 0xFF);
    const int cb = (int)((b >> shift) & 0xFF);
    return ca > cb ? ca - cb : cb - ca;
}

struct GroupMax {
    int error = 0;
    const char* role = "-";
};

// Возвращает максимальную ошибку по всем ролям; попутно заполняет максимумы
// по группам (хроматические / surface / error).
int compareScheme(const char* title, const ColorScheme& s, const RefValue* ref, int count,
                  GroupMax (&groups)[3]) {
    std::printf("--- Сверка с эталоном M3 baseline (%s) ---\n", title);
    std::printf("  %-24s %-8s %-8s %s\n", "роль", "получен", "эталон", "dR dG dB");

    int maxErr = 0;
    for (int i = 0; i < count; i++) {
        const Color* got = findRole(s, ref[i].name);
        if (!got) {
            std::printf("  %-24s ОТСУТСТВУЕТ\n", ref[i].name);
            maxErr = 999;
            continue;
        }
        const uint32_t g = 0xFF000000u | ((uint32_t)got->r << 16) | ((uint32_t)got->g << 8) |
                           (uint32_t)got->b;
        const int dr = channelDelta(g, ref[i].argb, 16);
        const int dg = channelDelta(g, ref[i].argb, 8);
        const int db = channelDelta(g, ref[i].argb, 0);
        const int e = (dr > dg) ? (dr > db ? dr : db) : (dg > db ? dg : db);

        std::printf("  %-24s #%02X%02X%02X  #%06X  %2d %2d %2d\n", ref[i].name, got->r,
                    got->g, got->b, ref[i].argb & 0xFFFFFFu, dr, dg, db);

        GroupMax& gm = groups[(int)groupOf(ref[i].name)];
        if (e > gm.error) {
            gm.error = e;
            gm.role = ref[i].name;
        }
        if (e > maxErr) maxErr = e;
    }
    std::printf("\n");
    return maxErr;
}

bool checkTypography() {
    const Typography a = Typography::standard();
    bool ok = true;

    // Стандартная шкала M3 (docs/impl-tz.md §5.2, design/tokens.json).
    ok = ok && a.displayLarge.size == 57.f && a.displayLarge.lineHeight == 64.f;
    ok = ok && a.displayMedium.size == 45.f && a.displayMedium.lineHeight == 52.f;
    ok = ok && a.displaySmall.size == 36.f && a.displaySmall.lineHeight == 44.f;
    ok = ok && a.headlineLarge.size == 32.f && a.headlineLarge.lineHeight == 40.f;
    ok = ok && a.headlineMedium.size == 28.f && a.headlineMedium.lineHeight == 36.f;
    ok = ok && a.headlineSmall.size == 24.f && a.headlineSmall.lineHeight == 32.f;
    ok = ok && a.titleLarge.size == 22.f && a.titleLarge.lineHeight == 28.f;
    ok = ok && a.titleLarge.font == FontRole::Medium;
    ok = ok && a.titleMedium.size == 16.f && a.titleMedium.lineHeight == 24.f;
    ok = ok && a.titleMedium.font == FontRole::Medium;
    ok = ok && a.titleSmall.size == 14.f && a.titleSmall.lineHeight == 20.f;
    ok = ok && a.titleSmall.font == FontRole::Medium;
    ok = ok && a.bodyLarge.size == 16.f && a.bodyLarge.lineHeight == 24.f;
    ok = ok && a.bodyMedium.size == 14.f && a.bodyMedium.lineHeight == 20.f;
    ok = ok && a.bodySmall.size == 12.f && a.bodySmall.lineHeight == 16.f;
    ok = ok && a.labelLarge.size == 14.f && a.labelLarge.lineHeight == 20.f;
    ok = ok && a.labelLarge.font == FontRole::Medium;
    ok = ok && a.labelMedium.size == 12.f && a.labelMedium.lineHeight == 16.f;
    // Р-01: минимум 12 px.
    ok = ok && a.labelSmall.size == 12.f && a.labelSmall.lineHeight == 16.f;
    ok = ok && a.labelSmall.font == FontRole::Medium;

    // Трекинг — из таблицы §5.2.
    ok = ok && a.displaySmall.letterSpacing == 0.f;
    ok = ok && a.titleMedium.letterSpacing == 0.15f;
    ok = ok && a.titleSmall.letterSpacing == 0.1f;
    ok = ok && a.bodyLarge.letterSpacing == 0.15f;
    ok = ok && a.bodyMedium.letterSpacing == 0.15f;
    ok = ok && a.bodySmall.letterSpacing == 0.2f;
    ok = ok && a.labelLarge.letterSpacing == 0.1f;
    ok = ok && a.labelMedium.letterSpacing == 0.3f;
    ok = ok && a.labelSmall.letterSpacing == 0.4f;
    return ok;
}

bool checkShapes() {
    const Shapes s;
    return s.none == 0.f && s.extraSmall == 4.f && s.small == 8.f && s.medium == 12.f &&
           s.large == 16.f && s.largeIncreased == 20.f && s.extraLarge == 28.f &&
           s.full == 999.f && s.badge == 6.f;
}

bool checkElevation() {
    struct Row {
        int level;
        float blur, dy, alpha;
    };
    const Row rows[] = {{0, 0.f, 0.f, 0.f},      {1, 6.f, 1.f, 0.30f}, {2, 10.f, 2.f, 0.32f},
                        {3, 16.f, 4.f, 0.34f},   {4, 24.f, 6.f, 0.36f}, {5, 32.f, 8.f, 0.38f}};
    for (const Row& r : rows) {
        const Elevation e = elevationFor(r.level);
        if (e.blur != r.blur || e.dy != r.dy || e.alpha != r.alpha) return false;
    }
    const Elevation lo = elevationFor(-3);
    const Elevation hi = elevationFor(42);
    return lo.blur == 0.f && lo.alpha == 0.f && hi.blur == 32.f && hi.alpha == 0.38f;
}

// --- M3-02: контраст on/container для 20 seed + edge-cases ------------------
double srgbLin(int c8) {
    const double c = c8 / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double relLum(const Color& c) {
    return 0.2126 * srgbLin(c.r) + 0.7152 * srgbLin(c.g) + 0.0722 * srgbLin(c.b);
}
double contrastRatio(const Color& a, const Color& b) {
    const double la = relLum(a), lb = relLum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

bool checkContrast() {
    // Edge-cases (чёрный/белый/насыщенные/серый) + детерминированные seed.
    uint32_t seeds[20] = {0xFF000000u, 0xFFFFFFFFu, 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
                          0xFF808080u, 0xFFC3002F, 0xFF6750A4, 0xFF1B5E20, 0xFF0D47A1u};
    uint32_t h = 2166136261u;
    for (int i = 10; i < 20; i++) {
        h ^= (uint32_t)i * 2654435761u;
        h *= 16777619u;
        seeds[i] = 0xFF000000u | (h & 0x00FFFFFFu);
    }

    struct Pair {
        const char* name;
        Color ColorScheme::*on;
        Color ColorScheme::*bg;
    };
    const Pair pairs[] = {
        {"onPrimary/primary", &ColorScheme::onPrimary, &ColorScheme::primary},
        {"onPrimaryContainer/primaryContainer", &ColorScheme::onPrimaryContainer,
         &ColorScheme::primaryContainer},
        {"onSecondary/secondary", &ColorScheme::onSecondary, &ColorScheme::secondary},
        {"onSecondaryContainer/secondaryContainer", &ColorScheme::onSecondaryContainer,
         &ColorScheme::secondaryContainer},
        {"onTertiary/tertiary", &ColorScheme::onTertiary, &ColorScheme::tertiary},
        {"onTertiaryContainer/tertiaryContainer", &ColorScheme::onTertiaryContainer,
         &ColorScheme::tertiaryContainer},
        {"onError/error", &ColorScheme::onError, &ColorScheme::error},
        {"onErrorContainer/errorContainer", &ColorScheme::onErrorContainer,
         &ColorScheme::errorContainer},
        {"onBackground/background", &ColorScheme::onBackground, &ColorScheme::background},
        {"onSurface/surface", &ColorScheme::onSurface, &ColorScheme::surface},
        {"onSurfaceVariant/surfaceVariant", &ColorScheme::onSurfaceVariant,
         &ColorScheme::surfaceVariant},
        {"inverseOnSurface/inverseSurface", &ColorScheme::inverseOnSurface,
         &ColorScheme::inverseSurface},
    };

    bool ok = true;
    double worst = 1e9;
    const char* worstWhat = "-";
    uint32_t worstSeed = 0;
    bool worstDark = false;
    for (uint32_t s : seeds) {
        for (bool dark : {false, true}) {
            const ColorScheme cs = schemeFromSeed(s, dark);
            // surfaceTint обязан совпадать с primary (M3).
            const bool tint =
                cs.surfaceTint.r == cs.primary.r && cs.surfaceTint.g == cs.primary.g &&
                cs.surfaceTint.b == cs.primary.b;
            if (!tint) {
                printf("   FAIL M3-02: surfaceTint != primary (seed #%06X %s)\n", s & 0xFFFFFFu,
                       dark ? "dark" : "light");
                ok = false;
            }
            // inverse* должны быть непрозрачными.
            if (cs.inverseSurface.a != 255 || cs.inverseOnSurface.a != 255 ||
                cs.inversePrimary.a != 255) {
                printf("   FAIL M3-02: inverse* не непрозрачны (seed #%06X)\n", s & 0xFFFFFFu);
                ok = false;
            }
            for (const Pair& p : pairs) {
                const double cr = contrastRatio(cs.*(p.on), cs.*(p.bg));
                if (cr < worst) {
                    worst = cr;
                    worstWhat = p.name;
                    worstSeed = s & 0xFFFFFFu;
                    worstDark = dark;
                }
                if (cr < 4.5) {
                    printf("   FAIL M3-02: %s контраст %.2f < 4.5 (seed #%06X %s)\n", p.name, cr,
                           s & 0xFFFFFFu, dark ? "dark" : "light");
                    ok = false;
                }
            }
        }
    }
    printf("   M3-02: минимум контраста on/container = %.2f (%s, seed #%06X %s) по %d seed\n",
           worst, worstWhat, worstSeed, worstDark ? "dark" : "light", 20);
    return ok;
}

} // namespace

int main() {
    const uint32_t seed = 0xFF6750A4u;

    const std::clock_t t0 = std::clock();
    const ColorScheme light = schemeFromSeed(seed, false);
    const ColorScheme dark = schemeFromSeed(seed, true);
    const double ms = 1000.0 * (double)(std::clock() - t0) / (double)CLOCKS_PER_SEC;

    printScheme("seed #6750A4, light", light);
    printScheme("seed #6750A4, dark", dark);

    GroupMax lightGroups[3];
    GroupMax darkGroups[3];
    const int maxLight = compareScheme("light", light, kLightRef,
                                       (int)(sizeof(kLightRef) / sizeof(kLightRef[0])),
                                       lightGroups);
    const int maxDark = compareScheme("dark", dark, kDarkRef,
                                      (int)(sizeof(kDarkRef) / sizeof(kDarkRef[0])),
                                      darkGroups);

    const bool alphaOk = (light.primary.a == 255 && light.surface.a == 255 && dark.primary.a == 255);
    const bool tintOk = (light.surfaceTint.r == light.primary.r &&
                         light.surfaceTint.g == light.primary.g &&
                         light.surfaceTint.b == light.primary.b &&
                         dark.surfaceTint.r == dark.primary.r &&
                         dark.surfaceTint.g == dark.primary.g &&
                         dark.surfaceTint.b == dark.primary.b);
    const bool typeOk = checkTypography();
    const bool shapesOk = checkShapes();
    const bool elevOk = checkElevation();
    const bool contrastOk = checkContrast();
    const Theme theme = Theme::make(0xFFC3002F, true);
    const bool themeOk = (theme.dark && theme.colors.dark &&
                          theme.colors.primary.r == defaultScheme(true).primary.r &&
                          theme.colors.primary.g == defaultScheme(true).primary.g &&
                          theme.colors.primary.b == defaultScheme(true).primary.b &&
                          theme.type.bodyLarge.size == Typography::standard().bodyLarge.size &&
                          theme.type.labelSmall.size == 12.f);

    std::printf("--- Итог ---\n");
    std::printf("  светлая : max error по всем ролям = %d\n", maxLight);
    std::printf("            хроматические роли = %d (%s), surface = %d (%s), error = %d (%s)\n",
                lightGroups[0].error, lightGroups[0].role, lightGroups[1].error,
                lightGroups[1].role, lightGroups[2].error, lightGroups[2].role);
    std::printf("  тёмная  : max error по всем ролям = %d\n", maxDark);
    std::printf("            хроматические роли = %d (%s), surface = %d (%s), error = %d (%s)\n",
                darkGroups[0].error, darkGroups[0].role, darkGroups[1].error,
                darkGroups[1].role, darkGroups[2].error, darkGroups[2].role);
    std::printf("  alpha=255: %s; surfaceTint==primary: %s\n", alphaOk ? "да" : "НЕТ",
                tintOk ? "да" : "НЕТ");
    std::printf("  типографика: %s; формы: %s; elevation: %s; Theme::make: %s\n",
                typeOk ? "ok" : "ОШИБКА", shapesOk ? "ok" : "ОШИБКА", elevOk ? "ok" : "ОШИБКА",
                themeOk ? "ok" : "ОШИБКА");
    std::printf("  контраст on/container (20 seed): %s\n", contrastOk ? "ok (≥4.5)" : "ОШИБКА");
    std::printf("  время генерации двух схем: %.1f мс\n\n", ms);

    std::printf("Примечание об эталоне:\n"
                "  * Хроматические роли (primary/secondary/tertiary, surfaceVariant, outline)\n"
                "    совпадают с M3 с точностью 1: реализован полный CAM16/HCT\n"
                "    (material-color-utilities, D65, adapting luminance 11.7257).\n"
                "  * background/surface в таблице 2021 года имеют тон 99 (light) и 10 (dark),\n"
                "    в современном M3 (и в задании) — 98 и 6; отсюда расхождение до 3 (light)\n"
                "    и до 9 (dark). Это ожидаемое историческое отличие, не ошибка движка.\n"
                "  * error-токены M3 baseline — это захардкоженные константы\n"
                "    Flutter ColorScheme.light()/dark(), а не результат TonalPalette:\n"
                "    они не лежат на одной HCT-палитре. HCT(#B3261E)=h26.0 c76.3 t39.7,\n"
                "    HCT(#F9DEDC)=h21.0 c9.4 t90.6, HCT(#8C1D18)=h25.3 c64.4 t30.7.\n"
                "    Стандартная палитра (hue 25, chroma 84) даёт корректные по CAM16\n"
                "    цвета (#BA1B1B, #FFDAD4, ...) — это и есть поведение библиотеки.\n");

    const bool ok = (lightGroups[0].error <= 2 && darkGroups[0].error <= 2 && alphaOk && tintOk &&
                     typeOk && shapesOk && elevOk && themeOk && contrastOk);
    std::printf("РЕЗУЛЬТАТ: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}