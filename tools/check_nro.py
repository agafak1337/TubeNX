#!/usr/bin/env python3
"""check_nro.py — проверка NACP и иконки в собранном .nro (PL-09).

Что проверяет:
  * в NRO есть asset-секция ASET;
  * иконка — настоящий JPEG 256×256 (hbmenu декодирует её как JPEG);
  * NACP: непустые название и автор, версия совпадает с APP_VERSION из Makefile;
  * romfs вложен.

Запуск:  python3 tools/check_nro.py [TubeNX.nro]
Возврат: 0 — всё в порядке, 1 — есть проблемы.
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_NRO = os.path.join(ROOT, "TubeNX.nro")
EXPECT_TITLE = "TubeNX"
EXPECT_AUTHOR = "agafak1337"


def jpeg_size(data: bytes):
    """Возвращает (w, h) из SOF-маркера JPEG или None."""
    if data[:3] != b"\xff\xd8\xff":
        return None
    i = 2
    n = len(data)
    while i + 9 < n:
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        i += 2
        if marker in (0xD8, 0xD9):  # SOI/EOI без длины
            continue
        if i + 2 > n:
            break
        seg_len = struct.unpack_from(">H", data, i)[0]
        # SOF0..SOF15, кроме DHT(C4), JPG(C8), DAC(CC)
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            h, w = struct.unpack_from(">HH", data, i + 3)
            return w, h
        i += seg_len
    return None


def app_version():
    try:
        for line in open(os.path.join(ROOT, "Makefile"), encoding="utf-8"):
            m = re.match(r"\s*APP_VERSION\s*:?=\s*(\S+)", line)
            if m:
                return m.group(1)
    except OSError:
        pass
    return None


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_NRO
    if not os.path.exists(path):
        print(f"НЕТ ФАЙЛА: {path}")
        return 1
    d = open(path, "rb").read()
    if len(d) < 0x20 or d[0x10:0x14] != b"NRO0":
        print("ОШИБКА: это не NRO (нет магии NRO0)")
        return 1

    # Asset-заголовок обычно лежит после кода; ищем по магии ASET.
    i = d.find(b"ASET")
    if i < 0:
        print("ОШИБКА: нет asset-секции ASET — иконка/NACP не вложены")
        return 1
    magic, version = struct.unpack_from("<II", d, i)
    icon_off, icon_size = struct.unpack_from("<QQ", d, i + 8)
    nacp_off, nacp_size = struct.unpack_from("<QQ", d, i + 0x18)
    romfs_off, romfs_size = struct.unpack_from("<QQ", d, i + 0x28)

    fails = []
    print(f"ASET: version={version}, icon={icon_size} Б, nacp={nacp_size} Б, romfs={romfs_size} Б")

    # --- иконка ---
    if icon_size == 0:
        fails.append("иконка не вложена")
    else:
        icon = d[i + icon_off:i + icon_off + icon_size]
        size = jpeg_size(icon)
        if size is None:
            fails.append("иконка не JPEG (hbmenu покажет заглушку)")
        elif size != (256, 256):
            fails.append(f"иконка {size[0]}×{size[1]}, нужна 256×256")
        else:
            print("иконка: JPEG 256×256 — ок")

    # NACP: 16 языковых записей по 0x300 (0x200 имя + 0x100 автор),
    # display_version — на 0x3060 (см. NacpStruct в libnx).
    if nacp_size < 0x3080:
        fails.append("NACP пустой/битый")
    else:
        n = d[i + nacp_off:i + nacp_off + 0x3080]

        def field(off, ln):
            raw = n[off:off + ln].split(b"\0", 1)[0]
            return raw.decode("utf-8", "replace").strip()

        title, author, ver = field(0, 0x200), field(0x200, 0x100), field(0x3060, 0x10)
        print(f"NACP: title={title!r} author={author!r} version={ver!r}")
        if not title:
            fails.append("пустое название в NACP")
        elif title != EXPECT_TITLE:
            fails.append(f"название {title!r}, ожидалось {EXPECT_TITLE!r}")
        if not author:
            fails.append("пустой автор в NACP")
        elif author != EXPECT_AUTHOR:
            fails.append(f"автор {author!r}, ожидался {EXPECT_AUTHOR!r}")
        want = app_version()
        if not ver:
            fails.append("пустая версия в NACP")
        elif want and ver != want:
            fails.append(f"версия {ver!r} ≠ APP_VERSION {want!r}")

    if romfs_size == 0:
        fails.append("romfs не вложен (шрифты/CA не найдутся)")

    if fails:
        print("\n== ЕСТЬ ПРОБЛЕМЫ ==")
        for f in fails:
            print(" -", f)
        return 1
    print("\n== ЗЕЛЕНО: NACP и иконка в порядке ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())