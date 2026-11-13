# Отчёт по дизайну TubeNX

Дата: 13.11.2026. Работа велась в `design/`; `source/`, `Makefile`,
`README.md`, `docs/` не изменялись.

## 1. Что было готово до меня

- Дизайн-система: `assets/tokens.css`, `base.css`, `components.css`,
  `app.css`, `tokens.json` (37 ролей в каждой теме, ARGB, шкала 12 ролей).
- Страницы 00–04, 06, 07 с разметкой и большинством фреймов; PNG лежали
  частично и были старше разметки.
- Страница `05-tv.html` существовала, но фреймы были пустыми: CSS готов,
  скрипт-построитель отсутствовал.
- Инструменты: `tools/wd.mjs` (frames / audit / shoot), `tools/render.sh`,
  `tools/png_tool.mjs`, `build.sh`.

## 2. Что я проверил и починил

Аудит `wd.mjs audit` по всем страницам (сырые результаты —
`/tmp/opencode/tv/audit/*.json`). Шум отделён от дефектов:

- «курсив» на пустых `<i>` (полоски прогресса, фигуры кадров) — ложное
  срабатывание, текста внутри нет;
- disabled-элементы с `opacity: .38` — требование §4.5, не дефект;
- тени у `menu` / `dialog` / `snackbar` / `elevated` и focus-гало — разрешены
  §2 (только всплывающие поверхности);
- «текст за границей фрейма» у контента, который обрезается краем экрана в
  скролле (`settings-top`, `settings-focus` и т.п.), — задуманная обрезка.

Настоящие дефекты и правки — в разделе 3.

## 3. Что доделал (по страницам и фреймам)

### 05 TV (сделано с нуля)

`design/screens/05-tv.html`, 10 фреймов 1920×1080 (внутренний холст 1280×720,
масштаб 1,5). Фреймы строятся скриптом из `data-tv`.

| frame-id | что внутри |
|---|---|
| `tv-home` | Home в доке: поля 64, карточка 276×245, «Продолжить просмотр», второй ряд обрезан |
| `tv-home-safe` | то же + safe area 64/36 |
| `tv-home-light` | светлая тема, фокус на карточке |
| `tv-home-variants` | сравнение A (64/276) и B (24/296) с обоснованием |
| `tv-search-empty` | пустой поиск + «Недавние», колонка 720 |
| `tv-search-results` | результаты 4×2, фокус на карточке |
| `tv-player` | плеер: контролы, дорожка, чипы, фокус на «назад» |
| `tv-player-safe` | пауза, фокус на дорожке (6 px линия, ползунок 20 px) |
| `tv-settings` | настройки в две колонки, нижняя секция под затуханием |
| `tv-settings-safe` | то же + safe area и фокус на сегменте |

Проверено глазами по PNG: сетка 4·276 + 3·16 = 1152 внутри 64…1216;
«Продолжить просмотр» и второй ряд обрезаются краем, как в утверждённом
handheld; в `tv-player-safe` фокус на дорожке виден на тёмном кадре
(кольцо primary + белое кольцо ползунка); в настройках нижняя секция уходит
под градиентное затухание, в safe area 684–720 интерактивных элементов нет.
Поле поиска 52 px поднято на 6 px (y 40–92), чтобы не заходить в safe area 36.

### 00 Cover — 1 фрейм

| frame-id | что проверено |
|---|---|
| `cover` | 1280×720, тёмная тема; заголовок и подзаголовок не обрезаны, логотип — вектор |

Правок не потребовалось. Аудит: 5 срабатываний «курсив» на пустых `<i>`
(полоски-декор) — ложные.

### 01 Tokens — 7 фреймов

| frame-id | что внутри |
|---|---|
| `tokens-color-dark` / `tokens-color-light` | 37 ролей в двух темах, подписи и hex |
| `tokens-contrast` | пары текст/фон с расчётом контраста |
| `tokens-elevation` | 5 уровней поверхности, без теней у списков |
| `tokens-seeds` | 6 seed-вариантов, основной `#C3002F` |
| `tokens-shape` | шкала радиусов и squircle |
| `tokens-type` | 12 ролей шкалы, живые образцы |

Исправлено два дефекта:

