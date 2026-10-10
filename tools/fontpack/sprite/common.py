import functools
import math

NONE, LIGHT, HEAVY, DOUBLE = 0, 1, 2, 3
UNICODE_DATA = None


def zround(x):
    """Zig @round: halves away from zero (Python's round() goes to even)."""
    return math.floor(x + 0.5) if x >= 0 else -math.floor(-x + 0.5)


def frac_min(f, size):
    """Start edge of a fraction line, size - round((1 - f) * size): both halves of an odd size get the same width."""
    return int(size - zround((1.0 - f) * size))


def frac_max(f, size):
    return int(zround(f * size))


def stroke_px(m, style=LIGHT):
    return m.box_thickness * 2 if style == HEAVY else m.box_thickness


def box(c, x0, y0, x1, y1, a=255):
    """Filled rectangle between two corners in any order."""
    c.rect(min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1), a)


def hline_middle(c, m, style=LIGHT):
    t = stroke_px(m, style)
    y = max(0, m.cell_height - t) // 2
    c.rect(0, y, m.cell_width, y + t)


def vline_middle(c, m, style=LIGHT):
    t = stroke_px(m, style)
    x = max(0, m.cell_width - t) // 2
    c.rect(x, 0, x + t, m.cell_height)


def fill(c, m, x0, x1, y0, y1, a=255):
    """The cell area between fraction lines (0..1)."""
    c.rect(frac_min(x0, m.cell_width), frac_min(y0, m.cell_height),
           frac_max(x1, m.cell_width), frac_max(y1, m.cell_height), a)


def block(c, m, h, v, wf, hf, a=255):
    """round(w * wf) x round(h * hf) px aligned left/centre/right (h: l/c/r) and top/middle/bottom (v: t/m/b)."""
    cw, ch = m.cell_width, m.cell_height
    w, hh = zround(cw * wf), zround(ch * hf)
    x = {"l": 0, "r": cw - w, "c": (cw - w) // 2}[h]
    y = {"t": 0, "b": ch - hh, "m": (ch - hh) // 2}[v]
    c.rect(x, y, x + w, y + hh, a)


def clip_to_cell(c):
    c.clip_left = c.clip_right = c.pad_x
    c.clip_top = c.clip_bottom = c.pad_y


@functools.cache
def unicode_names():
    """{scalar: name} of UnicodeData.txt (sprite.load sets the path; Python's unicodedata may be older)."""
    out = {}
    for line in UNICODE_DATA.read_text(encoding="utf-8").splitlines():
        f = line.split(";")
        out[int(f[0], 16)] = f[1]
    return out
