#!/usr/bin/env python3
"""Generate the fixed Unicode tables of the Shiroko text profile.

Inputs are pinned by fonts/text_profile.lock.json and cached in $SHIROKO_UCD_CACHE (.cache/ucd/).

  gen_unicode_tables.py --out FILE   write the C tables (property table, emoji sequences, profile id) from the
                                     cached inputs; never downloads (the build writes <build dir>/generated/)
  gen_unicode_tables.py --fetch      download and verify the inputs only
"""

import argparse
import bisect
import hashlib
import io
import json
import os
import pathlib
import struct
import sys
import zipfile

from fontTools.ttLib import TTFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
LOCK = ROOT / "fonts" / "text_profile.lock.json"
CACHE = pathlib.Path(os.environ.get("SHIROKO_UCD_CACHE") or ROOT / ".cache" / "ucd")

MAX_CP = 0x10FFFF

# Must match cores/shared/shr_unicode.h
GCB = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
       "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT"]
INCB = {"None": 0, "Linker": 1, "Consonant": 2, "Extend": 3}
F_DI, F_EXTPICT, F_EMOJI, F_EPRES, F_WIDE, F_MARK, F_CC, F_CO, F_EMOD, F_NERD, F_CJK = (
    1 << 6, 1 << 7, 1 << 8, 1 << 9, 1 << 10, 1 << 11, 1 << 12, 1 << 13, 1 << 14, 1 << 15, 1 << 16)


def load_lock():
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    if lock.get("format") != 1:
        sys.exit(f"{LOCK}: unsupported format {lock.get('format')}")
    if lock["cluster_max_bytes"] < 4 * lock["cluster_max_scalars"]:
        sys.exit("cluster_max_bytes must be at least 4 × cluster_max_scalars")
    if not 2 <= lock["cluster_max_scalars"] <= 16 or not 1 <= lock["tab_stop"] <= 64:
        sys.exit("cluster_max_scalars must be 2..16 (runtime key buffers) and tab_stop 1..64")
    return lock


def fetch(lock):
    import pooch

    for name, src in lock["sources"].items():
        pooch.retrieve(src["url"], f"sha256:{src['sha256']}", fname=name, path=CACHE, progressbar=False)


def verify_inputs(lock):
    for name, src in lock["sources"].items():
        path = CACHE / name
        if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != src["sha256"]:
            sys.exit(f"missing or modified input {path}; run `make fontpack-fetch`")


def data_lines(name):
    for raw in (CACHE / name).read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            yield [f.strip() for f in line.split(";")]


def cp_range(field):
    if ".." in field:
        lo, hi = field.split("..")
        return int(lo, 16), int(hi, 16)
    v = int(field, 16)
    return v, v


def build_props(lock):
    gcb = bytearray(MAX_CP + 1)
    flags = [0] * (MAX_CP + 1)

    def set_flag(lo, hi, bit):
        for cp in range(lo, hi + 1):
            flags[cp] |= bit

    for f in data_lines("GraphemeBreakProperty.txt"):
        lo, hi = cp_range(f[0])
        v = GCB.index(f[1])
        for cp in range(lo, hi + 1):
            gcb[cp] = v

    for f in data_lines("DerivedCoreProperties.txt"):
        lo, hi = cp_range(f[0])
        if f[1] == "Default_Ignorable_Code_Point":
            set_flag(lo, hi, F_DI)
        elif f[1] == "InCB":
            v = INCB[f[2]]
            for cp in range(lo, hi + 1):
                flags[cp] = (flags[cp] & ~0x30) | (v << 4)

    emoji_bits = {"Emoji": F_EMOJI, "Emoji_Presentation": F_EPRES,
                  "Emoji_Modifier": F_EMOD, "Extended_Pictographic": F_EXTPICT}
    for f in data_lines("emoji-data.txt"):
        if f[1] in emoji_bits:
            lo, hi = cp_range(f[0])
            set_flag(lo, hi, emoji_bits[f[1]])

    # EastAsianWidth.txt lists every non-N value explicitly; @missing is N.
    for f in data_lines("EastAsianWidth.txt"):
        if f[1] in ("W", "F"):
            lo, hi = cp_range(f[0])
            set_flag(lo, hi, F_WIDE)

    gc_bits = {"Mn": F_MARK, "Mc": F_MARK, "Me": F_MARK, "Cc": F_CC, "Co": F_CO}
    first = None
    decomp = {}
    for f in data_lines("UnicodeData.txt"):
        cp = int(f[0], 16)
        name, gc = f[1], f[2]
        if f[5] and not f[5].startswith("<"):
            decomp[cp] = [int(x, 16) for x in f[5].split()]
        if name.endswith(", First>"):
            first = cp
            continue
        lo = first if name.endswith(", Last>") else cp
        first = None
        if gc in gc_bits:
            set_flag(lo, cp, gc_bits[gc])

    for cp in nerd_scalars(lock):
        if flags[cp] & F_CO:
            flags[cp] |= F_NERD

    cjk = lock["cjk_class"]
    for f in data_lines("Scripts.txt"):
        if f[1] in cjk["scripts"]:
            lo, hi = cp_range(f[0])
            set_flag(lo, hi, F_CJK)
    for r in cjk["ranges"]:
        lo, hi = cp_range(r)
        set_flag(lo, hi, F_CJK)

    props = [gcb[cp] | flags[cp] for cp in range(MAX_CP + 1)]
    return props, decomp


