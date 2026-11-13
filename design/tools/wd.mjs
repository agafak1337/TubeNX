#!/usr/bin/env node
// Рендер и аудит фреймов через geckodriver (один сеанс headless Firefox на всю страницу).
//   node wd.mjs frames <page.html>                — список фреймов: id, размер
//   node wd.mjs shoot  <page.html> <outdir> [id…] — PNG каждого фрейма (снимок элемента, размер = размер фрейма)
//   node wd.mjs audit  <page.html>                — машинная проверка: id, размеры, кегль, иконки, обрезка, эффекты, тексты, контраст
// Пути страниц — относительно design/.
import { spawn } from "node:child_process";
import { mkdirSync, writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve, isAbsolute } from "node:path";
import { fileURLToPath } from "node:url";

const DESIGN = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const [cmd, pageArg, ...rest] = process.argv.slice(2);
if (!cmd || !pageArg) { console.error("usage: wd.mjs frames|shoot|audit <page.html> …"); process.exit(2); }
const page = isAbsolute(pageArg) ? pageArg : join(DESIGN, pageArg);
const port = 4500 + (process.pid % 400);
const profile = mkdtempSync(join(tmpdir(), "tubenx-wd-"));
const gd = spawn("geckodriver", ["--port", String(port)], { stdio: "ignore" });

const base = `http://127.0.0.1:${port}`;
async function call(method, path, body) {
  const r = await fetch(base + path, { method, headers: { "content-type": "application/json" }, body: body ? JSON.stringify(body) : undefined });
  const j = await r.json();
  if (j.value && j.value.error) throw new Error(j.value.error + ": " + j.value.message);
  return j.value;
}
async function waitUp() {
  for (let i = 0; i < 100; i++) {
    try { await fetch(base + "/status"); return; } catch { await new Promise((r) => setTimeout(r, 100)); }
  }
  throw new Error("geckodriver не поднялся");
}

const ICONS = new Set(("add arrow_back arrow_forward bolt check chevron_right close cloud_off dark_mode delete error forward_10 fullscreen " +
  "gpp_good hd high_quality history home info keyboard light_mode lock more_vert movie music_note newspaper north_west open_in_new palette " +
  "pause play_arrow podcasts refresh remove replay replay_10 restart_alt schedule search search_off settings shield skip_next speed " +
  "sports_esports subtitles sync thumb_up trending_up tune video_library visibility volume_off volume_up wifi wifi_off").split(" "));

