#!/usr/bin/env python3
"""Button prompt art for the MUA3 native port: keyboard keycaps, mouse, Xbox and PlayStation pad icons.

Everything here is original vector art drawn by this script: shapes are polygons / arcs and the lettering is this
file's own stroke-built alphabet (GLYPHS). No font file, no vendor artwork and no game data is read to make the icons.
The style follows the game's prompt family: a flat grey body with a thin black outline and a soft drop shadow, the
symbol in heavy yellow with a dark edge (orange while a button is to be held).

  python scripts/prompt_art/gen.py                 writes src/core/hle/prompt_art_data.cpp
  python scripts/prompt_art/gen.py --sheet out.png [--game-pack <80_button_icon.ext.kslt>]
                                                   a contact sheet (the pack, if given, is only shown for comparison)
Needs Pillow and numpy.
"""
import argparse
import math
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ICON = 64
SS = 8            # supersampling
PAD = 8           # icon pixels of margin while drawing (cropped at the end)
N = (ICON + 2 * PAD) * SS

# ---- palette (measured against the look of the game's prompts; the art itself is ours)
OUTLINE = (0, 0, 0)
FILL = (107, 113, 123)
RIM = (117, 123, 128)
SIDE = (62, 66, 74)       # the dark side of a raised part (keycap lip, stick)
INK = (240, 228, 0)
INK_HI = (255, 250, 60)
INK_EDGE = (8, 12, 24)
HOLD = (255, 96, 16)
HOLD_HI = (255, 150, 60)
XB_GREEN, XB_RED, XB_BLUE, XB_YELLOW = (112, 204, 64), (240, 68, 56), (64, 144, 255), (250, 208, 16)
PS_BLUE, PS_RED, PS_PINK, PS_GREEN = (104, 160, 255), (244, 84, 72), (236, 124, 204), (64, 208, 152)


def lighter(c, k=0.35):
    return tuple(int(v + (255 - v) * k) for v in c)


# ---- masks: float32 N x N coverage at the supersampled size, shapes given in icon pixels

def _px(v):
    return (v + PAD) * SS


class Pen:
    """Draws filled shapes (and holes) into one mask."""

    def __init__(self):
        self.im = Image.new('L', (N, N), 0)
        self.d = ImageDraw.Draw(self.im)

    def poly(self, pts, on=True):
        self.d.polygon([(_px(x), _px(y)) for x, y in pts], fill=255 if on else 0)
        return self

    def circle(self, cx, cy, r, on=True):
        self.d.ellipse([_px(cx - r), _px(cy - r), _px(cx + r), _px(cy + r)], fill=255 if on else 0)
        return self

    def rect(self, x0, y0, x1, y1, on=True):
        return self.poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], on)

    def stroke(self, pts, w, on=True, round_ends=False):
        """A line of width w through pts (flat ends unless round_ends; joins are round)."""
        f = 255 if on else 0
        for i in range(len(pts) - 1):
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            dx, dy = x1 - x0, y1 - y0
            ln = math.hypot(dx, dy)
            if ln < 1e-9:
                continue
            nx, ny = -dy / ln * w / 2, dx / ln * w / 2
            self.d.polygon([(_px(x0 + nx), _px(y0 + ny)), (_px(x1 + nx), _px(y1 + ny)), (_px(x1 - nx), _px(y1 - ny)),
                            (_px(x0 - nx), _px(y0 - ny))], fill=f)
        inner = pts if round_ends else pts[1:-1]
        for x, y in inner:
            self.d.ellipse([_px(x - w / 2), _px(y - w / 2), _px(x + w / 2), _px(y + w / 2)], fill=f)
        return self

    def mask(self):
        return np.asarray(self.im, dtype=np.float32) / 255.0


def rrect_pts(x0, y0, x1, y1, tl=0, tr=0, br=0, bl=0, n=20):
    """A rectangle with elliptical corners; each radius is r or (rx, ry)."""
    def rr(r):
        return (r, r) if not isinstance(r, tuple) else r
    out = []
    for (cx, cy, (rx, ry), a0) in ((x1, y0, rr(tr), -90), (x1, y1, rr(br), 0), (x0, y1, rr(bl), 90), (x0, y0, rr(tl), 180)):
        sx = -1 if cx == x1 else 1
        sy = -1 if cy == y1 else 1
        ox, oy = cx + sx * rx, cy + sy * ry
        if rx <= 0 or ry <= 0:
            out.append((cx, cy))
            continue
        for i in range(n + 1):
            a = math.radians(a0 + 90.0 * i / n)
            out.append((ox + rx * math.cos(a), oy + ry * math.sin(a)))
    return out


def arc_pts(cx, cy, rx, ry, a0, a1, n=48):
    """Points on an ellipse from angle a0 to a1 (degrees, y down: 0 right, 90 down, 270 up)."""
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n)))
            for i in range(n + 1)]


def rrect(x0, y0, x1, y1, r=0, **kw):
    if not kw:
        kw = dict(tl=r, tr=r, br=r, bl=r)
    return Pen().poly(rrect_pts(x0, y0, x1, y1, **kw)).mask()


def circle(cx, cy, r):
    return Pen().circle(cx, cy, r).mask()


_kernels = {}


def dilate(m, r):
    """Grown by r icon pixels (a disc)."""
    if r <= 0:
        return m
    R = r * SS
    key = round(R, 3)
    if key not in _kernels:
        k = np.zeros((N, N), np.float32)
        ir = int(math.ceil(R))
        yy, xx = np.mgrid[-ir:ir + 1, -ir:ir + 1]
        d = (xx * xx + yy * yy <= R * R).astype(np.float32)
        for dy in range(-ir, ir + 1):
            for dx in range(-ir, ir + 1):
                if d[dy + ir, dx + ir]:
                    k[dy % N, dx % N] = 1
        _kernels[key] = np.fft.rfft2(k)
    conv = np.fft.irfft2(np.fft.rfft2(m) * _kernels[key], s=(N, N))
    return (conv > 0.5).astype(np.float32)


def erode(m, r):
    return 1.0 - dilate(1.0 - m, r)


def shift(m, dx, dy):
    return np.roll(np.roll(m, int(round(dy * SS)), axis=0), int(round(dx * SS)), axis=1)


def blur(m, r):
    im = Image.fromarray((np.clip(m, 0, 1) * 255).astype(np.uint8), 'L').filter(ImageFilter.GaussianBlur(r * SS))
    return np.asarray(im, dtype=np.float32) / 255.0


def union(*ms):
    out = ms[0]
    for m in ms[1:]:
        out = np.maximum(out, m)
    return out


def smooth(m, r):
    """Corners rounded by r (opening then closing)."""
    return erode(dilate(dilate(erode(m, r), r), r), r)


# ---- the alphabet: heavy geometric capitals built from bars, diagonals, bowls and arcs.
# Units: capital height 1 (y 0 at the top, 1 on the baseline). Each glyph: (advance width, [ops]);
# an op is ('P', points) to fill, ('H', points) to cut, ('S', points, weight) for a stroked line.

T = 0.25   # stem
B = 0.20   # bar


def _rect(x0, y0, x1, y1):
    return ('P', [(x0, y0), (x1, y0), (x1, y1), (x0, y1)])


def _cut(op):
    return ('H', op[1])


def _diag(x0, y0, x1, y1, hw=0.27):
    return ('P', [(x0 - hw / 2, y0), (x0 + hw / 2, y0), (x1 + hw / 2, y1), (x1 - hw / 2, y1)])


def _rr(x0, y0, x1, y1, **kw):
    return ('P', rrect_pts(x0, y0, x1, y1, **kw))


