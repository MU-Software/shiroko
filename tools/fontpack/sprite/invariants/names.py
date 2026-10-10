"""Legacy computing glyphs against the shapes their Unicode names describe (not the drawers' tables).

 N1  blocks, strips, sixteenths, shades and line pieces: pixels inside the named rectangles have their level, pixels
     outside are empty; a pixel that a rectangle edge cuts may be either
 N2  separated blocks: one solid piece in each named cell of the 2x2 or 2x3 grid, nothing elsewhere, off the cell edge
 N3  diagonal and triangular blocks, black circles: full more than 1 px inside the named shape (1.5 px for the edge
     triangles, whose apex is rounded to a pixel), empty more than that outside; diagonal lines and white circles and
     ellipses: empty more than 1.5 px off the named path, ink along all of it
 N4  NEGATIVE glyphs invert the light glyph of the same name; 1FBAF is 2500 with the heavy 2503 over it; 1FB98/1FB99
     have more ink on the named diagonal than on the other unless they are symmetric; 1FB95 starts its row of four
     squares filled, 1FB96 is its inverse, 1FB97 fills the second and fourth quarters (as kitty draws them)
 N5  every legacy glyph has one of these expectations
"""
import math
import re
from fractions import Fraction as Fr

from .. import legacy
from ..common import unicode_names
from ..registry import render

MEDIUM = 0x80
SIDES = "(LEFT|RIGHT|UPPER|LOWER)"
FRAC = {"ONE EIGHTH": Fr(1, 8), "ONE QUARTER": Fr(1, 4), "THREE EIGHTHS": Fr(3, 8), "HALF": Fr(1, 2),
        "FIVE EIGHTHS": Fr(5, 8), "THREE QUARTERS": Fr(3, 4), "SEVEN EIGHTHS": Fr(7, 8), "ONE THIRD": Fr(1, 3),
        "TWO THIRDS": Fr(2, 3)}
ROW4 = {"UPPER": 0, "UPPER MIDDLE": 1, "LOWER MIDDLE": 2, "LOWER": 3}
COL4 = {"LEFT": 0, "CENTRE LEFT": 1, "CENTRE RIGHT": 2, "RIGHT": 3}
# MIDDLE alone is the half; with UPPER or LOWER it is a third (the sextant rows)
PY = {"UPPER": 0, "UPPER MIDDLE": Fr(1, 3), "MIDDLE": Fr(1, 2), "LOWER MIDDLE": Fr(2, 3), "LOWER": 1}
PX = {"LEFT": 0, "CENTRE": Fr(1, 2), "RIGHT": 1}
QUARTER_BLOCK = {"LEFT": (0, Fr(1, 2)), "CENTRE": (Fr(1, 4), Fr(3, 4)), "RIGHT": (Fr(1, 2), 1),
                 "UPPER": (0, Fr(1, 2)), "MIDDLE": (Fr(1, 4), Fr(3, 4)), "LOWER": (Fr(1, 2), 1)}
EDGE_CENTRE = {"TOP": (Fr(1, 2), 0), "BOTTOM": (Fr(1, 2), 1), "LEFT": (0, Fr(1, 2)), "RIGHT": (1, Fr(1, 2))}


def strip(side, f):
    return {"LEFT": (0, f, 0, 1), "RIGHT": (1 - f, 1, 0, 1), "UPPER": (0, 1, 0, f), "LOWER": (0, 1, 1 - f, 1)}[side]


