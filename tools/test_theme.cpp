// tools/test_theme.cpp — TEST-03: сверка темы в коде с design/tokens.json.
//
// Проверяет:
//   1) 37 цветовых ролей × 2 темы (dark/light) против design/tokens.json,
//      допуск ΔE00 ≤ 2,0 (docs/impl-tz.md §5.2, п.3);
//   2) типографику — 12 ролей по docs/impl-tz.md §5.2 (labelSmall = 12/16,
//      решение Р-01);
//   3) формы (Shapes) — 9 значений против tokens.json.
//
// Сборка/запуск:
//   g++ -std=c++17 -O2 -Isource tools/test_theme.cpp source/md3/theme.cpp -o /tmp/test_theme
//   /tmp/test_theme design/tokens.json
//
// Файл не зависит от сети, jansson и Switch: JSON читается минимальным
// собственным сканером, потому что нужен только плоский объект role->color.

#include "md3/theme.hpp"

#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace tnx;
using namespace tnx::md3;

namespace {

const char* const kSeedHex = "FFC3002F";

struct RoleRef {
    const char* name;
    Color ColorScheme::*field;
};

// 37 ролей — ровно набор из design/tokens.json.
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
    {"surfaceDim", &ColorScheme::surfaceDim},
    {"surfaceBright", &ColorScheme::surfaceBright},
    {"surfaceContainerLowest", &ColorScheme::surfaceContainerLowest},
    {"surfaceContainerLow", &ColorScheme::surfaceContainerLow},
    {"surfaceContainer", &ColorScheme::surfaceContainer},
    {"surfaceContainerHigh", &ColorScheme::surfaceContainerHigh},
    {"surfaceContainerHighest", &ColorScheme::surfaceContainerHighest},
    {"outline", &ColorScheme::outline},
    {"outlineVariant", &ColorScheme::outlineVariant},
    {"inverseSurface", &ColorScheme::inverseSurface},
    {"inverseOnSurface", &ColorScheme::inverseOnSurface},
    {"inversePrimary", &ColorScheme::inversePrimary},
    {"shadow", &ColorScheme::shadow},
    {"scrim", &ColorScheme::scrim},
    {"surfaceTint", &ColorScheme::surfaceTint},
};

std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// --- минимальный разбор плоского объекта вид {"name": "#AARRGGBB", ...} ----
// Возвращает карту role->ARGB для объекта темы ("dark"/"light").
std::vector<std::pair<std::string, uint32_t>> parseColorObject(const std::string& json,
                                                               const std::string& key) {
    std::vector<std::pair<std::string, uint32_t>> out;
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return out;
    size_t open = json.find('{', k);
    if (open == std::string::npos) return out;
    size_t i = open + 1;
    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' ||
                                   json[i] == '\t' || json[i] == ','))
            i++;
        if (i >= json.size() || json[i] == '}') break;
        if (json[i] != '"') break;
        size_t nameEnd = json.find('"', i + 1);
        if (nameEnd == std::string::npos) break;
        std::string name = json.substr(i + 1, nameEnd - i - 1);
        size_t colon = json.find(':', nameEnd);
        if (colon == std::string::npos) break;
        size_t q1 = json.find('"', colon);
        if (q1 == std::string::npos) break;
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string val = json.substr(q1 + 1, q2 - q1 - 1);
        if (!val.empty() && val[0] == '#') out.emplace_back(name, (uint32_t)strtoul(val.c_str() + 1, nullptr, 16));
        i = q2 + 1;
    }
    return out;
}

bool roleMatch(const std::vector<std::pair<std::string, uint32_t>>& map, const std::string& name,
               uint32_t& argb) {
    for (auto& p : map)
        if (p.first == name) { argb = p.second; return true; }
    return false;
}

