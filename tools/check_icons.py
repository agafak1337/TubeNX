#!/usr/bin/env python3
"""check_icons.py — полнота иконок (M3-06).

Проверяет:
  1) все имена иконок, которые использует код, есть в source/md3/icons.hpp;
  2) каждый кодпоинт из icons.hpp присутствует в сабсете
     resources/fonts/MaterialSymbols.ttf (иначе на экране будет квадратик).

Запуск:  python3 tools/check_icons.py
Возврат: 0 — всё найдено, 1 — есть missing.
"""
import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "source", "md3", "icons.hpp")
FONT = os.path.join(ROOT, "resources", "fonts", "MaterialSymbols.ttf")


def read_icons():
    icons = {}
    for line in open(HEADER, encoding="utf-8"):
        m = re.search(r'\{"([a-z0-9_]+)",\s*(0x[0-9a-fA-F]+)\}', line)
        if m:
            icons[m.group(1)] = int(m.group(2), 16)
    return icons


def cmap_lookup(font_path):
    """Возвращает функцию cp -> glyph_id (0 = нет глифа)."""
    d = open(font_path, "rb").read()
    num_tables = struct.unpack_from(">H", d, 4)[0]
    cmap_off = None
    for i in range(num_tables):
        off = 12 + i * 16
        tag = d[off:off + 4]
        if tag == b"cmap":
            cmap_off = struct.unpack_from(">I", d, off + 8)[0]
            break
    if cmap_off is None:
        raise RuntimeError("в шрифте нет таблицы cmap")

    n = struct.unpack_from(">H", d, cmap_off + 2)[0]
    best = None
    for i in range(n):
        rec = cmap_off + 4 + i * 8
        plat, enc = struct.unpack_from(">HH", d, rec)
        sub = cmap_off + struct.unpack_from(">I", d, rec + 4)[0]
        fmt = struct.unpack_from(">H", d, sub)[0]
        # 3/10 (format 12) предпочтительнее, затем 3/1 (format 4).
        score = {(3, 10): 3, (0, 4): 3, (3, 1): 2, (0, 3): 2}.get((plat, enc), 0)
        if fmt in (4, 12) and score and (best is None or score > best[0]):
            best = (score, fmt, sub)

    if best is None:
        raise RuntimeError("нет поддерживаемой cmap-подтаблицы")

    _, fmt, sub = best
    if fmt == 4:
        seg_x2 = struct.unpack_from(">H", d, sub + 6)[0]
        seg = seg_x2 // 2
        end = sub + 14
        start = end + seg_x2 + 2
        delta = start + seg_x2
        range_off = delta + seg_x2

        def lookup(cp):
            if cp > 0xFFFF:
                return 0
            for i in range(seg):
                e = struct.unpack_from(">H", d, end + i * 2)[0]
                if cp > e:
                    continue
                s = struct.unpack_from(">H", d, start + i * 2)[0]
                if cp < s:
                    return 0
                ro = struct.unpack_from(">H", d, range_off + i * 2)[0]
                if ro == 0:
                    g = (cp + struct.unpack_from(">h", d, delta + i * 2)[0]) & 0xFFFF
                else:
                    addr = range_off + i * 2 + ro + (cp - s) * 2
                    g = struct.unpack_from(">H", d, addr)[0]
                    if g:
                        g = (g + struct.unpack_from(">h", d, delta + i * 2)[0]) & 0xFFFF
                return g
            return 0

        return lookup

    n_groups = struct.unpack_from(">I", d, sub + 12)[0]
    groups = sub + 16

    def lookup12(cp):
        for i in range(n_groups):
            s, e, g = struct.unpack_from(">III", d, groups + i * 12)
            if s <= cp <= e:
                return g + (cp - s)
        return 0

    return lookup12


def used_icon_names():
    """Имена-литералы рядом с вызовами иконок в исходниках."""
    funcs = ("md3::icon(", "md3::iconButton(", "md3::iconCentered(", "md3::fab(",
             "iconName", "md3::listItem(", "md3::navigationBar(")
    used = set()
    for f in glob.glob(os.path.join(ROOT, "source", "**", "*.cpp"), recursive=True):
        src = open(f, encoding="utf-8", errors="replace").read()
        for fn in funcs:
            idx = 0
            while True:
                i = src.find(fn, idx)
                if i < 0:
                    break
                for m in re.finditer(r'"([a-z][a-z0-9_]{1,30})"', src[i:i + 300]):
                    used.add(m.group(1))
                idx = i + len(fn)
    return used


def main() -> int:
    icons = read_icons()
    if not icons:
        print("ОШИБКА: не разобрал source/md3/icons.hpp")
        return 1
    fails = []

    for name in sorted(used_icon_names() - set(icons)):
        fails.append(f"иконка «{name}» используется в коде, но её нет в icons.hpp")

    if not os.path.exists(FONT):
        fails.append("нет resources/fonts/MaterialSymbols.ttf")
    else:
        lookup = cmap_lookup(FONT)
        for name, cp in sorted(icons.items()):
            if lookup(cp) == 0:
                fails.append(f"«{name}» (U+{cp:04X}) отсутствует в шрифте")

    if fails:
        print(f"== ЕСТЬ ПРОБЛЕМЫ ({len(fails)}) ==")
        for f in fails:
            print(" -", f)
        return 1
    print(f"== ЗЕЛЕНО: {len(icons)} иконок, все есть в шрифте и в коде ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())