def _ell(cx, cy, rx, ry):
    return ('P', arc_pts(cx, cy, rx, ry, 0, 360, 72))


def _stroke(pts, w):
    return ('S', pts, w)


def _rot180(ops, cx, cy):
    out = []
    for op in ops:
        pts = [(2 * cx - x, 2 * cy - y) for x, y in op[1]]
        out.append((op[0], pts) + tuple(op[2:]))
    return out


_SIX = [_ell(.42, .66, .42, .34), _stroke(arc_pts(.50, .52, .38, .40, 180, 305), .24), _cut(_ell(.42, .665, .17, .135))]

GLYPHS = {
    'A': (.96, [_diag(.135, 1, .48, 0), _diag(.825, 1, .48, 0), _rect(.25, .60, .71, .80)]),
    'B': (.86, [_rr(0, 0, .80, .54, tr=.27, br=.27), _rr(0, .46, .86, 1, tr=.27, br=.27),
                _cut(_rr(.25, .20, .55, .40, tr=.10, br=.10)), _cut(_rr(.25, .60, .61, .80, tr=.10, br=.10))]),
    'C': (.90, [_ell(.47, .5, .47, .52), _cut(_ell(.47, .5, .21, .29)), _cut(_rect(.56, .36, 1.1, .64))]),
    'D': (.90, [_rr(0, 0, .90, 1, tr=(.45, .5), br=(.45, .5)), _cut(_rr(.25, .20, .65, .80, tr=(.20, .30), br=(.20, .30)))]),
    'E': (.74, [_rect(0, 0, T, 1), _rect(0, 0, .74, B), _rect(0, .40, .68, .60), _rect(0, .80, .74, 1)]),
    'F': (.72, [_rect(0, 0, T, 1), _rect(0, 0, .72, B), _rect(0, .42, .66, .62)]),
    'G': (.94, [_ell(.47, .5, .47, .52), _cut(_ell(.47, .5, .21, .29)), _cut(_rect(.56, .34, 1.1, .50)),
                _rect(.50, .46, .93, .66)]),
    'H': (.90, [_rect(0, 0, T, 1), _rect(.65, 0, .90, 1), _rect(0, .40, .90, .60)]),
    'I': (.26, [_rect(0, 0, .26, 1)]),
    'J': (.70, [_rr(0, 0, .70, 1, br=(.35, .36), bl=(.35, .36)), _cut(_rr(.25, -.1, .45, .80, br=(.10, .12), bl=(.10, .12))),
                _cut(_rect(-.1, -.1, .25, .62))]),
    'K': (.92, [_rect(0, 0, T, 1), _diag(.78, 0, .30, .60, .28), _diag(.44, .42, .78, 1, .28)]),
    'L': (.70, [_rect(0, 0, T, 1), _rect(0, .80, .70, 1)]),
    'M': (1.10, [_rect(0, 0, .25, 1), _rect(.85, 0, 1.10, 1), _diag(.125, 0, .55, .88, .25), _diag(.975, 0, .55, .88, .25)]),
    'N': (.92, [_rect(0, 0, T, 1), _rect(.67, 0, .92, 1), _diag(.125, 0, .795, 1, .25)]),
    'O': (.98, [_ell(.49, .5, .49, .52), _cut(_ell(.49, .5, .235, .30))]),
    'P': (.84, [_rect(0, 0, T, 1), _rr(0, 0, .84, .62, tr=.31, br=.31), _cut(_rr(.25, .20, .59, .42, tr=.11, br=.11))]),
    'Q': (.98, [_ell(.49, .5, .49, .52), _cut(_ell(.49, .5, .235, .30)), _diag(.56, .64, .86, 1.06, .24)]),
    'R': (.88, [_rect(0, 0, T, 1), _rr(0, 0, .82, .62, tr=.31, br=.31), _cut(_rr(.25, .20, .57, .42, tr=.11, br=.11)),
                _diag(.46, .60, .74, 1, .27)]),
    'S': (.84, [_stroke(arc_pts(.42, .30, .30, .185, 325, 90) + arc_pts(.42, .70, .30, .185, 270, 505), .235)]),
    'T': (.80, [_rect(0, 0, .80, .21), _rect(.275, 0, .525, 1)]),
    'U': (.90, [_rr(0, 0, .90, 1, br=(.45, .42), bl=(.45, .42)), _cut(_rr(.25, -.1, .65, .80, br=(.20, .22), bl=(.20, .22)))]),
    'V': (.94, [_diag(.135, 0, .47, 1), _diag(.805, 0, .47, 1)]),
    'W': (1.30, [_diag(.13, 0, .37, 1, .25), _diag(.65, 0, .37, 1, .25), _diag(.65, 0, .93, 1, .25), _diag(1.17, 0, .93, 1, .25)]),
    'X': (.92, [_diag(.14, 0, .78, 1, .28), _diag(.78, 0, .14, 1, .28)]),
    'Y': (.92, [_diag(.135, 0, .46, .56), _diag(.785, 0, .46, .56), _rect(.335, .50, .585, 1)]),
    'Z': (.84, [_rect(0, 0, .84, .21), _rect(0, .79, .84, 1), _diag(.70, 0, .14, 1, .28)]),
    '0': (.84, [_ell(.42, .5, .42, .52), _cut(_ell(.42, .5, .17, .30))]),
    '1': (.60, [('P', [(.30, 0), (.56, 0), (.56, 1), (.30, 1), (.30, .30), (.04, .42), (.04, .19)])]),
    '2': (.82, [_stroke(arc_pts(.40, .315, .28, .20, 185, 398) + [(.13, .90)], .235), _rect(0, .79, .82, 1)]),
    '3': (.82, [_stroke(arc_pts(.38, .30, .28, .185, 205, 450), .235), _stroke(arc_pts(.38, .70, .31, .185, 270, 518), .235)]),
    '4': (.90, [_diag(.62, 0, .13, .66, .26), _rect(0, .56, .90, .77), _rect(.50, 0, .76, 1)]),
    '5': (.82, [_rect(.08, 0, .76, .21), _rect(.08, 0, .32, .50),
                _stroke(arc_pts(.38, .67, .31, .215, 222, 270) + arc_pts(.38, .67, .31, .215, 270, 518), .235)]),
    '6': (.84, _SIX),
    '7': (.80, [_rect(0, 0, .80, .22), _diag(.655, 0, .30, 1, .27)]),
    '8': (.84, [_ell(.42, .28, .36, .29), _ell(.42, .69, .42, .32), _cut(_ell(.42, .285, .12, .095)), _cut(_ell(.42, .69, .17, .125))]),
    '9': (.84, _rot180(_SIX, .42, .5)),
    '-': (.50, [_rect(0, .40, .50, .61)]),
    '=': (.62, [_rect(0, .24, .62, .44), _rect(0, .58, .62, .78)]),
    '+': (.70, [_rect(0, .39, .70, .61), _rect(.24, .15, .46, .85)]),
    '*': (.70, [_rect(.25, .10, .45, .80), ('P', [(.02, .34), (.12, .17), (.68, .56), (.58, .73)]),
                ('P', [(.68, .34), (.58, .17), (.02, .56), (.12, .73)])]),
    '/': (.60, [_diag(.48, -.04, .12, 1.04, .24)]),
    '\\': (.60, [_diag(.12, -.04, .48, 1.04, .24)]),
    '[': (.46, [_rect(0, -.04, .24, 1.04), _rect(0, -.04, .46, .15), _rect(0, .85, .46, 1.04)]),
    ']': (.46, [_rect(.22, -.04, .46, 1.04), _rect(0, -.04, .46, .15), _rect(0, .85, .46, 1.04)]),
    '.': (.28, [_rect(0, .72, .28, 1)]),
    ',': (.30, [('P', [(.02, .68), (.30, .68), (.30, .96), (.14, 1.24), (0, 1.24), (.08, .96), (.02, .96)])]),
    ';': (.30, [_rect(.02, .22, .30, .50), ('P', [(.02, .68), (.30, .68), (.30, .96), (.14, 1.24), (0, 1.24), (.08, .96), (.02, .96)])]),
    "'": (.26, [('P', [(0, 0), (.26, 0), (.20, .42), (.06, .42)])]),
    '`': (.40, [('P', [(0, 0), (.24, 0), (.40, .34), (.22, .34)])]),
    ':': (.28, [_rect(0, .22, .28, .50), _rect(0, .72, .28, 1)]),
    '(': (.42, [_stroke(arc_pts(.62, .5, .50, .62, 118, 242), .22)]),
    ')': (.42, [_stroke(arc_pts(-.20, .5, .50, .62, -62, 62), .22)]),
    ' ': (.34, []),
}


