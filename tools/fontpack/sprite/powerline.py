"""Powerline U+E0B0-E0BF, E0D2 and E0D4."""
import math

from . import aa
from .box import diagonal
from .common import LIGHT, stroke_px
from .registry import draws

ARC_K = (math.sqrt(2) - 1.0) * 4.0 / 3.0


def _round(c, m, outline):
    w, h = m.cell_width, m.cell_height
    r = min(w, h / 2)
    p = aa.Path(c)
    p.move_to(0, 0)
    if outline:
        p.line_to(1, 0)
    p.curve_to(r * ARC_K, 0, r, r - r * ARC_K, r, r)
    p.line_to(r, h - r)
    p.curve_to(r, h - r + r * ARC_K, r * ARC_K, h, 1 if outline else 0, h)
    if outline:
        p.line_to(0, h)
        aa.inner_stroke(c, p, m.box_thickness)
    else:
        p.close()
        aa.fill(c, p)


def _chevron(c, m):
    w, h = m.cell_width, m.cell_height
    p = aa.Path(c)
    p.move_to(0, 0)
    p.line_to(w, h / 2)
    p.line_to(0, h)
    aa.stroke(c, p, stroke_px(m, LIGHT))


@draws(0xE0B0, 0xE0BF)
def powerline(cp, c, m):
    w, h = m.cell_width, m.cell_height
    if cp == 0xE0B0:
        aa.polygon(c, [(0, 0), (w, h / 2), (0, h)])
    elif cp == 0xE0B2:
        aa.polygon(c, [(w, 0), (0, h / 2), (w, h)])
    elif cp in (0xE0B1, 0xE0B3):
        _chevron(c, m)
    elif cp in (0xE0B4, 0xE0B6):
        _round(c, m, False)
    elif cp in (0xE0B5, 0xE0B7):
        _round(c, m, True)
    elif cp == 0xE0B8:
        aa.polygon(c, [(0, 0), (w, h), (0, h)])
    elif cp == 0xE0BA:
        aa.polygon(c, [(w, 0), (w, h), (0, h)])
    elif cp == 0xE0BC:
        aa.polygon(c, [(0, 0), (w, 0), (0, h)])
    elif cp == 0xE0BE:
        aa.polygon(c, [(0, 0), (w, 0), (w, h)])
    else:
        diagonal(c, m, cp in (0xE0BB, 0xE0BD))
    if cp in (0xE0B3, 0xE0B6, 0xE0B7):
        c.flip_h()


@draws(0xE0D2)
@draws(0xE0D4)
def flame(cp, c, m):
    w, h, t = m.cell_width, m.cell_height, m.box_thickness
    aa.polygon(c, [(0, 0), (w, 0), (w / 2, h / 2 - t / 2), (0, h / 2 - t / 2)])
    aa.polygon(c, [(0, h), (w, h), (w / 2, h / 2 + t / 2), (0, h / 2 + t / 2)])
    if cp == 0xE0D4:
        c.flip_h()
