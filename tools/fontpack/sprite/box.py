"""Box drawing 2500-257F: lines, dashes, rounded corners and diagonals."""
from typing import NamedTuple

from . import aa
from .common import DOUBLE, HEAVY, LIGHT, NONE, box, hline_middle, stroke_px, vline_middle
from .registry import draws

TL, TR, BL, BR = "tl", "tr", "bl", "br"

_S = {"N": NONE, "L": LIGHT, "H": HEAVY, "D": DOUBLE}

# arms as up, right, down, left
LINES = {
    0x2500: "NLNL", 0x2501: "NHNH", 0x2502: "LNLN", 0x2503: "HNHN", 0x250C: "NLLN", 0x250D: "NHLN", 0x250E: "NLHN",
    0x250F: "NHHN", 0x2510: "NNLL", 0x2511: "NNLH", 0x2512: "NNHL", 0x2513: "NNHH", 0x2514: "LLNN", 0x2515: "LHNN",
    0x2516: "HLNN", 0x2517: "HHNN", 0x2518: "LNNL", 0x2519: "LNNH", 0x251A: "HNNL", 0x251B: "HNNH", 0x251C: "LLLN",
    0x251D: "LHLN", 0x251E: "HLLN", 0x251F: "LLHN", 0x2520: "HLHN", 0x2521: "HHLN", 0x2522: "LHHN", 0x2523: "HHHN",
    0x2524: "LNLL", 0x2525: "LNLH", 0x2526: "HNLL", 0x2527: "LNHL", 0x2528: "HNHL", 0x2529: "HNLH", 0x252A: "LNHH",
    0x252B: "HNHH", 0x252C: "NLLL", 0x252D: "NLLH", 0x252E: "NHLL", 0x252F: "NHLH", 0x2530: "NLHL", 0x2531: "NLHH",
    0x2532: "NHHL", 0x2533: "NHHH", 0x2534: "LLNL", 0x2535: "LLNH", 0x2536: "LHNL", 0x2537: "LHNH", 0x2538: "HLNL",
    0x2539: "HLNH", 0x253A: "HHNL", 0x253B: "HHNH", 0x253C: "LLLL", 0x253D: "LLLH", 0x253E: "LHLL", 0x253F: "LHLH",
    0x2540: "HLLL", 0x2541: "LLHL", 0x2542: "HLHL", 0x2543: "HLLH", 0x2544: "HHLL", 0x2545: "LLHH", 0x2546: "LHHL",
    0x2547: "HHLH", 0x2548: "LHHH", 0x2549: "HLHH", 0x254A: "HHHL", 0x254B: "HHHH", 0x2550: "NDND", 0x2551: "DNDN",
    0x2552: "NDLN", 0x2553: "NLDN", 0x2554: "NDDN", 0x2555: "NNLD", 0x2556: "NNDL", 0x2557: "NNDD", 0x2558: "LDNN",
    0x2559: "DLNN", 0x255A: "DDNN", 0x255B: "LNND", 0x255C: "DNNL", 0x255D: "DNND", 0x255E: "LDLN", 0x255F: "DLDN",
    0x2560: "DDDN", 0x2561: "LNLD", 0x2562: "DNDL", 0x2563: "DNDD", 0x2564: "NDLD", 0x2565: "NLDL", 0x2566: "NDDD",
    0x2567: "LDND", 0x2568: "DLNL", 0x2569: "DDND", 0x256A: "LDLD", 0x256B: "DLDL", 0x256C: "DDDD", 0x2574: "NNNL",
    0x2575: "LNNN", 0x2576: "NLNN", 0x2577: "NNLN", 0x2578: "NNNH", 0x2579: "HNNN", 0x257A: "NHNN", 0x257B: "NNHN",
    0x257C: "NHNL", 0x257D: "LNHN", 0x257E: "NLNH", 0x257F: "HNLN",
}

