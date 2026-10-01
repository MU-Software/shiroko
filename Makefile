# Fonts are baked by the CMake build (target shiroko_fonts, into <build dir>/fonts) for CELL_WIDTH x CELL_HEIGHT
# from the sources in FONT_CACHE, which Make fetches first together with the uv tools environment.
# Run `make fontpack-fetch` once before running targets in parallel with -j.

.PHONY: test core-test render-test render-export headless-run bench tools-env fontpack-fetch fontpack fontpack-locales \
        test-asan test-ubsan test-tsan sanitizer-image test-asan-linux test-lsan-linux test-msan-linux \
        test-tsan-linux test-ubsan-linux test-hwasan-linux test-rtsan-linux test-sanitizers \
        fuzz fuzz-msan vt-run coverage test-gcc gcc-image clean

COMMA := ,
NPROC := $(shell getconf _NPROCESSORS_ONLN)
FONT_CACHE ?= .cache/fonts
UCD_CACHE ?= .cache/ucd
UV := env -u VIRTUAL_ENV PYTHONDONTWRITEBYTECODE=1 SHIROKO_FONT_CACHE=$(abspath $(FONT_CACHE)) SHIROKO_UCD_CACHE=$(abspath $(UCD_CACHE)) \
    uv run --frozen --no-sync
FUZZ_TIME ?= 60
FUZZ_TARGETS := fuzz_tilemap fuzz_package fuzz_driver fuzz_compositor
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
coverage: $(FETCHED)
	cmake -S . -B $(COV) $(CMAKE_HOST) -DCMAKE_BUILD_TYPE=Debug -DSHIROKO_COVERAGE=ON -DSHIROKO_BUILD_EXAMPLES=OFF
	cmake --build $(COV)
	rm -rf $(COV)/profiles && cd $(COV) && LLVM_PROFILE_FILE=$(CURDIR)/$(COV)/profiles/%p.profraw ctest -j$(NPROC)
	$(LLVM_PROFDATA) merge -o $(COV)/all.profdata $(COV)/profiles/*.profraw
	objs=$$(for f in $(COV)/tests/test_* $(COV)/tests/fuzz/fuzz_*; do printf -- '-object %s ' "$$f"; done) && \
	    $(LLVM_COV) report -instr-profile $(COV)/all.profdata $(COV)/tests/test_shared $$objs \
	    -ignore-filename-regex='shr_gen_|vcpkg_installed' cores ports/software | tee $(COV)/report.txt
	awk '/^TOTAL/ { for (i = 4; i <= NF; i += 3) if ($$i + 0 < $(COV_MIN)) bad = 1 } END { exit bad }' $(COV)/report.txt \
	    || { echo "coverage below $(COV_MIN)%"; exit 1; }

bench: $(FETCHED)
	cmake --preset release $(CMAKE_HOST)
	cmake --build --preset release --target shiroko_bench shiroko_fonts
	./build/release/tests/shiroko_bench build/release/fonts $(BENCH)

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

define linux_san
	$(SAN_DOCKER) bash -c 'set -e; cmake -S . -B build/linux-$(1) $(CMAKE_CELL) -DCMAKE_BUILD_TYPE=Debug \
	    -DSHIROKO_SANITIZE=$(2) -DSHIROKO_BUILD_EXAMPLES=OFF >/dev/null && \
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
	    -DSHIROKO_FUZZ=ON -DSHIROKO_BUILD_TESTS=OFF -DSHIROKO_BUILD_EXAMPLES=OFF -DSHIROKO_SANITIZE=$(2) >/dev/null && \
	    cmake --build $$b -j4 --target $(FUZZ_TARGETS) fuzz_seeds && mkdir -p build/fuzz-logs && pids= && \
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

vt-run: $(FETCHED)
	git submodule update --init --depth 1 third_party/ghostty
	cmake -S examples/vt -B build/vt-example $(CMAKE_HOST) -DCMAKE_BUILD_TYPE=Release
	cmake --build build/vt-example --target shiroko_vt shiroko_fonts
	./build/vt-example/shiroko_vt build/vt-example/shiroko-vt build/vt-example/fonts examples/vt/fixture.ans

gcc-image:
	docker build --platform linux/amd64 -t shiroko-gcc --build-context tools=. docker/gcc

GCC_DOCKER := $(DOCKER_RUN) --platform linux/amd64 shiroko-gcc

test-gcc: $(FETCHED)
	$(GCC_DOCKER) bash -c 'set -e; for cfg in "O0 Debug" "O2 Release" "san Debug -DSHIROKO_SANITIZE=address,undefined"; do \
	    set -- $$cfg; d=build/linux-gcc-$$1; cmake -S . -B $$d -G Ninja $(CMAKE_CELL) -DCMAKE_BUILD_TYPE=$$2 $$3 -DSHIROKO_BUILD_EXAMPLES=OFF >/dev/null && \
	    cmake --build $$d && (cd $$d && ctest --output-on-failure -j$$(nproc)); done'
	@printf "  ✓ gcc x86_64 [O0, O2, asan+ubsan]\n"

clean:
	rm -rf build
