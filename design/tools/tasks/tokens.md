# Задание: страницы 00 Cover, 01 Tokens, 07 Motion

Папка временных рендеров: `/tmp/opencode/agent-tokens/`.

## A) `design/screens/00-cover.html`

Один фрейм 1280×720, id `cover`: марка, «TubeNX», подзаголовок «Дизайн-система · Material 3 Expressive»,
версия 1.0, дата 13.11.2026, seed-цвет #C3002F образцом с подписью, список страниц файла
(00 Cover … 07 Motion). Спокойно, выразительная типографика, без запрещённых эффектов.

## B) `design/screens/01-tokens.html`

Доска с фреймами (размер свободный, можно выше 720 — задай inline width/height у `.frame`; id у каждого):

- `tokens-color-dark` и `tokens-color-light`: все роли из ТЗ §3.2 плитками в привычной M3-раскладке
  (пары роль / on-роль рядом; плитка залита ролью, текст на ней — её on-ролью; подпись = имя роли + hex).
  Hex бери из `tokens.json` (там формат #AARRGGBB — показывай как #RRGGBB). Плитка красит фон через
  `var(--роль)`, а не хексом.
- `tokens-contrast`: таблица контраста ключевых пар текст/фон в обеих темах: onSurface и onSurfaceVariant на
  surface, surfaceContainerLow, surfaceContainer, surfaceContainerHigh, surfaceContainerHighest;
  onPrimary/primary; onPrimaryContainer/primaryContainer; onSecondaryContainer/secondaryContainer;
  onTertiaryContainer/tertiaryContainer; onErrorContainer/errorContainer; primary на surface и на
  surfaceContainerLow; error на surface и surfaceContainerLow; tertiary на surface и surfaceContainerLow;
  inverseOnSurface/inverseSurface; inversePrimary/inverseSurface; outline на surface (неконтентный, порог 3:1).
  Коэффициенты ПОСЧИТАЙ скриптом (node, формула WCAG 2.x relative luminance) из `tokens.json` — не на глаз.
  Скрипт сохрани как `design/tools/contrast.mjs` (читает tokens.json, печатает таблицу, код возврата 1, если
  текстовая пара ниже 4.5). В таблице — значение и отметка «проходит / не проходит». Если пара не проходит —
  НЕ подгоняй цифры, покажи как есть и напиши об этом в отчёте.
- `tokens-seeds`: 6 seed-палитр (red/blue/green/purple/orange/pink из `seedPalette`) — для каждой кружок seed
  и полоска ключевых ролей dark (primary, primaryContainer, secondaryContainer, tertiary, surface,
  surfaceContainerLow). Используй классы `.seed-*`.
- `tokens-type`: шкала типографики из ТЗ §3.3 — каждая роль строкой-образцом на русском (в т.ч. длинный
  русский заголовок), рядом кегль / интерлиньяж / начертание / где используется. Отметь, что Google Sans Text
  в ресурсах нет и body/label пока рисуются Google Sans.
- `tokens-shape`: шкала радиусов (8/12/16/20/28/full + squircle марки), сетка 8 px и отступы 8/12/16/24/32,
  поля экрана 24, тач-таргет 48, фокус-кольцо 3 px + зазор 2, безопасная зона ТВ 64/36 (миниатюра с `.safe`).
- `tokens-elevation`: тональные поверхности surfaceContainerLowest…Highest как замена elevation + 3 уровня
  тени (только для всплывающих поверхностей) с параметрами blur/dy/alpha из tokens.json.

## C) `design/screens/07-motion.html`

Фреймы `motion-spec` и `motion-curves` — текстовая спека движения (ТЗ §3.5, без видео): таблица
«что анимируется → свойство → длительность → кривая»; кривые M3 нарисуй инлайновым SVG (графики
cubic-bezier: emphasized 0.2,0,0,1; emphasized decelerate; emphasized accelerate; linear).
Покрой: появление/скрытие контролов плеера (fade + сдвиг 8 px, 180 мс), state layer, фокус-кольцо,
переключение чипа, switch, сегмент, меню качества, подсказки поиска, диалог, снэкбар (автоскрытие 3 с),
скелетон (opacity 0.6→1.0 за 1.2 с), линейный/круговой прогресс, переходы между экранами, скролл
(инерционный, без «резинки»), нажатие кнопки (сжатие радиуса).
Явно пропиши правила: никаких бесконечных декоративных анимаций, кроме прогресса; при 60 fps анимировать
только opacity / сдвиг / размер небольших областей — без полноэкранных градиентов на каждом кадре.

## Сдача

Отрендери каждый фрейм, посмотри PNG глазами, исправь дефекты. Итоговые PNG — в `design/png/00-cover/`,
`design/png/01-tokens/`, `design/png/07-motion/`. Для высоких фреймов передавай высоту окна 5-м аргументом
render.sh. В отчёт включи полную таблицу контраста с числами.
