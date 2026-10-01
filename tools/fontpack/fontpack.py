#!/usr/bin/env python3
"""Build, verify and install Shiroko bitmap font packages.

  fontpack.py fetch                 download pinned inputs into $SHIROKO_FONT_CACHE/src (.cache/fonts/src)
  fontpack.py build --cell WxH [PACKAGE ...]   build packages (default set) for one cell size into build/fonts;
      --out DIR --scalars 0041-005A,AC00       never downloads; --scalars builds reduced packages (fuzz seeds)
  fontpack.py builtin --cell WxH --out FILE.c  write the built-in package (ASCII + U+FFFD) as a C array
  fontpack.py verify FILE ...       check every record and glyph of packages
  fontpack.py install --dest DIR FILE ...   verify, stage, then switch activation; ships NOTICE,
                                    LICENSES/ and inventory.json from the packages' build directory
  fontpack.py selftest              exercise install recovery in a temporary directory
"""

import argparse
import collections
import functools
import hashlib
import io
import json
import os
import pathlib
import re
import shutil
import struct
import sys
import tempfile
import zipfile

import fontTools
import freetype
import PIL
import pooch
import uharfbuzz as hb
import xxhash
from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "unicode"))
import gen_unicode_tables as ucd  # noqa: E402

FONTS = ROOT / "fonts"
SRC = pathlib.Path(os.environ.get("SHIROKO_FONT_CACHE") or ROOT / ".cache" / "fonts") / "src"
WORK = pathlib.Path(os.environ.get("SHIROKO_FONT_WORK") or ROOT / ".cache" / "fonts" / "work")
OUT = ROOT / "build" / "fonts"
LOCK = json.loads((FONTS / "fonts.lock.json").read_text(encoding="utf-8"))
CONFIG = json.loads((FONTS / "fontpack.config.json").read_text(encoding="utf-8"))
UNICODE_C = os.environ.get("SHIROKO_UNICODE_TABLES")
VERSION = "fontpack 3"
FORMAT_VERSION = 3
LOAD_FLAGS = {"target_light": freetype.FT_LOAD_TARGET_LIGHT, "target_normal": freetype.FT_LOAD_TARGET_NORMAL}
RENDER_MODES = {"normal": freetype.FT_RENDER_MODE_NORMAL, "light": freetype.FT_RENDER_MODE_LIGHT}

ROLES = {"latin": 1, "cjk": 2, "symbols": 3, "emoji": 4, "nerd": 5}
FMT_A4, FMT_A8 = 1, 2
SEQ_GLYPH, SEQ_ALIAS, SEQ_UVS_DEFAULT, SEQ_UVS_ALT = 1, 2, 3, 4
SECTION = {"MANIFEST": 1, "STRINGS": 2, "SOURCES": 3, "INSTANCES": 4, "CMAP": 6, "SEQS": 7, "SEQPOOL": 8,
           "PAGES": 9, "COVERAGE": 10}
FEAT_A4, FEAT_A8, FEAT_SEQ = 1, 2, 4
HEADER_SIZE = 128
ENTRY_SIZE = 32  # section table entries and page records
MAX_CELL = (64, 127)  # glyph records: u8 width/height, i8 bearing/top for glyphs up to two cells wide
SHAPING = {"direction": "ltr", "language": "und"}  # script per role; default features only


class FormatError(Exception):
    pass


def check(ok, what):
    if not ok:
        raise FormatError(what)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


xxh3 = xxhash.xxh3_64_intdigest


def check_locks():
    for path, doc in ((FONTS / "fonts.lock.json", LOCK), (FONTS / "fontpack.config.json", CONFIG)):
        if doc.get("format") != 1:
            sys.exit(f"{path}: unsupported format {doc.get('format')}")
    if (FONTS / LOCK["text_profile"]).resolve() != ucd.LOCK.resolve():
        sys.exit(f"fonts.lock.json refers to text profile {LOCK['text_profile']}, not {ucd.LOCK.name}")
    nerd = ucd.load_lock()["nerd_font"]
    if not any(f["file"] == nerd["file"] and f.get("member") == nerd["member"] and f["sha256"] == nerd["sha256"]
               for f in LOCK["faces"].values()):
        sys.exit("the text profile's Nerd font is not a face of fonts.lock.json")
    raster = CONFIG["raster"]
    if raster["load"] not in LOAD_FLAGS or raster["render"] not in RENDER_MODES:
        sys.exit(f"unknown raster mode {raster}; known: {sorted(LOAD_FLAGS)} / {sorted(RENDER_MODES)}")


def fetch(download=True):
    for name, f in LOCK["files"].items():
        dest = SRC / name
        if download:
            pooch.retrieve(f["url"], f"sha256:{f['sha256']}", fname=dest.name, path=dest.parent, progressbar=False)
        elif not dest.exists() or hashlib.sha256(dest.read_bytes()).hexdigest() != f["sha256"]:
            sys.exit(f"missing or modified input {dest}; run `make fontpack-fetch`")
    tools = {"freetype": ".".join(map(str, freetype.version())), "harfbuzz": hb.version_string(),
             "fonttools": fontTools.version, "pillow": PIL.__version__}
    for k, v in LOCK["tools"].items():
        if tools[k] != v:
            sys.exit(f"tool version mismatch: {k} {tools[k]} != {v} (run `uv sync --group tools`)")
    return tools


def face_bytes(face_id):
    face = LOCK["faces"][face_id]
    data = (zipfile.ZipFile(SRC / face["file"]).read(face["member"]) if "member" in face
            else (SRC / face["file"]).read_bytes())
    if sha256(data) != face["sha256"]:
        sys.exit(f"sha256 mismatch: face {face_id}")
    if "axes" in face:
        WORK.mkdir(parents=True, exist_ok=True)
        key = json.dumps([face["sha256"], face["axes"], fontTools.version], sort_keys=True).encode()
        cached = WORK / f"{face_id}-{sha256(key)[:16]}.ttf"
        sidecar = cached.with_suffix(".sha256")
        if cached.exists() and sidecar.exists():
            inst = cached.read_bytes()
            if sha256(inst) == sidecar.read_text().strip():
                return inst
        font = TTFont(io.BytesIO(data))
        instancer.instantiateVariableFont(font, face["axes"], inplace=True)
        buf = io.BytesIO()
        font.save(buf)
        data = buf.getvalue()
        write_atomic(cached, data)
        write_atomic(sidecar, (sha256(data) + "\n").encode())
    return data


def write_atomic(path, data):
    tmp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


class Profile:
    """Unicode data of the pinned text profile."""

    def __init__(self):
        lock = ucd.load_lock()
        ucd.verify_inputs(lock)
        self.props, decomp, starts, values, self.sequences, self.vs_bases, _ = ucd.tables(lock)
        self.id = bytes.fromhex(ucd.text_profile_id(lock, starts, values, self.sequences, self.vs_bases))
        if (self.id, lock["cluster_max_scalars"]) != pinned_profile():
            sys.exit(f"{UNICODE_C} does not match the Unicode data in {ucd.CACHE}")
        self.max_scalars, self.max_bytes = lock["cluster_max_scalars"], lock["cluster_max_bytes"]
        self.nfd = {}
        latin = set()
        for f in ucd.data_lines("Scripts.txt"):
            if f[1] == "Latin":
                lo, hi = ucd.cp_range(f[0])
                latin.update(range(lo, hi + 1))
        for cp in decomp:
            full = ucd.full_decomp(cp, decomp)
            if cp in latin and len(full) > 1:
                self.nfd[cp] = tuple(full)
        self.qualified = {}
        status = {}
        for f in ucd.data_lines("emoji-test.txt"):
            status[ucd.parse_seq(f[0])] = f[1]
        fq = {tuple(c for c in s if c != 0xFE0F): s for s, st in status.items() if st == "fully-qualified"}
        for s, st in status.items():
            if st != "fully-qualified" and len(s) > 1:
                target = fq.get(tuple(c for c in s if c != 0xFE0F))
                if target and target != s:
                    self.qualified[s] = target

    def has(self, cp, flag):
        return bool(self.props[cp] & flag)

    def cells(self, cp):
        if self.has(cp, ucd.F_NERD) or self.has(cp, ucd.F_CO):
            return 1
        return 2 if self.has(cp, ucd.F_WIDE) else 1

    def drawable(self, cp):
        return not (self.has(cp, ucd.F_CC) or self.has(cp, ucd.F_DI) or self.has(cp, ucd.F_MARK)
                    or 0xD800 <= cp <= 0xDFFF)


