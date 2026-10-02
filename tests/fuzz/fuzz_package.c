/* Font packages with arbitrary bytes, opened by the font plugin as memory-mapped sources (in place) and
 * through read() (header, table, index and pages read and cached), then read again next to a second package
 * whose reads always fail (two packages cool down). Byte 0 selects the package role (bits 0-2; 5-7 also give a
 * driver that wants 16-byte strides, so pages are copied into slots), asynchronous
 * reads completed later (0x08), bold italic text (0x10), a page cache of about 2.5 seed pages (0x20; the frames draw
 * other text in between, so pages are evicted and read again), every fourth read failing (0x40)
 * and (0x80) with 0x08 reads that never complete (the watchdog cancels them), else reads returning different
 * bytes each time. Byte 1: RESEAL recomputes the page, index entry, index and header checksums of the package
 * before it is served (so mutations reach the decoders and record checks), MISALIGN maps it at an odd address
 * (else 256-aligned, so stored pages can be drawn in place). Invariants: no out-of-bounds access, every frame
 * rasters (bad packages or pages fall back), both paths draw the same pixels while reads are faithful, each
 * opened source is closed. */
#include "fuzz_common.h"
#include "shr_hash.h"

static const char *const roles[5] = {"shiroko-latin.shrf", "shiroko-cjk-ko.shrf", "shiroko-symbols.shrf",
                                     "shiroko-emoji.shrf", "shiroko-nerd.shrf"};
static const char *const samples[3] = {
    "Aa0\xEA\xB0\x80\xE1\x84\x80\xE1\x85\xA1", /* latin, Hangul, conjoining jamo */
    "\xF0\x9F\x98\x80\xE2\x9D\xA4\xEF\xB8\x8F\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB",
    "\xEE\x82\xB0\xE2\x96\x88\xE6\xBC\xA2\x65\xCC\x81\xEF\xBF\xBD"};
/* The second frame: glyphs on other pages of the seed packages (tests/fuzz/CMakeLists.txt). */
static const char *const others[3] = {
    "\xC3\xBF\xC3\xBE\xC3\xBD\xC3\xA6\xC3\x86\xC2\xA9\xEE\x82\xB3",       /* U+00FF..00A9, U+E0B3 */
    "\xEA\xB0\xAF\xEA\xB0\xA7\xEA\xB0\x9F\xEA\xB0\x97",                     /* U+AC2F, AC27, AC1F, AC17 */
    "\xF0\x9F\x98\x9F\xF0\x9F\x98\x98\xF0\x9F\x98\x9A\xF0\x9F\x98\x94"}; /* U+1F61F, 1F618, 1F61A, 1F614 */

enum { ASYNC = 0x08, BOLD_ITALIC = 0x10, TINY_CACHE = 0x20, FAIL_FIRST = 0x40, ODD = 0x80 };
enum { RESEAL = 0x01, MISALIGN = 0x02 };

/* The smallest page side >= n, as the seed shape picks it. */
static uint32_t side(uint32_t n) {
    uint32_t s = 64;
    while (s < n) s *= 2;
    return s;
}

/* About 2.5 A4 pages of the seed shape, each charging its atlas and up to ~0.35 atlases of records. */
static uint64_t tiny_cache(void) { return 3ull * side(2 * SHR_CELL_WIDTH) * side(SHR_CELL_HEIGHT) / 2; }

typedef struct pending_read {
    uint64_t request, offset;
    uint32_t length;
    void *dst;
} pending_read;

typedef struct pkg_source {
    const uint8_t *data;
    size_t size;
    const char *name;
    bool mapped, failing;
    uint8_t mode;
    int opens, closes, reads, npending;
    pending_read pending[16];
    struct pkg_source *also; /* a second package, or NULL */
} pkg_source;

/* Read number `n`; with ODD (synchronous) one byte differs on every read. */
static shr_status copy(const pkg_source *s, uint64_t offset, uint32_t length, void *dst, int n) {
    if (offset > s->size || s->size - offset < length) return SHR_E_IO;
    memcpy(dst, s->data + offset, length);
    if ((s->mode & (ASYNC | ODD)) == ODD && length) ((uint8_t *)dst)[(uint32_t)n % length] ^= (uint8_t)(n | 1);
    return SHR_OK;
}

