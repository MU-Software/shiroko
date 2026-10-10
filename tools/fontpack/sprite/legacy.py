"""Symbols for Legacy Computing 1FB00-1FBEF and its supplement 1CC1B-1CEAF (octants and friends)."""
import math

from . import aa
from .box import BL, BR, TL, TR, diagonal
from .common import (HEAVY, LIGHT, block, clip_to_cell, fill, hline_middle, stroke_px, unicode_names, vline_middle,
                     zround)
from .registry import draws

EIGHTHS = [i / 8 for i in range(9)]
QUARTERS = [0.0, 0.25, 0.5, 0.75, 1.0]
THIRDS = [0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0]
HALVES = [0.0, 0.5, 1.0]
MEDIUM = 0x80


def _r(lo, hi):
    return list(range(lo, hi + 1))


# ---- sextants 1FB00-1FB3B, octants 1CD00-1CDE5

def sextant_mask(cp):
    """Bits tl, tr, ml, mr, bl, br; the code points skip the left column (21) and the right column (42)."""
    i = cp - 0x1FB00
    return i + i // 20 + 1


def draw_sextants(c, m, mask):
    for b in range(6):
        if mask >> b & 1:
            col, row = b % 2, b // 2
            fill(c, m, HALVES[col], HALVES[col + 1], THIRDS[row], THIRDS[row + 1])


@draws(0x1FB00, 0x1FB3B)
def sextants(cp, c, m):
    draw_sextants(c, m, sextant_mask(cp))


def octant_mask(cp):
    """Bit n - 1 = octant n (1 2 / 3 4 / 5 6 / 7 8), from the name "BLOCK OCTANT-1235"."""
    return sum(1 << (int(d) - 1) for d in unicode_names()[cp].removeprefix("BLOCK OCTANT-"))


def draw_octants(c, m, mask):
    for b in range(8):
        if mask >> b & 1:
            col, row = b % 2, b // 2
            fill(c, m, HALVES[col], HALVES[col + 1], QUARTERS[row], QUARTERS[row + 1])


@draws(0x1CD00, 0x1CDE5)
def octants(cp, c, m):
    draw_octants(c, m, octant_mask(cp))


# ---- smooth mosaics and triangles 1FB3C-1FB6F, 1FB9A-1FB9F

# 1FB3C-1FB67: 4 rows x 3 columns, '#' filled, '.' empty, '/' '\' diagonal halves (the outline runs through the
# corner and edge points mosaic_points picks)
MOSAICS = (
    "...|...|#..|##.", "...|...|#\\.|###", "...|#..|#\\.|##.", "...|#..|##.|###", "#..|#..|##.|##.",
    "/##|###|###|###", "./#|###|###|###", ".##|.##|###|###", "..#|.##|###|###", ".##|.##|.##|###", "...|./#|###|###",
    "...|...|..#|.##", "...|...|./#|###", "...|..#|./#|.##", "...|..#|.##|###", "..#|..#|.##|.##",
    "##\\|###|###|###", "#\\.|###|###|###", "##.|##.|###|###", "#..|##.|###|###", "##.|##.|##.|###", "...|#\\.|###|###",
    "###|###|###|\\##", "###|###|###|.\\#", "###|###|.##|.##", "###|###|.##|..#", "###|.##|.##|.##",
    "##.|#..|...|...", "###|#/.|...|...", "##.|#/.|#..|...", "###|##.|#..|...", "##.|##.|#..|#..",
    "###|###|#/.|...", "###|###|###|##/", "###|###|###|#/.", "###|###|##.|##.", "###|###|##.|#..", "###|##.|##.|##.",
    ".##|..#|...|...", "###|.\\#|...|...", ".##|.\\#|..#|...", "###|.##|..#|...", ".##|.##|..#|..#", "###|###|.\\#|...",
)