def grid(cols, rows, cells):
    return [(Fr(i % cols, cols), Fr(i % cols + 1, cols), Fr(i // cols, rows), Fr(i // cols + 1, rows)) for i in cells]


def digits(s):
    return [int(d) - 1 for d in s]


def place4(s):
    """(column, row) of "UPPER MIDDLE CENTRE LEFT" and the like in a 4 x 4 grid."""
    for v in ("UPPER MIDDLE", "LOWER MIDDLE", "UPPER", "LOWER"):
        if s.startswith(v + " ") and s[len(v) + 1:] in COL4:
            return COL4[s[len(v) + 1:]], ROW4[v]
    return None


def point(s):
    *v, hz = s.split()
    return PX[hz], PY[" ".join(v)]


def halfplane(a, b, inside):
    """Signed distance to the line ab, negative on the side of `inside`."""
    nx, ny = b[1] - a[1], a[0] - b[0]
    k = (-1 if nx * (inside[0] - a[0]) + ny * (inside[1] - a[1]) > 0 else 1) / math.hypot(nx, ny)
    return lambda x, y: k * (nx * (x - a[0]) + ny * (y - a[1]))


def convex(pts, w, h):
    """Signed distance to a convex polygon in the cell; edges on the cell border bound nothing."""
    c = (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))
    sides = [halfplane(a, b, c) for a, b in zip(pts, pts[1:] + pts[:1])
             if not (a[0] == b[0] in (0, w) or a[1] == b[1] in (0, h))]
    return lambda x, y: max(f(x, y) for f in sides)


def segment(a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]

    def d(x, y):
        u = max(0, min(1, ((x - a[0]) * dx + (y - a[1]) * dy) / (dx * dx + dy * dy)))
        return math.hypot(x - a[0] - u * dx, y - a[1] - u * dy)
    return d


def ellipse(cx, cy, a, b):
    """Approximate signed distance to the ellipse outline (F / |grad F|)."""
    def d(x, y):
        u, v = (x - cx) / a, (y - cy) / b
        g = 2 * math.hypot(u / a, v / b)
        return (u * u + v * v - 1) / g if g else -max(a, b)
    return d


def ring(ellipses, t):
    """Outlines of width t inside the ellipses (cx, cy, rx, ry)."""
    fs = [ellipse(cx, cy, rx - t / 2, ry - t / 2) for cx, cy, rx, ry in ellipses]
    trace = [(cx + (rx - t / 2) * math.cos(i * math.pi / 64), cy + (ry - t / 2) * math.sin(i * math.pi / 64))
             for cx, cy, rx, ry in ellipses for i in range(128)]
    return "shape", lambda x, y: min(abs(f(x, y)) for f in fs) - t / 2, 1.5, None, trace


def expect(cp, x):
    """What the name of cp says the glyph is: (kind, ...) in pixels, or None."""
    n, w, h, t = unicode_names()[cp], x.w, x.h, x.t

    def px(p):
        return p[0] * w, p[1] * h

    def rects(rs):
        return "rects", [(x0 * w, x1 * w, y0 * h, y1 * h, 255) for x0, x1, y0, y1 in rs]
    if m := re.fullmatch(r"BLOCK (SEXTANT|OCTANT)-(\d+)", n):
        return rects(grid(2, 3 if m[1] == "SEXTANT" else 4, digits(m[2])))
    if m := re.fullmatch(r"(VERTICAL|HORIZONTAL) ONE EIGHTH BLOCK-(\d+)", n):
        return rects(grid(8, 1, digits(m[2])) if m[1] == "VERTICAL" else grid(1, 8, digits(m[2])))
    if m := re.fullmatch(r"SEPARATED BLOCK (QUADRANT|SEXTANT)-(\d+)", n):
        return "separated", 2 if m[1] == "QUADRANT" else 3, digits(m[2])
    if (m := re.fullmatch(rf"((?:{SIDES} AND )*{SIDES}) (.+) BLOCK", n)) and m[4] in FRAC:
        return rects([strip(s, FRAC[m[4]]) for s in m[1].split(" AND ")])
    if m := re.fullmatch(rf"{SIDES} (HALF|THREE QUARTERS) {SIDES} ONE QUARTER BLOCK", n):
        a, b = strip(m[3], Fr(1, 4)), strip(m[1], FRAC[m[2]])
        return rects([(max(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), min(a[3], b[3]))])
    if m := re.fullmatch(r"(UPPER|MIDDLE|LOWER) (LEFT|CENTRE|RIGHT) ONE QUARTER BLOCK", n):
        return rects([QUARTER_BLOCK[m[2]] + QUARTER_BLOCK[m[1]]])
    if n.endswith(" ONE SIXTEENTH BLOCK") and (p := place4(n.removesuffix(" ONE SIXTEENTH BLOCK"))):
        return rects(grid(4, 4, [p[1] * 4 + p[0]]))
    clauses = [re.fullmatch(rf"(?:{SIDES} HALF )?(BLOCK|(?:INVERSE )?MEDIUM SHADE)", c) for c in n.split(" AND ")]
    if all(clauses):
        return "rects", [(x0 * w, x1 * w, y0 * h, y1 * h, 255 if c[2] == "BLOCK" else MEDIUM)
                         for c in clauses for x0, x1, y0, y1 in [strip(c[1], Fr(1, 2)) if c[1] else (0, 1, 0, 1)]]
    if n == "HEAVY HORIZONTAL FILL":
        return rects(grid(1, 4, (1, 3)))
    if m := re.fullmatch(r"BOX DRAWINGS LIGHT DIAGONAL (.+)", n):
        paths = ["UPPER CENTRE TO MIDDLE RIGHT TO LOWER CENTRE TO MIDDLE LEFT TO UPPER CENTRE"] \
            if m[1] == "DIAMOND" else m[1].split(" AND ")
        lines = [[px(point(p)) for p in path.split(" TO ")] for path in paths]
        segs = [segment(a, b) for ps in lines for a, b in zip(ps, ps[1:])]
        trace = [(a[0] + (b[0] - a[0]) * i / 16, a[1] + (b[1] - a[1]) * i / 16)
                 for ps in lines for a, b in zip(ps, ps[1:]) for i in range(17)]
        return "shape", lambda x, y: min(s(x, y) for s in segs) - t / 2, 1.5, None, trace
    if n == "BOX DRAWINGS LIGHT HORIZONTAL WITH VERTICAL STROKE":
        return "same", bytes(map(max, x.glyph(0x2500), x.glyph(0x2503)))
    if m := re.fullmatch(r"BOX DRAWINGS LIGHT (.+)", n):
        out = []
        for c in m[1].split(" AND "):
            if c in ("VERTICAL", "HORIZONTAL"):
                out.append((*x.vline, 0, h) if c == "VERTICAL" else (0, w, *x.hline))
            elif e := re.fullmatch(r"(TOP|BOTTOM)(?: (LEFT|RIGHT))?", c):
                xs = {None: (0, w), "LEFT": (0, Fr(w, 2)), "RIGHT": (Fr(w, 2), w)}[e[2]]
                out.append((*xs, *((0, t) if e[1] == "TOP" else (h - t, h))))
            elif e := re.fullmatch(r"(UPPER|LOWER) (LEFT|RIGHT)", c):
                ys = (0, Fr(h, 2)) if e[1] == "UPPER" else (Fr(h, 2), h)
                out.append(((0, t) if e[2] == "LEFT" else (w - t, w)) + ys)
            else:
                return None
        return "rects", [(*r, 255) for r in out]
    if m := re.fullmatch(r"(UPPER|LOWER) (LEFT|RIGHT) BLOCK DIAGONAL (.+) TO (.+)", n):
        return "shape", halfplane(px(point(m[3])), px(point(m[4])), px(point(f"{m[1]} {m[2]}"))), 1, 255, []
    if m := re.fullmatch(r"(.+) TRIANGULAR (?:ONE QUARTER BLOCK|HALF BLOCK|THREE QUARTERS BLOCK|MEDIUM SHADE)", n):
        if n.endswith("SHADE"):
            cx, cy = point(m[1])
            return "shape", convex([px((cx, cy)), px((1 - cx, cy)), px((cx, 1 - cy))], w, h), 1, MEDIUM, []
        ends = {"LEFT": ((0, 0), (0, 1)), "UPPER": ((0, 0), (1, 0)), "RIGHT": ((1, 0), (1, 1)),
                "LOWER": ((0, 1), (1, 1))}
        tris = [convex([px(a), px(b), (w / 2, h / 2)], w, h) for a, b in (ends[e] for e in m[1].split(" AND "))]
        return "shape", lambda x, y: min(f(x, y) for f in tris), 1.5, 255, []  # the apex is rounded to a pixel
    if m := re.fullmatch(r"(UPPER LEFT|UPPER RIGHT) TO (LOWER RIGHT|LOWER LEFT) FILL", n):
        return "fill", px(point(m[1])), px(point(m[2]))
    if m := re.fullmatch(r"NEGATIVE (DIAGONAL .+)", n):
        return "same", bytes(255 - v for v in x.glyph(x.named[f"BOX DRAWINGS LIGHT {m[1]}"]))
    if n == "CHECKER BOARD FILL":
        return "checker",
    if n == "INVERSE CHECKER BOARD FILL":
        return "same", bytes(255 - v for v in x.glyph(x.named["CHECKER BOARD FILL"]))
    r = min(w, h) / 2
    if m := re.fullmatch(r"(TOP|BOTTOM|LEFT|RIGHT)(?: (LEFT|RIGHT))? JUSTIFIED .+ (WHITE|BLACK) CIRCLE", n):
        cx, cy = px(EDGE_CENTRE[m[1]])
        if m[2]:
            cx = px(EDGE_CENTRE[m[2]])[0]
        return ring([(cx, cy, r, r)], t) if m[3] == "WHITE" else \
            ("shape", lambda x, y: math.hypot(x - cx, y - cy) - r, 1, 255, [])
    if m := re.fullmatch(r"(\w+) HALF AND (\w+) HALF WHITE CIRCLE", n):
        opposite = {"LEFT": "RIGHT", "RIGHT": "LEFT", "UPPER": "BOTTOM", "LOWER": "TOP"}
        return ring([(*px(EDGE_CENTRE[opposite[s]]), r, r) for s in (m[1], m[2])], t)
    if m := re.fullmatch(r"(LEFT|RIGHT) HALF WHITE ELLIPSE", n):
        return ring([(w if m[1] == "LEFT" else 0, h / 2, w, h / 2)], t)
    if (m := re.fullmatch(r"(.+) (TWELFTH|QUARTER) CIRCLE", n)) and (p := place4(m[1])):
        k = 2 if m[2] == "TWELFTH" else 1
        col, row = p if k == 2 else (p[0] // 2, p[1] // 2)
        return ring([(k * w - col * w, k * h - row * h, k * w, k * h)], t)
    return None


class Ctx:
    def __init__(self, m):
        self.m, self.w, self.h = m, m.cell_width, m.cell_height
        self.named = {v: k for k, v in unicode_names().items()}
        self.out = []
        v, hz = self.glyph(0x2502), self.glyph(0x2500)
        cols = [i for i in range(self.w) if v[i]]
        rows = [y for y in range(self.h) if hz[y * self.w]]
        self.vline, self.hline, self.t = (cols[0], cols[-1] + 1), (rows[0], rows[-1] + 1), len(cols)

    def bad(self, tag, msg):
        self.out.append(f"names {self.m.tag} {tag} {msg}")

    def glyph(self, cp):
        return render(cp, self.m).cell_bytes()


def check_rects(x, cp, g, rects):
    off = 0
    for i, v in enumerate(g):
        a, b = i % x.w, i // x.w
        levels = {lv for x0, x1, y0, y1, lv in rects if a + 1 > x0 and a < x1 and b + 1 > y0 and b < y1}
        if not any(x0 <= a and a + 1 <= x1 and y0 <= b and b + 1 <= y1 for x0, x1, y0, y1, _ in rects):
            levels.add(0)
        off += v not in levels
    if off:
        x.bad("N1", f"U+{cp:04X} {off} px off its named rectangles")


def pieces(g, w, h):
    """8-connected ink components."""
    ink = {(i % w, i // w) for i, v in enumerate(g) if v}
    out = []
    while ink:
        stack, part = [ink.pop()], set()
        while stack:
            p = stack.pop()
            part.add(p)
            for q in ((p[0] + dx, p[1] + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
                if q in ink:
                    ink.remove(q)
                    stack.append(q)
        out.append(part)
    return out


def check_separated(x, cp, g, rows, named):
    w, h = x.w, x.h
    cells = [(x0 * w, x1 * w, y0 * h, y1 * h) for x0, x1, y0, y1 in grid(2, rows, range(2 * rows))]
    parts = pieces(g, w, h)
    for part in parts:
        xs, ys = [p[0] for p in part], [p[1] for p in part]
        box = (min(xs), max(xs) + 1, min(ys), max(ys) + 1)
        inside = [i for i, (x0, x1, y0, y1) in enumerate(cells)
                  if x0 <= box[0] and box[1] <= x1 and y0 <= box[2] and box[3] <= y1]
        if len(part) != (box[1] - box[0]) * (box[3] - box[2]) or not inside or inside[0] not in named \
                or box[0] == 0 or box[2] == 0 or box[1] == w or box[3] == h:
            x.bad("N2", f"U+{cp:04X} piece {box} is not a solid block inside a named cell, off the cell edge")
    if len(parts) != len(named):
        x.bad("N2", f"U+{cp:04X} {len(parts)} pieces for {len(named)} named cells")


def check_shape(x, cp, g, sd, margin, level, trace):
    w, h = x.w, x.h
    off = sum(1 for i, v in enumerate(g)
              if (d := sd(i % w + 0.5, i // w + 0.5)) <= -margin and level not in (None, v) or d >= margin and v)
    if off:
        x.bad("N3", f"U+{cp:04X} {off} px off its named shape")
    gaps = [(a, b) for a, b in trace if 0 <= a < w and 0 <= b < h and
            not any(g[j * w + i] for i in range(int(a) - 1, int(a) + 2) for j in range(int(b) - 1, int(b) + 2)
                    if 0 <= i < w and 0 <= j < h)]
    if gaps:
        x.bad("N3", f"U+{cp:04X} no ink near its path at {gaps[0]}")


def check_fill(x, cp, g, a, b):
    w, h = x.w, x.h

    def ink(a, b):
        return sum(g[int(a[1] + (b[1] - a[1]) * f) * w + int(a[0] + (b[0] - a[0]) * f)]
                   for f in ((i + 0.5) / 64 for i in range(64)))
    mirror = bytes(g[i - i % w + w - 1 - i % w] for i in range(w * h))
    if ink(a, b) <= ink((w - a[0], a[1]), (w - b[0], b[1])) and g != mirror:  # a symmetric fill runs both ways
        x.bad("N4", f"U+{cp:04X} less ink along its named diagonal than along the other")


def check(m):
    x = Ctx(m)
    for cp in legacy.ALL:
        g = x.glyph(cp)
        e = expect(cp, x)
        if e is None:
            x.bad("N5", f"U+{cp:04X} {unicode_names()[cp]!r}: no expectation from the name")
        elif e[0] == "rects":
            check_rects(x, cp, g, e[1])
        elif e[0] == "separated":
            check_separated(x, cp, g, *e[1:])
        elif e[0] == "shape":
            check_shape(x, cp, g, *e[1:])
        elif e[0] == "same" and g != e[1]:
            x.bad("N4", f"U+{cp:04X} differs from the glyph its name derives from")
        elif e[0] == "checker" and (g[0] != 255 or sum(g[i] != g[i - 1] for i in range(1, x.w)) != 3):
            x.bad("N4", f"U+{cp:04X} top row is not four squares starting with a filled one")
        elif e[0] == "fill":
            check_fill(x, cp, g, *e[1:])
    return x.out
