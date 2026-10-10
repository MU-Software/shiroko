# Shiroko

C11 text and framebuffer renderer, built as one static library (`shiroko::shiroko`).

SPDX-License-Identifier: MIT (see `LICENSE`; third-party notices in `NOTICE`).

## Dependencies

- CMake ≥ 3.21 and a C11 compiler (a C++17 compiler for the header test)
- [vcpkg](https://vcpkg.io) with `VCPKG_ROOT` set (manifest `vcpkg.json`: `uthash` and `xxhash` for the library,
  `greatest` and `stb` (PNG output) for tests)
- [uv](https://docs.astral.sh/uv/) for the Python tools (`pyproject.toml`, `uv.lock`, `.python-version`)
- Docker for the Linux sanitizer, fuzz and x86_64 GCC targets (`make sanitizer-image gcc-image`)
- For `make desktop-run` only: SDL3 and a shared ANGLE, both through vcpkg (preset `desktop`)
- For `make tab5-build` only: Docker with the `espressif/idf:v6.1` image (M5Stack Tab5, ESP32-P4); vcpkg installs
  zstd's decoder sources (overlay port `cmake/ports/zstd-source`, feature `zstd-source`) for ESP-IDF to compile

On macOS: `brew install cmake uv`.

## Fonts and the build

Every Make build and test target syncs the uv tools environment (`uv sync --locked --group tools`, cheap when up to
date); `make fontpack-fetch` (run by the first `make test`) downloads the pinned font and Unicode inputs into
`.cache/fonts` and `.cache/ucd` (`make FONT_CACHE=... UCD_CACHE=...` to put them elsewhere), and, when available, the
glyph order inputs: Unihan and the Hangul syllable frequencies of 현대 국어 사용 빈도 조사 2 (National Institute of Korean
Language, 2005, KOGL Type 1), attributed in the packages' NOTICE (`docs/font-package-format.md`, Glyph order). The CMake
build never downloads and never syncs: it runs the tools with `uv run --frozen --no-sync --offline` and fails at
configure time, asking for `make fontpack-fetch`, when the inputs or the environment are missing.

The build generates the Unicode tables (`<build dir>/generated/shr_gen_unicode_tables.c`) from `.cache/ucd`, and
`test_grapheme` reads `GraphemeBreakTest.txt` from there.

The build bakes the fonts for one cell size, `SHIROKO_CELL_WIDTH` x `SHIROKO_CELL_HEIGHT` (default 8x16; width 6..64,
height 8..127; `make CELL_WIDTH=10 CELL_HEIGHT=20 ...`). The built-in package (ASCII + U+FFFD) is compiled into the
library; the other packages go to `<build dir>/fonts` with their NOTICE, LICENSES/ and inventory.json (target
`shiroko_fonts`, part of the default build when Shiroko is the top-level project).

The default packages are `latin`, `cjk-ko`, `symbols`, `emoji` and `nerd`, so the only CJK glyph variant is
Korean. For the `ja`, `zh-Hans`, `zh-Hant` and `zh-HK` variants (`shr_pl_res_bitmap_font_desc.locale`),
`make fontpack-locales` bakes every package, including `cjk-ja`, `cjk-zh-Hans`, `cjk-zh-Hant` and `cjk-zh-HK`,
into `build/fonts-locales`.

Box drawing and blocks (2500-259F, in `latin`), Braille, legacy computing and branch drawing (in `symbols`) and the
Powerline separators (in `nerd`) are drawn for the cell size instead of rasterised from the fonts, so lines join
across cells and blocks fill them exactly (`fontpack.config.json` "generated", `tools/fontpack/sprite`; the rules
follow Ghostty's sprite code). `fontpack.py selftest` checks their invariants at eight cell sizes.

Packages are zstd-compressed (`SHIROKO_ZSTD`, default ON, vcpkg feature `zstd`); `-DSHIROKO_ZSTD=OFF` bakes stored
packages and builds a library that reads only those (`make test-nozstd`). A library built with zstd reads both: each
box records its method, only zstd boxes go through the decoder, and the decoder's context (`ZSTD_DCtx`, 94 KiB on a
32-bit target) is allocated once per font, from the context's allocator as `SHR_ALLOC_PAYLOAD`, when the font first
opens a package with a zstd box.

The screen pixel format is `SHIROKO_PIXEL_FORMAT`: `RGB565` (default) or `RGBX8888` (preset `host-rgbx`,
`make PIXEL_FORMAT=RGBX8888 test`; presets `desktop` and `make desktop-run` use it too).

The render comparison test (`tests/render`, `test_render`) draws a catalog of scenes offscreen through the public API:
its golden list (`tests/render/golden.txt`, XXH3-128 per scene, cell size and pixel format) holds 8x16 entries only, so
other cell sizes skip it (exit code 77) after running the reftests (scene pairs that must draw identical pixels).
`SHR_UPDATE_GOLDEN=1` rewrites this build's entries. Mismatches leave expected/actual/diff PNGs in
`build/render/failures` (`build/render_scalar/failures` for `test_render_scalar`). `test_render --frames N` times N full
redraws per scene after the first frame; with `SHIROKO_PORT_ANGLE`, `--backend angle` (ctest `test_render_angle`;
`test_render_angle_keeps` with keeps) compares the GPU driver against the software images within per-scene fuzzy limits.

## Common targets

```
make test          # unit/golden tests, fontpack selftest, headless example, fuzz corpus replay
make core-test     # text measurement tests only
make fontpack      # font packages into build/host/fonts
make fontpack-locales  # all packages, every CJK locale, into build/fonts-locales
make render-test   # render comparison suite (goldens, reftests)
make render-export # every scene as PNG into build/render (RENDER_ARGS='--frames 100' times them instead)
make headless-run  # render examples/headless into a PPM image
make desktop-run   # SDL3 window, software and ANGLE drivers (DESKTOP_ARGS='--load scroll --frames 300 --quit')
make tab5-build    # M5Stack Tab5 firmware (examples/tab5) into build/tab5; tab5-flash, tab5-monitor (PORT=...)
                   #   (newer sdkconfig.defaults or Kconfig remake build/tab5/sdkconfig, losing menuconfig edits)
make bench         # per-part throughput (BENCH=software|compositor|tilemap|font|image|terminal filters;
                   #   SHR_BENCH_SECONDS=5 lengthens one case for a profiler)
make replay        # the Tab5 scenes recorded, then replayed into the software driver and the compositor alone
                   #   (REPLAY_ARGS='scroll,code 60'; replay-tab5 with the Tab5's packages; replay-cost: the
                   #   app cost table's scenes)
make replay-dma    # the same with the Tab5 example's keep copier (SHR_REPLAY_COPIER=tab5) for 8x16 10x20 12x24
                   #   16x32 (DMA_CELLS=...): fails when a copy the DMA2D could take, or a band rotation, goes to the CPU
make test-asan test-ubsan test-tsan   # host sanitizers (Apple Clang or the host compiler)
make test-sanitizers fuzz fuzz-msan   # adds the Linux container sanitizers; HWASan needs an arm64 host
make test-gcc      # x86_64 GCC 13 at -O0, -O2 and ASan+UBSan
make coverage      # clang source-based coverage; fails below COV_MIN (default 100); LLVM_PROFDATA, LLVM_COV
make check-inline  # hot functions (set_cell, set_row, flush, frame commands, software drawing) call only what
                   #   tools/inline/{clang,gcc,riscv}.txt allows, in the release, GCC and Tab5 builds;
                   #   check-inline-host|gcc|tab5 one by one; INLINE_ARGS=--record rewrites the lists
```

`-DSHIROKO_WERROR=ON` turns warnings in the library and the tests into errors. Every test has a 300 s timeout.

CI needs `VCPKG_ROOT` and must cache `.cache/` and the uv environment (`.venv` and uv's cache) together; direct
CMake builds do not sync, so run a Make target (or `uv sync --locked --group tools`) after restoring them.

The fuzz corpus replay (`fuzz_*_corpus` tests) reads the seeds the build writes (target `fuzz_seeds`:
`tools/fuzz/make_seeds.py` and, for `fuzz_package`, reduced packages baked for the build's cell size) and the
committed regression inputs in `tests/fuzz/corpus/<target>`.

## Runtime contract

- One owner thread makes every call except `shr_fence_signal()`, `shr_asset_complete()`, `shr_driver_ready()`
  and `shr_asset_ready()`, which are thread-safe (`shiroko.h`, "Context (compositor)").
- After `execute()` returns `SHR_IN_PROGRESS`, a driver thread may read `cmds`, `dst` and every buffer they refer
  to in place, without copying, until the fence resolves (`shiroko_driver.h`, above `shr_framebuffer_driver`).
- Work a driver or asset source refused (`SHR_E_WOULD_BLOCK`) is retried `io_retry_ns` later or at
  `shr_driver_ready()` / `shr_asset_ready()`; with `io_retry_ns = 0` only at those calls. A frame the output
  refused waits for `shr_output_ready()`.
- A font package (until it is ready) or page cools down after `io_retry_limit` failed opens and reads and is given
  up after `io_retry_limit` cool-downs in a row until `shr_asset_ready()` (`shr_context_desc`). A package that was
  not installed stays absent for the font's lifetime: to use one installed later, create a new font and pass it to
  `shr_pl_lyr_tilemap_resize()`.
- A rotated or converted screen is composed in a full-screen surface and converted into the output, or, with
  `shr_screen_desc.bands`, drawn band by band into one or two small app-owned CPU or DMA surfaces, each band's
  damage converted into the output by ROTATE (COPY) commands of its own batch (`shiroko.h`, `shr_screen_desc`).
- Keeps: a driver with `caps.max_keeps` holds rendered tilemap rows under ids the compositor assigns (KEEP_BEGIN/END
  store, KEEP_DRAW copies, KEEP_RELEASE empties, within `caps.keep_bytes`, each at most `caps.max_keep_bytes`). A row is
  stored the second time it is drawn (8 stores a frame, more while under 320 KiB), once per blink phase; rows met again
  are drawn from their keeps without resolving or emitting their commands. A driver whose stores cost about what drawing
  does sets `SHR_DRIVER_CHEAP_STORE` (`caps.flags`; the software driver does once it has a copier,
  `shr_software_driver_set_copier()`): rows are then also stored at first sight and past those limits, on a credit (from
  2/3 of `max_keeps`, one back each frame, two when such a row is drawn from its keep), and such stores never evict a
  keep that was drawn since it was stored. They pause after two frames in a row that draw or store more rows than they
  take from keeps, and in the first frame submitted after a `shr_pump()` that had nothing to do (no frame running or
  wanted, nothing loading; blink phases alone do not end the pause): a screen changed after a pause is not expected back
  soon. When a batch's stores fail for memory, keeps hold no more than they held before it, for the context's life.
  `shr_software_driver_create(allocator, keep_bytes, max_keeps, max_buffers, ...)` gives keep id k slot k of keep_bytes
  / max_keeps bytes (rounded down to 128), all in one block taken from its allocator at create and freed at destroy;
  `shr_angle_driver_create(allocator, texture_cache_bytes, keep_bytes, max_keeps, max_buffers, ...)` sizes its slots the
  same way, as texels of one array texture per destination format (`shiroko_driver.h`, above `shr_draw_cmd`;
  `port_angle.h`).
- BOLD and ITALIC glyphs: the software driver synthesizes a glyph's coverage once and draws it from its cache
  until a REGISTER, UPDATE or RELEASE of the glyph's buffer, in up to 256 KiB taken from its allocator as needed
  (`shr_software_driver_synth_cache()` sets the size, 0 synthesizes at every draw); the ANGLE driver synthesizes in
  its shader.
- Images: `shr_pl_res_image_create_from()` takes RGBA, RGB, gray or gray + alpha rows and converts them while copying,
  after checking `image_bytes` (at 2 bytes per pixel where the image may be kept as RGB565; a translucent image that
  needs 4 is refused once its alpha is read); on an RGB565 screen whose driver sets `SHR_DRIVER_IMAGE_565` (the software
  driver does), an image whose alpha is all 255 is kept as RGB565 (2 bytes per pixel) and drawn by copying rows, the
  same pixels. A released image no frame reads is freed at once; `shr_pl_res_image_budget()` gives the bytes in use and
  the limit.
- Translucent IMAGE into RGB565: the software driver blends from a plane of the RGBA8888 buffer (8 bytes per pixel) that
  the first such draw derives, while its planes fit in 8 MiB from its allocator (`shr_software_driver_image_planes()`
  sets the size, 0 makes none); a draw without one blends from the buffer, the same pixels.
- Scaled images: `shr_pl_res_image_create_scaled()` makes a scaled copy once (bilinear, or box for shrinking) from RGBA,
  RGB, gray or gray-alpha rows; `shr_pl_res_image_view()` shows part of an image scaled, drawn by drivers with
  `SHR_DRIVER_SCALE` (the software and ANGLE drivers) at every draw without a copy, otherwise copied once. Both give the
  same bytes (integer bilinear, `shiroko_driver.h`); a copy costs its pixels in `image_bytes`, a view costs a draw-time
  bilinear pass and keeps its source image alive.
- Scrolling: `shr_pl_lyr_tilemap_scroll(layer, top, bottom, n, style)` moves rows [top, bottom) by n rows (terminal
  SU/SD and scroll regions) without copying cells or rebuilding rows. Where the rows are opaque and the driver sets
  `SHR_DRIVER_CHEAP_MOVE` (`caps.flags`; the software and ANGLE drivers do), the frame moves their pixels with a COPY
  of the destination onto itself ahead of the other draws and draws only the uncovered rows and what other layers show
  over the moved area; otherwise the moved rows are drawn again (`shiroko_driver.h`, above `shr_draw_cmd`).
- Frame-rate cap: with `shr_context_desc.min_frame_interval_ns` (needs `now_ns`; 0 = uncapped, the default) a frame
  starts no sooner than that after the previous one started; changes made meanwhile accumulate and the next frame
  draws them together (scrolls included, as one move), so the final state always reaches the screen.
  `shr_next_deadline()` reports that start, and a blink phase that changed while waiting is drawn as it is at the
  frame's start.
- Text styles carry meaning in the colour alpha: fg 255 normal, 128 dim, 0 concealed; bg 255 painted, 0 none (other
  values are refused for now, so build colours with `SHR_RGB`). Underlines, strikes and overlines are not cell styles
  but lines of a row: `shr_pl_lyr_tilemap_set_lines(layer, row, lines, count, err)` sets a row's `shr_text_line` list
  (columns, kind, shape single/double/curly/dotted/dashed, blink, colour), which scrolls with the row and is cut by
  clears; drivers draw each as one `SHR_CMD_LINE` over the glyphs.
- `shr_pl_lyr_tilemap_set_row(layer, row, col, row_in, err)` takes a row at once as the VT engine holds it: cells by
  code point (`shr_row_cell`: text, style index, span, code point count), one style array, the code points of longer
  clusters and optionally the row's lines. It equals set_cell for each cell then set_lines, checks the whole row before
  changing anything, and skips cells the grid already holds.
- `shr_pl_lyr_tilemap_resize()` takes an optional background colour for cells without a bg of their own;
  `shr_pl_lyr_tilemap_measure()` lays text out into a caller array of `shr_text_cluster`.

## OpenGL ES driver (ANGLE)

`-DSHIROKO_PORT_ANGLE=ON` (preset `angle`, vcpkg feature `angle`: ANGLE with Metal on macOS) builds the static library
`shiroko_angle` (`shiroko::shiroko_angle`, header `shiroko/port_angle.h`): a synchronous `shr_framebuffer_driver` on
OpenGL ES 3.0 for CPU memory and GPU surfaces (`shr_angle_surface_create()`, `SHR_MEMORY_DEVICE` surfaces whose `pixels`
is a driver handle, so they can also be COPY/ROTATE sources such as a composition;
`shr_angle_surface_texture(driver, surface, &texture)` gives the GL texture, top row first). Every call needs the EGL
context the driver was created with current on the calling thread and returns `SHR_E_STATE` otherwise.
Registered buffers become layers of GL array textures (A4 kept packed, the shader picks the nibble), and a batch draws
as instanced quads with their clip, so a full screen of text is one or a few draws. Keep slots are sw x sh texels of one
array texture per destination format, created by its first store (sh at least a cell row, so a row of cells fits
whenever its bytes do); a batch stores its keep groups in one pass first, and KEEP_DRAWs share the destination's draw
call with glyphs.
`shr_angle_offscreen_create()` makes an EGL pbuffer context for tests and tools;
`SHIROKO_ANGLE_BACKEND` picks its backend (`metal`, `opengl`, `vulkan`, `d3d11`, `default`; unset: `metal` on macOS,
`opengl` elsewhere) and the others are tried when it fails. FILL, COPY, ROTATE and every RGBX8888 command match the
software port exactly. RGB565 blends (GLYPH, IMAGE, DIM) round once from the 5/6-bit destination in both drivers, as
GPUs blend; a GPU that blends 16-bit targets at reduced precision (Metal) still leaves under 1 % of the blended pixels
one level off.
Band composition works with CPU bands, each drawn through a scratch texture and read back.
`test_angle` compares both drivers and skips (exit code 77) without an EGL display. The library is installed but not
exported in `shirokoConfig.cmake`.

## Desktop example (SDL3)

`examples/desktop` (`-DSHIROKO_EXAMPLE_DESKTOP=ON`, vcpkg feature `desktop`: SDL3; preset `desktop`, RelWithDebInfo with
`SHIROKO_PORT_ANGLE` and RGBX8888 screen pixels) shows the `tests/render` scenes and 11 full-window load modes
(`scroll`, `churn`, `restyle`, `blink`, `images` and six `scroll-api` variants; `--help` lists them) in a window. Tab
switches between the software driver (an SDL_Renderer streaming texture) and the ANGLE driver (SDL's OpenGL ES 3.0
context; the composition is a GPU surface blitted to the window). SDL loads ANGLE itself, so the preset uses the overlay
triplet in `cmake/triplets` (arm64-osx for now: static, except ANGLE as a shared library) and the executable finds it
through its rpath; with a static ANGLE the example is software only. `SHIROKO_ANGLE_BACKEND` picks the backend as for
`shr_angle_offscreen_create()`.

RGBX8888 uploads without conversion: the texture is SDL's RGBA32 (same bytes, drawn without blending).
`make PIXEL_FORMAT=RGB565 desktop-run` builds the RGB565 screen instead; SDL's Metal renderer has no 16-bit texture, so
SDL converts every frame on the CPU there. The software mode uploads only the rectangle around what the frame drew
into the output (the example wraps the driver's `execute()` to note the commands' rectangles); SDL 3.4's Metal
renderer still allocates staging memory for every upload, and a persistent upload buffer would need SDL_GPU. On a
2560 x 1440 window, frames that change the whole screen are therefore bounded by the platform's texture upload: all
of the example's load modes do (MacBook Pro M4 Max, 120 Hz, vsync on, 2026-10-06: upload p50 1.1-2.2 ms, up to
7.3 ms), while a frame that changes one cell uploads 0.4 ms (p50) instead of 3.1 ms.

The ANGLE mode keeps up to two frames in flight with `glFenceSync`/`glClientWaitSync`, so the CPU and GPU overlap;
`--sync finish` restores a `glFinish` every frame. Each frame reports its work (the scene's changes, submit and pump
until presented, `glFinish` with `--sync finish`, and the texture upload or blit), its wait (the fence for the frame
before last, taking the next drawable, and the swap) and the interval to the next frame: the first frame alone, and
p50/p95/p99/max over the last 100 and 1000 frames, with missed frames (intervals over 1.5 display refreshes), on
stdout and, except in runs with `--frames`, `--print` or `--csv`, in the window title (setting the title takes 1-5 ms
under ANGLE, which would land in the measured frames).

A desktop frame is judged by three numbers, kept apart as frame-time tools such as PresentMon report them: the CPU time
of the worst frame, both the library's part (the load's changes, `shr_submit` and the pumps) and the example's whole
work (with the software driver's texture upload, or ANGLE's blit), with two frames in flight; the worst GPU time of a
frame, from a separate `--gpu-time on` run (its queries split ANGLE's command buffers, so its CPU times do not count);
and missed refreshes, for information only, since they come from the window system's waits. The first two must stay
within one refresh (8.33 ms at 120 Hz); `--sync finish` (CPU and GPU back to back) is a diagnostic. On a MacBook Pro M4
Max (2560 x 1440 window, 120 Hz, vsync on, every load twice for 2000 frames, 2026-10-06; power adapter, High Power mode,
`caffeinate -dimsu`, no other heavy work, the window focused) the worst CPU frame took 4.6 ms with the software driver
(3.3 ms in the library) and 5.1 ms with ANGLE (5.0 ms), the worst GPU frame 2.5 ms, with 2-11 missed refreshes per 4000
frames; runs the same day whose conditions were not recorded had uploads of up to 7.3 ms and up to 36 of 4000
software-driver frames over 8.33 ms (scroll-api-images). With vsync on, the render thread sleeps most of each refresh
and macOS runs it at a lower clock (ANGLE partly on efficiency cores), so a frame takes up to about twice the CPU time
of a loop that never waits.

`--driver`, `--scene`/`--load`, `--frames N --quit` and `--toggle-every K` script a run, `--csv FILE` writes every
frame's times, `--gpu-time on` adds GL timer queries (ANGLE), `--idle` only presents and `--keeps N` gives either driver
keeps for the rows of N screens (default 3, `off` = 0); `--help` lists the options and keys. With vsync off, macOS may
still hold a window to the display rate (not always), so frames shorter than that then wait for a drawable instead.

## ESP32-P4 / Tab5: sharing the CPU and PSRAM with your app

`examples/tab5` renders on core 0; core 1 is the application's. Both cores and the panel's scan-out share PSRAM, and
what slows the renderer is not the bytes the app moves but how often the app's CPU misses its caches into PSRAM.
Measured on Tab5 (ST7121 panel, 2026-10-04) with a probe task on core 1, renderer busy time per frame grew by:

| App CPU traffic to PSRAM (cache misses) | Renderer slower by |
|---|---|
| ~16 MB/s | ~9 % |
| ~42 MB/s | 23-28 % |
| ~83 MB/s | 63-77 % |
| unbounded (138-175 MB/s) | 3.5-4.5x |
| 107 MB/s moved by DMA2D, CPU idle | 4-7 % |

- Keep the app's CPU-side PSRAM traffic at about 25 MB/s or less to stay within ~15 % (interpolated between the 16
  and 42 MB/s points). Compute-only work costs the renderer nothing; even PSRAM reads that hit the cache cost 13-17 %.
- Move bulk PSRAM data by DMA (`esp_async_memcpy`, DMA2D): DMA bytes barely touch the renderer's time. DMA2D shares
  its channels with the PPA rotation, so it can lengthen frames (not busy time); 1-D GDMA avoids that.
- Preload the font pages you will use (`shr_pl_res_bitmap_font_preload()`, e.g. the hot pages fontpack reports for each
  package) before the first frame. A page first needed mid-scene is read from flash inside that frame, and the rows
  drawn with fallback glyphs meanwhile are drawn again and not kept. In the example's blink scene (emoji appearing
  after the first frame) preloading the emoji package's 9 hot pages cut the worst frame from ~51 to ~40 ms (23 ms
  with `SHR_DRIVER_CHEAP_STORE`), and the first frame of the other scenes from ~150 to ~50 ms.
