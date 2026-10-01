# Shiroko font package format v5

All integers are little-endian with the widths given below; every checksum is XXH3-64 (xxHash, seed 0); no native struct is serialized. Python `struct` formats are given for every record. One package holds one provider (role) with one instance, from its regular face, for the single cell size of the build (`fontpack.py build --cell WxH`, at most 64x127 so that a two-cell glyph fits the glyph record's u8 width and i8 bearing/top); the runtime supports no other size. Packages hold no bold or italic faces: `SHR_STYLE_BOLD` and `SHR_STYLE_ITALIC` text is drawn from the regular coverage, which drivers embolden and slant (`SHR_GLYPH_BOLD`, `SHR_GLYPH_ITALIC` in `shiroko_driver.h`), except for glyphs the emoji or Nerd package serves and for box drawing and block characters (U+2500–259F, U+1FB00–1FBFF).

## Boxes

A package is a chain of boxes that tiles the file exactly:

```
[shrf 128 B][sidx][metadata boxes ...][free and page boxes ...]
```

Every box starts with a 16-byte header, `"<I4sHHI"`:

| Offset | Type | Field |
|---:|---|---|
| 0 | u32 | size: the whole box, header included (at least 16) |
| 4 | u8[4] | type (FourCC) |
| 8 | u16 | version = 0 |
| 10 | u16 | flags: bit0 REQUIRED; bits 4–7 METHOD (0 stored, 1 zstd, 2–15 reserved); other bits 0 |
| 12 | u32 | raw_size: payload bytes after decoding; size − 16 when stored |

A checksum "of a box" always covers the whole box, `[offset, offset + size)`, header included.

A stored box's payload is its raw bytes. A zstd box's payload is one zstd frame, except a page box (below). Compression is a choice of the build; the runtime decodes whatever method a box declares. Only `cmap`, `seqs`, `pool` and `page` may be zstd.

### `shrf`: header (128 B, offset 0)

`"<I4sHHIHHIIIQQ32sQ32xQ"`:

| Offset | Type | Field |
|---:|---|---|
| 0 | box | size 128, type `shrf`, version 0, flags 1, raw_size 112: the bytes `80 00 00 00 73 68 72 66` are the magic |
| 16 | u16 | format version = 5 |
| 18 | u16 | 0 |
| 20 | u32 | required features: bit0 A4, bit1 A8, bit2 sequences, bit3 zstd (some box is zstd); other bits are rejected |
| 24 | u32 | optional features: ignored (written as 0) |
| 28 | u32 | sidx_size: size of the `sidx` box |
| 32 | u64 | file size |
| 40 | u64 | sidx_offset = 128 |
| 48 | u8[32] | text profile SHA-256 |
| 80 | u64 | XXH3-64 of the `sidx` box |
| 88 | u8[32] | 0 |
| 120 | u64 | XXH3-64 of bytes [0, 120) |

`package_id` is the SHA-256 of the whole file (computed by the tools; the runtime only carries it).

### `sidx`: box index (offset 128)

Header: size sidx_size, type `sidx`, version 0, flags 1 (stored), raw_size = size − 16. Payload: `"<II"` count (at most 32), 0; then count entries of 32 B, `"<4sHHQIIQ"`:

| Offset | Type | Field |
|---:|---|---|
| 0 | u8[4] | type |
| 4 | u16 | flags (note: flags before version, unlike the box header) |
| 6 | u16 | version |
| 8 | u64 | offset of the box |
| 16 | u32 | size |
| 20 | u32 | raw_size |
| 24 | u64 | XXH3-64 of the box |

so sidx_size = 24 + 32 × count. Entries are listed in offset order. `sidx` lists the metadata boxes and nothing else: never `shrf`, `sidx`, `page` or `free`. Entries of types this reader does not know are skipped unless REQUIRED (then the package is unsupported); the runtime does not read, hash or decode them.

### Metadata boxes

Each known type appears at most once and carries REQUIRED, except `covr`, which may leave it out (fontpack writes it without). Version 0, other flag bits 0.

| Type | Method | Raw payload |
|---|---|---|
| `mani` | stored | 36 B, `"<B3x8sIIIIII"`: `u8 role, u8[3] 0, u8[8] locale, u32 name_off, u32 name_len, u32 build_off, u32 build_len, u32 glyph_count, u32 page_count` |
| `strs` | stored | UTF-8 bytes |
| `srcs` | stored | n × 48 B, `"<II32sII"`: `u32 name_off, u32 name_len, u8[32] sha256, u32 license_off, u32 license_len`; strings inside `strs`; the licence is the SPDX conjunction of the font's licence and every component licence |
| `inst` | stored | 48 B, `"<HBBHHIhhhH16sHH8x"`: `u16 source, u8 0, u8 format, u16 line_height, u16 cell_width, u32 ppem_26_6, i16 baseline, i16 underline_y, i16 strike_y, u16 raster_flags, u8[16] font_instance_id, u16 page_width, u16 page_height, u8[8] 0`; page_width and page_height (the page shape W × H) are each 64, 128, 256 or 512; raster_flags bit0 = fontpack's pinned FreeType raster mode, other bits 0; ppem_26_6 is nominal: symbols and CJK glyphs that would not fit their cells are sized per glyph |
| `cmap` | stored or zstd | n × 8 B, `"<II"`: `u32 scalar, u32 glyph_index`, sorted by scalar, unique |
| `seqs` | stored or zstd | n × 12 B, `"<BBHII"`: `u8 length, u8 kind, u16 0, u32 pool_index, u32 glyph_index`, sorted by scalar array, unique |
| `pool` | stored or zstd | n × 4 B: u32 scalars |
| `ptab` | stored | page_count × 32 B, `"<QQIIIHH"`: `u64 offset, u64 xxh3 (of the page box), u32 size (of the page box), u32 first_glyph, u32 glyph_count, u16 height, u16 0` |
| `covr` | stored | 36 B, `"<9I"`: counters scalars, sequences, aliases, uvs, ivs, missing, reserved (written as 0), glyphs, bitmap_bytes |

Roles: 1 latin, 2 cjk, 3 symbols, 4 emoji, 5 nerd. Formats: 1 A4, 2 A8. Sequence kinds: 1 glyph, 2 alias, 3 default UVS, 4 non-default UVS. Every key maps to a glyph index, so aliases share bitmaps and cannot form cycles. Metrics are in pixels from the top of the line cell: `baseline`, `underline_y`, `strike_y`.

### `page` boxes

Pages are ordered, contiguous and cover every glyph index once. Page i holds glyphs [first_glyph, first_glyph + glyph_count) (1 to 4096 of them) in the top `height` rows (1 ≤ height ≤ H) of a W × H atlas of stride = W × bytes per pixel (A4 ½, A8 1). Its raw content is

```
atlas:   height rows of stride bytes, no padding                       (height × stride B)
records: glyph_count × 16 B, "<HHBBbbBBHI":
         u16 x, u16 y, u8 width, u8 height, i8 bearing_x, i8 top, u8 flags, u8 0, u16 source_glyph, u32 0
         flags: bits0-1 format (0 none, 1 A4, 2 A8), bits2-3 cells, bits4-7 0
```

The page box: header (type `page`, version 0, flags REQUIRED | METHOD << 4, raw_size = 8 + height × stride + 16 × glyph_count), then `"<II"` page_index (= i), atlas_stream_bytes, then the atlas stream (atlas_stream_bytes B), then the records stream (the rest of the box). The 8 bytes `page_index, atlas_stream_bytes` are never compressed. Stored: the streams are the atlas and the records as they are. zstd: each stream is one zstd frame, of the atlas and of the records; both streams are frames.

A stored page box starts where (offset + 24) % 256 == 0, so its atlas starts at a multiple of 256 B in the file and can be drawn from in place in a package mapped at a 256-aligned address (as the built-in one is). zstd pages are packed tightly.

A glyph's bitmap is the rect `[x, x + width) × [y, y + height)` of the atlas, in the instance's format (A4 packs the left pixel in the high nibble), with `x + width ≤ W` and `y + height ≤` the page's height; `bearing_x` is from the left edge of the first cell, `top` is the number of rows above the baseline. An A4 glyph starts at an even `x` and, for an odd width, the column right of its rect (inside the atlas, whose width is even) is a zero nibble in every row of the rect. Glyphs without a bitmap (spaces) have format 0 and `x`, `y`, `width` and `height` 0. Rects may touch (readers take nothing outside a glyph's rect).