def text_width(s, cap, track=0.09):
    return sum(GLYPHS.get(ch, GLYPHS[' '])[0] for ch in s) * cap + track * cap * (len(s) - 1)


def text(s, cap, cx, cy, max_w=None, track=0.09):
    """The mask of s in capitals `cap` icon pixels high, centred on (cx, cy)."""
    w = text_width(s, cap, track)
    if max_w and w > max_w:
        cap *= max_w / w
        w = max_w
    pen = Pen()
    x = cx - w / 2
    y = cy - cap / 2
    for ch in s:
        adv, ops = GLYPHS[ch]
        for op in ops:
            pts = [(x + px * cap, y + py * cap) for px, py in op[1]]
            if op[0] == 'P':
                pen.poly(pts)
            elif op[0] == 'H':
                pen.poly(pts, False)
            else:
                pen.stroke(pts, op[2] * cap)
        x += (adv + track) * cap
    return pen.mask()


# ---- compositing

class Canvas:
    def __init__(self):
        self.rgb = np.zeros((N, N, 3), np.float32)  # premultiplied
        self.a = np.zeros((N, N), np.float32)

    def over(self, m, col, alpha=1.0):
        a = m * alpha
        c = np.asarray(col, np.float32)
        self.rgb = c * a[..., None] + self.rgb * (1 - a[..., None])
        self.a = a + self.a * (1 - a)
        return self

    def image(self):
        """The 64 x 64 RGBA8 icon (straight alpha)."""
        lo, hi = PAD * SS, (PAD + ICON) * SS
        rgb = self.rgb[lo:hi, lo:hi].reshape(ICON, SS, ICON, SS, 3).mean(axis=(1, 3))
        a = self.a[lo:hi, lo:hi].reshape(ICON, SS, ICON, SS).mean(axis=(1, 3))
        out = np.zeros((ICON, ICON, 4), np.float32)
        nz = a > 1e-6
        out[..., :3][nz] = rgb[nz] / a[nz][:, None]
        out[..., 3] = a * 255.0
        out8 = np.clip(np.rint(out), 0, 255).astype(np.uint8)
        out8[out8[..., 3] == 0] = 0
        return out8


def body(c, shape, fill=FILL, rim=RIM, outline=1.5, rim_w=1.6, shadow=True):
    """A grey part: soft shadow below right, black outline around, a slightly lighter rim inside it."""
    outer = dilate(shape, outline)
    if shadow:
        c.over(blur(shift(outer, 2.6, 3.6), 1.5), (0, 0, 0), 0.52)
    c.over(outer, OUTLINE)
    c.over(shape, rim)
    if rim_w > 0 and fill != rim:
        c.over(erode(shape, rim_w), fill)


def ink(c, sym, col=INK, hi=None, edge=1.8, clip=None):
    """A symbol in heavy colour: dark edge around it (a little heavier below right), lighter along its top."""
    if hi is None:
        hi = INK_HI if col == INK else HOLD_HI if col == HOLD else lighter(col)
    e = dilate(sym, edge)
    e = union(e, shift(e, 0.7, 0.9))
    if clip is not None:
        e = e * clip
        sym = sym * clip
    c.over(e, INK_EDGE)
    c.over(sym, hi)
    c.over(sym * shift(sym, 0, 1.3), col)


def lone(c, sym):
    """A symbol alone, as a grey part (the game's "-" and "+" without a button)."""
    body(c, sym, rim_w=0, fill=FILL, rim=FILL, outline=1.6)


# ---- symbols (masks)

def tri(d, cx, cy, s):
    """A solid triangle pointing d ('u','d','l','r'), about 2s across."""
    pts = [(0, -s * 0.9), (s, s * 0.8), (-s, s * 0.8)]
    rot = {'u': lambda x, y: (x, y), 'd': lambda x, y: (-x, -y), 'l': lambda x, y: (y, -x), 'r': lambda x, y: (-y, x)}[d]
    return Pen().poly([(cx + rot(x, y)[0], cy + rot(x, y)[1]) for x, y in pts]).mask()


def arrow(d, cx, cy, ln, head=0.62, w=0.30, hw=0.95):
    """A solid arrow pointing d, ln long: a shaft and a wide head."""
    h = ln / 2
    pts = [(0, -h), (hw * h, -h + head * ln), (w * h, -h + head * ln), (w * h, h), (-w * h, h), (-w * h, -h + head * ln),
           (-hw * h, -h + head * ln)]
    rot = {'u': lambda x, y: (x, y), 'd': lambda x, y: (-x, -y), 'l': lambda x, y: (y, -x), 'r': lambda x, y: (-y, x)}[d]
    return Pen().poly([(cx + rot(x, y)[0], cy + rot(x, y)[1]) for x, y in pts]).mask()


def sym_enter(cx, cy, s=1.0):
    # down the right, then left to an arrow head
    p = Pen()
    p.rect(cx + 5.5 * s, cy - 9 * s, cx + 11 * s, cy + 6.5 * s)
    p.rect(cx - 3 * s, cy + 1 * s, cx + 11 * s, cy + 6.5 * s)
    p.poly([(cx - 12 * s, cy + 3.75 * s), (cx - 2 * s, cy - 4.5 * s), (cx - 2 * s, cy + 12 * s)])
    return p.mask()


def sym_backspace(cx, cy, s=1.0):
    p = Pen()
    p.poly([(cx - 14 * s, cy), (cx - 5 * s, cy - 8.5 * s), (cx + 13 * s, cy - 8.5 * s), (cx + 13 * s, cy + 8.5 * s), (cx - 5 * s, cy + 8.5 * s)])
    p.stroke([(cx - 0.5 * s, cy - 4 * s), (cx + 7.5 * s, cy + 4 * s)], 3.0 * s, on=False)
    p.stroke([(cx - 0.5 * s, cy + 4 * s), (cx + 7.5 * s, cy - 4 * s)], 3.0 * s, on=False)
    return p.mask()


def sym_tab(cx, cy, s=1.0):
    p = Pen()
    p.rect(cx - 13 * s, cy - 2.6 * s, cx + 1 * s, cy + 2.6 * s)
    p.poly([(cx + 8 * s, cy), (cx - 1 * s, cy - 8 * s), (cx - 1 * s, cy + 8 * s)])
    p.rect(cx + 8.5 * s, cy - 8.5 * s, cx + 13 * s, cy + 8.5 * s)
    return p.mask()


