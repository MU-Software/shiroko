"""Rounded corners and diagonals, branch drawing, Powerline and the anti-aliased legacy computing shapes.

 9ab F5D0 == 2500, F5D1 == 2502, F5D6..F5D9 == 256D, 256E, 2570, 256F
 9c  every arm/arc of a branch glyph and every rounded corner meets the cell edge with exactly the light line's edge
     profile; unused edges are empty
 9d  branch arms reach the node: no empty pixel on the arm axis between the edge and the circle
 C   ink of every line-like glyph is one 8-connected piece
 M   mirror pairs equal within one z2d sample row where the geometry is symmetric in the cell
 K   complement pairs (shapes sharing one diagonal) add up to a full cell
 P   Powerline: E0B0/E0B2 edge column full except the corner rows the triangle cannot fill, E0B4/E0B6 edge column
     full, triangle tips reach the opposite edge
 F   fade lines F5D2-F5D5: A4 levels fall away from the bright end, at least min(n, 15) levels
 A   axis-parallel glyphs use only 0/255
"""
import math

from .. import box as _box  # noqa: F401  (registers the drawers)
from .. import branch, legacy, powerline  # noqa: F401
from ..registry import render

# z2d rounds an edge through a sample centre half away from zero, so a shape and its mirror image can differ by one
# sample per sub-scanline and edge (4 samples = 64 per pixel).
MIRROR_TOL = 64
U, R, D, L = 1, 2, 4, 8
CORNER = {"br": D | R, "bl": D | L, "tl": U | L, "tr": U | R}
ARC_EDGES = {0x256D: D | R, 0x256E: D | L, 0x256F: U | L, 0x2570: U | R}


def branch_edges():
    out = {0xF5D0: L | R, 0xF5D1: U | D}
    for i, k in enumerate(("br", "bl", "tr", "tl")):
        out[0xF5D6 + i] = CORNER[k]
    for i, (line, *corners) in enumerate(branch.COMBOS):
        e = (U | D) if line == "v" else (L | R) if line == "h" else 0
        for k in corners:
            e |= CORNER[k]
        out[0xF5DA + i] = e
    for i, n in enumerate(branch.NODES):
        out[0xF5EE + i] = n & 15
    return out


def _inside(poly, x, y):
    wn = 0
    for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
        if (y0 <= y < y1 or y1 <= y < y0) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            wn += 1 if y1 > y0 else -1
    return wn != 0


def mosaic_pairs(m):
    """Mirror pairs from the patterns; complement pairs from the outlines (every probe point in exactly one)."""
    pats = {p: 0x1FB3C + i for i, p in enumerate(legacy.MOSAICS)}
    swap = str.maketrans({"/": "\\", "\\": "/"})
    mirror = []
    for p, cp in pats.items():
        mp = "|".join(row[::-1].translate(swap) for row in p.split("|"))
        if mp in pats and cp < pats[mp]:
            mirror.append((cp, pats[mp]))
    w, h = m.cell_width, m.cell_height
    probes = [((i + 0.37) * w / 23, (j + 0.61) * h / 29) for i in range(23) for j in range(29)]
    ins = {0x1FB3C + i: [_inside(legacy.mosaic_points(p, m), x, y) for x, y in probes]
           for i, p in enumerate(legacy.MOSAICS)}
    comp = [(a, b) for a in ins for b in ins if a < b and all(p != q for p, q in zip(ins[a], ins[b]))]
    return mirror, comp


def _hflip(b, w, h):
    return bytes(b[y * w + (w - 1 - x)] for y in range(h) for x in range(w))


def _vflip(b, w, h):
    return bytes(b[(h - 1 - y) * w + x] for y in range(h) for x in range(w))


def _connected(b, w, h):
    ink = {(x, y) for y in range(h) for x in range(w) if b[y * w + x]}
    if not ink:
        return False
    stack, seen = [next(iter(ink))], set()
    while stack:
        p = stack.pop()
        if p in seen:
            continue
        seen.add(p)
        stack += [q for q in ((p[0] + dx, p[1] + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1))
                  if q in ink and q not in seen]
    return len(seen) == len(ink)