### `free` boxes

Padding: type `free`, version 0, flags 0, raw_size = size − 16, payload all zero. Never indexed; readers skip them.

### zstd frames

A zstd stream of n bytes that decodes to `expected` bytes (known from `inst`, `ptab` and the box header, never from the frame) is accepted only if, in this order:

1. n ≥ 4 and it starts with the zstd frame magic `28 B5 2F FD` (no legacy or skippable frames);
2. it is exactly one frame, with no bytes after it (`ZSTD_findFrameCompressedSize(src, n) == n`);
3. its frame header has a content size equal to `expected`, a window size (the header's `windowSize`, which is the content size for a single-segment frame) of at most 2^18, dictionary id 0 and no checksum;
4. n < `expected` (otherwise the box must be stored);
5. it decodes to exactly `expected` bytes.

## Loader rules, in order

The runtime and `fontpack.py` (`Package`) check these rules in this order; the first that fails rejects the package (or the page) with the status given (SHR_E_FORMAT where none is). "Known" types are `mani strs srcs inst cmap seqs pool ptab covr`.

**Header** (read when the package opens):

1. The source holds at least 128 bytes.
2. Bytes [0, 8) are `80 00 00 00 73 68 72 66` (a v4 file starts with `SHRFPKG1` and fails here).
3. Version 0, flags 1, raw_size 112, format version 5, bytes [18, 20) zero.
4. XXH3 of [0, 120) equals the header checksum — SHR_E_CHECKSUM.
5. Required features: no bit beyond bit3 — SHR_E_UNSUPPORTED; bit3 (zstd) in a runtime built without zstd — SHR_E_UNSUPPORTED (nothing else is read).
6. Bytes [88, 120) zero.
7. File size equals the source's size; sidx_offset = 128; sidx_size = 24 + 32 × k for some k ≤ 32; 128 + sidx_size ≤ file size.
8. The text profile equals the runtime's — SHR_E_PROFILE_MISMATCH.

**Index** (sidx_size bytes at 128):

9. XXH3 of the box equals the header's — SHR_E_CHECKSUM.
10. Box header: size sidx_size, type `sidx`, version 0, flags 1, raw_size sidx_size − 16; count = (sidx_size − 24) / 32; the u32 after count 0.
11. For each entry, in listed order:
    1. size ≥ 16, offset ≥ the end of the previous entry (the first: ≥ 128 + sidx_size), offset + size ≤ file size;
    2. type not `shrf`, `sidx`, `page` or `free`;
    3. an unknown type: REQUIRED set — SHR_E_UNSUPPORTED; else the entry is skipped (no further rule applies to it);
    4. a known type: not seen before;
    5. METHOD ≥ 2 — SHR_E_UNSUPPORTED;
    6. flags: no bit set but REQUIRED and METHOD; REQUIRED set unless `covr`; METHOD 1 only for `cmap`, `seqs`, `pool`, and only with required feature bit3; version 0;
    7. stored: raw_size = size − 16; zstd: raw_size > size − 16.
12. `mani`, `inst`, `cmap` and `ptab` are present.
13. The metadata region, from the first known box to the end of the last known box, is at most 128 MiB, and so is the sum of the known boxes' raw_size — SHR_E_LIMIT.

**Metadata** (the region, read at once). For each known box, in offset order:

14. XXH3 of the box equals its entry's — SHR_E_CHECKSUM.
15. Its header equals its entry: size, type, version, flags, raw_size.
16. It decodes to raw_size bytes (stored: the payload; zstd: the frame rules above).

Then the contents:

17. `mani`: 36 B, padding 0; role 1–5; locale bytes 0 or 0x20–0x7E; (runtime) the role and, for CJK, the locale those of the file name (`shiroko-<role>.shrf`, `shiroko-cjk-<locale>.shrf`); name and build strings inside `strs` (absent: 0 bytes); 1 ≤ glyph_count ≤ 2^22.
18. `inst`: 48 B. `srcs`: a multiple of 48 B. `covr`: 36 B. Each source's name and licence strings inside `strs`.
19. `inst`: source < the number of sources when `srcs` is present; style byte 0; format 1 or 2 with its required feature bit; 1 ≤ line_height, cell_width ≤ 1024; 0 ≤ baseline ≤ line_height, 0 ≤ underline_y, strike_y < line_height; raster_flags & ~1 = 0; reserved bytes 0; page_width and page_height each 64, 128, 256 or 512.
20. (runtime) The instance's cell size is the build's — SHR_E_UNSUPPORTED.
21. `cmap`: a multiple of 8 B, at most 2^22 records; scalars ≤ 0x10FFFF, not surrogates, strictly increasing; glyph indices < glyph_count.
22. `pool`: a multiple of 4 B, at most 2^22 scalars.
23. `seqs` (when present): required feature bit2, `pool` present, a multiple of 12 B, at most 2^22 records; every pool scalar ≤ 0x10FFFF and not a surrogate; each record: 2 ≤ length ≤ the text profile's cluster limit, kind 1–4, zero 0, pool_index + length ≤ pool size, glyph index < glyph_count; keys strictly increasing.
24. `ptab`: raw_size = 32 × page_count (mani); 1 ≤ page_count ≤ 2^16. Each entry, in order: first_glyph = the glyphs of the pages before; 1 ≤ glyph_count ≤ 4096; 1 ≤ height ≤ page_height; the u16 after height 0; 24 ≤ size ≤ 1 MiB; offset ≥ the end of the previous page (the first: ≥ the end of the last box `sidx` lists, known or not); offset + size ≤ file size.
25. The pages cover glyph_count glyphs.

**Page** i (each time it is read, or decoded from a mapping):

26. XXH3 of the page box equals the ptab entry's — SHR_E_CHECKSUM (after a read, the page is first re-read like an I/O error).
27. Header: type `page`, version 0, size the entry's; METHOD ≥ 2 — SHR_E_UNSUPPORTED; flags: REQUIRED set, no bit but REQUIRED and METHOD, METHOD 1 only with required feature bit3; raw_size = 8 + height × stride + 16 × glyph_count.
28. Stored: size = raw_size + 16 and (offset + 24) % 256 = 0.
29. page_index = i; stored: atlas_stream_bytes = height × stride; zstd: atlas_stream_bytes ≤ size − 24.
30. The atlas stream decodes to height × stride bytes, then the records stream to 16 × glyph_count bytes.
31. Each record: flags & 0xF0 = 0, cells 1 or 2, the zero byte and the u32 0; format 0: x, y, width and height 0; else format = the instance's, width and height ≥ 1, A4 x even, x + width ≤ page_width, y + height ≤ the page's height, and for an odd A4 width the padding nibble (in the decoded atlas) zero in every row.

Overlapping rects are not rejected: they draw wrong pixels but never read outside the page. Nothing else is hashed; there is no whole-package hash and no hash of decoded bytes (decoding is deterministic and its input is hashed). COVERAGE is informational: `missing` counts profile sequences the source fonts cannot draw.

`fontpack.py verify`, `install` and `build` apply rules 1–31 (pages: all of them; the cell size and file name only in `install`) and then walk the box chain: from offset 0 the boxes tile the file; each is `shrf`, `sidx`, a box `sidx` lists (its header's type and size those of the entry), a page `ptab` lists, or a `free` box; every listed box and page is reached. Errors are explicit, also under `python -O`. A v4 file fails with "format v4 package: rebuild with fontpack 5".

## Runtime

The runtime opens a package in three asynchronous steps, all in `shr_pump()`: the header (128 B, rules 1–8), the index (rules 9–13), then one read of the metadata region (rules 14–25), whose boxes are decoded into one resident allocation. A mapped package's index is decoded the same way (stored boxes could stay in place). The built-in package and stored indexes need no zstd decoder; the decoder context (about 96 KB) is allocated at a font's first compressed box and shared by its packages. A runtime built without zstd (CMake `SHIROKO_ZSTD=OFF`) rejects any package whose header declares bit3 at open, before reading further.

Resident pages live in slots of the page cache (`page_cache_bytes`), driver buffers of the page shape (format, W, H); an evicted slot takes a page of any package with the same shape. A page is decoded into the top `height` rows of its slot; the rows below are zeroed after every decode. A page charges H × the slot's stride, its record block (16 × glyph_count) and, while its read is in flight, a staging buffer of the page box's size. A stored page of a mapped package (and of the built-in one) is drawn from in place when the driver can wrap it, else decoded into a slot. To make room, a page evicts only the oldest unpinned pages that the latest frame neither drew from nor wanted; when that is not enough, the page waits for memory (read and mapped packages alike) and the frame settles with fallback glyphs, while a page larger than the whole cache fails with SHR_E_LIMIT.

Failures: a format, checksum or profile error, or a page whose slot is larger than the page cache (SHR_E_LIMIT) or than the driver's buffers (SHR_E_UNSUPPORTED), disables the package or page for the font's lifetime (a failed package frees its buffers and closes its source at once). A page whose checksum does not match after a read is re-read like an I/O error, then fails with SHR_E_CHECKSUM; a page that fails rules 27–31 fails with its status at once, without a re-read. A mapped package or page that runs out of memory waits and is retried, as a read package does. An I/O error or timeout of open() or a read is retried after `io_retry_ns`; after `io_retry_limit` retries (counted per page while frames want it, and per package until it is ready) the package (closed) or page cools down: SHR_EVENT_RESOURCE_FAILED is queued and its glyphs draw the provisional fallback. A cool-down ends after `io_retry_ns` or at `shr_asset_ready()`, whichever comes first (with `io_retry_ns` 0 only at the signal); the pump then redraws, and the package or page is tried again by the next frame that needs it. After `io_retry_limit` cool-downs in a row it is given up: until it loads, only `shr_asset_ready()` ends its cool-downs, and no deadline is reported for them. A refused read (`SHR_E_WOULD_BLOCK`) is retried the same way without counting. `SHR_E_NOT_FOUND` from open() means not installed: the package stays absent for the font's lifetime; to use a package installed later, create a new font and pass it to `shr_pl_lyr_tilemap_resize()`, which redraws the tilemap with it. Emoji-presentation scalars the emoji package lacks (or all of them, when it is not installed) use the text fallback chain, then U+FFFD.

## Build and install

Providers are fixed at build time. The Nerd package serves every Private Use scalar of the pinned Nerd font's cmap (the text profile's Nerd class is generated from the same font), the emoji package the emoji-presentation scalars, and latin, CJK and symbols share the text fallback chain. `build` fails when a scalar of a package's source cmap is routed to it but no package serves it, unless `fontpack.config.json` lists it under `coverage_allowlist.<package>.scalars` with a `reason`; `coverage.json` reports both lists per package. `build` also fails while `fonts.lock.json` has an unverified licence component. `inventory.json` records per licence and component the SPDX id, verification state and the provenance (URL, SHA-256) of every licence file. Outputs are staged and moved into place file by file; `build` never downloads (inputs come from `make fontpack-fetch`).

A CJK package's ppem is the largest, in 1/64 steps up to the Latin em, at which the 99.9th-percentile rows of its kana, Han and Hangul hinted bitmaps above plus below the baseline fit the line height; all its glyphs move by the same rows to sit inside the line, and the few that still do not fit are moved or shrunk into their cells one by one. The build string records the tool versions (zstd included), raster mode and shaping properties (script, language, direction, features).

**Page shape.** `fontpack.config.json` gives per format the default shape (`page_atlas`), the candidate shapes (`page_shapes`, each side 64–512, a power of two) and the most glyphs per page (`max_page_records`, at most 4096). Candidates are the shapes in which every glyph fits (A4 span width + (width & 1) ≤ W, height ≤ H). If all glyphs fit on one page of some candidate whose trimmed atlas (used rows × stride) is smaller than one default page, the package takes the candidate with the fewest trimmed bytes (ties: the smaller W, then the smaller H); otherwise the default. Glyphs are shelf-packed in index order; a page ends when the next glyph does not fit below the last shelf or the page holds `max_page_records`. Every page's height is H except the last one's: its used rows (at least 1). Atlas bytes no glyph covers are 0. `--page-atlas WxH` forces that one shape for both formats (the fuzz seeds use the smallest shape of at least 2 cells by 1 line).

**Compression.** By default `cmap`, `seqs`, `pool` and pages are zstd: level 19, window log at most 18, content size written, no checksum or dictionary id, one frame per stream. A box (a page: either stream) whose frame is not smaller than its raw bytes is stored. `--store-index` keeps `cmap`, `seqs` and `pool` stored (for flash-mapped packages, whose stored boxes need no decoding); `--method stored` stores everything. Required feature bit3 is set when some box ended up zstd. Metadata boxes follow `sidx` in the order `mani strs srcs inst cmap seqs pool ptab covr`, then the pages; a stored page is preceded by a `free` box of g = (−(offset + 24)) mod 256 bytes, or g + 256 when 0 < g < 16 (none when g = 0). `package_id` and `install`'s byte-identical check therefore depend on the libzstd that the pinned `zstandard` bundles.

`install` verifies each package, copies NOTICE, LICENSES/ and inventory.json from the packages' build directory (whose inventory must describe exactly those package ids), rewrites an existing payload unless it is byte-identical to the verified package (temporary file, fsync, rename, directory fsync), and only then writes the next activation record. Only the runtime checks that the instance matches its cell size; a build that would produce no glyphs fails without writing a package.

The CMake build runs `fontpack.py build --cell WxH` into `<build dir>/fonts` (target `shiroko_fonts`) and `fontpack.py builtin --cell WxH --out FILE.c`, which writes the built-in package (ASCII U+0020–007E and U+FFFD from the Latin regular face, same raster settings; always stored, its shape chosen by the rule above) as `shr__builtin_package` (aligned to 256 B) / `shr__builtin_package_size`. The library opens it like any memory-mapped package and takes the line metrics from its `inst` box. `build` reports each package's size, shape, pages and `atlas_fill`: the share of atlas pixels glyphs cover, over W × height summed over pages.

## Activation record (64 B)

`u8[8] magic SHRFACT2, u64 generation, u8[32] package_id, u64 package_size, u64 XXH3-64 of bytes [0, 56)`. Two slots (A/B) are kept; the valid record with the higher generation wins, so a torn write falls back to the previous package.
