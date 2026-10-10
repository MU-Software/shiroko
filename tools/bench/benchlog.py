#!/usr/bin/env python3
"""Tables from the replay benchmark's lines (shiroko_replay, the Tab5 example built with TAB5_REPLAY=1).

  benchlog.py table LOG       per scene: the scene measurement (R lines, Tab5 only), per replayed driver the frame
                              p50 / MAX / busy over its reps (median, with the spread from smallest to largest) and the
                              batch kinds, the compositor replay's frame and its api / submit / build; checksums
  benchlog.py compare A B     per scene: recording hashes (commands and calls), recorded checksums and record-pass
                              checksums of two logs (e.g. shiroko_replay on the Mac against the Tab5), the scene
                              checksum where a log has one; what both logs have is compared, a recording hash being the
                              REPLAY CHECKSUMS line's or, without one, the last of the RH line
  benchlog.py cost TAB5 [MAC] the app cost table (README): per cost scene the Tab5 scene measurement and the replayed
                              parts, and the same replays on the desktop
"""

import argparse
import collections
import re
import statistics
import sys

FLAGS = {"ok", "DIFFER", "FULL", "EQUAL", "->"}
DRIVERS = ("driver-sw", "driver-p4")
COST = {"cost-cell": "One cell", "cost-row": "One row ({cols} cells)", "cost-screen": "Every cell ({cells})",
        "cost-scroll": "Scroll one line up, write the new line", "cost-restyle": "One row bold and italic, on and off",
        "cost-sprite": "One 96 x 96 sprite moved", "churn-ko": "1000 cells of Hangul text"}


def fields(line):
    """key=value and `name value` pairs of a line; the first value of a name wins."""
    t, d, i = line.split(), {}, 0
    while i < len(t):
        if t[i] in FLAGS:
            d.setdefault("status", t[i])
            i += 1
        elif "=" in t[i]:
            k, v = t[i].split("=", 1)
            d.setdefault(k, v)
            i += 1
        elif i + 1 < len(t):
            d.setdefault(t[i], t[i + 1])
            i += 2
        else:
            i += 1
    return d


def triple(v):
    return [float(x) for x in v.split("/")]


def parse(path):
    scenes, reps, checks, passes = collections.defaultdict(list), collections.defaultdict(list), {}, {}
    calls = {}
    for line in open(path, errors="replace"):
        s = line.strip()
        if not s.startswith(("R rep=", "B rep=", "REPLAY ", "RP rec=", "RH rec=")):
            continue
        d = fields(s.split(None, 2 if s.startswith("REPLAY ") else 1)[-1])
        if s.startswith("R rep="):
            scenes[d["load"]].append(d)
        elif s.startswith("B rep="):
            reps[d["rec"], d["part"]].append(d)
        elif s.startswith("REPLAY CHECKSUMS"):
            d["status"] = s.split()[-1]
            checks[d["rec"]] = d
        elif s.startswith("REPLAY CALLS"):
            d["status"] = s.split()[-1]
            calls[d["rec"]] = d
        elif s.startswith("RP rec="):
            passes.setdefault(d["rec"], {}).update(d)
        elif s.startswith("RH rec=") and len(s.split()) > 2:
            passes.setdefault(s.split()[1][4:], {})["rec-hash"] = s.split()[-1]
    return scenes, reps, checks, passes, calls


def max1(r):
    """An R line's MAX over loop frames 1.. (older logs: 0..)."""
    return float(r.get("max1", r["max"]))


def spread(xs):
    return f"{statistics.median(xs):.2f} ({min(xs):.2f}-{max(xs):.2f})"


def med(b, key, i=0):
    """Median over reps of value i of a p50/p95/max triple (or of a plain number)."""
    return statistics.median(triple(x[key])[i] if "/" in x[key] else float(x[key]) for x in b)


def table(args):
    scenes, reps, checks, _, calls = parse(args.log)
    parts = sorted({p for _, p in reps if p in DRIVERS})
    comp = any(p == "compositor" for _, p in reps)
    order = list(dict.fromkeys(r for r, _ in reps))
    head = ["scene", "scene p50 / MAX f1.. / busy p50"]
    for p in parts:
        head += [f"{p} p50", f"{p} MAX f0..", f"{p} busy p50 / busy-MAX f0..", f"{p} lead / raster / rotate / wait p50"]
    if comp:
        head += ["compositor p50", "compositor MAX f0..", "api / submit / build p50", "cells / scroll / layer / image p50"]
    head.append("checksums")
    print("| " + " | ".join(head) + " |")
    print("|" + "---|" * len(head))
    for sc in order:
        r = scenes.get(sc, [None])[0]
        row = [sc, f"{triple(r['frame'])[0]:.2f} / {max1(r):.2f} / {triple(r['busy'])[0]:.2f}" if r else "-"]
        for p in parts:
            b = reps.get((sc, p), [])
            if not b:
                row += ["-"] * 4
                continue
            row.append(spread([triple(x["frame"])[0] for x in b]))
            row.append(spread([float(x["max"]) for x in b]))
            row.append(f"{med(b, 'busy'):.2f} / {med(b, 'busy-max'):.2f}")
            row.append(" / ".join(f"{med(b, k):.2f}" for k in ("lead", "raster", "rotate", "wait")))
        if comp:
            b, a = reps.get((sc, "compositor"), []), reps.get((sc, "api"), [])
            row += [spread([triple(x["frame"])[0] for x in b]), spread([float(x["max"]) for x in b]),
                    " / ".join(f"{med(b, k):.2f}" for k in ("api", "submit", "build")),
                    " / ".join(f"{med(a, k):.2f}" for k in ("cells", "scroll", "layer", "image"))] if b else ["-"] * 4
        status = [d[sc]["status"] for d in (checks, calls) if sc in d]
        row.append(" ".join(status) or "-")
        print("| " + " | ".join(row) + " |")
    print("\nms; p50 of each rep from loop frame 10 on; MAX f1.. (the scene's R line, max1): the largest of loop frames "
          "1.. (frame 0 left out, as frames are judged); MAX f0.. (the replays' B lines): the largest of all loop "
          "frames; both leave out the frame before the loop. Replays as median (smallest-largest) over the reps; "
          "busy = frame less the device waits.")


