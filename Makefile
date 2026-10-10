# Fonts are baked by the CMake build (target shiroko_fonts, into <build dir>/fonts) for CELL_WIDTH x CELL_HEIGHT
# from the sources in FONT_CACHE, which Make fetches first together with the uv tools environment.
# Run `make fontpack-fetch` once before running targets in parallel with -j.

.PHONY: test test-nozstd test-tab5 core-test render-test render-export headless-run bench replay replay-tab5 replay-cost \
        replay-dma \
        tools-env fontpack-fetch fontpack fontpack-locales \
        test-asan test-ubsan test-tsan sanitizer-image test-asan-linux test-lsan-linux test-msan-linux \
        test-tsan-linux test-ubsan-linux test-hwasan-linux test-rtsan-linux test-sanitizers \
        fuzz fuzz-msan desktop-run tab5-host tab5-build tab5-idf tab5-flash tab5-monitor coverage test-gcc gcc-image \
        check-inline check-inline-host check-inline-gcc check-inline-tab5 clean

COMMA := ,
NPROC := $(shell getconf _NPROCESSORS_ONLN)
FONT_CACHE ?= .cache/fonts
UCD_CACHE ?= .cache/ucd
UV := env -u VIRTUAL_ENV PYTHONDONTWRITEBYTECODE=1 SHIROKO_FONT_CACHE=$(abspath $(FONT_CACHE)) SHIROKO_UCD_CACHE=$(abspath $(UCD_CACHE)) \
    uv run --frozen --no-sync
FUZZ_TIME ?= 60
FUZZ_TARGETS := fuzz_tilemap fuzz_package fuzz_driver fuzz_compositor fuzz_rows
CELL_WIDTH ?= 8
CELL_HEIGHT ?= 16
PIXEL_FORMAT ?= RGB565
CELL := $(CELL_WIDTH)x$(CELL_HEIGHT)
# Also passed into the Linux containers, so no host paths here.
CMAKE_CELL := -DSHIROKO_CELL_WIDTH=$(CELL_WIDTH) -DSHIROKO_CELL_HEIGHT=$(CELL_HEIGHT) -DSHIROKO_PIXEL_FORMAT=$(PIXEL_FORMAT)
CMAKE_HOST := $(CMAKE_CELL) -DSHIROKO_FONT_CACHE=$(abspath $(FONT_CACHE)) -DSHIROKO_UCD_CACHE=$(abspath $(UCD_CACHE))
PRESET := $(if $(filter RGBX8888,$(PIXEL_FORMAT)),host-rgbx,host)

FETCHED := $(FONT_CACHE)/.fetched

# Synced on every run (cheap when up to date), so a restored .cache/ never meets a missing environment.
tools-env:
	env -u VIRTUAL_ENV uv sync --locked --group tools --quiet

$(FETCHED): fonts/fonts.lock.json fonts/text_profile.lock.json pyproject.toml uv.lock .python-version | tools-env
	$(UV) tools/fontpack/fontpack.py fetch
	$(UV) tools/unicode/gen_unicode_tables.py --fetch
	touch $@

fontpack-fetch:
	rm -f $(FETCHED)
	$(MAKE) $(FETCHED)

fontpack: $(FETCHED)
	cmake --preset $(PRESET) $(CMAKE_HOST)
	cmake --build --preset $(PRESET) --target shiroko_fonts

fontpack-locales: $(FETCHED)
	$(UV) tools/fontpack/fontpack.py build --cell $(CELL) --out build/fonts-locales \
	    latin cjk-ko cjk-ja cjk-zh-Hans cjk-zh-Hant cjk-zh-HK symbols emoji nerd

test: $(FETCHED)
	cmake --preset $(PRESET) $(CMAKE_HOST)
	cmake --build --preset $(PRESET)
	ctest --preset $(PRESET) -j$(NPROC)

# Without zstd: stored packages only.
test-nozstd: $(FETCHED)
	cmake -S . -B build/nozstd $(CMAKE_HOST) -DCMAKE_BUILD_TYPE=Debug -DSHIROKO_ZSTD=OFF -DSHIROKO_BUILD_EXAMPLES=OFF
	cmake --build build/nozstd
	cd build/nozstd && ctest --output-on-failure -j$(NPROC)