1. образец `labelSmall` был набран 11 px — поднят до 12 px (минимум §2),
   в строке шкалы значение токена 11/16 оставлено как спецификация;
2. плитка `outline` показывала контраст 4,23:1 (ниже 4,5) — текст переведён
   на `onSurface`, контраст ≥ 7:1.

После правок аудит чистый: остались только разрешённые тени
`elevated`/`menu`/`dialog`/`snackbar` и ложный «курсив».

### 02 Components — 18 фреймов

`cmp-button`, `cmp-card`, `cmp-chip`, `cmp-dialog`, `cmp-empty-state`,
`cmp-focus-ring`, `cmp-icon-button`, `cmp-list-item`, `cmp-navigation-bar`,
`cmp-progress`, `cmp-search-bar`, `cmp-segmented`, `cmp-skeleton`,
`cmp-slider`, `cmp-snackbar`, `cmp-switch`, `cmp-top-app-bar`,
`cmp-video-card`.

Проверены все состояния из библиотеки: default / hover / focus / pressed /
disabled, тёмная и светлая темы, зоны нажатия и redline-размеры. Правок
композиции не потребовалось; после правок токенов страница перерендерена.
Оставшиеся замечания аудита — шум: opacity 0.38 у disabled по §4.5, тени
только у всплывающих поверхностей (§2), «курсив» на пустых `<i>`,
`span.sample` с обводкой 1 px. Все 18 PNG просмотрены глазами, включая
`cmp-slider`, `cmp-snackbar`, `cmp-switch`, `cmp-top-app-bar`,
`cmp-video-card`: обрезаний и наложений нет.

### 03 Screens Home — 21 фрейм

Состояния §4: `home-default`, `home-focus`, `home-focus-chip`,
`home-focus-light`, `home-default-light`, `home-loading`, `home-empty`,
`home-no-history`, `home-error`, `home-long-title`, `home-pressed`,
`home-scrolled`, `home-cols-3` (968×720), `home-cols-2` (656×720),
`home-toast`, `home-toast-action`, `home-concept-bottom-nav`,
`home-continue-variants`, `splash-checking`, `splash-ready`,
`splash-offline`.

Проверено: карточка сохраняет высоту при 4/3/2 колонках, заголовок
120 символов обрезается двумя строками, «Продолжить просмотр» и второй ряд
обрезаются краем — как в утверждённом макете. Аудит даёт 1,06:1 у заголовков
на обложках в светлой теме — это ограничение инструмента (заголовок лежит
на скриме поверх `.th`, аудит видит фон фрейма). Ручной расчёт: белый на
скриме 80 % поверх самой светлой обложки — 12,9:1.

### 03 Screens Search — 9 фреймов

`search-empty`, `search-empty-no-history`, `search-suggest`,
`search-results`, `search-results-focus`, `search-focus-field`,
`search-loading`, `search-not-found`, `search-error`.

Все состояния §4.3 на месте: пусто, недавние, подсказки, результаты, фокус
поля и карточки, загрузка, ничего не найдено, ошибка. Правок не
потребовалось; страница перерендерена. Иконка пустого состояния — `outline`
(см. вопрос 10), контраст ≈ 5:1.

### 03 Screens Settings — 11 фреймов

`settings-layout-variants`, `settings-top`, `settings-scrolled`,
`settings-full`, `settings-full-light`, `settings-focus`,
`settings-focus-states`, `settings-disabled`, `settings-dialog`,
`settings-top-light`, `settings-seed-blue`.

Исправлено: `settings-focus-states` не влезала в 1280 — сетка переведена на
`repeat(3, minmax(0,1fr)) × 3`, `.stage { zoom: 1.5 }`; все 9 ячеек
(8 примеров фокуса + правило) помещаются в кадр. После правки аудит не
находит обрезаний в этом фрейме.
`settings-full` и `settings-full-light` — служебные доски 1280×824 (вся
страница целиком, чтобы проверить нижние секции); остальные 9 — ровно
1280×720. «Сдвиг звука» в `settings-top` уходит под нижний край — это
задуманная обрезка скролла, аудит помечает её как overflow.

### 04 Player — 19 фреймов