def sym_shift(cx, cy, s=1.0):
    return Pen().poly([(cx, cy - 11 * s), (cx + 12 * s, cy + 1 * s), (cx + 5.5 * s, cy + 1 * s), (cx + 5.5 * s, cy + 10 * s),
                       (cx - 5.5 * s, cy + 10 * s), (cx - 5.5 * s, cy + 1 * s), (cx - 12 * s, cy + 1 * s)]).mask()


def sym_caps(cx, cy, s=1.0):
    p = Pen()
    p.poly([(cx, cy - 12 * s), (cx + 11.5 * s, cy - 1 * s), (cx + 5.5 * s, cy - 1 * s), (cx + 5.5 * s, cy + 4 * s),
            (cx - 5.5 * s, cy + 4 * s), (cx - 5.5 * s, cy - 1 * s), (cx - 11.5 * s, cy - 1 * s)])
    p.rect(cx - 5.5 * s, cy + 7 * s, cx + 5.5 * s, cy + 11.5 * s)
    return p.mask()


def sym_space(cx, cy, s=1.0):
    p = Pen()
    p.rect(cx - 16 * s, cy - 4.5 * s, cx - 11.5 * s, cy + 4.5 * s)
    p.rect(cx + 11.5 * s, cy - 4.5 * s, cx + 16 * s, cy + 4.5 * s)
    p.rect(cx - 16 * s, cy + 0.5 * s, cx + 16 * s, cy + 4.5 * s)
    return p.mask()


def sym_menu(cx, cy, s=1.0):
    p = Pen()
    for i in (-1, 0, 1):
        p.stroke([(cx - 8 * s, cy + i * 6.6 * s), (cx + 8 * s, cy + i * 6.6 * s)], 4.2 * s, round_ends=True)
    return p.mask()


def sym_view(cx, cy, s=1.0):
    back = Pen().poly(rrect_pts(cx - 11 * s, cy - 9.5 * s, cx + 4 * s, cy + 3 * s, tl=2.5 * s, tr=2.5 * s, br=2.5 * s, bl=2.5 * s))
    back.poly(rrect_pts(cx - 7.6 * s, cy - 6.1 * s, cx + 0.6 * s, cy - 0.4 * s, tl=s, tr=s, br=s, bl=s), False)
    m = back.mask()
    # the front window covers the back one (with a gap around it)
    gap = rrect(cx - 6.4 * s, cy - 5.4 * s, cx + 13.4 * s, cy + 11.9 * s, 3.5 * s)
    front = Pen().poly(rrect_pts(cx - 4 * s, cy - 3 * s, cx + 11 * s, cy + 9.5 * s, tl=2.5 * s, tr=2.5 * s, br=2.5 * s, bl=2.5 * s))
    front.poly(rrect_pts(cx - 0.6 * s, cy + 0.4 * s, cx + 7.6 * s, cy + 6.1 * s, tl=s, tr=s, br=s, bl=s), False)
    return union(m * (1 - gap), front.mask())


def sym_create(cx, cy, s=1.0):
    # a small upright pill with three rays fanning from its top
    p = Pen()
    p.poly(rrect_pts(cx - 4.2 * s, cy - 2 * s, cx + 4.2 * s, cy + 12 * s, tl=4.2 * s, tr=4.2 * s, br=4.2 * s, bl=4.2 * s))
    for a in (-52, 0, 52):
        dx, dy = math.sin(math.radians(a)), -math.cos(math.radians(a))
        p.stroke([(cx + dx * 7.5 * s, cy + 3 * s + dy * 9.5 * s), (cx + dx * 13 * s, cy + 3 * s + dy * 15 * s)], 3.4 * s, round_ends=True)
    return p.mask()


def ps_shape(kind, cx, cy, r, w):
    p = Pen()
    if kind == 'cross':
        k = r * 0.86
        p.stroke([(cx - k, cy - k), (cx + k, cy + k)], w)
        p.stroke([(cx - k, cy + k), (cx + k, cy - k)], w)
    elif kind == 'circle':
        p.circle(cx, cy, r)
        p.circle(cx, cy, r - w, False)
    elif kind == 'square':
        k = r * 0.88
        p.rect(cx - k, cy - k, cx + k, cy + k)
        p.rect(cx - k + w, cy - k + w, cx + k - w, cy + k - w, False)
    else:  # triangle
        def t(rad, dy):
            return [(cx + rad * math.cos(math.radians(a)), cy + dy + rad * math.sin(math.radians(a))) for a in (270, 30, 150)]
        p.poly(t(r * 1.22, r * 0.20))
        p.poly(t(r * 1.22 - 2 * w, r * 0.20), False)
    return p.mask()


# ---- icons. Each returns a Canvas; `col` is the ink (INK or HOLD).

CX, CY, DISC_R = 32.0, 32.0, 22.4   # the game's round buttons: 48 px across with the outline, centred


def disc_button(sym_fn, col=INK):
    c = Canvas()
    body(c, circle(CX, CY, DISC_R))
    ink(c, sym_fn(), col)
    return c


def face_letter(ch, col):
    return disc_button(lambda: text(ch, 21, CX, CY), col)


def face_shape(kind, col):
    return disc_button(lambda: ps_shape(kind, CX, CY, 10.2, 4.4), col)


def shoulder(label, kind, col=INK):
    """kind: 'lb' / 'rb' (a bumper, the outer top corner swept), 'lt' / 'rt' (a trigger, tall with a swept top),
    'l1' / 'r1' (a flat bar), 'l2' / 'r2' (a trigger that hangs down)."""
    c = Canvas()
    left = kind[0] == 'l'
    k = kind[1:]
    cy, cap = 32.0, 17.0
    if k == 'b':
        big, small = (15, 13), 5
        shape = rrect(7.5, 14, 56.5, 50, tl=big if left else small, tr=small if left else big, br=small, bl=small)
        cy = 32.5
    elif k == 't':
        big, small = (17, 21), (8, 9)
        shape = rrect(10.5, 8.5, 53.5, 55.5, tl=big if left else small, tr=small if left else big, br=6, bl=6)
        cy, cap = 34.0, 16.0
    elif k == '1':
        shape = rrect(7.5, 16, 56.5, 48, 6.5)
    else:
        shape = rrect(10.5, 9, 53.5, 56, tl=6.5, tr=6.5, br=(15, 19), bl=(15, 19))
        cy, cap = 30.0, 16.0
    body(c, shape)
    ink(c, text(label, cap, 32, cy), col)
    return c


def lone_symbol(sym):
    c = Canvas()
    lone(c, sym)
    return c


def stick_parts(c, cx, cy, r, h=5.0):
    whole = union(circle(cx, cy, r), circle(cx, cy + h, r), rrect(cx - r, cy, cx + r, cy + h))
    body(c, whole, fill=SIDE, rim=SIDE, rim_w=0)
    top = circle(cx, cy, r)
    c.over(dilate(top, 0.6), (30, 32, 38))
    c.over(top, RIM)
    c.over(erode(top, 1.5), FILL)


def ring(cx, cy, r, w):
    return Pen().circle(cx, cy, r).circle(cx, cy, r - w, False).mask()


