"""Branch drawing (commit graph) U+F5D0-F60D."""
import math

from . import aa
from .box import BL, BR, TL, TR, arc
from .common import LIGHT, box, hline_middle, stroke_px, vline_middle, zround
from .registry import draws

U, R, D, L, F = 1, 2, 4, 8, 16
# F5EE..F60D: arms (up, right, down, left) and filled circle
NODES = (F, 0, R | F, R, L | F, L, L | R | F, L | R, D | F, D, U | F, U, U | D | F, U | D, R | D | F, R | D, L | D | F,
         L | D, U | R | F, U | R, U | L | F, U | L, U | D | R | F, U | D | R, U | D | L | F, U | D | L, D | L | R | F,
         D | L | R, U | L | R | F, U | L | R, U | D | L | R | F, U | D | L | R)
# F5DA..F5ED: straight line ("v", "h" or None), then the rounded corners
COMBOS = (("v", TR), ("v", BR), (None, TR, BR), ("v", TL), ("v", BL), (None, TL, BL),
          ("h", BL), ("h", BR), (None, BR, BL), ("h", TL), ("h", TR), (None, TR, TL),
          ("v", TL, TR), ("v", BL, BR), ("h", BL, TL), ("h", TR, BR),
          ("v", TL, BR), ("v", TR, BL), ("h", TL, BR), ("h", TR, BL))


def fading_line(c, m, to):
    """Light middle line fading out towards the edge `to`."""
    t = stroke_px(m, LIGHT)
    w, h = m.cell_width, m.cell_height
    ht, vl = max(0, h - t) // 2, max(0, w - t) // 2
    color = 0.0 if to in ("top", "left") else 255.0
    inc = 255.0 / {"top": h, "bottom": -h, "left": w, "right": -w}[to]
    if to in ("top", "bottom"):
        for y in range(h):
            for x in range(vl, vl + t):
                c.pixel(x, y, int(zround(color)) & 0xFF)
            color += inc
    else:
        for x in range(w):
            for y in range(ht, ht + t):
                c.pixel(x, y, int(zround(color)) & 0xFF)
            color += inc


def circle(c, x, y, r):
    p = aa.Path(c)
    p.arc(x, y, r, 0, 2 * math.pi)
    p.close()
    return p


def node(c, m, arms):
    t = stroke_px(m, LIGHT)
    w, h = m.cell_width, m.cell_height
    ht, vl = max(0, h - t) // 2, max(0, w - t) // 2
    cx, cy = vl + t / 2, ht + t / 2
    r = min(cx, cy, w - cx, h - cy)
    if arms & U:
        box(c, vl, 0, vl + t, math.ceil(cy - r + t / 2))
    if arms & R:
        box(c, math.floor(cx + r - t / 2), ht, w, ht + t)
    if arms & D:
        box(c, vl, math.floor(cy + r - t / 2), vl + t, h)
    if arms & L:
        box(c, 0, ht, math.ceil(cx - r + t / 2), ht + t)
    if arms & F:
        aa.fill(c, circle(c, cx, cy, r))
    else:
        aa.stroke(c, circle(c, cx, cy, r - t / 2), t)


@draws(0xF5D0, 0xF60D)
def branch(cp, c, m):
    if cp == 0xF5D0:
        hline_middle(c, m, LIGHT)
    elif cp == 0xF5D1:
        vline_middle(c, m, LIGHT)
    elif cp <= 0xF5D5:
        fading_line(c, m, ("right", "left", "bottom", "top")[cp - 0xF5D2])
    elif cp <= 0xF5D9:
        arc(c, m, (BR, BL, TR, TL)[cp - 0xF5D6])
    elif cp <= 0xF5ED:
        line, *corners = COMBOS[cp - 0xF5DA]
        if line == "v":
            vline_middle(c, m, LIGHT)
        # The line overwrites the arcs' edge pixels: drawn first from F5E8 on, after the arcs before that.
        late = line == "h" and cp < 0xF5E8
        if line == "h" and not late:
            hline_middle(c, m, LIGHT)
        for k in corners:
            arc(c, m, k)
        if late:
            hline_middle(c, m, LIGHT)
    else:
        node(c, m, NODES[cp - 0xF5EE])
