#!/usr/bin/env bash
# tools/pull-log.sh — забрать свежий лог с SD в logs/ (RB-06, TEST-07).
# Ищет новый layout (sdmc:/switch/TubeNX/logs/tubenx-*.log) и старый
# TubeNX-boot.log, копирует последний в текущее дерево.
set -uo pipefail
cd "$(dirname "$0")/.."

MNT=""
while IFS= read -r cand; do
    [ -d "$cand/switch" ] && MNT="$cand" && break
done < <(find /run/user/"$(id -u)"/gvfs /media /mnt -maxdepth 3 -type d \
         \( -name 'mtp:*' -o -name '*SD*' -o -name '*SD Card*' \) 2>/dev/null)

if [ -z "$MNT" ]; then
    echo "SD не найдена (MTP). Подключите консоль и повторите."
    exit 1
fi

out="logs"
mkdir -p "$out"
picked=""
# Новейший лог из logs/.
newest=$(ls -1t "$MNT"/switch/TubeNX/logs/tubenx-*.log 2>/dev/null | head -1)
if [ -n "$newest" ]; then
    picked="$out/hardware-$(basename "$newest")"
    cp -f "$newest" "$picked"
elif [ -f "$MNT/switch/TubeNX-boot.log" ]; then
    picked="$out/hardware-boot.log"
    cp -f "$MNT/switch/TubeNX-boot.log" "$picked"
fi

if [ -z "$picked" ]; then
    echo "Логов на SD не найдено."
    exit 1
fi
echo "Сохранено: $picked"
tail -n 40 "$picked"
