#!/usr/bin/env python3
"""Writes fuzz seeds into OUT/<target>/seed-* (run by the build, target fuzz_seeds).

fuzz_package also starts from reduced real packages baked by the build for its cell size.
tests/fuzz/corpus/<target>/ holds only regression inputs.
"""
import pathlib
import struct
import sys

import xxhash

ROOT = pathlib.Path(sys.argv[1])


def u16(v):
    return struct.pack("<H", v & 0xFFFF)


def i8(*vs):
    return bytes(v & 0xFF for v in vs)


# fuzz_tilemap: cfg, then ops (see tests/fuzz/fuzz_tilemap.c).
def resize(rows, cols, font=0):
    return bytes([3 | font << 3, rows, cols])


def cell(row, col, s, span=1, flags=0, color=200):
    return bytes([0]) + i8(row, col) + bytes([span - 1, flags, color, len(s)]) + s


def text(row, col, s, wrap=True, run=None, flags=0, color=200):
    fl = (1 if wrap else 0) | (2 if run else 0)
    ra, rb = run or (0, 0)
    return bytes([1]) + i8(row, col) + bytes([fl, ra, rb, flags, color]) + u16(len(s)) + s + bytes([0x08, 50])


def clear(row, col, rows, cols, flags=0x80, color=90):
    return bytes([2]) + i8(row, col, rows, cols) + bytes([flags, color])


def scroll(top, bottom, n, flags=0x80, color=90):
    return bytes([2 | 8]) + i8(top, bottom, n, 0) + bytes([flags, color])


def measure(s, cols, wrap=True):
    return bytes([4]) + i8(cols) + bytes([1 if wrap else 0]) + u16(len(s)) + s


RENDER = bytes([5])


def tick(t):
    return bytes([6, t])


def move(x, y, hide=False):
    return bytes([7 | 8 | (0 if not hide else 16)]) + i8(x, y)


# fuzz_driver: cfg, width, height, stride padding, seed, then batches of [count, commands...].
def fill(rect, color=(255, 0, 0), dim=0):
    return bytes([1, dim]) + i8(*rect) + bytes(color) + bytes([0, 0, 0, 0]) + u16(0) + i8(0, 0)


def src_cmd(kind, rect, fmt, sw, sh, stride, length, origin=(0, 0)):
    return bytes([kind, 0]) + i8(*rect) + bytes([200, 100, 50, fmt]) + i8(sw, sh) + bytes([stride]) + u16(length) + i8(*origin)


# GLYPH (kind 2) or IMAGE (3) from `src` of buffer `buf`; GLYPH ends with an axis byte: as i8, or 40 times that
# beyond +-100.
def region(kind, rect, buf, src, origin=(0, 0), flags=0, axis=0):
    return (bytes([kind, flags]) + i8(*rect) + bytes([200, 100, 50, buf]) + i8(*src) + i8(*origin)
            + (i8(axis) if kind == 2 else b""))


