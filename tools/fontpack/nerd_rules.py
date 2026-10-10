"""Nerd icon sizes: the constraints of nerd-fonts' font-patcher and their placement in one cell.

font-patcher is only parsed: its patch sets (setup_patch_set) are evaluated by a whitelist walk over the syntax tree.
Codepoints, scale groups and attribute -> constraint follow Ghostty's nerd_font_codegen.py, the placement Ghostty's
RenderOptions.Constraint (src/font/Glyph.zig); both MIT, https://github.com/ghostty-org/ghostty."""

import ast
import math

from fontTools.pens.boundsPen import BoundsPen

DEFAULT = {"size": "none", "height": "cell", "max_constraint_width": 2, "align_horizontal": "none",
           "align_vertical": "none", "relative_width": 1.0, "relative_height": 1.0, "relative_x": 0.0,
           "relative_y": 0.0, "pad_top": 0.0, "pad_left": 0.0, "pad_right": 0.0, "pad_bottom": 0.0,
           "max_xy_ratio": None}
FIT = DEFAULT | {"size": "fit"}  # a symbol without a rule only shrinks to fit


class Unsupported(Exception):
    pass


class Evaluator:
    """Literals, containers, names assigned in the method, `self.args.<flag>`, range(), unary minus, + and -."""
    ARGS = {"careful": False, "custom": False}  # every other flag True
    NAMES = {"box_enabled": False, "box_keep": False, "True": True, "False": False, "None": None}

    def __init__(self, symbols):
        self.symbols = symbols

    def __call__(self, n):
        if isinstance(n, ast.Constant):
            return n.value
        if isinstance(n, ast.Name):
            if n.id in self.NAMES:
                return self.NAMES[n.id]
            if n.id in self.symbols:
                return self(self.symbols[n.id])
            raise Unsupported(f"name {n.id}")
        if (isinstance(n, ast.Attribute) and isinstance(n.value, ast.Attribute) and isinstance(n.value.value, ast.Name)
                and n.value.value.id == "self" and n.value.attr == "args"):
            return self.ARGS.get(n.attr, True)
        if isinstance(n, (ast.List, ast.Tuple)):
            out = []
            for e in n.elts:
                out.extend(self(e.value)) if isinstance(e, ast.Starred) else out.append(self(e))
            return out
        if isinstance(n, ast.Dict):
            if any(k is None for k in n.keys):
                raise Unsupported("dict unpacking")
            return {self(k): self(v) for k, v in zip(n.keys, n.values)}
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id == "range" and not n.keywords:
            args = [self(a) for a in n.args]
            if not all(isinstance(a, int) for a in args):
                raise Unsupported("range arguments")
            return range(*args)
        if isinstance(n, ast.BinOp) and isinstance(n.op, (ast.Add, ast.Sub)):
            a, b = self(n.left), self(n.right)
            if not all(isinstance(v, (int, float)) for v in (a, b)):
                raise Unsupported("operands")
            return a + b if isinstance(n.op, ast.Add) else a - b
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.USub):
            v = self(n.operand)
            if not isinstance(v, (int, float)):
                raise Unsupported("operand")
            return -v
        raise Unsupported(type(n).__name__)


def patch_sets(source):
    """The enabled patch sets of font-patcher (class font_patcher, method setup_patch_set) and its version."""
    tree = ast.parse(source)
    version = next(ast.literal_eval(s.value) for s in tree.body if isinstance(s, ast.Assign) and len(s.targets) == 1
                   and isinstance(s.targets[0], ast.Name) and s.targets[0].id == "version")
    cls = next(s for s in tree.body if isinstance(s, ast.ClassDef) and s.name == "font_patcher")
    fn = next(s for s in cls.body if isinstance(s, ast.FunctionDef) and s.name == "setup_patch_set")
    ev = Evaluator({s.targets[0].id: s.value for s in fn.body
                    if isinstance(s, ast.Assign) and len(s.targets) == 1 and isinstance(s.targets[0], ast.Name)})
    sets = []
    for s in fn.body:
        if isinstance(s, ast.Assign) and isinstance(s.targets[0], ast.Attribute) and s.targets[0].attr == "patch_set":
            for elt in s.value.elts:
                entry = {ev(k): ev(v) for k, v in zip(elt.keys, elt.values)}
                if entry.pop("Enabled", True):
                    sets.append(entry)
    return sets, version


