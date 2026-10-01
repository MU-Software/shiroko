/* Font packages with arbitrary bytes, opened by the font plugin as memory-mapped sources (in place) and
 * through read() (header, table, index and pages read and cached), then read again next to a second package
 * whose reads always fail (two packages cool down). Byte 0 selects the package role (bits 0-2), asynchronous
 * reads completed later (0x08), bold italic text (0x10), a tiny page cache (0x20), every fourth read failing (0x40)
 * and (0x80) with 0x08 reads that never complete (the watchdog cancels them), else reads returning different
 * bytes each time. Invariants: no out-of-bounds access, every frame rasters (bad packages or pages fall back),
 * both paths draw the same pixels while reads are faithful, each opened source is closed. */
#include "fuzz_common.h"

static const char *const roles[5] = {"shiroko-latin.shrf", "shiroko-cjk-ko.shrf", "shiroko-symbols.shrf",
                                     "shiroko-emoji.shrf", "shiroko-nerd.shrf"};
static const char *const samples[3] = {
    "Aa0\xEA\xB0\x80\xE1\x84\x80\xE1\x85\xA1", /* latin, Hangul, conjoining jamo */
    "\xF0\x9F\x98\x80\xE2\x9D\xA4\xEF\xB8\x8F\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB",
    "\xEE\x82\xB0\xE2\x96\x88\xE6\xBC\xA2\x65\xCC\x81\xEF\xBF\xBD"};

enum { ASYNC = 0x08, BOLD_ITALIC = 0x10, TINY_CACHE = 0x20, FAIL_FIRST = 0x40, ODD = 0x80 };

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
    FUZZ_CHECK(shr_software_driver_create(NULL, 0, &drv) == SHR_OK);
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
    cd.page_cache_bytes = (mode & TINY_CACHE) ? 4096 : 64u << 20;
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
    for (int i = 0; i < 3; i++)
        FUZZ_CHECK(shr_pl_lyr_tilemap_set_text(l, i, 0, samples[i], strlen(samples[i]), style, NULL, 0, 0, NULL) ==
                   SHR_OK);
    FUZZ_CHECK(shr_submit(ctx) == SHR_OK);
    settle(ctx, src);
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    uint8_t mode = data[0];
    const char *name = roles[(mode & 7) % 5];
    pkg_source src = {.data = data + 1, .size = size - 1, .name = name, .mapped = true};
    render(&src, mode);
    memcpy(mapped_pixels, out.pixels, sizeof(mapped_pixels));
    src = (pkg_source){.data = data + 1, .size = size - 1, .name = name, .mode = mode};
    render(&src, mode);
    if (!(mode & (TINY_CACHE | ODD))) FUZZ_CHECK(memcmp(mapped_pixels, out.pixels, sizeof(mapped_pixels)) == 0);
    pkg_source other = {.data = data + 1, .size = size - 1, .name = roles[name == roles[0]], .failing = true};
    src = (pkg_source){.data = data + 1, .size = size - 1, .name = name, .mode = mode, .also = &other};
    render(&src, mode);
    return 0;
}