def ivd_pairs():
    pairs = set()
    for raw in (SRC / LOCK["ivd"]).read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            base, sel = line.split(";")[0].split()
            pairs.add((int(base, 16), int(sel, 16)))
    return pairs


class Face:
    def __init__(self, face_id):
        self.id = face_id
        self.data = face_bytes(face_id)
        self.tt = TTFont(io.BytesIO(self.data), lazy=True)
        self.cmap = self.tt.getBestCmap()
        self.upem = self.tt["head"].unitsPerEm
        self.ft = freetype.Face(io.BytesIO(self.data))
        self.hb_font = hb.Font(hb.Face(hb.Blob(self.data)))
        self.uvs = {}
        for sub in self.tt["cmap"].tables:
            if sub.format == 14:
                for sel, entries in sub.uvsDict.items():
                    for base, name in entries:
                        self.uvs[(base, sel)] = None if name is None else self.tt.getGlyphID(name)
        os2, hhea = self.tt["OS/2"], self.tt["hhea"]
        self.ascent = max(os2.sTypoAscender, hhea.ascent)
        self.descent = max(-os2.sTypoDescender, -hhea.descent)
        post = self.tt["post"]
        self.underline = post.underlinePosition
        self.strike = os2.yStrikeoutPosition
        self.advance = collections.Counter(a for a, _ in self.tt["hmtx"].metrics.values() if a).most_common(1)[0][0]
        # the design cell: the full block, else the union of the sextants
        full = [0x2588] if 0x2588 in self.cmap else [c for c in range(0x1FB00, 0x1FB3C) if c in self.cmap]
        boxes = [self.bounds(self.tt.getGlyphID(self.cmap[c])) for c in full]
        self.cell_box = ((min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes),
                          max(b[3] for b in boxes)) if boxes else (0, -self.descent, self.advance, self.ascent))

    def bounds(self, gid):
        pen = BoundsPen(self.tt.getGlyphSet())
        self.tt.getGlyphSet()[self.tt.getGlyphName(gid)].draw(pen)
        return pen.bounds

    def gid(self, cp):
        return self.ft.get_char_index(cp)

    def shape(self, seq, role):
        buf = hb.Buffer()
        buf.add_codepoints(list(seq))
        buf.direction, buf.script, buf.language = SHAPING["direction"], shaping_script(role), SHAPING["language"]
        buf.flags = hb.BufferFlags.REMOVE_DEFAULT_IGNORABLES
        hb.shape(self.hb_font, buf, {})
        return [i.codepoint for i in buf.glyph_infos]


def shaping_script(role):
    return "Zyyy" if role == "emoji" else "Latn"


def resolve_providers(profile, packages, faces):
    """Fix the provider of every scalar at build time (text path)."""
    cover = {name: set(faces[p["faces"]["regular"]].cmap) for name, p in packages.items()}
    cjks = [n for n, p in packages.items() if p["role"] == "cjk"]
    latin = next((n for n, p in packages.items() if p["role"] == "latin"), None)
    out = {n: set() for n in packages}
    for name, p in packages.items():
        role = p["role"]
        for cp in cover[name]:
            if not profile.drawable(cp):
                continue
            cjk_class = profile.has(cp, ucd.F_CJK)
            if role == "latin":
                ok = not cjk_class or not any(cp in cover[c] for c in cjks)
            elif role == "cjk":
                ok = cjk_class or not (latin and cp in cover[latin])
            elif role == "symbols":
                ok = not (latin and cp in cover[latin]) and not (cjks and all(cp in cover[c] for c in cjks))
            elif role == "nerd":
                ok = profile.has(cp, ucd.F_NERD)
            else:
                ok = profile.has(cp, ucd.F_EPRES)
            if ok:
                out[name].add(cp)
    return out


def plan_keys(profile, pkg, face, scalars, ivd, report, keep=None):
    """Keys of one package: scalar -> slot key, sequences -> (kind, slot key)."""
    cmap = {cp: ("cp", cp) for cp in sorted(scalars) if keep is None or cp in keep}
    seqs = {}
    role = pkg["role"]
    if role == "emoji":
        for base in profile.vs_bases:
            if base in face.cmap:
                seqs[(base, 0xFE0F)] = (SEQ_ALIAS, ("cp", base)) if base in cmap else (SEQ_GLYPH, ("seq", (base, 0xFE0F)))
        rejected = []
        for seq in profile.sequences:
            if seq in profile.qualified:
                continue
            glyphs = face.shape(seq, role)
            if len(glyphs) == 1 and glyphs[0] != 0:
                seqs[seq] = (SEQ_GLYPH, ("seq", seq))
            else:
                rejected.append(seq)
        for alias, target in profile.qualified.items():
            if target in seqs:
                seqs[alias] = (SEQ_ALIAS, seqs[target][1])
            elif len(target) == 2 and target[1] == 0xFE0F and target[0] in cmap:
                seqs[alias] = (SEQ_ALIAS, ("cp", target[0]))
            else:
                rejected.append(alias)
        report["emoji_unsupported"] = len(rejected)
        report["emoji_unsupported_sample"] = [" ".join(f"{c:04X}" for c in s) for s in rejected[:40]]
    if role == "cjk":
        n = 0
        for s in range(0xAC00, 0xD7A4):
            if s not in cmap:
                continue
            i = s - 0xAC00
            l, v, t = 0x1100 + i // 588, 0x1161 + (i % 588) // 28, 0x11A7 + i % 28
            seqs[(l, v) if t == 0x11A7 else (l, v, t)] = (SEQ_ALIAS, ("cp", s))
            if t != 0x11A7:
                seqs[(s - (t - 0x11A7), t)] = (SEQ_ALIAS, ("cp", s))
            n += 1
        report["hangul_syllables_aliased"] = n
    if role != "emoji":
        nfd = 0
        for cp, full in profile.nfd.items():
            if cp in cmap:
                seqs[full] = (SEQ_ALIAS, ("cp", cp))
                nfd += 1
        for seq in CONFIG["latin_corpus"]:
            key = tuple(int(x, 16) for x in seq.split())
            glyphs = face.shape(key, role)
            if len(glyphs) == 1 and glyphs[0] != 0:
                seqs[key] = (SEQ_GLYPH, ("seq", key))
        report["nfd_aliases"] = nfd
        uvs = ivs = 0
        for (base, sel), gid in face.uvs.items():
            if base not in cmap:
                continue
            if 0xE0100 <= sel <= 0xE01EF:
                if (base, sel) not in ivd:
                    continue
                ivs += 1
            else:
                uvs += 1
            seqs[(base, sel)] = ((SEQ_UVS_DEFAULT, ("cp", base)) if gid is None
                                 else (SEQ_UVS_ALT, ("gid", gid)))
        report["uvs"], report["ivs"] = uvs, ivs
    if keep is not None:
        seqs = {k: v for k, v in seqs.items() if all(c in keep for c in k)}
    return cmap, seqs


