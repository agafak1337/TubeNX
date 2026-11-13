// Мини-утилита PNG без зависимостей (только node:zlib).
//   node png_tool.mjs info <in.png>                      — размер, тип цвета, min альфа, min альфа по краю
//   node png_tool.mjs alpha <on-black.png> <on-white.png> <out.png>
//        восстановить прозрачность из двух рендеров одного знака: на чёрном и на белом фоне
//   node png_tool.mjs sheet <out.png> <scale> <in1.png> [in2.png …]
//        контактный лист: картинки в ряд, увеличение без сглаживания (смотреть мелкие размеры)
import { readFileSync, writeFileSync } from "node:fs";
import { inflateSync, deflateSync, crc32 } from "node:zlib";

export function decode(file) {
  const b = readFileSync(file);
  let o = 8, w, h, ct, bd, idat = [];
  while (o < b.length) {
    const len = b.readUInt32BE(o), type = b.toString("latin1", o + 4, o + 8), data = b.subarray(o + 8, o + 8 + len);
    if (type === "IHDR") { w = data.readUInt32BE(0); h = data.readUInt32BE(4); bd = data[8]; ct = data[9]; if (data[12]) throw new Error("interlaced"); }
    if (type === "IDAT") idat.push(data);
    o += 12 + len;
  }
  if (bd !== 8 || (ct !== 2 && ct !== 6)) throw new Error(`unsupported PNG: depth ${bd}, color type ${ct}`);
  const bpp = ct === 6 ? 4 : 3, raw = inflateSync(Buffer.concat(idat)), stride = w * bpp;
  const px = Buffer.alloc(w * h * 4), cur = Buffer.alloc(stride), prev = Buffer.alloc(stride);
  for (let y = 0; y < h; y++) {
    const ft = raw[y * (stride + 1)], line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? cur[i - bpp] : 0, up = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      let v = line[i];
      if (ft === 1) v += a; else if (ft === 2) v += up; else if (ft === 3) v += (a + up) >> 1;
      else if (ft === 4) { const p = a + up - c, pa = Math.abs(p - a), pb = Math.abs(p - up), pc = Math.abs(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? up : c; }
      cur[i] = v & 255;
    }
    for (let x = 0; x < w; x++) {
      px[(y * w + x) * 4] = cur[x * bpp]; px[(y * w + x) * 4 + 1] = cur[x * bpp + 1]; px[(y * w + x) * 4 + 2] = cur[x * bpp + 2];
      px[(y * w + x) * 4 + 3] = bpp === 4 ? cur[x * bpp + 3] : 255;
    }
    cur.copy(prev);
  }
  return { w, h, ct, px };
}

export function encode(file, w, h, px, alpha = true) {
  const bpp = alpha ? 4 : 3, raw = Buffer.alloc(h * (w * bpp + 1));
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) for (let c = 0; c < bpp; c++)
    raw[y * (w * bpp + 1) + 1 + x * bpp + c] = px[(y * w + x) * 4 + c];
  const chunk = (type, data) => {
    const out = Buffer.alloc(12 + data.length);
    out.writeUInt32BE(data.length, 0); out.write(type, 4, "latin1"); data.copy(out, 8);
    out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)) >>> 0, 8 + data.length);
    return out;
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = alpha ? 6 : 2;
  writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw, { level: 9 })), chunk("IEND", Buffer.alloc(0))]));
}

const [cmd, ...a] = process.argv.slice(2);
if (cmd === "info") {
  const { w, h, ct, px } = decode(a[0]);
  let minA = 255, minEdge = 255, transparent = 0;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const al = px[(y * w + x) * 4 + 3];
    if (al < minA) minA = al;
    if (al === 0) transparent++;
    if ((x === 0 || y === 0 || x === w - 1 || y === h - 1) && al < minEdge) minEdge = al;
  }
  const at = (x, y) => "#" + px.subarray((y * w + x) * 4, (y * w + x) * 4 + 4).toString("hex");
  console.log(`${a[0]}: ${w}x${h} colorType=${ct} minAlpha=${minA} minEdgeAlpha=${minEdge} fullyTransparent=${transparent}/${w * h} corner=${at(0, 0)} center=${at(w >> 1, h >> 1)}`);
} else if (cmd === "alpha") {
  const k = decode(a[0]), wt = decode(a[1]);
  if (k.w !== wt.w || k.h !== wt.h) throw new Error("size mismatch");
  const out = Buffer.alloc(k.px.length);
  for (let i = 0; i < k.px.length; i += 4) {
    const al = Math.max(0, Math.min(255, 255 - Math.round(((wt.px[i] - k.px[i]) + (wt.px[i + 1] - k.px[i + 1]) + (wt.px[i + 2] - k.px[i + 2])) / 3)));
    for (let c = 0; c < 3; c++) out[i + c] = al ? Math.min(255, Math.round((k.px[i + c] * 255) / al)) : 255;
    out[i + 3] = al;
  }
  encode(a[2], k.w, k.h, out, true);
} else if (cmd === "sheet") {
  const [outFile, sc, ...ins] = a, s = +sc, imgs = ins.map(decode), pad = 16;
  const W = imgs.reduce((t, i) => t + i.w * s + pad, pad), H = Math.max(...imgs.map((i) => i.h * s)) + pad * 2;
  const out = Buffer.alloc(W * H * 4, 0x60);
  let ox = pad;
  for (const im of imgs) {
    for (let y = 0; y < im.h * s; y++) for (let x = 0; x < im.w * s; x++) {
      const si = (((y / s) | 0) * im.w + ((x / s) | 0)) * 4, di = ((y + pad) * W + ox + x) * 4, al = im.px[si + 3] / 255;
      for (let c = 0; c < 3; c++) out[di + c] = Math.round(im.px[si + c] * al + 0x60 * (1 - al));
    }
    ox += im.w * s + pad;
  }
  for (let i = 3; i < out.length; i += 4) out[i] = 255;
  encode(outFile, W, H, out, false);
} else if (cmd) {
  throw new Error("unknown command " + cmd);
}
