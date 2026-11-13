// md3/theme.cpp — Material 3 (Material You): динамические цвета HCT/CAM16,
// типографика и elevation.
//
// Пайплайн повторяет material-color-utilities (Google, Apache-2.0):
//   sRGB -> XYZ (D65) -> CAM16 -> HCT (hue/chroma/tone) -> TonalPalette -> роли M3.
// Обратное преобразование tone -> sRGB сделано так же, как в эталонном
// HCT.getInt(): бинарный поиск по CAM16-J, обрезка результата по sRGB и
// сверка L* с требуемым тоном. Это даёт те же 8-битные значения, что и M3.
//
// Только стандартная библиотека: файл собирается и под devkitA64, и на хосте.

#include "md3/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tnx::md3 {
namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------- мелкая математика ----------

double sanitizeDegrees(double deg) {
    deg = std::fmod(deg, 360.0);
    if (deg < 0.0) deg += 360.0;
    return deg;
}

int signum(double v) {
    if (v < 0.0) return -1;
    if (v > 0.0) return 1;
    return 0;
}

double lerpD(double a, double b, double t) {
    return a + (b - a) * t;
}

// ---------- sRGB / XYZ / L* ----------

// 0..255 -> линейный RGB 0..100 (sRGB EOTF).
double linearize(int channel) {
    const double n = (double)channel / 255.0;
    if (n <= 0.040449936) return n / 12.92 * 100.0;
    return std::pow((n + 0.055) / 1.055, 2.4) * 100.0;
}

// Линейный 0..100 -> 0..255 с обрезкой и округлением (как ColorUtils.delinearized).
int delinearize(double linear) {
    const double n = linear / 100.0;
    const double d = (n <= 0.0031308) ? n * 12.92 : 1.055 * std::pow(n, 1.0 / 2.4) - 0.055;
    const double v = std::round(d * 255.0);
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return (int)v;
}

double labF(double t) {
    constexpr double e = 216.0 / 24389.0;
    constexpr double kappa = 24389.0 / 27.0;
    return t > e ? std::cbrt(t) : (kappa * t + 16.0) / 116.0;
}

double labInvf(double ft) {
    constexpr double e = 216.0 / 24389.0;
    constexpr double kappa = 24389.0 / 27.0;
    const double ft3 = ft * ft * ft;
    return ft3 > e ? ft3 : (116.0 * ft - 16.0) / kappa;
}

double yFromLstar(double lstar) {
    return 100.0 * labInvf((lstar + 16.0) / 116.0);
}

double lstarFromY(double y) {
    return labF(y / 100.0) * 116.0 - 16.0;
}

uint32_t argbFromRgb(int r, int g, int b) {
    return 0xFF000000u | ((uint32_t)(r & 0xFF) << 16) | ((uint32_t)(g & 0xFF) << 8) |
           (uint32_t)(b & 0xFF);
}

uint32_t argbFromLstar(double lstar) {
    const int c = delinearize(yFromLstar(lstar));
    return argbFromRgb(c, c, c);
}

uint32_t argbFromXyz(double x, double y, double z) {
    // XYZ -> линейный sRGB (матрица из material-color-utilities).
    const double lr = 3.2413774792388685 * x - 1.5376652402851851 * y - 0.49885366846268053 * z;
    const double lg = -0.9691452513005321 * x + 1.8758853451067872 * y + 0.04156585616912061 * z;
    const double lb = 0.05562093689691305 * x - 0.20395524564742123 * y + 1.0571799111220335 * z;
    return argbFromRgb(delinearize(lr), delinearize(lg), delinearize(lb));
}

double lstarFromArgb(uint32_t argb) {
    const double y = 0.2126 * linearize((int)((argb >> 16) & 0xFF)) +
                     0.7152 * linearize((int)((argb >> 8) & 0xFF)) +
                     0.0722 * linearize((int)(argb & 0xFF));
    return lstarFromY(y);
}

