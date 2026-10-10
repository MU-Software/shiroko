# Shiroko font package format v6

All integers are little-endian with the widths given below; every checksum is XXH3-64 (xxHash, seed 0); no native struct is serialized. Python `struct` formats are given for every record. One package holds one provider (role) with one instance, from its regular face, for the single cell size of the build (`fontpack.py build --cell WxH`, at most 64x127 so that a two-cell glyph fits the glyph record's u8 width and i8 bearing/top); the runtime supports no other size. Packages hold no bold or italic faces: `SHR_STYLE_BOLD` and `SHR_STYLE_ITALIC` text is drawn from the regular coverage, which drivers embolden and slant (`SHR_GLYPH_BOLD`, `SHR_GLYPH_ITALIC` in `shiroko_driver.h`), except for glyphs the emoji or Nerd package serves and for the glyphs that join their neighbours: box drawing and block elements (U+2500–259F), Braille (U+2800–28FF), branch drawing (U+F5D0–F60D) and legacy computing (U+1CC00–1CEBF, U+1FB00–1FBFF).

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

A stored box's payload is its raw bytes. A zstd box's payload is one zstd frame, except a page box (below). Compression is a choice of the build; the runtime decodes whatever method a box declares. Only `ctri`, `seqs`, `pool` and `page` may be zstd.

### `shrf`: header (128 B, offset 0)

`"<I4sHHIHHIIIQQ32sQ32xQ"`:

| Offset | Type | Field |
|---:|---|---|
| 0 | box | size 128, type `shrf`, version 0, flags 1, raw_size 112: the bytes `80 00 00 00 73 68 72 66` are the magic |
| 16 | u16 | format version = 6 |
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
| `inst` | stored | 48 B, `"<HBBHHIhhhH16sHH8x"`: `u16 source, u8 0, u8 format, u16 line_height, u16 cell_width, u32 ppem_26_6, i16 baseline, i16 underline_y, i16 strike_y, u16 raster_flags, u8[16] font_instance_id, u16 page_width, u16 page_height, u8[8] 0`; page_width and page_height (the page shape W × H) are each 64, 128, 256 or 512; raster_flags bit0 = fontpack's pinned FreeType raster mode, other bits 0; ppem_26_6 is nominal: symbols and CJK glyphs that would not fit their cells and every Nerd glyph are sized per glyph |
| `ctri` | stored or zstd | the scalar table (below) |
| `seqs` | stored or zstd | n × 12 B, `"<BBHII"`: `u8 length, u8 kind, u16 0, u32 pool_index, u32 glyph`, sorted by scalar array, unique |
| `pool` | stored or zstd | n × 4 B: u32 scalars |
| `ptab` | stored | page_count × 24 B, `"<QQIHH"`: `u64 offset, u64 xxh3 (of the page box), u32 size (of the page box), u16 glyph_count, u16 height` |
| `covr` | stored | 36 B, `"<9I"`: counters scalars, sequences, aliases, uvs, ivs, missing, reserved (written as 0), glyphs, bitmap_bytes |

Roles: 1 latin, 2 cjk, 3 symbols, 4 emoji, 5 nerd. Formats: 1 A4, 2 A8. Sequence kinds: 1 glyph, 2 alias, 3 default UVS, 4 non-default UVS. Metrics are in pixels from the top of the line cell: `baseline`, `underline_y`, `strike_y`.

A **glyph** value names where a key's bitmap is: `page << 12 | record` for record `record` of page `page`, or one of two values no page can produce: MISS `0xFFFFFFFF` (the package has no glyph for the key) and BLANK `0xFFFFFFFE` (the key has a glyph that draws nothing, such as a space). Every key maps to a glyph value, so aliases share bitmaps and cannot form cycles.

### `ctri`: scalar table

A three-level table from a scalar `cp` (≤ 0x10FFFF) to its glyph value: `cp >> 10` (11 bits) picks a level-2 block in L1, `cp >> 4 & 63` (6 bits) a data block in that level-2 block, and `cp & 15` (4 bits) the glyph value in that data block. Raw payload:

| Offset | Type | Field |
|---:|---|---|
| 0 | u32 | l2_count: level-2 blocks (1..65536) |
| 4 | u32 | data_count: data blocks (1..65536) |
| 8 | u32[2] | 0 |
| 16 | u16[1088] | L1: level-2 block of each 1024 scalars |
| 2192 | u16[64] × l2_count | level-2 blocks: data block of each 16 scalars |
| D | u32[16] × data_count | data blocks: glyph values |

D = 2192 + 128 × l2_count rounded up to a multiple of 64; the bytes before D are 0, and raw_size = D + 64 × data_count. Level-2 block 0 is all 0 and data block 0 is all MISS, so scalars of empty ranges cost the same three reads. Blocks may be shared; which numbers they get is the builder's choice. A stored `ctri` box starts where (offset + 16) % 64 == 0, so its data blocks start at multiples of 64 B in the file and in a package mapped at a 64-aligned address.

### `page` boxes

Page i holds glyph_count glyphs (1 to 4096), the records 0 .. glyph_count − 1 that glyph values `i << 12 | record` name, in the top `height` rows (1 ≤ height ≤ H) of a W × H atlas of stride = W × bytes per pixel (A4 ½, A8 1). Only glyphs with a bitmap have records; keys of glyphs that draw nothing map to BLANK. Its raw content is

```
atlas:   height rows of stride bytes, no padding                       (height × stride B)
records: glyph_count × 16 B, "<HHBBbbBBHI":
         u16 x, u16 y, u8 width, u8 height, i8 bearing_x, i8 top, u8 flags, u8 0, u16 source_glyph, u32 0
         flags: bits0-1 format (the instance's: 1 A4, 2 A8), bits2-3 cells, bits4-7 0
```

The page box: header (type `page`, version 0, flags REQUIRED | METHOD << 4, raw_size = 8 + height × stride + 16 × glyph_count), then `"<II"` page_index (= i), atlas_stream_bytes, then the atlas stream (atlas_stream_bytes B), then the records stream (the rest of the box). The 8 bytes `page_index, atlas_stream_bytes` are never compressed. Stored: the streams are the atlas and the records as they are. zstd: each stream is one zstd frame, of the atlas and of the records; both streams are frames.

A stored page box starts where (offset + 24) % 256 == 0, so its atlas starts at a multiple of 256 B in the file and can be drawn from in place in a package mapped at a 256-aligned address (as the built-in one is). zstd pages are packed tightly.

A glyph's bitmap is the rect `[x, x + width) × [y, y + height)` of the atlas, in the instance's format (A4 packs the left pixel in the high nibble), with `x + width ≤ W` and `y + height ≤` the page's height; `bearing_x` is from the left edge of the first cell, `top` is the number of rows above the baseline. An A4 glyph starts at an even `x` and, for an odd width, the column right of its rect (inside the atlas, whose width is even) is a zero nibble in every row of the rect. Rects may touch (readers take nothing outside a glyph's rect).

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