# Buffer `buf` = w x h pixels at byte `off` of the source bytes; fmt as for src_cmd, | 0x40 DMA, | 0x80 DEVICE.
def register(buf, fmt, w, h, stride, off, length):
    return bytes([9, buf, fmt]) + i8(w, h) + bytes([off // 8, stride]) + u16(length)


def update(buf, rect):
    return bytes([10, buf]) + i8(*rect)


def release(buf):
    return bytes([11, buf])


def krelease(keep):
    return bytes([12, keep])


def kdraw(keep, rect, origin=(0, 0)):
    return bytes([8, 0]) + i8(*rect) + bytes([0, 0, 0, keep]) + i8(*origin)


def rotate(rot):
    return bytes([5, 0]) + i8(0, 0, 0, 0) + bytes([0, 0, 0, 0, 0, 0, 0]) + u16(0) + i8(0, 0) + bytes([rot])


# Keep group `keep` over `rect`, opened by a FILL of it.
def group(keep, rect, color=40):
    return bytes([6, keep]) + i8(*rect) + bytes([color])


END = bytes([7])


def batch(*cmds):
    return bytes([len(cmds) - 1 + sum(1 for c in cmds if c[0] == 6)]) + b"".join(cmds)


# fuzz_package: a sealed A4 header and empty box index of a `size`-byte package (another text profile).
def header(version, size):
    sidx = struct.pack("<I4sHHIII", 24, b"sidx", 0, 1, 8, 0, 0)
    head = struct.pack("<I4sHHIHHIIIQQ32sQ32x", 128, b"shrf", 0, 1, 112, version, 0, 1, 0, len(sidx), size, 128,
                       bytes(32), xxhash.xxh3_64_intdigest(sidx))
    return head + struct.pack("<Q", xxhash.xxh3_64_intdigest(head)) + sidx


# fuzz_compositor: setup, then [op, arg, extra...] (see tests/fuzz/fuzz_compositor.c).
def layer(slot, z, rect):
    return bytes([0, slot]) + i8(z, *rect)


def lfill(slot, rect, r=200):
    return bytes([5, slot, 6, slot]) + i8(*rect) + bytes([r]) + bytes([8, slot])


def image(slot, w, h):
    return bytes([9, slot, w - 1, h - 1])


def limage(slot, k, src, at):
    return bytes([5, slot, 7, slot, k]) + i8(*src, *at) + bytes([8, slot])


def limages(slot, *items):
    return bytes([5, slot]) + b"".join(bytes([7, slot, k]) + i8(*src, *at) for k, src, at in items) + bytes([8, slot])


def shift(slot, dy, drop=False):
    return bytes([14, slot | (0x80 if drop else 0)]) + i8(dy)


def update_image(k, rect):
    return bytes([10, k]) + i8(*rect)


SUBMIT, PUMP = bytes([12, 0]), bytes([13, 0])
ASYNC, SYNC, COMPLETE = bytes([23, 2]), bytes([23, 0]), bytes([23, 1])


def check(arg=0):
    return bytes([25, 0x10 | arg])


# fuzz_rows: cfg, then ops (see tests/fuzz/fuzz_rows.c); a row: op, head, key, then b x y f e per command.
def rcmd(kind, x0, w, y0, h, flags=0, glyph=0, invalid=False):
    x = next(v for v in range(256) if v % 9 == x0 and (v >> 4) % 5 == w and (v == 0xFF) == invalid)
    return bytes([kind + 8 * h, x, y0, flags, glyph | 0x40])


def rrow(g, cmds, key=(0, 0), pair=False, inplace=False):
    return bytes([g << 3 | inplace, len(cmds) | (0x80 if pair else 0), key[0], key[1]]) + b"".join(cmds)


def cached_row(g, glyph, key, flags=0):
    y = 16 * g
    return rrow(g, [rcmd(5, 0, 0, y, 16), rcmd(0, 0, 4, y, 16), rcmd(2, 1, 1, y, 16, flags, glyph),
                    rcmd(2, 2, 2, y, 16, flags | 8, glyph % 3 + 1), rcmd(6, 0, 0, 0, 0)], key, True, g % 2 == 1)


R_RENDER = bytes([3])


SEEDS = {
    "fuzz_tilemap": [
        # kept opaque rows whose inputs change one at a time (catches stale keeps)
        bytes([0x51]) + bytes([3 | 16, 1, 8]) + cell(0, 1, b"A", flags=0x80, color=0x11) + RENDER
        + cell(0, 1, b"A", flags=0x80, color=0x12) + RENDER + cell(0, 1, b"A", flags=0x80, color=0x22) + RENDER
        + cell(0, 1, b"A", 2, flags=0x80, color=0x22) + RENDER
        + clear(0, 0, 1, 8, flags=0) + cell(0, 2, b"A", 2, flags=0x80, color=0x22) + RENDER
        + b"".join(cell(0, 2, b"A", 2, flags=f, color=0x22) + RENDER for f in (0x81, 0x80, 0x82, 0x80))
        + cell(0, 2, b"A", 2, flags=0x88, color=0x22) + RENDER + cell(0, 2, b"B", 2, flags=0x88, color=0x22) + RENDER
        + bytes([3 | 16, 2, 8]) + RENDER,
        bytes([0]) + resize(3, 8) + text(0, 0, "A가😀B\tx\nwrap é".encode()) + RENDER,
        bytes([2]) + resize(3, 8) + cell(0, 0, b"$", flags=0x80) + cell(0, 1, "가".encode(), 2)
        + cell(1, 0, "👩‍💻".encode(), 2, 0x08) + cell(2, 7, b"", flags=0x80) + RENDER + cell(1, 1, b"x") + RENDER,
        bytes([1 | 2 | 0x30]) + resize(4, 10) + text(0, 0, b"row one\nrow two\nrow three", run=(4, 7)) + RENDER
        + move(-4, 8) + RENDER + clear(1, 2, 2, 4) + RENDER,
        bytes([4 | 8]) + resize(2, 8) + cell(0, 0, b"b", flags=0x20) + cell(1, 0, b"c", flags=0x60) + RENDER
        + tick(10) + RENDER + tick(10) + RENDER,
        bytes([2]) + resize(3, 8) + text(0, 0, b"abc") + RENDER + resize(2, 4, 1) + RENDER + resize(0, 0) + RENDER,
        bytes([2]) + resize(2, 6) + RENDER + cell(0, 5, "가".encode(), 2) + cell(5, 0, b"a") + cell(0, 0, b"\x1b")
        + cell(0, 0, b"\xe1\x80") + text(0, 0, b"x", flags=0xFF) + clear(0, 0, 9, 9) + RENDER,
        bytes([0]) + measure("👩‍💻🇰🇷👍🏽❤️❤A️⌚︎".encode(), 8) + measure(b"A\r\nB\rC\nD\n\n  ", 4)
        + measure("가".encode(), 1) + measure(b"e" + "́".encode() * 20, 8) + measure(b"x", -1)
        + measure(b"A\x1b[31mB", 8, False),
        # keeps in 1, 2 and 4 KiB: none, one and two 8-column rows held, so rows are evicted as they change
        *(bytes([k]) + resize(3, 8) + text(0, 0, b"one\ntwo\nsix") + RENDER + cell(0, 0, b"x") + RENDER
          + cell(2, 1, b"y", flags=0x80) + RENDER + cell(0, 0, b"o") + RENDER for k in (0x01, 0x11, 0x21)),
        # opaque rows scrolled as pixels up and down, a region drawn again, then everything cleared
        bytes([1 | 2 | 0x30]) + bytes([3 | 16, 3, 8]) + text(0, 0, "one 가\ntwo 😀\nsix".encode()) + RENDER
        + scroll(0, 3, 1) + cell(2, 0, b"n") + RENDER + scroll(0, 3, -1, 0) + RENDER + scroll(1, 3, 1) + RENDER
        + cell(0, 1, b"x") + scroll(0, 3, 1) + RENDER + scroll(0, 3, -9) + RENDER,
        # Fails while the layout extent misses the columns of a TAB piece that wraps (library bug, reported).
        bytes([0]) + measure(b"A\tB\t\tC", 3),
    ],
    "fuzz_driver": [
        bytes([0, 23, 19, 0, 1]) + batch(fill((0, 0, 24, 20)), fill((2, 2, 10, 10), dim=1)),
        bytes([1, 15, 15, 1, 2]) + batch(register(1, 0, 8, 8, 4, 0, 64), region(2, (1, 1, 9, 9), 1, (0, 0, 8, 8))),
        bytes([0, 10, 10, 2, 3]) + batch(register(2, 2, 4, 4, 16, 8, 64), region(3, (0, 0, 4, 4), 2, (0, 0, 4, 4)),
                                         src_cmd(4, (1, 1, 3, 3), 3, 4, 4, 8, 32)),
        bytes([1, 7, 11, 0, 4]) + batch(rotate(1)) + batch(rotate(2)),
        # the destination moved onto itself down and up, also with the rest left undefined, and inside a group
        bytes([0, 23, 19, 0, 11]) + batch(src_cmd(4, (0, 3, 24, 20), 250, 0, 0, 0, 0), src_cmd(4, (2, 0, 20, 15), 251, 0, 0, 0, 0, (0, 4)))
        + batch(group(1, (0, 0, 8, 8)), src_cmd(4, (0, 0, 8, 8), 250, 0, 0, 0, 0, (0, 1)), END),
        bytes([0, 5, 5, 0, 5]) + batch(fill((-3, -3, 30, 30)))
        + batch(register(1, 1, 2, 2, 2, 0, 4), region(2, (0, 0, 3, 3), 1, (0, 0, 2, 2))),
        # keeps stored and drawn in the batch storing them and later ones, also from a group reaching outside the
        # destination; then stored again, released, and drawn though gone
        bytes([2 | 4, 16, 12, 0, 6]) + batch(group(1, (0, 0, 16, 6)), fill((1, 1, 5, 5)), END, kdraw(1, (0, 0, 16, 6)))
        + batch(register(3, 1, 8, 8, 8, 16, 64), group(2, (0, 6, 16, 14)), region(2, (1, 7, 9, 11), 3, (0, 0, 8, 8)), END,
                kdraw(2, (4, 6, 12, 12), (4, 0)))
        + batch(kdraw(1, (2, 2, 6, 4), (2, 2)), kdraw(2, (0, 0, 16, 6), (0, 2)))
        + batch(group(1, (0, 0, 4, 4), 90), END, kdraw(1, (8, 8, 12, 12)), kdraw(2, (0, 6, 16, 12)))
        + batch(krelease(2), krelease(9), kdraw(1, (0, 0, 4, 4))) + batch(kdraw(2, (0, 0, 1, 1))),
        # bad keep batches: an END alone, a group never ended, ids 0 and 5, nested groups, stored twice, drawn before
        # stored, a KEEP_RELEASE after a draw; then a store over the 200 bytes and one within them
        bytes([2, 16, 12, 0, 7]) + batch(END) + batch(group(1, (0, 0, 8, 8)))
        + batch(group(0, (0, 0, 2, 2)), END) + batch(group(5, (0, 0, 2, 2)), END) + batch(kdraw(5, (0, 0, 1, 1)))
        + batch(group(1, (0, 0, 2, 2)), group(2, (0, 0, 2, 2)), END, END)
        + batch(group(1, (0, 0, 2, 2)), END, group(1, (0, 0, 2, 2)), END)
        + batch(group(3, (0, 0, 2, 2)), END, kdraw(4, (0, 0, 1, 1)), group(4, (0, 0, 2, 2)), END)
        + batch(fill((0, 0, 1, 1)), krelease(1))
        + batch(group(1, (0, 0, 8, 8)), END) + batch(group(1, (0, 0, 4, 4)), END, kdraw(1, (0, 0, 4, 4))),
        # BOLD, ITALIC and both (with DIM) over their whole footprints, from rects inside larger A8 and A4 buffers
        bytes([0, 23, 19, 0, 8]) + batch(register(1, 1, 9, 6, 9, 0, 54), register(2, 0, 11, 8, 6, 64, 48),
                                         fill((0, 0, 24, 20), (0, 0, 0)),
                                         region(2, (0, 0, 6, 4), 1, (2, 1, 7, 5), flags=2),
                                         region(2, (1, 5, 8, 9), 1, (2, 1, 7, 5), (-1, 0), flags=4, axis=4),
                                         region(2, (2, 10, 12, 16), 2, (2, 1, 9, 7), (-4, 0), flags=7, axis=-20)),
        # kept across batches: UPDATEs inside the glyphs' rect (its pixels change) and outside it
        bytes([0, 23, 19, 0, 10])
        + b"".join(batch(*pre, fill((0, 0, 24, 20), (0, 0, 0)), region(2, (0, 0, 6, 4), 1, (2, 1, 7, 5), flags=2),
                         region(2, (1, 5, 8, 9), 1, (2, 1, 7, 5), (-1, 0), flags=4, axis=4))
                   for pre in ((register(1, 1, 9, 6, 9, 0, 54),), (update(1, (3, 2, 5, 3)),), (update(1, (8, 0, 9, 6)),))),
        # the same in a keep drawn in part, twice; then outside the footprint, and a bad axis
        bytes([1 | 6, 15, 11, 1, 9]) + batch(register(2, 0, 11, 8, 6, 64, 48))
        + batch(group(1, (0, 0, 16, 8)), region(2, (1, 1, 11, 7), 2, (2, 1, 9, 7), (-4, 0), flags=6, axis=-20), END,
                kdraw(1, (2, 1, 10, 6), (2, 1))) * 2
        + batch(region(2, (0, 0, 10, 6), 2, (2, 1, 9, 7), (-5, 0), flags=6, axis=-20))
        + batch(register(1, 1, 9, 6, 9, 0, 54), region(2, (0, 0, 2, 2), 1, (2, 1, 7, 5), flags=4, axis=110)),
        # registrations: replace, update, release (also of ids naming nothing), commands after a draw, an odd A4
        # column, DMA and DEVICE memory, ids 0 and 7
        bytes([2, 12, 8, 0, 10]) + batch(register(1, 2, 2, 2, 8, 0, 16), region(3, (0, 0, 2, 2), 1, (0, 0, 2, 2)))
        + batch(register(1, 2 | 0x40, 3, 1, 12, 32, 12), update(1, (0, 0, 3, 1)), region(3, (0, 0, 3, 1), 1, (0, 0, 3, 1)))
        + batch(update(1, (0, 0, 4, 1)), fill((0, 0, 1, 1)))
        + batch(release(1), release(5), release(0), release(7), region(3, (0, 0, 1, 1), 1, (0, 0, 1, 1)))
        + batch(fill((0, 0, 1, 1)), register(2, 1, 4, 4, 4, 0, 16))
        + batch(register(2, 0, 6, 2, 3, 0, 6), region(2, (0, 0, 2, 2), 2, (1, 0, 3, 2)), region(2, (2, 0, 4, 2), 2, (2, 0, 4, 2)))
        + batch(register(3, 1 | 0x80, 4, 4, 4, 0, 16)) + batch(register(0, 1, 1, 1, 1, 0, 1)) + batch(register(7, 1, 1, 1, 1, 0, 1)),
    ],
    "fuzz_package": [
        b"\x00\x00",
        b"\x00\x00SHRFPKG1" + bytes(120),
        b"\x08\x01" + header(6, 200) + bytes(48),
        b"\x00\x00" + header(5, 200) + bytes(48),
    ],
    "fuzz_rows": [
        bytes([1 | 64]) + cached_row(0, 1, (1, 0)) + cached_row(1, 2, (2, 0)) + cached_row(2, 3, (3, 0)) + R_RENDER
        + cached_row(1, 3, (2, 1)) + R_RENDER + R_RENDER + bytes([2, 0, 3, 1, 16, 0]) + R_RENDER
        + bytes([5]) + i8(0, 0, 8, 8) + R_RENDER + bytes([6, 20]) + R_RENDER,
        bytes([2 | 4 | 16]) + cached_row(0, 1, (1, 1), 0x80 | 1) + rrow(1, [rcmd(0, 0, 4, 16, 16), rcmd(2, 3, 1, 16, 16, 6, 2)])
        + R_RENDER + bytes([4, 20]) + R_RENDER + bytes([4, 20]) + R_RENDER
        + rrow(2, [rcmd(0, 0, 1, 32, 16), rcmd(5, 0, 4, 32, 16), rcmd(6, 0, 0, 0, 0)])
        + rrow(2, [rcmd(0, 1, 2, 32, 8, 0x41)]) + rrow(2, [rcmd(0, 3, 0, 32, 8, invalid=True)])
        + rrow(2, [rcmd(7, 0, 1, 32, 8)]) + bytes([2, 0, 2, 0xFF, 0xF8, 8]) + R_RENDER
        + bytes([7, 4, 0xFC]) + R_RENDER + bytes([7 | 8 | 16, 0, 0]) + R_RENDER + rrow(1, []) + R_RENDER,
    ],
    "fuzz_compositor": [
        bytes([1]) + layer(0, 0, (0, 0, 64, 48)) + lfill(0, (4, 4, 40, 30)) + SUBMIT + PUMP + check(),
        bytes([0 | 8 | 32]) + layer(0, 1, (-8, -8, 40, 40)) + layer(1, 0, (10, 10, 60, 44)) + lfill(0, (0, 0, 30, 30))
        + lfill(1, (0, 0, 64, 64), 90) + image(0, 8, 8) + limage(1, 0, (0, 0, 8, 8), (2, 2)) + SUBMIT + PUMP
        + bytes([21, 1, 21, 0]) + bytes([3, 1, 3]) + SUBMIT + PUMP + bytes([4, 0]) + check() + bytes([4, 0x80])
        + bytes([10, 0]) + i8(0, 0, 4, 4) + check(),
        bytes([1 | 32]) + layer(0, 0, (0, 0, 64, 48)) + lfill(0, (4, 4, 40, 30)) + bytes([25, 1]) + check()
        + bytes([25, 2]) + check() + bytes([25, 3]) + check() + bytes([25, 8]) + check() + bytes([25, 4]) + check(),
        bytes([2 | 4 | 32]) + layer(0, 0, (0, 0, 64, 48)) + lfill(0, (0, 0, 8, 8)) + bytes([23, 2]) + SUBMIT + PUMP
        + bytes([19, 200]) + bytes([22, 0]) + PUMP + bytes([23, 1]) + PUMP + check(),
        bytes([1]) + bytes([17, 1]) + SUBMIT + PUMP + bytes([17, 4]) + SUBMIT + PUMP + PUMP + bytes([15, 3]) + SUBMIT
        + PUMP + bytes([15, 0, 16, 1]) + SUBMIT + PUMP + bytes([16, 3, 24, 1]) + SUBMIT + PUMP + bytes([24, 0]) + check(),
        bytes([1 | 8]) + image(0, 4, 4) + image(1, 8, 8) + image(2, 8, 8) + layer(0, 0, (0, 0, 64, 48))
        + limage(0, 1, (0, 0, 8, 8), (0, 0)) + bytes([11, 1]) + SUBMIT + PUMP + bytes([10, 0]) + i8(0, 0, 2, 2)
        + bytes([1, 0]) + PUMP + bytes([9, 1, 3, 3]),
        # three ids and a driver keeping copies in 3 images' bytes: an update while a frame reads the image makes a
        # second buffer, whose registration evicts the first; then updates in place
        bytes([1 | 32 | 64 | 128]) + image(0, 8, 8) + image(1, 8, 8) + image(2, 4, 4) + layer(0, 0, (0, 0, 64, 48))
        + limages(0, (0, (0, 0, 8, 8), (0, 0)), (1, (0, 0, 8, 8), (10, 0)), (2, (0, 0, 4, 4), (20, 0))) + SUBMIT + PUMP
        + ASYNC + SUBMIT + PUMP + update_image(0, (0, 0, 4, 4)) + update_image(0, (2, 2, 8, 8)) + COMPLETE + PUMP
        + SYNC + SUBMIT + PUMP + check() + update_image(1, (1, 1, 3, 7)) + SUBMIT + PUMP + check()
        + ASYNC + SUBMIT + PUMP + update_image(0, (0, 0, 1, 1)) + COMPLETE + PUMP + SYNC + check(),
        # the same with ids only, drawn in place
        bytes([1 | 32 | 64]) + image(0, 8, 8) + image(1, 8, 8) + image(2, 4, 4) + layer(0, 0, (0, 0, 64, 48))
        + limages(0, (0, (0, 0, 8, 8), (0, 0)), (1, (0, 0, 8, 8), (10, 0)), (2, (0, 0, 4, 4), (20, 0))) + ASYNC
        + SUBMIT + PUMP + update_image(2, (0, 0, 4, 4)) + COMPLETE + PUMP + SUBMIT + PUMP + update_image(2, (0, 0, 2, 2))
        + COMPLETE + PUMP + SYNC + check(),
        # bands: one 16 rows high (align 8), two 13 rows high, two 16 rows high (align 16), one 7 rows high converting the
        # format; a sprite and images across band edges, async batches and a failing one
        bytes([1 | 8 | 32]) + layer(0, 0, (0, 0, 64, 48)) + lfill(0, (4, 4, 40, 30)) + image(0, 8, 8)
        + layer(1, 1, (13, 9, 60, 47)) + limage(1, 0, (0, 0, 8, 8), (3, 3)) + bytes([25, 0x21, 1, 4]) + check()
        + bytes([25, 0x62, 12, 0]) + check() + bytes([2, 1]) + i8(20, 11, 50, 40) + SUBMIT + PUMP + check()
        + bytes([25, 0x63, 0, 5]) + ASYNC + SUBMIT + PUMP + COMPLETE + PUMP + COMPLETE + PUMP + SYNC + check()
        + bytes([25, 0x28, 6, 1]) + bytes([17, 1]) + SUBMIT + PUMP + check() + bytes([25, 0]) + check(),
        # a layer moved down and up as pixels between frames, with a sprite above it; then with bands and async batches
        bytes([1 | 8 | 32]) + layer(0, 0, (0, 0, 64, 48)) + lfill(0, (0, 0, 64, 48)) + image(0, 8, 8)
        + layer(1, 1, (20, 20, 40, 40)) + limage(1, 0, (0, 0, 8, 8), (2, 2)) + SUBMIT + PUMP + shift(0, -8) + SUBMIT
        + PUMP + check() + shift(0, 12) + shift(0, 4) + SUBMIT + PUMP + check() + bytes([25, 0x21, 1, 4]) + check()
        + shift(0, -16) + ASYNC + SUBMIT + PUMP + shift(0, 8) + SUBMIT + COMPLETE + PUMP + COMPLETE + PUMP + SYNC + check()
        + shift(0, 8, True) + SUBMIT + PUMP + check(),
        # a failed batch and a reset after a timeout make the driver forget its ids
        bytes([1 | 2 | 4 | 32 | 128]) + image(0, 8, 8) + layer(0, 0, (0, 0, 64, 48)) + limage(0, 0, (0, 0, 8, 8), (4, 4))
        + SUBMIT + PUMP + bytes([17, 1]) + bytes([20, 0]) + PUMP + check() + ASYNC + bytes([20, 0]) + PUMP
        + bytes([19, 200]) + PUMP + SYNC + check(),
        # the frame registering a second buffer (evicting the first) fails after running its buffer commands; the
        # next one releases the ids it named before registering again
        bytes([1 | 32 | 64 | 128]) + image(0, 8, 8) + image(1, 8, 8) + image(2, 8, 8) + layer(0, 0, (0, 0, 64, 48))
        + limages(0, (0, (0, 0, 8, 8), (0, 0)), (1, (0, 0, 8, 8), (10, 0)), (2, (0, 0, 8, 8), (20, 0))) + SUBMIT + PUMP
        + ASYNC + SUBMIT + PUMP + update_image(0, (0, 0, 4, 4)) + COMPLETE + PUMP + SYNC + bytes([17, 0x41])
        + SUBMIT + PUMP + SUBMIT + PUMP + check(),
    ],
}

for name, seeds in SEEDS.items():
    d = ROOT / name
    d.mkdir(parents=True, exist_ok=True)
    for old in d.glob("seed-*"):
        old.unlink()
    for i, data in enumerate(seeds):
        (d / f"seed-{i:02d}").write_bytes(data)
