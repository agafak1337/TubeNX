# Бриф для исполнителей дизайна TubeNX

Полное ТЗ: `$HOME/TubeNX/docs/design-tz.md` — **прочитай целиком перед работой**, оно главнее этого брифа.

Figma недоступна, поэтому «Figma-файл» — это набор HTML-страниц в `$HOME/TubeNX/design/`,
каждая страница = одна страница Figma (доска с фреймами). Страницы рендерятся в PNG headless Firefox.

## Что уже готово (не переписывать, только использовать)

- `design/assets/tokens.css` — роли цвета M3 как CSS-переменные (`--primary`, `--on-surface-variant`, …).
  Классы темы: `.dark` (по умолчанию), `.light`; seed: `.seed-blue|green|purple|orange|pink`.
  Автогенерация из HCT-движка приложения — руками не править.
- `design/assets/base.css` — шрифты, типографика (`.display-s .headline-m .headline-s .title-l .title-m
  .title-s .body-l .body-m .body-s .label-l .label-m .overline`), `.icon`, доска (`.board .row .shot .name .spec`),
  фрейм (`.frame`, `.frame.video`), `.abs`, `.safe`, `.redline`.
- `design/assets/components.css` — Button, Icon button, Chip, Segmented, Switch, Slider, Search bar, Card,
  List item, Menu, Dialog, Snackbar, Progress, Skeleton, Mark. Состояния — классами
  `.is-hover .is-focus .is-pressed .is-disabled .is-selected .is-on`.
- `design/assets/app.css` — app bar, сетка, карточка видео (`.vcard`), «Продолжить просмотр» (`.ccard`),
  обложки-заглушки (`.th.t1…t8`), скелетон, пустое состояние, карточка ошибки, скримы плеера.
- `design/assets/frame.js` — `?frame=<id>` оставляет один фрейм (для экспорта), `?scale=1.5` масштабирует.
- `design/screens/_reference.html` — **образец разметки, от него и отталкивайся**. Его рендер: `/tmp/opencode/ref.png`.
- `design/tokens.json` — экспорт токенов.

Прочитай все четыре CSS и `_reference.html` до того, как писать свою страницу.

## Правила

1. Цвета — только роли (`var(--…)`). Хексы допустимы только для: обложек-заглушек, слоёв поверх видео
   (белый/чёрный с альфой, как в ТЗ §4.4), служебной разметки доски (redline, подписи).
2. Запрещено (ТЗ §2): blur, backdrop-filter, glass, mesh-градиенты, тени на карточках списка, курсив,
   кегль меньше 12 px, иконки тоньше 1.5 px. Тень (`box-shadow`) — только у всплывающих поверхностей.
3. Иконки — `<span class="icon">имя</span>` (Material Symbols Rounded, лигатуры), залитые — `class="icon fill"`.
   В сабсете есть только: add arrow_back arrow_forward bolt check chevron_right close cloud_off dark_mode delete
   error forward_10 fullscreen gpp_good hd high_quality history home info keyboard light_mode lock more_vert
   movie music_note newspaper north_west open_in_new palette pause play_arrow podcasts refresh remove replay
   replay_10 restart_alt schedule search search_off settings shield skip_next speed sports_esports subtitles
   sync thumb_up trending_up tune video_library visibility volume_off volume_up wifi wifi_off.
   Других имён не использовать (отрисуются текстом).
4. Шрифт — только Google Sans (Regular 400 / Medium 500 / Bold 700). Google Sans Text в проекте нет,
   body/label рисуются Google Sans.
5. Фрейм — `<div class="frame dark" id="уникальный-id">` 1280×720. Внутри — абсолютное позиционирование
   по координатам ТЗ (`class="abs" style="left:…; top:…"`) либо готовые блоки из app.css.
   У каждого фрейма свой `id` в kebab-case — по нему делается экспорт.
6. Каждый фрейм обёрнут в `.shot` с `.name` (как в Figma: «Home / Загрузка / Тёмная») и `.spec` —
   короткая спека: сетка, отступы, размеры, роли цветов.
7. Тексты — русские, строго по ТЗ §7: без «Вы», без восклицательных знаков, без emoji; тысячи через
   пробел («1 200 000»), время `MM:SS` / `H:MM:SS`. Не писать «Материал 3».
8. Тач-таргет ≥ 48×48, между соседними ≥ 8 px. Фокус геймпада — 3 px `primary` снаружи + подложка
   (класс `.is-focus`), должен быть виден на любом фоне.
9. Безопасная зона ТВ: 64 px по бокам, 36 px сверху/снизу — без интерактивных элементов. Исключение,
   заданное самим ТЗ: поля экрана 24 px в handheld-макете (см. раздел «Вопросы» в README) — следуй ТЗ.
10. Если нужен стиль, которого нет в общих CSS, — пиши его в `<style>` **своей** страницы. Общие файлы в
    `design/assets/` не редактируй (их одновременно используют другие исполнители). Если нашёл в них
    ошибку — не чини, а опиши в итоговом отчёте.
11. Комментарии в коде — по минимуму.

## Проверка (обязательно)

Рендер одного фрейма:

    bash $HOME/TubeNX/design/tools/render.sh screens/<page>.html /tmp/opencode/<agent>/<id>.png <frame-id>

(пути страницы — относительно `design/`; 4-й и 5-й аргументы — ширина и высота окна, по умолчанию 1280×720).
Firefox headless стартует несколько секунд; запускай с таймаутом и не параллель больше 2 рендеров сразу.

Отрендери **каждый** свой фрейм, открой PNG инструментом чтения и посмотри глазами: наложения, обрезанный
текст, вылезание за 1280×720, не отрисованные иконки (видны как слово), контраст. Исправляй и перерендеривай,
пока не станет чисто. Контраст текста ≥ 4.5:1 (крупный ≥ 3:1) — посчитай для своих пар текст/фон.

Готовые PNG фреймов клади в `design/png/<имя-страницы>/<frame-id>.png`.

## Отчёт

В конце верни: список созданных файлов; список фреймов (id → что это); что проверено рендером;
найденные проблемы в общих CSS или в ТЗ; места, где ТЗ допускало трактовку и что ты выбрал.
Сообщай только то, что реально сделал и проверил; если что-то не получилось или не проверено — скажи прямо.