static shr_status src_read(void *user, uint64_t offset, uint32_t length, void *dst, uint64_t request) {
    pkg_source *s = user;
    int n = s->reads++;
    if (s->failing || ((s->mode & FAIL_FIRST) && n % 4 == 0)) return SHR_E_IO;
    if (!(s->mode & ASYNC)) return copy(s, offset, length, dst, n);
    FUZZ_CHECK(s->npending < 16);
    s->pending[s->npending++] = (pending_read){request, offset, length, dst};
    return SHR_IN_PROGRESS;
}

static void src_cancel(void *user, uint64_t request) {
    pkg_source *s = user;
    for (int i = 0; i < s->npending; i++)
        if (s->pending[i].request == request) s->pending[i] = s->pending[--s->npending];
}

static void src_close(void *user) { ((pkg_source *)user)->closes++; }

static shr_status src_open(void *user, const char *name, shr_asset_source *out) {
    pkg_source *s = user;
    if (s->also && !strcmp(name, s->also->name)) s = s->also;
    if (strcmp(name, s->name)) return SHR_E_NOT_FOUND;
    shr_asset_source_init(out);
    out->user = s, out->size = s->size, out->close = src_close;
    if (s->mapped)
        out->data = s->data;
    else
        out->read = src_read;
    if ((s->mode & (ASYNC | ODD)) == (ASYNC | ODD)) out->cancel = src_cancel;
    s->opens++;
    return SHR_OK;
}

/* Completes the deferred reads (none when they hang). */
static void complete_all(shr_context *ctx, pkg_source *s) {
    while (s->npending && (s->mode & ODD) == 0) {
        pending_read r = s->pending[--s->npending];
        FUZZ_CHECK(shr_asset_complete(ctx, r.request, copy(s, r.offset, r.length, r.dst, 0)) == SHR_OK);
    }
}

static uint64_t fake_now;
static uint64_t fake_clock(void *user) {
    (void)user;
    return fake_now;
}

/* Pumps until nothing is due, completing reads and moving the clock to each deadline. */
static void settle(shr_context *ctx, pkg_source *s) {
    for (int i = 0; i < 64; i++) {
        complete_all(ctx, s);
        fuzz_settle(ctx);
        shr_deadline dl;
        FUZZ_CHECK(shr_next_deadline(ctx, &dl) == SHR_OK);
        if (dl.kind == SHR_DEADLINE_AT)
            fake_now = dl.at_ns;
        else if (!s->npending || (s->mode & ODD))
            return;
    }
}

static fuzz_output out;
static uint8_t mapped_pixels[sizeof(out.pixels)];

static void render(pkg_source *src, uint8_t mode) {
    memset(&out, 0, sizeof(out));
    shr_framebuffer_driver drv;
    FUZZ_CHECK(shr_software_driver_create(NULL, 0, 64, &drv) == SHR_OK);
    if ((mode & 7) >= 5) drv.caps.stride_align = 16;
    shr_output o;
    fuzz_output_init(&out, &o, 0);
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.driver = &drv, cd.output = &o;
    cd.io_retry_ns = cd.io_timeout_ns = 0; /* no clock */
    if (src->mode & ASYNC) { /* a clock, so hanging reads time out */
        fake_now = 0;
        cd.now_ns = fake_clock, cd.io_retry_ns = 1000, cd.io_timeout_ns = 5000;
    }
    cd.io_retry_limit = 2;
    cd.page_cache_bytes = (mode & TINY_CACHE) ? tiny_cache() : 64u << 20;
    shr_context *ctx;
    FUZZ_CHECK(shr_create(&cd, &ctx) == SHR_OK);
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = FUZZ_W, sd.height = FUZZ_H;
    FUZZ_CHECK(shr_screen_configure(ctx, &sd) == SHR_OK);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.user = src, fd.open = src_open;
    shr_pl_res_bitmap_font *font;
    FUZZ_CHECK(shr_pl_res_bitmap_font_create(ctx, &fd, &font) == SHR_OK);
    shr_lyr *l;
    FUZZ_CHECK(shr_lyr_create(ctx, 0, (shr_rect){0, 0, FUZZ_W, FUZZ_H}, &l) == SHR_OK);
    FUZZ_CHECK(shr_pl_lyr_tilemap_resize(l, font, 3, FUZZ_W / SHR_CELL_WIDTH, NULL) == SHR_OK);
    shr_text_style style = {SHR_RGB(255, 255, 255), 0, (mode & BOLD_ITALIC) ? SHR_STYLE_BOLD | SHR_STYLE_ITALIC : 0};
    for (int frame = 0; frame < 3; frame++) { /* the samples, other glyphs, the samples again */
        const char *const *text = frame == 1 ? others : samples;
        FUZZ_CHECK(shr_pl_lyr_tilemap_clear(l, 0, 0, 3, FUZZ_W / SHR_CELL_WIDTH, style) == SHR_OK);
        for (int i = 0; i < 3; i++)
            FUZZ_CHECK(shr_pl_lyr_tilemap_set_text(l, i, 0, text[i], strlen(text[i]), style, NULL, 0, 0, NULL) ==
                       SHR_OK);
        FUZZ_CHECK(shr_submit(ctx) == SHR_OK);
        settle(ctx, src);
    }
    if (src->also) { /* the sources are back: the cool-downs end and fail again */
        FUZZ_CHECK(shr_asset_ready(ctx) == SHR_OK);
        settle(ctx, src);
    }
    FUZZ_CHECK(out.presents > 0);
    FUZZ_CHECK(shr_lyr_destroy(l) == SHR_OK);
    FUZZ_CHECK(shr_pl_res_bitmap_font_destroy(font) == SHR_OK);
    FUZZ_CHECK(shr_begin_shutdown(ctx) == SHR_OK);
    shr_pump(ctx);
    FUZZ_CHECK(shr_destroy(ctx) == SHR_OK);
    FUZZ_CHECK(src->closes == src->opens && src->npending == 0);
    FUZZ_CHECK(!src->also || src->also->closes == src->also->opens);
    FUZZ_CHECK(shr_software_driver_destroy(&drv) == SHR_OK);
}

