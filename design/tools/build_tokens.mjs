// Собирает design/tokens.json и design/assets/tokens.css из схем, посчитанных
// HCT-движком приложения (source/md3/theme.cpp, см. design/tools/dump_scheme.cpp).
// Запуск: design/tools/build.sh
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const dir = process.argv[2];

const seeds = [
  { id: "red", name: "Красный", hex: "#C3002F", default: true },
  { id: "blue", name: "Синий", hex: "#1A73E8" },
  { id: "green", name: "Зелёный", hex: "#1E8E3E" },
  { id: "purple", name: "Фиолетовый", hex: "#7E57C2" },
  { id: "orange", name: "Оранжевый", hex: "#E8710A" },
  { id: "pink", name: "Розовый", hex: "#D81B60" },
];

const schemes = {};
for (const s of seeds) schemes[s.id] = JSON.parse(readFileSync(join(dir, s.hex.slice(1) + ".json"), "utf8"));

const argb = (hex) => "#FF" + hex.slice(1).toUpperCase();
const kebab = (k) => k.replace(/[A-Z]/g, (m) => "-" + m.toLowerCase());

const type = {
  displaySmall: { family: "Google Sans", size: 36, lineHeight: 44, weight: 400, letterSpacing: 0 },
  headlineMedium: { family: "Google Sans", size: 28, lineHeight: 36, weight: 400, letterSpacing: 0 },
  headlineSmall: { family: "Google Sans", size: 24, lineHeight: 32, weight: 400, letterSpacing: 0 },
  titleLarge: { family: "Google Sans", size: 22, lineHeight: 28, weight: 500, letterSpacing: 0 },
  titleMedium: { family: "Google Sans", size: 16, lineHeight: 24, weight: 500, letterSpacing: 0.15 },
  titleSmall: { family: "Google Sans", size: 14, lineHeight: 20, weight: 500, letterSpacing: 0.1 },
  bodyLarge: { family: "Google Sans Text", size: 16, lineHeight: 24, weight: 400, letterSpacing: 0.15 },
  bodyMedium: { family: "Google Sans Text", size: 14, lineHeight: 20, weight: 400, letterSpacing: 0.15 },
  bodySmall: { family: "Google Sans Text", size: 12, lineHeight: 16, weight: 400, letterSpacing: 0.2 },
  labelLarge: { family: "Google Sans Text", size: 14, lineHeight: 20, weight: 500, letterSpacing: 0.1 },
  labelMedium: { family: "Google Sans Text", size: 12, lineHeight: 16, weight: 500, letterSpacing: 0.3 },
  labelSmall: { family: "Google Sans Text", size: 11, lineHeight: 16, weight: 500, letterSpacing: 0.4 },
};

const tokens = {
  meta: {
    name: "TubeNX design tokens",
    version: "1.0.0",
    canvas: { width: 1280, height: 720, unit: "px (720p-канвас, 1 px ≈ 1 dp)" },
    seed: "#C3002F",
    generator: "source/md3/theme.cpp · schemeFromSeed (HCT/CAM16, tonal spot)",
    colorFormat: "#AARRGGBB",
  },
  color: {
    dark: Object.fromEntries(Object.entries(schemes.red.dark).map(([k, v]) => [k, argb(v)])),
    light: Object.fromEntries(Object.entries(schemes.red.light).map(([k, v]) => [k, argb(v)])),
  },
  seedPalette: seeds.map((s) => ({
    id: s.id,
    name: s.name,
    seed: argb(s.hex),
    default: !!s.default,
    dark: Object.fromEntries(Object.entries(schemes[s.id].dark).map(([k, v]) => [k, argb(v)])),
    light: Object.fromEntries(Object.entries(schemes[s.id].light).map(([k, v]) => [k, argb(v)])),
  })),
  fixed: {
    comment: "Цвета поверх видео и обложек: не зависят от темы.",
    videoBlack: "#FF000000",
    onVideo: "#FFFFFFFF",
    onVideoSecondary: "#C7FFFFFF",
    videoChip: "#24FFFFFF",
    videoBuffer: "#4DFFFFFF",
    videoTrack: "#2EFFFFFF",
    durationBadge: "#B3000000",
    playOverlay: "#73000000",
    centerButton: "#59000000",
    hoverOverlay: "#0FFFFFFF",
    scrimTop: { from: "#B8000000", to: "#00000000", height: 190 },
    scrimBottom: { from: "#00000000", to: "#DB000000", height: 240 },
    pauseDim: "#1F000000",
  },
  state: { hover: 0.08, focus: 0.1, pressed: 0.1, dragged: 0.16, disabledContent: 0.38, disabledContainer: 0.12 },
  typography: type,
  typographyNote:
    "В resources/fonts лежит только Google Sans (Regular/Medium/Bold). Пока Google Sans Text не добавлен, роли body/label рисуются Google Sans.",
  shape: { none: 0, extraSmall: 4, small: 8, medium: 12, large: 16, largeIncreased: 20, extraLarge: 28, full: 999, badge: 6 },
  spacing: { xs: 4, sm: 8, md: 12, lg: 16, xl: 24, xxl: 32, screenMargin: 24, gridGap: 16, tvSafeX: 64, tvSafeY: 36 },
  size: {
    touchTarget: 48,
    topAppBar: 72,
    chipRow: 48,
    chip: 32,
    searchBar: 52,
    videoCard: { width: 296, cover: 166, height: 256 },
    continueCard: { width: 240, cover: 135 },
    bottomReserve: 88,
    focusRing: { width: 3, gap: 2 },
    icon: 24,
  },
  elevation: {
    comment: "Тень только у всплывающих поверхностей (меню, подсказки, диалог, снэкбар). Карточки — тональные.",
    level0: { blur: 0, dy: 0, alpha: 0 },
    level1: { blur: 6, dy: 1, alpha: 0.3 },
    level2: { blur: 10, dy: 2, alpha: 0.32 },
    level3: { blur: 16, dy: 4, alpha: 0.34 },
  },
  motion: {
    easing: {
      emphasized: [0.2, 0, 0, 1],
      emphasizedDecelerate: [0.05, 0.7, 0.1, 1],
      emphasizedAccelerate: [0.3, 0, 0.8, 0.15],
      standard: [0.2, 0, 0, 1],
      linear: [0, 0, 1, 1],
    },
    duration: { short2: 100, short4: 200, medium1: 250, medium2: 300, controls: 180, skeletonPulse: 1200 },
  },
};

writeFileSync(join(root, "tokens.json"), JSON.stringify(tokens, null, 2) + "\n");

const block = (sel, scheme) =>
  `${sel} {\n` +
  Object.entries(scheme)
    .map(([k, v]) => `  --${kebab(k)}: ${v};`)
    .join("\n") +
  "\n}\n";

let css = "/* АВТОГЕНЕРАЦИЯ: design/tools/build_tokens.mjs. Не править руками. */\n\n";
css += block(":root, .dark", schemes.red.dark) + "\n" + block(".light", schemes.red.light);
for (const s of seeds.slice(1)) {
  css += "\n" + block(`.seed-${s.id}, .seed-${s.id}.dark`, schemes[s.id].dark);
  css += "\n" + block(`.seed-${s.id}.light`, schemes[s.id].light);
}
css += "\n:root {\n" + seeds.map((s) => `  --seed-${s.id}: ${s.hex};`).join("\n") + "\n}\n";
writeFileSync(join(root, "assets", "tokens.css"), css);
console.log("tokens.json, assets/tokens.css — готово");
