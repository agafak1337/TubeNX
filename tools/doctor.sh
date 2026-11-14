#!/usr/bin/env bash
# tools/doctor.sh — печатает версии тулчейна и наличие портлиб (RB-04).
# Сборка не должна зависеть от «у меня стоит»: этот вывод идёт в лог CI.
set -uo pipefail

DEVKITPRO="${DEVKITPRO:-}"
if [ -z "$DEVKITPRO" ]; then
    echo "DEVKITPRO не задан. export DEVKITPRO=<path to>/devkitpro"
    exit 1
fi
echo "DEVKITPRO = $DEVKITPRO"

CC="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc"
if [ -x "$CC" ]; then
    echo -n "devkitA64 GCC: "; "$CC" --version | head -1
else
    echo "devkitA64 GCC: НЕ НАЙДЕН ($CC)"; exit 1
fi

echo -n "libnx: "
if [ -f "$DEVKITPRO/libnx/include/switch.h" ]; then echo "present"; else echo "ОТСУТСТВУЕТ"; fi

echo "portlibs (switch):"
# Пакет -> представительная библиотека (ffmpeg ставится набором libav*).
check_lib() {
    local name="$1" probe="$2"
    local f
    f=$(ls "$DEVKITPRO"/portlibs/switch/lib/lib${probe}.* 2>/dev/null | head -1)
    printf '  %-10s %s\n' "$name" "${f:-ОТСУТСТВУЕТ}"
}
check_lib mpv mpv
check_lib curl curl
check_lib jansson jansson
check_lib ffmpeg avcodec
check_lib SDL2 SDL2

echo -n "make: "; make --version | head -1
echo -n "git:  "; git --version
if command -v dkp-pacman >/dev/null 2>&1; then
    echo -n "pacman -Q (devkitPro): "; dkp-pacman -Q 2>/dev/null | wc -l
else
    echo "pacman -Q (devkitPro): dkp-pacman не найден"
fi
