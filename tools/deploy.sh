#!/usr/bin/env bash
# tools/deploy.sh — копирование TubeNX.nro на SD через MTP с автоопределением
# точки монтирования (RB-06). При отсутствии SD — понятная ошибка.
set -uo pipefail
cd "$(dirname "$0")/.."

if [ ! -f TubeNX.nro ]; then
    echo "TubeNX.nro не собран. Сначала: make -j\$(nproc)"
    exit 1
fi

MNT=""
# Самый частый путь: gvfs-моноunt с MTP-хостом Switch (DBI/Atmosphere).
while IFS= read -r cand; do
    [ -d "$cand/switch" ] && MNT="$cand" && break
done < <(find /run/user/"$(id -u)"/gvfs /media /mnt -maxdepth 3 -type d \
         \( -name 'mtp:*' -o -name '*SD*' -o -name '*SD Card*' \) 2>/dev/null)

if [ -z "$MNT" ]; then
    echo "SD не найдена. Подключите консоль по MTP (DBI) и повторите."
    echo "Подсказка: путь обычно /run/user/\$(id -u)/gvfs/mtp:host=*-*/1: SD Card"
    exit 1
fi

echo "SD: $MNT"
mkdir -p "$MNT/switch/TubeNX"
cp -f TubeNX.nro "$MNT/switch/TubeNX.nro"
sha256sum TubeNX.nro | tee "$MNT/switch/TubeNX.nro.sha256"
echo "Скопировано на $MNT/switch/TubeNX.nro"
