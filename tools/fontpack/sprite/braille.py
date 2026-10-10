"""Braille patterns U+2800-28FF."""
from .registry import draws

# Unicode dot n (1..8) is bit n-1: (column, row) of each bit.
DOTS = ((0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (1, 2), (0, 3), (1, 3))


def gaps(size, w, pos):
    """Gaps between dots inside the cell, then across the cell edge."""
    return [b - a - w for a, b in zip(pos, pos[1:])] + [size - pos[-1] - w + pos[0]]


def _even(size, n, w):
    return tuple(k * size // n + ((k + 1) * size // n - k * size // n - w) // 2 for k in range(n))


def _spread(width, height):
    """Dot size and dot columns/rows: spare pixels go to dot size, margins and spacing in turn."""
    w = min(width // 4, height // 8)
    xs, ys = width // 4, height // 8
    xm, ym = xs // 2, ys // 2
    xl = width - 2 * xm - xs - 2 * w
    yl = height - 2 * ym - 3 * ys - 4 * w
    if xl >= 2 and yl >= 4 and w == 0:
        w, xl, yl = w + 1, xl - 2, yl - 4
    if xl >= 2 and xm == 0:
        xm, xl = 1, xl - 2
    if yl >= 2 and ym == 0:
        ym, yl = 1, yl - 2
    if xl >= 1:
        xs, xl = xs + 1, xl - 1
    if yl >= 3:
        ys, yl = ys + 1, yl - 3
    if xl >= 2:
        xm, xl = xm + 1, xl - 2
    if yl >= 2:
        ym, yl = ym + 1, yl - 2
    if xl >= 2 and yl >= 4:
        w, xl, yl = w + 1, xl - 2, yl - 4
    return w, (xm, xm + w + xs), tuple(ym + i * (w + ys) for i in range(4))


def geometry(width, height):
    """(dot size, columns, rows); an axis whose gaps (across cells too) differ by more than 1 centres each dot in its
    half/quarter of the cell instead."""
    w, xs, ys = _spread(width, height)
    if max(gaps(width, w, xs)) - min(gaps(width, w, xs)) > 1:
        xs = _even(width, 2, w)
    if max(gaps(height, w, ys)) - min(gaps(height, w, ys)) > 1:
        ys = _even(height, 4, w)
    return w, xs, ys


@draws(0x2800, 0x28FF)
def braille(cp, c, m):
    w, xs, ys = geometry(m.cell_width, m.cell_height)
    for b, (col, row) in enumerate(DOTS):
        if (cp - 0x2800) >> b & 1:
            c.rect(xs[col], ys[row], xs[col] + w, ys[row] + w)