# At the Tab5's cell size (TAB5_CELL_WIDTH x TAB5_CELL_HEIGHT); the render goldens are 8x16 only and skip there.
test-tab5: $(FETCHED)
	cmake -S . -B build/host-tab5 $(CMAKE_TAB5) -DCMAKE_BUILD_TYPE=Debug
	cmake --build build/host-tab5
	cd build/host-tab5 && ctest --output-on-failure -j$(NPROC)

core-test: $(FETCHED)
	cmake --preset $(PRESET) $(CMAKE_HOST)
	cmake --build --preset $(PRESET) --target test_tilemap test_grapheme
	ctest --preset $(PRESET) -R '^test_(tilemap|grapheme)$$'

# Render comparison (tests/render): goldens and reftests; mismatches leave PNGs in build/render/failures.
# render-export writes every scene to build/render; RENDER_ARGS adds options (e.g. --frames 100, -t emoji).
render-test: $(FETCHED)
	cmake --preset $(PRESET) $(CMAKE_HOST)
	cmake --build --preset $(PRESET) --target test_render shiroko_fonts
	ctest --preset $(PRESET) -R '^test_render'

render-export: $(FETCHED)
	cmake --preset $(PRESET) $(CMAKE_HOST)
	cmake --build --preset $(PRESET) --target test_render shiroko_fonts
	./build/$(PRESET)/tests/test_render --export --out build/render $(RENDER_ARGS) || [ $$? -eq 77 ]

headless-run: $(FETCHED)
	cmake --preset $(PRESET) $(CMAKE_HOST)
	cmake --build --preset $(PRESET) --target shiroko_headless shiroko_fonts
	./build/$(PRESET)/examples/headless/shiroko_headless build/$(PRESET)/shiroko-headless.ppm build/$(PRESET)/fonts

