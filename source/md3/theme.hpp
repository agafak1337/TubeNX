// md3/theme.hpp — Material 3 (Material You): динамические цветовые роли, типографика, формы.
#pragma once

#include "core/types.hpp"

namespace tnx::md3 {

// Полный набор ролей M3 (light/dark генерируются из seed-цвета через HCT).
struct ColorScheme {
    Color primary, onPrimary, primaryContainer, onPrimaryContainer;
    Color secondary, onSecondary, secondaryContainer, onSecondaryContainer;
    Color tertiary, onTertiary, tertiaryContainer, onTertiaryContainer;
    Color error, onError, errorContainer, onErrorContainer;
    Color background, onBackground;
    Color surface, onSurface, surfaceVariant, onSurfaceVariant;
    Color surfaceDim, surfaceBright;
    Color surfaceContainerLowest, surfaceContainerLow, surfaceContainer;
    Color surfaceContainerHigh, surfaceContainerHighest;
    Color outline, outlineVariant;
    Color shadow, scrim;
    Color inverseSurface, inverseOnSurface, inversePrimary;
    Color surfaceTint;
    bool dark = true;
};

// Генерация схемы из seed (как в Material You / Monet).
ColorScheme schemeFromSeed(uint32_t seedArgb, bool dark);
// Дефолтная схема: YouTube-красный как seed, но в стиле M3.
ColorScheme defaultScheme(bool dark);

enum class FontRole { Regular = 0, Medium = 1, Bold = 2, Symbols = 3 };

struct TextStyle {
    FontRole font = FontRole::Regular;
    float size = 16.f;         // sp (px при 720p ~ 1.0)
    float lineHeight = 24.f;
    float letterSpacing = 0.f; // px
};

struct Typography {
    TextStyle displayLarge, displayMedium, displaySmall;
    TextStyle headlineLarge, headlineMedium, headlineSmall;
    TextStyle titleLarge, titleMedium, titleSmall;
    TextStyle bodyLarge, bodyMedium, bodySmall;
    TextStyle labelLarge, labelMedium, labelSmall;

    // Шкала M3 ровно по docs/impl-tz.md §5.2 и design/tokens.json.
    // Масштаб dock даёт физический кегль на канвасе, логический size не растёт.
    static Typography standard();
};

// Шкала форм M3 (в px для 720p-канваса), docs/impl-tz.md §5.2.
struct Shapes {
    float none = 0.f;
    float extraSmall = 4.f;
    float small = 8.f;
    float medium = 12.f;
    float large = 16.f;
    float largeIncreased = 20.f;
    float extraLarge = 28.f;
    float full = 999.f;
    float badge = 6.f;
};

// Уровни elevation (тень: blur, dy, alpha).
struct Elevation {
    float blur, dy, alpha;
};
Elevation elevationFor(int level);

// M3-08: единые токены движения (design/tokens.json → "motion"). Длительности
// в секундах; кривые — cubic-bezier(x1,y1,x2,y2). Пружины — согласованный
// пресет UI (emphasized decelerate), а не магия по месту.
struct Easing {
    float x1, y1, x2, y2;
};
struct Motion {
    Easing emphasized{0.2f, 0.f, 0.f, 1.f};
    Easing emphasizedDecelerate{0.05f, 0.7f, 0.1f, 1.f};
    Easing emphasizedAccelerate{0.3f, 0.f, 0.8f, 0.15f};
    Easing standard{0.2f, 0.f, 0.f, 1.f};
    Easing linear{0.f, 0.f, 1.f, 1.f};

    float short2 = 0.100f;        // 100 мс
    float short4 = 0.200f;        // 200 мс
    float medium1 = 0.250f;       // 250 мс
    float medium2 = 0.300f;       // 300 мс
    float controls = 0.180f;      // 180 мс
    float skeletonPulse = 1.200f; // 1200 мс
    float ripple = 0.450f;        // заливка нажатия

    float springStiffness = 220.f;
    float springDamping = 28.f;
};
inline constexpr Motion motion{};

// M3-03/M3-01: state layer ровно по design/tokens.json → "state".
struct StateLayer {
    float hover = 0.08f;
    float focus = 0.10f;
    float pressed = 0.10f;
    float dragged = 0.16f;
    float disabledContent = 0.38f;
    float disabledContainer = 0.12f;
};
inline constexpr StateLayer stateLayerTokens{};

struct Theme {
    ColorScheme colors;
    Typography type;
    Shapes shapes;
    bool dark = true;

    static Theme make(uint32_t seedArgb, bool dark);
};

} // namespace tnx::md3