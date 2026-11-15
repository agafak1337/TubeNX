# M3: соответствие дизайну, состояния компонентов, доступность

Документ закрывает M3-01 (таблица «ТЗ → код»), M3-03 (матрица состояний),
M3-07 (светлая тема), M3-08 (motion-токены), RD-08 (тени/градиенты) и
RD-11 (чек-лист доступности). Источник правды — `design/tokens.json` и
`docs/impl-tz.md` §5; `design/` только читаем.

## 1. Токены → код (M3-01)

| Дизайн (`design/tokens.json` / `design-tz.md`) | Код | Проверка |
|---|---|---|
| `color.*` — 37 ролей, seed `#C3002F`, dark/light | `md3::ColorScheme`, `md3::schemeFromSeed()`, `source/md3/theme.cpp` | `tools/test_theme.cpp`: ΔE00 ≤ 2 к tokens.json для обеих тем |
| `typography.*` — 12 ролей M3, целые px | `md3::Typography::standard()`, `source/md3/theme.cpp` | `tools/test_theme.cpp`, `tools/test_m3.cpp` (целые, ≥ 12) |
| `shape.*` — none…full, badge | `md3::Shapes` (largeIncreased 20, extraLarge 28) | `tools/test_theme.cpp`, `tools/test_m3.cpp` |
| `state.*` — hover 0.08, focus 0.10, pressed 0.10, dragged 0.16 | `md3::stateLayerTokens`, `Ui::stateLayer()` | `tools/test_m3.cpp` |
| `motion.*` — short2/short4/medium1/medium2/controls/pulse, кривые | `md3::motion`, `Spring`/`Tween` | `tools/test_m3.cpp` |
| `size.touchTarget` 48, `topAppBar` 72, `focusRing` 3/2 | `source/ui/screens.cpp`, `source/md3/ui.cpp` | чек-лист §4 |
| `size.videoCard` 296×256, cover 166; 4 колонки | `App::gridLayout()`, `drawHome()` | `preview` 02_home |
| `size.continueCard` 240×135, полоса 187 | `drawContinueStrip()` | `preview` 02_home |
| `size.bottomReserve` 88 | `kBottomReserve` | все экраны |
| Иконки Material Symbols | `source/md3/icons.hpp` + `resources/fonts/MaterialSymbols.ttf` | `tools/check_icons.py` — 0 missing |
| Экраны Home/Search/Settings/Player/Splash | `draw*()` в `source/ui/screens.cpp` | `preview` 7 PNG |
| Компоненты (button/chip/switch/slider/listItem/videoTile) | `source/md3/widgets.cpp` | матрица §2 |

Расхождения, зафиксированные осознанно:

- Логический канвас всегда 1280×720; dock рисует его с `scale=1.5` в буфер
  1920×1080 (`core/layout.hpp`, `main.cpp`). Кегли не пересчитываются вручную —
  масштаб применяют `Canvas` и `Fonts`, поэтому текст на TV резкий.
- Поля dock берутся из safe area (64/36), handheld — 24 (`ScreenLayout`).
- Полноэкранный aurora-фон сплэша считается один раз и кэшируется в `Image`
  (RD-08), а не каждый кадр.

## 2. Матрица состояний компонентов (M3-03)

Состояния: **H** hover, **F** focus, **P** pressed, **D** disabled,
**S** selected/checked. Значения state layer — из `stateLayerTokens`.

| Компонент | H | F | P | D | S | Реализация |
|---|---|---|---|---|---|---|
| `button` (Filled/Tonal/Elevated/Outlined/Text) | ✓ 0.08 | ✓ ring + 0.10 | ✓ 0.10 | ✓ контейнер 0.12, текст 0.38 | — | `md3::button`, `widgets.cpp` |
| `chip` (Filter/Assist) | ✓ 0.08 | ✓ ring внутрь | ✓ 0.10 | ✓ | ✓ заливка + галочка | `md3::chip`, `widgets.cpp` |
| `switch` | ✓ halo 0.08 | ✓ ring | ✓ дорожка | ✓ | ✓ анимация Spring | `md3::switchWidget` |
| `slider` | ✓ halo | ✓ ring | ✓ перетаскивание | ✓ | ✓ value | `md3::slider` |
| `listItem` (настройки/меню) | ✓ 0.08 | ✓ ring offset −3 | ✓ 0.10 | ✓ | ✓ выбранный фон | `md3::listItem` |
| `videoTile` | ✓ 0.08 | ✓ ring + подпись | ✓ 0.10 | — | ✓ «×» у «Продолжить» | `md3::videoTile*` |
| `iconButton` | ✓ | ✓ ring | ✓ ripple | ✓ приглушение | ✓ toggle | `md3::iconButton` |

Фокус хранится по стабильному `u.id(...)` и переживает скролл
(`Ui::focusable` + `scrollId`). Ripple ограничен формой через `Canvas::ripple`
и clip-стек.

## 3. Светлая тема (M3-07)

Обе темы генерируются из одного seed (`schemeFromSeed`) и сверяются с
`design/tokens.json` в `tools/test_theme.cpp` (ΔE00). Дополнительно M3-02
перебирает 20 seed-цветов и требует контраст ≥ 5.45 для основного текста.
Светлая тема рендерится в `preview` (07_home_light.png).

## 4. Доступность и читаемость (RD-11)

- [x] Минимальный кегль — 12 px. В `font().draw` в UI остались только
      12/13/14/16/18/20/22/24; два подписных кегля 11 px подняты до 12.
- [x] Зона нажатия — 48×48 у кнопок верхних панелей и плеера.
  Исключение: «×» на карточке «Продолжить» — компактная накладка 26 px,
  чтобы не перекрывать обложку; основная карточка 240×135 нажимается целиком.
- [x] Фокус видим: кольцо 3 px + заливка state layer; для строк списков —
      кольцо внутрь (offset −3), чтобы не обрезалось.
- [x] Не только цветом: состояние дублируется иконкой (галочка, `×`, точка),
      а ошибки — текстом, а не только красным.
- [x] Контраст токенов — `test_theme` + M3-02.
- [ ] Финальная проверка на TV (dock) — в общем прогоне на консоли.

## 5. Тени и градиенты (RD-08)

- `Canvas::shadow` в UI вызывается только у приподнятых поверхностей
  (elevated/FAB/диалоги); карточки — тональные поверхности + outline.
- Полноэкранные градиенты есть только на сплэше и кэшируются в `Image`
  (`splashBgReady`), поэтому не стоят миллисекунд каждый кадр.
- Градиенты плеера — узкие полосы (24/190/240 px), не весь экран.