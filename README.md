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
- For `make vt-run` only: Zig 0.16.0 (`brew install zig`) and the `third_party/ghostty` submodule

On macOS: `brew install cmake uv zig`.

## Fonts and the build

Every Make build and test target syncs the uv tools environment (`uv sync --locked --group tools`, cheap when up to date);
`make fontpack-fetch` (run by the first `make test`) downloads the pinned font and Unicode inputs into
`.cache/fonts` and `.cache/ucd` (`make FONT_CACHE=... UCD_CACHE=...` to put them elsewhere). The CMake build never
downloads and never syncs: it runs the tools with `uv run --frozen --no-sync --offline` and fails at configure
time, asking for `make fontpack-fetch`, when the inputs or the environment are missing.

The build generates the Unicode tables (`<build dir>/generated/shr_gen_unicode_tables.c`) from `.cache/ucd`, and
`test_grapheme` reads `GraphemeBreakTest.txt` from there.

The build bakes the fonts for one cell size, `SHIROKO_CELL_WIDTH` x `SHIROKO_CELL_HEIGHT` (default 8x16; width
1..64, height 1..127; `make CELL_WIDTH=10 CELL_HEIGHT=20 ...`). The built-in package (ASCII + U+FFFD) is compiled into the
library; the other packages go to `<build dir>/fonts` with their NOTICE, LICENSES/ and inventory.json (target
`shiroko_fonts`, part of the default build when Shiroko is the top-level project).

The default packages are `latin`, `cjk-ko`, `symbols`, `emoji` and `nerd`, so the only CJK glyph variant is
Korean. For the `ja`, `zh-Hans`, `zh-Hant` and `zh-HK` variants (`shr_pl_res_bitmap_font_desc.locale`),
`make fontpack-locales` bakes every package, including `cjk-ja`, `cjk-zh-Hans`, `cjk-zh-Hant` and `cjk-zh-HK`,
into `build/fonts-locales`.

The screen pixel format is `SHIROKO_PIXEL_FORMAT`: `RGB565` (default) or `RGBX8888` (preset `host-rgbx`,
`make PIXEL_FORMAT=RGBX8888 test`).

The render comparison test (`tests/render`, `test_render`) draws a catalog of scenes offscreen through the public
API: its golden list (`tests/render/golden.txt`, XXH3-128 per scene, cell size and pixel format) holds 8x16 entries
only, so other cell sizes skip it (exit code 77) after running the reftests (scene pairs that must draw identical
pixels). `SHR_UPDATE_GOLDEN=1` rewrites this build's entries. Mismatches leave expected/actual/diff PNGs in
`build/render/failures`. `test_render --frames N` times N full redraws per scene after the first frame; with
`SHIROKO_PORT_ANGLE`, `--backend angle` (ctest `test_render_angle`) compares the GPU driver against the software
images within per-scene fuzzy limits.

## Common targets

```
make test          # unit/golden tests, fontpack selftest, headless example, fuzz corpus replay
make core-test     # text measurement tests only
make fontpack      # font packages into build/host/fonts
make fontpack-locales  # all packages, every CJK locale, into build/fonts-locales
make render-test   # render comparison suite (goldens, reftests)
make render-export # every scene as PNG into build/render (RENDER_ARGS='--frames 100' times them instead)
make headless-run  # render examples/headless into a PPM image
make vt-run        # libghostty-vt consumer example
make desktop-run   # SDL3 window, software and ANGLE drivers (DESKTOP_ARGS='--load scroll --frames 300 --quit')
make bench         # per-part throughput (BENCH=software|compositor|tilemap|font|image|terminal filters;
                   #   SHR_BENCH_SECONDS=5 lengthens one case for a profiler)
make test-asan test-ubsan test-tsan   # host sanitizers (Apple Clang or the host compiler)
make test-sanitizers fuzz fuzz-msan   # adds the Linux container sanitizers; HWASan needs an arm64 host
make test-gcc      # x86_64 GCC 13 at -O0, -O2 and ASan+UBSan
make coverage      # clang source-based coverage; fails below COV_MIN (default 100); LLVM_PROFDATA, LLVM_COV
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
- `shr_pl_lyr_tilemap_resize()` takes an optional background colour for cells without `SHR_STYLE_BG`;
  `shr_pl_lyr_tilemap_measure()` lays text out into a caller array of `shr_text_cluster`.

## OpenGL ES driver (ANGLE)

`-DSHIROKO_PORT_ANGLE=ON` (preset `angle`, vcpkg feature `angle`: ANGLE with Metal on macOS) builds the static library
`shiroko_angle` (`shiroko::shiroko_angle`, header `shiroko/port_angle.h`): a synchronous `shr_framebuffer_driver` on
OpenGL ES 3.0 for CPU memory and GPU surfaces (`shr_angle_surface_create()`, `SHR_MEMORY_DEVICE` surfaces whose `pixels`
is a driver handle, so they can also be COPY/ROTATE sources such as a composition;
`shr_angle_surface_texture(driver, surface, &texture)` gives the GL texture, top row first). Every call needs the EGL
context the driver was created with current on the calling thread and returns `SHR_E_STATE` otherwise.
Registered buffers become layers of GL array textures (A4 kept packed, the shader picks the nibble), and a batch draws
as instanced quads with their clip, so a full screen of text is one or a few draws.
`shr_angle_offscreen_create()` makes an EGL pbuffer context for tests and tools;
`SHIROKO_ANGLE_BACKEND` picks its backend (`metal`, `opengl`, `vulkan`, `d3d11`, `default`; unset: `metal` on macOS,
`opengl` elsewhere) and the others are tried when it fails. FILL, COPY, ROTATE and every RGBX8888 command match the
software port exactly. RGB565 blends (GLYPH, IMAGE, DIM) round once from the 5/6-bit destination in both drivers, as
GPUs blend; a GPU that blends 16-bit targets at reduced precision (Metal) still leaves under 1 % of the blended pixels
one level off.
`test_angle` compares both drivers and skips (exit code 77) without an EGL display. The library is installed but not
exported in `shirokoConfig.cmake`.

## Desktop example (SDL3)

`examples/desktop` (`-DSHIROKO_EXAMPLE_DESKTOP=ON`, vcpkg feature `desktop`: SDL3; preset `desktop`, RelWithDebInfo with
`SHIROKO_PORT_ANGLE`) shows the `tests/render` scenes and full-window load modes (`scroll`, `churn`, `restyle`, `blink`,
`images`) in a window. Tab switches between the software driver (an SDL_Renderer streaming texture) and the ANGLE
driver (SDL's OpenGL ES 3.0 context; the composition is a GPU surface blitted to the window). SDL loads ANGLE itself,
so the preset uses the overlay triplet in `cmake/triplets` (arm64-osx for now: static, except ANGLE as a shared
library) and the executable finds it through its rpath; with a static ANGLE the example is software only.
`SHIROKO_ANGLE_BACKEND` picks the backend as for `shr_angle_offscreen_create()`.

Each frame reports its render time (the scene's changes, submit and pump until presented, plus `glFinish` with ANGLE)
and its present time (texture upload or blit, and the swap) separately: the first frame alone, and p50/p95/p99/max over
the last 100 and 1000 frames, in the window title and on stdout. `--driver`, `--scene`/`--load`, `--frames N --quit`
and `--toggle-every K` script a run; `--help` lists the options and keys. With vsync off, macOS still holds a window
to the display rate, so frames shorter than that wait in present instead.

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
