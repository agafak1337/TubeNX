# Первый день в TubeNX

Пошаговая инструкция для нового участника: от чистого компьютера до
осмысленного первого Pull Request. Рассчитана на 2–3 часа; консоль для
разработки **не нужна**.

Если что-то в инструкции не сходится с реальностью — это баг документации.
Поправь его отдельным PR (задача `DOC-01` в [`docs/tasks.md`](tasks.md)).

---

## 0. Что за проект

TubeNX — неофициальный YouTube-клиент для Nintendo Switch homebrew. C++17,
libnx, libmpv, собственный software-рендерер Material 3. Сборка — `.nro`,
запускается из hbmenu.

Состояние на сегодня:

| Что | Статус |
|---|---|
| Сборка, запуск, интерфейс, лента, поиск | работает |
| Видео и звук на реальной консоли | код есть, сквозной прогон ещё не подтверждён |
| Обход DPI и внешний прокси | работают, но зависят от провайдера и CDN |
| Аккаунт Google, DRM, подписки | не поддерживаются |

Проект не связан с Google, Nintendo или YouTube. Это хобби-разработка, а не
продукт с поддержкой.

---

## 1. Что понадобится

- Linux или WSL2 (на macOS собирать `.nro` неудобно, host-тесты пойдут).
- ~10 ГБ на диске: devkitPro + портлибы + сборка.
- `git`, `make`, `g++` (C++17), `python3`.
- Для консольных проверок (необязательно): Switch с Atmosphere + hbmenu,
  SD-карта, USB-кабель, Wi-Fi.

Проверка хостового компилятора:

```bash
g++ --version    # нужен C++17, подойдёт g++ 11+
```

---

## 2. Установка devkitPro