def codepoint_tables(sets, cmap, source_cmap):
    """{patch set: {source scalar: Nerd scalar}}. A set with its own source font (source_cmap(Filename)) numbers its
    glyphs from SrcStart in the source's order; an Exact set keeps the scalars of the Symbols font that no earlier set
    took."""
    tables, used = {}, set()
    for e in sets:
        src = None if e["Exact"] else source_cmap(e["Filename"])
        nerd = e["SrcStart"] - 1 if src is not None else 0
        t = tables.setdefault(e["Name"], {})
        for cp in range(e["SymStart"], e["SymEnd"] + 1):
            if src is not None:
                if cp not in src:
                    continue
                nerd += 1
            else:
                nerd = cp
                if cp not in cmap or cp in used:
                    continue
            if nerd not in cmap or nerd in used:
                raise ValueError(f"{e['Name']}: U+{nerd:04X} missing or taken")
            t[cp] = nerd
            used.add(nerd)
    return tables


def bounds(font, cp):
    glyphs = font.getGlyphSet()
    pen = BoundsPen(glyphSet=glyphs)
    glyphs[font.getBestCmap()[cp]].draw(pen)
    return pen.bounds


def attributes(sets, font, tables):
    """{Nerd scalar: font-patcher attributes}, with the relative box of the scalar in its scale group."""
    out = {}
    for e in sets:
        attrs, table = e["Attributes"], tables[e["Name"]]
        mine = {table[cp]: dict(attrs.get(cp, attrs["default"])) for cp in range(e["SymStart"], e["SymEnd"] + 1)
                if cp in table}
        for group in (e["ScaleRules"] or {}).get("ScaleGroups", []):
            x0 = y0 = math.inf
            x1 = y1 = -math.inf
            own, advances = {}, set()
            for cp in group:
                if cp not in table:
                    if e["Name"] == "Progress Indicators" and cp == 0xEDFF:  # a helper glyph for vertical padding
                        b = bounds(font, 0xE0B0)
                        y0, y1 = min(b[1], y0), max(b[3], y1)
                    continue
                b = own[table[cp]] = bounds(font, table[cp])
                advances.add(font["hmtx"][font.getBestCmap()[table[cp]]][0])
                x0, y0, x1, y1 = min(b[0], x0), min(b[1], y0), max(b[2], x1), max(b[3], y1)
            mono = len(own) > 1 and len(advances) == 1
            for nerd, b in own.items():
                a = mine.get(nerd)
                if a is None or "relative_height" in a:
                    continue
                a["relative_height"], a["relative_y"] = (b[3] - b[1]) / (y1 - y0), (b[1] - y0) / (y1 - y0)
                if mono:
                    a["relative_width"], a["relative_x"] = (b[2] - b[0]) / (x1 - x0), (b[0] - x0) / (x1 - x0)
        if out.keys() & mine.keys():
            raise ValueError(f"{e['Name']}: overlapping scalars")
        out |= mine
    return out