// --- CIEDE2000 (sRGB→Lab, D65) ---------------------------------------------
double srgbToLinear(double c) {
    c /= 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
void rgbToLab(uint8_t r, uint8_t g, uint8_t b, double& L, double& A, double& B) {
    double R = srgbToLinear(r), G = srgbToLinear(g), Bl = srgbToLinear(b);
    double X = R * 0.4124564 + G * 0.3575761 + Bl * 0.1804375;
    double Y = R * 0.2126729 + G * 0.7151522 + Bl * 0.0721750;
    double Z = R * 0.0193339 + G * 0.1191920 + Bl * 0.9503041;
    X /= 0.95047; Z /= 1.08883;
    auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : (7.787 * t + 16.0 / 116.0); };
    double fx = f(X), fy = f(Y), fz = f(Z);
    L = 116 * fy - 16;
    A = 500 * (fx - fy);
    B = 200 * (fy - fz);
}
double deltaE00(const Color& a, const Color& b) {
    double L1, a1, b1, L2, a2, b2;
    rgbToLab(a.r, a.g, a.b, L1, a1, b1);
    rgbToLab(b.r, b.g, b.b, L2, a2, b2);
    const double kL = 1, kC = 1, kH = 1;
    double C1 = std::sqrt(a1 * a1 + b1 * b1), C2 = std::sqrt(a2 * a2 + b2 * b2);
    double Cbar = (C1 + C2) / 2;
    double G = 0.5 * (1 - std::sqrt(std::pow(Cbar, 7) / (std::pow(Cbar, 7) + std::pow(25.0, 7))));
    double ap1 = (1 + G) * a1, ap2 = (1 + G) * a2;
    double Cp1 = std::sqrt(ap1 * ap1 + b1 * b1), Cp2 = std::sqrt(ap2 * ap2 + b2 * b2);
    double hp1 = (ap1 == 0 && b1 == 0) ? 0 : std::atan2(b1, ap1);
    double hp2 = (ap2 == 0 && b2 == 0) ? 0 : std::atan2(b2, ap2);
    if (hp1 < 0) hp1 += 2 * M_PI;
    if (hp2 < 0) hp2 += 2 * M_PI;
    double dLp = L2 - L1, dCp = Cp2 - Cp1;
    double dhp = 0;
    if (Cp1 * Cp2 != 0) {
        dhp = hp2 - hp1;
        if (dhp > M_PI) dhp -= 2 * M_PI;
        else if (dhp < -M_PI) dhp += 2 * M_PI;
    }
    double dHp = 2 * std::sqrt(Cp1 * Cp2) * std::sin(dhp / 2);
    double Lbp = (L1 + L2) / 2, Cbp = (Cp1 + Cp2) / 2;
    double hbp;
    if (Cp1 * Cp2 == 0) hbp = hp1 + hp2;
    else {
        hbp = (hp1 + hp2) / 2;
        if (std::fabs(hp1 - hp2) > M_PI) hbp += (hp1 + hp2 < 2 * M_PI) ? M_PI : -M_PI;
    }
    double T = 1 - 0.17 * std::cos(hbp - M_PI / 6) + 0.24 * std::cos(2 * hbp) +
               0.32 * std::cos(3 * hbp + M_PI / 30) - 0.20 * std::cos(4 * hbp - 63 * M_PI / 180);
    double dThetaDeg = 30.0 * std::exp(-std::pow((hbp * 180 / M_PI - 275) / 25, 2));
    double Rc = 2 * std::sqrt(std::pow(Cbp, 7) / (std::pow(Cbp, 7) + std::pow(25.0, 7)));
    double Sl = 1 + (0.015 * std::pow(Lbp - 50, 2)) / std::sqrt(20 + std::pow(Lbp - 50, 2));
    double Sc = 1 + 0.045 * Cbp;
    double Sh = 1 + 0.015 * Cbp * T;
    double Rt = -std::sin(2.0 * dThetaDeg * M_PI / 180.0) * Rc;
    return std::sqrt(std::pow(dLp / (kL * Sl), 2) + std::pow(dCp / (kC * Sc), 2) +
                     std::pow(dHp / (kH * Sh), 2) + Rt * (dCp / (kC * Sc)) * (dHp / (kH * Sh)));
}

struct TypoExp { const char* name; float size, lh, track; bool medium; };
const TypoExp kTypo[] = {
    {"displaySmall", 36, 44, 0.f, false},   {"headlineMedium", 28, 36, 0.f, false},
    {"headlineSmall", 24, 32, 0.f, false},  {"titleLarge", 22, 28, 0.f, true},
    {"titleMedium", 16, 24, 0.15f, true},   {"titleSmall", 14, 20, 0.1f, true},
    {"bodyLarge", 16, 24, 0.15f, false},    {"bodyMedium", 14, 20, 0.15f, false},
    {"bodySmall", 12, 16, 0.2f, false},     {"labelLarge", 14, 20, 0.1f, true},
    {"labelMedium", 12, 16, 0.3f, true},    {"labelSmall", 12, 16, 0.4f, true},
};

