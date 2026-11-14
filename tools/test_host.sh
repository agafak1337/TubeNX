#!/usr/bin/env bash
# tools/test_host.sh — host-тесты без консоли (RB-07, TEST-01). Собирает и
# запускает offline-тесты; код возврата 0 — всё зелёное. CI вызывает make test-host.
#
# Не требует Switch и (кроме test_innertube) внешних библиотек.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT="${TMPDIR:-/tmp}/tubenx-host"
mkdir -p "$OUT"
# ВАЖНО: не используем внешний CXX. devkitPro base_tools экспортирует
# CXX=aarch64-none-elf-g++, и тогда host-тесты собираются под ARM.
CC_HOST="${HOST_CXX:-g++}"
CXX="$CC_HOST"
BASE="-std=c++17 -O2 -g -Isource -Itools -Wall"
DEVKITPRO="${DEVKITPRO:-}"

fail=0
step() { printf '\n== %s ==\n' "$1"; }
ok()   { printf '   OK: %s\n' "$1"; }
bad()  { printf '   FAIL: %s\n' "$1"; fail=1; }

# 1. Движок HCT/CAM16, типографика, elevation (существующий тест).
step "test_hct — HCT/CAM16, типографика, формы"
if $CXX $BASE tools/test_hct.cpp source/md3/theme.cpp -o "$OUT/test_hct" 2> "$OUT/test_hct.build.log"; then
    if "$OUT/test_hct" > "$OUT/test_hct.log" 2>&1; then ok "test_hct"; else bad "test_hct (см. $OUT/test_hct.log)"; fi
else
    bad "test_hct: сборка (см. $OUT/test_hct.build.log)"
fi

# 2. TEST-03: токены = design/tokens.json.
step "test_theme — токены против design/tokens.json (TEST-03)"
if $CXX $BASE tools/test_theme.cpp source/md3/theme.cpp -o "$OUT/test_theme" 2> "$OUT/test_theme.build.log"; then
    if "$OUT/test_theme" design/tokens.json | tee "$OUT/test_theme.log"; then ok "test_theme"; else bad "test_theme"; fi
else
    bad "test_theme: сборка (см. $OUT/test_theme.build.log)"
fi

# 3. TEST-01: парсер InnerTube на фикстуре (нужен jansson-заголовок).
step "test_innertube — парсер ленты/поиска (TEST-01)"
JANSSON_INC="$DEVKITPRO/portlibs/switch/include/jansson.h"
if [ -n "$DEVKITPRO" ] && [ -f "$JANSSON_INC" ]; then
    if $CXX $BASE -DTNX_HOST -I"$DEVKITPRO/portlibs/switch/include" \
        tools/test_innertube.cpp source/yt/innertube.cpp \
        -o "$OUT/test_innertube" -l:libjansson.so.4 2> "$OUT/test_innertube.build.log"; then
        if "$OUT/test_innertube" tools/sample_yt.json > "$OUT/test_innertube.log" 2>&1; then
            ok "test_innertube"
        else
            bad "test_innertube (см. $OUT/test_innertube.log)"
        fi
    else
        bad "test_innertube: сборка (см. $OUT/test_innertube.build.log)"
    fi
else
    printf '   SKIP: jansson.h не найден (DEVKITPRO=%s)\n' "${DEVKITPRO:-не задан}"
fi

# 4. DPI-01..05: фрагментация TLS, SOCKS5 и ретранслятор (offline, -DTNX_HOST).
step "test_dpi — фрагментация, SOCKS, ретранслятор (DPI-01..05)"
if $CXX $BASE -DTNX_HOST tools/test_dpi.cpp source/dpi/dpi.cpp \
    -o "$OUT/test_dpi" -lpthread 2> "$OUT/test_dpi.build.log"; then
    if "$OUT/test_dpi" > "$OUT/test_dpi.log" 2>&1; then ok "test_dpi"; else bad "test_dpi (см. $OUT/test_dpi.log)"; fi
else
    bad "test_dpi: сборка (см. $OUT/test_dpi.build.log)"
fi

# 5. NET-01..06/09/14 + PERF-03: заголовки, бюджеты, обложки, LRU (curl/curl.h).
step "test_net — заголовки, бюджеты, обложки, LRU кэша (NET/PERF-03)"
CURL_INC=""
if [ ! -f /usr/include/curl/curl.h ] && [ ! -f /usr/include/x86_64-linux-gnu/curl/curl.h ]; then
    # Системных dev-заголовков нет — берём хостовый сискрут (если есть).
    if [ -f /tmp/opencode/hostdev/usr/include/x86_64-linux-gnu/curl/curl.h ]; then
        CURL_INC="-I/tmp/opencode/hostdev/usr/include -I/tmp/opencode/hostdev/usr/include/x86_64-linux-gnu"
    fi
fi
if printf '#include <curl/curl.h>\n' | $CXX -E -x c++ - $CURL_INC >/dev/null 2>&1; then
    if $CXX $BASE -DTNX_HOST $CURL_INC tools/test_net.cpp source/net/http.cpp source/net/images.cpp \
        -o "$OUT/test_net" -lpthread -l:libcurl.so.4 2> "$OUT/test_net.build.log"; then
        if "$OUT/test_net" > "$OUT/test_net.log" 2>&1; then ok "test_net"; else bad "test_net (см. $OUT/test_net.log)"; fi
    else
        bad "test_net: сборка (см. $OUT/test_net.build.log)"
    fi
else
    printf '   SKIP: curl/curl.h не найден (нет dev-заголовков libcurl)\n'
fi

# 6. preview генерирует экраны в PNG.
step "preview — offscreen-рендер экранов"
if bash tools/preview.sh "$OUT/preview" > "$OUT/preview.log" 2>&1; then
    n=$(ls "$OUT"/preview/*.png 2>/dev/null | wc -l)
    if [ "$n" -ge 7 ]; then ok "preview: $n PNG"; else bad "preview: всего $n PNG (ожидалось ≥ 7)"; fi
else
    bad "preview (см. $OUT/preview.log)"
fi

printf '\n== ИТОГ: %s ==\n' "$([ $fail -eq 0 ] && echo ЗЕЛЁНО || echo ЕСТЬ ОШИБКИ)"
exit $fail
