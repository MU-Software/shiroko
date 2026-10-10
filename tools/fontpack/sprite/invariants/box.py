"""Box drawing 2500-257F (without the arcs and diagonals 256D-2573) and block elements 2580-259F.

What each glyph contains comes from its Unicode name; the stroke profiles come from the base glyphs 2500-2503,
2550 and 2551.
"""
from .. import blocks as _blocks  # noqa: F401  (registers the drawers)
from .. import box as _box  # noqa: F401
from ..canvas import Canvas
from ..common import unicode_names, zround
from ..registry import render

N, L, H, D = 0, 1, 2, 3
_WEIGHT = {"LIGHT": L, "SINGLE": L, "HEAVY": H, "DOUBLE": D}
_DIRS = {"UP": ("u",), "DOWN": ("d",), "LEFT": ("l",), "RIGHT": ("r",), "VERTICAL": ("u", "d"),
         "HORIZONTAL": ("l", "r")}
_FRAC = {"HALF": 1 / 2, "ONE EIGHTH": 1 / 8, "ONE QUARTER": 1 / 4, "THREE EIGHTHS": 3 / 8, "FIVE EIGHTHS": 5 / 8,
         "THREE QUARTERS": 3 / 4, "SEVEN EIGHTHS": 7 / 8}
BOX = [cp for cp in range(0x2500, 0x2580) if not 0x256D <= cp <= 0x2573]
BLOCKS = list(range(0x2580, 0x25A0))
SHADES = (0x2591, 0x2592, 0x2593)


def name_spec(cp):
    """('lines', (up, right, down, left)) or ('dash', vertical, count, weight), from the Unicode name."""
    words = unicode_names()[cp].removeprefix("BOX DRAWINGS ").split()
    if "DASH" in words:
        count = {"DOUBLE": 2, "TRIPLE": 3, "QUADRUPLE": 4}[words[words.index("DASH") - 1]]
        return ("dash", words[-1] == "VERTICAL", count, _WEIGHT[words[0]])
    arms = {"u": N, "r": N, "d": N, "l": N}
    weight = None
    for clause in " ".join(words).split(" AND "):
        cw = clause.split()
        weight = next((_WEIGHT[w] for w in cw if w in _WEIGHT), weight)
        for w in cw:
            for a in _DIRS.get(w, ()):
                arms[a] = weight
    return ("lines", (arms["u"], arms["r"], arms["d"], arms["l"]))


def block_spec(cp):
    """('full',), ('shade', level), ('side', side, fraction) or ('quad', {'tl', ...}), from the Unicode name."""
    n = unicode_names()[cp]
    if n == "FULL BLOCK":
        return ("full",)
    if n.endswith(" SHADE"):
        return ("shade", {"LIGHT": 1, "MEDIUM": 2, "DARK": 3}[n.split()[0]])
    if n.startswith("QUADRANT "):
        q = set()
        for part in n.removeprefix("QUADRANT ").split(" AND "):
            v, hz = part.split()
            q.add(("t" if v == "UPPER" else "b") + ("l" if hz == "LEFT" else "r"))
        return ("quad", q)
    side, rest = n.split(" ", 1)
    return ("side", side.lower(), _FRAC[rest.removesuffix(" BLOCK")])


class Glyph:
    def __init__(self, cp, m, c):
        render(cp, m, c)
        self.w, self.h = m.cell_width, m.cell_height
        cell = c.cell_bytes()
        self.px = [list(cell[y * self.w:(y + 1) * self.w]) for y in range(self.h)]
        self.outside = sum(1 for v in c.padded_bytes() if v) - sum(1 for v in cell if v)

    def col(self, x):
        return [r[x] for r in self.px]

    def row(self, y):
        return list(self.px[y])

    def ink(self):
        return {(x, y) for y, r in enumerate(self.px) for x, v in enumerate(r) if v}


