from .canvas import Canvas

REGISTRY = {}


def draws(lo, hi=None):
    """Registers fn(cp, canvas, metrics) for lo..hi (inclusive)."""
    def wrap(fn):
        for cp in range(lo, (lo if hi is None else hi) + 1):
            if cp in REGISTRY:
                raise ValueError(f"U+{cp:04X} registered twice ({REGISTRY[cp][1]}, {fn.__module__})")
            REGISTRY[cp] = (fn, fn.__module__)
        return fn
    return wrap


def render(cp, m, canvas=None):
    """Draws cp into a padded canvas laid out like Ghostty's sprite canvas."""
    c = canvas or Canvas(m.cell_width, m.cell_height)
    c.clear()
    REGISTRY[cp][0](cp, c, m)
    c.clear_clipping_regions()
    return c