def constraint(attr):
    """font-patcher attributes as a constraint."""
    align = {"l": "start", "r": "end", "c": "center1"}
    stretch, params = attr.get("stretch", ""), attr.get("params", {})
    overlap, ratio, ypad = params.get("overlap", 0.0), params.get("xy-ratio", -1.0), params.get("ypadding", 0.0)
    c = dict(DEFAULT)
    if "pa" in stretch:
        c["size"] = "cover" if "!" in stretch or overlap else "fit_cover1"
    elif "xy" in stretch:
        c["size"] = "stretch"
    else:
        raise ValueError(f"stretch {stretch}")
    if "^" not in stretch:
        c["height"] = "icon"
    if "1" in stretch or ("pa" not in stretch and "2" not in stretch):
        c["max_constraint_width"] = 1
    c["align_horizontal"] = align.get(attr.get("align", ""), "none")
    c["align_vertical"] = align.get(attr.get("valign", ""), "none")
    for k in ("relative_width", "relative_height", "relative_x", "relative_y"):
        c[k] = attr.get(k, c[k])
    if overlap:
        c["pad_left"] = c["pad_right"] = -overlap / 2
        c["pad_top"] = c["pad_bottom"] = -min(0.01, overlap) / 2
    elif ypad:
        c["pad_top"] = c["pad_bottom"] = ypad / 2
    if ratio > 0:
        c["max_xy_ratio"] = ratio
    return c


def rules(patcher, font, source_cmap):
    """{Nerd scalar: constraint} from font-patcher's source text, the Symbols font (a TTFont) and the cmaps of the
    patch sets' source fonts; also font-patcher's version."""
    sets, version = patch_sets(patcher)
    tables = codepoint_tables(sets, font.getBestCmap(), source_cmap)
    return {cp: constraint(a) for cp, a in attributes(sets, font, tables).items()}, version


def scale(c, gw, gh, m):
    if c["size"] == "none":
        return 1.0, 1.0
    tw = (1 - c["pad_left"] - c["pad_right"]) * m["face_width"]
    th = (1 - c["pad_bottom"] - c["pad_top"]) * (m["face_height"] if c["height"] == "cell" else m["icon_height"])
    wf, hf = tw / gw, th / gh
    if c["size"] == "fit":
        wf = hf = min(1, wf, hf)
    elif c["size"] in ("cover", "fit_cover1"):
        wf = hf = min(wf, hf)
    if c["max_xy_ratio"] is not None and gw * wf > gh * hf * c["max_xy_ratio"]:
        wf = gh * hf * c["max_xy_ratio"] / gw
    return wf, hf


def place(c, glyph, m):
    """The box (width, height, x, y) of a glyph box (pixels, y up from the cell bottom) under constraint c in one cell.
    m: cell_width, cell_height, face_width, face_height, face_y (face box in the cell), icon_height."""
    w, h, x, y = glyph
    if c["size"] == "stretch":
        m = m | {"face_width": float(m["cell_width"]), "face_height": float(m["cell_height"]), "face_y": 0.0}
        c = c | {k: max(0.0, c[k]) for k in ("pad_top", "pad_bottom", "pad_left", "pad_right")}
    gw, gh = w / c["relative_width"], h / c["relative_height"]
    gx, gy = x - gw * c["relative_x"], y - gh * c["relative_y"]
    wf, hf = scale(c, gw, gh, m)
    cx, cy = gx + gw / 2, gy + gh / 2
    gw, gh = gw * wf, gh * hf
    gx, gy = cx - gw / 2, cy - gh / 2
    if c["size"] != "none" or c["align_vertical"] != "none":
        start = m["face_y"] + c["pad_bottom"] * m["face_height"]
        end = m["face_y"] + m["face_height"] - gh - c["pad_top"] * m["face_height"]
        a = c["align_vertical"]
        gy = (((start + end) / 2 if end < start else max(start, min(gy, end))) if a == "none" else start
              if a == "start" else end if a == "end" else (start + end) / 2)
    if c["size"] != "none" or c["align_horizontal"] != "none":
        start = c["pad_left"] * m["face_width"]
        end = m["face_width"] - gw - c["pad_right"] * m["face_width"]
        a = c["align_horizontal"]
        gx = (max(start, min(gx, end)) if a == "none" else start if a == "start" else max(start, end) if a == "end"
              else max(start, (start + m["face_width"] - gw - c["pad_right"] * m["face_width"]) / 2))
    return wf * w, hf * h, gx + gw * c["relative_x"], gy + gh * c["relative_y"]
