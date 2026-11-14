// Генератор SVG иконки TubeNX (ТЗ §6): node design/tools/build_icon.mjs
// Пишет design/export/icon/{icon.svg, icon-mono.svg, variants/*.svg} и печатает параметры построения.
import { writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const OUT = join(dirname(fileURLToPath(import.meta.url)), "..", "export", "icon");
mkdirSync(join(OUT, "variants"), { recursive: true });

export const P = {
  size: 1024,
  margin: 32,        // поле между squircle и краем холста
  n: 5,              // показатель суперэллипса: диагональ совпадает с rect при r = 22 % стороны
  c0: "#C3002F", c1: "#7A0020",
  back: "#3A000F",   // подложка до краёв (вариант A)
  triH: 416,         // вертикальная сторона треугольника
  triW: 364,         // ширина по вершинам
  triR: 14,          // скругление: 3.5 px в масштабе 256
  shift: 0.025,      // оптический сдвиг вправо, доля холста
};

const f = (v) => (Math.round(v * 100) / 100).toString();

// Суперэллипс |x/a|^n + |y/a|^n = 1 как 32 кубических Безье (касательные из градиента, длина плеч — как у дуги).
export function squircle(cx, cy, a, n, perQuad = 8) {
  const pts = [];
  const N = perQuad * 4;
  for (let i = 0; i < N; i++) {
    const t = (i / N) * 2 * Math.PI;
    const c = Math.cos(t), s = Math.sin(t);
    const x = a * Math.sign(c) * Math.abs(c) ** (2 / n);
    const y = a * Math.sign(s) * Math.abs(s) ** (2 / n);
    let tx = -Math.sign(y) * Math.abs(y / a) ** (n - 1);
    let ty = Math.sign(x) * Math.abs(x / a) ** (n - 1);
    const l = Math.hypot(tx, ty);
    pts.push({ x: cx + x, y: cy + y, tx: tx / l, ty: ty / l });
  }
  let d = `M${f(pts[0].x)} ${f(pts[0].y)}`;
  for (let i = 0; i < N; i++) {
    const p = pts[i], q = pts[(i + 1) % N];
    const chord = Math.hypot(q.x - p.x, q.y - p.y);
    const th = Math.acos(Math.max(-1, Math.min(1, p.tx * q.tx + p.ty * q.ty)));
    const h = th < 1e-6 ? chord / 3 : (chord / (2 * Math.sin(th / 2))) * (4 / 3) * Math.tan(th / 4);
    d += `C${f(p.x + p.tx * h)} ${f(p.y + p.ty * h)} ${f(q.x - q.tx * h)} ${f(q.y - q.ty * h)} ${f(q.x)} ${f(q.y)}`;
  }
  return d + "Z";
}

// Многоугольник со скруглёнными дугами углами (обход по часовой стрелке в экранных координатах).
export function roundPoly(v, r) {
  const n = v.length;
  let d = "";
  for (let i = 0; i < n; i++) {
    const p = v[(i + n - 1) % n], c = v[i], q = v[(i + 1) % n];
    const u1 = [p[0] - c[0], p[1] - c[1]], u2 = [q[0] - c[0], q[1] - c[1]];
    const l1 = Math.hypot(...u1), l2 = Math.hypot(...u2);
    const ang = Math.acos((u1[0] * u2[0] + u1[1] * u2[1]) / (l1 * l2));
    const t = r / Math.tan(ang / 2);
    const a = [c[0] + (u1[0] / l1) * t, c[1] + (u1[1] / l1) * t];
    const b = [c[0] + (u2[0] / l2) * t, c[1] + (u2[1] / l2) * t];
    d += `${i ? "L" : "M"}${f(a[0])} ${f(a[1])}A${r} ${r} 0 0 1 ${f(b[0])} ${f(b[1])}`;
  }
  return d + "Z";
}

// Треугольник: видимый габарит (с учётом скругления носика) сдвинут вправо на shift от центра холста.
export function triangle(p = P) {
  const tip = p.triR / Math.sin(Math.atan2(p.triH / 2, p.triW)) - p.triR;
  const visW = p.triW - tip;
  const left = p.size / 2 + p.shift * p.size - visW / 2;
  const top = p.size / 2 - p.triH / 2;
  const v = [[left, top], [left + p.triW, p.size / 2], [left, top + p.triH]];
  return { d: roundPoly(v, p.triR), left, top, visW, right: left + visW, v };
}

const S = P.size, A = S / 2 - P.margin;
const sq = squircle(S / 2, S / 2, A, P.n);
const tri = triangle();
const head = `<svg xmlns="http://www.w3.org/2000/svg" width="${S}" height="${S}" viewBox="0 0 ${S} ${S}">`;
const grad = (x0, x1) => `<defs><linearGradient id="g" gradientUnits="userSpaceOnUse" x1="${x0}" y1="${x0}" x2="${x1}" y2="${x1}"><stop offset="0" stop-color="${P.c0}"/><stop offset="1" stop-color="${P.c1}"/></linearGradient></defs>`;

const iconA = `${head}
<title>TubeNX</title>
${grad(P.margin, S - P.margin)}
<rect width="${S}" height="${S}" fill="${P.back}"/>
<path fill="url(#g)" d="${sq}"/>
<path fill="#FFFFFF" d="${tri.d}"/>
</svg>
`;

const iconB = `${head}
<title>TubeNX — вариант B</title>
${grad(0, S)}
<rect width="${S}" height="${S}" fill="url(#g)"/>
<path fill="#FFFFFF" d="${tri.d}"/>
</svg>
`;

// Вариант-идея «N»: play-треугольник = левая стойка и диагональ, справа стойка; между ними контрформа.
const nH = 416, nTop = S / 2 - nH / 2, nL = 300, stemW = 104, gap = 44, nR = 724;
const triN = roundPoly([[nL, nTop], [nR - stemW - gap, S / 2], [nL, nTop + nH]], P.triR);
const stemN = roundPoly([[nR - stemW, nTop], [nR, nTop], [nR, nTop + nH], [nR - stemW, nTop + nH]], P.triR);
const iconN = `${head}
<title>TubeNX — вариант N</title>
${grad(P.margin, S - P.margin)}
<rect width="${S}" height="${S}" fill="${P.back}"/>
<path fill="url(#g)" d="${sq}"/>
<path fill="#FFFFFF" d="${triN}${stemN}"/>
</svg>
`;

// Монохром: белый squircle с вырезанным треугольником, фон прозрачный.
const mono = `${head}
<title>TubeNX — монохром</title>
<path fill="#FFFFFF" fill-rule="evenodd" d="${sq}${tri.d}"/>
</svg>
`;

writeFileSync(join(OUT, "icon.svg"), iconA);
writeFileSync(join(OUT, "icon-mono.svg"), mono);
writeFileSync(join(OUT, "variants", "icon-a-squircle.svg"), iconA);
writeFileSync(join(OUT, "variants", "icon-b-fullbleed.svg"), iconB);
writeFileSync(join(OUT, "variants", "icon-n-idea.svg"), iconN);

const diag = A * 2 ** (-1 / P.n);
console.log(JSON.stringify({
  squircle: { a: A, side: 2 * A, n: P.n, eqRadius: +((A - diag) / (1 - Math.SQRT1_2)).toFixed(1), eqRadiusPct: +(((A - diag) / (1 - Math.SQRT1_2)) / (2 * A) * 100).toFixed(1) },
  triangle: { left: +tri.left.toFixed(1), right: +tri.right.toFixed(1), top: tri.top, bottom: tri.top + P.triH, visCenterX: +((tri.left + tri.right) / 2).toFixed(1), centroidX: +(tri.left + P.triW / 3).toFixed(1) },
}, null, 1));