def mosaic_points(pattern, m):
    on = [ch == "#" for ch in pattern.replace("|", "\n")]
    w, h = m.cell_width, m.cell_height
    top, upper, lower, bottom = 0.0, (1.0 / 3.0) * h, (2.0 / 3.0) * h, float(h)
    left, center, right = 0.0, 0.5 * w, float(w)
    pts = []
    if on[0]:
        pts.append((left, top))
    if on[4] and (not on[0] or not on[8]):
        pts.append((left, upper))
    if on[8] and (not on[4] or not on[12]):
        pts.append((left, lower))
    if on[12]:
        pts.append((left, bottom))
    if on[13] and (not on[12] or not on[14]):
        pts.append((center, bottom))
    if on[14]:
        pts.append((right, bottom))
    if on[10] and (not on[14] or not on[6]):
        pts.append((right, lower))
    if on[6] and (not on[10] or not on[2]):
        pts.append((right, upper))
    if on[2]:
        pts.append((right, top))
    if on[1] and (not on[2] or not on[0]):
        pts.append((center, top))
    return pts


def edge_triangle(c, m, edge):
    w, h = m.cell_width, m.cell_height
    a, b = {"top": ((w, 0), (0, 0)), "left": ((0, 0), (0, h)),
            "bottom": ((0, h), (w, h)), "right": ((w, h), (w, 0))}[edge]
    aa.polygon(c, [(zround(w / 2), zround(h / 2)), a, b])


def corner_triangle(c, m, corner, shade=255):
    w, h = m.cell_width, m.cell_height
    aa.polygon(c, {TL: [(0, 0), (0, h), (w, 0)], TR: [(0, 0), (w, h), (w, 0)],
                   BL: [(0, 0), (0, h), (w, h)], BR: [(0, h), (w, h), (w, 0)]}[corner], shade=shade)


@draws(0x1FB3C, 0x1FB6F)
def wedges(cp, c, m):
    if cp <= 0x1FB67:
        aa.polygon(c, mosaic_points(MOSAICS[cp - 0x1FB3C], m))
        return
    k = cp - 0x1FB68
    edge_triangle(c, m, ("left", "top", "right", "bottom")[k % 4])
    if k < 4:
        c.invert()
        clip_to_cell(c)


@draws(0x1FB9A, 0x1FB9F)
def triangles(cp, c, m):
    if cp == 0x1FB9A:
        edge_triangle(c, m, "top")
        edge_triangle(c, m, "bottom")
    elif cp == 0x1FB9B:
        edge_triangle(c, m, "left")
        edge_triangle(c, m, "right")
    else:
        corner_triangle(c, m, (TL, TR, BR, BL)[cp - 0x1FB9C], shade=MEDIUM)


# ---- eighth blocks and friends 1FB70-1FB99, 1FBCE-1FBCF

def vstrip(c, m, n):
    fill(c, m, EIGHTHS[n], EIGHTHS[n + 1], 0.0, 1.0)


def hstrip(c, m, n):
    fill(c, m, 0.0, 1.0, EIGHTHS[n], EIGHTHS[n + 1])


@draws(0x1FB70, 0x1FB75)
def vertical_eighths(cp, c, m):
    vstrip(c, m, cp + 1 - 0x1FB70)


@draws(0x1FB76, 0x1FB7B)
def horizontal_eighths(cp, c, m):
    hstrip(c, m, cp + 1 - 0x1FB76)


E = 1 / 8
EIGHTH_BLOCKS = {
    0x1FB7C: (("l", "m", E, 1), ("c", "b", 1, E)),
    0x1FB7D: (("l", "m", E, 1), ("c", "t", 1, E)),
    0x1FB7E: (("r", "m", E, 1), ("c", "t", 1, E)),
    0x1FB7F: (("r", "m", E, 1), ("c", "b", 1, E)),
    0x1FB80: (("c", "t", 1, E), ("c", "b", 1, E)),
    0x1FB82: (("c", "t", 1, 0.25),),
    0x1FB83: (("c", "t", 1, 0.375),),
    0x1FB84: (("c", "t", 1, 0.625),),
    0x1FB85: (("c", "t", 1, 0.75),),
    0x1FB86: (("c", "t", 1, 0.875),),
    0x1FB87: (("r", "m", 0.25, 1),),
    0x1FB88: (("r", "m", 0.375, 1),),
    0x1FB89: (("r", "m", 0.625, 1),),
    0x1FB8A: (("r", "m", 0.75, 1),),
    0x1FB8B: (("r", "m", 0.875, 1),),
    0x1FBCE: (("l", "m", 2.0 / 3.0, 1),),
    0x1FBCF: (("l", "m", 1.0 / 3.0, 1),),
}
SHADED = {
    0x1FB8C: ("l", "m", 0.5, 1), 0x1FB8D: ("r", "m", 0.5, 1),
    0x1FB8E: ("c", "t", 1, 0.5), 0x1FB8F: ("c", "b", 1, 0.5),
}
HALF_OVER_MEDIUM = {0x1FB91: ("c", "t", 1, 0.5), 0x1FB92: ("c", "b", 1, 0.5), 0x1FB94: ("r", "m", 0.5, 1)}