def stick(label='', push=False, roll=None, col=INK):
    c = Canvas()
    if roll:
        cx, cy, r = 32.0, 29.5, 13.0
        for side in (-1, 1):
            # an arc beside the stick with an arrow head at the end the stick turns towards
            a0, a1 = (128, 232) if side < 0 else (-52, 52)
            arc = Pen().stroke(arc_pts(cx, cy + 2, 23.5, 23.5, a0, a1), 4.6)
            cw = roll == 'r'
            end = (a1 if cw else a0)
            ex, ey = cx + 23.5 * math.cos(math.radians(end)), cy + 2 + 23.5 * math.sin(math.radians(end))
            tx, ty = -math.sin(math.radians(end)), math.cos(math.radians(end))
            if not cw:
                tx, ty = -tx, -ty
            nx, ny = -ty, tx
            arc.poly([(ex + tx * 7.5, ey + ty * 7.5), (ex + nx * 6.2 - tx * 1.5, ey + ny * 6.2 - ty * 1.5), (ex - nx * 6.2 - tx * 1.5, ey - ny * 6.2 - ty * 1.5)])
            ink(c, arc.mask(), col, edge=1.5)
        stick_parts(c, cx, cy, r, 4.0)
        ink(c, ring(cx, cy, 9.6, 2.0), col, edge=1.2)
        if label:
            ink(c, text(label, 8.0, cx, cy), col, edge=1.2)
        return c
    if push:
        cx, cy, r = 32.0, 37.0, 17.5
        stick_parts(c, cx, cy, r, 5.0)
        ink(c, ring(cx, cy, 13.0, 2.3), col, edge=1.4)
        if label:
            ink(c, text(label, 11.5, cx, cy + 0.5), col, edge=1.4)
        ink(c, arrow('d', 32, 11.5, 19, head=0.56, w=0.42, hw=1.05), col, edge=1.6)
        return c
    cx, cy, r = 32.0, 29.5, 21.5
    stick_parts(c, cx, cy, r, 5.5)
    ink(c, ring(cx, cy, 16.2, 2.6), col, edge=1.5)
    if label:
        ink(c, text(label, 14.5, cx, cy), col, edge=1.6)
    return c


def dpad(style, lit='', col=INK):
    """style 'xbox': a rounded cross on a dark disc; 'ps': four separate keys. lit: any of 'udlr'."""
    c = Canvas()
    pos = {'u': (32, 14.0), 'd': (32, 50.0), 'l': (14.0, 32), 'r': (50.0, 32)}
    if style == 'xbox':
        base = circle(32, 32, 26.5)
        body(c, base, fill=SIDE, rim=(74, 79, 88), rim_w=1.6)
        cross = smooth(union(rrect(23, 7, 41, 57, 2), rrect(7, 23, 57, 41, 2)), 2.2)
        body(c, cross, shadow=False, outline=1.4)
        s = 5.2
    else:
        s = 5.0
        for d, (x, y) in pos.items():
            pts = [(-9.5, -18.5), (9.5, -18.5), (9.5, -3.5), (0, 6.5), (-9.5, -3.5)]  # pointing down, for 'u'
            rot = {'u': lambda px, py: (px, py), 'd': lambda px, py: (-px, -py), 'l': lambda px, py: (py, -px), 'r': lambda px, py: (-py, px)}[d]
            petal = smooth(Pen().poly([(32 + rot(px, py)[0], 32 + rot(px, py)[1]) for px, py in [(a, b - 8.0) for a, b in pts]]).mask(), 2.4)
            body(c, petal)
        pos = {'u': (32, 13.5), 'd': (32, 50.5), 'l': (13.5, 32), 'r': (50.5, 32)}
    for d in lit:
        x, y = pos[d]
        ink(c, tri(d, x, y, s), col, edge=1.4)
    return c


def diamond(marks):
    """Four small buttons; marks: [(x, y, mask function(cx, cy), colour)] top, left, right, bottom."""
    c = Canvas()
    spots = [(32, 11.5), (11.5, 32), (52.5, 32), (32, 52.5)]
    shapes = union(*[circle(x, y, 9.6) for x, y in spots])
    body(c, shapes, outline=1.4, rim_w=1.2)
    for (x, y), (fn, col) in zip(spots, marks):
        ink(c, fn(x, y), col, edge=0.9)
    return c


# keyboard

def keycap_body(c, x0, y0, x1, y1, r=6.5, lip=4.5, outline=1.5):
    whole = rrect(x0, y0, x1, y1, r)
    top = rrect(x0, y0, x1, y1 - lip, r)
    body(c, whole, fill=SIDE, rim=SIDE, rim_w=0, outline=outline)
    c.over(dilate(top, 0.5) * whole, (34, 36, 42))
    c.over(top, RIM)
    c.over(erode(top, 1.5), FILL)


SQ = (10.5, 9.5, 53.5, 54.0)      # a one-unit key
WIDE = (4.5, 13.0, 59.5, 51.5)    # a key with a word on it
BAR = (3.5, 18.0, 60.5, 47.5)     # the space bar


def keycap(legend, col=INK, tag=None):
    """legend: ('char', 'A') | ('word', 'ESC') | ('two', 'PG', 'UP') | ('sym', name) | ('arrow', 'u').
    tag: a small letter in the top left corner ('R' for the right-hand modifier, 'N' for the number pad)."""
    c = Canvas()
    kind = legend[0]
    box = SQ
    if kind == 'word' and len(legend[1]) >= 3:
        box = WIDE
    if kind == 'sym' and legend[1] in ('enter', 'backspace', 'tab', 'shift', 'caps'):
        box = WIDE if legend[1] != 'caps' else SQ
    if kind == 'sym' and legend[1] == 'space':
        box = BAR
    keycap_body(c, *box)
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1 - 4.5) / 2
    dx = dy = 0.0
    scale = 1.0
    if tag:
        ink(c, text(tag, 8.0, x0 + 8.2, y0 + 8.4), col, edge=1.1)
        if box == SQ:
            dx, dy, scale = 2.5, 2.5, 0.84
        else:
            dx, scale = 4.0, 0.9
    if kind == 'char':
        ch = legend[1]
        cap = 21.0 * scale
        if ch in "-=+*/\\[];',.`":
            cap = (30.0 if ch in ",.'`" else 24.0 if ch == ';' else 22.0) * scale
            yoff = {',': -8.0, '.': -10.5, ';': -2.5, "'": 8.0, '`': 9.0}.get(ch, 0.0) * scale
            sym = text(ch, cap, cx + dx, cy + dy + yoff)
        else:
            sym = text(ch, cap, cx + dx, cy + dy)
    elif kind == 'word':
        w = legend[1]
        cap = {1: 21.0, 2: 17.0, 3: 14.0}.get(len(w), 13.0) * scale
        sym = text(w, cap, cx + dx, cy + dy, max_w=(x1 - x0) - 11 - 2 * dx)
    elif kind == 'two':
        sym = union(text(legend[1], 11.5, cx, cy - 7.2, max_w=32), text(legend[2], 11.5, cx, cy + 7.4, max_w=32))
    elif kind == 'arrow':
        sym = arrow(legend[1], cx, cy, 25.0)
    else:
        fn = {'enter': sym_enter, 'backspace': sym_backspace, 'tab': sym_tab, 'shift': sym_shift, 'caps': sym_caps,
              'space': sym_space}[legend[1]]
        s = scale * (0.86 if legend[1] in ('shift',) else 0.9 if legend[1] in ('caps', 'enter') else 1.0)
        sym = fn(cx + dx, cy + dy + (0.5 if legend[1] == 'space' else 0), s)
    ink(c, sym, col)
    return c


# One small key of a cluster, drawn at `at` (its top left) on a shared canvas
MINI_W, MINI_H = 19.0, 20.5