def nerd_scalars(lock):
    nf = lock["nerd_font"]
    data = zipfile.ZipFile(CACHE / nf["file"]).read(nf["member"])
    if hashlib.sha256(data).hexdigest() != nf["sha256"]:
        sys.exit(f"sha256 mismatch: {nf['member']}")
    return set(TTFont(io.BytesIO(data), lazy=True).getBestCmap())


def parse_seq(field):
    return tuple(int(x, 16) for x in field.split())


def build_sequences():
    seqs = set()
    for f in data_lines("emoji-sequences.txt"):
        if ".." in f[0]:
            continue  # ranges are single scalars
        s = parse_seq(f[0])
        if len(s) > 1:
            seqs.add(s)
    for f in data_lines("emoji-zwj-sequences.txt"):
        seqs.add(parse_seq(f[0]))
    # emoji-test.txt adds the qualification aliases as exact keys.
    for f in data_lines("emoji-test.txt"):
        s = parse_seq(f[0])
        if len(s) > 1:
            seqs.add(s)
    vs_bases = set()
    for f in data_lines("emoji-variation-sequences.txt"):
        s = parse_seq(f[0])
        if len(s) == 2 and s[1] == 0xFE0F:
            vs_bases.add(s[0])
    return sorted(seqs), sorted(vs_bases)


def utf8_len(cp):
    return 1 if cp < 0x80 else 2 if cp < 0x800 else 3 if cp < 0x10000 else 4


def full_decomp(cp, decomp):
    if cp in decomp:
        out = []
        for c in decomp[cp]:
            out.extend(full_decomp(c, decomp))
        return out
    return [cp]


def required_limits(seqs, decomp):
    reports = []
    s = max(seqs, key=len)
    b = max(seqs, key=lambda q: sum(map(utf8_len, q)))
    reports.append(("emoji sequence (scalars)", len(s), sum(map(utf8_len, s))))
    reports.append(("emoji sequence (bytes)", len(b), sum(map(utf8_len, b))))
    reports.append(("modern Hangul L+V+T", 3, 9))
    latin = [cp for cp in list(range(0xC0, 0x250)) + list(range(0x1E00, 0x1F00))
             if cp in decomp]
    worst = max((full_decomp(cp, decomp) for cp in latin),
                key=lambda d: sum(map(utf8_len, d)))
    reports.append(("Latin NFD", len(worst), sum(map(utf8_len, worst))))
    reports.append(("UVS/IVS base+selector", 2, 8))
    return reports


def text_profile_id(lock, starts, values, seqs, vs_bases):
    """Hash of everything that changes text behaviour: rules, limits and the generated tables."""
    h = hashlib.sha256()
    h.update(json.dumps([lock["unicode_version"], lock["rules_version"], lock["tab_stop"],
                         lock["cluster_max_scalars"], lock["cluster_max_bytes"]]).encode())
    for table in (starts, values, vs_bases, [len(s) for s in seqs], [c for s in seqs for c in s]):
        h.update(struct.pack(f"<I{len(table)}I", len(table), *table))
    return h.hexdigest()


def tables(lock):
    props, decomp = build_props(lock)
    starts = [cp for cp, v in enumerate(props) if cp == 0 or v != props[cp - 1]]
    values = [props[cp] for cp in starts]
    seqs, vs_bases = build_sequences()
    limits = required_limits(seqs, decomp)
    for name, sc, by in limits:
        if sc > lock["cluster_max_scalars"] or by > lock["cluster_max_bytes"]:
            sys.exit(f"{name} needs {sc} scalars/{by} bytes; raise the lock limits "
                     "explicitly and bump the profile")
    return props, decomp, starts, values, seqs, vs_bases, limits


def prop_table(props, starts, values):
    """cp >> 10 picks a level-2 block in L1, cp >> 4 & 63 a data block in it, cp & 15 an index into the distinct
    values. Identical blocks are shared; level-2 block 0 and data block 0 are all 0, value 0 is Other."""
    distinct = sorted(set(values))
    if distinct[0] != 0 or len(distinct) > 256:
        sys.exit(f"{len(distinct)} distinct property values; the data blocks hold u8 indices with value 0 first")
    index = {v: i for i, v in enumerate(distinct)}
    data, data_ids, l2, l2_ids, l1 = [], {}, [], {}, []

    def share(block, blocks, ids):
        if block not in ids:
            ids[block] = len(blocks)
            blocks.append(block)
        return ids[block]

    share(bytes(16), data, data_ids)
    share((0,) * 64, l2, l2_ids)
    for hi in range(0, MAX_CP + 1, 1024):
        block = tuple(share(bytes(index[props[cp]] for cp in range(mid, mid + 16)), data, data_ids)
                      for mid in range(hi, hi + 1024, 16))
        l1.append(share(block, l2, l2_ids))
    if len(l2) > 65536 or len(data) > 65536:
        sys.exit("property table blocks exceed u16 numbers")
    for cp in range(MAX_CP + 1):
        got = distinct[data[l2[l1[cp >> 10]][cp >> 4 & 63]][cp & 15]]
        want = values[bisect.bisect_right(starts, cp) - 1]
        if got != want:
            sys.exit(f"property table mismatch at U+{cp:04X}: {got:#x} != {want:#x}")
    return l1, l2, data, distinct