def _runs(seq):
    """[(is_255, length), ...]"""
    out = []
    for v in seq:
        on = v == 255
        if out and out[-1][0] == on:
            out[-1][1] += 1
        else:
            out.append([on, 1])
    return [tuple(r) for r in out]


def _span(profile):
    on = [i for i, v in enumerate(profile) if v]
    return (on[0], on[-1] + 1) if on else None


def check(m):
    w, h = m.cell_width, m.cell_height
    c = Canvas(w, h)
    g = {cp: Glyph(cp, m, c) for cp in BOX + BLOCKS}
    bad = []

    def v(msg):
        bad.append(f"box {m.tag} {msg}")

    # 1: coverage only 0/255 (shades aside), nothing outside the cell
    for cp, gl in g.items():
        odd = {p for r in gl.px for p in r if p not in (0, 255)}
        if odd and cp not in SHADES:
            v(f"U+{cp:04X} (1) grey coverage {sorted(odd)[:4]}")
        if gl.outside:
            v(f"U+{cp:04X} (1) {gl.outside} px outside the cell")

    # 2: base glyphs are uniform along the stroke; their cross-sections are the canonical profiles
    hprof, vprof = {N: [0] * h}, {N: [0] * w}
    for style, hcp, vcp in ((L, 0x2500, 0x2502), (H, 0x2501, 0x2503), (D, 0x2550, 0x2551)):
        hprof[style], vprof[style] = g[hcp].col(0), g[vcp].row(0)
        if any(g[hcp].col(x) != hprof[style] for x in range(w)):
            v(f"U+{hcp:04X} (2) not uniform along the stroke")
        if any(g[vcp].row(y) != vprof[style] for y in range(h)):
            v(f"U+{vcp:04X} (2) not uniform along the stroke")

    # 3: light vertical = light horizontal, heavy = 2 x light, one solid run, centred within 1 px
    thick = {}
    for style in (L, H):
        hs, vs = _span(hprof[style]), _span(vprof[style])
        if hs is None or vs is None:
            v(f"(3) style {style} base glyph empty")
            continue
        th, tv = hs[1] - hs[0], vs[1] - vs[0]
        thick[style] = th
        if th != tv:
            v(f"(3) style {style} horizontal {th}px != vertical {tv}px")
        if sum(1 for p in hprof[style] if p) != th or sum(1 for p in vprof[style] if p) != tv:
            v(f"(3) style {style} stroke not one solid run")
        if abs(hs[0] - (h - hs[1])) > 1 or abs(vs[0] - (w - vs[1])) > 1:
            v(f"(3) style {style} not centred: rows {hs} cols {vs}")
    if L in thick and H in thick and thick[H] != 2 * thick[L]:
        v(f"(3) heavy {thick[H]}px != 2 x light {thick[L]}px")

    # 4: double = two light-thick runs with a gap, symmetric about the light stroke
    for cp, prof, light in ((0x2550, hprof[D], hprof[L]), (0x2551, vprof[D], vprof[L])):
        runs = _runs(prof)
        on = [n for is_on, n in runs if is_on]
        if len(on) != 2 or any(n != thick.get(L) for n in on):
            v(f"U+{cp:04X} (4) runs {runs}")
            continue
        a, b = _span(prof)
        ls = _span(light)
        if ls and ls[0] - a != b - ls[1]:
            v(f"U+{cp:04X} (4) not symmetric about the light stroke {a, b} vs {ls}")

    # 2: every arm ends on the cell edge with its style's profile; a missing arm leaves the edge empty
    specs = {cp: name_spec(cp) for cp in BOX}
    for cp, sp in specs.items():
        if sp[0] != "lines":
            continue
        up, right, down, left = sp[1]
        gl = g[cp]
        for edge, got, want in (("left", gl.col(0), hprof[left]), ("right", gl.col(w - 1), hprof[right]),
                                ("top", gl.row(0), vprof[up]), ("bottom", gl.row(h - 1), vprof[down])):
            if got != want:
                v(f"U+{cp:04X} (2) {edge} edge {_span(got)} != canonical {_span(want)}")

    # 5: dashes over 3 tiled cells: equal gaps, lengths within 1 (longer first); solid light line if too small
    for cp, sp in specs.items():
        if sp[0] != "dash":
            continue
        _, vertical, count, weight = sp
        gl = g[cp]
        size = h if vertical else w
        prof_of = gl.row if vertical else gl.col
        stroke = [i for i in range(size) if any(prof_of(i))]
        sections = {tuple(prof_of(i)) for i in stroke}
        if len(sections) != 1:
            v(f"U+{cp:04X} (5) stroke cross-section varies")
            continue
        sec = list(sections.pop())
        if size < 2 * count:
            if sec != (vprof[L] if vertical else hprof[L]) or len(stroke) != size:
                v(f"U+{cp:04X} (5) small cell: expected a solid light line")
            continue
        solid = vprof[weight] if vertical else hprof[weight]
        if sec != solid:
            v(f"U+{cp:04X} (5) cross-section {_span(sec)} != solid {_span(solid)}")
        k = _span(sec)[0]
        line = [gl.px[y][k] if vertical else gl.px[k][y] for y in range(size)] * 3
        if all(p == 255 for p in line):
            v(f"U+{cp:04X} (5) solid line where {count} dashes fit")
            continue
        start = next(i for i in range(len(line)) if line[i] == 255 and line[i - 1] != 255)
        runs = _runs(line[start:] + line[:start])
        dashes = [n for on, n in runs if on]
        gaps = [n for on, n in runs if not on]
        if len(dashes) != 3 * count:
            v(f"U+{cp:04X} (5) {len(dashes)} dashes in 3 cells, want {3 * count}")
        if len(set(gaps)) != 1:
            v(f"U+{cp:04X} (5) uneven gaps across tiles {gaps}")
        if dashes and (max(dashes) - min(dashes) > 1 or dashes[:count] != sorted(dashes[:count], reverse=True)):
            v(f"U+{cp:04X} (5) dash lengths {dashes[:count]}")

    # 10: mirror pairs of line glyphs about the stroke axis, on the columns/rows both images cover (pairs whose
    # crossing arms mix heavy with other weights have no single axis and are skipped)
    lspec = {sp[1]: cp for cp, sp in specs.items() if sp[0] == "lines"}
    axis = {(k, s): sum(_span(p[s])) - 1 for k, p in (("x", vprof), ("y", hprof)) for s in (L, H)}
    for (up, right, down, left), cp in lspec.items():
        for kind, partner, cross in (("H", (up, left, down, right), {up, down} - {N}),
                                     ("V", (down, right, up, left), {left, right} - {N})):
            pc = lspec.get(partner)
            if pc is None or pc < cp or (H in cross and cross - {H}):
                continue
            ax = axis[("x" if kind == "H" else "y", H if cross == {H} else L)]
            if kind == "H":
                ok = all(g[pc].px[y][x] == g[cp].px[y][ax - x] for y in range(h) for x in range(w) if 0 <= ax - x < w)
            else:
                ok = all(g[pc].px[y][x] == g[cp].px[ax - y][x] for y in range(h) if 0 <= ax - y < h for x in range(w))
            if not ok:
                v(f"U+{cp:04X}/U+{pc:04X} (10) {kind}-mirror differs about axis {ax}/2")

    # 7: block shapes from the names: rectangles on their side, extent within 0.5 px of the fraction
    for cp in BLOCKS:
        sp = block_spec(cp)
        gl = g[cp]
        ink = gl.ink()
        if sp[0] == "full" and len(ink) != w * h:
            v(f"U+{cp:04X} (7) full block has holes")
        elif sp[0] == "side":
            _, side, f = sp
            vertical = side in ("upper", "lower")
            size = h if vertical else w
            if vertical:
                span = _span([1 if any(gl.px[y]) else 0 for y in range(h)])
            else:
                span = _span([1 if any(gl.px[y][x] for y in range(h)) else 0 for x in range(w)])
            if span is None:
                v(f"U+{cp:04X} (7) empty")
                continue
            ext = span[1] - span[0]
            touches = span[0] == 0 if side in ("upper", "left") else span[1] == size
            rect = ext * (w if vertical else h) == len(ink)
            if not touches or not rect or abs(ext - f * size) > 0.5:
                v(f"U+{cp:04X} (7) {side} {f}: span {span} of {size}")
    # 7: complementary pairs: OR = full; AND only on the split line of an odd size (both halves round up)
    full = {(x, y) for y in range(h) for x in range(w)}
    hx = {(w // 2, y) for y in range(h)} if w % 2 else set()
    hy = {(x, h // 2) for x in range(w)} if h % 2 else set()
    split87 = {(x, h - zround(h * 7 / 8)) for x in range(w)} if h * 7 % 8 == 4 else set()
    split89 = {(zround(w * 7 / 8) - 1, y) for y in range(h)} if w * 7 % 8 == 4 else set()
    for a, b, allowed in ((0x2580, 0x2584, hy), (0x258C, 0x2590, hx), (0x2587, 0x2594, split87),
                          (0x2589, 0x2595, split89), (0x2596, 0x259C, hx | hy), (0x2597, 0x259B, hx | hy),
                          (0x2598, 0x259F, hx | hy), (0x259D, 0x2599, hx | hy), (0x259A, 0x259E, hx | hy)):
        ia, ib = g[a].ink(), g[b].ink()
        if ia | ib != full:
            v(f"U+{a:04X}+U+{b:04X} (7) OR leaves {len(full - (ia | ib))} px empty")
        if not (ia & ib) <= allowed:
            v(f"U+{a:04X}+U+{b:04X} (7) AND = {len(ia & ib)} px beyond the split line")
    # 7: eighth blocks nest, each step floor or ceil of size/8
    for seq, size, across in (((0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588), h, w),
                              ((0x258F, 0x258E, 0x258D, 0x258C, 0x258B, 0x258A, 0x2589, 0x2588), w, h)):
        prev, prev_n = set(), 0
        for cp in seq:
            ink = g[cp].ink()
            n = len(ink) // across
            if not prev <= ink or n - prev_n not in (size // 8, -(-size // 8)):
                v(f"U+{cp:04X} (7) eighths do not nest/step evenly ({prev_n} -> {n} of {size})")
            prev, prev_n = ink, n
    # 10: block mirror pairs about the cell centre
    for a, b, kind in ((0x2580, 0x2584, "V"), (0x258C, 0x2590, "H"), (0x2581, 0x2594, "V"), (0x258F, 0x2595, "H"),
                       (0x2596, 0x2597, "H"), (0x2598, 0x259D, "H"), (0x2596, 0x2598, "V"), (0x2597, 0x259D, "V"),
                       (0x2599, 0x259F, "H"), (0x259B, 0x259C, "H"), (0x2599, 0x259B, "V"), (0x259F, 0x259C, "V"),
                       (0x259A, 0x259E, "H"), (0x259A, 0x259E, "V")):
        pa, pb = g[a].px, g[b].px
        if any(pb[y][x] != (pa[y][w - 1 - x] if kind == "H" else pa[h - 1 - y][x]) for y in range(h) for x in range(w)):
            v(f"U+{a:04X}/U+{b:04X} (10) {kind}-mirror differs")
    # shade: each shade one uniform level, levels rising inside (0, 255)
    levels = []
    for cp in SHADES:
        vals = {p for r in g[cp].px for p in r}
        if len(vals) != 1:
            v(f"U+{cp:04X} (shade) not uniform {sorted(vals)[:4]}")
        levels.append(min(vals))
    if not 0 < levels[0] < levels[1] < levels[2] < 255:
        v(f"(shade) levels {levels} not rising inside (0, 255)")
    return bad
