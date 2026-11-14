#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TubeNX icon generator — Material 3 / Material You squircle icon.

Pure Python 3, standard library only (no PIL, no numpy, no ImageMagick).
The 256x256 master is drawn at 1024x1024 with analytic anti-aliasing and
box-downsampled 4x4 -> 256x256.

Outputs (relative to the repository root):
    resources/images/icon.png   256x256 RGBA, transparent outside the squircle
    resources/images/icon.jpg   256x256 baseline JPEG (full-bleed for elf2nro)

Design
------
  * full-bleed superellipse "squircle" (n = 4.2), M3 tonal gradient
    primary #B3261E -> warm tertiary, with a primaryFixedDim aurora glow
  * a white rounded "screen" (56% width) with a primary-colored play glyph
    with round joins, soft drop shadow and an inner top rim highlight
  * one tertiaryContainer "point of interest" dot in the lower right

Everything is deterministic: no random numbers anywhere.
"""

import math
import os
import struct
import zlib

OUT = 256          # final icon size, px
SS = 4             # supersampling factor (4x4 = 16 samples / px)
S = OUT * SS       # master size, 1024

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG_DIR = os.path.join(ROOT, "resources", "images")

# --------------------------------------------------------------------------
# M3 light scheme derived from seed #FF0033 (YouTube red)
# --------------------------------------------------------------------------
PRIMARY = (0xB3, 0x26, 0x1E)            # primary
TERTIARY = (0x7D, 0x52, 0x60)           # tertiary
PRIMARY_CONTAINER = (0xFF, 0xDA, 0xD6)  # primaryContainer
PRIMARY_FIXED_DIM = (0xFF, 0xB4, 0xAB) # primaryFixedDim (aurora glow)
TERTIARY_CONTAINER = (0xFF, 0xD8, 0xE4) # point-of-interest accent
ON_PRIMARY = (0xFF, 0xFF, 0xFF)         # screen

HALF = OUT / 2.0
SQ_N = 4.2                              # superellipse exponent
PIX = 1.0 / SS                          # one sample, in icon units

# central "screen"
SCREEN_W = 0.56 * OUT
SCREEN_H = SCREEN_W / 1.42
SCX, SCY = HALF, 125.0
SCREEN_R = 0.30 * SCREEN_H
SHW, SHH = SCREEN_W / 2.0, SCREEN_H / 2.0
SL, SR = SCX - SHW, SCX + SHW
ST, SB = SCY - SHH, SCY + SHH

# play glyph (equilateral, rounded joins)
TRI_H = 0.52 * SCREEN_H
TRI_W = TRI_H * math.sqrt(3.0) / 2.0
TRI_CX, TRI_CY = SCX + 2.5, SCY          # small optical shift to the right
TRI_R = 2.5
TRI_POLY = [
    (TRI_CX - TRI_W / 2.0, TRI_CY - TRI_H / 2.0),
    (TRI_CX - TRI_W / 2.0, TRI_CY + TRI_H / 2.0),
    (TRI_CX + TRI_W / 2.0, TRI_CY),
]

# accent dot
DOT_CX, DOT_CY, DOT_R = 206.0, 201.0, 13.5

# shadow
SH_DY, SH_SIGMA, SH_ALPHA = 6.0, 6.5, 0.22

# aurora
AUR_X, AUR_Y = 221.0, 9.0
AUR_RX, AUR_RY = 116.0, 98.0
AUR_OP = 0.35

RIM_ALPHA = 0.20
RIM_WIDTH = 1.5


def clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def prepare_rounded_poly(poly, r):
    """Offset a convex polygon's edges inward by r.

    Returns (planes, vertices) of the inset polygon. Planes carry the
    *outward* normal, so max_i dot(p - a_i, n_i) is negative inside.
    Dilating this inset polygon by r (see make_rounded_tri_sdf) gives the
    original polygon with radius-r rounded joins.
    """
    n = len(poly)
    cx = sum(p[0] for p in poly) / n
    cy = sum(p[1] for p in poly) / n
    lines = []
    for i in range(n):
        ax, ay = poly[i]
        bx, by = poly[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        ln = math.hypot(ex, ey)
        nx, ny = -ey / ln, ex / ln
        if nx * (cx - ax) + ny * (cy - ay) < 0.0:
            nx, ny = -nx, -ny
        # offset the line inward, but keep the outward normal for the
        # "negative inside" plane test
        lines.append((ax + nx * r, ay + ny * r, -nx, -ny))
    verts = []
    for i in range(n):
        ax, ay, nx, ny = lines[i]
        bx, by, mx, my = lines[(i + 1) % n]
        d1x, d1y = -ny, nx
        d2x, d2y = -my, mx
        den = d1x * d2y - d1y * d2x
        t = ((bx - ax) * d2y - (by - ay) * d2x) / den
        verts.append((ax + d1x * t, ay + d1y * t))
    return lines, verts


def make_rounded_tri_sdf(poly, r):
    """Exact signed distance of a triangle with radius-r rounded joins.

    The rounded triangle is the inset triangle dilated by a disk of radius r,
    so its SDF is  sd(inset_triangle) - r.
    """
    lines, verts = prepare_rounded_poly(poly, r)
    segs = [(verts[i], verts[(i + 1) % len(verts)]) for i in range(len(verts))]
    sqrt = math.sqrt

    def sdf(x, y):
        d = -1e9
        for ax, ay, nx, ny in lines:
            v = (x - ax) * nx + (y - ay) * ny
            if v > d:
                d = v
        if d <= 0.0:
            return d - r
        best = 1e9
        for (ax, ay), (bx, by) in segs:
            ex = bx - ax
            ey = by - ay
            t = ((x - ax) * ex + (y - ay) * ey) / (ex * ex + ey * ey)
            if t < 0.0:
                t = 0.0
            elif t > 1.0:
                t = 1.0
            dx = x - (ax + ex * t)
            dy = y - (ay + ey * t)
            dd = sqrt(dx * dx + dy * dy)
            if dd < best:
                best = dd
        return best - r

    return sdf


GRAD_STOPS = (
    (0.00, (0x8E, 0x1E, 0x19)),   # deep primary tone, bottom-left
    (0.55, (0xB3, 0x26, 0x1E)),   # primary
    (1.00, (0x9E, 0x3B, 0x4B)),   # tertiary pulled warm, top-right
)


def _smoothstep(t):
    return t * t * (3.0 - 2.0 * t)


def gradient_lut(n=2049):
    """Multi-stop tonal ramp; each segment is smoothstep-eased."""
    stops = [(p, tuple(c / 255.0 for c in rgb)) for p, rgb in GRAD_STOPS]
    lr = [0.0] * n
    lg = [0.0] * n
    lb = [0.0] * n
    ns = len(stops) - 1
    for i in range(n):
        t = i / (n - 1.0)
        k = 0
        while k < ns - 1 and t > stops[k + 1][0]:
            k += 1
        t0, c0 = stops[k]
        t1, c1 = stops[k + 1]
        f = _smoothstep(min(1.0, max(0.0, (t - t0) / (t1 - t0))))
        lr[i] = c0[0] + (c1[0] - c0[0]) * f
        lg[i] = c0[1] + (c1[1] - c0[1]) * f
        lb[i] = c0[2] + (c1[2] - c0[2]) * f
    return lr, lg, lb


def rr_sdf(x, y, cx, cy, hw, hh, r):
    """Signed distance to a rounded rectangle."""
    qx = abs(x - cx) - (hw - r)
    qy = abs(y - cy) - (hh - r)
    ax = qx if qx > 0.0 else 0.0
    ay = qy if qy > 0.0 else 0.0
    m = qx if qx > qy else qy
    if m > 0.0:
        m = 0.0
    return math.sqrt(ax * ax + ay * ay) + m - r

def render():
    """Render the master image and return 256x256 straight-alpha RGBA bytes."""
    W = S
    lr, lg, lb = gradient_lut()
    tri_sd = make_rounded_tri_sdf(TRI_POLY, TRI_R)

    pcr = PRIMARY_FIXED_DIM[0] / 255.0
    pcg = PRIMARY_FIXED_DIM[1] / 255.0
    pcb = PRIMARY_FIXED_DIM[2] / 255.0
    tcr = TERTIARY_CONTAINER[0] / 255.0
    tcg = TERTIARY_CONTAINER[1] / 255.0
    tcb = TERTIARY_CONTAINER[2] / 255.0

    inv_rx = 1.0 / AUR_RX
    inv_ry = 1.0 / AUR_RY
    n_grad = len(lr) - 1

    # per-column squircle terms (x only) and sample coordinates
    xs = [(i + 0.5) * PIX for i in range(W)]
    u4x = [0.0] * W
    u64x = [0.0] * W
    for i in range(W):
        u = abs(xs[i] - HALF) / HALF
        if u > 0.0:
            u4x[i] = u ** SQ_N
            u64x[i] = u ** (2.0 * SQ_N - 2.0)
    sq_scale = HALF / SQ_N

    # bboxes
    sx0, sx1 = SL - 30.0, SR + 30.0
    sy0, sy1 = ST - 26.0, SB + SH_DY + 28.0
    tx0, tx1 = TRI_CX - TRI_W / 2 - 3, TRI_CX + TRI_W / 2 + 3
    ty0, ty1 = TRI_CY - TRI_H / 2 - 3, TRI_CY + TRI_H / 2 + 3

    out = bytearray(OUT * OUT * 4)
    sqrt = math.sqrt
    erfc = math.erfc
    inv_sig = 1.0 / (SH_SIGMA * 1.4142135623730951)

    for oy in range(OUT):
        acc_r = [0.0] * OUT
        acc_g = [0.0] * OUT
        acc_b = [0.0] * OUT
        acc_a = [0.0] * OUT
        for sy in range(SS):
            y = oy + (sy + 0.5) * PIX
            vy = abs(y - HALF) / HALF
            if vy > 0.0:
                v4 = vy ** SQ_N
                v64 = vy ** (2.0 * SQ_N - 2.0)
            else:
                v4 = 0.0
                v64 = 0.0
            gbase = (OUT - y) * 4.0
            vy_aur = (y - AUR_Y) * inv_ry
            vy_aur2 = vy_aur * vy_aur
            rim_y = clamp01((118.0 - y) / 110.0) * RIM_ALPHA if y < 118.0 else 0.0
            shadow_y = y - SH_DY
            in_rows = sy0 < y < sy1
            for i in range(W):
                x = xs[i]

                # --- background: diagonal gradient + aurora -----------------
                k = int(gbase + x * 4.0 + 0.5)
                if k < 0:
                    k = 0
                elif k > n_grad:
                    k = n_grad
                r = lr[k]
                g = lg[k]
                b = lb[k]

                dx = (x - AUR_X) * inv_rx
                d2 = dx * dx + vy_aur2
                if d2 < 1.0:
                    s = 1.0 - d2
                    w = AUR_OP * s * sqrt(s)
                    iw = 1.0 - w
                    r = r * iw + pcr * w
                    g = g * iw + pcg * w
                    b = b * iw + pcb * w

                # --- squircle mask (first order distance) -------------------
                den = sqrt(u64x[i] + v64)
                if den > 1e-9:
                    sd = (u4x[i] + v4 - 1.0) * sq_scale / den
                else:
                    sd = -1000.0

                # --- inner rim highlight (upper part of the squircle) -------
                if rim_y > 0.0 and sd > -RIM_WIDTH - PIX:
                    c1 = 0.5 - sd / PIX
                    c2 = 0.5 - (sd + RIM_WIDTH) / PIX
                    if c1 > 0.0:
                        if c1 > 1.0:
                            c1 = 1.0
                        if c2 < 0.0:
                            c2 = 0.0
                        elif c2 > 1.0:
                            c2 = 1.0
                        band = (c1 - c2) * rim_y
                        if band > 0.0:
                            iw = 1.0 - band
                            r = r * iw + band
                            g = g * iw + band
                            b = b * iw + band

                # --- shadow + white screen ---------------------------------
                if in_rows and sx0 < x < sx1:
                    sd_sh = rr_sdf(x, shadow_y, SCX, SCY, SHW, SHH, SCREEN_R)
                    if -3.0 < sd_sh < 24.0:
                        a = SH_ALPHA * 0.5 * erfc(sd_sh * inv_sig)
                        if a > 0.0:
                            iw = 1.0 - a
                            r *= iw
                            g *= iw
                            b *= iw
                    sd_sc = rr_sdf(x, y, SCX, SCY, SHW, SHH, SCREEN_R)
                    if sd_sc < PIX:
                        cs = 0.5 - sd_sc / PIX
                        if cs > 0.0:
                            if cs > 1.0:
                                cs = 1.0
                            iw = 1.0 - cs
                            r = r * iw + ON_PRIMARY[0] / 255.0 * cs
                            g = g * iw + ON_PRIMARY[1] / 255.0 * cs
                            b = b * iw + ON_PRIMARY[2] / 255.0 * cs

                    # --- play glyph ---------------------------------------
                    if tx0 < x < tx1 and ty0 < y < ty1:
                        d = tri_sd(x, y)
                        if d < PIX:
                            ct = 0.5 - d / PIX
                            if ct > 0.0:
                                if ct > 1.0:
                                    ct = 1.0
                                iw = 1.0 - ct
                                r = r * iw + (PRIMARY[0] / 255.0) * ct
                                g = g * iw + (PRIMARY[1] / 255.0) * ct
                                b = b * iw + (PRIMARY[2] / 255.0) * ct

                # --- accent dot -------------------------------------------
                ddx = x - DOT_CX
                ddy = y - DOT_CY
                dr2 = ddx * ddx + ddy * ddy
                if dr2 < DOT_R * DOT_R * 2.25:
                    cd = (DOT_R - sqrt(dr2)) / PIX + 0.5
                    if cd > 0.0:
                        if cd > 1.0:
                            cd = 1.0
                        iw = 1.0 - cd
                        r = r * iw + tcr * cd
                        g = g * iw + tcg * cd
                        b = b * iw + tcb * cd

                # --- accumulate (premultiplied) ---------------------------
                a = 0.5 - sd / PIX
                if a > 1.0:
                    a = 1.0
                elif a < 0.0:
                    a = 0.0
                ox = i >> 2
                acc_r[ox] += r * a
                acc_g[ox] += g * a
                acc_b[ox] += b * a
                acc_a[ox] += a

        base = oy * OUT * 4
        for ox in range(OUT):
            a = acc_a[ox] * 0.0625
            if a <= 0.0:
                # outside the squircle: keep the background colour so the
                # opaque JPEG export stays full-bleed (elf2nro has no alpha)
                x = ox + 0.5
                y = oy + 0.5
                k = int((x + (OUT - y)) * 4.0 + 0.5)
                if k < 0:
                    k = 0
                elif k > n_grad:
                    k = n_grad
                rr = lr[k]
                gg = lg[k]
                bb = lb[k]
                dx = (x - AUR_X) * inv_rx
                dy = (y - AUR_Y) * inv_ry
                d2 = dx * dx + dy * dy
                if d2 < 1.0:
                    w = AUR_OP * (1.0 - d2) * sqrt(1.0 - d2)
                    iw = 1.0 - w
                    rr = rr * iw + pcr * w
                    gg = gg * iw + pcg * w
                    bb = bb * iw + pcb * w
                out[base + ox * 4 + 0] = int(rr * 255.0 + 0.5)
                out[base + ox * 4 + 1] = int(gg * 255.0 + 0.5)
                out[base + ox * 4 + 2] = int(bb * 255.0 + 0.5)
                out[base + ox * 4 + 3] = 0
                continue
            inv = 1.0 / (acc_a[ox])
            rr = acc_r[ox] * inv
            gg = acc_g[ox] * inv
            bb = acc_b[ox] * inv
            out[base + ox * 4 + 0] = int(rr * 255.0 + 0.5) if rr < 1.0 else 255
            out[base + ox * 4 + 1] = int(gg * 255.0 + 0.5) if gg < 1.0 else 255
            out[base + ox * 4 + 2] = int(bb * 255.0 + 0.5) if bb < 1.0 else 255
            out[base + ox * 4 + 3] = int(a * 255.0 + 0.5) if a < 1.0 else 255
    return bytes(out)


# --------------------------------------------------------------------------
# PNG writer
# --------------------------------------------------------------------------
def _chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _paeth(a, b, c):
    p = a + b - c
    pa = p - a
    if pa < 0:
        pa = -pa
    pb = p - b
    if pb < 0:
        pb = -pb
    pc = p - c
    if pc < 0:
        pc = -pc
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def write_png(path, w, h, pixels):
    stride = w * 4
    raw = bytearray()
    prev = bytes(stride)
    for y in range(h):
        row = pixels[y * stride:(y + 1) * stride]
        best = None
        best_sum = None
        best_type = 0
        for ft in (0, 1, 2, 4):
            buf = bytearray(stride)
            if ft == 0:
                buf[:] = row
            elif ft == 1:
                for i in range(stride):
                    buf[i] = (row[i] - (row[i - 4] if i >= 4 else 0)) & 255
            elif ft == 2:
                for i in range(stride):
                    buf[i] = (row[i] - prev[i]) & 255
            else:
                for i in range(stride):
                    a = row[i - 4] if i >= 4 else 0
                    c = prev[i - 4] if i >= 4 else 0
                    buf[i] = (row[i] - _paeth(a, prev[i], c)) & 255
            s = 0
            for v in buf:
                s += v if v < 128 else 256 - v
            if best_sum is None or s < best_sum:
                best_sum = s
                best = buf
                best_type = ft
        raw.append(best_type)
        raw += best
        prev = row
    png = b"\x89PNG\r\n\x1a\n"
    png += _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += _chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += _chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


# --------------------------------------------------------------------------
# Baseline JPEG writer (4:4:4, quality 92) — no external tools required
# --------------------------------------------------------------------------
_ZZ = []
for _s in range(15):
    _idx = [(r, _s - r) for r in range(8) if 0 <= _s - r < 8]
    if _s % 2 == 0:
        _idx.reverse()
    _ZZ.extend(r * 8 + c for r, c in _idx)

_Q_LUM = [
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
]
_Q_CHR = [
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
]

_COS = []
for _u in range(8):
    _cu = 0.5 / math.sqrt(2.0) if _u == 0 else 0.5
    _COS.append([_cu * math.cos((2 * _x + 1) * _u * math.pi / 16.0)
                 for _x in range(8)])


class _BitWriter(object):
    __slots__ = ("out", "acc", "n")

    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def write(self, code, nbits):
        if not nbits:
            return
        self.acc = (self.acc << nbits) | (code & ((1 << nbits) - 1))
        self.n += nbits
        while self.n >= 8:
            self.n -= 8
            b = (self.acc >> self.n) & 0xFF
            self.out.append(b)
            if b == 0xFF:
                self.out.append(0x00)
        self.acc &= (1 << self.n) - 1

    def flush(self):
        if self.n:
            self.write((1 << (8 - self.n)) - 1, 8 - self.n)


def _huffman(freq, max_bits=16):
    """Canonical Huffman table (bits[17], huffval, codes) for a DHT segment.

    JPEG reserves the all-ones code word, so the code tree must keep one leaf
    free: a zero-weight dummy symbol is added, which the Huffman merge always
    pushes to the deepest level. It is then dropped from huffval, leaving the
    all-ones code unassigned (libjpeg rejects tables that use it).
    """
    import heapq

    syms = [s for s in range(256) if freq[s] > 0]
    if not syms:
        syms = [0]
        freq = list(freq)
        freq[0] = 1

    def depths(weights):
        nodes = []
        heap = []
        for sym, w in weights:
            nodes.append((None, None, sym))
            heapq.heappush(heap, (w, len(nodes) - 1))
        while len(heap) > 1:
            w1, i1 = heapq.heappop(heap)
            w2, i2 = heapq.heappop(heap)
            nodes.append((i1, i2, None))
            heapq.heappush(heap, (w1 + w2, len(nodes) - 1))
        dep = {}
        stack = [(heap[0][1], 0)]
        while stack:
            ni, d = stack.pop()
            left, right, sym = nodes[ni]
            if sym is not None:
                dep[sym] = max(1, d)
            else:
                stack.append((left, d + 1))
                stack.append((right, d + 1))
        return dep

    weights = [(s, max(1, freq[s])) for s in syms] + [(256, 0)]
    dep = depths(weights)
    if max(dep[s] for s in syms) > max_bits:
        weights = [(s, 1) for s in syms] + [(256, 0)]
        dep = depths(weights)

    bits = [0] * (max_bits + 1)
    huffval = []
    for l in range(1, max_bits + 1):
        for s in sorted(dep):
            if s != 256 and dep[s] == l:
                huffval.append(s)
                bits[l] += 1

    codes = {}
    code = 0
    k = 0
    for l in range(1, max_bits + 1):
        for _ in range(bits[l]):
            codes[huffval[k]] = (code, l)
            code += 1
            k += 1
        code <<= 1
    return bits, huffval, codes


def _fdct(src):
    """8x8 forward DCT (JPEG definition), separable: rows then columns."""
    cos = _COS
    tmp = [0.0] * 64
    for y in range(8):
        o = y * 8
        b0 = src[o]
        b1 = src[o + 1]
        b2 = src[o + 2]
        b3 = src[o + 3]
        b4 = src[o + 4]
        b5 = src[o + 5]
        b6 = src[o + 6]
        b7 = src[o + 7]
        for u in range(8):
            c = cos[u]
            tmp[o + u] = (c[0] * b0 + c[1] * b1 + c[2] * b2 + c[3] * b3 +
                          c[4] * b4 + c[5] * b5 + c[6] * b6 + c[7] * b7)
    dst = [0.0] * 64
    for v in range(8):
        c = cos[v]
        a0 = c[0]
        a1 = c[1]
        a2 = c[2]
        a3 = c[3]
        a4 = c[4]
        a5 = c[5]
        a6 = c[6]
        a7 = c[7]
        o = v * 8
        for x in range(8):
            dst[o + x] = (a0 * tmp[x] + a1 * tmp[8 + x] + a2 * tmp[16 + x] +
                          a3 * tmp[24 + x] + a4 * tmp[32 + x] +
                          a5 * tmp[40 + x] + a6 * tmp[48 + x] +
                          a7 * tmp[56 + x])
    return dst


def _cat(v):
    return v.bit_length()


def write_jpeg(path, w, h, rgb, quality=92):
    scale = 200 - 2 * quality
    ql = [max(1, min(255, (v * scale + 50) // 100)) for v in _Q_LUM]
    qc = [max(1, min(255, (v * scale + 50) // 100)) for v in _Q_CHR]

    n = w * h
    yp = [0.0] * n
    cbp = [0.0] * n
    crp = [0.0] * n
    for i in range(n):
        r = rgb[i * 4]
        g = rgb[i * 4 + 1]
        b = rgb[i * 4 + 2]
        yp[i] = 0.299 * r + 0.587 * g + 0.114 * b - 128.0
        cbp[i] = -0.168736 * r - 0.331264 * g + 0.5 * b
        crp[i] = 0.5 * r - 0.418688 * g - 0.081312 * b

    nbx = w // 8
    nby = h // 8
    nblocks = nbx * nby

    # quantize every block once, in MCU raster order per component
    all_blocks = []
    for comp, plane, qt in ((0, yp, ql), (1, cbp, qc), (2, crp, qc)):
        blocks = []
        for by in range(nby):
            for bx in range(nbx):
                blk = [0.0] * 64
                for yy in range(8):
                    o = (by * 8 + yy) * w + bx * 8
                    for xx in range(8):
                        blk[yy * 8 + xx] = plane[o + xx]
                co = _fdct(blk)
                q = [0] * 64
                for i in range(64):
                    val = co[i] / qt[i]
                    q[i] = int(val + 0.5) if val >= 0.0 else int(val - 0.5)
                blocks.append(q)
        all_blocks.append(blocks)

    # Huffman statistics over the exact symbol sequence that will be emitted
    DC_FREQ = [[0] * 256 for _ in range(2)]
    AC_FREQ = [[0] * 256 for _ in range(2)]
    pred = [0, 0, 0]
    for mi in range(nblocks):
        for comp in (0, 1, 2):
            hidx = 0 if comp == 0 else 1
            q = all_blocks[comp][mi]
            diff = q[0] - pred[comp]
            pred[comp] = q[0]
            DC_FREQ[hidx][_cat(abs(diff))] += 1
            run = 0
            for k in range(1, 64):
                z = q[_ZZ[k]]
                if z == 0:
                    run += 1
                    continue
                while run > 15:
                    AC_FREQ[hidx][0xF0] += 1
                    run -= 16
                AC_FREQ[hidx][(run << 4) | _cat(abs(z))] += 1
                run = 0
            if run:
                AC_FREQ[hidx][0x00] += 1
    for hidx in (0, 1):
        DC_FREQ[hidx][0] = max(1, DC_FREQ[hidx][0])
        AC_FREQ[hidx][0x00] = max(1, AC_FREQ[hidx][0x00])
        AC_FREQ[hidx][0xF0] = max(1, AC_FREQ[hidx][0xF0])

    dht = []
    tables = []
    for hidx in (0, 1):
        bits, val, codes = _huffman(DC_FREQ[hidx])
        dht.append((0, hidx, bits, val))
        tables.append(codes)
        bits, val, codes = _huffman(AC_FREQ[hidx])
        dht.append((1, hidx, bits, val))
        tables.append(codes)
    dc_codes = {0: tables[0], 1: tables[2]}
    ac_codes = {0: tables[1], 1: tables[3]}

    bw = _BitWriter()
    pred = [0, 0, 0]
    for mi in range(nblocks):
        for comp in (0, 1, 2):
            hidx = 0 if comp == 0 else 1
            q = all_blocks[comp][mi]
            dc = q[0]
            diff = dc - pred[comp]
            pred[comp] = dc
            s = _cat(abs(diff))
            code, nb = dc_codes[hidx][s]
            bw.write(code, nb)
            if s:
                bw.write(diff if diff > 0 else diff + (1 << s) - 1, s)
            run = 0
            ac = ac_codes[hidx]
            for k in range(1, 64):
                z = q[_ZZ[k]]
                if z == 0:
                    run += 1
                    continue
                while run > 15:
                    code, nb = ac[0xF0]
                    bw.write(code, nb)
                    run -= 16
                s = _cat(abs(z))
                code, nb = ac[(run << 4) | s]
                bw.write(code, nb)
                bw.write(z if z > 0 else z + (1 << s) - 1, s)
                run = 0
            if run:
                code, nb = ac[0x00]
                bw.write(code, nb)
    bw.flush()

    out = bytearray()
    out += b"\xFF\xD8"
    out += b"\xFF\xE0" + struct.pack(">H", 16) + b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
    # DQT
    dqt = bytearray()
    for tid, tbl in ((0, ql), (1, qc)):
        dqt.append(tid)
        for k in range(64):
            dqt.append(tbl[_ZZ[k]])
    out += b"\xFF\xDB" + struct.pack(">H", len(dqt) + 2) + bytes(dqt)
    # SOF0
    sof = bytearray()
    sof.append(8)
    sof += struct.pack(">HH", h, w)
    sof.append(3)
    for cid, samp, tq in ((1, 0x11, 0), (2, 0x11, 1), (3, 0x11, 1)):
        sof += bytes((cid, samp, tq))
    out += b"\xFF\xC0" + struct.pack(">H", len(sof) + 2) + bytes(sof)
    # DHT
    body = bytearray()
    for tc, th, bits, val in dht:
        body.append((tc << 4) | th)
        for l in range(1, 17):
            body.append(bits[l])
        body += bytes(val)
    out += b"\xFF\xC4" + struct.pack(">H", len(body) + 2) + bytes(body)
    # SOS
    sos = bytearray()
    sos.append(3)
    for cid, hidx in ((1, 0), (2, 1), (3, 1)):
        sos += bytes((cid, (hidx << 4) | hidx))
    sos += bytes((0, 63, 0))
    out += b"\xFF\xDA" + struct.pack(">H", len(sos) + 2) + bytes(sos)
    out += bytes(bw.out)
    out += b"\xFF\xD9"
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def main():
    os.makedirs(IMG_DIR, exist_ok=True)
    pixels = render()
    png_path = os.path.join(IMG_DIR, "icon.png")
    jpg_path = os.path.join(IMG_DIR, "icon.jpg")

    png_size = write_png(png_path, OUT, OUT, pixels)
    jpg_size = write_jpeg(jpg_path, OUT, OUT, pixels, quality=92)

    print("icon: %dx%d (4x4 supersampled from %dx%d)" % (OUT, OUT, S, S))
    print("  %s  %6.1f KiB" % (png_path, png_size / 1024.0))
    print("  %s  %6.1f KiB" % (jpg_path, jpg_size / 1024.0))


if __name__ == "__main__":
    main()
