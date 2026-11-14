#!/usr/bin/env python3
"""gen_icons.py — генерация сабсета Material Symbols и source/md3/icons.hpp.

Что делает:
  1. скачивает список codepoint'ов Material Symbols (variable font repo);
  2. запрашивает у Google Fonts сабсет ровно с нужными иконками;
  3. кладёт его в resources/fonts/MaterialSymbols.ttf;
  4. пишет source/md3/icons.hpp (имя → кодпоинт + iconCodepoint()).

Запускается вручную при добавлении иконок (нужен интернет):
    python3 tools/gen_icons.py
"""
import os
import re
import sys
import urllib.request

ICONS = """
account_circle add arrow_back arrow_forward arrow_upward bolt cast check chevron_left
chevron_right close cloud_download cloud_off dark_mode delete download error expand_more
filter_list forward_10 fullscreen fullscreen_exit gpp_good high_quality history home
info keyboard_arrow_up language light_mode live_tv lock logout menu more_vert movie
music_note open_in_new pause person play_arrow playlist_play refresh replay replay_10
restart_alt schedule search settings share skip_next skip_previous sort speed sports
subscriptions subtitles sync thumb_down thumb_up trending_up tune verified volume_off
volume_up vpn_key wifi wifi_off
""".split()

CODEPOINTS_URL = (
    "https://raw.githubusercontent.com/google/material-design-icons/master/"
    "variablefont/MaterialSymbolsOutlined%5BFILL%2CGRAD%2Copsz%2Cwght%5D.codepoints"
)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UA = {"User-Agent": "curl/7.68.0"}


def fetch(url: str, timeout: int = 60) -> bytes:
    req = urllib.request.Request(url, headers=UA)
    return urllib.request.urlopen(req, timeout=timeout).read()


def main() -> int:
    icons = sorted(set(ICONS))
    print(f"иконок в списке: {len(icons)}")

    codepoints = {}
    for line in fetch(CODEPOINTS_URL).decode().splitlines():
        parts = line.split()
        if len(parts) == 2:
            codepoints[parts[0]] = parts[1]

    missing = [i for i in icons if i not in codepoints]
    if missing:
        print(f"ОШИБКА: неизвестные имена: {' '.join(missing)}", file=sys.stderr)
        return 1

    url = (
        "https://fonts.googleapis.com/css2?family=Material+Symbols+Outlined&icon_names="
        + ",".join(icons)
    )
    css = fetch(url).decode()
    m = re.search(r"url\((https://[^)]+)\)", css)
    if not m:
        print("ОШИБКА: Google Fonts не вернул URL шрифта", file=sys.stderr)
        return 1
    data = fetch(m.group(1))
    if data[:4] != b"\x00\x01\x00\x00":
        print(f"ОШИБКА: ожидался TrueType, получено {data[:4]!r}", file=sys.stderr)
        return 1

    font_path = os.path.join(ROOT, "resources", "fonts", "MaterialSymbols.ttf")
    with open(font_path, "wb") as f:
        f.write(data)

    header = os.path.join(ROOT, "source", "md3", "icons.hpp")
    with open(header, "w", encoding="utf-8") as f:
        f.write("// md3/icons.hpp — АВТОГЕНЕРАЦИЯ: имена Material Symbols → кодпоинты.\n")
        f.write("// Файл создан tools/gen_icons.py; шрифт — resources/fonts/MaterialSymbols.ttf (сабсет).\n")
        f.write("#pragma once\n#include <cstdint>\n#include <cstring>\n\nnamespace tnx::md3 {\n\n")
        f.write("struct IconEntry { const char* name; uint32_t cp; };\n")
        f.write("inline const IconEntry kIcons[] = {\n")
        for name in icons:
            f.write('    {"%s", 0x%s},\n' % (name, codepoints[name]))
        f.write("};\n\n")
        f.write("inline uint32_t iconCodepoint(const char* name) {\n")
        f.write("    if (!name) return 0;\n")
        f.write("    for (const auto& e : kIcons)\n")
        f.write("        if (std::strcmp(e.name, name) == 0) return e.cp;\n")
        f.write("    return 0;\n}\n\n} // namespace tnx::md3\n")

    print(f"шрифт:  {font_path} ({len(data)} байт)")
    print(f"заголовок: {header} ({len(icons)} иконок)")
    return 0


if __name__ == "__main__":
    sys.exit(main())