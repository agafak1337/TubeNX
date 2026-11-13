#!/usr/bin/env node
// Заполняет в screens/01-tokens.html блоки между <!-- gen:NAME --> и <!-- /gen:NAME -->
// данными из tokens.json: плитки ролей, таблица контраста, seed-палитры.
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { allTables, hex } from "./contrast.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const tokens = JSON.parse(readFileSync(join(here, "..", "tokens.json"), "utf8"));
const pagePath = join(here, "..", "screens", "01-tokens.html");

const kebab = (s) => s.replace(/[A-Z]/g, (c) => "-" + c.toLowerCase());
const v = (role) => `var(--${kebab(role)})`;

// Роль → роль текста на ней.
function onRole(role) {
  if (role === "shadow" || role === "scrim") return null;
  if (role === "inverseSurface") return "inverseOnSurface";
  if (role === "inverseOnSurface" || role === "inversePrimary") return "inverseSurface";
  if (role === "outline") return "surface";
  if (role === "outlineVariant") return "onSurface";
  if (role === "surfaceVariant") return "onSurfaceVariant";
  if (role === "onSurfaceVariant") return "surfaceVariant";
  if (role.startsWith("surface")) return "onSurface";
  if (role.startsWith("on")) return role[2].toLowerCase() + role.slice(3);
  return "on" + role[0].toUpperCase() + role.slice(1);
}

function tile(scheme, role, cls = "") {
  const on = onRole(role);
  const color = on ? v(on) : "#fff";
  return `<div class="tile ${cls}" style="background:${v(role)}; color:${color}"><b>${role}</b><span>${hex(scheme[role])}</span></div>`;
}

function colorFrame(scheme) {
  const t = (role, cls) => tile(scheme, role, cls);
  const group = (name) => {
    const cap = name[0].toUpperCase() + name.slice(1);
    return `<div class="tcol">${t(name, "tall")}${t("on" + cap)}${t(name + "Container", "tall")}${t("on" + cap + "Container")}</div>`;
  };
  return `
        <div class="tgrid g4">${["primary", "secondary", "tertiary", "error"].map(group).join("")}</div>
        <div class="tgrid g6">${t("surfaceDim", "tall edge")}${t("surface", "tall edge")}${t("surfaceBright", "tall edge")}${t("inverseSurface", "tall")}${t("inverseOnSurface", "tall")}${t("inversePrimary", "tall")}</div>
        <div class="tgrid g5">${["surfaceContainerLowest", "surfaceContainerLow", "surfaceContainer", "surfaceContainerHigh", "surfaceContainerHighest"].map((r) => t(r, "tall edge")).join("")}</div>
        <div class="tgrid g6">${t("onSurface")}${t("onSurfaceVariant")}${t("surfaceVariant")}${t("outline")}${t("outlineVariant")}${t("background", "edge")}</div>
        <div class="tgrid g6">${t("onBackground")}${t("shadow", "edge")}${t("scrim", "edge")}</div>`;
}

function contrastRows() {
  const { dark, light } = allTables();
  const cell = (r, theme) => `
            <td><span class="${theme}"><span class="sample" style="background:${v(r.bg)}; color:${v(r.fg)}">${r.min < 4.5 ? '<i style="background:' + v(r.fg) + '"></i>' : "Текст Aa"}</span></span></td>
            <td class="num">${r.ratio.toFixed(2)}</td>
            <td><span class="verdict ${r.pass ? "ok" : "fail"}"><span class="icon s18">${r.pass ? "check" : "close"}</span>${r.pass ? "проходит" : "не проходит"}</span></td>`;
  return dark.map((d, i) => `
          <tr>
            <td><b>${d.fg}</b></td><td>${d.bg}</td><td class="num">${d.min === 3 ? "3 : 1" : "4,5 : 1"}</td>${cell(d, "dark")}${cell(light[i], "light")}
          </tr>`).join("").replace(/(\d)\.(\d\d)</g, "$1,$2<");
}

function seedRows() {
  const roles = ["primary", "primaryContainer", "secondaryContainer", "tertiary", "surface", "surfaceContainerLow"];
  return tokens.seedPalette.map((p) => `
        <div class="seedrow ${p.id === "red" ? "" : "seed-" + p.id} dark">
          <span class="dot" style="background:var(--seed-${p.id})"></span>
          <div class="who"><div class="title-m">${p.name}${p.default ? " · по умолчанию" : ""}</div><div class="body-s muted">seed ${hex(p.seed)} · ${p.default ? "без класса" : ".seed-" + p.id}</div></div>
          <div class="strip">${roles.map((r) => tile(p.dark, r, r.startsWith("surface") ? "edge" : "")).join("")}</div>
        </div>`).join("");
}

const blocks = {
  "color-dark": colorFrame(tokens.color.dark),
  "color-light": colorFrame(tokens.color.light),
  contrast: contrastRows(),
  seeds: seedRows(),
};

let html = readFileSync(pagePath, "utf8");
for (const [name, body] of Object.entries(blocks)) {
  const re = new RegExp(`(<!-- gen:${name} -->)[\\s\\S]*?(<!-- /gen:${name} -->)`);
  if (!re.test(html)) throw new Error(`нет блока gen:${name}`);
  html = html.replace(re, (_, a, b) => `${a}${body}\n        ${b}`);
}
writeFileSync(pagePath, html);
console.log("ok:", pagePath);
