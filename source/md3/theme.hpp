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

    static Typography standard();      // M3 default type scale
    static Typography switchUi();      // чуть крупнее — читаемость с дивана
};

// Шкала форм M3 (в px для 720p-канваса).
struct Shapes {
    float none = 0.f;
    float extraSmall = 4.f;
    float small = 8.f;
    float medium = 12.f;
    float large = 16.f;
    float extraLarge = 28.f;
    float full = 999.f;
};

// Уровни elevation (тень: blur, dy, alpha).
struct Elevation {
    float blur, dy, alpha;
};
Elevation elevationFor(int level);

struct Theme {
    ColorScheme colors;
    Typography type;
    Shapes shapes;
    bool dark = true;

    static Theme make(uint32_t seedArgb, bool dark);
};

} // namespace tnx::md3