def mini_key(c, x, y, legend, col=INK):
    whole = rrect(x, y, x + MINI_W, y + MINI_H, 4.0)
    top = rrect(x, y, x + MINI_W, y + MINI_H - 2.8, 4.0)
    outer = dilate(whole, 1.3)
    c.over(blur(shift(outer, 1.8, 2.4), 1.1), (0, 0, 0), 0.5)
    c.over(outer, OUTLINE)
    c.over(whole, SIDE)
    c.over(top, RIM)
    c.over(erode(top, 1.2), FILL)
    cx, cy = x + MINI_W / 2, y + (MINI_H - 2.8) / 2
    if legend[0] == 'arrow':
        sym = tri(legend[1], cx, cy, 4.6)
    else:
        ch = legend[1]
        yoff = {',': -2.5, '.': -3.0, ';': -1.0, "'": 2.5, '`': 2.5}.get(ch, 0.0)
        sym = text(ch, 10.5, cx, cy + yoff, max_w=12.5)
    ink(c, sym, col, edge=1.15)


CLUSTER_SPOTS = {'u': (22.5, 8.5), 'l': (1.5, 32.0), 'd': (22.5, 32.0), 'r': (43.5, 32.0)}


def cluster(legends, col=INK):
    """Four small keys in an inverted T; legends for up, left, down, right."""
    c = Canvas()
    for d, lg in zip('uldr', legends):
        mini_key(c, *CLUSTER_SPOTS[d], lg, col)
    return c


# mouse

def mouse(lit=None, col=INK, cx=32.0, cy=32.0, s=1.0, arrows=''):
    """lit: 'l', 'r', 'm' (wheel), 'x1' (back, the lower side button), 'x2' (forward), 'wu' / 'wd' (wheel + arrow).
    arrows: triangles around a small mouse ('udlr') for "move the mouse"."""
    c = Canvas()

    def X(v):
        return cx + (v - 32.0) * s

    def Y(v):
        return cy + (v - 32.0) * s

    if lit in ('wu', 'wd'):
        cx -= 6.0
    shape = Pen().poly(rrect_pts(X(16.5), Y(6), X(47.5), Y(58), tl=(15 * s, 17 * s), tr=(15 * s, 17 * s), br=(15.5 * s, 21 * s), bl=(15.5 * s, 21 * s))).mask()
    if lit in ('x1', 'x2') or lit == 'side':
        for name, (ya, yb) in (('x2', (21.5, 31.0)), ('x1', (33.0, 42.5))):
            nub = rrect(X(11.5), Y(ya), X(20), Y(yb), 2.8 * s)
            body(c, nub, shadow=False)
            if lit == name:
                ink(c, erode(nub, 0.4), col, edge=1.5)
    body(c, shape)
    inner = erode(shape, 1.5)
    seam = Pen().rect(X(10), Y(28.2), X(54), Y(29.8)).rect(X(31.2), Y(0), X(32.8), Y(29)).mask() * shape
    c.over(seam, (30, 32, 38))
    if lit in ('l', 'r'):
        half = Pen().rect(X(0) if lit == 'l' else X(33.6), Y(0), X(30.4) if lit == 'l' else X(64), Y(27.4)).mask()
        ink(c, half * erode(shape, 2.4), col, edge=1.5, clip=shape)
    wheel = rrect(X(28.6), Y(11.5), X(35.4), Y(25.5), 3.2 * s)
    if lit in ('m', 'wu', 'wd'):
        ink(c, wheel, col, edge=1.6, clip=shape)
    else:
        c.over(dilate(wheel, 1.3) * shape, (30, 32, 38))
        c.over(wheel, SIDE)
    if lit in ('wu', 'wd'):
        d = 'u' if lit == 'wu' else 'd'
        ink(c, tri(d, cx + 26.5, Y(18.5) + (0 if d == 'u' else 1), 6.6), col, edge=1.5)
    for d in arrows:
        x, y = {'u': (cx, cy - 25.5), 'd': (cx, cy + 25.5), 'l': (cx - 24.5, cy), 'r': (cx + 24.5, cy)}[d]
        ink(c, tri(d, x, y, 5.4), col, edge=1.4)
    return c


# ---- the set

def key_legends():
    """HID usage -> (legend, tag)."""
    k = {}
    for i in range(26):
        k[4 + i] = (('char', chr(65 + i)), None)
    for i in range(10):
        k[30 + i] = (('char', '1234567890'[i]), None)
        k[89 + i] = (('char', '1234567890'[i]), 'N')
    for i in range(12):
        k[58 + i] = (('word', 'F%d' % (i + 1)), None)
    k.update({
        40: (('sym', 'enter'), None), 41: (('word', 'ESC'), None), 42: (('sym', 'backspace'), None),
        43: (('sym', 'tab'), None), 44: (('sym', 'space'), None),
        45: (('char', '-'), None), 46: (('char', '='), None), 47: (('char', '['), None), 48: (('char', ']'), None),
        49: (('char', '\\'), None), 51: (('char', ';'), None), 52: (('char', "'"), None), 53: (('char', '`'), None),
        54: (('char', ','), None), 55: (('char', '.'), None), 56: (('char', '/'), None), 57: (('sym', 'caps'), None),
        70: (('two', 'PRT', 'SC'), None), 71: (('two', 'SCR', 'LK'), None), 72: (('word', 'PAUSE'), None),
        73: (('word', 'INS'), None), 74: (('word', 'HOME'), None), 75: (('two', 'PG', 'UP'), None),
        76: (('word', 'DEL'), None), 77: (('word', 'END'), None), 78: (('two', 'PG', 'DN'), None),
        79: (('arrow', 'r'), None), 80: (('arrow', 'l'), None), 81: (('arrow', 'd'), None), 82: (('arrow', 'u'), None),
        83: (('two', 'NUM', 'LK'), None), 84: (('char', '/'), 'N'), 85: (('char', '*'), 'N'), 86: (('char', '-'), 'N'),
        87: (('char', '+'), 'N'), 88: (('sym', 'enter'), 'N'), 99: (('char', '.'), 'N'), 101: (('word', 'MENU'), None),
        224: (('word', 'CTRL'), None), 225: (('sym', 'shift'), None), 226: (('word', 'ALT'), None),
        227: (('word', 'WIN'), None),
        228: (('word', 'CTRL'), 'R'), 229: (('sym', 'shift'), 'R'), 230: (('word', 'ALT'), 'R'),
        231: (('word', 'WIN'), 'R'),
    })
    return k


def mini_legends():
    """HID usage -> legend for the keys that have a small cap (one character or an arrow)."""
    out = {}
    for u, (lg, tag) in key_legends().items():
        if lg[0] in ('char', 'arrow'):
            out[u] = lg
    return out