The runtime and `fontpack.py` (`Package`) check these rules in this order; the first that fails rejects the package (or the page) with the status given (SHR_E_FORMAT where none is). "Known" types are `mani strs srcs inst ctri seqs pool ptab covr`.

**Header** (read when the package opens):

1. The source holds at least 128 bytes.
2. Bytes [0, 8) are `80 00 00 00 73 68 72 66` (a v4 file starts with `SHRFPKG1` and fails here).
3. Version 0, flags 1, raw_size 112, format version 6, bytes [18, 20) zero.
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
    6. flags: no bit set but REQUIRED and METHOD; REQUIRED set unless `covr`; METHOD 1 only for `ctri`, `seqs`, `pool`, and only with required feature bit3; version 0;
    7. stored: raw_size = size − 16; zstd: raw_size > size − 16.
12. `mani`, `inst`, `ctri` and `ptab` are present.
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
21. `ctri`: raw_size ≥ 2192; 1 ≤ l2_count, data_count ≤ 65536 and the two u32 after them 0; raw_size = D + 64 × data_count with the bytes before D 0; every L1 entry < l2_count and every level-2 entry < data_count; level-2 block 0 all 0 and data block 0 all MISS; stored: (offset + 16) % 64 = 0.
22. `pool`: a multiple of 4 B, at most 2^22 scalars.
23. `seqs` (when present): required feature bit2, `pool` present, a multiple of 12 B, at most 2^22 records; every pool scalar ≤ 0x10FFFF and not a surrogate; each record: 2 ≤ length ≤ the text profile's cluster limit, kind 1–4, zero 0, pool_index + length ≤ pool size; keys strictly increasing.
24. `ptab`: raw_size = 24 × page_count (mani); 1 ≤ page_count ≤ 2^16. Each entry, in order: 1 ≤ glyph_count ≤ 4096; 1 ≤ height ≤ page_height; 24 ≤ size ≤ 1 MiB; offset ≥ the end of the previous page (the first: ≥ the end of the last box `sidx` lists, known or not); offset + size ≤ file size.
25. The pages' glyph_count sum to glyph_count (mani). Every glyph value in the `ctri` data blocks and in `seqs` is MISS, BLANK, or names a record: page < page_count and record < that page's glyph_count. U+0020 maps to MISS or BLANK.

