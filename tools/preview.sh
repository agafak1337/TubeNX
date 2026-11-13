#!/usr/bin/env bash
# Сборка offscreen-предпросмотра UI (только для разработки, на Linux-хосте).
set -e
cd "$(dirname "$0")/.."
OUT="${1:-/tmp/tubenx}"
mkdir -p "$OUT"

# Пути к шрифтам: и компилятору (не нужно), и рантайму (нужно) — см. core/font.cpp.
export TUBENX_FONTS="$(pwd)/resources/fonts"

g++ -std=c++17 -O2 -g -I source -I tools \
    tools/preview.cpp tools/host_stubs.cpp \
    source/core/canvas.cpp source/core/font.cpp source/core/threads.cpp \
    source/md3/ui.cpp source/md3/theme.cpp source/md3/widgets.cpp \
    source/ui/app.cpp source/ui/screens.cpp \
    -o /tmp/tubenx_preview -lpthread

/tmp/tubenx_preview "$OUT"