static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i);
}

static bool inside(uint64_t off, uint64_t len, size_t n) { return off <= n && len <= n - off; }

/* Pages first (ptab is always stored), then the index entries, the index and the header. */
static void reseal(uint8_t *p, size_t n) {
    if (n < 128) return;
    uint64_t sidx = shr__rd64(p + 40), sidx_size = shr__rd32(p + 28);
    if (inside(sidx, sidx_size, n))
        for (int pass = 0; pass < 2; pass++)
            for (uint64_t e = sidx + 24; e + 32 <= sidx + sidx_size; e += 32) {
                uint64_t off = shr__rd64(p + e + 8), len = shr__rd32(p + e + 16);
                if (!inside(off, len, n)) continue;
                if (pass) {
                    wr64(p + e + 24, XXH3_64bits(p + off, len));
                    continue;
                }
                for (uint64_t r = off + 16; !memcmp(p + e, "ptab", 4) && r + 24 <= off + len; r += 24)
                    if (inside(shr__rd64(p + r), shr__rd32(p + r + 16), n))
                        wr64(p + r + 8, XXH3_64bits(p + shr__rd64(p + r), shr__rd32(p + r + 16)));
            }
    if (inside(sidx, sidx_size, n)) wr64(p + 80, XXH3_64bits(p + sidx, sidx_size));
    wr64(p + 120, XXH3_64bits(p, 120));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    uint8_t mode = data[0], flags = data[1];
    size_t n = size - 2, at = (flags & MISALIGN) ? 1 : 0;
    uint8_t *buf = aligned_alloc(256, (n + at + 255) / 256 * 256 + 256), *pkg = buf + at;
    FUZZ_CHECK(buf);
    memcpy(pkg, data + 2, n);
    if (flags & RESEAL) reseal(pkg, n);
    const char *name = roles[(mode & 7) % 5];
    pkg_source src = {.data = pkg, .size = n, .name = name, .mapped = true};
    render(&src, mode);
    memcpy(mapped_pixels, out.pixels, sizeof(mapped_pixels));
    src = (pkg_source){.data = pkg, .size = n, .name = name, .mode = mode};
    render(&src, mode);
    if (!(mode & (TINY_CACHE | ODD))) FUZZ_CHECK(memcmp(mapped_pixels, out.pixels, sizeof(mapped_pixels)) == 0);
    pkg_source other = {.data = pkg, .size = n, .name = roles[name == roles[0]], .failing = true};
    src = (pkg_source){.data = pkg, .size = n, .name = name, .mode = mode, .also = &other};
    render(&src, mode);
    free(buf);
    return 0;
}
