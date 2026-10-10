#!/usr/bin/env python3
"""Build, verify and install Shiroko bitmap font packages (format v6, docs/font-package-format.md).

  fontpack.py fetch                 download pinned inputs into $SHIROKO_FONT_CACHE/src (.cache/fonts/src)
  fontpack.py build --cell WxH [PACKAGE ...]   build packages (default set) for one cell size into build/fonts;
      --out DIR --scalars 0041-005A,AC00       never downloads; --scalars builds reduced packages (fuzz seeds),
      --page-atlas WxH                         --page-atlas forces one page shape for both formats,
      --method zstd|stored --store-index       --method stored stores every box, --store-index ctri/seqs/pool,
      --order hot|codepoint --rank FILE        --order places glyphs hot tiers first (default) or by scalar,
                                               --rank orders each tier by first use in a UTF-8 text file
                                               instead of the fetched frequencies (fonts.lock.json "order")
  fontpack.py builtin --cell WxH --out FILE.c  write the built-in package (ASCII + U+FFFD, stored) as a C array
      [--page-atlas WxH]
  fontpack.py verify FILE ...       check every box, page and glyph of packages
  fontpack.py install --dest DIR FILE ...   verify, stage, then switch activation; ships NOTICE,
                                    LICENSES/ and inventory.json from the packages' build directory
  fontpack.py selftest              exercise the reader's rejections and install recovery
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
import zstandard
from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools" / "unicode"), str(ROOT / "tools" / "fontpack")]
import gen_unicode_tables as ucd  # noqa: E402
import nerd_rules  # noqa: E402
import sprite  # noqa: E402

FONTS = ROOT / "fonts"
SRC = pathlib.Path(os.environ.get("SHIROKO_FONT_CACHE") or ROOT / ".cache" / "fonts") / "src"
WORK = pathlib.Path(os.environ.get("SHIROKO_FONT_WORK") or ROOT / ".cache" / "fonts" / "work")
OUT = ROOT / "build" / "fonts"
LOCK = json.loads((FONTS / "fonts.lock.json").read_text(encoding="utf-8"))
CONFIG = json.loads((FONTS / "fontpack.config.json").read_text(encoding="utf-8"))
UNICODE_C = os.environ.get("SHIROKO_UNICODE_TABLES")
VERSION = "fontpack 6"
FORMAT_VERSION = 6
LOAD_FLAGS = {"target_light": freetype.FT_LOAD_TARGET_LIGHT, "target_normal": freetype.FT_LOAD_TARGET_NORMAL}
RENDER_MODES = {"normal": freetype.FT_RENDER_MODE_NORMAL, "light": freetype.FT_RENDER_MODE_LIGHT}

ROLES = {"latin": 1, "cjk": 2, "symbols": 3, "emoji": 4, "nerd": 5}
FMT_A4, FMT_A8 = 1, 2
SEQ_GLYPH, SEQ_ALIAS, SEQ_UVS_DEFAULT, SEQ_UVS_ALT = 1, 2, 3, 4
FEAT_A4, FEAT_A8, FEAT_SEQ, FEAT_ZSTD = 1, 2, 4, 8
HEADER_SIZE = 128
BOX = struct.Struct("<I4sHHI")  # size, type, version, flags, raw_size
HEAD = struct.Struct("<I4sHHIHHIIIQQ32sQ32xQ")
ENTRY = struct.Struct("<4sHHQIIQ")  # sidx entry: type, flags, version, offset, size, raw_size, xxh3
PTAB = struct.Struct("<QQIHH")  # offset, xxh3, size, glyph_count, height
RECORD = struct.Struct("<HHBBbbBBHI")
REQUIRED, STORED, ZSTD = 1, 0, 1
MAGIC = b"\x80\0\0\0shrf"
META = (b"mani", b"strs", b"srcs", b"inst", b"ctri", b"seqs", b"pool", b"ptab", b"covr")
PACKED = (b"ctri", b"seqs", b"pool")  # the metadata boxes that may be zstd
MAX_INDEX = 32
PAGE_ALIGN = 256  # stored atlases, so a mapped page can be a driver buffer in place
CTRI_ALIGN = 64  # stored ctri data blocks
CTRI_L2 = 2192  # ctri: header and L1 (1088 u16) before the level-2 blocks
MISS, BLANK = 0xFFFFFFFF, 0xFFFFFFFE
SHAPE_SIDES = (64, 128, 256, 512)
MAX_PAGE_RECORDS = 4096
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
ZSTD_WINDOW = 1 << 18
MAX_CELL = (64, 127)  # glyph records: u8 width/height, i8 bearing/top for glyphs up to two cells wide
SHAPING = {"direction": "ltr", "language": "und"}  # script per role; default features only
# Glyph order (--order hot): hot tiers first, each in scalar order, so the glyphs of everyday text share few pages.
# CJK: the legacy double-byte set of the locale by lead-byte rows (KS X 1001 punctuation, compatibility jamo and
# Hangul, its other symbols, its Hanja; JIS X 0208 symbols, kana and level 1, then level 2; GB 2312 and Big5 levels
# 1 and 2), from Python's codecs.
LEGACY_TIERS = {"ko": ("euc-kr", (((0xA1, 0xA1), (0xA4, 0xA4), (0xB0, 0xC8)), ((0xA2, 0xAC),), ((0xCA, 0xFD),))),
                "ja": ("euc-jp", (((0xA1, 0xCF),), ((0xD0, 0xF4),))),
                "zh-Hans": ("gb2312", (((0xA1, 0xD7),), ((0xD8, 0xF7),))),
                "zh-Hant": ("big5", (((0xA1, 0xC6),), ((0xC9, 0xF9),))),
                "zh-HK": ("big5", (((0xA1, 0xC6),), ((0xC9, 0xF9),)))}
# Other roles: scalar ranges (Nerd: glyph sets of the Nerd Fonts wiki, Material Design last).
HOT_RANGES = {"latin": ((0x20, 0x7E), (0x2500, 0x259F), (0xA0, 0xFF), (0x2000, 0x206F), (0x2190, 0x21FF),
                        (0x2200, 0x23FF), (0x25A0, 0x25FF), (0x100, 0x17F)),
              "symbols": (((0x2800, 0x28FF), (0xF5D0, 0xF60D)), (0x25A0, 0x25FF), (0x2190, 0x21FF), (0x2300, 0x23FF),
                          (0x2600, 0x27BF), (0x2B00, 0x2BFF), (0x1FB00, 0x1FBFF), (0x1CC00, 0x1CEBF)),
              "nerd": ((0xE0A0, 0xE0D7), (0xE5FA, 0xE6B7), (0xE700, 0xE8EF), (0xF000, 0xF2FF), (0xF400, 0xF533),
                       (0xEA60, 0xEC1E), (0xF300, 0xF381), (0xE000, 0xF8FF))}


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
    for key, shapes in CONFIG["page_shapes"].items():
        default = CONFIG["page_atlas"][key]
        if not all(w in SHAPE_SIDES and h in SHAPE_SIDES for w, h in shapes) or default not in shapes:
            sys.exit(f"page_shapes.{key} {shapes}: sides must be {SHAPE_SIDES} and include page_atlas.{key} {default}")
    if not 1 <= CONFIG["max_page_records"] <= MAX_PAGE_RECORDS:
        sys.exit(f"max_page_records must be 1..{MAX_PAGE_RECORDS}")


def format_key(fmt):
    return "a4" if fmt == FMT_A4 else "a8"


def atlas_stride(fmt, width):
    return width // 2 if fmt == FMT_A4 else width


def order_path(name):
    return (ucd.CACHE if LOCK["order"][name]["cache"] == "ucd" else SRC) / name


def fetch(download=True):
    for name, f in LOCK["files"].items():
        dest = SRC / name
        if download:
            pooch.retrieve(f["url"], f"sha256:{f['sha256']}", fname=dest.name, path=dest.parent, progressbar=False)
        elif not dest.exists() or hashlib.sha256(dest.read_bytes()).hexdigest() != f["sha256"]:
            sys.exit(f"missing or modified input {dest}; run `make fontpack-fetch`")
    for name, f in LOCK["order"].items() if download else ():
        dest = order_path(name)
        try:
            pooch.retrieve(f["url"], f"sha256:{f['sha256']}", fname=name, path=dest.parent, progressbar=False)
        except (OSError, ValueError) as e:
            dest.unlink(missing_ok=True)
            print(f"warning: optional glyph order input {name} not fetched ({e}); "
                  "the CJK packages fall back to the legacy sets' tiers in code order")
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
        self.underline_thickness = post.underlineThickness
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


@functools.cache
def sprites():
    """The glyph drawers of tools/fontpack/sprite: {scalar: (drawer, module)}."""
    return sprite.load(ucd.CACHE / "UnicodeData.txt")


@functools.cache
def assigned_scalars():
    """Scalars UnicodeData.txt assigns."""
    out, first = set(), None
    for line in (ucd.CACHE / "UnicodeData.txt").read_text(encoding="utf-8").splitlines():
        f = line.split(";")
        cp = int(f[0], 16)
        if f[1].endswith(", First>"):
            first = cp
            continue
        out.update(range(first, cp + 1) if f[1].endswith(", Last>") else (cp,))
    return out


def generated_scalars(profile):
    """{package: scalars} drawn by the sprite code instead of rasterised from the package's face: the ranges of
    fontpack.config.json "generated" that a drawer covers; also {package: configured scalars without a drawer}."""
    config = CONFIG.get("generated", {})
    if not config:
        return {}, {}
    drawn = set(sprites())
    assigned = assigned_scalars()
    out, undrawn, owner = {}, {}, {}
    for name, ranges in config.items():
        want = {cp for r in ranges for cp in scalar_set(r) if profile.drawable(cp) and cp in assigned}
        out[name], undrawn[name] = want & drawn, want - drawn
        for cp in out[name]:
            if owner.setdefault(cp, name) != name:
                sys.exit(f"generated U+{cp:04X} configured for {owner[cp]} and {name}")
    return out, undrawn


def sprite_glyph(cp, m, fmt, baseline):
    """Coverage rows of the drawn cell cut to their ink (A4: nonzero after quantisation), bearing x and top."""
    w, h = m.cell_width, m.cell_height
    cov = sprite.render(cp, m).cell_bytes()
    ink = (lambda a: a * 15 + 127 >= 255) if fmt == FMT_A4 else bool
    xs = [x for x in range(w) if any(ink(cov[y * w + x]) for y in range(h))]
    ys = [y for y in range(h) if any(ink(cov[y * w + x]) for x in range(w))]
    if not xs:
        return None, 0, 0
    x0, x1, y0, y1 = xs[0], xs[-1] + 1, ys[0], ys[-1] + 1
    return [cov[y * w + x0:y * w + x1] for y in range(y0, y1)], x0, baseline - y0


def resolve_providers(profile, packages, faces, generated=None):
    """Fix the provider of every scalar at build time (text path). A generated scalar is served by its package
    only."""
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
    for name, cps in (generated or {}).items():
        for other in out:
            out[other] = out[other] | cps if other == name else out[other] - cps
    return out


def plan_keys(profile, pkg, face, scalars, ivd, report, keep=None, generated=frozenset()):
    """Keys of one package: scalar -> slot key, sequences -> (kind, slot key). The face's variation sequences of a
    generated scalar are left out (they would draw the face's glyph)."""
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
            if base not in cmap or base in generated:
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


def pinned_input(name):
    path = SRC / name
    data = path.read_bytes() if path.exists() else b""
    if name not in LOCK["files"] or sha256(data) != LOCK["files"][name]["sha256"]:
        sys.exit(f"missing or modified input {path}; run `make fontpack-fetch`")
    return data


@functools.cache
def icon_rules(face):
    """{Nerd scalar: constraint} from the pinned font-patcher (fonts.lock.json "nerd_rules") for the Symbols face."""
    pin = LOCK["nerd_rules"]
    sources = functools.cache(lambda f: TTFont(io.BytesIO(pinned_input(pin["glyphs"] + f)), lazy=True).getBestCmap())
    rules, version = nerd_rules.rules(pinned_input(pin["patcher"]).decode(), face.tt, sources)
    if version != pin["version"]:
        sys.exit(f"font-patcher version {version} != {pin['version']}")
    return rules


def icon_metrics(latin_face, size, baseline, em):
    """The cell as the constraints see it: the face fills the cell, y up from its bottom; icons are two cap heights
    and the cell height averaged."""
    lh, cw = size["line_height"], size["cell_width"]
    cap = latin_face.tt["OS/2"].sCapHeight * em / latin_face.upem
    return {"cell_width": cw, "cell_height": lh, "cell_baseline": lh - baseline, "face_width": float(cw),
            "face_height": float(lh), "face_y": 0.0, "icon_height": (2 * cap + lh) / 3}


def raster_icon(face, gid, rule, m, em):
    """The unhinted outline at the latin em, scaled and moved where the constraint puts it in the cell."""
    ft = face.ft
    ft.set_char_size(face.upem * 64, face.upem * 64, 72, 72)  # one font unit per pixel: exact 26.6 points
    ft.load_glyph(gid, freetype.FT_LOAD_NO_HINTING)
    b = ft.glyph.outline.get_bbox()
    s = em / face.upem / 64
    glyph = ((b.xMax - b.xMin) * s, (b.yMax - b.yMin) * s, b.xMin * s, b.yMin * s + m["cell_baseline"])
    if glyph[0] < 0.25 or glyph[1] < 0.25:
        return None, 0, 0
    w, h, x, y = nerd_rules.place(rule, glyph, m)
    sx, sy = w / glyph[0], h / glyph[1]
    o = ft.glyph.outline._FT_Outline
    for i in range(o.n_points):
        px = (o.points[i].x - b.xMin) * s * sx + x
        py = (o.points[i].y - b.yMin) * s * sy + y - m["cell_baseline"]
        o.points[i].x, o.points[i].y = round(px * 64), round(py * 64)
    ft.glyph.render(RENDER_MODES[CONFIG["raster"]["render"]])
    bm = ft.glyph.bitmap
    rows = [bytes(bm.buffer[r * bm.pitch:r * bm.pitch + bm.width]) for r in range(bm.rows)]
    if not any(any(r) for r in rows):
        return None, 0, 0
    return rows, ft.glyph.bitmap_left, ft.glyph.bitmap_top


def crop(rows, bx, top, width, lh, baseline):
    """The part of a bitmap inside its cells."""
    y0, x0 = max(0, top - baseline), max(0, -bx)
    y1, x1 = min(len(rows), lh - (baseline - top)), min(len(rows[0]), width - bx)
    rows = [r[x0:x1] for r in rows[y0:y1]]
    if not rows or not rows[0] or not any(any(r) for r in rows):
        return None, 0, 0
    return rows, bx + x0, top - y0


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


def pack_rows(rows, fmt):
    """Coverage rows in the atlas format; A4 rows of odd width end in a zero nibble."""
    if fmt == FMT_A8:
        return [bytes(r) for r in rows]
    out = []
    for r in rows:
        n = [(a * 15 + 127) // 255 for a in r] + [0]
        out.append(bytes((n[x] << 4) | n[x + 1] for x in range(0, len(r), 2)))
    return out


def span(fmt, w):
    return w + (w & 1) if fmt == FMT_A4 else w


def layout(records, fmt, shape, limit=1 << 16):
    """Shelf-packs glyphs in index order: (first, positions, used rows) per page, at most `limit` pages; also
    whether every glyph was placed."""
    aw, ah = shape
    pages, g = [], 0
    while g < len(records) and len(pages) < limit:
        first, x, y, shelf, pos = g, 0, 0, 0, []
        while g < len(records) and g - first < CONFIG["max_page_records"]:
            w, h = records[g][:2]
            if x + span(fmt, w) > aw:
                x, y, shelf = 0, y + shelf, 0
            if y + h > ah:
                break
            pos.append((x, y))
            x, shelf = x + span(fmt, w), max(shelf, h)
            g += 1
        pages.append((first, pos, y + shelf))
    return pages, g == len(records)


def page_shape(records, fmt, name):
    """The smallest trimmed single page of a candidate shape, if smaller than one default page; else the default."""
    key = format_key(fmt)
    default = tuple(CONFIG["page_atlas"][key])
    fits = [tuple(s) for s in CONFIG["page_shapes"][key]
            if all(span(fmt, r[0]) <= s[0] and r[1] <= s[1] for r in records)]
    if default not in fits:
        sys.exit(f"{name}: a glyph exceeds the {default[0]}x{default[1]} page atlas")
    best = None
    for shape in fits:
        pages, done = layout(records, fmt, shape, 1)
        cost = pages[0][2] * atlas_stride(fmt, shape[0])
        if done and cost < default[1] * atlas_stride(fmt, default[0]) and (best is None or (cost, *shape) < best[0]):
            best = (cost, *shape), shape
    return best[1] if best else default


def pack_pages(records, fmt, shape):
    """Pages (count, height, atlas, records): every page H rows tall but the last, trimmed to its glyphs."""
    aw, ah = shape
    stride = atlas_stride(fmt, aw)
    placed, _ = layout(records, fmt, shape)
    pages, used, area = [], 0, 0
    for n, (first, pos, rows) in enumerate(placed):
        height = rows if n == len(placed) - 1 else ah
        atlas, recs = bytearray(stride * height), bytearray()
        for (w, h, bx, top, flags, gid, bitmap), (px, py) in zip(records[first:], pos):
            at = px // 2 if fmt == FMT_A4 else px
            for r, row in enumerate(bitmap):
                atlas[(py + r) * stride + at:(py + r) * stride + at + len(row)] = row
            used += span(fmt, w) * h
            recs += RECORD.pack(px, py, w, h, bx, top, flags, 0, gid & 0xFFFF, 0)
        pages.append((len(pos), height, bytes(atlas), bytes(recs)))
        area += aw * height
    return pages, used / area


def ctri_table(glyphs):
    """The ctri payload of {scalar: glyph value}: level-2 and data blocks shared by content, block 0 empty."""
    l2, data, used, l1 = {(0,) * 64: 0}, {(MISS,) * 16: 0}, {cp >> 4 for cp in glyphs}, []
    for hi in range(0x110000 >> 10):
        row = tuple(data.setdefault(tuple(glyphs.get(b << 4 | i, MISS) for i in range(16)), len(data))
                    if b in used else 0 for b in range(hi << 6, hi + 1 << 6))
        l1.append(l2.setdefault(row, len(l2)))
    if len(data) > 1 << 16:
        sys.exit(f"ctri: {len(data)} data blocks exceed 65536")
    out = bytearray(struct.pack("<II8x1088H", len(l2), len(data), *l1))
    for row in l2:
        out += struct.pack("<64H", *row)
    out += bytes(-len(out) % CTRI_ALIGN)
    for block in data:
        out += struct.pack("<16I", *block)
    return bytes(out)


def zstd(raw):
    """One frame: level 19, window at most 2^18, content size, no checksum or dictionary id."""
    level = zstandard.ZstdCompressionParameters.from_level(19, source_size=len(raw))
    params = zstandard.ZstdCompressionParameters.from_level(
        19, source_size=len(raw), window_log=min(18, level.window_log), write_content_size=True,
        write_checksum=False, write_dict_id=False)
    return zstandard.ZstdCompressor(compression_params=params).compress(raw)


def unzstd(s, expected):
    check(len(s) >= 4 and s[:4] == ZSTD_MAGIC, "zstd frame magic")
    try:
        fp = zstandard.get_frame_parameters(s)
    except zstandard.ZstdError:
        raise FormatError("zstd frame header") from None
    check(fp.content_size == expected and fp.window_size <= ZSTD_WINDOW and not fp.dict_id and not fp.has_checksum,
          "zstd frame header")
    check(len(s) < expected, "zstd frame not smaller than its content")
    try:
        out = zstandard.ZstdDecompressor().decompress(s, max_output_size=expected, allow_extra_data=False)
    except zstandard.ZstdError:
        raise FormatError("zstd frame data") from None
    check(len(out) == expected, "zstd frame data")
    return out


def decode(method, s, expected):
    if method == STORED:
        check(len(s) == expected, "stored size")
        return s
    return unzstd(s, expected)


def assemble(profile_id, features, meta, pages, method="zstd", store_index=False, tamper=None, extra=()):
    """The package bytes. meta: raw payload by box type (ptab is laid out here); pages: (count, height, atlas,
    records); extra: (type, flags, payload) boxes indexed after covr. tamper(type, page, raws, method,
    streams) -> (method, streams) changes encoded streams before layout and hashing."""
    def encode(t, i, raws, packed):
        streams = [zstd(r) for r in raws] if packed else []
        if not streams or any(len(z) >= len(r) for z, r in zip(streams, raws)):
            m, streams = STORED, list(raws)
        else:
            m = ZSTD
        return tamper(t, i, raws, m, streams) if tamper else (m, streams)

    zip_meta, zip_pages = method == "zstd" and not store_index, method == "zstd"
    boxes = []
    for t in META:
        if t == b"ptab":
            boxes.append([t, REQUIRED, PTAB.size * len(pages), None])
        elif t in meta:
            m, (data,) = encode(t, 0, [meta[t]], zip_meta and t in PACKED)
            boxes.append([t, (0 if t == b"covr" else REQUIRED) | m << 4, len(meta[t]), data])
    boxes += [[t, flags, len(data), data] for t, flags, data in extra]
    encoded = []
    for i, (count, height, atlas, recs) in enumerate(pages):
        m, (a, r) = encode(b"page", i, [atlas, recs], zip_pages)
        encoded.append((m, BOX.pack(16 + 8 + len(a) + len(r), b"page", 0, REQUIRED | m << 4, 8 + len(atlas) + len(recs))
                        + struct.pack("<II", i, len(a)) + a + r))
    sidx_size = 24 + ENTRY.size * len(boxes)
    off = HEADER_SIZE + sidx_size
    for b in boxes:
        gap = -(off + 16) % CTRI_ALIGN if b[0] == b"ctri" and b[1] >> 4 == STORED else 0
        gap += CTRI_ALIGN if 0 < gap < 16 else 0
        b += [gap, off + gap]
        off += gap + 16 + (b[2] if b[3] is None else len(b[3]))
    tail, ptab = bytearray(), bytearray()
    for (m, page), (count, height, _, _) in zip(encoded, pages):
        gap = -(off + len(tail) + 24) % PAGE_ALIGN if m == STORED else 0
        if gap:
            gap += PAGE_ALIGN if gap < 16 else 0
            tail += BOX.pack(gap, b"free", 0, 0, gap - 16) + bytes(gap - 16)
        ptab += PTAB.pack(off + len(tail), xxh3(page), len(page), count, height)
        tail += page
    ptab = bytes(ptab)
    next(b for b in boxes if b[0] == b"ptab")[3] = tamper(b"ptab", 0, [ptab], STORED, [ptab])[1][0] if tamper else ptab
    body, entries = bytearray(), bytearray()
    for t, flags, raw, data, gap, at in boxes:
        box = BOX.pack(16 + len(data), t, 0, flags, raw) + data
        entries += ENTRY.pack(t, flags, 0, at, len(box), raw, xxh3(box))
        body += (BOX.pack(gap, b"free", 0, 0, gap - 16) + bytes(gap - 16) if gap else b"") + box
    sidx = BOX.pack(sidx_size, b"sidx", 0, REQUIRED, sidx_size - 16) + struct.pack("<II", len(boxes), 0) + entries
    zipped = any(b[1] >> 4 & 15 == ZSTD for b in boxes) or any(m == ZSTD for m, _ in encoded)
    size = HEADER_SIZE + len(sidx) + len(body) + len(tail)
    head = HEAD.pack(HEADER_SIZE, b"shrf", 0, REQUIRED, HEADER_SIZE - 16, FORMAT_VERSION, 0,
                     features | (FEAT_ZSTD if zipped else 0), 0, sidx_size, size, HEADER_SIZE, profile_id, xxh3(sidx), 0)
    return head[:120] + struct.pack("<Q", xxh3(head[:120])) + sidx + body + tail


@functools.cache
def legacy_tiers(locale):
    """{scalar: tier} of the locale's legacy set."""
    codec, rows = LEGACY_TIERS[locale]
    out = {}
    for tier, ranges in enumerate(rows):
        for b in (b for lo, hi in ranges for b in range(lo << 8 | 0x40, hi + 1 << 8)):
            try:
                s = struct.pack(">H", b).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                out.setdefault(ord(s), tier)
    return out


@functools.cache
def order_input(name):
    """Bytes of an optional order input (fonts.lock.json "order"), None when it was not fetched."""
    path = order_path(name)
    if not path.exists():
        print(f"{path} not fetched: the CJK packages fall back to the legacy sets' tiers in code order "
              "(run `make fontpack-fetch`)")
        return None
    data = path.read_bytes()
    if sha256(data) != LOCK["order"][name]["sha256"]:
        sys.exit(f"missing or modified input {path}; run `make fontpack-fetch`")
    return data


@functools.cache
def unihan():
    """{field: {scalar: value}} of the Unihan fields the CJK order uses, None without Unihan.zip."""
    data = order_input("Unihan.zip")
    if data is None:
        return None
    fields = {"kHanyuPinlu": {}, "kJoyoKanji": {}, "kKoreanEducationHanja": {}}
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for member in ("Unihan_OtherMappings.txt", "Unihan_Readings.txt"):
            for line in z.read(member).decode().splitlines():
                cp, field, value = line.split("\t", 2) if line.startswith("U+") else ("", "", "")
                if field in fields:
                    fields[field][int(cp[2:], 16)] = value
    return fields


def frequency_ranks(counts):
    return {cp: r for r, (_, cp) in enumerate(sorted((-n, cp) for cp, n in counts.items()))}


@functools.cache
def cjk_order(locale):
    """({scalar: tier}, {scalar: rank}, inputs of the tiers, inputs of the ranks) of a CJK locale for --order hot.
    The legacy set's tiers; Unihan puts the Korean education Hanja (kKoreanEducationHanja) ahead of the other
    KS X 1001 Hanja and the Jōyō kanji (kJoyoKanji 2010) into the first ja tier, ahead of the other level 1 kanji.
    Ranks: ko punctuation and jamo, then Hangul by syllable frequency in the 2005 survey of the National Institute
    of Korean Language; zh by kHanyuPinlu."""
    tiers, ranks = dict(legacy_tiers(locale)), {}
    u = unihan()
    if u and locale == "ko":
        edu = u["kKoreanEducationHanja"]
        tiers.update({cp: 3 for cp, t in tiers.items() if t == 2 and cp not in edu})
    elif u and locale == "ja":
        joyo = {cp for cp, v in u["kJoyoKanji"].items() if v == "2010"}
        tiers.update({cp: 0 if cp in joyo else 2 if t else int(0x4E00 <= cp <= 0x9FFF) for cp, t in tiers.items()})
    elif u:
        ranks = frequency_ranks({cp: sum(map(int, re.findall(r"\((\d+)\)", v)))
                                 for cp, v in u["kHanyuPinlu"].items()})
        return tiers, ranks, [], ["Unihan.zip"]
    data = order_input("korean-frequency-2005.zip") if locale == "ko" else None
    if data:
        f = LOCK["order"]["korean-frequency-2005.zip"]
        with zipfile.ZipFile(io.BytesIO(data), metadata_encoding=f["member_encoding"]) as z:
            raw = z.read(f["member"])
        if sha256(raw) != f["member_sha256"]:
            sys.exit(f"{order_path('korean-frequency-2005.zip')}: {f['member']} does not match fonts.lock.json")
        counts = {ord(s): int(n) for _, n, s in (line.split("\t") for line in
                                                  raw.decode(f["member_encoding"]).splitlines()[1:])}
        top = max(counts.values()) + 1
        ranks = frequency_ranks(counts | {cp: top for cp, t in tiers.items() if t == 0 and not 0xAC00 <= cp <= 0xD7A3})
    return tiers, ranks, ["Unihan.zip"] if u else [], ["korean-frequency-2005.zip"] if data else []


def slot_order(pkg, key, ranked):
    """Sort key of a slot for --order hot: its tier (emoji: single glyphs and VS16 forms, keycaps and flags,
    skin tones, ZWJ sequences), then its rank in `ranked` (a single scalar), then its scalars."""
    kind, v = key
    if kind == "gid":
        return (99,)
    s = (v,) if kind == "cp" else v
    if pkg["role"] == "emoji":
        tier = (3 if 0x200D in s else 2 if any(0x1F3FB <= c <= 0x1F3FF for c in s)
                else int(len(s) > 1 and s[-1] != 0xFE0F))
    elif kind == "seq":
        tier = 98
    elif pkg["role"] == "cjk":
        tier = cjk_order(pkg["locale"])[0].get(v, 9)
    else:
        tier = next((t for t, r in enumerate(HOT_RANGES[pkg["role"]])
                     if any(lo <= v <= hi for lo, hi in (r if isinstance(r[0], tuple) else (r,)))), 9)
    return tier, ranked.get(v, len(ranked)) if kind == "cp" else len(ranked), s


def build_package(name, pkg, profile, faces, scalars, ivd, tools, size, keep, method="zstd", store_index=False,
                  order="codepoint", ranked=None, generated=frozenset()):
    role = pkg["role"]
    report = {"package": name, "role": role}
    regular = faces[pkg["faces"]["regular"]]
    cmap, seqs = plan_keys(profile, pkg, regular, scalars, ivd, report, keep, generated)

    slot_keys = list(dict.fromkeys(list(cmap.values()) + [v[1] for v in seqs.values()]))
    inputs = []
    if order == "hot":
        _, auto, inputs, rank_inputs = cjk_order(pkg["locale"]) if role == "cjk" else ({}, {}, [], [])
        inputs = inputs + (["--rank"] if ranked else rank_inputs)
        ranked = ranked or auto
        slot_keys.sort(key=lambda k: slot_order(pkg, k, ranked or {}))

    latin_face = faces[CONFIG["packages"]["latin"]["faces"]["regular"]]
    baseline, underline, strike = size_metrics(latin_face, size)
    fmt = FMT_A8 if any(e.get("package") == name for e in CONFIG["a8"]) else FMT_A4
    lh, cw = size["line_height"], size["cell_width"]
    em = fit_ppem(latin_face, "latin", size, baseline)
    inst_ppem, lift = (cjk_ppem(regular, scalars, size, baseline, em) if role == "cjk"
                       else (fit_ppem(regular, role, size, baseline), 0))
    rules, icons = (icon_rules(regular), icon_metrics(latin_face, size, baseline, em)) if role == "nerd" else ({}, None)
    glyph_records, bitmaps, record_of = [], [], []
    clipped = notdef = drawn = cropped = cropped_ink = 0
    m = sprite.Metrics.from_face(latin_face, size["cell_width"], size["line_height"])
    for key in slot_keys:
        kind, v = key
        if kind == "cp" and v in generated:
            rows, bx, top = sprite_glyph(v, m, fmt, baseline)
            if rows is None:
                record_of.append(None)
                continue
            packed = pack_rows(rows, fmt)
            record_of.append(len(glyph_records))
            cells = key_cells(profile, role, key)
            glyph_records.append((len(rows[0]), len(rows), bx, top, fmt | (cells << 2), 0, packed))
            bitmaps.append(sum(map(len, packed)))
            drawn += 1
            continue
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
        if icons and gid:  # sized by the Nerd Fonts rules, then cut to the cell
            if cells != 1:
                sys.exit(f"{name}: glyph {key} is not one cell wide")
            rows, bx, top = raster_icon(regular, gid, rules.get(cp, nerd_rules.FIT), icons, em)
            if rows and (bx < 0 or bx + len(rows[0]) > cw or baseline - top < 0 or baseline - top + len(rows) > lh):
                ink = sum(a >= 9 for r in rows for a in r)
                rows, bx, top = crop(rows, bx, top, cw, lh, baseline)
                cropped += 1
                cropped_ink += ink - sum(a >= 9 for r in rows or () for a in r)
            if rows is None:
                record_of.append(None)
                continue
            packed = pack_rows(rows, fmt)
            record_of.append(len(glyph_records))
            glyph_records.append((len(rows[0]), len(rows), bx, top, fmt | (cells << 2), gid, packed))
            bitmaps.append(sum(map(len, packed)))
            continue
        box = regular.cell_box if 0x2500 <= cp <= 0x259F or 0x1FB00 <= cp <= 0x1FBFF else None
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
            record_of.append(None)
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
        packed = pack_rows(rows, fmt)
        if not (-128 <= bx <= 127 and -128 <= top <= 127 and w < 256 and h < 256):
            sys.exit(f"{name}: glyph {key} exceeds record range")
        record_of.append(len(glyph_records))
        glyph_records.append((w, h, bx, top, fmt | (cells << 2), gid, packed))
        bitmaps.append(sum(map(len, packed)))
    report.update(slots=len(slot_keys), blank=len(slot_keys) - len(glyph_records), clipped=clipped, notdef=notdef,
                  bitmap_bytes=sum(bitmaps), generated=sum(1 for cp in cmap if cp in generated), generated_ink=drawn)
    if icons:
        report.update(cropped=cropped, cropped_ink=cropped_ink)
    if not glyph_records:
        sys.exit(f"{name}: the build has no glyphs")

    keys = [(k,) if isinstance(k, int) else k for k in cmap] + list(seqs)
    longest = max((len(k) for k in keys), default=0)
    longest_bytes = max((sum(ucd.utf8_len(c) for c in k) for k in keys), default=0)
    if longest > profile.max_scalars or longest_bytes > profile.max_bytes:
        sys.exit(f"{name}: key of {longest} scalars/{longest_bytes} bytes exceeds the profile limit")
    report.update(max_key_scalars=longest, max_key_bytes=longest_bytes)

    shape = page_shape(glyph_records, fmt, name)
    pages, fill = pack_pages(glyph_records, fmt, shape)
    values = [n << 12 | r for n, page in enumerate(pages) for r in range(page[0])]
    glyph_of = {k: BLANK if r is None else values[r] for k, r in zip(slot_keys, record_of)}
    hot = [glyph_of[k] >> 12 for k in slot_keys
           if order == "hot" and glyph_of[k] < BLANK and slot_order(pkg, k, {})[0] == 0] + [-1]

    strings = bytearray()

    def s(text):
        b = text.encode()
        off = len(strings)
        strings.extend(b)
        return off, len(b)

    locale = pkg.get("locale", "").encode().ljust(8, b"\0")
    build = s(f"{VERSION}; freetype {tools['freetype']}; harfbuzz {tools['harfbuzz']}; "
              f"fonttools {tools['fonttools']}; zstd {'.'.join(map(str, zstandard.ZSTD_VERSION))}; "
              f"raster {CONFIG['raster']['load']}/{CONFIG['raster']['render']}; "
              f"shaping {shaping_script(role)}/{SHAPING['language']}/{SHAPING['direction']}/default features")
    pname = s(f"shiroko-{name}")
    meta = {b"mani": struct.pack("<B3x8sIIIIII", ROLES[role], locale, *pname, *build, len(values), len(pages))}
    meta[b"srcs"] = struct.pack("<II32sII", *s(regular.id), bytes.fromhex(LOCK["faces"][regular.id]["sha256"]),
                                *s(spdx_expression(LOCK["licenses"][pkg["license"]])))
    meta[b"strs"] = bytes(strings)
    iid = hashlib.sha256(json.dumps([LOCK["faces"][regular.id]["sha256"], pkg.get("locale", ""), "regular",
                                     round(inst_ppem * 64), baseline, CONFIG["raster"]],
                                    sort_keys=True).encode()).digest()[:16]
    meta[b"inst"] = struct.pack("<HBBHHIhhhH16sHH8x", 0, 0, fmt, lh, cw, round(inst_ppem * 64), baseline, underline,
                                strike, 1, iid, *shape)
    meta[b"ctri"] = ctri_table({cp: glyph_of[k] for cp, k in cmap.items()})
    pool, seq_recs = [], bytearray()
    for key in sorted(seqs):
        kind, target = seqs[key]
        seq_recs += struct.pack("<BBHII", len(key), kind, 0, len(pool), glyph_of[target])
        pool.extend(key)
    if seqs:
        meta[b"seqs"], meta[b"pool"] = bytes(seq_recs), struct.pack(f"<{len(pool)}I", *pool)
    meta[b"covr"] = struct.pack("<9I", len(cmap), sum(1 for k in seqs.values() if k[0] == SEQ_GLYPH),
                                sum(1 for k in seqs.values() if k[0] == SEQ_ALIAS), report.get("uvs", 0),
                                report.get("ivs", 0), report.get("emoji_unsupported", 0), 0,
                                len(values), sum(bitmaps))
    features = (FEAT_SEQ if seqs else 0) | (FEAT_A4 if fmt == FMT_A4 else FEAT_A8)
    blob = assemble(profile.id, features, meta, pages, method, store_index)
    report.update(file_bytes=len(blob), pages=len(pages), shape=f"{shape[0]}x{shape[1]}",
                  index_bytes=PTAB.unpack_from(blob, box_offset(blob, b"ptab") + 16)[0],
                  atlas_raw_bytes=sum(len(p[2]) for p in pages), atlas_fill=round(fill, 4), package_id=sha256(blob),
                  scalars=len(cmap), sequences=len(seqs), order=order, order_inputs=inputs, hot_pages=max(hot) + 1)
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


def box_offset(blob, t):
    """Offset of the box `sidx` lists for type t."""
    sidx = struct.unpack_from("<Q", blob, 40)[0]
    for i in range(struct.unpack_from("<I", blob, sidx + 16)[0]):
        e = ENTRY.unpack_from(blob, sidx + 24 + ENTRY.size * i)
        if e[0] == t:
            return e[3]
    raise KeyError(t)


class Package:
    """Reader for `verify`, `install`, `build` and previews. Raises FormatError unless the package passes the
    runtime loader's rules in their order (docs/font-package-format.md), every page and glyph record included,
    matches the given text profile and its boxes tile the file."""

    def __init__(self, blob, profile_id, max_scalars):
        self.blob = blob
        check(len(blob) >= HEADER_SIZE, "shorter than a header")
        check(blob[:8] != b"SHRFPKG1", "format v4 package: rebuild with fontpack 6")
        check(blob[:8] == MAGIC, "bad magic")
        (_, _, ver, flags, raw, fver, zero, feat, _, sidx_size, fsize, soff, self.profile, sidx_digest,
         digest) = HEAD.unpack_from(blob)
        check(fver != 5, "format v5 package: rebuild with fontpack 6")
        check(ver == 0 and flags == REQUIRED and raw == HEADER_SIZE - 16 and fver == FORMAT_VERSION and zero == 0,
              "header version")
        check(digest == xxh3(blob[:120]), "header checksum")
        check(feat & ~15 == 0, "unsupported required feature")
        check(not any(blob[88:120]), "reserved header bytes")
        check(fsize == len(blob) and soff == HEADER_SIZE and (sidx_size - 24) % ENTRY.size == 0
              and 24 <= sidx_size <= 24 + ENTRY.size * MAX_INDEX and soff + sidx_size <= fsize, "file size or index")
        check(self.profile == profile_id, "text profile mismatch")
        self.features = feat

        sidx = blob[soff:soff + sidx_size]
        check(xxh3(sidx) == sidx_digest, "index checksum")
        count, zero = struct.unpack_from("<II", sidx, 16)
        check(BOX.unpack_from(sidx) == (sidx_size, b"sidx", 0, REQUIRED, sidx_size - 16)
              and count == (sidx_size - 24) // ENTRY.size and zero == 0, "index box")
        boxes, self.listed, end = {}, {}, soff + sidx_size
        for i in range(count):
            t, flags, ver, off, size, raw, digest = ENTRY.unpack_from(sidx, 24 + ENTRY.size * i)
            check(size >= 16 and off >= end and off + size <= fsize, "index entry out of order or range")
            end = off + size
            self.listed[off] = (t, size)
            check(t not in (b"shrf", b"sidx", b"page", b"free"), f"{t!r} in the index")
            if t not in META:
                check(not flags & REQUIRED, f"unsupported required box {t!r}")
                continue
            check(t not in boxes, f"duplicate box {t!r}")
            method = flags >> 4 & 15
            check(method < 2, f"unsupported method {method} of {t!r}")
            check(flags & ~0xF1 == 0 and (flags & REQUIRED or t == b"covr") and ver == 0
                  and (method == STORED or (t in PACKED and feat & FEAT_ZSTD)), f"{t!r} flags")
            check(raw == size - 16 if method == STORED else raw > size - 16, f"{t!r} raw size")
            boxes[t] = (off, size, flags, raw, digest)
        check(all(t in boxes for t in (b"mani", b"inst", b"ctri", b"ptab")), "missing box")
        check(max(o + s for o, s, *_ in boxes.values()) - min(o for o, *_ in boxes.values()) <= 128 << 20
              and sum(b[3] for b in boxes.values()) <= 128 << 20, "package index too large")

        meta = {}
        for t, (off, size, flags, raw, digest) in sorted(boxes.items(), key=lambda b: b[1][0]):
            box = blob[off:off + size]
            check(xxh3(box) == digest, f"{t!r} checksum")
            check(BOX.unpack_from(box) == (size, t, 0, flags, raw), f"{t!r} header")
            meta[t] = decode(flags >> 4, box[16:], raw)
        self.meta = meta
        strings = len(meta.get(b"strs", b""))
        check(len(meta[b"mani"]) == 36, "mani size")
        (self.role, pad, self.locale, name_off, name_len, build_off, build_len, self.nglyphs,
         npages) = struct.unpack("<B3s8sIIIIII", meta[b"mani"])
        check(pad == bytes(3), "mani")
        check(1 <= self.role <= 5, "role")
        check(all(c == 0 or 0x20 <= c <= 0x7E for c in self.locale), "locale")
        check(name_off + name_len <= strings and build_off + build_len <= strings, "string reference")
        check(1 <= self.nglyphs <= 1 << 22, "glyph count")
        sources = meta.get(b"srcs")
        check(len(meta[b"inst"]) == 48, "inst size")
        check(sources is None or len(sources) % 48 == 0, "srcs size")
        check(len(meta.get(b"covr", bytes(36))) == 36, "covr size")
        for name_off, name_len, _, lic_off, lic_len in struct.iter_unpack("<II32sII", sources or b""):
            check(name_off + name_len <= strings and lic_off + lic_len <= strings, "source string reference")
        self.inst = struct.unpack("<HBBHHIhhhH16sHH8s", meta[b"inst"])
        src, style, fmt, lh, cw, _, base, under, strike, raster, _, aw, ah, reserved = self.inst
        check(sources is None or src < len(sources) // 48, "instance source")
        check(style == 0 and fmt in (FMT_A4, FMT_A8) and feat & fmt, "instance style or format")
        check(1 <= lh <= 1024 and 1 <= cw <= 1024, "instance size")
        check(0 <= base <= lh and 0 <= under < lh and 0 <= strike < lh, "instance line metrics")
        check(raster & ~1 == 0 and reserved == bytes(8), "instance reserved bytes")
        check(aw in SHAPE_SIDES and ah in SHAPE_SIDES, "instance page shape")
        self.atlas = (aw, ah, atlas_stride(fmt, aw))
        ctri = meta[b"ctri"]
        check(len(ctri) >= CTRI_L2, "ctri size")
        l2n, dn, z0, z1 = struct.unpack_from("<IIII", ctri)
        check(1 <= l2n <= 1 << 16 and 1 <= dn <= 1 << 16 and z0 == z1 == 0, "ctri header")
        d = CTRI_L2 + 128 * l2n + 63 & ~63
        check(len(ctri) == d + 64 * dn, "ctri size")
        check(not any(ctri[CTRI_L2 + 128 * l2n:d]), "ctri padding")
        l1 = struct.unpack_from("<1088H", ctri, 16)
        l2 = struct.unpack_from(f"<{64 * l2n}H", ctri, CTRI_L2)
        values = struct.unpack_from(f"<{16 * dn}I", ctri, d)
        check(max(l1) < l2n and max(l2) < dn, "ctri block number")
        check(not any(l2[:64]) and set(values[:16]) == {MISS}, "ctri block 0")
        check(boxes[b"ctri"][2] >> 4 != STORED or (boxes[b"ctri"][0] + 16) % CTRI_ALIGN == 0, "ctri not aligned")
        self.scalars = {hi << 10 | mid << 4 | i: v for hi, b in enumerate(l1) if b
                        for mid, block in enumerate(l2[64 * b:64 * b + 64]) if block
                        for i, v in enumerate(values[16 * block:16 * block + 16]) if v != MISS}
        pool_data = meta.get(b"pool", b"")
        check(len(pool_data) % 4 == 0 and len(pool_data) // 4 <= 1 << 22, "pool size")
        pool = struct.unpack(f"<{len(pool_data) // 4}I", pool_data)
        self.seqs = {}
        if b"seqs" in meta:
            seq_data = meta[b"seqs"]
            check(feat & FEAT_SEQ and b"pool" in meta and len(seq_data) % 12 == 0 and len(seq_data) // 12 <= 1 << 22,
                  "seqs size")
            check(all(cp <= 0x10FFFF and not 0xD800 <= cp <= 0xDFFF for cp in pool), "sequence scalar")
            prev = None
            for length, kind, zero, idx, glyph in struct.iter_unpack("<BBHII", seq_data):
                check(2 <= length <= max_scalars and 1 <= kind <= 4 and zero == 0 and idx + length <= len(pool),
                      "sequence record")
                key = pool[idx:idx + length]
                check(prev is None or key > prev, "sequence order")
                prev = key
                self.seqs[key] = (kind, glyph)
        check(len(meta[b"ptab"]) == PTAB.size * npages and 1 <= npages <= 1 << 16, "ptab size")
        self.pages = list(PTAB.iter_unpack(meta[b"ptab"]))
        prev_end = end
        for off, _, size, count, height in self.pages:
            check(1 <= count <= MAX_PAGE_RECORDS and 1 <= height <= ah and 24 <= size <= 1 << 20 and off >= prev_end
                  and off + size <= fsize, "page record")
            prev_end = off + size
        counts = [p[3] for p in self.pages]
        check(sum(counts) == self.nglyphs, "pages do not cover every glyph")
        check(all(v >= BLANK or (v >> 12 < npages and v & 4095 < counts[v >> 12])
                  for v in set(values) | {g for _, g in self.seqs.values()}), "glyph value")
        check(self.scalars.get(0x20, MISS) >= BLANK, "U+0020 has a bitmap")
        self.data = [self.check_page(i) for i in range(npages)]

        pos, reached = soff + sidx_size, 0
        pages = {p[0]: p[2] for p in self.pages}
        while pos < fsize:
            check(fsize - pos >= 16, "box chain")
            size, t, ver, flags, raw = BOX.unpack_from(blob, pos)
            check(16 <= size <= fsize - pos, "box chain")
            if pos in self.listed or pos in pages:
                check(self.listed.get(pos, (b"page", pages.get(pos))) == (t, size), "box chain")
                reached += 1
            else:
                check(t == b"free" and ver == flags == 0 and raw == size - 16 and not any(blob[pos + 16:pos + size]),
                      "free box")
            pos += size
        check(reached == len(self.listed) + len(pages), "box chain")

    def check_page(self, i):
        """The page's method, atlas and records after rules 26-31."""
        off, digest, size, count, height = self.pages[i]
        aw, ah, stride = self.atlas
        box = self.blob[off:off + size]
        check(xxh3(box) == digest, f"page {i} checksum")
        bsize, t, ver, flags, raw = BOX.unpack_from(box)
        method = flags >> 4 & 15
        check(t == b"page" and ver == 0 and bsize == size, f"page {i} header")
        check(method < 2, f"unsupported method {method} of page {i}")
        check(flags & ~0xF0 == REQUIRED and (method == STORED or self.features & FEAT_ZSTD)
              and raw == 8 + height * stride + 16 * count, f"page {i} header")
        check(method != STORED or size == raw + 16, f"page {i} stored size")
        check(method != STORED or (off + 24) % PAGE_ALIGN == 0, f"page {i} not aligned")
        index, abytes = struct.unpack_from("<II", box, 16)
        check(index == i and (abytes == height * stride if method == STORED else abytes <= size - 24),
              f"page {i} streams")
        atlas = decode(method, box[24:24 + abytes], height * stride)
        recs = decode(method, box[24 + abytes:], 16 * count)
        a4 = self.inst[2] == FMT_A4
        for g in range(count):
            x, y, w, h, _, _, flags, zero, _, zero2 = RECORD.unpack_from(recs, 16 * g)
            check(flags & 3 == self.inst[2] and 1 <= flags >> 2 & 3 <= 2 and flags & 0xF0 == 0 and zero == zero2 == 0,
                  f"page {i} record {g} flags")
            check(w and h and not (a4 and x % 2) and x + w <= aw and y + h <= height, f"page {i} record {g} rect")
            check(not a4 or w % 2 == 0 or not any(atlas[(y + r) * stride + (x + w) // 2] & 15 for r in range(h)),
                  f"page {i} record {g} A4 padding")
        return method, atlas, recs

    def glyph(self, value):
        """fmt, w, h, bearing_x, top, stride, flags and the bitmap rows packed at stride = row bytes."""
        if value == BLANK:
            return self.inst[2], 0, 0, 0, 0, 0, 0, b"", None
        _, atlas, recs = self.data[value >> 12]
        x, y, w, h, bx, top, flags, _, _, _ = RECORD.unpack_from(recs, 16 * (value & 4095))
        stride, fmt = self.atlas[2], flags & 3
        row = (w + 1) // 2 if fmt == FMT_A4 else w
        at = x // 2 if fmt == FMT_A4 else x
        data = b"".join(atlas[at + (y + r) * stride:at + (y + r) * stride + row] for r in range(h))
        return fmt, w, h, bx, top, row, flags, data, (x, y)

    def content(self):
        """What `assemble` takes to write this package again."""
        meta = {t: v for t, v in self.meta.items() if t != b"ptab"}
        pages = [(p[3], p[4], atlas, recs) for p, (_, atlas, recs) in zip(self.pages, self.data)]
        return self.profile, self.features & ~FEAT_ZSTD, meta, pages


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
        if cp not in pkg.scalars:
            continue
        lh, cw, base = pkg.inst[3], pkg.inst[4], pkg.inst[6]
        fmt, w, h, bx, top, stride, flags, data, _ = pkg.glyph(pkg.scalars[cp])
        cov = [[0] * cw for _ in range(lh)]
        for y in range(h):
            for x in range(w):
                a = (data[y * stride + x // 2] >> (4 if x % 2 == 0 else 0) & 15) * 17 if fmt == FMT_A4 else data[y * stride + x]
                px, py = bx + x, base - top + y
                if 0 <= px < cw and 0 <= py < lh:
                    cov[py][px] = a
        need = {0x2588: [(x, y) for y in range(lh) for x in range(cw)],
                0xE0B0: [(0, y) for y in range(lh)], 0xE0B2: [(cw - 1, y) for y in range(lh)]}.get(cp)
        if cp == 0x2588:
            ok = all(cov[y][x] == 255 for x, y in need)
        elif cp in (0xE0B0, 0xE0B2):
            ok = all(cov[y][x] and (cov[y][x] == 255 or y in (0, lh - 1)) for x, y in need)
        elif cp == 0x2500:  # a row with ink at both edges, near the middle
            ok = any(cov[y][0] and cov[y][cw - 1] for y in range(lh // 2 - 2, lh // 2 + 2))
        else:  # a column with ink at both ends, near the middle
            ok = any(cov[0][x] and cov[lh - 1][x] for x in range(cw // 2 - 2, cw // 2 + 2))
        out[f"U+{cp:04X}"] = ok
        if not ok:
            print(f"warning: U+{cp:04X} does not fill its cell edges")
    return out


def preview(pkg, path, report):
    step = max(1, pkg.nglyphs // 192)
    lh, cw, baseline = pkg.inst[3], pkg.inst[4], pkg.inst[6]
    values = [n << 12 | r for n, p in enumerate(pkg.pages) for r in range(p[3])][::step][:192]
    cols = 32
    img = Image.new("L", (cols * 2 * cw + 8, ((len(values) + cols - 1) // cols) * lh + 8), 0)
    for n, value in enumerate(values):
        fmt, w, h, bx, top, stride, flags, data, _ = pkg.glyph(value)
        ox, oy = 4 + (n % cols) * 2 * cw, 4 + (n // cols) * lh
        for y in range(h):
            for x in range(w):
                a = (data[y * stride + x // 2] >> (4 if x % 2 == 0 else 0) & 15) * 17 if fmt == FMT_A4 else \
                    data[y * stride + x]
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
    drawn = {f"shiroko-{n}": r["generated"] for n, r in reports.items() if r.get("generated")}
    if drawn:
        notice += ["", f"Generated glyphs ({', '.join(f'{k}: {v}' for k, v in drawn.items())}) are drawn by "
                   "tools/fontpack/sprite. Generated glyph rules follow Ghostty (MIT) sprite code."]
        inventory["generated"] = drawn
    (lic_dir / "Unicode-3.0.txt").write_bytes((ROOT / "LICENSES/Unicode-3.0.txt").read_bytes())
    used = sorted({n for r in reports.values() for n in r["order_inputs"] if n in LOCK["order"]})
    inventory["order_inputs"] = {n: {k: v for k, v in LOCK["order"][n].items() if k != "cache"} for n in used}
    if any(CONFIG["packages"][n]["role"] == "nerd" for n in names):
        pin = LOCK["nerd_rules"]
        inventory["nerd_rules"] = {"version": pin["version"], "use": "parsed for the Nerd glyph sizes, not packaged",
                                   "files": [{"path": f, "url": v["url"], "sha256": v["sha256"]}
                                             for f, v in LOCK["files"].items()
                                             if f == pin["patcher"] or f.startswith(pin["glyphs"])]}
    if "nerd_rules" in inventory:
        notice += ["", "Nerd glyph sizes follow the rules of nerd-fonts font-patcher v3.4.0 (MIT, Copyright (c) 2014 Ryan L "
                   "McIntyre; LICENSES/nerd/nerd-fonts-LICENSE), placed as Ghostty (MIT, "
                   "https://github.com/ghostty-org/ghostty) places them."]
    notice += ["", "Unicode data: Copyright (c) Unicode, Inc. Unicode License v3 (LICENSES/Unicode-3.0.txt)."
               + (" Unihan (Unihan.zip) orders CJK glyphs." if "Unihan.zip" in used else ""), ""]
    if "korean-frequency-2005.zip" in used:
        ko = LOCK["order"]["korean-frequency-2005.zip"]
        notice += ["The Hangul glyphs are ordered by the syllable frequencies of 현대 국어 사용 빈도 조사 2 (National "
                   f"Institute of Korean Language, 2005), {ko['license']}, {ko['license_url']}:", ko["attribution"], ""]
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


def build(names, size, out=None, keep=None, method="zstd", store_index=False, order="hot", ranked=None):
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
    generated, undrawn = generated_scalars(profile)
    if set(generated) - set(resolve_set):
        sys.exit(f"generated scalars for unknown packages {sorted(set(generated) - set(resolve_set))}")
    providers = resolve_providers(profile, resolve_set, faces, generated)
    gaps = unserved(profile, resolve_set, faces, providers) if keep is None else {}
    out.mkdir(parents=True, exist_ok=True)
    stage = pathlib.Path(tempfile.mkdtemp(prefix=".stage-", dir=out))
    try:
        reports = {}
        for name in names:
            blob, report = build_package(name, CONFIG["packages"][name], profile, faces, providers[name], ivd, tools,
                                         size, keep, method, store_index, order, ranked, generated.get(name, set()))
            if undrawn.get(name):
                report["generated_undrawn"] = [f"{cp:04X}" for cp in sorted(undrawn[name])]
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
            print(f"{out / f'shiroko-{name}.shrf'}: {report['file_bytes']} bytes (index {report['index_bytes']}), "
                  f"{report['pages']} pages of {report['shape']} ({report['hot_pages']} hot"
                  f"{''.join('; ' + n for n in report['order_inputs'])}), "
                  f"atlas fill {report['atlas_fill']:.1%}, "
                  f"{report['scalars']} scalars ({report['generated']} generated), {report['sequences']} sequences, "
                  f"clipped {report['clipped']}")
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
    blob, report = build_package("builtin", pkg, profile, {face_id: face}, cps, set(), tools, size, cps, "stored")
    try:
        Package(blob, profile.id, profile.max_scalars)
    except FormatError as e:
        sys.exit(f"builtin: built package is invalid: {e}")
    lines = [f"/* Generated by tools/fontpack/fontpack.py builtin from {face_id} "
             f"({LOCK['faces'][face_id]['sha256'][:16]}), {spdx_expression(LOCK['licenses'][latin['license']])}. */",
             "#include <stddef.h>", "#include <stdint.h>", "",
             f"_Alignas({PAGE_ALIGN}) const uint8_t shr__builtin_package[{len(blob)}] = {{"]
    lines += ["  " + ", ".join(f"0x{b:02X}" for b in blob[i:i + 16]) + "," for i in range(0, len(blob), 16)]
    lines += ["};", f"const size_t shr__builtin_package_size = {len(blob)};"]
    out = pathlib.Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    write_atomic(out, ("\n".join(lines) + "\n").encode())
    print(f"{out}: built-in package {size['cell_width']}x{size['line_height']}, {len(blob)} bytes, "
          f"{report['pages']} pages of {report['shape']}")


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
    inputs = {n for v in inventory["packages"].values() for n in v["report"].get("order_inputs", ())}
    inventory["order_inputs"] = {k: v for k, v in inventory.get("order_inputs", {}).items() if k in inputs}
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


def resealed(b):
    """Recomputes the page, index entry, index and header checksums of a package."""
    b = bytearray(b)
    sidx, sidx_size = struct.unpack_from("<Q", b, 40)[0], struct.unpack_from("<I", b, 28)[0]
    entries = range(sidx + 24, sidx + sidx_size, ENTRY.size)
    for e in entries:
        t, _, _, off, size = ENTRY.unpack_from(b, e)[:5]
        for r in range(off + 16, off + size, PTAB.size) if t == b"ptab" else ():
            poff, _, psize = struct.unpack_from("<QQI", b, r)
            struct.pack_into("<Q", b, r + 8, xxh3(bytes(b[poff:poff + psize])))
    for e in entries:
        off, size = struct.unpack_from("<QI", b, e + 8)
        struct.pack_into("<Q", b, e + 24, xxh3(bytes(b[off:off + size])))
    struct.pack_into("<Q", b, 80, xxh3(bytes(b[sidx:sidx + sidx_size])))
    struct.pack_into("<Q", b, 120, xxh3(bytes(b[:120])))
    return bytes(b)


def raw_frame(data):
    """A valid zstd frame of raw blocks, larger than its content."""
    out = ZSTD_MAGIC + struct.pack("<BI", 0xA0, len(data))
    for at in range(0, len(data), 1 << 17):
        block = data[at:at + (1 << 17)]
        out += (len(block) << 3 | (at + len(block) >= len(data))).to_bytes(3, "little") + block
    return out


def reader_selftest(blob, profile):
    """The reader rejects a package breaking any one box, frame, page or atlas rule, and skips an unknown
    optional box."""
    pkg = Package(blob, *profile)
    if pkg.inst[2] != FMT_A4:
        sys.exit("selftest needs an A4 package")
    aw, ah, stride = pkg.atlas
    content = pkg.content()
    profile_id, features, meta, pages = content
    last = len(pages) - 1
    count, height, atlas, recs = pages[last]
    if height >= ah:
        sys.exit("selftest needs a package whose last page is trimmed")
    glyphs = [(n, i, RECORD.unpack_from(p[3], 16 * i)) for n, p in enumerate(pages) for i in range(p[0])]
    odd, g, (x, y, w, h) = next((n, i, r[:4]) for n, i, r in glyphs if r[2] % 2 and r[0] + r[2] + 3 <= aw)
    lg, lh = next((i, r[3]) for n, i, r in glyphs if n == last)
    tri = meta[b"ctri"]
    l2n, dn = struct.unpack_from("<II", tri)
    d = len(tri) - 64 * dn
    some = min(pkg.scalars)

    def zstd_pkg(**kw):
        return assemble(*content, **kw)

    def stored_pkg(**kw):
        return assemble(*content, method="stored", **kw)

    def page(fn, kind=zstd_pkg):
        """The last page's streams changed by fn(raws, streams), which must be zstd (or, for stored_pkg, stored)."""
        def tamper(t, i, raws, m, streams):
            if t == b"page" and i == last:
                if m != (ZSTD if kind is zstd_pkg else STORED):
                    sys.exit("selftest needs a last page that compresses")
                return fn(raws, streams)
            return m, streams
        return lambda: kind(tamper=tamper)

    def ptab(at, value, fmt):
        def tamper(t, i, raws, m, streams):
            if t != b"ptab":
                return m, streams
            p = bytearray(streams[0])
            struct.pack_into(fmt, p, PTAB.size * last + at, value)
            return m, [bytes(p)]
        return lambda: stored_pkg(tamper=tamper)

    def with_page(n, a=None, r=None):
        p = pages[n]
        return assemble(profile_id, features, meta, pages[:n] + [p[:2] + (a or p[2], r or p[3])] + pages[n + 1:])

    def poked(data, at, value, fmt="<H"):
        b = bytearray(data)
        struct.pack_into(fmt, b, at, value)
        return bytes(b)

    def with_meta(t, data):
        return lambda: assemble(profile_id, features, meta | {t: data}, pages)

    def edit(fn, reseal=True, base=zstd_pkg):
        def build():
            b = bytearray(base())
            fn(b)
            return resealed(b) if reseal else bytes(b)
        return build

    def inserted(b, at):
        """A 16-byte free box at `at`, every listed box and page behind it moved."""
        p = box_offset(b, b"ptab")
        for r in [*range(HEADER_SIZE + 32, HEADER_SIZE + struct.unpack_from("<I", b, 28)[0], ENTRY.size),
                  *range(p + 16, p + 16 + PTAB.size * len(pages), PTAB.size)]:
            off = struct.unpack_from("<Q", b, r)[0]
            struct.pack_into("<Q", b, r, off + 16 * (off >= at))
        b[at:at] = BOX.pack(16, b"free", 0, 0, 0)
        struct.pack_into("<Q", b, 32, len(b))

    def appended(b):
        b += bytes(16)
        struct.pack_into("<Q", b, 32, len(b))

    def flip(at):
        return lambda b: b.__setitem__(at, b[at] ^ 1)

    rec, recs_odd, atlas_odd, pad = 16 * g, pages[odd][3], pages[odd][2], y * stride + (x + w) // 2
    cases = [
        ("v4 package", edit(lambda b: b.__setitem__(slice(0, 8), b"SHRFPKG1"), False), "format v4"),
        ("v5 package", edit(lambda b: struct.pack_into("<H", b, 16, 5)), "format v5"),
        ("header checksum", edit(flip(48), False), "header checksum"),
        ("unknown required feature", edit(lambda b: struct.pack_into("<I", b, 20, features | 16)),
         "unsupported required feature"),
        ("index checksum", edit(flip(HEADER_SIZE + 24 + 16), False), "index checksum"),
        ("unknown required box", lambda: zstd_pkg(extra=[(b"xtra", REQUIRED, b"x")]), "unsupported required box"),
        ("method 2 index box", lambda: zstd_pkg(tamper=lambda t, i, r, m, s: (2 if t == b"ctri" else m, s)),
         "unsupported method"),
        ("zstd without its feature bit", edit(lambda b: struct.pack_into("<I", b, 20, features)), "flags"),
        ("metadata box checksum", edit(lambda b: flip(box_offset(b, b"ctri") + 20)(b), False), "b'ctri' checksum"),
        ("missing inst", lambda: assemble(profile_id, features, {k: v for k, v in meta.items() if k != b"inst"},
                                          pages), "missing box"),
        ("ctri without level-2 blocks", with_meta(b"ctri", poked(tri, 0, 0, "<I")), "ctri header"),
        ("ctri reserved bytes", with_meta(b"ctri", poked(tri, 12, 1, "<I")), "ctri header"),
        ("ctri shorter than its blocks", with_meta(b"ctri", tri[:-64]), "ctri size"),
        ("ctri padding before the data blocks", with_meta(b"ctri", poked(tri, d - 1, 1, "<B")), "ctri padding"),
        ("L1 entry past the level-2 blocks", with_meta(b"ctri", poked(tri, 16 + 2 * 1087, l2n)), "block number"),
        ("level-2 entry past the data blocks", with_meta(b"ctri", poked(tri, CTRI_L2 + 128 * l2n - 2, dn)),
         "block number"),
        ("dirty level-2 block 0", with_meta(b"ctri", poked(tri, CTRI_L2 + 126, 1)), "ctri block 0"),
        ("dirty data block 0", with_meta(b"ctri", poked(tri, d + 60, BLANK, "<I")), "ctri block 0"),
        ("stored ctri off the 64-byte grid", edit(lambda b: inserted(b, box_offset(b, b"ctri")), base=stored_pkg),
         "ctri not aligned"),
        ("glyph value past the pages", with_meta(b"ctri", ctri_table(pkg.scalars | {some: len(pages) << 12})),
         "glyph value"),
        ("glyph value past the records", with_meta(b"ctri", ctri_table(pkg.scalars | {some: last << 12 | count})),
         "glyph value"),
        ("glyph value 0xFFFFFFFD", with_meta(b"ctri", ctri_table(pkg.scalars | {some: BLANK - 1})), "glyph value"),
        ("U+0020 with a bitmap", with_meta(b"ctri", ctri_table(pkg.scalars | {0x20: 0})), "U+0020"),
        ("page checksum", edit(flip(-1), False), "page %d checksum" % last),
        ("method 2 page", page(lambda r, s: (2, s)), "unsupported method"),
        ("truncated frame", page(lambda r, s: (ZSTD, [s[0][:-1], s[1]])), "zstd frame data"),
        ("bytes after the frame", page(lambda r, s: (ZSTD, [s[0], s[1] + b"\0"])), "zstd frame data"),
        ("wrong content size", page(lambda r, s: (ZSTD, [zstd(r[0] + bytes(1)), s[1]])), "zstd frame header"),
        ("compressed not smaller", page(lambda r, s: (ZSTD, [s[0], raw_frame(r[1])])), "not smaller"),
        ("legacy frame magic", page(lambda r, s: (ZSTD, [b"\x27\xb5\x2f\xfd" + s[0][4:], s[1]])),
         "zstd frame magic"),
        ("skippable frame", page(lambda r, s: (ZSTD, [struct.pack("<II", 0x184D2A50, 0) + s[0], s[1]])),
         "zstd frame magic"),
        ("stored page off the 256-byte grid",
         edit(lambda b: inserted(b, struct.unpack_from("<Q", b, box_offset(b, b"ptab") + 16 + PTAB.size * last)[0]),
              base=stored_pkg), "page %d not aligned" % last),
        ("stored page longer than its content", page(lambda r, s: (STORED, [s[0], s[1] + b"\0"]), stored_pkg),
         "stored size"),
        ("page glyph count", ptab(20, count + 1, "<H"), "cover"),
        ("page height above the shape", ptab(22, ah + 1, "<H"), "page record"),
        ("rect below the trimmed height", lambda: with_page(last, r=poked(recs, 16 * lg + 2, height - lh + 1)), "rect"),
        ("rect past the atlas width", lambda: with_page(odd, r=poked(recs_odd, rec, aw - w + 1)), "rect"),
        ("odd A4 x", lambda: with_page(odd, r=poked(recs_odd, rec, x + 1)), "rect"),
        ("padding nibble", lambda: with_page(odd, a=poked(atlas_odd, pad, atlas_odd[pad] | 1, "<B")), "padding"),
        ("record without bitmap", lambda: with_page(odd, r=poked(recs_odd, rec + 8, 1 << 2, "<B")), "flags"),
        ("page shape off the shape set", with_meta(b"inst", poked(meta[b"inst"], 36, 96)), "page shape"),
        ("reserved instance bytes", with_meta(b"inst", poked(meta[b"inst"], 47, 1, "<B")), "reserved"),
        ("box chain gap", edit(appended), "box chain"),
    ]
    if b"seqs" in meta:
        cases.append(("sequence glyph value", with_meta(b"seqs", poked(meta[b"seqs"], 8, len(pages) << 12, "<I")),
                      "glyph value"))
    for name, make, why in cases:
        try:
            Package(make(), *profile)
        except FormatError as e:
            if why not in str(e):
                sys.exit(f"selftest: {name}: rejected for another reason ({e})")
            continue
        sys.exit(f"selftest: {name}: accepted")
    for name, make in (("stored", stored_pkg), ("stored index", lambda: zstd_pkg(store_index=True)), ("zstd", zstd_pkg),
                       ("unknown optional box", lambda: zstd_pkg(extra=[(b"xtra", 0, b"x")]))):
        try:
            Package(make(), *profile)
        except FormatError as e:
            sys.exit(f"selftest: {name}: rejected ({e})")
    frames = [("window above 2^18", zstandard.ZstdCompressionParameters.from_level(3, window_log=19), 300000),
              ("frame checksum", zstandard.ZstdCompressionParameters.from_level(3, write_checksum=True), len(atlas)),
              ("no content size", zstandard.ZstdCompressionParameters.from_level(3, write_content_size=False),
               len(atlas))]
    for name, params, n in frames:
        data = bytes(n) if n != len(atlas) else atlas
        try:
            unzstd(zstandard.ZstdCompressor(compression_params=params).compress(data), n)
        except FormatError:
            continue
        sys.exit(f"selftest: {name}: accepted")


def sprite_selftest():
    """The generated glyphs keep their cell-size invariants (tools/fontpack/sprite/invariants) at every size of
    invariants.SIZES."""
    from sprite import invariants
    face = Face(CONFIG["packages"]["latin"]["faces"]["regular"])
    drawn = sprites()
    bad = invariants.check_all(lambda w, h: sprite.Metrics.from_face(face, w, h))
    if bad:
        sys.exit(f"selftest: generated glyph invariants: {len(bad)} violations\n  " + "\n  ".join(bad[:20]))
    print(f"generated glyphs: {len(drawn)} drawn, invariants hold at {len(invariants.SIZES)} cell sizes")


def icon_selftest():
    """Nerd glyph placement against the values of Ghostty's constraint test."""
    m = {"cell_width": 10, "cell_height": 22, "face_width": 9.6, "face_height": 21.12, "face_y": 0.2,
         "icon_height": 44.48 / 3}
    rules = icon_rules(Face(CONFIG["packages"]["nerd"]["faces"]["regular"]))
    cases = ((nerd_rules.DEFAULT, (6.784, 15.28, 1.408, 4.84), (6.784, 15.28, 1.408, 4.84)),
             (nerd_rules.FIT, (10.272, 10.272, 2.864, 5.304), (9.6, 9.6, 0, 5.64)),
             (rules[0xEA61], (9.015625, 13.015625, 3.015625, 3.76525), (7.2125, 10.4125, 0.8125, 5.950695224719102)),
             (rules[0xE0C0], (16.796875, 16.46875, -0.796875, 1.7109375), (10, 22, 0, 0)))
    for c, glyph, want in cases:
        got = nerd_rules.place(c, glyph, m)
        if any(abs(a - b) > 1e-6 * max(1, abs(b)) for a, b in zip(got, want)):
            sys.exit(f"selftest: Nerd glyph {glyph} placed at {got}, not {want}")


def selftest(packages):
    """Install recovery: a damaged payload is rewritten, the notices follow, activation advances. The reader
    rejects broken pages. The generated glyphs keep their invariants; Nerd glyphs are placed as Ghostty places them."""
    if CONFIG.get("generated"):
        sprite_selftest()
    icon_selftest()
    src = min(pathlib.Path(packages).glob("shiroko-*.shrf"), key=lambda p: p.stat().st_size, default=None)
    if not src:
        sys.exit(f"{packages}: no packages to test with; run `make fontpack` first")
    blob = src.read_bytes()
    reader_selftest(blob, pinned_profile())
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


def atlas(value):
    m = re.fullmatch(r"(\d+)x(\d+)", value)
    if not m or int(m[1]) not in SHAPE_SIDES or int(m[2]) not in SHAPE_SIDES:
        raise argparse.ArgumentTypeError(f"page atlas {value!r} is not WIDTHxHEIGHT with sides in {SHAPE_SIDES}")
    return [int(m[1]), int(m[2])]


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
    b.add_argument("--page-atlas", type=atlas)
    b.add_argument("--method", choices=("zstd", "stored"), default="zstd")
    b.add_argument("--store-index", action="store_true")
    b.add_argument("--order", choices=("hot", "codepoint"), default="hot")
    b.add_argument("--rank", type=pathlib.Path)
    v = sub.add_parser("verify")
    v.add_argument("files", nargs="+")
    bi = sub.add_parser("builtin")
    bi.add_argument("--cell", type=cell, required=True)
    bi.add_argument("--out", required=True)
    bi.add_argument("--page-atlas", type=atlas)
    i = sub.add_parser("install")
    i.add_argument("--dest", required=True)
    i.add_argument("files", nargs="+")
    t = sub.add_parser("selftest")
    t.add_argument("--packages", default=str(OUT))
    args = ap.parse_args()
    unknown = [n for n in args.packages if n not in CONFIG["packages"]] if args.cmd == "build" else []
    if unknown:
        b.error(f"unknown package {', '.join(unknown)} (known: {', '.join(sorted(CONFIG['packages']))})")
    if args.cmd in ("build", "builtin") and args.page_atlas:
        CONFIG["page_atlas"] = {"a4": args.page_atlas, "a8": args.page_atlas}
        CONFIG["page_shapes"] = {"a4": [args.page_atlas], "a8": [args.page_atlas]}
    check_locks()
    if args.cmd == "fetch":
        fetch()
    elif args.cmd == "build":
        ranked = {}
        for c in args.rank.read_text(encoding="utf-8") if args.rank else "":
            ranked.setdefault(ord(c), len(ranked))
        build(args.packages, args.cell, args.out, args.scalars, args.method, args.store_index, args.order, ranked)
    elif args.cmd == "builtin":
        builtin(args.cell, args.out)
    elif args.cmd == "verify":
        profile = pinned_profile()
        for f in args.files:
            pkg = verified(f, profile)
            print(f"{f}: ok ({pkg.nglyphs} glyphs, {len(pkg.pages)} pages of {pkg.atlas[0]}x{pkg.atlas[1]})")
    elif args.cmd == "selftest":
        selftest(args.packages)
    else:
        install(args.dest, args.files)


if __name__ == "__main__":
    main()
