#!/usr/bin/env bash
# Рендер фрейма или целой страницы в PNG через headless Firefox.
#   render.sh <page.html> <out.png> [frame-id] [width] [height]
# С frame-id снимается один фрейм (по умолчанию 1280x720), без него — вся доска.
set -e
DESIGN="$(cd "$(dirname "$0")/.." && pwd)"
PAGE="$1"; OUT="$2"; FRAME="${3:-}"; W="${4:-1280}"; H="${5:-720}"
case "$PAGE" in /*) ;; *) PAGE="$DESIGN/$PAGE" ;; esac
case "$OUT" in /*) ;; *) OUT="$DESIGN/$OUT" ;; esac
mkdir -p "$(dirname "$OUT")"
PROFILE="$(mktemp -d)"
URL="file://$PAGE"
[ -n "$FRAME" ] && URL="$URL?frame=$FRAME"
firefox --headless --no-remote --profile "$PROFILE" --window-size="$W,$H" --screenshot "$OUT" "$URL" >/dev/null 2>&1
rm -rf "$PROFILE"
echo "$OUT"