ifeq ($(shell uname),Darwin)
LLVM_PROFDATA ?= xcrun llvm-profdata
LLVM_COV ?= xcrun llvm-cov
else
LLVM_PROFDATA ?= llvm-profdata
LLVM_COV ?= llvm-cov
endif
COV := build/coverage
COV_MIN ?= 100
# One build per configuration: $(1) build dir, $(2) CMake options, $(3) sources reported.
define cov_run
	cmake -S . -B $(1) $(CMAKE_HOST) -DCMAKE_BUILD_TYPE=Debug -DSHIROKO_COVERAGE=ON -DSHIROKO_BUILD_EXAMPLES=OFF $(2)
	cmake --build $(1)
	rm -rf $(1)/profiles && cd $(1) && LLVM_PROFILE_FILE=$(CURDIR)/$(1)/profiles/%p.profraw ctest -j$(NPROC)
	$(LLVM_PROFDATA) merge -o $(1)/all.profdata $(1)/profiles/*.profraw
	objs=$$(for f in $(1)/tests/test_* $(1)/tests/fuzz/fuzz_*; do printf -- '-object %s ' "$$f"; done) && \
	    $(LLVM_COV) report -instr-profile $(1)/all.profdata $(1)/tests/test_shared $$objs \
	    -ignore-filename-regex='shr_gen_|vcpkg_installed' $(3) | tee $(1)/report.txt
	awk '/^TOTAL/ { for (i = 4; i <= NF; i += 3) if ($$i + 0 < $(COV_MIN)) bad = 1 } END { exit bad }' $(1)/report.txt \
	    || { echo "coverage below $(COV_MIN)%"; exit 1; }
endef
# The font plugin also without zstd, where decode.c is stored-only.
coverage: $(FETCHED)
	$(call cov_run,$(COV),,cores ports/software)
	$(call cov_run,$(COV)-nozstd,-DSHIROKO_ZSTD=OFF,cores/pl_res_bitmap_font)

bench: $(FETCHED)
	cmake --preset release $(CMAKE_HOST)
	cmake --build --preset release --target shiroko_bench shiroko_fonts
	./build/release/tests/shiroko_bench build/release/fonts $(BENCH)

# Replay (tests/bench/replay.c): the Tab5 example's scenes recorded on its screen configuration, then replayed into the
# software driver alone and into the compositor over a driver drawing nothing; REPLAY_ARGS = [SCENE[,SCENE...]|all|cost]
# [FRAMES]. replay-tab5 takes the Tab5's packages and cell size (built into build/tab5-replay), so its recording hashes
# match the device's (TAB5_DEFS="-D TAB5_REPLAY=1"; for a firmware with other keep screens, bands or rotation, the same
# SHR_REPLAY_KEEPS, SHR_REPLAY_BANDS and SHR_REPLAY_ROTATION).
# replay-cost: the app cost table's scenes (README; on the Tab5: TAB5_DEFS="-D TAB5_COST=1"), as
# tools/bench/benchlog.py cost TAB5_LOG [DESKTOP_LOG] tabulates them.
replay: $(FETCHED)
	cmake --preset release $(CMAKE_HOST)
	cmake --build --preset release --target shiroko_replay shiroko_fonts
	./build/release/tests/shiroko_replay build/release/fonts $(REPLAY_ARGS)

replay-tab5: tab5-host
	cmake -S . -B build/tab5-replay $(CMAKE_TAB5) -DCMAKE_BUILD_TYPE=Release
	cmake --build build/tab5-replay --target shiroko_replay
	./build/tab5-replay/tests/shiroko_replay $(TAB5_HOST)/fonts $(REPLAY_ARGS)

replay-cost:
	$(MAKE) replay-tab5 REPLAY_ARGS=cost

# replay-dma: every scene replayed with the Tab5 example's keep copier (SHR_REPLAY_COPIER=tab5) for each cell size in
# DMA_CELLS (built-in glyphs; build/replay-dma-WxH); fails when a keep copy the DMA2D could take, or a band rotation,
# would go to the CPU on the Tab5.
DMA_CELLS ?= 8x16 10x20 12x24 16x32
replay-dma: $(FETCHED)
	@r=0; for c in $(DMA_CELLS); do b=build/replay-dma-$$c; \
	  cmake -S . -B $$b -DCMAKE_BUILD_TYPE=Release -DSHIROKO_BUILD_EXAMPLES=OFF -DSHIROKO_PIXEL_FORMAT=RGB565 \
	    -DSHIROKO_CELL_WIDTH=$${c%x*} -DSHIROKO_CELL_HEIGHT=$${c#*x} -DSHIROKO_FONT_CACHE=$(abspath $(FONT_CACHE)) \
	    -DSHIROKO_UCD_CACHE=$(abspath $(UCD_CACHE)) >$$b.log 2>&1 && cmake --build $$b --target shiroko_replay -j$(NPROC) >>$$b.log 2>&1 \
	    || { echo "replay-dma $$c: build failed, $$b.log"; r=1; continue; }; \
	  SHR_REPLAY_COPIER=tab5 SHR_REPLAY_PART=driver-sw SHR_REPLAY_REPS=1 $$b/tests/shiroko_replay $$b/no-fonts all >>$$b.log 2>&1 \
	    || r=1; grep -E '^  DP |^DMA-PATHS' $$b.log | tail -18; \
	done; exit $$r

# Apple Clang on purpose: on macOS 26+ Homebrew LLVM's ASan runtime deadlocks
# in __asan_init. LSan/MSan/HWASan/RTSan run in the Linux container.
SAN_ENV := UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 TSAN_OPTIONS=halt_on_error=1

define host_san
	cmake -S . -B $(1) $(CMAKE_HOST) -DCMAKE_BUILD_TYPE=Debug -DSHIROKO_SANITIZE=$(2) -DSHIROKO_BUILD_EXAMPLES=OFF
	cmake --build $(1)
	cd $(1) && $(3) ctest --output-on-failure -j$(NPROC)
endef

test-asan: $(FETCHED)
	$(call host_san,build/host-asan,address$(COMMA)undefined,$(SAN_ENV))
test-ubsan: $(FETCHED)
	$(call host_san,build/host-ubsan,undefined,$(SAN_ENV))
test-tsan: $(FETCHED)
	$(call host_san,build/host-tsan,thread,$(SAN_ENV))

# The images build natively for the host architecture (gcc-image is always linux/amd64). HWASan needs
# aarch64, so test-hwasan-linux runs only on an arm64 host (Apple silicon, Linux arm64); test-sanitizers skips it
# elsewhere.
# Containers run as the calling user so the mounted tree gets no root-owned files.
DOCKER_RUN := docker run --rm -v "$(CURDIR)":/src -w /src --user $$(id -u):$$(id -g) \
    -e HOME=/tmp/home -e UV_CACHE_DIR=/tmp/uv-cache
SAN_IMAGE := shiroko-sanitizers
# LSan needs ptrace; unconfined seccomp keeps Docker from blocking it.
SAN_DOCKER := $(DOCKER_RUN) --cap-add SYS_PTRACE --security-opt seccomp=unconfined \
    -e ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
    -e UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    -e MSAN_OPTIONS=halt_on_error=1 \
    -e TSAN_OPTIONS=halt_on_error=1 \
    -e HWASAN_OPTIONS=abort_on_error=1 \
    -e RTSAN_OPTIONS=halt_on_error=1 \
    $(SAN_IMAGE)

sanitizer-image:
	docker build -t $(SAN_IMAGE) --build-context tools=. -f docker/sanitizers/Dockerfile docker/sanitizers

# MSan builds take their vcpkg libraries (zstd) instrumented too.
san_triplets = $(if $(filter memory,$(1)),-DVCPKG_OVERLAY_TRIPLETS=/src/cmake/triplets-msan)

define linux_san
	$(SAN_DOCKER) bash -c 'set -e; cmake -S . -B build/linux-$(1) $(CMAKE_CELL) -DCMAKE_BUILD_TYPE=Debug \
	    -DSHIROKO_SANITIZE=$(2) $(call san_triplets,$(2)) -DSHIROKO_BUILD_EXAMPLES=OFF >/dev/null && \
	    cmake --build build/linux-$(1) -j4 && cd build/linux-$(1) && ctest --output-on-failure -j$$(nproc)'
	@printf "  ✓ %s\n" "$(1) [linux]"
endef

test-asan-linux: $(FETCHED)
	$(call linux_san,asan,address$(COMMA)leak$(COMMA)undefined)
test-lsan-linux: $(FETCHED)
	$(call linux_san,lsan,leak)
test-msan-linux: $(FETCHED)
	$(call linux_san,msan,memory)
test-tsan-linux: $(FETCHED)
	$(call linux_san,tsan,thread)
test-ubsan-linux: $(FETCHED)
	$(call linux_san,ubsan,undefined)
test-hwasan-linux: $(FETCHED)
	$(call linux_san,hwasan,hwaddress)
test-rtsan-linux: $(FETCHED)
	$(call linux_san,rtsan,realtime)

HWASAN := $(if $(filter arm64 aarch64,$(shell uname -m)),test-hwasan-linux)
test-sanitizers: test-asan test-ubsan test-tsan test-asan-linux test-lsan-linux test-msan-linux test-tsan-linux \
                 test-ubsan-linux $(HWASAN) test-rtsan-linux

# Inputs: seeds written by the build (target fuzz_seeds) plus the regressions in tests/fuzz/corpus/<target>.
define fuzz_run
	$(SAN_DOCKER) bash -c 'set -e; b=build/linux-fuzz-$(1); cmake -S . -B $$b $(CMAKE_CELL) -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	    -DSHIROKO_FUZZ=ON -DSHIROKO_BUILD_TESTS=OFF -DSHIROKO_BUILD_EXAMPLES=OFF -DSHIROKO_SANITIZE=$(2) \
	    $(call san_triplets,$(2)) >/dev/null; \
	    cmake --build $$b -j4 --target $(FUZZ_TARGETS) fuzz_seeds; mkdir -p build/fuzz-logs; rm -f build/fuzz-logs/*-$(1).log; pids=; \
	    for t in $(FUZZ_TARGETS); do \
	      mkdir -p build/fuzz-corpus/$$t build/fuzz-artifacts/$$t $$b/tests/fuzz/seeds/$$t; \
	      $$b/tests/fuzz/$$t -max_total_time=$(FUZZ_TIME) -max_len=65536 -print_final_stats=1 \
	        -artifact_prefix=build/fuzz-artifacts/$$t/ build/fuzz-corpus/$$t $$b/tests/fuzz/seeds/$$t $$(ls -d tests/fuzz/corpus/$$t 2>/dev/null) \
	        >build/fuzz-logs/$$t-$(1).log 2>&1 & pids="$$pids $$!"; \
	    done; fail=0; for p in $$pids; do wait $$p || fail=1; done; \
	    for t in $(FUZZ_TARGETS); do echo "== $$t ($(1), $(FUZZ_TIME)s)"; \
	      grep -E "^(stat::(number_of_executed_units|new_units_added)|==[0-9]+==|SUMMARY|.*invariant failed)" \
	        build/fuzz-logs/$$t-$(1).log || true; done; exit $$fail'
endef

fuzz: $(FETCHED)
	$(call fuzz_run,asan,address$(COMMA)undefined)
fuzz-msan: $(FETCHED)
	$(call fuzz_run,msan,memory)

# SDL3 window with the software and ANGLE drivers; DESKTOP_ARGS adds options (e.g. --load scroll --frames 300 --quit).
# RGBX8888 unless PIXEL_FORMAT is given: SDL's Metal renderer has no RGB565 texture and converts it every frame.
DESKTOP_PIXEL_FORMAT := $(if $(filter file,$(origin PIXEL_FORMAT)),RGBX8888,$(PIXEL_FORMAT))
desktop-run: $(FETCHED)
	cmake --preset desktop $(filter-out -DSHIROKO_PIXEL_FORMAT=%,$(CMAKE_HOST)) -DSHIROKO_PIXEL_FORMAT=$(DESKTOP_PIXEL_FORMAT)
	cmake --build --preset desktop --target shiroko_desktop shiroko_fonts
	./build/desktop/examples/desktop/shiroko_desktop $(DESKTOP_ARGS)

# M5Stack Tab5 (ESP32-P4) example: the host generates the sources and the packages into TAB5_HOST (64x512 pages, which
# draw CJK text faster on the P4) and, with TAB5_ZSTD=ON (default), bakes them with zstd and installs zstd's decoder
# sources there through vcpkg (cmake/ports/zstd-source); then ESP-IDF builds examples/tab5 in its stock image into
# build/tab5, compiling those sources with its own toolchain. TAB5_ZSTD=OFF: stored packages (they fit the partitions
# at 8x16 only), no decoder. The container cannot see USB devices, so flashing and the monitor run on the host (pinned
# esptool / esp-idf-monitor through uvx);
# PORT=/dev/cu.usbmodem... picks the board.
# IDF_ARGS runs any idf.py command on the same build (e.g. make tab5-idf IDF_ARGS=size-components); TAB5_DEFS passes
# the example's build options (e.g. TAB5_DEFS="-D TAB5_REPLAY=1 -D TAB5_LOADS=0xff"; CMake keeps them until changed).
# TAB5_BOARD=headless builds for an ESP32-P4 board without a panel into build/tab5-headless (sdkconfig.defaults.headless).
# The build folder's sdkconfig is made anew when the defaults or the Kconfig change (menuconfig edits are lost).
TAB5_HOST := build/tab5-host
# Tab5's own cell size, whatever CELL is.
TAB5_CELL_WIDTH ?= 12
TAB5_CELL_HEIGHT ?= 24
TAB5_CELL := $(TAB5_CELL_WIDTH)x$(TAB5_CELL_HEIGHT)
CMAKE_TAB5 := $(filter-out -DSHIROKO_CELL_%,$(CMAKE_HOST)) -DSHIROKO_CELL_WIDTH=$(TAB5_CELL_WIDTH) \
    -DSHIROKO_CELL_HEIGHT=$(TAB5_CELL_HEIGHT)
TAB5_ZSTD ?= ON
IDF_IMAGE ?= espressif/idf:v6.1
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*) $(wildcard /dev/ttyACM*))
TAB5_BOARD ?= tab5
TAB5_BUILD := build/tab5$(if $(filter headless,$(TAB5_BOARD)),-headless)
TAB5_IDF := $(DOCKER_RUN) -w /src/examples/tab5 -e IDF_TARGET=esp32p4 $(IDF_IMAGE) \
    idf.py -B /src/$(TAB5_BUILD) -D SDKCONFIG=/src/$(TAB5_BUILD)/sdkconfig \
    $(if $(filter headless,$(TAB5_BOARD)),-D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.headless') \
    -D SHIROKO_CELL_WIDTH=$(TAB5_CELL_WIDTH) -D SHIROKO_CELL_HEIGHT=$(TAB5_CELL_HEIGHT) -D SHIROKO_PIXEL_FORMAT=RGB565 \
    -D SHIROKO_TAB5_ZSTD=$(TAB5_ZSTD) $(TAB5_DEFS)
TAB5_DEFAULTS := examples/tab5/sdkconfig.defaults $(if $(filter headless,$(TAB5_BOARD)),examples/tab5/sdkconfig.defaults.headless) \
    examples/tab5/main/Kconfig.projbuild

$(TAB5_BUILD)/sdkconfig: $(TAB5_DEFAULTS)
	rm -f $@

tab5-host: $(FETCHED)
	cmake -S . -B $(TAB5_HOST) $(CMAKE_TAB5) -DCMAKE_BUILD_TYPE=Release -DSHIROKO_ZSTD=OFF \
	    -DSHIROKO_BUILD_TESTS=OFF -DSHIROKO_BUILD_EXAMPLES=OFF \
	    $(if $(filter ON,$(TAB5_ZSTD)),-DVCPKG_OVERLAY_PORTS=$(CURDIR)/cmake/ports -DVCPKG_MANIFEST_FEATURES=zstd-source)
	cmake --build $(TAB5_HOST) --target shiroko_unicode_tables shiroko_pl_res_bitmap_font
	$(UV) tools/fontpack/fontpack.py build --cell $(TAB5_CELL) --out $(TAB5_HOST)/fonts --method $(if $(filter ON,$(TAB5_ZSTD)),zstd,stored) \
	    --page-atlas 64x512 latin cjk-ko symbols emoji nerd

tab5-build: tab5-host $(TAB5_BUILD)/sdkconfig
	$(TAB5_IDF) build

tab5-idf: tab5-host $(TAB5_BUILD)/sdkconfig
	$(TAB5_IDF) $(IDF_ARGS)

tab5-flash: tab5-build
	@test -n "$(PORT)" || { echo "no serial port found; pass PORT=/dev/cu.usbmodemXXXX"; exit 1; }
	cd $(TAB5_BUILD) && uvx esptool==5.3.1 --chip esp32p4 --port $(PORT) --baud 921600 write-flash @flash_args

tab5-monitor:
	@test -n "$(PORT)" || { echo "no serial port found; pass PORT=/dev/cu.usbmodemXXXX"; exit 1; }
	uvx --from esp-idf-monitor==1.9.0 idf-monitor --port $(PORT) --target esp32p4 $(TAB5_BUILD)/shiroko_tab5.elf

gcc-image:
	docker build --platform linux/amd64 -t shiroko-gcc --build-context tools=. docker/gcc

GCC_DOCKER := $(DOCKER_RUN) --platform linux/amd64 shiroko-gcc

test-gcc: $(FETCHED)
	$(GCC_DOCKER) bash -c 'set -e; for cfg in "O0 Debug" "O2 Release" "san Debug -DSHIROKO_SANITIZE=address,undefined"; do \
	    set -- $$cfg; d=build/linux-gcc-$$1; cmake -S . -B $$d -G Ninja $(CMAKE_CELL) -DCMAKE_BUILD_TYPE=$$2 $$3 -DSHIROKO_BUILD_EXAMPLES=OFF >/dev/null && \
	    cmake --build $$d && (cd $$d && ctest --output-on-failure -j$$(nproc)); done'
	@printf "  ✓ gcc x86_64 [O0, O2, asan+ubsan]\n"

# The hot functions call only what tools/inline/<toolchain>.txt allows: a helper that stops being inlined fails here
# instead of in a benchmark. INLINE_ARGS=--record rewrites the lists from the build (review the diff).
INLINE_PARTS := shiroko_pl_lyr_tilemap shiroko_compositor shiroko_port_software
check-inline: check-inline-host check-inline-gcc check-inline-tab5

check-inline-host: $(FETCHED)
	cmake --preset release $(CMAKE_HOST)
	cmake --build --preset release --target $(INLINE_PARTS)
	$(UV) tools/inline/check_inline.py $(INLINE_ARGS) tools/inline/clang.txt build/release

check-inline-gcc: $(FETCHED)
	$(GCC_DOCKER) bash -c 'set -e; d=build/linux-gcc-O2; cmake -S . -B $$d -G Ninja $(CMAKE_CELL) -DCMAKE_BUILD_TYPE=Release -DSHIROKO_BUILD_EXAMPLES=OFF >/dev/null && \
	    cmake --build $$d --target $(INLINE_PARTS) && python3 tools/inline/check_inline.py $(INLINE_ARGS) tools/inline/gcc.txt $$d'

check-inline-tab5: tab5-build
	$(DOCKER_RUN) $(IDF_IMAGE) python3 tools/inline/check_inline.py --objdump riscv32-esp-elf-objdump $(INLINE_ARGS) \
	    tools/inline/riscv.txt $(TAB5_BUILD)

clean:
	rm -rf build
