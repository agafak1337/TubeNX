#!/usr/bin/env bash
# tools/build.sh — сборка .nro одной командой (RB-06).
#
# Что делает: находит devkitPro (если DEVKITPRO не задан), добавляет его
# инструменты в PATH, запускает make -j, складывает лог в logs/build.log,
# печатает число предупреждений, размер и sha256 готового .nro.
#
# Использование:
#   tools/build.sh            # обычная сборка
#   JOBS=4 tools/build.sh     # ограничить параллельность
#   DIAG=1 tools/build.sh     # диагностическая сборка (DEBUG-лог)
#   tools/build.sh clean      # предварительно make clean
#
# Коды выхода: 0 — собрано; 1 — ошибка сборки; 2 — нет toolchain.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

# devkitPro: переменная окружения важнее, иначе ищем типовые пути.
if [ -z "${DEVKITPRO:-}" ]; then
    for cand in /opt/devkitpro "$HOME/devkitpro" "$HOME/devkitpro-root/opt/devkitpro"; do
        if [ -d "$cand/libnx" ]; then
            export DEVKITPRO="$cand"
            break
        fi
    done
fi
if [ -z "${DEVKITPRO:-}" ] || [ ! -d "$DEVKITPRO/libnx" ]; then
    echo "DEVKITPRO не найден. Установите devkitPro и задайте DEVKITPRO=.../devkitpro" >&2
    echo "Инструкция: tools/toolchain.md" >&2
    exit 2
fi
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"

jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
mkdir -p logs
log="logs/build.log"

if [ "${1:-}" = "clean" ]; then
    echo "make clean ..."
    make clean >/dev/null 2>&1 || true
fi

# DIAG=1 собирает отдельный .nro с включённым DEBUG-логом (make diag).
target=""
out="TubeNX.nro"
if [ "${DIAG:-0}" = "1" ]; then
    target="diag"
    out="TubeNX-diag.nro"
fi

echo "DEVKITPRO=$DEVKITPRO"
echo "make -j$jobs ${target:+(цель: $target)} (лог: $log)"
# shellcheck disable=SC2086
make -j"$jobs" $target > "$log" 2>&1
rc=$?

warn=$(grep -c "warning:" "$log" 2>/dev/null || true)
errs=$(grep -c "error:" "$log" 2>/dev/null || true)
echo "предупреждений: $warn, ошибок: $errs"

if [ "$rc" -ne 0 ] || [ ! -f "$out" ]; then
    echo "СБОРКА НЕ УДАЛАСЬ (код $rc). Последние строки:" >&2
    tail -n 25 "$log" >&2
    exit 1
fi

size=$(stat -c %s "$out" 2>/dev/null || echo 0)
if [ "$size" -lt 1048576 ]; then
    echo "Внимание: $out меньше 1 МБ ($size Б) — похоже на битую сборку." >&2
    exit 1
fi
echo "готово: $out ($size байт)"
sha256sum "$out" | tee -a "$log"
echo
echo "Дальше: tools/deploy.sh (на SD) или tools/pull-log.sh (забрать лог)."