"""Braille and the axis-aligned legacy computing cells.

 1  axis-aligned glyphs (not the wedge, diagonal and circle groups) are only 0/255 (shades 0/0x80/255), ink stays
    in the cell, assigned glyphs are not empty
 6  braille = OR of the single-dot glyphs; dots are equal solid squares; gaps (inside and across cells) differ <= 1
 7  sextant/octant masks, eighth strips, sixteenths and edge blocks: complementary pieces OR = full, AND = empty
    except on tie lines (k * size / n ends in .5 and both sides round up)
 8  octant masks are the 230 UnicodeData octant names and the 26 missing masks are the glyphs named elsewhere;
    sextant, separated block and braille bits match their names
"""
from .. import braille, legacy
from ..canvas import Canvas
from ..common import block, unicode_names
from ..registry import render

MEDIUM = 0x80
# standard braille cell: dots 1-3 and 7 down the left column, 4-6 and 8 down the right
LAYOUT = ((0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (1, 2), (0, 3), (1, 3))
CURVED = {cp for k in ("wedge", "diagonal", "circle") for cp in legacy.GROUPS.get(k, ())}
# octant masks without a 1CD00-1CDE5 code point and the code point that already draws each
OCTANT_ELSEWHERE = {
    0x00: (0x0020, "SPACE"), 0x01: (0x1CEA8, "LEFT HALF UPPER ONE QUARTER BLOCK"),
    0x02: (0x1CEAB, "RIGHT HALF UPPER ONE QUARTER BLOCK"), 0x03: (0x1FB82, "UPPER ONE QUARTER BLOCK"),
    0x05: (0x2598, "QUADRANT UPPER LEFT"), 0x0A: (0x259D, "QUADRANT UPPER RIGHT"), 0x0F: (0x2580, "UPPER HALF BLOCK"),
    0x14: (0x1FBE6, "MIDDLE LEFT ONE QUARTER BLOCK"), 0x28: (0x1FBE7, "MIDDLE RIGHT ONE QUARTER BLOCK"),
    0x3F: (0x1FB85, "UPPER THREE QUARTERS BLOCK"), 0x40: (0x1CEA3, "LEFT HALF LOWER ONE QUARTER BLOCK"),
    0x50: (0x2596, "QUADRANT LOWER LEFT"), 0x55: (0x258C, "LEFT HALF BLOCK"),
    0x5A: (0x259E, "QUADRANT UPPER RIGHT AND LOWER LEFT"),
    0x5F: (0x259B, "QUADRANT UPPER LEFT AND UPPER RIGHT AND LOWER LEFT"),
    0x80: (0x1CEA0, "RIGHT HALF LOWER ONE QUARTER BLOCK"), 0xA0: (0x2597, "QUADRANT LOWER RIGHT"),
    0xA5: (0x259A, "QUADRANT UPPER LEFT AND LOWER RIGHT"), 0xAA: (0x2590, "RIGHT HALF BLOCK"),
    0xAF: (0x259C, "QUADRANT UPPER LEFT AND UPPER RIGHT AND LOWER RIGHT"), 0xC0: (0x2582, "LOWER ONE QUARTER BLOCK"),
    0xF0: (0x2584, "LOWER HALF BLOCK"), 0xF5: (0x2599, "QUADRANT UPPER LEFT AND LOWER LEFT AND LOWER RIGHT"),
    0xFA: (0x259F, "QUADRANT UPPER RIGHT AND LOWER LEFT AND LOWER RIGHT"), 0xFC: (0x2586, "LOWER THREE QUARTERS BLOCK"),
    0xFF: (0x2588, "FULL BLOCK"),
}


def bits(name, prefix):
    return sum(1 << (int(d) - 1) for d in name[len(prefix):])


def ties(size, n=8):
    """Pixels where a k/n line falls on .5: both sides round up and share it."""
    return {k * size // n for k in range(1, n) if 2 * k * size % (2 * n) == n}


def union(pieces, size):
    out = bytearray(size)
    for p in pieces:
        out = bytearray(max(a, b) for a, b in zip(out, p))
    return bytes(out)


class Ctx:
    def __init__(self, m):
        self.m, self.w, self.h = m, m.cell_width, m.cell_height
        self.tx, self.ty = ties(self.w), ties(self.h)
        self.out = []

    def bad(self, tag, msg):
        self.out.append(f"cells {self.m.tag} {tag} {msg}")

    def draw(self, fn, *args):
        c = Canvas(self.w, self.h)
        fn(c, self.m, *args)
        return c.cell_bytes()

    def glyph(self, cp):
        return render(cp, self.m).cell_bytes()

    def pair(self, tag, what, a, b):
        """OR = full; AND only on tie lines."""
        if any(max(x, y) != 255 for x, y in zip(a, b)):
            self.bad(tag, f"{what}: OR is not the full cell")
        loose = [i for i, (x, y) in enumerate(zip(a, b))
                 if x and y and i % self.w not in self.tx and i // self.w not in self.ty]
        if loose:
            self.bad(tag, f"{what}: AND has {len(loose)} px")


def check_axis(x):
    for cp in [*range(0x2800, 0x2900), *(cp for cp in legacy.ALL if cp not in CURVED)]:
        c = render(cp, x.m)
        cell = c.cell_bytes()
        if sum(c.padded_bytes()) != sum(cell):
            x.bad("1", f"U+{cp:04X} ink outside the cell")
        if cp in legacy.AA:
            continue
        allowed = {0, 255, MEDIUM} if cp in legacy.SHADE else {0, 255}
        if set(cell) - allowed:
            x.bad("1", f"U+{cp:04X} values {sorted(set(cell) - allowed)}")
        if not any(cell) and cp != 0x2800:
            x.bad("1", f"U+{cp:04X} empty")


def check_braille(x):
    w, xs, ys = braille.geometry(x.w, x.h)
    if w < 1:
        x.bad("6", "dot size 0")
        return
    dots = [x.glyph(0x2800 + (1 << b)) for b in range(8)]
    for b, (col, row) in enumerate(LAYOUT):
        ink = [i for i, v in enumerate(dots[b]) if v]
        want = [(ys[row] + j) * x.w + xs[col] + i for j in range(w) for i in range(w)]
        if ink != want or any(dots[b][i] != 255 for i in ink):
            x.bad("6", f"dot {b + 1} is not a solid {w}x{w} square at ({xs[col]}, {ys[row]})")
    for cp in range(0x2800, 0x2900):
        want = union((dots[b] for b in range(8) if (cp - 0x2800) >> b & 1), x.w * x.h)
        if x.glyph(cp) != want:
            x.bad("6", f"U+{cp:04X} is not the OR of its dots")
    for axis, g in (("x", braille.gaps(x.w, w, xs)), ("y", braille.gaps(x.h, w, ys))):
        if max(g) - min(g) > 1 or min(g) < 1:
            x.bad("6", f"{axis} gaps {g} (inside, across cells) differ by more than 1 or touch")
    if abs(xs[0] - (x.w - xs[1] - w)) > 1 or abs(ys[0] - (x.h - ys[3] - w)) > 1:
        x.bad("6", f"dots off centre: x margins {xs[0]}/{x.w - xs[1] - w}, y margins {ys[0]}/{x.h - ys[3] - w}")


def check_tiling(x):
    size = x.w * x.h
    for mask in range(64):
        x.pair("7", f"sextant {mask:02X}|{63 ^ mask:02X}", x.draw(legacy.draw_sextants, mask),
               x.draw(legacy.draw_sextants, 63 ^ mask))
    for cp in legacy.GROUPS["sextant"]:
        if x.glyph(cp) != x.draw(legacy.draw_sextants, legacy.sextant_mask(cp)):
            x.bad("7", f"U+{cp:04X} differs from its sextant mask")
    for mask in range(256):
        x.pair("7", f"octant {mask:02X}|{255 ^ mask:02X}", x.draw(legacy.draw_octants, mask),
               x.draw(legacy.draw_octants, 255 ^ mask))
    for name, fn in (("vertical", legacy.vstrip), ("horizontal", legacy.hstrip)):
        strips = [x.draw(fn, n) for n in range(8)]
        for n, s in enumerate(strips):
            if not any(s):
                x.bad("7", f"{name} eighth {n + 1} empty")
            x.pair("7", f"{name} eighth {n + 1} vs others", s, union((t for k, t in enumerate(strips) if k != n), size))
        for e in range(1, 8):
            for args, ks in ((("l", "m", e / 8, 1) if name == "vertical" else ("c", "t", 1, e / 8), range(0, e)),
                             (("r", "m", e / 8, 1) if name == "vertical" else ("c", "b", 1, e / 8), range(8 - e, 8))):
                if x.draw(block, *args) != union((strips[k] for k in ks), size):
                    x.bad("7", f"block {args} differs from the union of {name} eighths {list(ks)}")
    flip = {"t": ("c", "b"), "b": ("c", "t"), "l": ("r", "m"), "r": ("l", "m")}
    for cp, parts in legacy.EIGHTH_BLOCKS.items():
        if len(parts) != 1:
            continue
        h, v, wf, hf = parts[0]
        partner = (*flip[v], wf, 1 - hf) if h == "c" else (*flip[h], 1 - wf, hf)
        x.pair("7", f"U+{cp:04X} vs block {partner}", x.glyph(cp), x.draw(block, *partner))
    six = [x.glyph(cp) for cp in range(0x1CE90, 0x1CEA0)]
    for i, s in enumerate(six):
        rest = union((t for k, t in enumerate(six) if k != i), size)
        x.pair("7", f"sixteenth U+{0x1CE90 + i:04X} vs others", s, rest)
    for cp in range(0x1CEA0, 0x1CEB0):
        x0, x1, y0, y1 = legacy.SIXTEENTHS[cp]
        if x.glyph(cp) != union((six[r * 4 + q] for r in range(y0, y1) for q in range(x0, x1)), size):
            x.bad("7", f"U+{cp:04X} is not the union of its sixteenths")
    for name, rects in (("quadrant", legacy.separated_quadrant_rects(x.w, x.h)),
                        ("sextant", legacy.separated_sextant_rects(x.w, x.h))):
        for i, (a0, b0, a1, b1) in enumerate(rects):
            if a1 <= a0 or b1 <= b0 or a0 < 1 or b0 < 1 or a1 > x.w - 1 or b1 > x.h - 1:
                x.bad("7", f"separated {name} block {i} {a0, b0, a1, b1} empty or touching the cell edge")
            for j, (c0, d0, c1, d1) in enumerate(rects[:i]):
                if a0 <= c1 and c0 <= a1 and b0 <= d1 and d0 <= b1:
                    x.bad("7", f"separated {name} blocks {j} and {i} touch")


def check_names():
    out = []
    names = unicode_names()
    octants = range(0x1CD00, 0x1CDE6)
    if any(not names.get(cp, "").startswith("BLOCK OCTANT-") for cp in octants) or \
            names.get(0x1CDE6, "").startswith("BLOCK OCTANT-"):
        out.append("cells 8 octant names are not exactly 1CD00-1CDE5")
        return out
    masks = [legacy.octant_mask(cp) for cp in octants]
    if [bits(names[cp], "BLOCK OCTANT-") for cp in octants] != masks:
        out.append("cells 8 octant masks differ from their names")
    if len(set(masks)) != 230 or masks != sorted(masks):
        out.append("cells 8 octant masks repeat or are not ascending")
    missing = set(range(256)) - set(masks)
    if missing != set(OCTANT_ELSEWHERE):
        out.append(f"cells 8 missing octant masks {sorted(missing ^ set(OCTANT_ELSEWHERE))} not in the elsewhere table")
    for mask, (cp, name) in OCTANT_ELSEWHERE.items():
        if names.get(cp) != name:
            out.append(f"cells 8 U+{cp:04X} is {names.get(cp)!r}, not {name!r}")
    for lo, hi, prefix, value in ((0x1FB00, 0x1FB3B, "BLOCK SEXTANT-", legacy.sextant_mask),
                                  (0x1CE51, 0x1CE8F, "SEPARATED BLOCK SEXTANT-", lambda cp: cp - 0x1CE50),
                                  (0x1CC21, 0x1CC2F, "SEPARATED BLOCK QUADRANT-", lambda cp: cp - 0x1CC20),
                                  (0x2801, 0x28FF, "BRAILLE PATTERN DOTS-", lambda cp: cp - 0x2800)):
        for cp in range(lo, hi + 1):
            n = names.get(cp, "")
            if not n.startswith(prefix) or bits(n, prefix) != value(cp):
                out.append(f"cells 8 U+{cp:04X} {n!r} bits != {value(cp):#x}")
    return out


def check(m):
    x = Ctx(m)
    check_axis(x)
    check_braille(x)
    check_tiling(x)
    return x.out