def build_icons(only=None):
    """name -> 64 x 64 RGBA8 array. Names: key/<usage>[/hold], mouse/<n>[/hold], xbox/<id>, ps/<id>, pad/<id>."""
    jobs = {}

    def add(name, fn):
        jobs[name] = fn

    for u, (lg, tag) in sorted(key_legends().items()):
        add('key/%d' % u, lambda lg=lg, tag=tag: keycap(lg, INK, tag))
        add('key/%d/hold' % u, lambda lg=lg, tag=tag: keycap(lg, HOLD, tag))
    for n, lit in ((0, None), (1, 'l'), (2, 'm'), (3, 'r'), (4, 'x1'), (5, 'x2'), (6, 'wu'), (7, 'wd')):
        if n == 0:
            add('mouse/0', lambda: mouse(None, INK, cy=32.0, s=0.62, arrows='udlr'))
            add('mouse/0/hold', lambda: mouse(None, HOLD, cy=32.0, s=0.62, arrows='udlr'))
        else:
            add('mouse/%d' % n, lambda lit=lit: mouse(lit, INK))
            add('mouse/%d/hold' % n, lambda lit=lit: mouse(lit, HOLD))

    for hold, col in (('', None), ('_hold', HOLD)):
        for ch, xc in (('a', XB_GREEN), ('b', XB_RED), ('x', XB_BLUE), ('y', XB_YELLOW)):
            add('xbox/%s%s' % (ch, hold), lambda ch=ch, c2=col or xc: face_letter(ch.upper(), c2))
        for kind, pc in (('cross', PS_BLUE), ('circle', PS_RED), ('square', PS_PINK), ('triangle', PS_GREEN)):
            add('ps/%s%s' % (kind, hold), lambda kind=kind, c2=col or pc: face_shape(kind, c2))
        c2 = col or INK
        for k, label in (('lb', 'LB'), ('rb', 'RB'), ('lt', 'LT'), ('rt', 'RT')):
            add('xbox/%s%s' % (k, hold), lambda k=k, label=label, c2=c2: shoulder(label, k, c2))
        for k, label in (('l1', 'L1'), ('r1', 'R1'), ('l2', 'L2'), ('r2', 'R2')):
            add('ps/%s%s' % (k, hold), lambda k=k, label=label, c2=c2: shoulder(label, k, c2))
        for style in ('xbox', 'ps'):
            for nm, lit in (('up', 'u'), ('down', 'd'), ('left', 'l'), ('right', 'r'), ('lr', 'lr'), ('ud', 'ud')):
                add('%s/dpad_%s%s' % (style, nm, hold), lambda style=style, lit=lit, c2=c2: dpad(style, lit, c2))
    for style in ('xbox', 'ps'):
        add('%s/dpad' % style, lambda style=style: dpad(style))
    add('xbox/menu', lambda: disc_button(lambda: sym_menu(CX, CY)))
    add('xbox/view', lambda: disc_button(lambda: sym_view(CX, CY)))
    add('xbox/menu_alone', lambda: lone_symbol(sym_menu(32, 32, 1.15)))
    add('xbox/view_alone', lambda: lone_symbol(sym_view(32, 32, 1.15)))
    add('ps/options', lambda: disc_button(lambda: sym_menu(CX, CY)))
    add('ps/create', lambda: disc_button(lambda: sym_create(CX, CY - 3)))
    add('ps/options_alone', lambda: lone_symbol(sym_menu(32, 32, 1.15)))
    add('ps/create_alone', lambda: lone_symbol(sym_create(32, 29, 1.15)))
    add('xbox/diamond', lambda: diamond([(lambda x, y: text('Y', 10.5, x, y), XB_YELLOW), (lambda x, y: text('X', 10.5, x, y), XB_BLUE),
                                         (lambda x, y: text('B', 10.5, x, y), XB_RED), (lambda x, y: text('A', 10.5, x, y), XB_GREEN)]))
    add('ps/diamond', lambda: diamond([(lambda x, y: ps_shape('triangle', x, y, 4.6, 2.1), PS_GREEN), (lambda x, y: ps_shape('square', x, y, 5.2, 2.1), PS_PINK),
                                       (lambda x, y: ps_shape('circle', x, y, 5.2, 2.1), PS_RED), (lambda x, y: ps_shape('cross', x, y, 5.2, 2.3), PS_BLUE)]))
    # sticks (shared by both pads)
    for nm, label in (('ls', 'L'), ('rs', 'R'), ('stick', '')):
        add('pad/%s' % nm, lambda label=label: stick(label))
        add('pad/%s_push' % nm, lambda label=label: stick(label, push=True))
        add('pad/%s_roll_l' % nm, lambda label=label: stick(label, roll='l'))
        add('pad/%s_roll_r' % nm, lambda label=label: stick(label, roll='r'))

    out = {}
    for name, fn in jobs.items():
        if only and not any(name.startswith(o) for o in only):
            continue
        out[name] = fn().image()
    return out


def build_minis(only=None):
    """usage -> (RGBA tile, hold RGBA tile): one small key drawn at the cluster's 'd' spot, cropped to MINI_BOX."""
    out = {}
    if only and not any(o.startswith('mini') for o in only):
        return out
    x, y = CLUSTER_SPOTS['d']
    x0, y0, x1, y1 = mini_box()
    for u, lg in sorted(mini_legends().items()):
        tiles = []
        for col in (INK, HOLD):
            c = Canvas()
            mini_key(c, x, y, lg, col)
            tiles.append(c.image()[y0:y1, x0:x1].copy())
        out[u] = tuple(tiles)
    return out


def mini_box():
    """The pixels a small key at the 'd' spot can touch (outline and shadow included)."""
    x, y = CLUSTER_SPOTS['d']
    return int(x) - 2, int(y) - 2, int(x) - 2 + 28, int(y) - 2 + 30


def compose_cluster(minis, usages, hold=False):
    """The cluster icon the game code builds from the small keys (mirrors prompt_art.cpp)."""
    x0, y0, x1, y1 = mini_box()
    bx, by = CLUSTER_SPOTS['d']
    out = np.zeros((ICON, ICON, 4), np.float32)
    for d, u in zip('uldr', usages):
        tile = minis[u][1 if hold else 0].astype(np.float32)
        ox = int(round(CLUSTER_SPOTS[d][0] - bx)) + x0
        oy = int(round(CLUSTER_SPOTS[d][1] - by)) + y0
        for ty in range(tile.shape[0]):
            for tx in range(tile.shape[1]):
                X, Y = ox + tx, oy + ty
                if not (0 <= X < ICON and 0 <= Y < ICON):
                    continue
                sa = tile[ty, tx, 3] / 255.0
                if sa <= 0:
                    continue
                da = out[Y, X, 3] / 255.0
                oa = sa + da * (1 - sa)
                out[Y, X, :3] = (tile[ty, tx, :3] * sa + out[Y, X, :3] * da * (1 - sa)) / oa
                out[Y, X, 3] = oa * 255.0
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


# ---- contact sheet

def read_game_pack(path):
    """The game's own icons (name -> RGBA), only to show beside ours on the contact sheet. Never written to the repo."""
    import io
    d = open(path, 'rb').read()
    if d[:8] != b'TLSK3100':
        return {}
    n, params = struct.unpack_from('<I', d, 8)[0], struct.unpack_from('<I', d, 0x10)[0]
    offs = 0x40 + params
    p = offs + 0x14 * n
    out = {}
    for i in range(n):
        e = d.index(b'\0', p)
        name = d[p:e].decode()
        p = e + 1
        at = struct.unpack_from('<I', d, offs + 0x14 * i)[0]
        w, h = struct.unpack_from('<HH', d, at + 4)
        size = struct.unpack_from('<I', d, at + 0x1c)[0]
        if (w, h, size) != (64, 64, 4096):
            continue
        dds = b'DDS ' + struct.pack('<7I', 124, 0x81007, h, w, size, 0, 1) + b'\0' * 44 + \
            struct.pack('<2I4s5I', 32, 4, b'DXT5', 0, 0, 0, 0, 0) + struct.pack('<5I', 0x1000, 0, 0, 0, 0)
        out[name] = np.asarray(Image.open(io.BytesIO(dds + d[at + 0x48:at + 0x48 + size])).convert('RGBA'))
    return out


