"""Block elements 2580-259F. Odd cell sizes give the two halves the same width, overlapping on the middle line,
as Ghostty does; shades are flat alpha."""
from .common import block, fill
from .registry import draws

# cp: (horizontal align, vertical align, width fraction, height fraction)
BLOCKS = {
    0x2580: ("l", "t", 1, 1 / 2), 0x2581: ("l", "b", 1, 1 / 8), 0x2582: ("l", "b", 1, 1 / 4),
    0x2583: ("l", "b", 1, 3 / 8), 0x2584: ("l", "b", 1, 1 / 2), 0x2585: ("l", "b", 1, 5 / 8),
    0x2586: ("l", "b", 1, 3 / 4), 0x2587: ("l", "b", 1, 7 / 8), 0x2588: ("l", "t", 1, 1),
    0x2589: ("l", "t", 7 / 8, 1), 0x258A: ("l", "t", 3 / 4, 1), 0x258B: ("l", "t", 5 / 8, 1),
    0x258C: ("l", "t", 1 / 2, 1), 0x258D: ("l", "t", 3 / 8, 1), 0x258E: ("l", "t", 1 / 4, 1),
    0x258F: ("l", "t", 1 / 8, 1), 0x2590: ("r", "t", 1 / 2, 1), 0x2594: ("l", "t", 1, 1 / 8),
    0x2595: ("r", "t", 1 / 8, 1),
}

# quadrants as tl, tr, bl, br
QUADS = {
    0x2596: "0010", 0x2597: "0001", 0x2598: "1000", 0x2599: "1011", 0x259A: "1001",
    0x259B: "1110", 0x259C: "1101", 0x259D: "0100", 0x259E: "0110", 0x259F: "0111",
}

SHADES = {0x2591: 0x40, 0x2592: 0x80, 0x2593: 0xC0}

_QUARTERS = ((0, .5, 0, .5), (.5, 1, 0, .5), (0, .5, .5, 1), (.5, 1, .5, 1))


@draws(0x2580, 0x259F)
def draw(cp, c, m):
    if cp in BLOCKS:
        block(c, m, *BLOCKS[cp])
    elif cp in SHADES:
        c.rect(0, 0, m.cell_width, m.cell_height, SHADES[cp])
    else:
        for on, q in zip(QUADS[cp], _QUARTERS):
            if on == "1":
                fill(c, m, *q)
