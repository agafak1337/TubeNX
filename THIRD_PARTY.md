# Сторонние компоненты TubeNX

Дата инвентаризации: 2026-11-14.

TubeNX — MIT-проект (см. `LICENSE`), но **итоговый `.nro` статически содержит
GPL-компоненты** (FFmpeg, libmpv). Это не делает исходники TubeNX GPL, но
накладывает на распространение бинарника обязательства GPL-2.0+: получатель
должен иметь доступ к соответствующим исходникам и тексту лицензии. Подробнее —
в `NOTICE`.

## Зависимости

| Компонент | Версия | Лицензия | Источник | Где используется |
|---|---|---|---|---|
| **libnx** | 4.12.0 | MIT | https://github.com/switchbrew/libnx | платформа, ввод, framebuffer, applet |
| **devkitPro / devkitA64** | GCC 15.2.0 | MIT / BSD-подобная | https://devkitpro.org | тулчейн |
| **libmpv** | 0.39.0 (`switch-libmpv`) | GPL-2.0+ (сборка devkitPro) | https://mpv.io | воспроизведение видео/аудио |
| **FFmpeg** | 7.1 (`switch-ffmpeg`, `--enable-gpl`) | GPL-2.0+ | https://ffmpeg.org | демуксинг, декодирование, ресемплинг |
| **libcurl** | 7.69.1 (`switch-curl`) | curl (MIT-подобная) | https://curl.se | HTTPS, SOCKS5 |
| **mbedTLS** | из portlibs Switch | Apache-2.0 | https://tls.mbed.org | TLS для libcurl/libnx |
| **Jansson** | 2.11 (`switch-jansson`) | MIT | https://github.com/akheron/jansson | разбор JSON InnerTube |
| **SDL2** | 2.28.5 (`switch-sdl2`) | zlib | https://libsdl.org | транзитивная зависимость libmpv |
| **libass** | из portlibs Switch | ISC | https://github.com/libass/libass | субтитры внутри mpv |
| **FreeType** | из portlibs Switch | FTL / GPL-2.0 (выбрана FTL) | https://freetype.org | шрифты в libass |
| **stb_image** | 2.32 | public domain / MIT | https://github.com/nothings/stb | загрузка PNG/JPEG обложек и иконок |
| **stb_image_write** | 1.18 | public domain / MIT | https://github.com/nothings/stb | только host-preview (PNG) |
| **stb_truetype** | 1.28 | public domain / MIT | https://github.com/nothings/stb | растеризация глифов |
| **Google Sans** | 14.000 | SIL Open Font License 1.1 | https://github.com/googlefonts/googlesans | основной шрифт UI |
| **Material Symbols** | 2.983 (сабсет) | Apache-2.0 | https://fonts.google.com/icons | 63 иконки |
| **CA-бандл** | Mozilla CA bundle | MPL-2.0 (данные) | https://curl.se/docs/caextract.html | `resources/cacert.pem`, `cacert-trim.pem` |

## Тексты лицензий в репозитории

| Файл | Что покрывает |
|---|---|
| `LICENSE` | код TubeNX — MIT |
| `resources/fonts/LICENSE-OFL.txt` | Google Sans — SIL OFL 1.1 |
| `resources/fonts/LICENSE` | Material Symbols — Apache License 2.0 |
| `NOTICE` | сводка по GPL-компонентам в бинарнике |

## Происхождение шрифтов (SEC-05)

Проверены `name`-таблицы TTF:

- `GoogleSans-Regular/Medium/Bold.ttf` — `Version 14.000`,
  `Copyright 2025 The Google Sans Project Authors`
  (`github.com/googlefonts/googlesans`), лицензия **SIL OFL 1.1**.
  OFL разрешает включение шрифта в программу и распространение вместе с ней;
  шрифт не продаётся отдельно, изменённые версии должны сохранять имя либо
  явно указывать изменения.
- `MaterialSymbols.ttf` — сабсет официального **Material Symbols**, лицензия
  **Apache-2.0**; текст лежит в `resources/fonts/LICENSE`, уведомление об
  изменениях — в `source/md3/icons.hpp` (сгенерирован `tools/gen_icons.py`).

Оба шрифта совместимы с MIT-кодом проекта. Ранее в README было сказано
«собственность Google, условия Google Fonts» — это неточно и заменено ссылкой
на этот файл.

## Что важно для релиза

1. Бинарник `.nro` содержит GPL-2.0+ код (FFmpeg/libmpv). При публикации
   артефакта нужно приложить `LICENSE`, `NOTICE` и ссылку на исходники ровно
   той ревизии, из которой собран бинарник.
2. Исходники самого TubeNX остаются MIT: MIT совместим с GPL-2.0+.
3. Портлибы devkitPro не входят в репозиторий; их исходники доступны у
   devkitPro и апстримов, перечисленных выше.
4. `resources/cacert.pem` — запасной полный бандл; в обычной работе
   используется урезанный `cacert-trim.pem`.