def key_cells(profile, role, key):
    kind, v = key
    if role == "emoji":
        return 2
    if kind == "cp":
        return profile.cells(v)
    if kind == "seq":
        return 2 if 0x1100 <= v[0] <= 0x115F else profile.cells(v[0])
    return 1


def size_metrics(latin_face, size):
    lh, cw = size["line_height"], size["cell_width"]
    ppem = cw * latin_face.upem / latin_face.advance
    asc, desc = latin_face.ascent * ppem / latin_face.upem, latin_face.descent * ppem / latin_face.upem
    baseline = round((lh - asc - desc) / 2 + asc)
    underline = min(lh - 1, baseline + max(1, round(-latin_face.underline * ppem / latin_face.upem)))
    strike = max(0, min(lh - 1, baseline - round(latin_face.strike * ppem / latin_face.upem)))
    return baseline, underline, strike


def fit_ppem(face, role, size, baseline):
    lh, cw = size["line_height"], size["cell_width"]
    if role == "latin":
        return cw * face.upem / face.advance
    cells = 2 if role == "emoji" else 1
    width_fit = cells * cw * face.upem / (face.upem if role == "emoji" else face.advance)
    return min(width_fit, baseline * face.upem / face.ascent, (lh - baseline) * face.upem / face.descent)


CJK_CORE = ((0x3040, 0x30FF), (0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xAC00, 0xD7A3))  # kana, Han, Hangul syllables