def compare(args):
    a, b = parse(args.a), parse(args.b)
    print("| scene | rec-hash A | rec-hash B | calls-hash A | calls-hash B | record A / B | pass A / B | scene | verdict |")
    print("|---|---|---|---|---|---|---|---|---|")
    bad = unchecked = 0
    pa = {k: {**a[3].get(k, {}), **a[2].get(k, {})} for k in dict.fromkeys(list(a[3]) + list(a[2]))}
    pb = {k: {**b[3].get(k, {}), **b[2].get(k, {})} for k in dict.fromkeys(list(b[3]) + list(b[2]))}
    for sc in dict.fromkeys(list(pa) + list(pb)):
        ca, cb = pa.get(sc, {}), pb.get(sc, {})
        la, lb = a[4].get(sc, {}), b[4].get(sc, {})
        scene = ca.get("scene") or cb.get("scene") or "-"
        if not ca or not cb:
            print(f"| {sc} | only in {'A' if ca else 'B'} |")
            continue
        same = all(ca[k] == cb[k] for k in ("rec-hash", "record", "pass") if k in ca and k in cb)
        same &= la.get("calls-hash", "-") == lb.get("calls-hash", "-") or not la or not lb
        compared = "rec-hash" in ca and "rec-hash" in cb
        bad += not same
        unchecked += not compared
        verdict = "DIFFER" if not same else "same" if compared else "same, recording hash not compared"
        print(f"| {sc} | {ca.get('rec-hash', '-')} | {cb.get('rec-hash', '-')} | {la.get('calls-hash', '-')} | "
              f"{lb.get('calls-hash', '-')} | {ca.get('record', '-')} / {cb.get('record', '-')} | "
              f"{ca.get('pass', '-')} / {cb.get('pass', '-')} | {scene} | {verdict} |")
    if unchecked:
        print(f"\n{unchecked} scene(s) without a recording hash in both logs")
    return 1 if bad else 0


def small(v):
    return "<0.01" if v < 0.005 else f"{v:.2f}"


def grid(path):
    """{cols} and {cells} of the example's landscape grid, from its boot line (portrait panel size, cell size)."""
    for line in open(path, errors="replace"):
        s, c = re.search(r"^shiroko on .*: (\d+)x(\d+) ", line), re.search(r" cells (\d+)x(\d+)", line)
        if s and c:
            h, v, cw, ch = map(int, s.groups() + c.groups())
            return {"cols": v // cw, "cells": v // cw * (h // ch)}
    return {"cols": "?", "cells": "?"}


def cost(args):
    t, m = parse(args.tab5), parse(args.mac) if args.mac else None
    g = grid(args.tab5)
    head = ["Change per frame", "Frame p50 / MAX", "Calls", "Submit", "Build", "Driver busy p50 / MAX"]
    if m:
        head.append("M4 Max (µs)")
    print("| " + " | ".join(head) + " |")
    print("|---|" + "---:|" * (len(head) - 1))
    for sc, name in COST.items():
        rs, c, d = t[0].get(sc, []), t[1].get((sc, "compositor"), []), t[1].get((sc, "driver-p4"), [])
        if not rs or not c:
            continue
        row = [name.format(**g), f"{statistics.median(triple(r['frame'])[0] for r in rs):.1f} / "
                     f"{max(max1(r) for r in rs):.1f}"]
        row += [small(med(c, k)) for k in ("api", "submit", "build")]
        row.append(f"{med(d, 'busy'):.1f} / {med(d, 'busy-max'):.1f}" if d else "-")
        if m:
            mc, md = m[1].get((sc, "compositor"), []), m[1].get((sc, "driver-sw"), [])
            row.append(f"{1000 * (med(mc, 'frame') + med(md, 'lead') + med(md, 'raster')):.0f}" if mc and md else "-")
        print("| " + " | ".join(row) + " |")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("table")
    t.add_argument("log")
    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    k = sub.add_parser("cost")
    k.add_argument("tab5")
    k.add_argument("mac", nargs="?")
    args = ap.parse_args()
    return {"table": table, "compare": compare, "cost": cost}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