**Page** i (each time it is read, or decoded from a mapping):

26. XXH3 of the page box equals the ptab entry's — SHR_E_CHECKSUM (after a read, the page is first re-read like an I/O error).
27. Header: type `page`, version 0, size the entry's; METHOD ≥ 2 — SHR_E_UNSUPPORTED; flags: REQUIRED set, no bit but REQUIRED and METHOD, METHOD 1 only with required feature bit3; raw_size = 8 + height × stride + 16 × glyph_count.
28. Stored: size = raw_size + 16 and (offset + 24) % 256 = 0.
29. page_index = i; stored: atlas_stream_bytes = height × stride; zstd: atlas_stream_bytes ≤ size − 24.
30. The atlas stream decodes to height × stride bytes, then the records stream to 16 × glyph_count bytes.
31. Each record: flags & 0xF0 = 0, cells 1 or 2, the zero byte and the u32 0; format = the instance's, width and height ≥ 1, A4 x even, x + width ≤ page_width, y + height ≤ the page's height, and for an odd A4 width the padding nibble (in the decoded atlas) zero in every row.

Overlapping rects are not rejected: they draw wrong pixels but never read outside the page. Nothing else is hashed; there is no whole-package hash and no hash of decoded bytes (decoding is deterministic and its input is hashed). COVERAGE is informational: `missing` counts profile sequences the source fonts cannot draw.

`fontpack.py verify`, `install` and `build` apply rules 1–31 (pages: all of them; the cell size and file name only in `install`) and then walk the box chain: from offset 0 the boxes tile the file; each is `shrf`, `sidx`, a box `sidx` lists (its header's type and size those of the entry), a page `ptab` lists, or a `free` box; every listed box and page is reached. Errors are explicit, also under `python -O`. A v4 file fails with "format v4 package: rebuild with fontpack 6", a v5 file with "format v5 package: rebuild with fontpack 6".

## Runtime

The runtime opens a package in three asynchronous steps, all in `shr_pump()`: the header (128 B, rules 1–8), the index (rules 9–13), then one read of the metadata region (rules 14–25), whose boxes are decoded into one resident allocation (each box at a multiple of 64 B). A mapped package's stored boxes are used in place; its zstd boxes are decoded the same way. The built-in package and stored indexes need no zstd decoder; the decoder context (about 96 KB) is allocated at a font's first compressed box and shared by its packages. A runtime built without zstd (CMake `SHIROKO_ZSTD=OFF`) rejects any package whose header declares bit3 at open, before reading further.

`shr_pl_res_bitmap_font_preload()` loads a package's first pages (its hot tier with the default order) without a frame: from `shr_pump()`, one page per pump while no frame waits for pages and no other preload read is in flight, until the cache would have to evict, which ends it. Pages that exist already, whose read cannot start or whose descriptor cannot be allocated are skipped; a failed preload read is retried only by a frame that needs the page.

Resident pages live in slots of the page cache (`page_cache_bytes`), driver buffers of the page shape (format, W, H); an evicted slot takes a page of any package with the same shape. A page is decoded into the top `height` rows of its slot; the rows below are zeroed after every decode. A page charges H × the slot's stride, its record block (16 × glyph_count) and, while its read is in flight, a staging buffer of the page box's size. A stored page of a mapped package (and of the built-in one) is drawn from in place when the driver can wrap it, else decoded into a slot. To make room, a page evicts only the oldest unpinned pages that the latest frame neither drew from nor wanted; when that is not enough, the page waits for memory (read and mapped packages alike) and the frame settles with fallback glyphs, while a page larger than the whole cache fails with SHR_E_LIMIT.

Failures: a format, checksum or profile error, or a page whose slot is larger than the page cache (SHR_E_LIMIT) or than the driver's buffers (SHR_E_UNSUPPORTED), disables the package or page for the font's lifetime (a failed package frees its buffers and closes its source at once). A page whose checksum does not match after a read is re-read like an I/O error, then fails with SHR_E_CHECKSUM; a page that fails rules 27–31 fails with its status at once, without a re-read. A mapped package or page that runs out of memory waits and is retried, as a read package does. An I/O error or timeout of open() or a read is retried after `io_retry_ns`; after `io_retry_limit` retries (counted per page while frames want it, and per package until it is ready) the package (closed) or page cools down: SHR_EVENT_RESOURCE_FAILED is queued and its glyphs draw the provisional fallback. A cool-down ends after `io_retry_ns` or at `shr_asset_ready()`, whichever comes first (with `io_retry_ns` 0 only at the signal); the pump then redraws, and the package or page is tried again by the next frame that needs it. After `io_retry_limit` cool-downs in a row it is given up: until it loads, only `shr_asset_ready()` ends its cool-downs, and no deadline is reported for them. A refused read (`SHR_E_WOULD_BLOCK`) is retried the same way without counting. `SHR_E_NOT_FOUND` from open() means not installed: the package stays absent for the font's lifetime; to use a package installed later, create a new font and pass it to `shr_pl_lyr_tilemap_resize()`, which redraws the tilemap with it. Emoji-presentation scalars the emoji package lacks (or all of them, when it is not installed) use the text fallback chain, then U+FFFD.

## Build and install

Providers are fixed at build time. The Nerd package serves every Private Use scalar of the pinned Nerd font's cmap (the text profile's Nerd class is generated from the Mono face of the same release, which has the same cmap), the emoji package the emoji-presentation scalars, and latin, CJK and symbols share the text fallback chain. `build` fails when a scalar of a package's source cmap is routed to it but no package serves it, unless `fontpack.config.json` lists it under `coverage_allowlist.<package>.scalars` with a `reason`; `coverage.json` reports both lists per package. `build` also fails while `fonts.lock.json` has an unverified licence component. `inventory.json` records per licence and component the SPDX id, verification state and the provenance (URL, SHA-256) of every licence file. Outputs are staged and moved into place file by file; `build` never downloads (inputs come from `make fontpack-fetch`).