- `make tab5-build` bakes the packages with zstd and builds the decoder in (`TAB5_ZSTD=OFF`: stored packages and no
  decoder; a firmware with it reads either, and the partitions hold either): 3.6 instead of 7.8 MB of flash (cjk-ko 3.0
  instead of 6.3 MB) for 47 KiB more firmware, and the decoder's 94 KiB context in PSRAM. Loading pages costs more CPU:
  on Tab5 (ILI9881C, 2026-10-06) the example's preload of 26 pages took 184 instead of 120 ms, a page first needed later
  ~1-1.5 ms more (cjk-mix's first frame, 40 pages: 201 instead of 157 ms); frames that load no page draw as fast.
- Run app tasks at priority 1 or higher: at priority 0 a task shares its core's ticks with the idle task and gets
  half the throughput (the renderer, on the other core, is unaffected either way).
- Cap the frame rate (`min_frame_interval_ns`, Kconfig `SHIROKO_TAB5_FPS_CAP` in the example, default 30): renderer
  PSRAM traffic falls in proportion to the frames it no longer draws, which helps only while the renderer is faster than
  the cap (most of the example's scenes draw in 13-39 ms a frame there, worst frames up to ~65 ms, so the cap limits
  most of them). With the ~42 MB/s app above, a screen of blinking text drew 31.6 frames per second and 117 MB/s of
  renderer PSRAM traffic; capped at 15 it drew 56 MB/s, and the app's PSRAM-to-internal-RAM copies went from 69 % to
  85 % of their speed beside an idle renderer.
- Size the keeps to the screens or looks the app switches among (Kconfig `SHIROKO_TAB5_KEEP_SCREENS` in the example,
  default 4: slots for the rows of that many screens, 45 x 40 KiB = 1.8 MiB of PSRAM each). On Tab5 (uncapped), 4
  fits rows cycling through four looks (restyle4: 180 stores over 60 frames, frame p50 ~28 ms; its worst frame,
  ~65 ms, stays); 3 frees 1.8 MiB and runs the example's other scenes as fast, but such rows are drawn instead of
  copied (restyle4: 600 stores, p50 47.6 ms); 2 frees 3.6 MiB (restyle4 p50 55.7 ms; a new screen every 20 frames
  ~2 ms faster at worst: fewer rows stored that are never drawn again). The desktop
  example's `--keeps N` and the replay bench's `SHR_REPLAY_KEEPS` set the same count.
- On a screen turned a quarter onto the panel, as in the example, scrolls move pixels only along the panel's rows, so
  the DMA2D moves them in place and needs no temporary frame (the example keeps none, 1.8 MiB of PSRAM less).

What a change costs the renderer per frame on Tab5 (ILI9881C panel, 2026-10-06, uncapped, two bands), from scenes on
a 160 x 45 grid of Latin text: the frame as the example measures it (p50 / MAX over 60 frames, with the app's own work
and the DMA2D and PPA waits), then each part replayed alone from a recording of the same frames: the Shiroko calls that
make the change (`set_cell`, `shr_pl_lyr_tilemap_scroll`, `shr_lyr_set_rect`), `shr_submit`, the pumps that build the
frame, and the driver's CPU time without the waits. In ms; the last column, in µs, replays the same recordings on a
MacBook Pro M4 Max (software driver, without the rotation). On the ST7121 board the scroll frames take ~1 ms longer
(the DMA2D wait) and the rest is the same within 0.7 ms.

| Change per frame | Frame p50 / MAX | Calls | Submit | Build | Driver busy p50 / MAX | M4 Max (µs) |
|---|---:|---:|---:|---:|---:|---:|
| One cell | 0.5 / 1.2 | <0.01 | 0.19 | 0.10 | 0.1 / 0.7 | 3 |
| One row (160 cells) | 1.4 / 1.5 | 0.16 | 0.20 | 0.33 | 0.5 / 0.6 | 12 |
| Every cell (7200) | 28.5 / 28.7 | 7.11 | 9.74 | 1.63 | 1.4 / 1.6 | 183 |
| Scroll one line up, write the new line | 12.8 / 13.7 | 0.24 | 0.14 | 0.32 | 0.5 / 0.6 | 44 |
| One row bold and italic, on and off | 0.6 / 0.8 | 0.13 | 0.19 | 0.07 | 0.1 / 2.8 | 6 |
| One 96 x 96 sprite moved | 1.9 / 2.1 | <0.01 | <0.01 | 0.21 | 1.5 / 1.7 | 7 |
| 1000 cells of Hangul text | 35.1 / 37.8 | 0.89 | 5.38 | 5.37 | 21.1 / 23.8 | 303 |

- Submit and build take 0.2-0.5 ms a frame however little changed.
- Scrolling with `shr_pl_lyr_tilemap_scroll` costs the CPU less than rewriting one row, since the moved rows are not
  drawn again, but the frame waits ~12 ms for the DMA2D to move them.
- Rewriting every cell is mostly the calls and submit (17 of 29 ms); the driver draws little because most rows come
  back from the copies it keeps. Hangul text is the opposite: the driver's glyph drawing takes most of the frame.
- A row restyled back and forth is drawn from the copies kept of both looks after the first two frames.
- At the default 30 FPS cap the renderer idles for the rest of each 33.3 ms frame, and its PSRAM traffic falls with
  it; changes that take longer (the Hangul text, the first frame of a full-screen rewrite) lower the frame rate
  instead.
- Reproduce: `make tab5-build TAB5_DEFS="-D TAB5_COST=1"` (`=0` to go back) and `make tab5-flash`, save a few minutes
  of the serial output (a round of the seven scenes takes ~1.5 min) as tab5.log, and `make replay-cost > desktop.log`;
  `uv run tools/bench/benchlog.py cost tab5.log desktop.log` prints the table.

The renderer and the app also share the internal RAM (on Tab5 the example has about 400 KiB of it free once its bands,
panel and driver are set up). Shiroko allocates while it warms up (the first frames of a screen, rows longer than any
before, font pages first used, the driver's caches growing) and then nothing per frame: `tests/unit/test_alloczero.c`
refuses every allocation after each scene of the example has run once, and the example built with
`TAB5_DEFS="-D TAB5_SOAK=1"` prints a `Z` line per scene with the allocations of its last 10 frames (a `FAIL` line if
any; the heap count needs `CONFIG_HEAP_USE_HOOKS=y`). What still allocates after warm-up: cells holding a cluster of
over 12 bytes (each new one, and `shr_pl_lyr_tilemap_set_text` even when unchanged) and glyph pages loaded later.

- Allocate what must be internal RAM (task stacks, DMA buffers and descriptors, Wi-Fi and LWIP buffers, anything
  used while the flash cache is off) before creating the renderer, or statically: once a scene has warmed up, the
  internal heap may have little left, and an internal-only request then fails.
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` is not a guarantee: a plain `malloc()` falls back to the reserved pool once
  the other internal RAM is gone, so a renderer warming up can use it too.
- Prefer static buffers and the app's own pools (`xTaskCreateStatic()`, fixed-size queues and message buffers) for
  what the app needs while it runs, so that it does not race the renderer for the heap.
- Give the renderer an allocator (`shr_context_desc.allocator`, and the same one to `shr_software_driver_create()`)
  that decides by `shr_alloc_kind`: `SHR_ALLOC_DESCRIPTOR` is small state, `SHR_ALLOC_PAYLOAD` large buffers (rows
  of commands, font pages, images, keeps) that can live in PSRAM, `SHR_ALLOC_DMA` memory a device reads. With
  `SHR_ALLOC_HOT` in the allocator's `flags`, the kind of memory used every frame also carries `SHR_ALLOC_HOT`
  (rows of commands, a band's command list, per-frame state of the compositor, tilemap and driver), the memory most
  worth the fast RAM; with `flags` 0 the allocator never sees it. The example's `place_alloc` puts hot memory in
  internal RAM while it fits in 64 KiB (Kconfig `SHIROKO_TAB5_INTERNAL_BUDGET_KB`) and the rest of it and all other
  memory in PSRAM: at the end of each scene about 360 KiB of internal RAM is free, and the least since boot is about
  330 KiB. 216 KiB leaves the app about 150 KiB less and draws the example's scenes about 4 % faster on Tab5
  (uncapped, ST7121, p50 summed over the scenes; the Hangul scenes 3-4 ms a frame).
- Draw with two bands (Kconfig `SHIROKO_TAB5_BANDS`, default 2: 2 x 40 KiB of internal RAM) so the CPU draws one
  while the PPA turns the other. One band leaves the app 40 KiB more, but on Tab5 (uncapped, ST7121, 64 KiB budget) the
  scenes took about 12 % longer (p50 summed) and a scrolling screen 34 ms a frame instead of 29. The replay bench's
  `SHR_REPLAY_BANDS` sets the same count.
- Turn the screen so that scrolling up moves rows toward the start of the frame buffer (Kconfig
  `SHIROKO_TAB5_ROTATION` in the example, default 90° counterclockwise, also the right way up on Tab5 with the keyboard
  side down): DMA2D moves rows that way in one copy at any distance, the other way only up to 48 px. On Tab5 (capped
  30, 2026-10-10) scrolling up 5 rows a frame took 1.4-1.5 ms less busy time than clockwise and scrolling down 5 rows
  1.5 ms more (ILI9881C); one row either way costs the same busy time. The replay bench's `SHR_REPLAY_ROTATION` sets
  the same turn.
- Watch the internal heap: `heap_caps_register_failed_alloc_callback()` reports any failed request with its size and
  caps, `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)` the lowest free since boot, and
  `heap_caps_monitor_local_minimum_free_size_start()` / `_stop()` the lowest over a stretch, such as one screen of
  your app. The example's `TAB5_SOAK=1` build prints them per scene (`H` lines).

## Using an installed Shiroko

```
cmake --install build/host --prefix /opt/shiroko                      # library, headers, fonts
cmake --install build/host --prefix /opt/shiroko --component fonts     # font packages only
```

The library, headers and CMake package go to `lib/`, `include/shiroko/` and `lib/cmake/shiroko/`; `LICENSE`,
`NOTICE` and `LICENSES/` to `share/shiroko/`; the font packages, with their NOTICE, LICENSES/ and inventory.json,
to `share/shiroko/fonts/` (component `fonts`). Ship that directory with the application and open packages from
it in `shr_pl_res_bitmap_font_desc.open`.

```cmake
find_package(shiroko 0.1 CONFIG REQUIRED)   # CMAKE_PREFIX_PATH=/opt/shiroko
target_link_libraries(app PRIVATE shiroko::shiroko)
```

The cell size and pixel format are fixed when Shiroko is built and reach consumers as compile definitions.
A project that adds Shiroko with `add_subdirectory` needs uv and the fetched inputs too; set
`SHIROKO_FONT_CACHE` and `SHIROKO_UCD_CACHE` to where `make fontpack-fetch FONT_CACHE=... UCD_CACHE=...` put them
if not in Shiroko's `.cache/`.
