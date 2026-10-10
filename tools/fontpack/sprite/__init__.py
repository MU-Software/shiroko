"""Glyphs drawn at bake time instead of rasterised from a font: box drawing, blocks, Braille, legacy computing,
branch drawing and Powerline. The rules follow Ghostty's sprite code (MIT, src/font/sprite), reimplemented."""
import importlib

from . import common
from .metrics import Metrics
from .registry import REGISTRY, draws, render

__all__ = ["MODULES", "Metrics", "REGISTRY", "draws", "load", "render"]
MODULES = ("box", "blocks", "legacy", "braille", "branch", "powerline")


def load(unicode_data):
    """Imports the drawing modules once; unicode_data is the UnicodeData.txt they read names from."""
    common.UNICODE_DATA = unicode_data
    for name in MODULES:
        importlib.import_module(f".{name}", __name__)
    return REGISTRY