Color colorFromArgb(uint32_t argb) {
    return Color((uint8_t)((argb >> 16) & 0xFF), (uint8_t)((argb >> 8) & 0xFF),
                 (uint8_t)(argb & 0xFF), 255);
}

// ---------- условия наблюдения (ViewingConditions.DEFAULT) ----------

struct ViewingConditions {
    double n = 0.0;
    double aw = 0.0;
    double nbb = 0.0;
    double ncb = 0.0;
    double c = 0.0;
    double nc = 0.0;
    double z = 0.0;
    double fl = 0.0;
    double flRoot = 0.0;
    double rgbD[3] = {1.0, 1.0, 1.0};
};

// D65, adapting luminance ~11.7257 (200 лк), фон L*=50, surround=2 (телевизор в комнате).
ViewingConditions makeDefaultViewingConditions() {
    const double whitePoint[3] = {95.047, 100.0, 108.883};
    const double adaptingLuminance = (200.0 / kPi) * yFromLstar(50.0) / 100.0;
    const double surround = 2.0;
    const bool discountingIlluminant = false;

    // XYZ белой точки -> «колбочковый» отклик.
    const double rW = whitePoint[0] * 0.401288 + whitePoint[1] * 0.650173 - whitePoint[2] * 0.051461;
    const double gW = -whitePoint[0] * 0.250268 + whitePoint[1] * 1.204414 + whitePoint[2] * 0.045854;
    const double bW = -whitePoint[0] * 0.002079 + whitePoint[1] * 0.048952 + whitePoint[2] * 0.953127;
    const double coneWhite[3] = {rW, gW, bW};

    const double f = 0.8 + surround / 10.0;
    const double c = (f >= 0.9) ? lerpD(0.59, 0.69, (f - 0.9) * 10.0)
                                : lerpD(0.525, 0.59, (f - 0.8) * 10.0);
    double d = discountingIlluminant
                   ? 1.0
                   : f * (1.0 - (1.0 / 3.6) * std::exp((-adaptingLuminance - 42.0) / 92.0));
    d = std::clamp(d, 0.0, 1.0);

    ViewingConditions vc;
    vc.c = c;
    vc.nc = f;
    for (int i = 0; i < 3; i++) {
        vc.rgbD[i] = d * (100.0 / coneWhite[i]) + 1.0 - d;
    }

    const double k = 1.0 / (5.0 * adaptingLuminance + 1.0);
    const double k4 = k * k * k * k;
    const double k4F = 1.0 - k4;
    vc.fl = k4 * adaptingLuminance + 0.1 * k4F * k4F * std::cbrt(5.0 * adaptingLuminance);

    const double n = yFromLstar(50.0) / whitePoint[1];
    vc.n = n;
    vc.z = 1.48 + std::sqrt(n);
    vc.nbb = 0.725 / std::pow(n, 0.2);
    vc.ncb = vc.nbb;

    double aFactors[3];
    double aResponses[3];
    for (int i = 0; i < 3; i++) {
        aFactors[i] = std::pow((vc.fl * vc.rgbD[i] * coneWhite[i]) / 100.0, 0.42);
        aResponses[i] = 400.0 * aFactors[i] / (aFactors[i] + 27.13);
    }
    vc.aw = (2.0 * aResponses[0] + aResponses[1] + 0.05 * aResponses[2]) * vc.nbb;
    vc.flRoot = std::pow(vc.fl, 0.25);
    return vc;
}

const ViewingConditions& defaultVc() {
    static const ViewingConditions vc = makeDefaultViewingConditions();
    return vc;
}

// ---------- CAM16 ----------

struct Cam16 {
    double hue = 0.0;
    double chroma = 0.0;
    double j = 0.0;
    double q = 0.0;
    double m = 0.0;
    double s = 0.0;
    double jstar = 0.0;
    double astar = 0.0;
    double bstar = 0.0;
};