# cp: (vertical, count, stroke style, gap style); gap None = max(4, light)
DASHES = {
    0x2504: (False, 3, LIGHT, None), 0x2505: (False, 3, HEAVY, None),
    0x2506: (True, 3, LIGHT, None), 0x2507: (True, 3, HEAVY, None),
    0x2508: (False, 4, LIGHT, None), 0x2509: (False, 4, HEAVY, None),
    0x250A: (True, 4, LIGHT, None), 0x250B: (True, 4, HEAVY, None),
    0x254C: (False, 2, LIGHT, LIGHT), 0x254D: (False, 2, HEAVY, HEAVY),
    0x254E: (True, 2, LIGHT, HEAVY), 0x254F: (True, 2, HEAVY, HEAVY),
}


class Placement(NamedTuple):
    """Half-open row spans of the horizontal strokes and column spans of the vertical ones."""
    h_light_top: int
    h_light_bottom: int
    h_heavy_top: int
    h_heavy_bottom: int
    h_double_top: int
    h_double_bottom: int
    v_light_left: int
    v_light_right: int
    v_heavy_left: int
    v_heavy_right: int
    v_double_left: int
    v_double_right: int


def placement(m):
    """Strokes are centred; a double stroke is two light strokes one light thickness either side of the light one."""
    lt, hv = stroke_px(m, LIGHT), stroke_px(m, HEAVY)
    w, h = m.cell_width, m.cell_height
    hlt, hht = max(0, h - lt) // 2, max(0, h - hv) // 2
    vll, vhl = max(0, w - lt) // 2, max(0, w - hv) // 2
    return Placement(hlt, hlt + lt, hht, hht + hv, max(0, hlt - lt), hlt + 2 * lt,
                     vll, vll + lt, vhl, vhl + hv, max(0, vll - lt), vll + 2 * lt)


def _arm_end(a, b, same, other, d_near, l_near, l_far, h_near):
    """Where an arm stops at the centre, given the two crossing arms a and b."""
    if a == HEAVY or b == HEAVY:
        return h_near
    if a != b or same == other:
        return d_near if DOUBLE in (a, b) else l_near
    if a == NONE:
        return l_near
    return l_far


def lines(c, m, up=NONE, right=NONE, down=NONE, left=NONE):
    """Each arm runs from its cell edge to the centre; the joins follow Ghostty."""
    p = placement(m)
    w, h = m.cell_width, m.cell_height
    up_bottom = _arm_end(left, right, down, up, p.h_double_bottom, p.h_light_bottom, p.h_light_top, p.h_heavy_bottom)
    down_top = _arm_end(left, right, up, down, p.h_double_top, p.h_light_top, p.h_light_bottom, p.h_heavy_top)
    left_right = _arm_end(up, down, left, right, p.v_double_right, p.v_light_right, p.v_light_left, p.v_heavy_right)
    right_left = _arm_end(up, down, right, left, p.v_double_left, p.v_light_left, p.v_light_right, p.v_heavy_left)

    if up == LIGHT:
        box(c, p.v_light_left, 0, p.v_light_right, up_bottom)
    elif up == HEAVY:
        box(c, p.v_heavy_left, 0, p.v_heavy_right, up_bottom)
    elif up == DOUBLE:
        box(c, p.v_double_left, 0, p.v_light_left, p.h_light_top if left == DOUBLE else up_bottom)
        box(c, p.v_light_right, 0, p.v_double_right, p.h_light_top if right == DOUBLE else up_bottom)

    if right == LIGHT:
        box(c, right_left, p.h_light_top, w, p.h_light_bottom)
    elif right == HEAVY:
        box(c, right_left, p.h_heavy_top, w, p.h_heavy_bottom)
    elif right == DOUBLE:
        box(c, p.v_light_right if up == DOUBLE else right_left, p.h_double_top, w, p.h_light_top)
        box(c, p.v_light_right if down == DOUBLE else right_left, p.h_light_bottom, w, p.h_double_bottom)

    if down == LIGHT:
        box(c, p.v_light_left, down_top, p.v_light_right, h)
    elif down == HEAVY:
        box(c, p.v_heavy_left, down_top, p.v_heavy_right, h)
    elif down == DOUBLE:
        box(c, p.v_double_left, p.h_light_bottom if left == DOUBLE else down_top, p.v_light_left, h)
        box(c, p.v_light_right, p.h_light_bottom if right == DOUBLE else down_top, p.v_double_right, h)

    if left == LIGHT:
        box(c, 0, p.h_light_top, left_right, p.h_light_bottom)
    elif left == HEAVY:
        box(c, 0, p.h_heavy_top, left_right, p.h_heavy_bottom)
    elif left == DOUBLE:
        box(c, 0, p.h_double_top, p.v_light_left if up == DOUBLE else left_right, p.h_light_top)
        box(c, 0, p.h_light_bottom, p.v_light_left if down == DOUBLE else left_right, p.h_double_bottom)


