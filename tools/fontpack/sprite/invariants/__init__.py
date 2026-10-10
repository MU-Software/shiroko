"""Cell-size invariants of the generated glyphs, derived from Unicode names and the glyphs' own base strokes (no
reference images). Each module's check(m) returns violation strings; empty means the glyphs hold."""
from . import box, cells, curves

SIZES = ((8, 16), (10, 20), (6, 12), (7, 14), (9, 17), (12, 24), (16, 32))
MODULES = (box, cells, curves)


def check_all(metrics):
    """Violations of every module at every size of SIZES; metrics(w, h) gives the cell's Metrics."""
    out = cells.check_names()
    for w, h in SIZES:
        m = metrics(w, h)
        for mod in MODULES:
            out += mod.check(m)
    return out