Cam16 cam16FromInt(uint32_t argb, const ViewingConditions& vc) {
    const double rl = linearize((int)((argb >> 16) & 0xFF));
    const double gl = linearize((int)((argb >> 8) & 0xFF));
    const double bl = linearize((int)(argb & 0xFF));

    const double x = 0.41233895 * rl + 0.35762064 * gl + 0.18051042 * bl;
    const double y = 0.2126 * rl + 0.7152 * gl + 0.0722 * bl;
    const double z = 0.01932141 * rl + 0.11916382 * gl + 0.95034478 * bl;

    const double rC = 0.401288 * x + 0.650173 * y - 0.051461 * z;
    const double gC = -0.250268 * x + 1.204414 * y + 0.045854 * z;
    const double bC = -0.002079 * x + 0.048952 * y + 0.953127 * z;

    const double rD = vc.rgbD[0] * rC;
    const double gD = vc.rgbD[1] * gC;
    const double bD = vc.rgbD[2] * bC;

    const double rAF = std::pow(vc.fl * std::fabs(rD) / 100.0, 0.42);
    const double gAF = std::pow(vc.fl * std::fabs(gD) / 100.0, 0.42);
    const double bAF = std::pow(vc.fl * std::fabs(bD) / 100.0, 0.42);

    const double rA = (double)signum(rD) * 400.0 * rAF / (rAF + 27.13);
    const double gA = (double)signum(gD) * 400.0 * gAF / (gAF + 27.13);
    const double bA = (double)signum(bD) * 400.0 * bAF / (bAF + 27.13);

    const double a = (11.0 * rA - 12.0 * gA + bA) / 11.0;
    const double b = (rA + gA - 2.0 * bA) / 9.0;
    const double u = (20.0 * rA + 20.0 * gA + 21.0 * bA) / 20.0;
    const double p2 = (40.0 * rA + 20.0 * gA + bA) / 20.0;

    const double hue = sanitizeDegrees(std::atan2(b, a) * 180.0 / kPi);
    const double hueRadians = hue * kPi / 180.0;

    const double ac = p2 * vc.nbb;
    const double j = 100.0 * std::pow(ac / vc.aw, vc.c * vc.z);
    const double q = (4.0 / vc.c) * std::sqrt(j / 100.0) * (vc.aw + 4.0) * vc.flRoot;

    const double huePrime = (hue < 20.14) ? hue + 360.0 : hue;
    const double eHue = 0.25 * (std::cos(huePrime * kPi / 180.0 + 2.0) + 3.8);
    const double p1 = (50000.0 / 13.0) * eHue * vc.nc * vc.ncb;
    const double t = p1 * std::sqrt(a * a + b * b) / (u + 0.305);
    const double alpha = std::pow(t, 0.9) * std::pow(1.64 - std::pow(0.29, vc.n), 0.73);
    const double c = alpha * std::sqrt(j / 100.0);
    const double m = c * vc.flRoot;
    const double s = 50.0 * std::sqrt(alpha * vc.c / (vc.aw + 4.0));

    const double jstar = (1.0 + 100.0 * 0.007) * j / (1.0 + 0.007 * j);
    const double mstar = (1.0 / 0.0228) * std::log(1.0 + 0.0228 * m);
    const double astar = mstar * std::cos(hueRadians);
    const double bstar = mstar * std::sin(hueRadians);

    Cam16 out;
    out.hue = hue;
    out.chroma = c;
    out.j = j;
    out.q = q;
    out.m = m;
    out.s = s;
    out.jstar = jstar;
    out.astar = astar;
    out.bstar = bstar;
    return out;
}