def dash_spans(size, count, desired_gap, centered):
    """Dash spans along one axis, or None when the cell is too small. count gaps per cell (horizontal: half gaps
    at both ends, vertical: a full gap at the bottom); leftover pixels widen the first dashes."""
    if size < 2 * count:
        return None
    gap = min(desired_gap, size // (2 * count))
    dash, extra = divmod(size - count * gap, count)
    pos = gap // 2 if centered else 0
    spans = []
    for i in range(count):
        end = pos + dash + (1 if i < extra else 0)
        spans.append((pos, end))
        pos = end + gap
    return spans


def dashes(c, m, vertical, count, style, gap_style):
    t = stroke_px(m, style)
    gap = max(4, stroke_px(m, LIGHT)) if gap_style is None else stroke_px(m, gap_style)
    spans = dash_spans(m.cell_height if vertical else m.cell_width, count, gap, not vertical)
    if spans is None:
        (vline_middle if vertical else hline_middle)(c, m, LIGHT)
        return
    at = max(0, (m.cell_width if vertical else m.cell_height) - t) // 2
    for a, b in spans:
        if vertical:
            c.rect(at, a, at + t, b)
        else:
            c.rect(a, at, b, at + t)


def arc(c, m, corner, style=LIGHT):
    """Rounded corner along the line axis. The radius is min(w, h) / 2 cut to the distance from the axis to the
    edges the corner runs to, so the line meets the cell edge with its plain profile."""
    t = stroke_px(m, style)
    w, h = m.cell_width, m.cell_height
    cx = max(0, w - t) // 2 + t / 2
    cy = max(0, h - t) // 2 + t / 2
    up = corner in (TL, TR)
    sx = -1 if corner in (TL, BL) else 1
    sy = -1 if up else 1
    r = min(w / 2, h / 2, cx if sx < 0 else w - cx, cy if up else h - cy)
    s = 0.25
    p = aa.Path(c)
    p.move_to(cx, 0 if up else h)
    p.line_to(cx, cy + sy * r)
    p.curve_to(cx, cy + sy * s * r, cx + sx * s * r, cy, cx + sx * r, cy)
    p.line_to(0 if sx < 0 else w, cy)
    aa.stroke(c, p, t)


def diagonal(c, m, rising):
    """Light cell diagonal, overshooting the corners by half a pixel along the slope."""
    w, h = m.cell_width, m.cell_height
    sx, sy = min(1.0, w / h), min(1.0, h / w)
    t = stroke_px(m, LIGHT)
    if rising:
        aa.line(c, w + 0.5 * sx, -0.5 * sy, -0.5 * sx, h + 0.5 * sy, t)
    else:
        aa.line(c, -0.5 * sx, -0.5 * sy, w + 0.5 * sx, h + 0.5 * sy, t)


@draws(0x2500, 0x257F)
def draw(cp, c, m):
    if cp in LINES:
        lines(c, m, *(_S[k] for k in LINES[cp]))
    elif cp in DASHES:
        dashes(c, m, *DASHES[cp])
    elif cp <= 0x2570:
        arc(c, m, {0x256D: BR, 0x256E: BL, 0x256F: TL, 0x2570: TR}[cp])
    else:
        if cp != 0x2572:
            diagonal(c, m, True)
        if cp != 0x2571:
            diagonal(c, m, False)
