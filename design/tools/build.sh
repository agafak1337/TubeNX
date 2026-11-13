#!/usr/bin/env bash
# Пересобирает токены из HCT-движка приложения: tokens.json + assets/tokens.css.
set -e
cd "$(dirname "$0")/../.."
TMP="${TMPDIR:-/tmp}/tubenx-tokens"
mkdir -p "$TMP"
g++ -std=c++17 -O2 -I source design/tools/dump_scheme.cpp source/md3/theme.cpp -o "$TMP/dump"
for s in C3002F 1A73E8 1E8E3E 7E57C2 E8710A D81B60; do "$TMP/dump" "$s" > "$TMP/$s.json"; done
node design/tools/build_tokens.mjs "$TMP"