Официальный установщик описан на [devkitpro.org](https://devkitpro.org/wiki/Getting_Started).
Для Debian/Ubuntu это выглядит так:

```bash
wget https://apt.devkitpro.org/install-devkitpro-pacman
chmod +x install-devkitpro-pacman
sudo ./install-devkitpro-pacman

sudo dkp-pacman -Syu
sudo dkp-pacman -S --needed switch-dev switch-mpv switch-curl \
    switch-jansson switch-ffmpeg switch-sdl2
```

`switch-dev` приносит devkitA64 и libnx; остальные пакеты — портлибы, которые
нужны для плеера, сети и JSON.

Версии, на которых проект собирается, зафиксированы в
[`tools/toolchain.md`](../tools/toolchain.md). Если у тебя другая версия
GCC и сборка падает — сначала сверься с этим файлом.

### Переменные окружения

Добавь в `~/.bashrc` (путь подставь свой):

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH
```

Затем:

```bash
source ~/.bashrc
```

`tools/build.sh` умеет сам искать devkitPro в `/opt/devkitpro`, `~/devkitpro`
и `~/devkitpro-root/opt/devkitpro`, но `make` из корня требует
`DEVKITPRO` в окружении. Надёжнее выставить переменную явно.

---

## 3. Проверка окружения

```bash
git clone https://github.com/agafak1337/TubeNX.git
cd TubeNX
make doctor
```

Ожидаемый вывод — примерно такой:

```
DEVKITPRO = /opt/devkitpro
devkitA64 GCC: aarch64-none-elf-gcc (devkitA64) 15.2.0
libnx: present
portlibs (switch):
  mpv        /opt/devkitpro/portlibs/switch/lib/libmpv.a
  curl       /opt/devkitpro/portlibs/switch/lib/libcurl.a
  jansson    /opt/devkitpro/portlibs/switch/lib/libjansson.a
  ffmpeg     /opt/devkitpro/portlibs/switch/lib/libavcodec.a
  SDL2       /opt/devkitpro/portlibs/switch/lib/libSDL2.a
make: GNU Make 4.x
git:  git version 2.x
```

Если `devkitA64 GCC` или `libnx` — `НЕ НАЙДЕН`, дальше идти нет смысла:
проверь `DEVKITPRO` и установку `switch-dev`.

---

## 4. Первая сборка

```bash
make -j$(nproc)
```

- Первая сборка идёт 10–25 минут: компилируется libmpv/FFmpeg-обвязка.
  Последующие — меньше минуты.
- Результат: `TubeNX.nro`, около 30 МБ.
- **Предупреждений должно быть 0.** Если появилось `warning:` — это регрессия,
  даже если файл собрался.
- Диагностическая сборка с подробным логом:

```bash
make diag          # → TubeNX-diag.nro
```

Альтернатива одной командой (сама находит toolchain, пишет `logs/build.log`,
считает предупреждения и sha256):

```bash
bash tools/build.sh
DIAG=1 bash tools/build.sh
```

---

## 5. Host-тесты: то, что можно проверить без Switch

```bash
make test-host
```

Что запускается:

- HCT/CAM16 и токены Material 3 — против `design/tokens.json`;
- типографика, формы, elevation, состояния виджетов;
- InnerTube: разбор фикстур, ошибки, таймауты;
- SOCKS5, DPI-фрагментация, HTTP-ретранслятор, `Range`;
- сеть: заголовки, редиректы, лимиты тела;
- конфиг и история: миграции, атомарная запись, битые строки;
- строки интерфейса и склонения;
- offscreen-рендер всех экранов в PNG.

Успех выглядит так:

```
== ИТОГ: ЗЕЛЁНО ==
```

Если `ЕСТЬ ОШИБКИ` — логи лежат в `$TMPDIR/tubenx-host/` (обычно
`/tmp/tubenx-host/`). Начни с `*.build.log`: часто это не логика, а
окружение (например, `CXX` указывает на `aarch64-none-elf-g++` из devkitPro).

---

## 6. Интерфейс без консоли

```bash
bash tools/preview.sh /tmp/tubenx
```

В `/tmp/tubenx` появятся 7 PNG — тот же код UI, что и в `.nro`:

```
01_splash.png   02_home.png      03_search.png   04_suggest.png
05_player.png   06_settings.png  07_home_light.png
```

Что смотреть глазами:

- ничего не обрезано по краям и в кнопках;
- кириллица без «тофу» и без наложения строк;
- фокус виден (кольцо/подсветка), размеры интерактивных элементов ≥ 48×48;
- контраст текста на фоне читается в тёмной и светлой теме;
- на главной нет «прыгающих» карточек и пустых прямоугольников без
  placeholder.

Это быстрый способ поймать визуальную регрессию, не заливая `.nro` на SD.

---

## 7. Что читать в коде

Порядок на первый день:

1. [`docs/architecture.md`](architecture.md) — слои, потоки, таймауты, где логи.
   15 минут, обязательный минимум.
2. `source/main.cpp` — окно, framebuffer, главный цикл, applet-hook. Читать,
   не менять.
3. `source/ui/app.cpp` + `source/ui/app.hpp` — состояние, навигация, фоновые
   запросы.
4. `source/ui/screens.cpp` — экраны. Самый большой файл; читать по одному
   экрану.
5. `source/md3/theme.cpp`, `ui.cpp`, `widgets.cpp` — токены, ввод/фокус, виджеты.
6. `source/net/http.cpp` — libcurl, SOCKS, таймауты.
7. `source/dpi/dpi.cpp` — SOCKS5-фрагментатор и HTTP-ретранслятор.
8. `source/player/player.cpp` — libmpv, кадр, звук.
9. `source/yt/innertube.cpp` — запросы к InnerTube.

Карта зон и правила — в [`CONTRIBUTING.md`](../CONTRIBUTING.md) и
[`docs/tasks.md`](tasks.md).

---

## 8. Рабочий цикл

```bash
git switch -c имя/тема          # например, anna/heap-log
# ... правки ...
make -j$(nproc)                 # 0 предупреждений
make test-host                  # ЗЕЛЕНО
git add -p                      # коммить осмысленными кусками
git commit
git push -u origin имя/тема
```

Формат коммита:

```
тип(область): короткое описание в императиве

Почему это нужно и что меняется. Номер задачи (PL-05, RD-01) — в заголовке
или в теле.
```

Типы: `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `build`, `diag`.
Области: `ui`, `md3`, `net`, `dpi`, `yt`, `player`, `core`, `docs`, `build`.

Правила:

- в `main` напрямую не пушим — только PR;
- один PR — одна тема (не смешивай рефакторинг и фикс);
- если взял задачу, напиши об этом, чтобы двое не делали одно и то же;
- `design/` — read-only; `docs/impl-tz.md` важнее комментариев в коде;
- `source/main.cpp`, `source/platform.hpp`, `Makefile` — только по согласованию;
- никаких секретов, cookie, личных IP, серийных номеров и путей со своего
  компьютера в коде, логах и документации.

Подробнее — [`CONTRIBUTING.md`](../CONTRIBUTING.md). Если хочешь помогать
регулярно и получить право записи в репозиторий — [`docs/coauthorship.md`](coauthorship.md).

---

## 9. Если есть консоль

Это не обязательный шаг для первого дня, но именно консольные прогоны
двигают проект к альфе.

```bash
bash tools/build.sh            # сборка с проверками
DIAG=1 bash tools/build.sh     # диагностическая сборка
bash tools/deploy.sh           # копирование на SD через MTP + sha256
bash tools/pull-log.sh         # забрать свежий лог в logs/
```

Чек-лист из 13 шагов и шаблон отчёта — [`docs/hardware-smoke.md`](hardware-smoke.md).

Важно при публикации логов и скриншотов:

- подписанные ссылки `googlevideo` — секрет на несколько часов, в issue их не
  вставляй, оставь только хост;
- убери из лога свой IP, имя Wi-Fi, серийный номер консоли и путь MTP;
- скриншот с консоли — это скриншот, а не рендер; не выдавай одно за другое.

---

## 10. Частые проблемы

| Симптом | Причина и что делать |
|---|---|
| `DEVKITPRO не задан` | `export DEVKITPRO=/opt/devkitpro`, затем `source ~/.bashrc` |
| `aarch64-none-elf-g++: not found` | не установлен `switch-dev` или PATH без `$DEVKITA64/bin` |
| `libmpv.a` не найден | не установлен `switch-mpv` (и другие портлибы) |
| Сборка падает на `curl`/`jansson` | нет `switch-curl`/`switch-jansson`, проверь `make doctor` |
| Host-тесты собираются под ARM | в окружении `CXX` от devkitPro; запусти `make test-host` в чистом шелле или задай `HOST_CXX=g++` |
| `make test-host` красный, а код не менял | прочитай `/tmp/tubenx-host/*.build.log`; часто это окружение |
| `preview.sh` не находит шрифты | запускай из корня репозитория; скрипт сам выставляет `TUBENX_FONTS` |
| MTP-путь к SD пропал | переподключи кабель, проверь `ls $XDG_RUNTIME_DIR/gvfs/` |
| Первая сборка «висит» | это нормально, смотри `logs/build.log` и `top` |

---

## 11. Контрольный список первого дня

- [ ] `make doctor` показывает GCC, libnx и все портлибы.
- [ ] `make -j$(nproc)` собрал `TubeNX.nro`, предупреждений 0.
- [ ] `make test-host` закончился `== ИТОГ: ЗЕЛЁНО ==`.
- [ ] `bash tools/preview.sh /tmp/tubenx` дал 7 PNG без визуальных дефектов.
- [ ] Прочитан `docs/architecture.md`.
- [ ] Прочитан `CONTRIBUTING.md`, понятны зоны и запреты.
- [ ] Выбрана задача из `docs/tasks.md` (раздел «Первые задачи»).
- [ ] Создана ветка, сделан первый PR.

Если застрял — пиши в issue с меткой `question` или в общий чат. Вопрос
дешевле часа догадок.