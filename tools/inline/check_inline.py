#!/usr/bin/env python3
"""Hot functions may call only the functions their toolchain's list allows; any other call is a helper that stopped
being inlined (clang's cold call-site limit took set_cell's helpers out of line in 1bb107e).

  check_inline.py [--objdump CMD] [--record] LIST BUILD_DIR

LIST lines: `SOURCE FUNCTION CALLEE...` (calls and tail calls, read from BUILD_DIR's object of SOURCE), `SOURCE FUNCTION
inlined` (no out-of-line copy). A `~CALLEE` is a known exception: reported while the call is there. --record rewrites
LIST from the build, keeping its comments and known exceptions.
"""

import argparse
import pathlib
import re
import subprocess
import sys

HEAD = re.compile(r"^[0-9a-f]+ <(.+)>:$")
RELOC = re.compile(r"\s(?:ARM64_RELOC_BRANCH26|R_X86_64_PLT32|R_RISCV_CALL|R_RISCV_CALL_PLT|R_RISCV_JAL)\s+(\S+)$")
INSN = re.compile(r"\t(?:bl|b|call|callq|jmp|jmpq|jal|j|tail)\s.*<([^>+]+)>$")
SUFFIX = re.compile(r"\.(?:isra|constprop|part|cold|lto_priv)(?:\.\d+)?")


def base(name, macho):
    name = SUFFIX.sub("", re.sub(r"[-+]0x[0-9a-f]+$", "", name).removesuffix("@plt"))
    return name[1:] if macho and name.startswith("_") else name


def calls(objdump, obj):
    """function -> the functions it calls, for every function of the object."""
    out = subprocess.run([*objdump.split(), "-dr", "--no-show-raw-insn", str(obj)], check=True, capture_output=True,
                         text=True).stdout
    macho = "mach-o" in out[:500]
    found, fn = {}, None
    for line in out.splitlines():
        if (m := HEAD.match(line)) and not m[1].startswith((".", "ltmp")):
            fn = base(m[1], macho)
            found.setdefault(fn, set())
            continue
        m = RELOC.search(line) or INSN.search(line)
        if m and fn in found:
            to = base(m[1], macho)
            if to != fn and not to.startswith((".", "ltmp")):
                found[fn].add(to)
    return found


def object_of(build, source):
    hits = [p for p in build.rglob(pathlib.Path(source).name + ".o*") if p.suffix in (".o", ".obj")
            and p.as_posix().removesuffix(p.suffix).endswith("/" + source) and "/__/" not in p.as_posix()]
    if len(hits) != 1:
        sys.exit(f"check_inline: {len(hits)} objects for {source} in {build}")
    return hits[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--record", action="store_true")
    ap.add_argument("list", type=pathlib.Path)
    ap.add_argument("build", type=pathlib.Path)
    a = ap.parse_args()
    lines = a.list.read_text().splitlines()
    entries = [line.split() for line in lines if line.strip() and not line.startswith("#")]
    objects = {}
    fails, notes, known = [], [], []
    for source, fn, *allowed in entries:
        if source not in objects:
            objects[source] = calls(a.objdump, object_of(a.build, source))
        now = objects[source].get(fn)
        if allowed == ["inlined"]:
            if now is not None:
                fails.append(f"{fn}: out of line now")
            continue
        if now is None:
            fails.append(f"{fn}: no symbol (inlined or renamed)")
            continue
        exc = {c[1:] for c in allowed if c.startswith("~")}
        ok = {c for c in allowed if not c.startswith("~")}
        fails += [f"{fn} -> {c}: not allowed" for c in sorted(now - ok - exc)]
        known += [f"{fn} -> {c}" for c in sorted(exc & now)]
        notes += [f"known gone: {fn} -> {c} (rerun with --record)" for c in sorted(exc - now)]
        notes += [f"fewer: {fn} -> {c} no longer called" for c in sorted(ok - now)]
    name = a.list.stem
    if a.record:
        rec = [line for line in lines if not line.strip() or line.startswith("#")]
        for source, fn, *allowed in entries:
            now = objects[source].get(fn)
            exc = {c[1:] for c in allowed if c.startswith("~")}
            got = ["inlined"] if now is None else sorted(now - exc) + sorted(f"~{c}" for c in exc & now)
            rec.append(" ".join([source, fn, *got]))
        a.list.write_text("\n".join(rec) + "\n")
        print(f"inline {name}: recorded {len(entries)} functions into {a.list}")
        return 0
    for line in fails + notes:
        print(f"inline {name}: {line}")
    print(f"inline {name}: {len(entries)} functions, {len(fails)} not allowed"
          + (f"; known: {', '.join(known)}" if known else ""))
    if fails:
        print(f"inline {name}: inline the new callees, or rerun with --record and review the list diff")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