A CJK package's ppem is the largest, in 1/64 steps up to the Latin em, at which the 99.9th-percentile rows of its kana, Han and Hangul hinted bitmaps above plus below the baseline fit the line height; all its glyphs move by the same rows to sit inside the line, and the few that still do not fit are moved or shrunk into their cells one by one. Nerd glyphs are sized and placed in their cell, unhinted, by the rules of nerd-fonts' font-patcher (the version and inputs pinned in `fonts.lock.json` `nerd_rules`; font-patcher is parsed, never run) as Ghostty places them: most icons scale to fit a box of the cell width and (2 × cap height + line height) / 3, centred, the Powerline glyphs that are not generated stretch to the cell, and every glyph is cut to its cell. The build string records the tool versions (zstd included), raster mode and shaping properties (script, language, direction, features).

**Page shape.** `fontpack.config.json` gives per format the default shape (`page_atlas`), the candidate shapes (`page_shapes`, each side 64–512, a power of two) and the most glyphs per page (`max_page_records`, at most 4096). Candidates are the shapes in which every glyph fits (A4 span width + (width & 1) ≤ W, height ≤ H). If all glyphs fit on one page of some candidate whose trimmed atlas (used rows × stride) is smaller than one default page, the package takes the candidate with the fewest trimmed bytes (ties: the smaller W, then the smaller H); otherwise the default. Glyphs are shelf-packed in slot order; a page ends when the next glyph does not fit below the last shelf or the page holds `max_page_records`. Every page's height is H except the last one's: its used rows (at least 1). Atlas bytes no glyph covers are 0. `--page-atlas WxH` forces that one shape for both formats (the fuzz seeds use the smallest shape of at least 2 cells by 1 line).

