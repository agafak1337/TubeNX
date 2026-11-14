// core/layout.hpp — режим вывода и параметры кадра (docs/impl-tz.md §5.1, §5.9).
//
// Логический канвас всегда 1280×720. Handheld рисует его 1:1 в буфер
// 1280×720, dock — с масштабом 1,5 в буфер 1920×1080. Экраны про dock не
// знают: они работают в логических координатах, а масштаб применяет Canvas.
#pragma once

#include "core/canvas.hpp"

namespace tnx {

struct DisplayLayout {
    bool dock = false;
    bool fallback = false;      // dock запрошен, но 1920×1080 недоступен
    int fbWidth = 1280;         // физический framebuffer
    int fbHeight = 720;
    float scale = 1.f;          // логические px → физические
    Insets safe{};              // безопасные поля, логические px

    static DisplayLayout handheld() { return DisplayLayout{}; }

    static DisplayLayout dockLayout() {
        DisplayLayout l;
        l.dock = true;
        l.fbWidth = 1920;
        l.fbHeight = 1080;
        l.scale = 1.5f;
        // Поля 64, верхний/нижний резерв 36 (в логических px; физически 96/54).
        l.safe = Insets{64.f, 36.f, 64.f, 36.f};
        return l;
    }

    // 1920×1080 не создался: рисуем канвас 1280×720 как есть, композитор
    // растянет его на телевизор. Раскладка остаётся dock-овой (поля 64/36).
    static DisplayLayout dockFallback() {
        DisplayLayout l;
        l.dock = true;
        l.fallback = true;
        l.safe = Insets{64.f, 36.f, 64.f, 36.f};
        return l;
    }

    float logicalWidth() const { return (float)fbWidth / scale; }
    float logicalHeight() const { return (float)fbHeight / scale; }
};

// Раскладка экрана в логических px (docs/impl-tz.md Р-03, §5.9).
// Экраны должны читать её вместо жёстких «поля 24 / 3 колонки»:
//   auto L = tnx::ScreenLayout::forCanvas(u.canvas());
//   L.margin, L.cols, L.cardW, L.safe ...
// Handheld: поля 24, 4×296, зазор 16. Dock: поля 64, 4×276, зазор 16.
struct ScreenLayout {
    float margin = 24.f;   // поля экрана
    float gap = 16.f;      // зазор сетки
    int cols = 3;
    float cardW = 0.f;
    Insets safe{};         // безопасные поля (dock 64/36)

    static ScreenLayout forCanvas(const Canvas& c) {
        ScreenLayout l;
        l.safe = c.safeInsets();
        const float w = c.logicalWidth();
        // Dock узнаётся по масштабу канваса (1,5), а не по флагам платформы:
        // экраны не должны знать про appletGetOperationMode(). Поля берём из
        // safe area (dock 64), иначе — handheld 24.
        l.margin = (l.safe.l > 0.f) ? l.safe.l : 24.f;
        l.cols = 4;
        l.cardW = (w - 2.f * l.margin - (float)(l.cols - 1) * l.gap) / (float)l.cols;
        return l;
    }
};

} // namespace tnx