const TextStyle* typoPtr(const Typography& t, const char* name) {
    if (!strcmp(name, "displaySmall")) return &t.displaySmall;
    if (!strcmp(name, "headlineMedium")) return &t.headlineMedium;
    if (!strcmp(name, "headlineSmall")) return &t.headlineSmall;
    if (!strcmp(name, "titleLarge")) return &t.titleLarge;
    if (!strcmp(name, "titleMedium")) return &t.titleMedium;
    if (!strcmp(name, "titleSmall")) return &t.titleSmall;
    if (!strcmp(name, "bodyLarge")) return &t.bodyLarge;
    if (!strcmp(name, "bodyMedium")) return &t.bodyMedium;
    if (!strcmp(name, "bodySmall")) return &t.bodySmall;
    if (!strcmp(name, "labelLarge")) return &t.labelLarge;
    if (!strcmp(name, "labelMedium")) return &t.labelMedium;
    if (!strcmp(name, "labelSmall")) return &t.labelSmall;
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "design/tokens.json";
    const std::string json = readFile(path);
    if (json.empty()) {
        std::printf("ОШИБКА: не читается %s\n", path);
        return 1;
    }

    int failures = 0;

    // 1) Цвета: 37 ролей × 2 темы.
    for (int d = 0; d < 2; d++) {
        const bool dark = d == 0;
        auto map = parseColorObject(json, dark ? "dark" : "light");
        ColorScheme cs = schemeFromSeed(0xFFC3002F, dark);
        double worst = -1.0;
        const char* worstRole = "—";
        int missing = 0;
        for (const RoleRef& r : kRoles) {
            uint32_t argb = 0;
            if (!roleMatch(map, r.name, argb)) { missing++; continue; }
            Color ref = Color::argb(argb);
            Color got = cs.*(r.field);
            double de = deltaE00(got, ref);
            if (de > worst) { worst = de; worstRole = r.name; }
            if (de > 2.0) {
                failures++;
                std::printf("  [%s] %-26s ΔE00=%.2f (> 2.0) ref=#%08X got=#%02X%02X%02X\n",
                            dark ? "dark" : "light", r.name, de, argb, got.r, got.g, got.b);
            }
        }
        std::printf("%s: ролей=%d, отсутствуют=%d, макс ΔE00=%.2f (%s)\n",
                    dark ? "dark " : "light", (int)(sizeof(kRoles) / sizeof(kRoles[0])), missing,
                    worst, worstRole);
        if (missing) failures++;
    }

    // 2) Типографика (docs/impl-tz.md §5.2).
    Typography ty = Typography::standard();
    for (const TypoExp& e : kTypo) {
        const TextStyle* s = typoPtr(ty, e.name);
        bool ok = s && std::fabs(s->size - e.size) < 0.01f &&
                  std::fabs(s->lineHeight - e.lh) < 0.01f &&
                  std::fabs(s->letterSpacing - e.track) < 0.001f &&
                  (e.medium ? s->font == FontRole::Medium : s->font == FontRole::Regular);
        if (!ok) {
            failures++;
            std::printf("  типографика %s: не совпала (%.1f/%.1f/%.2f)\n", e.name,
                        s ? s->size : -1.f, s ? s->lineHeight : -1.f, s ? s->letterSpacing : -1.f);
        }
    }
    std::printf("typography: 12 ролей проверено\n");

    // 3) Формы.
    const Shapes sh;
    struct { const char* n; float v, got; } shapes[] = {
        {"none", 0, sh.none}, {"extraSmall", 4, sh.extraSmall}, {"small", 8, sh.small},
        {"medium", 12, sh.medium}, {"large", 16, sh.large}, {"largeIncreased", 20, sh.largeIncreased},
        {"extraLarge", 28, sh.extraLarge}, {"full", 999, sh.full}, {"badge", 6, sh.badge},
    };
    for (auto& s : shapes)
        if (std::fabs(s.v - s.got) > 0.01f) { failures++; std::printf("  shape %s: %g != %g\n", s.n, s.got, s.v); }
    std::printf("shapes: 9 значений проверено\n");

    std::printf("РЕЗУЛЬТАТ TEST-03: %s\n", failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}
