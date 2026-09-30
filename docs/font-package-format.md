# Shiroko font package format v2

All integers are little-endian with the widths given below; every checksum is XXH3-64 (xxHash, seed 0); no native struct is serialized. One package holds one provider (role) with one instance per real style, all for the single cell size of the build (`fontpack.py build --cell WxH`, at most 64x127 so that a two-cell glyph fits the glyph record's u8 width and i8 bearing/top); the runtime supports no other size.

## Layout

```
[header 128 B][section table][sections ...][pages ...]
```

### Header

| Offset | Type | Field |
|---:|---|---|
| 0 | u8[8] | magic `SHRFPKG1` |
| 8 | u16 | format version = 2 |
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
| 1 | MANIFEST | 1 × 36 B: `u8 role, u8[3] 0, u8[8] locale, u32 name_off, u32 name_len, u32 build_off, u32 build_len, u32 glyph_count, u32 instance_count` |
| 2 | STRINGS | UTF-8 bytes |
| 3 | SOURCES | 48 B: `u32 name_off, u32 name_len, u8[32] sha256, u32 license_off, u32 license_len`; strings inside STRINGS; the licence is the SPDX conjunction of the font's licence and every component licence |
| 4 | INSTANCES | 48 B: `u16 source, u8 style, u8 format, u16 line_height, u16 cell_width, u32 ppem_26_6, i16 baseline, i16 underline_y, i16 strike_y, u16 raster_flags, u8[16] font_instance_id, u8[12] 0`; raster_flags bit0 = fontpack's pinned FreeType raster mode, other bits 0 |
| 5 | STYLES | 8 B: `u16 line_height, u8 requested_style, u8 0, u16 instance, u16 0`, sorted by (line_height, requested_style); styles 0–3 for every instance line height, mapped to an instance of that height |
| 6 | CMAP | 8 B: `u32 scalar, u32 glyph_index`, sorted by scalar, unique |
| 7 | SEQS | 12 B: `u8 length, u8 kind, u16 0, u32 pool_index, u32 glyph_index`, sorted by scalar array, unique |
| 8 | SEQPOOL | u32 scalars |
| 9 | PAGES | 32 B: `u64 offset, u64 xxh3, u32 length, u32 first_glyph, u32 glyph_count, u32 0` |
| 10 | COVERAGE | count 9, length 36: u32 counters scalars, sequences, aliases, uvs, ivs, missing, style_fallbacks, glyphs, bitmap_bytes |

Roles: 1 latin, 2 cjk, 3 symbols, 4 emoji, 5 nerd. Styles: 0 regular, 1 bold, 2 italic, 3 bold italic; an instance's style is 0 or 1 (real faces only). Formats: 1 A4, 2 A8. Sequence kinds: 1 glyph, 2 alias, 3 default UVS, 4 non-default UVS. Every key maps to a glyph index, so aliases share bitmaps and cannot form cycles.

Metrics are in pixels from the top of the line cell: `baseline`, `underline_y`, `strike_y`.

### Glyph ids and pages

`glyph = instance × glyph_count + glyph_index`. Pages are ordered, contiguous and cover every glyph once. A page payload is

```
u32 glyph_count, then glyph_count × 16 B headers, then bitmaps
header: u32 bitmap_offset, u16 length, u8 width, u8 height,
        i8 bearing_x, i8 top, u8 stride, u8 flags, u16 source_glyph, u16 0
flags:  bits0-1 format (0 none, 1 A4, 2 A8), bits2-3 cells, bit4 style fallback
```

`bitmap_offset` is relative to the page start. `bearing_x` is from the left edge of the first cell, `top` is the number of rows above the baseline. A bitmap's format is its instance's format. A4 packs the left pixel in the high nibble; odd widths pad the last low nibble of every row with 0 and `(height - 1) × stride + row_bytes ≤ length`. Glyphs without a bitmap (spaces) have format 0 and length 0.

## Loader limits

A package is rejected unless every rule above holds and: at most 16 sections and 2^22 records per section; MANIFEST, INSTANCES, STYLES, CMAP and PAGES present; 1–64 instances; at least one glyph; at most 2^16 pages of at most 1 MiB each; the section table plus sections at most 128 MiB; SEQS only with the sequence feature and SEQPOOL, each sequence no longer than the text profile's cluster limit. COVERAGE is informational (but must have its fixed size): `missing` counts profile sequences the source fonts cannot draw.

The runtime checks the header and its checksum when a package opens, the section checksums and every record except page payloads when its index loads (sections stay where they were read: the index buffer, or the mapping), and a page payload (checksum, glyph records, bitmap formats, A4 padding) each time the page is read, or once for a mapped package. Nothing else is hashed; there is no whole-package hash. All of it runs in `shr_pump()`: a frame that needs an unopened package or an unchecked page draws a provisional fallback.

Failures: a format, checksum or profile error, or a page longer than the page cache (SHR_E_LIMIT), disables the package or page for the font's lifetime (a failed package frees its buffers and closes its source at once). A page whose checksum does not match after a read is first re-read like an I/O error. An I/O error or timeout of open() or a read is retried after `io_retry_ns`; after `io_retry_limit` retries (counted per page while frames want it, and per package until it is ready) the package (closed) or page cools down: SHR_EVENT_RESOURCE_FAILED is queued and its glyphs draw the provisional fallback. A cool-down ends after `io_retry_ns` or at `shr_asset_ready()`, whichever comes first (with `io_retry_ns` 0 only at the signal); the pump then redraws, and the package or page is tried again by the next frame that needs it. After `io_retry_limit` cool-downs in a row it is given up: until it loads, only `shr_asset_ready()` ends its cool-downs, and no deadline is reported for them. A refused read (`SHR_E_WOULD_BLOCK`) is retried the same way without counting. `SHR_E_NOT_FOUND` from open() means not installed: the package stays absent for the font's lifetime; to use a package installed later, create a new font and pass it to `shr_pl_lyr_tilemap_resize()`, which redraws the tilemap with it. Emoji-presentation scalars the emoji package lacks (or all of them, when it is not installed) use the text fallback chain, then U+FFFD.

## Build and install

Providers are fixed at build time. The Nerd package serves every Private Use scalar of the pinned Nerd font's cmap (the text profile's Nerd class is generated from the same font), the emoji package the emoji-presentation scalars, and latin, CJK and symbols share the text fallback chain. `build` fails when a scalar of a package's source cmap is routed to it but no package serves it, unless `fontpack.config.json` lists it under `coverage_allowlist.<package>.scalars` with a `reason`; `coverage.json` reports both lists per package. `build` also fails while `fonts.lock.json` has an unverified licence component. `inventory.json` records per licence and component the SPDX id, verification state and the provenance (URL, SHA-256) of every licence file. Outputs are staged and moved into place file by file; `build` never downloads (inputs come from `make fontpack-fetch`).

`install` verifies each package, copies NOTICE, LICENSES/ and inventory.json from the packages' build directory (whose inventory must describe exactly those package ids), rewrites an existing payload unless it is byte-identical to the verified package (temporary file, fsync, rename, directory fsync), and only then writes the next activation record.

`fontpack.py verify`, `install` and `build` apply the same rules to every page and glyph (with explicit errors, also under `python -O`); a build that would produce no instances or glyphs fails without writing a package. The build string records the tool versions, raster mode and shaping properties (script, language, direction, features).

The CMake build runs `fontpack.py build --cell WxH` into `<build dir>/fonts` (target `shiroko_fonts`) and `fontpack.py builtin --cell WxH --out FILE.c`, which writes the built-in package (ASCII U+0020–007E and U+FFFD from the Latin regular face, same raster settings, all four styles on the one instance) as `shr__builtin_package` / `shr__builtin_package_size`. The library opens it like any memory-mapped package and takes the line metrics from it.

## Activation record (64 B)

`u8[8] magic SHRFACT2, u64 generation, u8[32] package_id, u64 package_size, u64 XXH3-64 of bytes [0, 56)`. Two slots (A/B) are kept; the valid record with the higher generation wins, so a torn write falls back to the previous package.