`player-controls`, `player-controls-bright`, `player-hidden`,
`player-paused`, `player-buffering`, `player-loading`, `player-error`,
`player-ended`, `player-ended-related`, `player-quality-menu`,
`player-more-menu`, `player-close-dialog`, `player-focus-button`,
`player-focus-chip`, `player-focus-seek`, `player-liked`,
`player-proxy-chip`, `player-proxy-chip-off`, `player-redline`.

Поправлено пять вещей:

1. `player-more-menu` — меню опущено до `bottom: 96px`, больше не наезжает
   на чипы;
2. фокус на дорожке получил подложку, видимую на светлом кадре;
3. в `player-paused` вернулось солнце (тема/яркость) в панель;
4. `player-hidden` — восстановлен ночной кадр `v-night`;
5. redline-подписи приведены к сетке.

Слои поверх видео (78 % / 35 % / 18 %) не менялись — это §4.4; ручные
расчёты контраста для светлого и белого кадра вынесены в
`OPEN_QUESTIONS.md`, п. 2.

### 06 Icon — 5 фреймов

`icon-construction`, `icon-context`, `icon-sizes`, `icon-variants`,
`illustrations` (1280×720). Проверены сетка построения, контекст на тёмной
и светлой плитке, размеры 48/64/128/256, варианты и 4 иллюстрации.
Правок не потребовалось; PNG перерендерены. Монохром — белая марка со
сквозным треугольником, см. вопрос 7.

### 07 Motion — 2 фрейма

`motion-curves` (1280×1100) и `motion-spec` (1280×1784) — спецификационные
доски: кривые, длительности, правила перехода. Это не экраны 1280×720,
поэтому высота доски больше кадра. Правок не потребовалось.

### Экспорт

- `export/icon/`: 256 (NACP), 1024, 128, 64, 48, mono, SVG — пиксель-в-пиксель
  совпадение с эталоном для 48/256/mono, diff 0.000;
- 4 SVG иллюстраций валидны, без фильтров и внешних ссылок;
- `tokens.json`: 37 ролей в каждой теме, ARGB, шкала, радиусы.

## 4. Что не удалось / требует решения

См. `OPEN_QUESTIONS.md`: 12 противоречий ТЗ и дизайн-системы.

Отдельно: саб-агент по Cover/Tokens/Components/Motion оборвался по связи
с провайдером уже после правок. Полуприменённых состояний нет: обе правки
(`labelSmall` 12 px, плитка `outline` на `onSurface`) видны в текущем
`01-tokens.html`, аудит `01-tokens-new.json` чист по кеглю и контрасту,
PNG перерендерены и просмотрены.

## 5. Проблемы в общих CSS или в ТЗ

1. `components.css`: `.slider.is-focus { outline: none }` — у слайдера нет
   кольца фокуса, хотя §3.4 требует 3 px primary. На страницах Player и TV
   кольцо добавлено локально.
2. В системе нет варианта фокуса «поверх видео»: `primary` на светлом кадре
   даёт ≈ 1,4:1. Страницы добавляют тёмную обводку 7 px сами.
3. `.chip.on-video` — 36 px без зоны нажатия 48; `.btn.text` — 40 px.
4. `.center-btn` привязан к координатам 1280×720 (`left: 598; top: 318`).
5. `.slider.on-video` не имеет варианта громкости (белая заливка/ползунок) —
   на странице плеера он локальный.
6. ТЗ противоречит себе по контрасту слоёв поверх видео (§4.4 против §3.2/§8)
   и по кеглю `labelSmall` 11 px (§3.3 против §2).

## 6. Где мои файлы

- `design/screens/05-tv.html` — новая страница TV, 10 фреймов;
- `design/png/` — 103 PNG по всем страницам:
  `00-cover` 1, `01-tokens` 7, `02-components` 18, `03-screens-home` 21,
  `03-screens-search` 9, `03-screens-settings` 11, `04-player` 19,
  `05-tv` 10, `06-icon` 5, `07-motion` 2;
- `design/handoff/design-agent/` — этот отчёт, `manifest.json`,
  `OPEN_QUESTIONS.md`, `SHA256SUMS` (133 строки: PNG, страницы, экспорт,
  токены и файлы отчёта).

Изменения только в `design/`; `source/`, `Makefile`, `README.md`, `docs/`
не тронуты, коммитов нет.