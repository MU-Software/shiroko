# Shiroko font package format v4

All integers are little-endian with the widths given below; every checksum is XXH3-64 (xxHash, seed 0); no native struct is serialized. One package holds one provider (role) with one instance, from its regular face, for the single cell size of the build (`fontpack.py build --cell WxH`, at most 64x127 so that a two-cell glyph fits the glyph record's u8 width and i8 bearing/top); the runtime supports no other size. Packages hold no bold or italic faces: `SHR_STYLE_BOLD` and `SHR_STYLE_ITALIC` text is drawn from the regular coverage, which drivers embolden and slant (`SHR_GLYPH_BOLD`, `SHR_GLYPH_ITALIC` in `shiroko_driver.h`), except for glyphs the emoji or Nerd package serves and for box drawing and block characters (U+2500–259F, U+1FB00–1FBFF).

## Layout

```
[header 128 B][section table][sections ...][pages ...]
```

### Header

| Offset | Type | Field |
|---:|---|---|
| 0 | u8[8] | magic `SHRFPKG1` |
| 8 | u16 | format version = 4 |
| 10 | u16 | header size = 128 |
| 12 | u32 | required features: bit0 A4, bit1 A8, bit2 sequences; unknown bits are rejected |
| 16 | u32 | section count |
| 20 | u32 | reserved = 0 |
| 24 | u64 | section table offset |
| 32 | u64 | file size |
| 40 | u8[32] | text profile SHA-256 |
| 72 | u64 | XXH3-64 of bytes [0, 72) |
| 80 | u8[48] | reserved = 0 |

`package_id` is the SHA-256 of the whole file (computed by the tools; the runtime only carries it).

### Section table entry (32 B)

`u32 type, u32 count, u64 offset, u32 length, u32 0, u64 xxh3` (of the section bytes). Each type appears at most once; sections must lie inside the file and not overlap the header, the table or each other.

| Type | Name | Record |
|---:|---|---|
| 1 | MANIFEST | 1 × 36 B: `u8 role, u8[3] 0, u8[8] locale, u32 name_off, u32 name_len, u32 build_off, u32 build_len, u32 glyph_count, u32 instance_count = 1` |
| 2 | STRINGS | UTF-8 bytes |
| 3 | SOURCES | 48 B: `u32 name_off, u32 name_len, u8[32] sha256, u32 license_off, u32 license_len`; strings inside STRINGS; the licence is the SPDX conjunction of the font's licence and every component licence |
| 4 | INSTANCES | exactly 1 × 48 B: `u16 source, u8 0, u8 format, u16 line_height, u16 cell_width, u32 ppem_26_6, i16 baseline, i16 underline_y, i16 strike_y, u16 raster_flags, u8[16] font_instance_id, u16 page_width, u16 page_height, u8[8] 0`; page_width × page_height is the atlas of every page (at least 1 × 1, an even width for A4); raster_flags bit0 = fontpack's pinned FreeType raster mode, other bits 0; ppem_26_6 is nominal: symbols and CJK glyphs that would not fit their cells are sized per glyph |
| 6 | CMAP | 8 B: `u32 scalar, u32 glyph_index`, sorted by scalar, unique |
| 7 | SEQS | 12 B: `u8 length, u8 kind, u16 0, u32 pool_index, u32 glyph_index`, sorted by scalar array, unique |
| 8 | SEQPOOL | u32 scalars |
| 9 | PAGES | 32 B: `u64 offset, u64 xxh3, u32 length, u32 first_glyph, u32 glyph_count, u32 0`; offsets are multiples of 256 |
| 10 | COVERAGE | count 9, length 36: u32 counters scalars, sequences, aliases, uvs, ivs, missing, reserved (written as 0), glyphs, bitmap_bytes |

Type 5 is reserved and rejected. Roles: 1 latin, 2 cjk, 3 symbols, 4 emoji, 5 nerd. Formats: 1 A4, 2 A8. Sequence kinds: 1 glyph, 2 alias, 3 default UVS, 4 non-default UVS. Every key maps to a glyph index, so aliases share bitmaps and cannot form cycles.

Metrics are in pixels from the top of the line cell: `baseline`, `underline_y`, `strike_y`.

### Pages

Pages are ordered, contiguous and cover every glyph index once. A page payload is

```
atlas: page_height rows of stride = page_width × bytes per pixel (A4 ½, A8 1), no padding
u32 glyph_count, then glyph_count × 16 B records
record: u16 x, u16 y, u8 width, u8 height, i8 bearing_x, i8 top, u8 flags, u8 0, u16 source_glyph, u32 0
flags:  bits0-1 format (0 none, 1 A4, 2 A8), bits2-3 cells, bits4-7 0
```

so `length = page_height × stride + 4 + 16 × glyph_count`. A glyph's bitmap is the rect `[x, x + width) × [y, y + height)` of the atlas, in the instance's format (A4 packs the left pixel in the high nibble), with `x + width ≤ page_width` and `y + height ≤ page_height`; `bearing_x` is from the left edge of the first cell, `top` is the number of rows above the baseline. An A4 glyph starts at an even `x` and, for an odd width, the column right of its rect (inside the atlas, whose width is even) is a zero nibble in every row of the rect. Glyphs without a bitmap (spaces) have format 0 and `x`, `y`, `width` and `height` 0. Rects may touch (readers take nothing outside a glyph's rect); `fontpack.py` shelf-packs each page's glyphs in index order and writes 0 to every atlas byte no glyph covers. Pages start at multiples of 256 B in the file, so the atlas of a page in a package mapped at a 256-aligned address (as the built-in one is) can be drawn from in place.

## Loader limits

A package is rejected unless every rule above holds and: at most 16 sections and 2^22 records per section; MANIFEST, INSTANCES, CMAP and PAGES present; the manifest's role (and for CJK its locale) that of the file name (`shiroko-<role>.shrf`, `shiroko-cjk-<locale>.shrf`); one instance of exactly the build's cell size (else SHR_E_UNSUPPORTED); at least one glyph; at most 2^16 pages of at most 1 MiB each; the section table plus sections at most 128 MiB; SEQS only with the sequence feature and SEQPOOL, each sequence no longer than the text profile's cluster limit. COVERAGE is informational (but must have its fixed size): `missing` counts profile sequences the source fonts cannot draw.

The runtime checks the header and its checksum when a package opens, the section checksums and every record except page payloads when its index loads (sections stay where they were read: the index buffer, or the mapping), and a page payload (checksum, glyph count, glyph records, formats, rects inside the atlas, even A4 x, A4 padding) each time the page is read, or copied or first drawn from in place for a mapped package. Overlapping rects are not rejected: they draw wrong pixels but never read outside the page. Nothing else is hashed; there is no whole-package hash. All of it runs in `shr_pump()`: a frame that needs an unopened package or an unchecked page draws a provisional fallback.

Resident pages live in slots of the page cache (`page_cache_bytes`): driver buffers of the atlas size with room for the records; an evicted page's slot takes the next page of the same shape (format, atlas and slot size), keeping its driver buffer. A mapped package's pages, and the built-in package's, are drawn from in place when the driver can reach them, else copied into slots (the built-in pages outside the page cache).

Failures: a format, checksum or profile error, or a page whose slot is larger than the page cache (SHR_E_LIMIT) or than the driver's buffers (SHR_E_UNSUPPORTED), disables the package or page for the font's lifetime (a failed package frees its buffers and closes its source at once). A page whose checksum does not match after a read is first re-read like an I/O error. An I/O error or timeout of open() or a read is retried after `io_retry_ns`; after `io_retry_limit` retries (counted per page while frames want it, and per package until it is ready) the package (closed) or page cools down: SHR_EVENT_RESOURCE_FAILED is queued and its glyphs draw the provisional fallback. A cool-down ends after `io_retry_ns` or at `shr_asset_ready()`, whichever comes first (with `io_retry_ns` 0 only at the signal); the pump then redraws, and the package or page is tried again by the next frame that needs it. After `io_retry_limit` cool-downs in a row it is given up: until it loads, only `shr_asset_ready()` ends its cool-downs, and no deadline is reported for them. A refused read (`SHR_E_WOULD_BLOCK`) is retried the same way without counting. `SHR_E_NOT_FOUND` from open() means not installed: the package stays absent for the font's lifetime; to use a package installed later, create a new font and pass it to `shr_pl_lyr_tilemap_resize()`, which redraws the tilemap with it. Emoji-presentation scalars the emoji package lacks (or all of them, when it is not installed) use the text fallback chain, then U+FFFD.

## Build and install

Providers are fixed at build time. The Nerd package serves every Private Use scalar of the pinned Nerd font's cmap (the text profile's Nerd class is generated from the same font), the emoji package the emoji-presentation scalars, and latin, CJK and symbols share the text fallback chain. `build` fails when a scalar of a package's source cmap is routed to it but no package serves it, unless `fontpack.config.json` lists it under `coverage_allowlist.<package>.scalars` with a `reason`; `coverage.json` reports both lists per package. `build` also fails while `fonts.lock.json` has an unverified licence component. `inventory.json` records per licence and component the SPDX id, verification state and the provenance (URL, SHA-256) of every licence file. Outputs are staged and moved into place file by file; `build` never downloads (inputs come from `make fontpack-fetch`).

`install` verifies each package, copies NOTICE, LICENSES/ and inventory.json from the packages' build directory (whose inventory must describe exactly those package ids), rewrites an existing payload unless it is byte-identical to the verified package (temporary file, fsync, rename, directory fsync), and only then writes the next activation record.

`fontpack.py verify`, `install` and `build` apply the loader's rules to every section, page and glyph (with explicit errors, also under `python -O`), and `install` also checks the role and locale against the file name; only the runtime checks that the instance matches its cell size; a build that would produce no glyphs fails without writing a package. A CJK package's ppem is the largest, in 1/64 steps up to the Latin em, at which the 99.9th-percentile rows of its kana, Han and Hangul hinted bitmaps above plus below the baseline fit the line height; all its glyphs move by the same rows to sit inside the line, and the few that still do not fit are moved or shrunk into their cells one by one. The build string records the tool versions, raster mode and shaping properties (script, language, direction, features).

The CMake build runs `fontpack.py build --cell WxH` into `<build dir>/fonts` (target `shiroko_fonts`) and `fontpack.py builtin --cell WxH --out FILE.c`, which writes the built-in package (ASCII U+0020–007E and U+FFFD from the Latin regular face, same raster settings) as `shr__builtin_package` (aligned to 256 B) / `shr__builtin_package_size`. The library opens it like any memory-mapped package and takes the line metrics from it. `fontpack.config.json` sets the atlas per format (`page_atlas`, width × height) and the largest page (`page_bytes`); `build` reports each package's `atlas_fill`, the share of its atlas pixels glyphs cover.

## Activation record (64 B)

`u8[8] magic SHRFACT2, u64 generation, u8[32] package_id, u64 package_size, u64 XXH3-64 of bytes [0, 56)`. Two slots (A/B) are kept; the valid record with the higher generation wins, so a torn write falls back to the previous package.