def emit(lock, starts, values, seqs, vs_bases, limits, table):
    pid = text_profile_id(lock, starts, values, seqs, vs_bases)
    out = []
    w = out.append
    w("/* Generated by tools/unicode/gen_unicode_tables.py. Do not edit. */")
    w(f"/* Unicode {lock['unicode_version']}, text profile {pid} */")
    w("/* Unicode data: Copyright (c) Unicode, Inc., Unicode License v3. */")
    w("")
    w('#include "shr_unicode.h"')
    w("")
    w(f'const char shr__unicode_version[] = "{lock["unicode_version"]}";')
    w(f"const uint8_t shr__text_profile_id[32] = {{{', '.join(f'0x{pid[i:i + 2]}' for i in range(0, 64, 2))}}};")
    w(f"const uint32_t shr__text_rules_version = {lock['rules_version']};")
    w(f"const uint32_t shr__cluster_max_scalars = {lock['cluster_max_scalars']};")
    w(f"const uint32_t shr__cluster_max_bytes = {lock['cluster_max_bytes']};")
    w(f"const uint32_t shr__tab_stop = {lock['tab_stop']};")
    w("")
    l1, l2, data, distinct = table
    w(f"const uint16_t shr__uprop_l1[{len(l1)}] = {{")
    for i in range(0, len(l1), 16):
        w("  " + ", ".join(str(x) for x in l1[i:i + 16]) + ",")
    w("};")
    w(f"const uint16_t shr__uprop_l2[] = {{ /* {len(l2)} x 64 */")
    for block in l2:
        for i in range(0, 64, 16):
            w("  " + ", ".join(str(x) for x in block[i:i + 16]) + ",")
    w("};")
    w(f"const uint8_t shr__uprop_data[] = {{ /* {len(data)} x 16 */")
    for block in data:
        w("  " + ", ".join(str(x) for x in block) + ",")
    w("};")
    w("const uint32_t shr__uprop_values[] = {")
    for i in range(0, len(distinct), 10):
        w("  " + ", ".join(f"0x{x:05X}" for x in distinct[i:i + 10]) + ",")
    w("};")
    w("")
    pool, offs = [], []
    for s in seqs:
        offs.append((len(pool), len(s)))
        pool.extend(s)
    w(f"const uint32_t shr__emoji_seq_count = {len(seqs)};")
    w("const uint32_t shr__emoji_seq_pool[] = {")
    for i in range(0, len(pool), 8):
        w("  " + ", ".join(f"0x{x:05X}" for x in pool[i:i + 8]) + ",")
    w("};")
    w("const shr__seq_ref shr__emoji_seq_index[] = {")
    for i in range(0, len(offs), 6):
        w("  " + ", ".join(f"{{{o}, {n}}}" for o, n in offs[i:i + 6]) + ",")
    w("};")
    w("")
    w(f"const uint32_t shr__emoji_vs_base_count = {len(vs_bases)};")
    w("const uint32_t shr__emoji_vs_base[] = {")
    for i in range(0, len(vs_bases), 8):
        w("  " + ", ".join(f"0x{x:05X}" for x in vs_bases[i:i + 8]) + ",")
    w("};")
    w("")
    w("/* Required cluster lengths checked against the lock:")
    for name, sc, by in limits:
        w(f" *   {name}: {sc} scalars, {by} bytes")
    w(" */")
    return ("\n".join(out) + "\n").encode()


def write_atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser()
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--fetch", action="store_true")
    mode.add_argument("--out", type=pathlib.Path)
    args = ap.parse_args()
    lock = load_lock()
    if args.fetch:
        fetch(lock)
        return
    verify_inputs(lock)
    props, _, starts, values, seqs, vs_bases, limits = tables(lock)
    table = prop_table(props, starts, values)
    l1, l2, data, distinct = table
    write_atomic(args.out, emit(lock, starts, values, seqs, vs_bases, limits, table))
    size = 2 * len(l1) + 128 * len(l2) + 16 * len(data) + 4 * len(distinct)
    print(f"ranges={len(starts)} l2_blocks={len(l2)} data_blocks={len(data)} values={len(distinct)} "
          f"table={size} B sequences={len(seqs)} vs_bases={len(vs_bases)}")
    for name, sc, by in limits:
        print(f"  {name}: {sc} scalars, {by} bytes")
    print(f"profile={text_profile_id(lock, starts, values, seqs, vs_bases)}")


if __name__ == "__main__":
    main()