Cam16 cam16FromJch(double j, double c, double h, const ViewingConditions& vc) {
    const double hueRadians = h * kPi / 180.0;
    const double q = (4.0 / vc.c) * std::sqrt(j / 100.0) * (vc.aw + 4.0) * vc.flRoot;
    const double m = c * vc.flRoot;
    const double alpha = c / std::sqrt(j / 100.0);
    const double s = 50.0 * std::sqrt(alpha * vc.c / (vc.aw + 4.0));
    const double jstar = (1.0 + 100.0 * 0.007) * j / (1.0 + 0.007 * j);
    const double mstar = (1.0 / 0.0228) * std::log(1.0 + 0.0228 * m);
    const double astar = mstar * std::cos(hueRadians);
    const double bstar = mstar * std::sin(hueRadians);

    Cam16 out;
    out.hue = h;
    out.chroma = c;
    out.j = j;
    out.q = q;
    out.m = m;
    out.s = s;
    out.jstar = jstar;
    out.astar = astar;
    out.bstar = bstar;
    return out;
}

// CAM16 -> sRGB (с обрезкой по sRGB, как CAM16.viewed).
uint32_t argbFromCam16(const Cam16& cam, const ViewingConditions& vc) {
    const double alpha =
        (cam.chroma == 0.0 || cam.j == 0.0) ? 0.0 : cam.chroma / std::sqrt(cam.j / 100.0);

    const double t = std::pow(
        alpha / std::pow(1.64 - std::pow(0.29, vc.n), 0.73), 1.0 / 0.9);
    const double hRad = cam.hue * kPi / 180.0;

    const double eHue = 0.25 * (std::cos(hRad + 2.0) + 3.8);
    const double ac = vc.aw * std::pow(cam.j / 100.0, 1.0 / vc.c / vc.z);
    const double p1 = eHue * (50000.0 / 13.0) * vc.nc * vc.ncb;
    const double p2 = ac / vc.nbb;

    const double hSin = std::sin(hRad);
    const double hCos = std::cos(hRad);

    const double gamma =
        (23.0 * (p2 + 0.305) * t) / (23.0 * p1 + 11.0 * t * hCos + 108.0 * t * hSin);
    const double a = gamma * hCos;
    const double b = gamma * hSin;

    const double rA = (460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0;
    const double gA = (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0;
    const double bA = (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0;

    const double rBase = std::max(0.0, 27.13 * std::fabs(rA) / (400.0 - std::fabs(rA)));
    const double gBase = std::max(0.0, 27.13 * std::fabs(gA) / (400.0 - std::fabs(gA)));
    const double bBase = std::max(0.0, 27.13 * std::fabs(bA) / (400.0 - std::fabs(bA)));

    const double rC = (double)signum(rA) * (100.0 / vc.fl) * std::pow(rBase, 1.0 / 0.42);
    const double gC = (double)signum(gA) * (100.0 / vc.fl) * std::pow(gBase, 1.0 / 0.42);
    const double bC = (double)signum(bA) * (100.0 / vc.fl) * std::pow(bBase, 1.0 / 0.42);

    const double rF = rC / vc.rgbD[0];
    const double gF = gC / vc.rgbD[1];
    const double bF = bC / vc.rgbD[2];

    const double x = 1.86206786 * rF - 1.01125463 * gF + 0.14918677 * bF;
    const double y = 0.38752654 * rF + 0.62144744 * gF - 0.00897398 * bF;
    const double z = -0.01584150 * rF - 0.03412294 * gF + 1.04996444 * bF;
    return argbFromXyz(x, y, z);
}

double cam16Distance(const Cam16& a, const Cam16& b) {
    const double dJ = a.jstar - b.jstar;
    const double dA = a.astar - b.astar;
    const double dB = a.bstar - b.bstar;
    return 1.41 * std::pow(std::sqrt(dJ * dJ + dA * dA + dB * dB), 0.63);
}

// ---------- HCT: tone -> sRGB ----------

constexpr double kChromaSearchEndpoint = 0.4;
constexpr double kDeMax = 1.0;
constexpr double kDlMax = 0.2;
constexpr double kLightnessSearchEndpoint = 0.01;

// Подбор CAM16-J, при котором обрезанный по sRGB цвет имеет нужный L*.
bool findCamByJ(double hue, double chroma, double tone, Cam16& out) {
    const ViewingConditions& vc = defaultVc();
    double low = 0.0;
    double high = 100.0;
    double bestdL = 1000.0;
    double bestdE = 1000.0;
    bool hasAnswer = false;
    Cam16 best;

    while (std::fabs(low - high) > kLightnessSearchEndpoint) {
        const double mid = low + (high - low) / 2.0;
        const Cam16 beforeClip = cam16FromJch(mid, chroma, hue, vc);
        const uint32_t clipped = argbFromCam16(beforeClip, vc);
        const double clippedLstar = lstarFromArgb(clipped);
        const double dL = std::fabs(tone - clippedLstar);

        if (dL < kDlMax) {
            const Cam16 camClipped = cam16FromInt(clipped, vc);
            const Cam16 replayed = cam16FromJch(camClipped.j, camClipped.chroma, hue, vc);
            const double dE = cam16Distance(camClipped, replayed);
            if (dE <= kDeMax && dE <= bestdE) {
                bestdL = dL;
                bestdE = dE;
                best = camClipped;
                hasAnswer = true;
            }
        }

        if (bestdL == 0.0 && bestdE == 0.0) break;
        if (clippedLstar < tone) {
            low = mid;
        } else {
            high = mid;
        }
    }

    if (hasAnswer) out = best;
    return hasAnswer;
}

// HCT(hue, chroma, tone) -> ARGB. Внешний поиск — по хроме (если запрошенная
// хрома недостижима при данном тоне, берём максимально близкую).
uint32_t hctToInt(double hue, double chroma, double tone) {
    tone = std::clamp(tone, 0.0, 100.0);
    hue = sanitizeDegrees(hue);
    if (chroma < 0.0) chroma = 0.0;

    if (chroma < 1.0 || std::round(tone) <= 0.0 || std::round(tone) >= 100.0) {
        return argbFromLstar(tone);
    }

    const ViewingConditions& vc = defaultVc();
    double low = 0.0;
    double high = chroma;
    double mid = chroma;
    bool firstLoop = true;
    bool hasAnswer = false;
    Cam16 answer;

    while (std::fabs(low - high) >= kChromaSearchEndpoint) {
        Cam16 possible;
        const bool found = findCamByJ(hue, mid, tone, possible);
        if (firstLoop) {
            if (found) return argbFromCam16(possible, vc);
            firstLoop = false;
            mid = low + (high - low) / 2.0;
            continue;
        }
        if (!found) {
            high = mid;
        } else {
            answer = possible;
            hasAnswer = true;
            low = mid;
        }
        mid = low + (high - low) / 2.0;
    }

    if (!hasAnswer) return argbFromLstar(tone);
    return argbFromCam16(answer, vc);
}

struct Hct {
    double hue = 0.0;
    double chroma = 0.0;
    double tone = 0.0;
};

Hct hctFromInt(uint32_t argb) {
    const Cam16 cam = cam16FromInt(argb, defaultVc());
    Hct out;
    out.hue = cam.hue;
    out.chroma = cam.chroma;
    out.tone = lstarFromArgb(argb);
    return out;
}

// ---------- TonalPalette ----------

bool isYellowHue(double hue) {
    return hue >= 105.0 && hue < 125.0;
}

uint32_t averageArgb(uint32_t a, uint32_t b) {
    const int r = (int)(((a >> 16) & 0xFF) + ((b >> 16) & 0xFF) + 1) / 2;
    const int g = (int)(((a >> 8) & 0xFF) + ((b >> 8) & 0xFF) + 1) / 2;
    const int bl = (int)((a & 0xFF) + (b & 0xFF) + 1) / 2;
    return argbFromRgb(r, g, bl);
}

// Палитра: постоянные hue/chroma, меняется только тон. Кэш на 101 тон —
// роли M3 часто переиспользуют одни и те же тона.
struct TonalPalette {
    double hue = 0.0;
    double chroma = 0.0;
    mutable uint32_t cache[101];
    mutable bool valid[101];

    TonalPalette() {
        for (int i = 0; i <= 100; i++) {
            cache[i] = 0u;
            valid[i] = false;
        }
    }

    TonalPalette(double h, double c) : hue(h), chroma(c) {
        for (int i = 0; i <= 100; i++) {
            cache[i] = 0u;
            valid[i] = false;
        }
    }

    uint32_t tone(int t) const {
        t = std::clamp(t, 0, 100);
        if (valid[t]) return cache[t];

        uint32_t argb;
        if (t == 99 && isYellowHue(hue)) {
            // У жёлтых тонов 99 максимум хромы нестабилен — усредняем соседей.
            argb = averageArgb(tone(98), tone(100));
        } else {
            argb = hctToInt(hue, chroma, (double)t);
        }
        cache[t] = argb;
        valid[t] = true;
        return argb;
    }
};

} // namespace

// ---------- Схема из seed ----------

ColorScheme schemeFromSeed(uint32_t seedArgb, bool dark) {
    const Hct seed = hctFromInt(seedArgb);

    // Tonal spot из M3: primary — насыщенный (не меньше 48), tertiary — сдвиг +60°.
    const TonalPalette primaryPalette(seed.hue, std::max(seed.chroma, 48.0));
    const TonalPalette secondaryPalette(seed.hue, 16.0);
    const TonalPalette tertiaryPalette(sanitizeDegrees(seed.hue + 60.0), 24.0);
    const TonalPalette neutralPalette(seed.hue, 4.0);
    const TonalPalette neutralVariantPalette(seed.hue, 8.0);
    // Ошибка в M3 фиксирована и не зависит от seed: hue 25, chroma 84.
    const TonalPalette errorPalette(25.0, 84.0);

    ColorScheme s;
    s.dark = dark;

    if (!dark) {
        s.primary = colorFromArgb(primaryPalette.tone(40));
        s.onPrimary = colorFromArgb(primaryPalette.tone(100));
        s.primaryContainer = colorFromArgb(primaryPalette.tone(90));
        s.onPrimaryContainer = colorFromArgb(primaryPalette.tone(10));

        s.secondary = colorFromArgb(secondaryPalette.tone(40));
        s.onSecondary = colorFromArgb(secondaryPalette.tone(100));
        s.secondaryContainer = colorFromArgb(secondaryPalette.tone(90));
        s.onSecondaryContainer = colorFromArgb(secondaryPalette.tone(10));

        s.tertiary = colorFromArgb(tertiaryPalette.tone(40));
        s.onTertiary = colorFromArgb(tertiaryPalette.tone(100));
        s.tertiaryContainer = colorFromArgb(tertiaryPalette.tone(90));
        s.onTertiaryContainer = colorFromArgb(tertiaryPalette.tone(10));

        s.error = colorFromArgb(errorPalette.tone(40));
        s.onError = colorFromArgb(errorPalette.tone(100));
        s.errorContainer = colorFromArgb(errorPalette.tone(90));
        s.onErrorContainer = colorFromArgb(errorPalette.tone(10));

        s.background = colorFromArgb(neutralPalette.tone(98));
        s.onBackground = colorFromArgb(neutralPalette.tone(10));
        s.surface = colorFromArgb(neutralPalette.tone(98));
        s.onSurface = colorFromArgb(neutralPalette.tone(10));
        s.surfaceVariant = colorFromArgb(neutralVariantPalette.tone(90));
        s.onSurfaceVariant = colorFromArgb(neutralVariantPalette.tone(30));

        s.surfaceDim = colorFromArgb(neutralPalette.tone(87));
        s.surfaceBright = colorFromArgb(neutralPalette.tone(98));
        s.surfaceContainerLowest = colorFromArgb(neutralPalette.tone(100));
        s.surfaceContainerLow = colorFromArgb(neutralPalette.tone(96));
        s.surfaceContainer = colorFromArgb(neutralPalette.tone(94));
        s.surfaceContainerHigh = colorFromArgb(neutralPalette.tone(92));
        s.surfaceContainerHighest = colorFromArgb(neutralPalette.tone(90));

        s.outline = colorFromArgb(neutralVariantPalette.tone(50));
        s.outlineVariant = colorFromArgb(neutralVariantPalette.tone(80));
        s.shadow = colorFromArgb(neutralPalette.tone(0));
        s.scrim = colorFromArgb(neutralPalette.tone(0));

        s.inverseSurface = colorFromArgb(neutralPalette.tone(20));
        s.inverseOnSurface = colorFromArgb(neutralPalette.tone(95));
        s.inversePrimary = colorFromArgb(primaryPalette.tone(80));

        // surfaceTint в M3 совпадает с primary (в light — тон 40).
        s.surfaceTint = s.primary;
    } else {
        s.primary = colorFromArgb(primaryPalette.tone(80));
        s.onPrimary = colorFromArgb(primaryPalette.tone(20));
        s.primaryContainer = colorFromArgb(primaryPalette.tone(30));
        s.onPrimaryContainer = colorFromArgb(primaryPalette.tone(90));

        s.secondary = colorFromArgb(secondaryPalette.tone(80));
        s.onSecondary = colorFromArgb(secondaryPalette.tone(20));
        s.secondaryContainer = colorFromArgb(secondaryPalette.tone(30));
        s.onSecondaryContainer = colorFromArgb(secondaryPalette.tone(90));

        s.tertiary = colorFromArgb(tertiaryPalette.tone(80));
        s.onTertiary = colorFromArgb(tertiaryPalette.tone(20));
        s.tertiaryContainer = colorFromArgb(tertiaryPalette.tone(30));
        s.onTertiaryContainer = colorFromArgb(tertiaryPalette.tone(90));

        s.error = colorFromArgb(errorPalette.tone(80));
        s.onError = colorFromArgb(errorPalette.tone(20));
        s.errorContainer = colorFromArgb(errorPalette.tone(30));
        s.onErrorContainer = colorFromArgb(errorPalette.tone(90));

        s.background = colorFromArgb(neutralPalette.tone(6));
        s.onBackground = colorFromArgb(neutralPalette.tone(90));
        s.surface = colorFromArgb(neutralPalette.tone(6));
        s.onSurface = colorFromArgb(neutralPalette.tone(90));
        s.surfaceVariant = colorFromArgb(neutralVariantPalette.tone(30));
        s.onSurfaceVariant = colorFromArgb(neutralVariantPalette.tone(80));

        s.surfaceDim = colorFromArgb(neutralPalette.tone(6));
        s.surfaceBright = colorFromArgb(neutralPalette.tone(24));
        s.surfaceContainerLowest = colorFromArgb(neutralPalette.tone(4));
        s.surfaceContainerLow = colorFromArgb(neutralPalette.tone(10));
        s.surfaceContainer = colorFromArgb(neutralPalette.tone(12));
        s.surfaceContainerHigh = colorFromArgb(neutralPalette.tone(17));
        s.surfaceContainerHighest = colorFromArgb(neutralPalette.tone(22));

        s.outline = colorFromArgb(neutralVariantPalette.tone(60));
        s.outlineVariant = colorFromArgb(neutralVariantPalette.tone(30));
        s.shadow = colorFromArgb(neutralPalette.tone(0));
        s.scrim = colorFromArgb(neutralPalette.tone(0));

        s.inverseSurface = colorFromArgb(neutralPalette.tone(90));
        s.inverseOnSurface = colorFromArgb(neutralPalette.tone(20));
        s.inversePrimary = colorFromArgb(primaryPalette.tone(40));

        // surfaceTint в M3 совпадает с primary (в dark — тон 80).
        s.surfaceTint = s.primary;
    }

    return s;
}

ColorScheme defaultScheme(bool dark) {
    // YouTube-подобный красный; tonal spot делает из него спокойную M3-палитру.
    return schemeFromSeed(0xFFB3261Eu, dark);
}

// ---------- Типографика ----------

namespace {

TextStyle makeStyle(FontRole font, float size, float lineHeight, float letterSpacing) {
    TextStyle t;
    t.font = font;
    t.size = size;
    t.lineHeight = lineHeight;
    t.letterSpacing = letterSpacing;
    return t;
}

} // namespace

Typography Typography::standard() {
    // M3 type scale, px при 720p (1 sp ~ 1 px), трекинг — из спецификации M3.
    Typography t;
    t.displayLarge = makeStyle(FontRole::Regular, 57.f, 64.f, -0.25f);
    t.displayMedium = makeStyle(FontRole::Regular, 45.f, 52.f, 0.f);
    t.displaySmall = makeStyle(FontRole::Regular, 36.f, 44.f, 0.f);

    t.headlineLarge = makeStyle(FontRole::Regular, 32.f, 40.f, 0.f);
    t.headlineMedium = makeStyle(FontRole::Regular, 28.f, 36.f, 0.f);
    t.headlineSmall = makeStyle(FontRole::Regular, 24.f, 32.f, 0.f);

    t.titleLarge = makeStyle(FontRole::Regular, 22.f, 28.f, 0.f);
    t.titleMedium = makeStyle(FontRole::Medium, 16.f, 24.f, 0.15f);
    t.titleSmall = makeStyle(FontRole::Medium, 14.f, 20.f, 0.1f);

    t.bodyLarge = makeStyle(FontRole::Regular, 16.f, 24.f, 0.5f);
    t.bodyMedium = makeStyle(FontRole::Regular, 14.f, 20.f, 0.25f);
    t.bodySmall = makeStyle(FontRole::Regular, 12.f, 16.f, 0.4f);

    t.labelLarge = makeStyle(FontRole::Medium, 14.f, 20.f, 0.1f);
    t.labelMedium = makeStyle(FontRole::Medium, 12.f, 16.f, 0.5f);
    t.labelSmall = makeStyle(FontRole::Medium, 11.f, 16.f, 0.5f);
    return t;
}

Typography Typography::switchUi() {
    // Телевизор/диван: title/body/label +2 px и +2 px интерлиньяжа.
    Typography t = standard();
    TextStyle* readable[] = {
        &t.titleLarge, &t.titleMedium, &t.titleSmall,
        &t.bodyLarge,  &t.bodyMedium,  &t.bodySmall,
        &t.labelLarge, &t.labelMedium, &t.labelSmall,
    };
    for (TextStyle* s : readable) {
        s->size += 2.f;
        s->lineHeight += 2.f;
    }
    return t;
}

// ---------- Elevation ----------

Elevation elevationFor(int level) {
    level = std::clamp(level, 0, 5);
    // blur, dy, alpha — мягкие тени M3 для тёмного ТВ-интерфейса.
    static const float kBlur[6] = {0.f, 6.f, 10.f, 16.f, 24.f, 32.f};
    static const float kDy[6] = {0.f, 1.f, 2.f, 4.f, 6.f, 8.f};
    static const float kAlpha[6] = {0.f, 0.30f, 0.32f, 0.34f, 0.36f, 0.38f};

    Elevation e;
    e.blur = kBlur[level];
    e.dy = kDy[level];
    e.alpha = kAlpha[level];
    return e;
}

// ---------- Тема ----------

Theme Theme::make(uint32_t seedArgb, bool dark) {
    Theme t;
    t.colors = schemeFromSeed(seedArgb, dark);
    // Консольный UI смотрят с дивана — сразу берём крупную шкалу.
    t.type = Typography::switchUi();
    t.shapes = Shapes{};
    t.dark = dark;
    return t;
}

} // namespace tnx::md3