#!/usr/bin/env bash
# Рендер SVG в PNG точного размера через headless Firefox (SVG рисуется в нужном размере, без уменьшения).
#   render_icon.sh <in.svg> <out.png> <size> [css-фон страницы, по умолчанию #000]
set -e
SVG="$(realpath "$1")"; OUT="$(realpath -m "$2")"; S="$3"; BG="${4:-#000}"
TMP="$(mktemp -d)"
cat > "$TMP/p.html" <<EOF
<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;padding:0;background:$BG;overflow:hidden}img{display:block;width:${S}px;height:${S}px}</style></head><body><img src="file://$SVG"></body></html>
EOF
mkdir -p "$(dirname "$OUT")" "$TMP/prof"
firefox --headless --no-remote --profile "$TMP/prof" --window-size="$S,$S" --screenshot "$OUT" "file://$TMP/p.html" >/dev/null 2>&1
rm -rf "$TMP"
echo "$OUT"