def checkerboard(c, m, parity):
    w, h = m.cell_width, m.cell_height
    ny = zround(4 * (h / w))
    for x in range(4):
        for y in range(ny):
            if (x + y) % 2 == parity:
                c.rect(w * x // 4, h * y // ny, w * (x + 1) // 4, h * (y + 1) // ny)


@draws(0x1FB7C, 0x1FB92)
@draws(0x1FB94, 0x1FB97)
@draws(0x1FBCE, 0x1FBCF)
def eighth_blocks(cp, c, m):
    w, h = m.cell_width, m.cell_height
    if cp in EIGHTH_BLOCKS:
        for args in EIGHTH_BLOCKS[cp]:
            block(c, m, *args)
    elif cp == 0x1FB81:
        for n in (0, 2, 4, 7):
            hstrip(c, m, n)
    elif cp in SHADED:
        block(c, m, *SHADED[cp], a=MEDIUM)
    elif cp == 0x1FB90:
        c.rect(0, 0, w, h, MEDIUM)
    elif cp in HALF_OVER_MEDIUM:
        c.rect(0, 0, w, h, MEDIUM)
        block(c, m, *HALF_OVER_MEDIUM[cp])
    elif cp in (0x1FB95, 0x1FB96):
        checkerboard(c, m, cp - 0x1FB95)
    else:  # 1FB97
        c.rect(0, h // 4, w, 2 * h // 4)
        c.rect(0, 3 * h // 4, w, h)


@draws(0x1FB98, 0x1FB99)
def diagonal_fills(cp, c, m):
    """Parallel light diagonals (they do not line up across cells for most sizes, as in Ghostty)."""
    clip_to_cell(c)
    w, h, t = m.cell_width, m.cell_height, stroke_px(m)
    n = w // (2 * t)
    step = zround(w / n)
    for i in range(-n, n + 1):
        a = i * step
        if cp == 0x1FB98:
            aa.line(c, a, 0, w + a, h, t)
        else:
            aa.line(c, w + a, 0, a, h, t)


# ---- diagonals and circles 1FBA0-1FBAE, 1FBBD-1FBBF, 1FBD0-1FBEF

def corner_lines(c, m, corners):
    """Lines from the middle of the top or bottom edge to the middle of a side edge."""
    w, h, t = m.cell_width, m.cell_height, stroke_px(m)
    cx, cy = float(w // 2 + w % 2), float(h // 2 + h % 2)
    ends = {TL: (cx, 0, 0, cy), TR: (cx, 0, w, cy), BL: (cx, h, 0, cy), BR: (cx, h, w, cy)}
    for k in (TL, TR, BL, BR):
        if k in corners:
            aa.line(c, *ends[k], t)


CORNER_LINES = ((TL,), (TR,), (BL,), (BR,), (TL, BL), (TR, BR), (BL, BR), (TL, TR), (TL, BR), (TR, BL),
                (TR, BL, BR), (TL, BL, BR), (TL, TR, BR), (TL, TR, BL), (TL, TR, BL, BR))


@draws(0x1FBA0, 0x1FBAE)
def corner_diagonals(cp, c, m):
    corner_lines(c, m, CORNER_LINES[cp - 0x1FBA0])


@draws(0x1FBAF)
def heavy_vertical_light_horizontal(cp, c, m):
    vline_middle(c, m, HEAVY)
    hline_middle(c, m, LIGHT)


@draws(0x1FBBD, 0x1FBBF)
def inverse_diagonals(cp, c, m):
    if cp == 0x1FBBD:
        diagonal(c, m, True)
        diagonal(c, m, False)
    else:
        corner_lines(c, m, (BR,) if cp == 0x1FBBE else (TL, TR, BL, BR))
    c.invert()
    clip_to_cell(c)


def _align(a, w, h):
    v, hz = a
    return {"l": 0.0, "r": float(w), "c": w / 2}[hz], {"t": 0.0, "b": float(h), "m": h / 2}[v]


# 1FBD0-1FBDF: (from, to) alignments as vertical t/m/b + horizontal l/c/r
CELL_DIAGONALS = (
    [("mr", "bl")], [("tr", "ml")], [("tl", "mr")], [("ml", "br")], [("tl", "bc")], [("tc", "br")], [("tr", "bc")],
    [("tc", "bl")], [("tl", "mc"), ("mc", "tr")], [("tr", "mc"), ("mc", "br")], [("bl", "mc"), ("mc", "br")],
    [("tl", "mc"), ("mc", "bl")], [("tl", "bc"), ("bc", "tr")], [("tr", "ml"), ("ml", "br")],
    [("bl", "tc"), ("tc", "br")], [("tl", "mr"), ("mr", "bl")],
)


def circle(c, m, x, y, filled):
    """Circle of radius min(w, h) / 2 at (x, y); outlined circles stroke at r - t / 2 so they stay inside."""
    clip_to_cell(c)
    r, t = 0.5 * min(m.cell_width, m.cell_height), stroke_px(m)
    p = aa.Path(c)
    p.arc(x, y, r if filled else r - t / 2, 0, 2 * math.pi)
    p.close()
    if filled:
        aa.fill(c, p)
    else:
        aa.stroke(c, p, t)


@draws(0x1FBD0, 0x1FBEF)
def cell_diagonals_circles(cp, c, m):
    w, h = m.cell_width, m.cell_height
    if cp <= 0x1FBDF:
        for a, b in CELL_DIAGONALS[cp - 0x1FBD0]:
            aa.line(c, *_align(a, w, h), *_align(b, w, h), stroke_px(m))
        return
    k = cp - 0x1FBE0
    if k < 4:
        circle(c, m, *_align(("tc", "mr", "bc", "ml")[k], w, h), False)
    elif k < 8:
        block(c, m, *(("c", "t"), ("c", "b"), ("l", "m"), ("r", "m"))[k - 4], 0.5, 0.5)
    elif k < 12:
        circle(c, m, *_align(("tc", "mr", "bc", "ml")[k - 8], w, h), True)
    else:
        circle(c, m, *_align(("tr", "bl", "br", "tl")[k - 12], w, h), True)


# ---- supplement: line pieces, circles, separated blocks, sixteenths

@draws(0x1CC1B, 0x1CC1E)
def light_corner_pieces(cp, c, m):
    w, h, t = m.cell_width, m.cell_height, stroke_px(m)
    if cp == 0x1CC1B:
        hline_middle(c, m)
        c.rect(w - t, 0, w, h // 2)
    elif cp == 0x1CC1C:
        hline_middle(c, m)
        c.rect(w - t, h // 2, w, h)
    elif cp == 0x1CC1D:
        c.rect(0, 0, w, t)
        c.rect(0, 0, t, h // 2)
    else:
        c.rect(0, h - t, w, h)
        c.rect(0, h // 2, t, h)


@draws(0x1CE16, 0x1CE19)
def light_vertical_ticks(cp, c, m):
    w, h, t = m.cell_width, m.cell_height, stroke_px(m)
    vline_middle(c, m)
    x0, x1 = (w // 2, w) if cp in (0x1CE16, 0x1CE17) else (0, w // 2)
    y0, y1 = (0, t) if cp in (0x1CE16, 0x1CE18) else (h - t, h)
    c.rect(x0, y0, x1, y1)


@draws(0x1CE00, 0x1CE01)
def white_circles(cp, c, m):
    w, h = m.cell_width, m.cell_height
    for x, y in (((0, h / 2), (w, h / 2)) if cp == 0x1CE00 else ((w / 2, 0), (w / 2, h))):
        circle(c, m, x, y, False)


def circle_piece(c, m, x, y, w, h, corner):
    """Quarter-ellipse stroke of a w x h cells ellipse; (x, y) = this cell's offset in cells."""
    clip_to_cell(c)
    wd, hg, xp, yp = m.cell_width * w, m.cell_height * h, m.cell_width * x, m.cell_height * y
    k = (math.sqrt(2) - 1.0) * 4.0 / 3.0
    cw, ch = k * wd, k * hg
    t = stroke_px(m)
    ht = t * 0.5
    p = aa.Path(c)
    if corner == TL:
        p.move_to(wd - xp, ht - yp)
        p.curve_to(wd - cw - xp, ht - yp, ht - xp, hg - ch - yp, ht - xp, hg - yp)
    elif corner == TR:
        p.move_to(wd - xp, ht - yp)
        p.curve_to(wd + cw - xp, ht - yp, wd * 2 - ht - xp, hg - ch - yp, wd * 2 - ht - xp, hg - yp)
    elif corner == BL:
        p.move_to(ht - xp, hg - yp)
        p.curve_to(ht - xp, hg + ch - yp, wd - cw - xp, hg * 2 - ht - yp, wd - xp, hg * 2 - ht - yp)
    else:
        p.move_to(wd * 2 - ht - xp, hg - yp)
        p.curve_to(wd * 2 - ht - xp, hg + ch - yp, wd + cw - xp, hg * 2 - ht - yp, wd - xp, hg * 2 - ht - yp)
    aa.stroke(c, p, t)


@draws(0x1CE0B, 0x1CE0C)
def half_ellipses(cp, c, m):
    x = 0 if cp == 0x1CE0B else 1
    for k in ((TL, BL) if cp == 0x1CE0B else (TR, BR)):
        circle_piece(c, m, x, 0, 1, 0.5, k)


CIRCLE_PIECES = {
    0x1CC30: (0, 0, 2, 2, TL), 0x1CC31: (1, 0, 2, 2, TL), 0x1CC32: (2, 0, 2, 2, TR), 0x1CC33: (3, 0, 2, 2, TR),
    0x1CC34: (0, 1, 2, 2, TL), 0x1CC35: (0, 0, 1, 1, TL), 0x1CC36: (1, 0, 1, 1, TR), 0x1CC37: (3, 1, 2, 2, TR),
    0x1CC38: (0, 2, 2, 2, BL), 0x1CC39: (0, 1, 1, 1, BL), 0x1CC3A: (1, 1, 1, 1, BR), 0x1CC3B: (3, 2, 2, 2, BR),
    0x1CC3C: (0, 3, 2, 2, BL), 0x1CC3D: (1, 3, 2, 2, BL), 0x1CC3E: (2, 3, 2, 2, BR), 0x1CC3F: (3, 3, 2, 2, BR),
}


@draws(0x1CC30, 0x1CC3F)
def circle_pieces(cp, c, m):
    circle_piece(c, m, *CIRCLE_PIECES[cp])


def separated_quadrant_rects(w, h):
    gap = max(1, w // 12)
    mx, my = gap * 2 + w % 2, gap * 2 + h % 2
    qw, qh = max(1, (w - gap * 2 - mx) // 2), max(1, (h - gap * 2 - my) // 2)
    mx, my = min(mx, w - gap * 2 - qw * 2), min(my, h - gap * 2 - qh * 2)  # small cells: pieces keep 1 px
    xs, ys = (gap, gap + qw + mx), (gap, gap + qh + my)
    return [(xs[b % 2], ys[b // 2], xs[b % 2] + qw, ys[b // 2] + qh) for b in range(4)]


def separated_sextant_rects(w, h):
    gap = max(1, w // 12)
    mx, my = gap * 2 + w % 2, gap * 2 + (h % 3) // 2
    sw = max(1, (w - gap * 2 - mx) // 2)
    sh = max(1, (h - gap * 2 - my * 2) // 3)
    mx, my = min(mx, w - gap * 2 - sw * 2), min(my, (h - gap * 2 - sh * 3) // 2)
    mh = h - gap * 2 - my * 2 - sh * 2
    xs = (gap, gap + sw + mx)
    y1 = gap + sh + my
    y2 = y1 + mh + my
    rows = ((gap, gap + sh), (y1, y1 + mh), (y2, y2 + sh))
    return [(xs[b % 2], rows[b // 2][0], xs[b % 2] + sw, rows[b // 2][1]) for b in range(6)]


@draws(0x1CC21, 0x1CC2F)
def separated_quadrants(cp, c, m):
    for b, r in enumerate(separated_quadrant_rects(m.cell_width, m.cell_height)):
        if (cp - 0x1CC20) >> b & 1:
            c.rect(*r)


@draws(0x1CE51, 0x1CE8F)
def separated_sextants(cp, c, m):
    for b, r in enumerate(separated_sextant_rects(m.cell_width, m.cell_height)):
        if (cp - 0x1CE50) >> b & 1:
            c.rect(*r)


# (x0, x1, y0, y1) in quarters
SIXTEENTHS = {0x1CE90 + i: (i % 4, i % 4 + 1, i // 4, i // 4 + 1) for i in range(16)}
SIXTEENTHS.update({
    0x1CEA0: (2, 4, 3, 4), 0x1CEA1: (1, 4, 3, 4), 0x1CEA2: (0, 3, 3, 4), 0x1CEA3: (0, 2, 3, 4),
    0x1CEA4: (0, 1, 2, 4), 0x1CEA5: (0, 1, 1, 4), 0x1CEA6: (0, 1, 0, 3), 0x1CEA7: (0, 1, 0, 2),
    0x1CEA8: (0, 2, 0, 1), 0x1CEA9: (0, 3, 0, 1), 0x1CEAA: (1, 4, 0, 1), 0x1CEAB: (2, 4, 0, 1),
    0x1CEAC: (3, 4, 0, 2), 0x1CEAD: (3, 4, 0, 3), 0x1CEAE: (3, 4, 1, 4), 0x1CEAF: (3, 4, 2, 4),
})


@draws(0x1CE90, 0x1CEAF)
def sixteenths(cp, c, m):
    x0, x1, y0, y1 = SIXTEENTHS[cp]
    fill(c, m, QUARTERS[x0], QUARTERS[x1], QUARTERS[y0], QUARTERS[y1])


GROUPS = {
    "sextant": _r(0x1FB00, 0x1FB3B),
    "octant": _r(0x1CD00, 0x1CDE5),
    "eighth": _r(0x1FB70, 0x1FB92) + _r(0x1FB94, 0x1FB97) + [0x1FBCE, 0x1FBCF],
    "separated": _r(0x1CC21, 0x1CC2F) + _r(0x1CE51, 0x1CE8F),
    "sixteenth": _r(0x1CE90, 0x1CEAF),
    "misc": [0x1FB98, 0x1FB99, 0x1FBAF, 0x1FBBD, 0x1FBBE, 0x1FBBF] + _r(0x1CC1B, 0x1CC1E) + _r(0x1CC30, 0x1CC3F)
            + [0x1CE00, 0x1CE01, 0x1CE0B, 0x1CE0C] + _r(0x1CE16, 0x1CE19),
    "wedge": _r(0x1FB3C, 0x1FB6F) + _r(0x1FB9A, 0x1FB9F),
    "diagonal": _r(0x1FBA0, 0x1FBAE) + _r(0x1FBD0, 0x1FBDF),
    "circle": _r(0x1FBE0, 0x1FBEF),
}
AA = [0x1FB98, 0x1FB99, 0x1FBBD, 0x1FBBE, 0x1FBBF, 0x1CE00, 0x1CE01, 0x1CE0B, 0x1CE0C, *_r(0x1CC30, 0x1CC3F)]
SHADE = sorted(SHADED) + [0x1FB90] + sorted(HALF_OVER_MEDIUM)
ALL = sorted(cp for g in GROUPS.values() for cp in g)