def _a4(v):
    return (v * 15 + 127) // 255


def check(m):
    w, h, t = m.cell_width, m.cell_height, m.box_thickness
    cache = {}

    def g(cp):
        if cp not in cache:
            cache[cp] = render(cp, m).cell_bytes()
        return cache[cp]

    def row(b, y):
        return list(b[y * w:(y + 1) * w])

    def col(b, x):
        return [b[y * w + x] for y in range(h)]

    errs = []

    def err(code, msg):
        errs.append(f"curves {m.tag} {code} {msg}")

    for a, b in ((0xF5D0, 0x2500), (0xF5D1, 0x2502), (0xF5D6, 0x256D), (0xF5D7, 0x256E), (0xF5D8, 0x2570),
                 (0xF5D9, 0x256F)):
        if g(a) != g(b):
            err("9ab", f"U+{a:04X} != U+{b:04X} ({sum(1 for x, y in zip(g(a), g(b)) if x != y)} px)")

    hl, vl = g(0x2500), g(0x2502)
    want = {U: row(vl, 0), D: row(vl, h - 1), L: col(hl, 0), R: col(hl, w - 1)}
    for cp, e in sorted({**ARC_EDGES, **branch_edges()}.items()):
        b = g(cp)
        got = {U: row(b, 0), D: row(b, h - 1), L: col(b, 0), R: col(b, w - 1)}
        node = cp >= 0xF5EE
        for side, name in ((U, "top"), (D, "bottom"), (L, "left"), (R, "right")):
            if e & side:
                if node:  # the circle may touch this edge too; the line's pixels must be solid
                    if any(gv != 255 for gv, wv in zip(got[side], want[side]) if wv == 255):
                        err("9c", f"U+{cp:04X} {name} edge gap {got[side]}")
                elif got[side] != want[side]:
                    err("9c", f"U+{cp:04X} {name} edge {got[side]} != light line {want[side]}")
            elif not node and any(got[side]):
                err("9c", f"U+{cp:04X} {name} edge not empty {got[side]}")

    vleft, htop = max(0, w - t) // 2, max(0, h - t) // 2
    cx, cy = vleft + t / 2, htop + t / 2
    r = min(cx, cy, w - cx, h - cy)
    spans = {U: [(vleft, y) for y in range(0, math.ceil(cy - r) + 1)],
             D: [(vleft, y) for y in range(math.floor(cy + r) - 1, h)],
             L: [(x, htop) for x in range(0, math.ceil(cx - r) + 1)],
             R: [(x, htop) for x in range(math.floor(cx + r) - 1, w)]}
    for i, n in enumerate(branch.NODES):
        b = g(0xF5EE + i)
        for side, pts in spans.items():
            if n & side and any(b[y * w + x] == 0 for x, y in pts if 0 <= x < w and 0 <= y < h):
                err("9d", f"U+{0xF5EE + i:04X} gap between arm and node")

    line_like = [*range(0x256D, 0x2574), *range(0xF5D6, 0xF60E), 0xE0B1, 0xE0B3, 0xE0B5, 0xE0B7,
                 *(cp for cp in range(0x1FBA0, 0x1FBAF) if cp not in (0x1FBA8, 0x1FBA9)), *range(0x1FBD0, 0x1FBE0)]
    for cp in line_like:
        if not _connected(g(cp), w, h):
            err("C", f"U+{cp:04X} ink not connected")

    pairs = [(0x2571, 0x2572, "h"), (0x2573, 0x2573, "h"), (0x2573, 0x2573, "v"), (0xE0B0, 0xE0B2, "h"),
             (0xE0B0, 0xE0B0, "v"), (0xE0B8, 0xE0BA, "h"), (0xE0BC, 0xE0BE, "h"), (0xE0B8, 0xE0BC, "v"),
             (0xE0B4, 0xE0B4, "v"), (0xE0B1, 0xE0B1, "v"),
             (0x1FBD0, 0x1FBD3, "h"), (0x1FBD0, 0x1FBD2, "v"), (0x1FBD4, 0x1FBD6, "h"), (0x1FBD5, 0x1FBD7, "h"),
             (0x1FBE0, 0x1FBE2, "v"), (0x1FBE1, 0x1FBE3, "h"), (0x1FBE8, 0x1FBEA, "v"), (0x1FBE9, 0x1FBEB, "h"),
             (0x1FBEC, 0x1FBEF, "h"), (0x1FBED, 0x1FBEE, "h")]
    if w % 2 == 0:  # centred with integer rounding
        pairs += [(0x1FBA0, 0x1FBA1, "h"), (0x1FBA2, 0x1FBA3, "h"), (0x1FB9A, 0x1FB9A, "h"), (0x1FB6C, 0x1FB6E, "h")]
    if h % 2 == 0:
        pairs += [(0x1FBA0, 0x1FBA2, "v"), (0x1FBA1, 0x1FBA3, "v"), (0x1FB9B, 0x1FB9B, "v"), (0x1FB6D, 0x1FB6F, "v")]
    if (w - t) % 2 == 0:  # light line axis centred
        pairs += [(0x256D, 0x256E, "h"), (0x256F, 0x2570, "h"), (0xF5F0, 0xF5F2, "h"), (0xF5F1, 0xF5F3, "h"),
                  (0xF5DA, 0xF5DD, "h"), (0xF5E6, 0xF5E6, "h")]
    if (h - t) % 2 == 0:
        pairs += [(0x256D, 0x2570, "v"), (0x256E, 0x256F, "v"), (0xF5F6, 0xF5F8, "v"), (0xF5F7, 0xF5F9, "v")]
    mirror, comp = mosaic_pairs(m)
    for a, b, axis in pairs + [(a, b, "h") for a, b in mirror]:
        d = max(abs(x - y) for x, y in zip(g(a), (_hflip if axis == "h" else _vflip)(g(b), w, h)))
        if d > MIRROR_TOL:
            err("M", f"U+{a:04X} vs {axis}-mirror of U+{b:04X}: max diff {d}")

    for a, b in [(0xE0B8, 0xE0BE), (0xE0BA, 0xE0BC), (0x1FB68, 0x1FB6C), (0x1FB69, 0x1FB6D), (0x1FB6A, 0x1FB6E),
                 (0x1FB6B, 0x1FB6F), *comp]:
        bad = sum(1 for x, y in zip(g(a), g(b)) if not (x + y == 255 or (0 < x < 255 and 0 < y < 255 and x + y == 254)))
        if bad:
            err("K", f"U+{a:04X} + U+{b:04X} not a full cell ({bad} px)")

    k = math.ceil(h / (2 * w))
    for cp, x in ((0xE0B0, 0), (0xE0B2, w - 1)):
        edge = col(g(cp), x)
        if any(v != 255 for v in edge[k:h - k]):
            err("P", f"U+{cp:04X} edge column {x} has a gap: {edge}")
        tip = col(g(cp), w - 1 - x)
        if not (tip[h // 2 - 1] and tip[h // 2]):
            err("P", f"U+{cp:04X} tip does not reach column {w - 1 - x}")
    for cp, x in ((0xE0B4, 0), (0xE0B6, w - 1)):
        if any(v != 255 for v in col(g(cp), x)):
            err("P", f"U+{cp:04X} edge column {x} has a gap: {col(g(cp), x)}")

    for cp, horiz, rev in ((0xF5D2, True, False), (0xF5D3, True, True), (0xF5D4, False, False), (0xF5D5, False, True)):
        line = row(g(cp), htop) if horiz else col(g(cp), vleft)
        lv = [_a4(v) for v in (line[::-1] if rev else line)]
        n = len(lv)
        # steps of 255/n from the far end: the bright end of F5D3/F5D5 is 255 * (n - 1) / n
        if lv[0] < _a4(255 * (n - 1) // n) or any(a < b for a, b in zip(lv, lv[1:])) or len(set(lv)) < min(n, 15):
            err("F", f"U+{cp:04X} A4 levels {lv}")

    for cp in (0xF5D0, 0xF5D1, 0x1FBE4, 0x1FBE5, 0x1FBE6, 0x1FBE7):
        if any(v not in (0, 255) for v in g(cp)):
            err("A", f"U+{cp:04X} has grey pixels")
    return errs