def cjk_ink(face, gids, p):
    """Rows above and below the baseline of the hinted bitmaps at ppem p/64, 99.9th percentile."""
    face.ft.set_char_size(p, p, 72, 72)
    above, below = [], []
    for gid in gids:
        face.ft.load_glyph(gid, freetype.FT_LOAD_DEFAULT | LOAD_FLAGS[CONFIG["raster"]["load"]])
        box = face.ft.glyph.outline.get_cbox()
        if box.yMax > box.yMin:
            above.append(-(-box.yMax // 64))
            below.append(-(box.yMin // 64))
    return tuple(sorted(v)[len(v) * 999 // 1000] for v in (above, below)) if above else (0, 0)


def cjk_ppem(face, scalars, size, baseline, em):
    """The latin em, unless two cells of the ideographic advance or the line height need less: the largest ppem
    (in 1/64) at which the package's kana, Han and Hangul bitmaps fit the line, 99.9th percentile above plus below
    the baseline, so a few tall or deep glyphs do not shrink the rest (the build fits those one by one). Also the
    rows to raise every glyph by so that the family sits inside the line."""
    lh, cw = size["line_height"], size["cell_width"]
    core = [cp for cp in scalars if any(lo <= cp <= hi for lo, hi in CJK_CORE)] or list(scalars)
    gids = [g for g in map(face.gid, core) if g]
    ink = functools.cache(lambda p: cjk_ink(face, gids, p))
    cap = min(em, 2 * cw * face.upem / face.advance)
    hi = lo = round(cap * 64)
    while lo > 64 and sum(ink(lo)) > lh:
        hi, lo = lo, max(64, lo * 3 // 4)
    while hi - lo > 1:
        mid = (lo + hi) // 2
        lo, hi = (mid, hi) if sum(ink(mid)) <= lh else (lo, mid)
    above, below = ink(lo)
    return cap if lo == round(cap * 64) else lo / 64, max(0, below - lh + baseline) - max(0, above - baseline)


def raster(face, gid, ppem, box=None, width=0, height=0, snap=False):
    """Box glyphs map the design box `box` exactly onto a width x height cell area; `snap` rounds their vertices
    to whole pixels."""
    ft = face.ft
    if box:
        sx, sy = width / (box[2] - box[0]), height / (box[3] - box[1])
        ft.set_char_size(round(sx * face.upem * 64), round(sy * face.upem * 64), 72, 72)
        ft.set_transform(freetype.Matrix(0x10000, 0, 0, 0x10000),
                         freetype.Vector(round(-box[0] * sx * 64), round(-box[1] * sy * 64)))
        ft.load_glyph(gid, freetype.FT_LOAD_NO_HINTING)
        if snap:
            o = ft.glyph.outline._FT_Outline
            for i in range(o.n_points):
                o.points[i].x, o.points[i].y = (o.points[i].x + 32) & -64, (o.points[i].y + 32) & -64
        ft.set_transform(freetype.Matrix(0x10000, 0, 0, 0x10000), freetype.Vector(0, 0))
    else:
        ft.set_char_size(round(ppem * 64), round(ppem * 64), 72, 72)
        ft.load_glyph(gid, freetype.FT_LOAD_DEFAULT | LOAD_FLAGS[CONFIG["raster"]["load"]])
    ft.glyph.render(RENDER_MODES[CONFIG["raster"]["render"]])
    bm = ft.glyph.bitmap
    w, h, pitch, buf = bm.width, bm.rows, bm.pitch, bytes(bm.buffer)
    rows = [buf[y * pitch:y * pitch + w] for y in range(h)]
    top, left, advance = ft.glyph.bitmap_top, ft.glyph.bitmap_left, ft.glyph.advance.x / 64
    if not any(any(r) for r in rows):
        return None, left, top, advance
    return rows, left, top, advance


def pack_bitmap(rows, fmt):
    w = len(rows[0])
    if fmt == FMT_A8:
        return w, b"".join(rows)
    stride = (w + 1) // 2
    out = bytearray()
    for r in rows:
        n = [(a * 15 + 127) // 255 for a in r] + [0]
        out += bytes((n[x] << 4) | n[x + 1] for x in range(0, w, 2))
    return stride, bytes(out)


def build_package(name, pkg, profile, faces, scalars, ivd, tools, size, keep):
    role = pkg["role"]
    report = {"package": name, "role": role}
    regular = faces[pkg["faces"]["regular"]]
    cmap, seqs = plan_keys(profile, pkg, regular, scalars, ivd, report, keep)

    slot_keys, slot_of = [], {}
    for key in list(cmap.values()) + [v[1] for v in seqs.values()]:
        if key not in slot_of:
            slot_of[key] = len(slot_keys)
            slot_keys.append(key)

    latin_face = faces[CONFIG["packages"]["latin"]["faces"]["regular"]]
    baseline, underline, strike = size_metrics(latin_face, size)
    fmt = FMT_A8 if any(e.get("package") == name for e in CONFIG["a8"]) else FMT_A4
    lh, cw = size["line_height"], size["cell_width"]
    em = fit_ppem(latin_face, "latin", size, baseline)
    inst_ppem, lift = (cjk_ppem(regular, scalars, size, baseline, em) if role == "cjk"
                       else (fit_ppem(regular, role, size, baseline), 0))
    glyph_records, bitmaps = [], []
    clipped = notdef = 0
    for key in slot_keys:
        kind, v = key
        if kind == "cp":
            gid = regular.gid(v)
        elif kind == "gid":
            gid = v
        else:
            shaped = regular.shape(v, role)
            gid = shaped[0] if len(shaped) == 1 else 0
        if gid == 0:
            notdef += 1
        cells = key_cells(profile, role, key)
        cp = v if kind == "cp" else -1
        box = (regular.cell_box if 0x2500 <= cp <= 0x259F or 0x1FB00 <= cp <= 0x1FBFF
               else regular.bounds(gid) if 0xE0B0 <= cp <= 0xE0D7 and gid else None)
        ppem, own = inst_ppem, role == "symbols" and gid and not box
        fit = own or (role == "cjk" and gid and not box)  # shrunk or moved into its cells where it would be cut
        if own:  # each symbol scaled on its own: the latin em, shrunk until its ink fits its cells
            x0, y0, x1, y1 = regular.bounds(gid) or (0, 0, 1, 1)
            ppem = min(em, cells * cw * regular.upem / max(1, x1 - x0), lh * regular.upem / max(1, y1 - y0))
        snap = 0x1FB00 <= cp <= 0x1FB6F  # sextants, wedges and triangles: thirds and halves of the cell
        rows, left, top, advance = raster(regular, gid, ppem, box, cells * cw, lh, snap) if gid else (None, 0, 0, 0)
        while fit and rows and (len(rows[0]) > cells * cw or len(rows) > lh):
            ppem *= min(cells * cw / len(rows[0]), lh / len(rows)) * 0.98
            rows, left, top, advance = raster(regular, gid, ppem)
        if rows is None:
            glyph_records.append((0, 0, 0, 0, 0, 0, cells << 2, gid, b""))
            continue
        w, h = len(rows[0]), len(rows)
        if box:
            bx, top = left, baseline - (lh - top)
        else:
            bx = round((cells * cw - advance) / 2) + left
        if fit:
            bx, top = min(max(bx, 0), cells * cw - w), max(min(top + lift, baseline), baseline + h - lh)
        if not box and (bx < 0 or bx + w > cells * cw or baseline - top < 0 or baseline - top + h > lh):
            clipped += 1
        stride, data = pack_bitmap(rows, fmt)
        if not (-128 <= bx <= 127 and -128 <= top <= 127 and w < 256 and h < 256):
            sys.exit(f"{name}: glyph {key} exceeds record range")
        glyph_records.append((len(data), w, h, bx, top, stride, fmt | (cells << 2), gid, data))
        bitmaps.append(len(data))
    report.update(slots=len(slot_keys), clipped=clipped, notdef=notdef, bitmap_bytes=sum(bitmaps))
    if not slot_keys:
        sys.exit(f"{name}: the build has no glyphs")

    keys = [(k,) if isinstance(k, int) else k for k in cmap] + list(seqs)
    longest = max((len(k) for k in keys), default=0)
    longest_bytes = max((sum(ucd.utf8_len(c) for c in k) for k in keys), default=0)
    if longest > profile.max_scalars or longest_bytes > profile.max_bytes:
        sys.exit(f"{name}: key of {longest} scalars/{longest_bytes} bytes exceeds the profile limit")
    report.update(max_key_scalars=longest, max_key_bytes=longest_bytes)

    nglyphs = len(slot_keys)
    pages, payload = [], bytearray()
    page_limit = CONFIG["page_bytes"]
    g = 0
    while g < nglyphs:
        first, used = g, 4
        while g < nglyphs and (g == first or used + 16 + glyph_records[g][0] <= page_limit):
            used += 16 + glyph_records[g][0]
            g += 1
        count = g - first
        header = bytearray(struct.pack("<I", count))
        body = bytearray()
        base = 4 + 16 * count
        for length, w, h, bx, top, stride, flags, gid, data in glyph_records[first:g]:
            header += struct.pack("<IHBBbbBBHH", base + len(body) if length else 0, length, w, h, bx, top, stride,
                                  flags, gid & 0xFFFF, 0)
            body += data
        page = bytes(header + body)
        pages.append((len(payload), page, first, count))
        payload += page

    strings = bytearray()

    def s(text):
        b = text.encode()
        off = len(strings)
        strings.extend(b)
        return off, len(b)

    locale = pkg.get("locale", "").encode().ljust(8, b"\0")
    build = s(f"{VERSION}; freetype {tools['freetype']}; harfbuzz {tools['harfbuzz']}; "
              f"fonttools {tools['fonttools']}; raster {CONFIG['raster']['load']}/{CONFIG['raster']['render']}; "
              f"shaping {shaping_script(role)}/{SHAPING['language']}/{SHAPING['direction']}/default features")
    pname = s(f"shiroko-{name}")
    meta = struct.pack("<B3x8sIIIIII", ROLES[role], locale, *pname, *build, nglyphs, 1)
    src_rec = struct.pack("<II32sII", *s(regular.id), bytes.fromhex(LOCK["faces"][regular.id]["sha256"]),
                          *s(spdx_expression(LOCK["licenses"][pkg["license"]])))
    iid = hashlib.sha256(json.dumps([LOCK["faces"][regular.id]["sha256"], pkg.get("locale", ""), "regular",
                                     round(inst_ppem * 64), baseline, CONFIG["raster"]],
                                    sort_keys=True).encode()).digest()[:16]
    inst_rec = struct.pack("<HBBHHIhhhH16s12x", 0, 0, fmt, lh, cw, round(inst_ppem * 64), baseline, underline, strike,
                           1, iid)
    cmap_recs = b"".join(struct.pack("<II", cp, slot_of[k]) for cp, k in sorted(cmap.items()))
    pool, seq_recs = [], bytearray()
    for key in sorted(seqs):
        kind, target = seqs[key]
        seq_recs += struct.pack("<BBHII", len(key), kind, 0, len(pool), slot_of[target])
        pool.extend(key)
    seqpool = struct.pack(f"<{len(pool)}I", *pool)
    coverage = struct.pack("<9I", len(cmap), sum(1 for k in seqs.values() if k[0] == SEQ_GLYPH),
                           sum(1 for k in seqs.values() if k[0] == SEQ_ALIAS), report.get("uvs", 0),
                           report.get("ivs", 0), report.get("emoji_unsupported", 0), 0,
                           len(glyph_records), sum(bitmaps))
    sections = [("MANIFEST", 1, meta), ("STRINGS", len(strings), bytes(strings)), ("SOURCES", 1, src_rec),
                ("INSTANCES", 1, inst_rec), ("CMAP", len(cmap), cmap_recs), ("SEQS", len(seqs), bytes(seq_recs)),
                ("SEQPOOL", len(pool), seqpool),
                ("PAGES", len(pages), None), ("COVERAGE", 9, coverage)]
    table_off = HEADER_SIZE
    off = table_off + ENTRY_SIZE * len(sections)
    layout = []
    for sname, count, data in sections:
        length = ENTRY_SIZE * len(pages) if data is None else len(data)
        layout.append([sname, count, off, length, data])
        off += (length + 7) & ~7
    page_base = off
    page_recs = b"".join(struct.pack("<QQIII4x", page_base + o, xxh3(p), len(p), first, count)
                         for o, p, first, count in pages)
    next(e for e in layout if e[0] == "PAGES")[4] = page_recs
    body = bytearray(off - HEADER_SIZE)
    table = bytearray()
    for sname, count, soff, length, data in layout:
        table += struct.pack("<IIQI4xQ", SECTION[sname], count, soff, length, xxh3(data))
        body[soff - HEADER_SIZE:soff - HEADER_SIZE + length] = data
    body[0:len(table)] = table
    body += payload
    features = (FEAT_SEQ if seqs else 0) | (FEAT_A4 if fmt == FMT_A4 else FEAT_A8)
    file_size = HEADER_SIZE + len(body)
    head = struct.pack("<8sHHIIIQQ32s", b"SHRFPKG1", FORMAT_VERSION, HEADER_SIZE, features, len(sections), 0,
                       table_off, file_size, profile.id)
    head += struct.pack("<Q", xxh3(head)) + bytes(HEADER_SIZE - 80)
    blob = head + bytes(body)
    report.update(file_bytes=len(blob), pages=len(pages), index_bytes=page_base,
                  package_id=sha256(blob), cmap=len(cmap), sequences=len(seqs))
    return blob, report


def spdx_expression(lic):
    """Conjunction of the licence and every component licence."""
    ids = set(lic["spdx"].split(" AND ")) | {c["spdx"] for c in lic.get("components", [])}
    return " AND ".join(sorted(ids))


def unserved(profile, packages, faces, providers):
    """Scalars of each package's source cmap that the profile routes to it but no package serves.

    Nerd and emoji have dedicated routes (Nerd class, emoji presentation) and must serve them themselves;
    the other roles share the text fallback chain, so any package of the chain may serve their scalars."""
    chain = set().union(*(providers[n] for n, p in packages.items() if p["role"] != "emoji"))
    out = {}
    for name, p in packages.items():
        role = p["role"]
        cmap = {cp for cp in faces[p["faces"]["regular"]].cmap if profile.drawable(cp)}
        if role == "nerd":
            missing = {cp for cp in cmap if profile.has(cp, ucd.F_NERD)} - providers[name]
        elif role == "emoji":
            missing = {cp for cp in cmap if profile.has(cp, ucd.F_EPRES)} - providers[name]
        else:
            missing = cmap - chain
        allowed = {int(cp, 16) for cp in CONFIG.get("coverage_allowlist", {}).get(name, {}).get("scalars", [])}
        out[name] = (sorted(missing - allowed), sorted(missing & allowed))
    return out


class Package:
    """Reader for `verify`, `install`, `build` and previews. Raises FormatError unless the package passes the
    runtime loader's rules, every page and glyph record included, and matches the given text profile."""

    def __init__(self, blob, profile_id, max_scalars):
        self.blob = blob
        check(len(blob) >= HEADER_SIZE, "shorter than a header")
        magic, ver, hsize, feat, nsec, res, toff, fsize, self.profile, digest = struct.unpack_from(
            "<8sHHIIIQQ32sQ", blob)
        check(magic == b"SHRFPKG1" and ver == FORMAT_VERSION and hsize == HEADER_SIZE, "bad magic or version")
        check(digest == xxh3(blob[:72]), "header checksum")
        check(feat & ~7 == 0, "unsupported required feature")
        check(not any(blob[80:HEADER_SIZE]) and res == 0 and 1 <= nsec <= 16, "reserved bytes or section count")
        check(fsize == len(blob) and HEADER_SIZE <= toff <= fsize and fsize - toff >= ENTRY_SIZE * nsec, "file size")
        check(self.profile == profile_id, "text profile mismatch")
        entries = sorted((struct.unpack_from("<IIQIIQ", blob, toff + ENTRY_SIZE * i) for i in range(nsec)),
                         key=lambda e: e[2])
        sec, end = {}, toff + ENTRY_SIZE * nsec
        for t, count, off, length, zero, digest in entries:
            check(1 <= t <= 10 and t != 5 and t not in sec, "unknown or duplicate section")
            check(off >= end and off + length <= fsize, "overlapping sections or out of range")
            data = blob[off:off + length]
            check(zero == 0 and xxh3(data) == digest and count <= 1 << 22, "section checksum or record count")
            sec[t] = (count, data)
            end = off + length
        check(end - toff <= 128 << 20, "package index too large")
        check(all(t in sec for t in (1, 4, 6, 9)), "missing section")
        strings = len(sec.get(2, (0, b""))[1])
        check(sec[1][0] == 1 and len(sec[1][1]) == 36, "MANIFEST")
        (self.role, pad, self.locale, name_off, name_len, build_off, build_len, self.nglyphs,
         self.ninst) = struct.unpack("<B3s8sIIIIII", sec[1][1])
        check(1 <= self.role <= 5 and pad == bytes(3), "role")
        check(all(c == 0 or 0x20 <= c <= 0x7E for c in self.locale), "locale")
        check(name_off + name_len <= strings and build_off + build_len <= strings, "string reference")
        check(1 <= self.nglyphs <= 1 << 22 and self.ninst == 1, "glyph or instance count")
        check(sec[4][0] == 1 and len(sec[4][1]) == 48, "INSTANCES size")
        sources = sec.get(3, (0, b""))
        check(len(sources[1]) == 48 * sources[0], "SOURCES size")
        for name_off, name_len, _, lic_off, lic_len in struct.iter_unpack("<II32sII", sources[1]):
            check(name_off + name_len <= strings and lic_off + lic_len <= strings, "source string reference")
        self.inst = struct.unpack("<HBBHHIhhhH16s12s", sec[4][1])
        src, style, fmt, lh, cw, _, base, under, strike, raster, _, reserved = self.inst
        check(3 not in sec or src < sources[0], "instance source")
        check(style == 0 and fmt in (FMT_A4, FMT_A8) and feat & fmt, "instance style or format")
        check(1 <= lh <= 1024 and 1 <= cw <= 1024, "instance size")
        check(0 <= base <= lh and 0 <= under < lh and 0 <= strike < lh, "instance line metrics")
        check(raster & ~1 == 0 and reserved == bytes(12), "instance reserved bytes")
        check(len(sec[6][1]) == 8 * sec[6][0], "CMAP size")
        self.cmap = dict(struct.iter_unpack("<II", sec[6][1]))
        keys = list(self.cmap)
        check(len(keys) == sec[6][0] and keys == sorted(keys), "cmap order")
        check(all(cp <= 0x10FFFF and not 0xD800 <= cp <= 0xDFFF for cp in keys), "cmap record")
        check(all(slot < self.nglyphs for slot in self.cmap.values()), "cmap record")
        pool_count, pool_data = sec.get(8, (0, b""))
        check(len(pool_data) == 4 * pool_count, "SEQPOOL size")
        pool = struct.unpack(f"<{pool_count}I", pool_data)
        self.seqs = {}
        seq_count, seq_data = sec.get(7, (0, b""))
        if seq_count:
            check(feat & FEAT_SEQ and 8 in sec and len(seq_data) == 12 * seq_count, "SEQS size")
            check(all(cp <= 0x10FFFF and not 0xD800 <= cp <= 0xDFFF for cp in pool), "sequence scalar")
            prev = None
            for length, kind, zero, idx, slot in struct.iter_unpack("<BBHII", seq_data):
                check(2 <= length <= max_scalars and 1 <= kind <= 4 and zero == 0 and idx + length <= pool_count
                      and slot < self.nglyphs, "sequence record")
                key = pool[idx:idx + length]
                check(prev is None or key > prev, "sequence order")
                prev = key
                self.seqs[key] = (kind, slot)
        check(1 <= sec[9][0] <= 1 << 16 and len(sec[9][1]) == ENTRY_SIZE * sec[9][0], "PAGES size")
        self.pages = list(struct.iter_unpack("<QQIIII", sec[9][1]))
        expect, prev_end = 0, end
        for off, digest, length, first, count, zero in self.pages:
            check(first == expect and count and zero == 0 and 4 + 16 * count <= length <= 1 << 20
                  and off >= prev_end and off + length <= fsize, "page record")
            self.check_page(blob[off:off + length], digest, first, count)
            expect += count
            prev_end = off + length
        check(expect == self.nglyphs, "pages do not cover every glyph")
        check(10 not in sec or (sec[10][0] == 9 and len(sec[10][1]) == 36), "COVERAGE size")

    def check_page(self, page, digest, first, count):
        check(xxh3(page) == digest and struct.unpack_from("<I", page)[0] == count, "page checksum or count")
        for g in range(count):
            off, length, w, h, _, _, stride, flags, _, zero = struct.unpack_from("<IHBBbbBBHH", page, 4 + 16 * g)
            fmt, cells = flags & 3, flags >> 2 & 3
            check(1 <= cells <= 2 and flags & 0xF0 == 0 and zero == 0, f"glyph {first + g} flags")
            if not fmt:
                check(length == w == h == 0, f"glyph {first + g} without bitmap")
                continue
            row = (w + 1) // 2 if fmt == FMT_A4 else w
            check(fmt == self.inst[2] and w and h and stride >= row
                  and (h - 1) * stride + row <= length and off >= 4 + 16 * count and off + length <= len(page),
                  f"glyph {first + g} bitmap")
            check(fmt == FMT_A8 or w % 2 == 0 or not any(page[off + y * stride + w // 2] & 15 for y in range(h)),
                  f"glyph {first + g} A4 padding")

    def glyph(self, slot):
        for off, _, length, first, count, _ in self.pages:
            if first <= slot < first + count:
                page = self.blob[off:off + length]
                boff, blen, w, h, bx, top, stride, flags, _, _ = struct.unpack_from("<IHBBbbBBHH", page,
                                                                                      4 + 16 * (slot - first))
                return flags & 3, w, h, bx, top, stride, flags, page[boff:boff + blen]
        raise KeyError(slot)


def pinned_profile():
    """Profile id and cluster limit of the runtime: from $SHIROKO_UNICODE_TABLES (the build's generated tables),
    else computed from the Unicode data."""
    if not UNICODE_C:
        lock = ucd.load_lock()
        ucd.verify_inputs(lock)
        _, _, starts, values, seqs, vs_bases, _ = ucd.tables(lock)
        return bytes.fromhex(ucd.text_profile_id(lock, starts, values, seqs, vs_bases)), lock["cluster_max_scalars"]
    text = pathlib.Path(UNICODE_C).read_text(encoding="utf-8")
    pid = re.search(r"shr__text_profile_id\[32\] = \{([^}]*)\}", text).group(1)
    scalars = re.search(r"shr__cluster_max_scalars = (\d+);", text).group(1)
    return bytes(int(x, 16) for x in pid.split(",")), int(scalars)


def activation_record(generation, package_id, size):
    rec = struct.pack("<8sQ32sQ", b"SHRFACT2", generation, package_id, size)
    return rec + struct.pack("<Q", xxh3(rec))


def activation_generation(rec):
    """Generation of a valid 64-byte record, else -1."""
    if len(rec) != 64 or rec[:8] != b"SHRFACT2" or struct.unpack_from("<Q", rec, 56)[0] != xxh3(rec[:56]):
        return -1
    return struct.unpack_from("<Q", rec, 8)[0]


def cell_fit(pkg):
    """Box/block and Powerline glyphs must reach the cell edges (triangle tips may be partial)."""
    out = {}
    for cp in (0x2588, 0x2502, 0x2500, 0xE0B0, 0xE0B2):
        if cp not in pkg.cmap:
            continue
        lh, cw, base = pkg.inst[3], pkg.inst[4], pkg.inst[6]
        fmt, w, h, bx, top, stride, flags, data = pkg.glyph(pkg.cmap[cp])
        cov = [[0] * cw for _ in range(lh)]
        for y in range(h):
            for x in range(w):
                a = (data[y * stride + x // 2] >> (4 if x % 2 == 0 else 0) & 15) * 17 if fmt == FMT_A4 else data[y * stride + x]
                px, py = bx + x, base - top + y
                if 0 <= px < cw and 0 <= py < lh:
                    cov[py][px] = a
        need = {0x2588: [(x, y) for y in range(lh) for x in range(cw)],
                0x2502: [(cw // 2 - 1 + (cw % 2), y) for y in (0, lh - 1)],
                0x2500: [(x, lh // 2) for x in (0, cw - 1)],
                0xE0B0: [(0, y) for y in range(lh)], 0xE0B2: [(cw - 1, y) for y in range(lh)]}[cp]
        if cp == 0x2588:
            ok = all(cov[y][x] == 255 for x, y in need)
        elif cp in (0xE0B0, 0xE0B2):
            ok = all(cov[y][x] and (cov[y][x] == 255 or y in (0, lh - 1)) for x, y in need)
        else:
            ok = all(any(cov[y][xx] for xx in range(cw)) and any(cov[yy][x] for yy in range(lh)) for x, y in need)
        out[f"U+{cp:04X}"] = ok
        if not ok:
            print(f"warning: U+{cp:04X} does not fill its cell edges")
    return out


def preview(pkg, path, report):
    step = max(1, pkg.nglyphs // 192)
    lh, cw, baseline = pkg.inst[3], pkg.inst[4], pkg.inst[6]
    slots = list(range(0, pkg.nglyphs, step))[:192]
    cols = 32
    img = Image.new("L", (cols * 2 * cw + 8, ((len(slots) + cols - 1) // cols) * lh + 8), 0)
    for n, slot in enumerate(slots):
        fmt, w, h, bx, top, stride, flags, data = pkg.glyph(slot)
        ox, oy = 4 + (n % cols) * 2 * cw, 4 + (n // cols) * lh
        for y in range(h):
            for x in range(w):
                a = (data[y * stride + x // 2] >> (4 if x % 2 == 0 else 0) & 15) * 17 if fmt == FMT_A4 else \
                    data[y * stride + x] if fmt == FMT_A8 else 0
                px, py = ox + bx + x, oy + baseline - top + y
                if 0 <= px < img.width and 0 <= py < img.height:
                    img.putpixel((px, py), max(img.getpixel((px, py)), a))
    img.resize((img.width * 2, img.height * 2), Image.NEAREST).save(path)
    report["preview"] = path.name


def unverified_components(names):
    return [(name, c) for name in names for c in LOCK["licenses"][CONFIG["packages"][name]["license"]].get("components", [])
            if not c["verified"]]


def write_licenses(out, names, reports):
    lic_dir = out / "LICENSES"
    lic_dir.mkdir(parents=True, exist_ok=True)
    inventory = {"packages": {}, "sources": {}, "licenses": {}}
    notice = ["Shiroko font packages", "", "Bitmap subsets rasterised from the fonts below (modified: subset, "
              "rasterised to A4/A8 coverage bitmaps, repacked). Package names do not use any Reserved Font Name.", ""]

    def provenance(key, files):
        out_files = []
        for f in files:
            dest = lic_dir / key / pathlib.Path(f).name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes((SRC / f).read_bytes())
            out_files.append({"path": str(dest.relative_to(out)), "url": LOCK["files"][f]["url"],
                              "sha256": LOCK["files"][f]["sha256"]})
        return out_files

    for name in names:
        pkg = CONFIG["packages"][name]
        key = pkg["license"]
        lic = LOCK["licenses"][key]
        rfn = [r for r in lic.get("rfn", []) + [x for c in lic.get("components", []) for x in c.get("rfn", [])]]
        if any(r.lower() in f"shiroko-{name}".lower() for r in rfn):
            sys.exit(f"package name shiroko-{name} uses a Reserved Font Name")
        if key not in inventory["licenses"]:
            entry = {"spdx": lic["spdx"], "expression": spdx_expression(lic), "notes": lic.get("notes"),
                     "files": provenance(key, lic["files"]), "components": []}
            for c in lic.get("components", []):
                entry["components"].append({"name": c["name"], "spdx": c["spdx"], "verified": c["verified"],
                                            "notes": c.get("notes"), "files": provenance(key, c["files"])})
            for spdx in sorted(set(spdx_expression(lic).split(" AND "))):
                text = ROOT / "LICENSES" / f"{spdx}.txt"
                if text.exists():
                    (lic_dir / key / text.name).write_bytes(text.read_bytes())
            inventory["licenses"][key] = entry
        face_id = pkg["faces"]["regular"]
        face = LOCK["faces"][face_id]
        copyright_ = TTFont(io.BytesIO(face_bytes(face_id)), lazy=True)["name"].getDebugName(0) or ""
        inventory["sources"][face_id] = {"file": face["file"], "member": face.get("member"),
                                         "url": LOCK["files"][face["file"]]["url"], "sha256": face["sha256"],
                                         "license": spdx_expression(lic), "copyright": copyright_,
                                         "axes": face.get("axes")}
        notice.append(f"{face_id}: {copyright_} License: {spdx_expression(lic)} (LICENSES/{key}/)")
        notice += [f"  {c['name']} ({c['spdx']}): {c['notes']}" for c in lic.get("components", [])
                   if "notes" in c]
        inventory["packages"][f"shiroko-{name}"] = {"license": key, "spdx": spdx_expression(lic), "rfn_checked": rfn,
                                                    "report": reports[name]}
    (lic_dir / "Unicode-3.0.txt").write_bytes((ROOT / "LICENSES/Unicode-3.0.txt").read_bytes())
    notice += ["", "Unicode data: Copyright (c) Unicode, Inc. Unicode License v3 (LICENSES/Unicode-3.0.txt).", ""]
    (out / "NOTICE").write_text("\n".join(notice) + "\n", encoding="utf-8")
    (out / "inventory.json").write_text(json.dumps(inventory, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def publish(stage, out):
    """Moves every staged output over `out`; each file is replaced atomically."""
    for entry in sorted(stage.iterdir()):
        target = out / entry.name
        if entry.is_dir():
            old = out / f".old-{entry.name}-{os.getpid()}"
            if target.exists():
                os.replace(target, old)
            os.replace(entry, target)
            shutil.rmtree(old, ignore_errors=True)
        else:
            os.replace(entry, target)


def build(names, size, out=None, keep=None):
    out = pathlib.Path(out).resolve() if out else OUT
    names = names or CONFIG["default"]
    unverified = unverified_components(names)
    if unverified:
        sys.exit("unverified licence components: " + ", ".join(f"{n}: {c['name']} ({c['spdx']})" for n, c in unverified))
    tools = fetch(False)
    profile = Profile()
    ivd = ivd_pairs()
    resolve_set = CONFIG["packages"]
    faces = {f: Face(f) for f in {p["faces"]["regular"] for p in resolve_set.values()}}
    providers = resolve_providers(profile, resolve_set, faces)
    gaps = unserved(profile, resolve_set, faces, providers) if keep is None else {}
    out.mkdir(parents=True, exist_ok=True)
    stage = pathlib.Path(tempfile.mkdtemp(prefix=".stage-", dir=out))
    try:
        reports = {}
        for name in names:
            blob, report = build_package(name, CONFIG["packages"][name], profile, faces, providers[name], ivd, tools,
                                         size, keep)
            try:
                pkg = Package(blob, profile.id, profile.max_scalars)
            except FormatError as e:
                sys.exit(f"{name}: built package is invalid: {e}")
            (stage / f"shiroko-{name}.shrf").write_bytes(blob)
            report["cell_fit"] = cell_fit(pkg)
            if name in gaps:
                missing, allowed = gaps[name]
                report["unserved"] = [f"{cp:04X}" for cp in missing]
                report["unserved_allowed"] = [f"{cp:04X}" for cp in allowed]
            preview(pkg, stage / f"shiroko-{name}.png", report)
            reports[name] = report
            print(f"{out / f'shiroko-{name}.shrf'}: {report['file_bytes']} bytes, {report['cmap']} scalars, "
                  f"{report['sequences']} sequences, {report['pages']} pages, clipped {report['clipped']}")
        write_licenses(stage, names, reports)
        (stage / "coverage.json").write_text(json.dumps(reports, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        bad = {n: r["unserved"] for n, r in reports.items() if r.get("unserved")}
        if bad:
            sys.exit("source cmap scalars routed to a package but served by none (see coverage_allowlist): "
                     + "; ".join(f"{n}: {' '.join(v)}" for n, v in bad.items()))
        publish(stage, out)
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def builtin(size, out):
    tools = fetch(False)
    latin = CONFIG["packages"]["latin"]
    face_id = latin["faces"]["regular"]
    face = Face(face_id)
    cps = set(range(0x20, 0x7F)) | {0xFFFD}
    if not cps <= set(face.cmap):
        sys.exit(f"{face_id} does not cover the built-in scalars")
    profile = Profile()
    pkg = {"role": "latin", "license": latin["license"], "faces": {"regular": face_id}}
    blob, _ = build_package("builtin", pkg, profile, {face_id: face}, cps, set(), tools, size, cps)
    try:
        Package(blob, profile.id, profile.max_scalars)
    except FormatError as e:
        sys.exit(f"builtin: built package is invalid: {e}")
    lines = [f"/* Generated by tools/fontpack/fontpack.py builtin from {face_id} "
             f"({LOCK['faces'][face_id]['sha256'][:16]}), {spdx_expression(LOCK['licenses'][latin['license']])}. */",
             "#include <stddef.h>", "#include <stdint.h>", "",
             f"_Alignas(8) const uint8_t shr__builtin_package[{len(blob)}] = {{"]
    lines += ["  " + ", ".join(f"0x{b:02X}" for b in blob[i:i + 16]) + "," for i in range(0, len(blob), 16)]
    lines += ["};", f"const size_t shr__builtin_package_size = {len(blob)};"]
    out = pathlib.Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    write_atomic(out, ("\n".join(lines) + "\n").encode())
    print(f"{out}: built-in package {size['cell_width']}x{size['line_height']}, {len(blob)} bytes")


def fsync_dir(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_synced(path, data):
    with open(path, "wb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())


def replace_synced(path, data):
    tmp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    write_synced(tmp, data)
    if tmp.read_bytes() != data:
        sys.exit(f"{tmp}: read-back mismatch")
    os.replace(tmp, path)
    fsync_dir(path.parent)


def verified(path, profile):
    blob = pathlib.Path(path).read_bytes()
    try:
        return Package(blob, *profile)
    except FormatError as e:
        sys.exit(f"{path}: {e}")


def named(path, pkg):
    """The runtime opens shiroko-<role>.shrf, or shiroko-cjk-<locale>.shrf, and rejects any other manifest."""
    m = re.fullmatch(r"shiroko-(?:(latin|symbols|emoji|nerd)|cjk-(.{1,8}))", pathlib.Path(path).stem)
    if not m or pkg.role != ROLES[m[1] or "cjk"] or (m[2] and pkg.locale != m[2].encode().ljust(8, b"\0")):
        sys.exit(f"{path}: package role or locale differs from its file name")
    return pkg


def active_packages(dest):
    """Package id of each package with a valid activation record in dest, by name: the higher generation, slot a
    on a tie (as shr_pl_res_bitmap_font_activation_select() picks)."""
    best = {}
    for slot in sorted(dest.glob("shiroko-*.act.[ab]")):
        rec = slot.read_bytes()
        name, gen = slot.name.split(".act.")[0], activation_generation(rec)
        if gen >= 0 and gen > best.get(name, (-1, b""))[0]:
            best[name] = (gen, rec[16:48])
    return {name: pid for name, (_, pid) in best.items()}


def install_notices(src, dest, packages):
    """Ships the notices of the packages active after the install: these and those already active in dest, which
    the source inventory must describe as well. inventory.json lists exactly those packages, their licences and
    source faces; NOTICE and LICENSES/ are copied whole from the build and may attribute more."""
    try:
        inventory = json.loads((src / "inventory.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        sys.exit(f"{src}: no licence inventory next to the packages ({e})")
    active = active_packages(dest) | dict(packages)
    for name, pid in active.items():
        entry = inventory.get("packages", {}).get(name)
        if not entry or entry.get("report", {}).get("package_id") != pid.hex():
            sys.exit(f"{src / 'inventory.json'} does not describe {name} ({pid.hex()}); "
                     "install every active package from one build")
    if not (src / "NOTICE").is_file() or not (src / "LICENSES").is_dir():
        sys.exit(f"{src}: NOTICE or LICENSES/ missing next to the packages")
    tmp = dest / f".LICENSES.{os.getpid()}.tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    shutil.copytree(src / "LICENSES", tmp)
    old = dest / f".LICENSES.{os.getpid()}.old"
    if (dest / "LICENSES").exists():
        os.replace(dest / "LICENSES", old)
    os.replace(tmp, dest / "LICENSES")
    shutil.rmtree(old, ignore_errors=True)
    replace_synced(dest / "NOTICE", (src / "NOTICE").read_bytes())
    names = set(active)
    inventory["packages"] = {k: v for k, v in inventory["packages"].items() if k in names}
    used = {v["license"] for v in inventory["packages"].values()}
    inventory["licenses"] = {k: v for k, v in inventory.get("licenses", {}).items() if k in used}
    unknown = sorted(n for n in names if n.removeprefix("shiroko-") not in CONFIG["packages"])
    if unknown:  # the inventory does not record each package's faces
        sys.exit(f"{', '.join(unknown)} not in {FONTS / 'fontpack.config.json'}: cannot tell the source faces to list")
    faces = {CONFIG["packages"][n.removeprefix("shiroko-")]["faces"]["regular"] for n in names}
    inventory["sources"] = {k: v for k, v in inventory.get("sources", {}).items() if k in faces}
    replace_synced(dest / "inventory.json", (json.dumps(inventory, indent=2, ensure_ascii=False) + "\n").encode())


def install(dest, files):
    dest = pathlib.Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    profile = pinned_profile()
    dirs = {pathlib.Path(f).resolve().parent for f in files}
    if len(dirs) != 1:
        sys.exit("install packages from one build directory at a time")
    blobs = [(pathlib.Path(f).stem, named(f, verified(f, profile)).blob) for f in files]
    install_notices(dirs.pop(), dest, [(name, hashlib.sha256(blob).digest()) for name, blob in blobs])
    for name, blob in blobs:
        pid = hashlib.sha256(blob).digest()
        final = dest / f"{pid.hex()}.shrf"
        # An existing payload is trusted only if it is byte-identical to the verified package.
        if not (final.is_file() and final.stat().st_size == len(blob) and sha256(final.read_bytes()) == sha256(blob)):
            replace_synced(final, blob)
        slots = [dest / f"{name}.act.a", dest / f"{name}.act.b"]
        gens = [activation_generation(s.read_bytes() if s.exists() else b"") for s in slots]
        target = slots[0] if gens[0] <= gens[1] else slots[1]
        rec = activation_record(max(gens) + 1, pid, len(blob))
        if activation_generation(rec) != max(gens) + 1:
            sys.exit("activation record does not round-trip")
        created = not target.exists()
        write_synced(target, rec)
        if created:
            fsync_dir(dest)
        print(f"{name}: generation {max(gens) + 1} -> {final.name} ({target.name})")


def selftest(packages):
    """Install recovery: a damaged payload is rewritten, the notices follow, activation advances."""
    src = min(pathlib.Path(packages).glob("shiroko-*.shrf"), key=lambda p: p.stat().st_size, default=None)
    if not src:
        sys.exit(f"{packages}: no packages to test with; run `make fontpack` first")
    blob = src.read_bytes()
    pid = sha256(blob)
    with tempfile.TemporaryDirectory() as tmp:
        dest = pathlib.Path(tmp) / "dest"
        install(dest, [src])
        final = dest / f"{pid}.shrf"
        for damage in (lambda b: b[:-1], lambda b: b[:200] + bytes([b[200] ^ 1]) + b[201:], lambda b: b""):
            final.write_bytes(damage(blob))
            install(dest, [src])
            if final.read_bytes() != blob:
                sys.exit("selftest: a damaged payload survived install")
        gens = [activation_generation((dest / f"{src.stem}.act.{s}").read_bytes()) for s in "ab"]
        if sorted(gens) != [2, 3]:
            sys.exit(f"selftest: unexpected activation generations {gens}")
        if not ((dest / "NOTICE").is_file() and (dest / "inventory.json").is_file() and (dest / "LICENSES").is_dir()):
            sys.exit("selftest: notices were not installed")
        if list(json.loads((dest / "inventory.json").read_text(encoding="utf-8"))["packages"]) != [src.stem]:
            sys.exit("selftest: the installed inventory lists packages that were not installed")
        other = next(p for p in sorted(pathlib.Path(packages).glob("shiroko-*.shrf")) if p != src)
        install(dest, [other])
        install(dest, [src])
        if set(json.loads((dest / "inventory.json").read_text(encoding="utf-8"))["packages"]) != {src.stem, other.stem}:
            sys.exit("selftest: reinstalling one package dropped another active package from the inventory")
        lone = pathlib.Path(tmp) / "lone"
        lone.mkdir()
        shutil.copyfile(src, lone / src.name)
        try:
            install(pathlib.Path(tmp) / "dest2", [lone / src.name])
            sys.exit("selftest: install without a licence inventory succeeded")
        except SystemExit as e:
            if "inventory" not in str(e.code):
                raise
        shutil.copyfile(src, lone / "shiroko-cjk-xx.shrf")
        try:
            install(pathlib.Path(tmp) / "dest3", [lone / "shiroko-cjk-xx.shrf"])
            sys.exit("selftest: a package installed under another role's or locale's name")
        except SystemExit as e:
            if "file name" not in str(e.code):
                raise
    print("fontpack selftest: ok")


def cell(value):
    m = re.fullmatch(r"([1-9]\d{0,2})x([1-9]\d{0,2})", value)
    if not m:
        raise argparse.ArgumentTypeError(f"cell size {value!r} is not WIDTHxHEIGHT")
    if int(m[1]) > MAX_CELL[0] or int(m[2]) > MAX_CELL[1]:
        raise argparse.ArgumentTypeError(f"cell size {value!r} exceeds {MAX_CELL[0]}x{MAX_CELL[1]}, the largest a "
                                         "glyph record can hold")
    return {"cell_width": int(m[1]), "line_height": int(m[2])}


def scalar_set(value):
    keep = set()
    for part in value.split(","):
        m = re.fullmatch(r"([0-9A-Fa-f]{1,6})(?:-([0-9A-Fa-f]{1,6}))?", part.strip())
        if not m or int(m[2] or m[1], 16) < int(m[1], 16) or int(m[2] or m[1], 16) > 0x10FFFF:
            raise argparse.ArgumentTypeError(f"bad scalar range {part!r}: use hex like 0041-005A,AC00")
        keep.update(range(int(m[1], 16), int(m[2] or m[1], 16) + 1))
    return keep


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("fetch")
    b = sub.add_parser("build")
    b.add_argument("packages", nargs="*")
    b.add_argument("--cell", type=cell, required=True)
    b.add_argument("--out")
    b.add_argument("--scalars", type=scalar_set)
    v = sub.add_parser("verify")
    v.add_argument("files", nargs="+")
    bi = sub.add_parser("builtin")
    bi.add_argument("--cell", type=cell, required=True)
    bi.add_argument("--out", required=True)
    i = sub.add_parser("install")
    i.add_argument("--dest", required=True)
    i.add_argument("files", nargs="+")
    t = sub.add_parser("selftest")
    t.add_argument("--packages", default=str(OUT))
    args = ap.parse_args()
    unknown = [n for n in args.packages if n not in CONFIG["packages"]] if args.cmd == "build" else []
    if unknown:
        b.error(f"unknown package {', '.join(unknown)} (known: {', '.join(sorted(CONFIG['packages']))})")
    check_locks()
    if args.cmd == "fetch":
        fetch()
    elif args.cmd == "build":
        build(args.packages, args.cell, args.out, args.scalars)
    elif args.cmd == "builtin":
        builtin(args.cell, args.out)
    elif args.cmd == "verify":
        profile = pinned_profile()
        for f in args.files:
            pkg = verified(f, profile)
            print(f"{f}: ok ({pkg.nglyphs} slots, {len(pkg.pages)} pages)")
    elif args.cmd == "selftest":
        selftest(args.packages)
    else:
        install(args.dest, args.files)


if __name__ == "__main__":
    main()
