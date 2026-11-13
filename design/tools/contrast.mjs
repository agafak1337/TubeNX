#!/usr/bin/env node
// Контраст ключевых пар текст/фон по WCAG 2.x (relative luminance) из design/tokens.json.
//   node design/tools/contrast.mjs          — таблица в консоль
//   node design/tools/contrast.mjs --json   — то же в JSON
// Код возврата 1, если текстовая пара ниже 4.5:1 (или неконтентная ниже 3:1).
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const tokens = JSON.parse(readFileSync(join(here, "..", "tokens.json"), "utf8"));

const TEXT = 4.5;
const NON_TEXT = 3;

const surfaces = ["surface", "surfaceContainerLow", "surfaceContainer", "surfaceContainerHigh", "surfaceContainerHighest"];
export const pairs = [
  ...surfaces.map((bg) => ["onSurface", bg, TEXT]),
  ...surfaces.map((bg) => ["onSurfaceVariant", bg, TEXT]),
  ["onPrimary", "primary", TEXT],
  ["onPrimaryContainer", "primaryContainer", TEXT],
  ["onSecondaryContainer", "secondaryContainer", TEXT],
  ["onTertiaryContainer", "tertiaryContainer", TEXT],
  ["onErrorContainer", "errorContainer", TEXT],
  ["primary", "surface", TEXT],
  ["primary", "surfaceContainerLow", TEXT],
  ["error", "surface", TEXT],
  ["error", "surfaceContainerLow", TEXT],
  ["tertiary", "surface", TEXT],
  ["tertiary", "surfaceContainerLow", TEXT],
  ["inverseOnSurface", "inverseSurface", TEXT],
  ["inversePrimary", "inverseSurface", TEXT],
  ["outline", "surface", NON_TEXT],
];

// #AARRGGBB → [r, g, b]
function rgb(argb) {
  const h = argb.replace("#", "");
  const s = h.length === 8 ? h.slice(2) : h;
  return [0, 2, 4].map((i) => parseInt(s.slice(i, i + 2), 16));
}

function luminance(argb) {
  const [r, g, b] = rgb(argb).map((v) => {
    const c = v / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function contrast(a, b) {
  const la = luminance(a);
  const lb = luminance(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}

export function hex(argb) {
  return "#" + argb.replace("#", "").slice(-6).toUpperCase();
}

// Округление вниз до сотых: 4.499 не должно показываться как 4.50.
const floor2 = (v) => Math.floor(v * 100) / 100;

export function table(scheme) {
  return pairs.map(([fg, bg, min]) => {
    const ratio = contrast(scheme[fg], scheme[bg]);
    return { fg, bg, fgHex: hex(scheme[fg]), bgHex: hex(scheme[bg]), min, ratio: floor2(ratio), pass: ratio >= min };
  });
}

export function allTables() {
  return { dark: table(tokens.color.dark), light: table(tokens.color.light) };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const t = allTables();
  if (process.argv.includes("--json")) {
    console.log(JSON.stringify(t, null, 2));
  } else {
    const pad = (s, n) => String(s).padEnd(n);
    console.log(pad("Текст / знак", 22) + pad("Фон", 26) + pad("Порог", 7) + pad("Тёмная", 16) + "Светлая");
    t.dark.forEach((d, i) => {
      const l = t.light[i];
      const cell = (r) => pad(r.ratio.toFixed(2) + (r.pass ? "  ок" : "  НЕ ПРОХОДИТ"), 16);
      console.log(pad(d.fg, 22) + pad(d.bg, 26) + pad(d.min + ":1", 7) + cell(d) + cell(l));
    });
  }
  const failed = [...t.dark.map((r) => ({ ...r, theme: "dark" })), ...t.light.map((r) => ({ ...r, theme: "light" }))].filter((r) => !r.pass);
  if (failed.length) {
    console.error(`\nНе проходит: ${failed.length}`);
    failed.forEach((r) => console.error(`  ${r.theme}: ${r.fg} на ${r.bg} — ${r.ratio.toFixed(2)} (нужно ${r.min})`));
    process.exit(1);
  }
  console.error("\nВсе пары проходят.");
}