// Выполняется в странице (доска целиком, без ?frame).
function inPage(icons) {
  const allowed = new Set(icons);
  const out = { frames: [], issues: [] };
  const add = (frame, kind, what, el) => out.issues.push({ frame, kind, what, el });
  const desc = (el) => (el.tagName.toLowerCase() + (el.className && typeof el.className === "string" ? "." + el.className.trim().split(/\s+/).join(".") : "")).slice(0, 80);
  const parse = (c) => { const m = c.match(/rgba?\(([^)]+)\)/); if (!m) return null; const p = m[1].split(/[, /]+/).map(Number); return [p[0], p[1], p[2], p.length > 3 ? p[3] : 1]; };
  const lum = (c) => { const f = (v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; }; return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2]); };
  const over = (top, bot) => { const a = top[3]; return [0, 1, 2].map((i) => top[i] * a + bot[i] * (1 - a)).concat(1); };
  const ids = {};
  const frames = Array.from(document.querySelectorAll(".frame"));
  frames.forEach((f, n) => {
    const nested = !!f.parentElement.closest(".frame");
    const fid = f.id || "(без id #" + n + ")";
    out.frames.push({ id: f.id || null, w: f.offsetWidth, h: f.offsetHeight, nested, cls: f.className });
    if (!f.id && !nested) add(fid, "id", "фрейм без id");
    if (f.id) { if (ids[f.id]) add(fid, "id", "id не уникален"); ids[f.id] = 1; }
    if (nested) return;
    const fr = f.getBoundingClientRect();
    f.querySelectorAll("*").forEach((el) => {
      const cs = getComputedStyle(el);
      if (cs.display === "none" || cs.visibility === "hidden") return;
      const r = el.getBoundingClientRect();
      if (!r.width || !r.height) return;
      const isIcon = el.classList.contains("icon");
      if (cs.backdropFilter && cs.backdropFilter !== "none") add(fid, "effect", "backdrop-filter", desc(el));
      if (cs.filter && /blur/.test(cs.filter)) add(fid, "effect", "filter blur", desc(el));
      if (cs.fontStyle === "italic") add(fid, "effect", "курсив", desc(el));
      if (cs.boxShadow && cs.boxShadow !== "none" && !/inset/.test(cs.boxShadow)) add(fid, "shadow", cs.boxShadow, desc(el));
      if (cs.animationName && cs.animationName !== "none") add(fid, "anim", cs.animationName, desc(el));
      if (isIcon) {
        const t = el.textContent.trim();
        if (!allowed.has(t)) add(fid, "icon", "нет в сабсете: " + t, desc(el));
        return;
      }
      const own = Array.from(el.childNodes).filter((c) => c.nodeType === 3 && c.textContent.trim()).map((c) => c.textContent.trim()).join(" ");
      if (!own) return;
      if (el.closest(".redline, .safe, .note-board")) return;
      const fs = parseFloat(cs.fontSize);
      if (fs < 12) add(fid, "font", "кегль " + fs + " px: «" + own.slice(0, 40) + "»", desc(el));
      // вылезание за фрейм и обрезка без многоточия
      let clipper = el, clipped = false, ell = false;
      for (let p = el; p && p !== f; p = p.parentElement) {
        const ps = getComputedStyle(p);
        if (ps.textOverflow === "ellipsis" || ps.webkitLineClamp !== "none") ell = true;
        if (ps.overflowX !== "visible" || ps.overflowY !== "visible") {
          if (p.scrollWidth > p.clientWidth + 1 || p.scrollHeight > p.clientHeight + 1) { clipped = true; clipper = p; break; }
        }
      }
      if (clipped && !ell && clipper === el) add(fid, "clip", "текст обрезан без многоточия: «" + own.slice(0, 50) + "»", desc(el));
      const range = document.createRange(); range.selectNodeContents(el);
      const tr = range.getBoundingClientRect();
      if (!clipped && (tr.right > fr.right + 1 || tr.bottom > fr.bottom + 1 || tr.left < fr.left - 1 || tr.top < fr.top - 1))
        add(fid, "overflow", "текст за границей фрейма: «" + own.slice(0, 50) + "»", desc(el));
      // тексты §7
      if (/!/.test(own)) add(fid, "text", "восклицательный знак: «" + own.slice(0, 60) + "»");
      if (/(^|[^.…?»]\s)Вы(\s|$)|\sВам\s|\sВаш/.test(own)) add(fid, "text", "«Вы» с большой: «" + own.slice(0, 60) + "»");
      if (/[\u{1F300}-\u{1FAFF}\u{2600}-\u{27BF}]/u.test(own)) add(fid, "text", "emoji/символ: «" + own.slice(0, 60) + "»");
      if (/Материал 3/.test(own)) add(fid, "text", "«Материал 3»");
      if (/(^|[^\d#.,:×xA-Fa-f])\d{5,}(?![\dpPx])/.test(own) && !/#[0-9A-Fa-f]{6}/.test(own)) add(fid, "text", "тысячи без пробела: «" + own.slice(0, 60) + "»");
      // контраст (только сплошные фоны)
      let col = parse(cs.color); if (!col) return;
      let op = 1, layers = [], unknown = false;
      for (let p = el; p; p = p.parentElement) {
        const ps = getComputedStyle(p);
        op *= parseFloat(ps.opacity);
        if (ps.backgroundImage !== "none") { unknown = true; break; }
        const b = parse(ps.backgroundColor);
        if (b && b[3] > 0) { layers.push(b); if (b[3] === 1) break; }
        if (p === f) break;
      }
      if (unknown || !layers.length || layers[layers.length - 1][3] < 1) return;
      if (el.closest(".th, .cover, .frame.video")) return;
      let bg = layers.pop(); while (layers.length) bg = over(layers.pop(), bg);
      col = over([col[0], col[1], col[2], col[3] * op], bg);
      const a = lum(col), b = lum(bg), cr = (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
      const large = fs >= 24 || (fs >= 18.66 && parseInt(cs.fontWeight) >= 700);
      if (cr < (large ? 3 : 4.5)) add(fid, "contrast", cr.toFixed(2) + ":1 (нужно " + (large ? 3 : 4.5) + ") «" + own.slice(0, 40) + "»" + (op < 1 ? " [opacity " + op.toFixed(2) + "]" : ""), desc(el));
    });
  });
  return out;
}

let session;
try {
  await waitUp();
  const s = await call("POST", "/session", { capabilities: { alwaysMatch: { "moz:firefoxOptions": { args: ["-headless", "-profile", profile], prefs: { "layout.css.devPixelsPerPx": "1.0" } } } } });
  session = "/session/" + s.sessionId;
  await call("POST", session + "/window/rect", { width: 2200, height: 1400 });
  const go = async (q) => { await call("POST", session + "/url", { url: "file://" + page + (q || "") }); await call("POST", session + "/execute/async", { script: "var d=arguments[arguments.length-1];document.fonts.ready.then(function(){requestAnimationFrame(function(){requestAnimationFrame(function(){d(1)})})})", args: [] }); };
  await go();
  const res = await call("POST", session + "/execute/sync", { script: "return (" + inPage.toString() + ")(arguments[0])", args: [[...ICONS]] });
  if (cmd === "frames") {
    console.log(JSON.stringify(res.frames.filter((f) => !f.nested), null, 1));
  } else if (cmd === "audit") {
    console.log(JSON.stringify(res, null, 1));
  } else if (cmd === "shoot") {
    const outdir = isAbsolute(rest[0]) ? rest[0] : join(DESIGN, rest[0]);
    const only = rest.slice(1);
    mkdirSync(outdir, { recursive: true });
    for (const f of res.frames) {
      if (f.nested || !f.id || (only.length && !only.includes(f.id))) continue;
      await go("?frame=" + f.id);
      const el = await call("POST", session + "/element", { using: "css selector", value: "#" + f.id });
      const eid = Object.values(el)[0];
      const png = await call("GET", session + "/element/" + eid + "/screenshot");
      writeFileSync(join(outdir, f.id + ".png"), Buffer.from(png, "base64"));
      console.log(f.id + ".png " + f.w + "x" + f.h);
    }
  } else throw new Error("unknown command " + cmd);
} finally {
  try { if (session) await call("DELETE", session); } catch {}
  gd.kill();
  rmSync(profile, { recursive: true, force: true });
}