def label_image(s, cap, col=(214, 220, 232)):
    """A line of text in our alphabet (for the sheet's headings)."""
    s = s.upper()
    w = int(text_width(s, cap, 0.14) + 4)
    scale = 4
    im = Image.new('L', (w * scale, int(cap * 1.5) * scale), 0)
    d = ImageDraw.Draw(im)
    x = 1.0
    for ch in s:
        adv, ops = GLYPHS.get(ch, GLYPHS[' '])
        for op in ops:
            pts = [((x + px * cap) * scale, (cap * 0.2 + py * cap) * scale) for px, py in op[1]]
            if op[0] == 'S':
                wd = op[2] * cap * scale
                for i in range(len(pts) - 1):
                    (ax, ay), (bx, by) = pts[i], pts[i + 1]
                    ln = math.hypot(bx - ax, by - ay) or 1
                    nx, ny = -(by - ay) / ln * wd / 2, (bx - ax) / ln * wd / 2
                    d.polygon([(ax + nx, ay + ny), (bx + nx, by + ny), (bx - nx, by - ny), (ax - nx, ay - ny)], fill=255)
                for ax, ay in pts[1:-1]:
                    d.ellipse([ax - wd / 2, ay - wd / 2, ax + wd / 2, ay + wd / 2], fill=255)
            else:
                d.polygon(pts, fill=255 if op[0] == 'P' else 0)
        x += (adv + 0.14) * cap
    a = im.resize((w, int(cap * 1.5)), Image.LANCZOS)
    out = Image.new('RGBA', a.size, col + (0,))
    out.putalpha(a)
    return out


def make_sheet(path, icons, minis, game):
    rows = []  # (heading, [RGBA arrays])

    def pick(names):
        return [icons[n] for n in names if n in icons]

    if game:
        order = ['btn_A_icon', 'btn_B_icon', 'btn_X_icon', 'btn_Y_icon', 'btn_A_icon_hold', 'btn_Y_icon_hold', 'btn_L_icon', 'btn_R_icon',
                 'btn_ZL_icon', 'btn_ZR_icon', 'btn_L_icon_hold', 'btn_Plus_icon', 'btn_Minus_icon', 'btn_Plus_JOY_icon', 'btn_Minus_JOY_icon',
                 'stick_btn_L_icon', 'stick_btn_R_icon', 'stick_btn_L_push_icon', 'stick_btn_R_push_icon', 'stick_L_roll_L_icon',
                 'stick_R_roll_R_icon', 'btn_Cross_empty_icon', 'btn_Cross_UP_icon', 'btn_Cross_LR_icon', 'btn_dir_all_ABXY_icon']
        rows.append(("The game's own Switch icons (for comparison only)", [game[n] for n in order if n in game]))
    xb = ['a', 'b', 'x', 'y', 'a_hold', 'y_hold', 'lb', 'rb', 'lt', 'rt', 'lb_hold', 'menu', 'view', 'menu_alone', 'view_alone']
    st = ['ls', 'rs', 'ls_push', 'rs_push', 'ls_roll_l', 'rs_roll_r']
    rows.append(('Xbox', pick(['xbox/' + n for n in xb] + ['pad/' + n for n in st] + ['xbox/dpad', 'xbox/dpad_up', 'xbox/dpad_lr', 'xbox/diamond'])))
    ps = ['cross', 'circle', 'square', 'triangle', 'cross_hold', 'triangle_hold', 'l1', 'r1', 'l2', 'r2', 'l1_hold', 'options', 'create',
          'options_alone', 'create_alone']
    rows.append(('PlayStation', pick(['ps/' + n for n in ps] + ['pad/' + n for n in st] + ['ps/dpad', 'ps/dpad_up', 'ps/dpad_lr', 'ps/diamond'])))
    rows.append(('Sticks and d-pads, the rest', pick(['pad/stick', 'pad/stick_push', 'pad/stick_roll_l', 'pad/stick_roll_r', 'pad/ls_roll_r', 'pad/rs_roll_l',
                                                     'xbox/dpad_down', 'xbox/dpad_left', 'xbox/dpad_right', 'xbox/dpad_ud', 'xbox/dpad_left_hold',
                                                     'ps/dpad_down', 'ps/dpad_left', 'ps/dpad_right', 'ps/dpad_ud', 'ps/dpad_right_hold',
                                                     'xbox/rb_hold', 'xbox/lt_hold', 'ps/r2_hold', 'xbox/b_hold', 'ps/circle_hold', 'ps/square_hold'])))
    clusters = []
    if minis:
        for us in ((26, 4, 22, 7), (82, 80, 81, 79), (12, 13, 14, 15), (96, 92, 93, 94)):
            if all(u in minis for u in us):
                clusters.append(compose_cluster(minis, us))
        if all(u in minis for u in (26, 4, 22, 7)):
            clusters.append(compose_cluster(minis, (26, 4, 22, 7), True))
    rows.append(('Mouse and stick clusters (WASD, arrows, any four keys)',
                 pick(['mouse/%d' % n for n in (1, 3, 2, 6, 7, 4, 5, 0)] + ['mouse/1/hold', 'mouse/3/hold']) + clusters))
    rows.append(('Keyboard: letters', pick(['key/%d' % u for u in range(4, 30)])))
    rows.append(('Keyboard: digits, F keys', pick(['key/%d' % u for u in list(range(30, 40)) + list(range(58, 70))])))
    rows.append(('Keyboard: named keys', pick(['key/%d' % u for u in (44, 40, 41, 43, 225, 229, 224, 228, 226, 230, 42, 57, 82, 81, 80, 79, 73, 76, 74, 77, 75, 78, 227, 101)])))
    rows.append(('Keyboard: punctuation, number pad, the rest', pick(['key/%d' % u for u in (45, 46, 47, 48, 49, 51, 52, 53, 54, 55, 56, 98, 89, 90, 91, 92, 93, 94, 95, 96, 97, 99, 84, 85, 86, 87, 88, 83, 70, 71, 72)])))
    rows.append(('Keyboard: hold (orange, as the game marks a held button)', pick(['key/%d/hold' % u for u in (8, 9, 20, 44, 225, 40, 82, 59, 30, 93, 228)])))

    per_row, cell, gap, head = 16, ICON, 6, 26
    lines = []
    for heading, ims in rows:
        if not ims:
            continue
        lines.append(('h', heading))
        for i in range(0, len(ims), per_row):
            lines.append(('i', ims[i:i + per_row]))
    W = 24 * 2 + per_row * (cell + gap)
    Hh = 20 + sum(head if k == 'h' else cell + gap for k, _ in lines) + 16
    # a backdrop like the game's menus: dark blue-black with a lighter band behind every other row
    sheet = Image.new('RGBA', (W, Hh), (10, 14, 24, 255))
    y = 20
    band = 0
    for k, v in lines:
        if k == 'h':
            sheet.alpha_composite(label_image(v, 11), (24, y + 6))
            y += head
        else:
            if band % 2:
                ImageDraw.Draw(sheet).rectangle([12, y - 3, W - 12, y + cell + 2], fill=(34, 40, 56, 255))
            band += 1
            for i, im in enumerate(v):
                sheet.alpha_composite(Image.fromarray(im, 'RGBA'), (24 + i * (cell + gap), y))
            y += cell + gap
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    sheet.convert('RGB').save(path)
    big = sheet.resize((W * 2, Hh * 2), Image.NEAREST).convert('RGB')
    big.save(os.path.splitext(path)[0] + '_2x.png')
    return sheet.size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sheet')
    ap.add_argument('--game-pack')
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', '..', 'src', 'core', 'hle', 'prompt_art_data.cpp'))
    ap.add_argument('--no-embed', action='store_true')
    a = ap.parse_args()
    icons = build_icons(a.only)
    minis = build_minis(a.only)
    print('%d icons, %d small keys' % (len(icons), len(minis)))
    if a.sheet:
        game = read_game_pack(a.game_pack) if a.game_pack else {}
        print('sheet', make_sheet(a.sheet, icons, minis, game))
    if not a.no_embed and not a.only:
        import embed
        embed.write(a.out, icons, minis, mini_box(), CLUSTER_SPOTS)


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
