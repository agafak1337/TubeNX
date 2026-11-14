# Синхронизация дизайна и кода (DOC-05)

Источник истины — `design/tokens.json` и `docs/impl-tz.md` §5.2. Этот файл
фиксирует, где те же значения живут в коде и чем они проверяются. `design/`
не редактируется.

## 1. Seed и холст

| Токен | Значение | Код | Проверка |
|---|---|---|---|
| canvas | 1280×720, 1 px ≈ 1 dp | `docs/impl-tz.md` §5.2, `main.cpp` | preview 1280×720 |
| seed | `#C3002F` (`0xFFC3002F`) | `theme.cpp: defaultScheme`, `App::Config::seed` | `test_theme` |
| dark | по умолчанию `true` | `Config::dark` | `test_theme` (2 темы) |
| dock | масштаб канваса, не рост логических размеров | `Canvas::setScale` | шаг 12 smoke |

## 2. Цветовые роли

37 ролей M3, значения — `design/tokens.json → color.dark/light`.
Код: `md3::schemeFromSeed()` (HCT/CAM16, tonal spot).
Проверка: `tools/test_theme.cpp` сравнивает каждую роль с токеном, ΔE00 ≤ 2,
для обеих тем.

Правило: **не хардкодить цвета в экранах**. Новый оттенок сначала появляется
в токенах, затем в `ColorScheme`/`Theme`, и только потом используется.

## 3. Типографика

Шкала одна на 720p; в dock кегль растёт физически за счёт масштаба канваса.

| Роль | Шрифт | size / line-height | tracking |
|---|---|---:|---:|
| displayLarge | Regular | 57 / 64 | −0.25 |
| displayMedium | Regular | 45 / 52 | 0 |
| displaySmall | Regular | 36 / 44 | 0 |
| headlineLarge | Regular | 32 / 40 | 0 |
| headlineMedium | Regular | 28 / 36 | 0 |
| headlineSmall | Regular | 24 / 32 | 0 |
| titleLarge | Medium | 22 / 28 | 0 |
| titleMedium | Medium | 16 / 24 | 0.15 |
| titleSmall | Medium | 14 / 20 | 0.1 |
| bodyLarge | Regular | 16 / 24 | 0.15 |
| bodyMedium | Regular | 14 / 20 | 0.15 |
| bodySmall | Regular | 12 / 16 | 0.2 |
| labelLarge | Medium | 14 / 20 | 0.1 |
| labelMedium | Medium | 12 / 16 | 0.3 |
| labelSmall | Medium | 12 / 16 | 0.4 |

Код: `Typography::standard()` (`source/md3/theme.cpp`).
Проверка: `test_theme` (включая Р-01: `labelSmall` = 12, а не 11).

## 4. Формы

| Токен | px |
|---|---:|
| none | 0 |
| extraSmall | 4 |
| small | 8 |
| medium | 12 |
| large | 16 |
| largeIncreased | 20 |
| extraLarge | 28 |
| full | 999 |
| badge | 6 |

Код: `md3::Shapes{}`. Проверка: `test_theme` (9 значений).

## 5. Движение и состояния

| Токен | Значение | Код |
|---|---|---|
| short2 / short4 | 100 / 200 мс | `Motion` |
| medium1 / medium2 | 250 / 300 мс | `Motion` |
| controls | 180 мс | `Motion` |
| skeletonPulse | 1200 мс | `Motion` |
| ripple | 450 мс | `Motion` |
| emphasized | cubic-bezier(0.2, 0, 0, 1) | `Motion` |
| state hover / focus / pressed | 0.08 / 0.10 / 0.10 | `StateLayer` |
| state dragged | 0.16 | `StateLayer` |
| disabled content / container | 0.38 / 0.12 | `StateLayer` |

Правило: длительности и кривые берутся из `md3::motion`, а не пишутся числом
в экране.

## 6. Скриншоты

`make preview` (или `bash tools/preview.sh /tmp/tubenx`) рендерит те же экраны,
что и NRO, в 7 PNG:

```
01_splash.png
02_home.png
03_search.png
04_suggest.png
05_player.png
06_settings.png
07_home_light.png
```

При изменении темы/размеров обновляйте preview и прикладывайте PNG к
изменению. Расхождение «код ↔ макет» ловится `test_theme`; расхождение
«код ↔ картинка» — глазами по preview.

## 7. Что делать при изменении дизайна

1. Дизайн-агент меняет `design/tokens.json` и макеты (вне этого репозитория).
2. В коде меняется только `theme.cpp`/`widgets.cpp`/экраны.
3. `make test-host` — `test_theme` должен быть зелёным; при смене значений
   обновляется и токен, и тест.
4. `make preview` — 7 PNG; сравнить с макетом.
5. Запись в `CHANGELOG.md`, если изменение видно пользователю.