**Glyph order.** The runtime finds glyphs through `ctri` and `seqs`, so their order is the build's choice; it decides how many pages a screen touches. With `--order hot` (the default) the scalar glyphs go in tiers, each in scalar order: CJK packages by the lead-byte rows of the locale's legacy double-byte set as Python's codecs map them (ko: KS X 1001 punctuation A1, compatibility jamo A4 and Hangul B0–C8, its other symbols A2–AC, its Hanja CA–FD; ja: JIS X 0208 A1–CF in EUC-JP, which is symbols, kana and level 1, then level 2 D0–F4; zh-Hans: GB 2312 A1–D7, then D8–F7; zh-Hant and zh-HK: Big5 A1–C6, the 5,401 level 1 characters with the symbols before them and the Eten additions of row C6 that Python decodes, then C9–F9), latin, symbols and Nerd by the scalar ranges of `HOT_RANGES` in fontpack.py, the rest after them. Emoji go single glyphs and their VS16 forms first, then keycaps and flags, skin-tone sequences, ZWJ sequences. Other sequence glyphs and alternate UVS glyphs come last. Two optional inputs of `fonts.lock.json` `order`, fetched by `make fontpack-fetch` when available, refine the CJK tiers: Unihan (`.cache/ucd/Unihan.zip`) puts the Korean education Hanja (`kKoreanEducationHanja`) before the other KS X 1001 Hanja, moves the Jōyō kanji (`kJoyoKanji` 2010) of ja into its first tier ahead of the other level 1 kanji, and ranks the zh tiers by `kHanyuPinlu` (summed over readings); the syllable frequencies of the National Institute of Korean Language's 현대 국어 사용 빈도 조사 2 (2005; `음절통계.txt` in `.cache/fonts/src/korean-frequency-2005.zip`, KOGL Type 1) rank the first ko tier, punctuation and jamo first, then the Hangul from the most frequent (ties by scalar). Without them the tiers keep scalar order and `build` says so; a fetched input that does not match its SHA-256 fails the build. `--rank FILE` replaces the fetched ranks: each tier by first appearance in a UTF-8 text file first. `--order codepoint` keeps the scalar order of earlier builds. `build` reports per package the pages that hold its first tier ("hot") and the order inputs used (`order_inputs` in `coverage.json`); NOTICE and `inventory.json` (`order_inputs`: URL, SHA-256, licence) attribute the inputs a package was built with.

**Compression.** By default `ctri`, `seqs`, `pool` and pages are zstd: level 19, window log at most 18, content size written, no checksum or dictionary id, one frame per stream. A box (a page: either stream) whose frame is not smaller than its raw bytes is stored. `--store-index` keeps `ctri`, `seqs` and `pool` stored (for flash-mapped packages, whose stored boxes need no decoding); `--method stored` stores everything. Required feature bit3 is set when some box ended up zstd. Metadata boxes follow `sidx` in the order `mani strs srcs inst ctri seqs pool ptab covr`, then the pages. A stored `ctri` is preceded by a `free` box of g = (−(offset + 16)) mod 64 bytes, or g + 64 when 0 < g < 16 (none when g = 0); a stored page by one of g = (−(offset + 24)) mod 256 bytes, or g + 256 when 0 < g < 16. Glyphs are numbered in page order as they are packed; glyphs without a bitmap get no record, and their keys map to BLANK. `package_id` and `install`'s byte-identical check therefore depend on the libzstd that the pinned `zstandard` bundles.

`install` verifies each package, copies NOTICE, LICENSES/ and inventory.json from the packages' build directory (whose inventory must describe exactly those package ids), rewrites an existing payload unless it is byte-identical to the verified package (temporary file, fsync, rename, directory fsync), and only then writes the next activation record. Only the runtime checks that the instance matches its cell size; a build that would produce no glyphs fails without writing a package.

The CMake build runs `fontpack.py build --cell WxH` into `<build dir>/fonts` (target `shiroko_fonts`) and `fontpack.py builtin --cell WxH --out FILE.c`, which writes the built-in package (ASCII U+0020–007E and U+FFFD from the Latin regular face, same raster settings; always stored, its shape chosen by the rule above) as `shr__builtin_package` (aligned to 256 B) / `shr__builtin_package_size`. The library opens it like any memory-mapped package and takes the line metrics from its `inst` box. `build` reports each package's size, shape, pages and `atlas_fill`: the share of atlas pixels glyphs cover, over W × height summed over pages.

## Activation record (64 B)

`u8[8] magic SHRFACT2, u64 generation, u8[32] package_id, u64 package_size, u64 XXH3-64 of bytes [0, 56)`. Two slots (A/B) are kept; the valid record with the higher generation wins, so a torn write falls back to the previous package.
