#include "font_core.h"
#include "harness.h"

#define CW SHR_CELL_WIDTH
#define CH SHR_CELL_HEIGHT
#define MS 1000000ull

/* ===== Test sources ===== */

typedef struct pending_read {
    uint64_t request, off;
    uint32_t len;
    void *dst;
} pending_read;

enum { SRC_READ, SRC_MAP, SRC_NONE };

/* Reads past `size` return zeros, so a source can claim any size. */
typedef struct tsrc {
    const uint8_t *data;
    size_t size;
    uint64_t claim; /* reported size; 0 = size */
    int mode;
    bool cancellable;
    shr_status open_result;
    shr_status script[32]; /* per read call; 0 = served synchronously, SHR_IN_PROGRESS = deferred */
    int poke_call, poke_at; /* read call `poke_call` returns 0x7F at dst[poke_at] */
    int calls, pending, closes, cancels;
    pending_read q[8];
    shr_pl_res_bitmap_font *destroy_on_cancel; /* called back into from cancel() */
    shr_status destroyed;
} tsrc;

static void ts_copy(const tsrc *s, uint64_t off, uint32_t len, void *dst) {
    memset(dst, 0, len);
    if (off < s->size) memcpy(dst, s->data + off, s->size - off < len ? s->size - off : len);
}

static shr_status ts_read(void *user, uint64_t off, uint32_t len, void *dst, uint64_t request) {
    tsrc *s = user;
    shr_status st = s->calls < 32 ? s->script[s->calls] : SHR_OK;
    if (st == SHR_IN_PROGRESS) s->q[s->pending++] = (pending_read){request, off, len, dst};
    if (st == SHR_OK) ts_copy(s, off, len, dst);
    if (++s->calls == s->poke_call) ((uint8_t *)dst)[s->poke_at] = 0x7F;
    return st;
}

static void ts_cancel(void *user, uint64_t request) {
    tsrc *s = user;
    s->cancels++;
    if (s->destroy_on_cancel) s->destroyed = shr_pl_res_bitmap_font_destroy(s->destroy_on_cancel);
    for (int i = 0; i < s->pending; i++)
        if (s->q[i].request == request) {
            memmove(&s->q[i], &s->q[i + 1], (size_t)(--s->pending - i) * sizeof(s->q[0]));
            return;
        }
}

static void ts_close(void *user) { ((tsrc *)user)->closes++; }

/* Completes the oldest deferred read. */
static void ts_complete(shr_context *ctx, tsrc *s, shr_status result) {
    pending_read r = s->q[0];
    memmove(&s->q[0], &s->q[1], (size_t)--s->pending * sizeof(s->q[0]));
    if (result == SHR_OK) ts_copy(s, r.off, r.len, r.dst);
    ASSERT_EQ_LL(shr_asset_complete(ctx, r.request, result), SHR_OK);
}

/* The packages an open() callback serves, by role; NULL = not installed. */
typedef struct lib {
    tsrc *src[ROLE_COUNT];
    char first[40];
    int opens;
    shr_context *reenter; /* open() tries context calls; each must be refused */
    shr_pl_res_bitmap_font *font;
    int refused;
} lib;

static shr_status lib_open(void *user, const char *name, shr_asset_source *out) {
    static const char *const names[ROLE_COUNT] = {"", "shiroko-latin.shrf", "shiroko-cjk-", "shiroko-symbols.shrf",
                                                  "shiroko-emoji.shrf", "shiroko-nerd.shrf"};
    lib *l = user;
    if (!l->opens++) snprintf(l->first, sizeof(l->first), "%s", name);
    if (l->reenter) {
        shr_pl_res_bitmap_font_desc d;
        shr_pl_res_bitmap_font_desc_init(&d);
        shr_pl_res_bitmap_font *f = (shr_pl_res_bitmap_font *)l;
        l->refused += shr_pl_res_bitmap_font_create(l->reenter, &d, &f) == SHR_E_STATE && !f;
        l->refused += shr_pl_res_bitmap_font_destroy(l->font) == SHR_E_STATE;
    }
    int role = ROLE_LATIN;
    while (role < ROLE_COUNT && strncmp(name, names[role], strlen(names[role]))) role++;
    tsrc *s = role < ROLE_COUNT ? l->src[role] : NULL;
    if (!s) return SHR_E_NOT_FOUND;
    if (s->open_result) return s->open_result;
    shr_asset_source_init(out);
    out->user = s;
    out->size = s->claim ? s->claim : s->size;
    if (s->mode == SRC_MAP) out->data = s->data;
    if (s->mode == SRC_READ) out->read = ts_read;
    out->cancel = s->cancellable ? ts_cancel : NULL;
    out->close = ts_close;
    return SHR_OK;
}

static shr_pl_res_bitmap_font *font_new(shr_context *ctx, lib *l, const char *locale) {
    shr_pl_res_bitmap_font_desc d;
    shr_pl_res_bitmap_font_desc_init(&d);
    d.user = l, d.open = l ? lib_open : NULL, d.locale = locale;
    shr_pl_res_bitmap_font *f = NULL;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(ctx, &d, &f), SHR_OK);
    return f;
}

static void font_free_now(shr_context *ctx, shr_pl_res_bitmap_font *f) {
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(f), SHR_OK);
    shr_pump(ctx);
}

/* ===== Glyphs ===== */

static uint64_t glyph(shr_pl_res_bitmap_font *f, const uint32_t *cps, size_t n) {
    shr__cluster_class cls;
    shr__classify(cps, n, &cls);
    uint64_t id = 0;
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, cps, n, &cls, &id), SHR_OK);
    return id;
}

static uint64_t glyph1(shr_pl_res_bitmap_font *f, uint32_t cp) { return glyph(f, &cp, 1); }

/* A+U+0301: the glyph on a synthetic package's second page. */
static uint64_t glyph2(shr_pl_res_bitmap_font *f) {
    const uint32_t s[2] = {'A', 0x301};
    return glyph(f, s, 2);
}

static uint64_t frame_no = 1000;

/* Resolves in a frame of its own, which then ends. */
static shr_status resolve(shr_pl_res_bitmap_font *f, uint64_t id, shr__resolved *out) {
    shr__res *res = shr__bitmap_font_res(f);
    *out = (shr__resolved){0};
    shr_status st = res->ops->resolve(res, id, ++frame_no, out);
    res->ops->frame_end(res, frame_no);
    return st;
}

/* Resolves several glyphs in one frame, which then ends. */
static void resolve_all(shr_pl_res_bitmap_font *f, const uint64_t *ids, size_t n) {
    shr__res *res = shr__bitmap_font_res(f);
    shr__resolved r;
    frame_no++;
    for (size_t i = 0; i < n; i++) res->ops->resolve(res, ids[i], frame_no, &r);
    res->ops->frame_end(res, frame_no);
}

/* Pumps, advancing the fake clock to each deadline, until nothing is left (bounded). */
static void drain(shr_context *ctx) {
    for (int i = 0; i < 64; i++) {
        settle(ctx);
        shr_deadline dl;
        shr_next_deadline(ctx, &dl);
        if (dl.kind != SHR_DEADLINE_AT) return;
        fake_now = dl.at_ns;
    }
}

/* Resolves until the result is final. */
static shr_status load(shr_context *ctx, shr_pl_res_bitmap_font *f, uint64_t id, shr__resolved *out) {
    shr_status st = resolve(f, id, out);
    for (int i = 0; i < 16 && st == SHR_OK && out->provisional; i++) {
        drain(ctx);
        st = resolve(f, id, out);
    }
    return st;
}

static bool inside(const shr__resolved *r, const uint8_t *data, size_t size) {
    const uint8_t *p = r->image.pixels;
    return p >= data && p < data + size;
}

static bool same_image(const shr__resolved *a, const shr__resolved *b) {
    if (a->image.width != b->image.width || a->image.height != b->image.height || a->image.format != b->image.format ||
        a->offset.x != b->offset.x || a->offset.y != b->offset.y)
        return false;
    size_t row = a->image.format == SHR_FORMAT_A4 ? ((size_t)a->image.width + 1) / 2 : (size_t)a->image.width;
    for (int32_t y = 0; y < a->image.height; y++)
        if (memcmp((const uint8_t *)a->image.pixels + y * a->image.stride,
                   (const uint8_t *)b->image.pixels + y * b->image.stride, row))
            return false;
    return true;
}

static shr_status last_failure(shr_context *ctx) {
    shr_event ev = {.status = SHR_OK};
    count_events(ctx, SHR_EVENT_RESOURCE_FAILED, &ev);
    return ev.status;
}

/* ===== Synthetic packages =====
 * One CWxCH instance (A4 or A8), glyph slots cp0, 'B' (empty) and one shared by the sequences A+U+0301 and
 * B+U+0301 (and cp1 other than 'B'): slots 0 and 1 on the first page, slot 2 on the second. The table lists
 * STRINGS first. */
enum {
    SY_NSEC = 8,
    SY_MAN = 128 + 32 * SY_NSEC, SY_STR = SY_MAN + 36, SY_SRC = SY_STR + 8, SY_INS = SY_SRC + 48,
    SY_CMAP = SY_INS + 48, SY_SEQ = SY_CMAP + 16, SY_POOL = SY_SEQ + 24, SY_PGS = SY_POOL + 16,
    SY_PAGE0 = SY_PGS + 64, SY_PAGE1 = SY_PAGE0 + 52, SY_SIZE = SY_PAGE1 + 36,
    SY_G0 = SY_PAGE0 + 4, SY_G1 = SY_G0 + 16, SY_BM0 = SY_PAGE0 + 36, SY_BM2 = SY_PAGE1 + 20,
    SY_BASE = CH - 4, SY_A4 = 1, SY_A8 = 2
};
#define SY_TE(type) (128 + 32 * ((type) == 1 ? 1 : (type) == 2 ? 0 : (type) < 5 ? (type) - 1 : (type) - 2))
#define SY_PG(i) (SY_PGS + 32 * (i)) /* page record: offset, hash +8, length +16, first +20, count +24 */

static void put(uint8_t *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> 8 * i);
}

/* Recomputes the page, section and header checksums of a package of `size` bytes. */
static void seal_n(uint8_t *d, uint64_t size) {
    uint8_t *pgs = d + shr__rd64(d + SY_TE(9) + 8);
    for (int i = 0; i < 2 && pgs + 64 <= d + size; i++) {
        uint8_t *r = pgs + 32 * i;
        uint64_t off = shr__rd64(r);
        if (off <= size && size - off >= shr__rd32(r + 16)) put(r + 8, XXH3_64bits(d + off, shr__rd32(r + 16)), 8);
    }
    for (uint32_t i = 0; i < SY_NSEC; i++) {
        uint8_t *e = d + 128 + 32 * i;
        uint64_t off = shr__rd64(e + 8);
        if (off <= size && size - off >= shr__rd32(e + 16)) put(e + 24, XXH3_64bits(d + off, shr__rd32(e + 16)), 8);
    }
    put(d + 72, XXH3_64bits(d, 72), 8);
}

static void seal(uint8_t *d) { seal_n(d, SY_SIZE); }

static void synth_as(uint8_t *d, uint8_t role, const char *locale, uint32_t cp0, uint32_t cp1, uint8_t fmt) {
    static const uint32_t offs[10] = {0, SY_MAN, SY_STR, SY_SRC, SY_INS, 0, SY_CMAP, SY_SEQ, SY_POOL, SY_PGS};
    static const uint32_t lens[10] = {0, 36, 8, 48, 48, 0, 16, 24, 16, 64};
    static const uint32_t counts[10] = {0, 1, 8, 1, 1, 0, 2, 2, 4, 2};
    static const uint32_t pool[4] = {'A', 0x301, 'B', 0x301};
    memset(d, 0, SY_SIZE);
    memcpy(d, "SHRFPKG1", 8);
    put(d + 8, 3, 2), put(d + 10, 128, 2), put(d + 12, fmt | 4u, 4), put(d + 16, SY_NSEC, 4), put(d + 24, 128, 8);
    put(d + 32, SY_SIZE, 8);
    memcpy(d + 40, shr__text_profile_id, 32);
    for (uint32_t t = 1; t <= 9; t++)
        if (t != 5)
            put(d + SY_TE(t), t, 4), put(d + SY_TE(t) + 4, counts[t], 4), put(d + SY_TE(t) + 8, offs[t], 8),
                put(d + SY_TE(t) + 16, lens[t], 4);
    d[SY_MAN] = role;
    memcpy(d + SY_MAN + 4, locale, strlen(locale));
    put(d + SY_MAN + 16, 4, 4), put(d + SY_MAN + 20, 4, 4), put(d + SY_MAN + 24, 4, 4);
    put(d + SY_MAN + 28, 3, 4), put(d + SY_MAN + 32, 1, 4);
    memcpy(d + SY_STR, "synthpkg", 8);
    d[SY_INS + 3] = fmt;
    put(d + SY_INS + 4, CH, 2), put(d + SY_INS + 6, CW, 2), put(d + SY_INS + 12, SY_BASE, 2);
    put(d + SY_INS + 14, CH - 2, 2), put(d + SY_INS + 16, CH / 2, 2);
    put(d + SY_CMAP, cp0, 4), put(d + SY_CMAP + 8, cp1, 4), put(d + SY_CMAP + 12, cp1 == 'B' ? 1 : 2, 4);
    for (int i = 0; i < 4; i++) put(d + SY_POOL + 4 * i, pool[i], 4);
    for (int i = 0; i < 2; i++) {
        uint8_t *r = d + SY_SEQ + 12 * i;
        r[0] = 2, r[1] = 1, put(r + 4, 2 * (uint64_t)i, 4), put(r + 8, 2, 4);
    }
    uint8_t stride = fmt == SY_A4 ? 2 : 4;
    for (int i = 0; i < 2; i++) { /* slot 0 at x 1 on the baseline; slot 2 two cells wide, 3 rows lower */
        uint32_t at = i ? SY_PAGE1 : SY_PAGE0, count = i ? 1 : 2, table = 4 + 16 * count;
        uint8_t *rec = d + SY_PG(i), *pg = d + at, *e = pg + 4;
        put(rec, at, 8), put(rec + 16, table + 16, 4), put(rec + 20, 2 * (uint64_t)i, 4), put(rec + 24, count, 4);
        put(pg, count, 4);
        put(e, table, 4), put(e + 4, 4u * stride, 2);
        e[6] = 4, e[7] = 4, e[8] = (uint8_t)(i ? 0 : 1), e[9] = (uint8_t)(i ? SY_BASE - 3 : SY_BASE), e[10] = stride;
        e[11] = (uint8_t)(fmt | (i ? 8 : 4));
        if (!i) pg[4 + 16 + 11] = 4;
        memset(pg + table, 0xFF, 16);
    }
    seal(d);
}

static void synth(uint8_t *d) { synth_as(d, ROLE_LATIN, "", 'A', 'B', SY_A4); }

/* How a latin package ends: the RESOURCE_FAILED status, SHR_OK when 'A' resolves from it. */
static shr_status pkg_status(shr_context *ctx, tsrc *s) {
    lib l = {.src = {[ROLE_LATIN] = s}};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, NULL);
    shr__resolved r;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
    shr_status st = last_failure(ctx);
    ASSERT_EQ_LL(r.provisional, st == SHR_E_IO); /* a package cooling down after I/O errors is retried */
    if (st == SHR_OK && r.image.height != 4) st = SHR_E_NOT_FOUND;
    font_free_now(ctx, f);
    return st;
}

static uint64_t cache_limit;
static void tweak_cache(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)drv;
    d->page_cache_bytes = cache_limit;
}

/* ===== Create and destroy ===== */

TEST test_create_and_destroy_rules(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    shr_pl_res_bitmap_font_desc d;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_desc_init(NULL), SHR_E_INVALID_ARG);
    shr_pl_res_bitmap_font_desc_init(&d);
    shr_pl_res_bitmap_font *f = (shr_pl_res_bitmap_font *)&h;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(NULL, &d, &f), SHR_E_INVALID_ARG);
    ASSERT(f == NULL);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(ctx, NULL, &f), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(ctx, &d, NULL), SHR_E_INVALID_ARG);
    d.locale = "en";
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(ctx, &d, &f), SHR_E_INVALID_ARG);
    static const char *const locales[] = {"ko", "ja", "zh-Hans", "zh-Hant", "zh-HK"};
    for (int i = 0; i < 5; i++) {
        d.locale = locales[i];
        ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(ctx, &d, &f), SHR_OK);
        font_free_now(ctx, f);
    }
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(NULL), SHR_E_INVALID_ARG);

    /* A tilemap keeps it alive; so does a frame's pin, until the frame ends. */
    lib l = {.reenter = ctx};
    f = l.font = font_new(ctx, &l, NULL);
    shr_lyr *layer;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &layer), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, f, 1, 1, NULL), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(f), SHR_E_STATE);
    ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
    shr__resolved r;
    load(ctx, f, glyph1(f, 0xAC00), &r); /* open() runs as a host callback */
    ASSERT_EQ_LL(l.opens, 3);
    ASSERT_EQ_LL(l.refused, 6);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(f), SHR_OK);

    shr_begin_shutdown(ctx);
    d.locale = NULL;
    f = (shr_pl_res_bitmap_font *)&h;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(ctx, &d, &f), SHR_E_STATE);
    ASSERT(f == NULL);
    harness_close(&h);
    PASS();
}

static fail_alloc oom;
static shr_allocator oom_allocator;
static void tweak_oom(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)drv;
    d->allocator = &oom_allocator;
}

/* Every allocation of loading a package through reads or a mapping, interning and pinning may fail. */
TEST test_out_of_memory(void) {
    uint8_t d[SY_SIZE];
    synth(d);
    oom_allocator = fail_allocator(&oom);
    for (int mode = SRC_READ; mode <= SRC_MAP; mode++) {
        bool done = false;
        for (long budget = 0; !done; budget++) {
            oom = (fail_alloc){-1, 0};
            harness h;
            shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_oom);
            oom.budget = budget;
            tsrc s = {d, SY_SIZE, .mode = mode};
            lib l = {.src = {[ROLE_LATIN] = &s}};
            shr_pl_res_bitmap_font_desc fd;
            shr_pl_res_bitmap_font_desc_init(&fd);
            fd.user = &l, fd.open = lib_open;
            shr_pl_res_bitmap_font *f = NULL;
            shr_status st = shr_pl_res_bitmap_font_create(ctx, &fd, &f);
            if (st == SHR_OK) {
                uint32_t seq[2] = {'A', 0x301};
                shr__cluster_class cls;
                shr__classify(seq, 2, &cls);
                uint64_t id;
                st = shr__bitmap_font_glyph(f, seq, 2, &cls, &id);
                shr__resolved r = {0};
                if (st == SHR_OK) st = load(ctx, f, id, &r);
                done = st == SHR_OK && inside(&r, d, SY_SIZE) == (mode == SRC_MAP) && r.image.height == 4;
                shr_status ev = last_failure(ctx);
                ASSERT(ev == SHR_OK || ev == SHR_E_NO_MEMORY);
                h.font = f;
            }
            ASSERT(st == SHR_OK || st == SHR_E_NO_MEMORY);
            oom.budget = -1;
            harness_close(&h);
            ASSERT_EQ_LL(oom.live, 0);
        }
    }
    PASS();
}

/* ===== Glyph ids and the built-in package ===== */

TEST test_builtin_without_packages(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    lib l = {0};
    shr_pl_res_bitmap_font *fonts[2] = {font_new(ctx, NULL, NULL), font_new(ctx, &l, NULL)};
    for (int i = 0; i < 2; i++) {
        shr_pl_res_bitmap_font *f = fonts[i];
        shr__resolved a, fffd, hangul;
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &a), SHR_OK);
        ASSERT(!a.provisional && a.image.height > 4 && inside(&a, shr__builtin_package, shr__builtin_package_size));
        ASSERT_EQ_LL(a.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xFFFD), &fffd), SHR_OK);
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &hangul), SHR_OK);
        ASSERT(!hangul.provisional && same_image(&hangul, &fffd)); /* not installed: U+FFFD */
        ASSERT_EQ_LL(hangul.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
        static const uint32_t box[4] = {0x2500, 0x259F, 0x1FB00, 0x1FBFF}, text[4] = {0x24FF, 0x25A0, 0x1FAFF, 0x1FC00};
        for (int k = 0; k < 4; k++) { /* box drawing and blocks join their neighbours, whatever draws them */
            ASSERT_EQ_LL(load(ctx, f, box[k], &a), SHR_OK);
            ASSERT_EQ_LL(a.synth, 0);
            ASSERT_EQ_LL(load(ctx, f, text[k], &a), SHR_OK);
            ASSERT_EQ_LL(a.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
        }
        uint32_t cluster[2] = {0x2500, 0xFE0E};
        ASSERT_EQ_LL(load(ctx, f, glyph(f, cluster, 2), &a), SHR_OK); /* by the cluster's base */
        ASSERT_EQ_LL(a.synth, 0);
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, ' '), &a), SHR_E_NOT_FOUND); /* empty glyph: nothing to draw */
    }
    ASSERT_EQ_LL(l.opens, 3); /* latin, CJK and symbols, each asked once */
    shr__line_metrics lm = shr__bitmap_font_line_metrics();
    ASSERT(lm.baseline > 0 && lm.baseline <= CH && lm.underline_y < CH && lm.strike_y < lm.baseline);
    ASSERT_EQ_LL(shr__bitmap_font_res(fonts[0])->ops->has_work(shr__bitmap_font_res(fonts[0])), false);
    h.font = fonts[0];
    font_free_now(ctx, fonts[1]);
    harness_close(&h);
    PASS();
}

TEST test_glyph_ids(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, NULL, NULL);
    uint32_t a = 'A', big = 0x110000, zw = 0x200B;
    shr__cluster_class cls;
    shr__classify(&a, 1, &cls);
    uint64_t id;
    ASSERT_EQ_LL(shr__bitmap_font_glyph(NULL, &a, 1, &cls, &id), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, NULL, 1, &cls, &id), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, &a, 0, &cls, &id), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, &a, 1, NULL, &id), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, &a, 1, &cls, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, &big, 1, &cls, &id), SHR_E_INVALID_ARG);
    shr__classify(&zw, 1, &cls);
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, &zw, 1, &cls, &id), SHR_E_NOT_FOUND); /* invisible */
    uint32_t mark = 0x301;
    ASSERT_EQ_LL(glyph(f, &mark, 1), 0xFFFD); /* replacement */
    ASSERT_EQ_LL(glyph1(f, 'A'), 'A');
    ASSERT_EQ_LL(glyph1(f, 0x1F600), 0x1F600 | SHR_ID_EMOJI);

    uint32_t s1[2] = {'e', 0x301}, s2[2] = {'a', 0x301};
    uint64_t i1 = glyph(f, s1, 2), i2 = glyph(f, s2, 2);
    ASSERT_EQ_LL(i1, SHR_ID_CLUSTER);
    ASSERT_EQ_LL(i2, SHR_ID_CLUSTER | 1);
    ASSERT_EQ_LL(glyph(f, s1, 2), i1); /* interned once */
    static uint32_t many[256];
    for (int i = 0; i < 256; i++) many[i] = i ? 0x301 : 'a';
    shr__classify(many, 256, &cls);
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, many, 256, &cls, &id), SHR_OK); /* too long to intern */
    ASSERT_EQ_LL(id, 0xFFFD);
    for (int i = 1; i < 256; i++) many[i] = 0x200B;
    ASSERT_EQ_LL(glyph(f, many, 256), 'a'); /* one visible scalar: its base */
    for (uint32_t i = 2; i < SHR_FONT_MAX_CLUSTERS; i++) {
        uint32_t s[2] = {0x4E00 + i % 20000, 0x301 + i / 20000};
        glyph(f, s, 2);
    }
    ASSERT_EQ_LL(glyph(f, s1, 2), i1); /* found even when the table is full */
    uint32_t s3[2] = {'o', 0x301}, s4[3] = {'o', 0x301, 0x302}, text[2] = {'A', 0xFE0E}, smile[2] = {0x1F600, 0xFE0F};
    ASSERT_EQ_LL(glyph(f, s3, 2), 0xFFFD);
    ASSERT_EQ_LL(glyph(f, s4, 3), 0xFFFD);
    ASSERT_EQ_LL(glyph(f, text, 2), 'A'); /* full: the visible base */
    ASSERT_EQ_LL(glyph(f, smile, 2), 0x1F600 | SHR_ID_EMOJI);
    shr__resolved r;
    ASSERT_EQ_LL(resolve(f, SHR_ID_CLUSTER | SHR_FONT_MAX_CLUSTERS, &r), SHR_E_INVALID_ARG);
    harness_close(&h);
    PASS();
}

/* ===== Package format ===== */

typedef struct mutation {
    uint32_t at;
    uint8_t width;
    uint64_t value;
    shr_status want;
} mutation;

TEST test_package_validation(void) {
    static const mutation cases[] = {
        {0, 1, 'X', SHR_E_FORMAT},                 /* magic */
        {8, 2, 2, SHR_E_FORMAT},                   /* version 2 */
        {10, 2, 64, SHR_E_FORMAT},                 /* header size */
        {12, 4, 15, SHR_E_UNSUPPORTED},            /* unknown required feature */
        {120, 1, 1, SHR_E_FORMAT},                 /* reserved header bytes */
        {80, 1, 1, SHR_E_FORMAT},
        {20, 4, 1, SHR_E_FORMAT},
        {16, 4, 0, SHR_E_FORMAT},                  /* section count */
        {16, 4, 17, SHR_E_FORMAT},
        {24, 8, 64, SHR_E_FORMAT},                 /* table offset */
        {24, 8, SY_SIZE + 8, SHR_E_FORMAT},
        {24, 8, SY_SIZE - 16, SHR_E_FORMAT},
        {32, 8, SY_SIZE + 1, SHR_E_FORMAT},           /* file size */
        {SY_TE(2) + 8, 8, SY_SIZE + 1, SHR_E_FORMAT}, /* section past the file */
        {SY_TE(2) + 16, 4, SY_SIZE, SHR_E_LIMIT},     /* index past the file */
        {SY_TE(2), 4, 0, SHR_E_FORMAT},               /* unknown section type */
        {SY_TE(2), 4, 11, SHR_E_FORMAT},
        {SY_TE(2), 4, 1, SHR_E_FORMAT},               /* duplicate */
        {SY_TE(3), 4, 5, SHR_E_FORMAT},               /* section type 5 */
        {SY_TE(2) + 20, 4, 1, SHR_E_FORMAT},          /* reserved entry field */
        {SY_TE(2) + 8, 8, SY_MAN + 4, SHR_E_FORMAT},  /* overlapping */
        {SY_TE(2) + 4, 4, 5000000, SHR_E_FORMAT},     /* record count */
        {SY_TE(1), 4, 10, SHR_E_FORMAT},              /* missing required sections */
        {SY_TE(4), 4, 10, SHR_E_FORMAT},
        {SY_TE(6), 4, 10, SHR_E_FORMAT},
        {SY_TE(9), 4, 10, SHR_E_FORMAT},
        {SY_TE(1) + 4, 4, 2, SHR_E_FORMAT},           /* MANIFEST */
        {SY_TE(1) + 16, 4, 35, SHR_E_FORMAT},
        {SY_MAN + 1, 1, 1, SHR_E_FORMAT},
        {SY_MAN, 1, 0, SHR_E_FORMAT},                 /* role */
        {SY_MAN, 1, 6, SHR_E_FORMAT},
        {SY_MAN, 1, ROLE_CJK, SHR_E_FORMAT},          /* not the role of its file name */
        {SY_MAN + 4, 1, 0x1F, SHR_E_FORMAT},          /* locale */
        {SY_MAN + 4, 1, 0x7F, SHR_E_FORMAT},
        {SY_MAN + 16, 4, 9, SHR_E_FORMAT},            /* string references */
        {SY_MAN + 24, 4, 5, SHR_E_FORMAT},
        {SY_MAN + 28, 4, 5000000, SHR_E_FORMAT},      /* glyph and instance counts */
        {SY_MAN + 28, 4, 0, SHR_E_FORMAT},
        {SY_MAN + 32, 4, 0, SHR_E_FORMAT},
        {SY_MAN + 32, 4, 2, SHR_E_FORMAT},            /* one instance only */
        {SY_TE(4) + 4, 4, 2, SHR_E_FORMAT},           /* INSTANCES */
        {SY_TE(4) + 16, 4, 47, SHR_E_FORMAT},
        {SY_TE(3) + 4, 4, 2, SHR_E_FORMAT},           /* SOURCES */
        {SY_SRC, 4, 9, SHR_E_FORMAT},                 /* source name past STRINGS */
        {SY_SRC + 44, 4, 9, SHR_E_FORMAT},            /* source license past STRINGS */
        {SY_INS, 2, 1, SHR_E_FORMAT},
        {SY_INS + 2, 1, 1, SHR_E_FORMAT},             /* a style other than regular */
        {SY_INS + 3, 1, 3, SHR_E_FORMAT},
        {SY_INS + 3, 1, 0, SHR_E_FORMAT},
        {12, 4, 6, SHR_E_FORMAT},                     /* A4 instance without the feature */
        {SY_INS + 4, 2, 0, SHR_E_FORMAT},
        {SY_INS + 4, 2, 1025, SHR_E_FORMAT},
        {SY_INS + 6, 2, 0, SHR_E_FORMAT},
        {SY_INS + 6, 2, 1025, SHR_E_FORMAT},
        {SY_INS + 6, 2, CW + 1, SHR_E_UNSUPPORTED},   /* another cell size */
        {SY_INS + 4, 2, CH + 1, SHR_E_UNSUPPORTED},
        {SY_INS + 12, 2, 0xFFFF, SHR_E_FORMAT},       /* line metrics */
        {SY_INS + 12, 2, CH + 1, SHR_E_FORMAT},
        {SY_INS + 14, 2, 0xFFFF, SHR_E_FORMAT},
        {SY_INS + 14, 2, CH, SHR_E_FORMAT},
        {SY_INS + 16, 2, 0xFFFF, SHR_E_FORMAT},
        {SY_INS + 16, 2, CH, SHR_E_FORMAT},
        {SY_INS + 44, 1, 1, SHR_E_FORMAT},
        {SY_INS + 36, 1, 1, SHR_E_FORMAT},            /* reserved bytes start after the instance id */
        {SY_INS + 18, 2, 2, SHR_E_FORMAT},            /* unknown raster flag */
        {SY_INS + 18, 2, 1, SHR_OK},
        {SY_TE(6) + 16, 4, 15, SHR_E_FORMAT},         /* CMAP */
        {SY_CMAP + 8, 4, 0x110000, SHR_E_FORMAT},
        {SY_CMAP + 8, 4, 0xD800, SHR_E_FORMAT},
        {SY_CMAP + 12, 4, 3, SHR_E_FORMAT},
        {SY_CMAP + 8, 4, 'A', SHR_E_FORMAT},
        {SY_TE(8), 4, 10, SHR_E_FORMAT},              /* SEQS and SEQPOOL */
        {SY_TE(8) + 16, 4, 15, SHR_E_FORMAT},
        {12, 4, 3, SHR_E_FORMAT},
        {SY_TE(7) + 16, 4, 23, SHR_E_FORMAT},
        {SY_POOL + 4, 4, 0x110000, SHR_E_FORMAT},
        {SY_POOL + 4, 4, 0xDC00, SHR_E_FORMAT},
        {SY_SEQ, 1, 1, SHR_E_FORMAT},
        {SY_SEQ, 1, 17, SHR_E_FORMAT},
        {SY_SEQ + 1, 1, 0, SHR_E_FORMAT},
        {SY_SEQ + 1, 1, 5, SHR_E_FORMAT},
        {SY_SEQ + 2, 2, 1, SHR_E_FORMAT},
        {SY_SEQ + 4, 4, 3, SHR_E_FORMAT},
        {SY_SEQ + 8, 4, 3, SHR_E_FORMAT},
        {SY_POOL + 8, 4, 'A', SHR_E_FORMAT},
        {SY_TE(9) + 4, 4, 0, SHR_E_FORMAT},           /* PAGES */
        {SY_TE(9) + 4, 4, 70000, SHR_E_FORMAT},
        {SY_TE(9) + 16, 4, 63, SHR_E_FORMAT},
        {SY_PG(0) + 20, 4, 1, SHR_E_FORMAT},
        {SY_PG(1) + 24, 4, 0, SHR_E_FORMAT},
        {SY_PG(0) + 16, 4, 20, SHR_E_FORMAT},
        {SY_PG(1) + 16, 4, 2u << 20, SHR_E_FORMAT},
        {SY_PG(0), 8, SY_PGS, SHR_E_FORMAT},
        {SY_PG(1), 8, SY_SIZE + 1, SHR_E_FORMAT},
        {SY_PG(1) + 16, 4, 100, SHR_E_FORMAT},
        {SY_PG(0) + 28, 4, 1, SHR_E_FORMAT},          /* reserved record field */
        {SY_PG(1) + 24, 4, 2, SHR_E_FORMAT},          /* glyphs not covered */
        {SY_TE(3), 4, 10, SHR_E_FORMAT},              /* SOURCES retyped as a malformed COVERAGE */
        {SY_TE(7), 4, 10, SHR_E_FORMAT},
        {SY_TE(7) + 4, 4, 0, SHR_OK},
        {SY_MAN + 4, 1, 'k', SHR_OK},                 /* a latin package's locale is not checked */
    };
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    for (int mode = SRC_READ; mode <= SRC_MAP; mode++) {
        tsrc s = {d, SY_SIZE, .mode = mode};
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            synth(d);
            put(d + cases[i].at, cases[i].value, cases[i].width);
            seal(d);
            shr_status st = pkg_status(ctx, &s);
            if (st != cases[i].want) fprintf(stderr, "mode %d case %zu\n", mode, i);
            ASSERT_EQ_LL(st, cases[i].want);
        }
        synth(d); /* optional sections: SOURCES absent, a well-formed COVERAGE in its place */
        put(d + SY_TE(3), 10, 4), put(d + SY_TE(3) + 4, 9, 4), put(d + SY_TE(3) + 16, 36, 4);
        seal(d);
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_OK);
        put(d + SY_TE(3) + 4, 8, 4);
        seal(d);
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT);
        put(d + SY_TE(3) + 4, 9, 4), put(d + SY_TE(3) + 16, 35, 4);
        seal(d);
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT);
        for (int seqs = 0; seqs < 2; seqs++) { /* SEQPOOL absent: the last table entry takes its slot */
            synth(d);
            memcpy(d + SY_TE(8), d + SY_TE(9), 32);
            put(d + 16, SY_NSEC - 1, 4);
            if (!seqs) put(d + SY_TE(7) + 4, 0, 4), put(d + SY_TE(7) + 16, 0, 4);
            seal(d);
            ASSERT_EQ_LL(pkg_status(ctx, &s), seqs ? SHR_E_FORMAT : SHR_OK);
        }
        synth(d); /* SEQS absent: COVERAGE in its place, a one-scalar SEQPOOL after it */
        put(d + SY_TE(7), 10, 4), put(d + SY_TE(7) + 4, 9, 4), put(d + SY_TE(7) + 16, 36, 4);
        put(d + SY_TE(8) + 4, 1, 4), put(d + SY_TE(8) + 8, SY_SEQ + 36, 8), put(d + SY_TE(8) + 16, 4, 4);
        seal(d);
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_OK);
        synth_as(d, ROLE_LATIN, "", 'A', 'B', SY_A8);
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_OK);
        put(d + 12, 5, 4); /* A8 instance without the feature */
        seal(d);
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT);
        synth(d);
        d[40] ^= 1;
        seal(d);
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_PROFILE_MISMATCH);
        synth(d);
        d[50] ^= 1;
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_CHECKSUM); /* header checksum */
        synth(d);
        d[SY_TE(2) + 24] ^= 1;
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT); /* section checksum */
        s.claim = SY_SIZE - 1;
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT); /* source shorter than the header says */
        s.claim = 100;
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT); /* shorter than a header */
        s.claim = 0;
    }
    harness_close(&h);
    PASS();
}

TEST test_index_size_limit(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    uint64_t size = 200ull << 20;
    put(d + 32, size, 8);
    put(d + SY_TE(9) + 16, 129u << 20, 4);
    seal(d);
    tsrc s = {d, SY_SIZE, .claim = size};
    ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_LIMIT);
    ASSERT_EQ_LL(s.closes, 1);
    harness_close(&h);
    PASS();
}

TEST test_table_changed_between_reads(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    for (int at = 15; at <= 19; at += 4) { /* an offset, then a length, in the re-read index */
        tsrc s = {d, SY_SIZE, .poke_call = 3, .poke_at = at};
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT);
    }
    harness_close(&h);
    PASS();
}

TEST test_page_validation(void) {
    static const mutation cases[] = {
        {SY_PAGE0, 4, 3, 0},       /* glyph count */
        {SY_G0 + 11, 1, 0x07, 0},  /* format 3 */
        {SY_G0 + 11, 1, 0x01, 0},  /* 0 cells */
        {SY_G0 + 11, 1, 0x0D, 0},  /* 3 cells */
        {SY_G0 + 11, 1, 0x25, 0},  /* reserved flag */
        {SY_G0 + 11, 1, 0x15, 0},  /* flag bit 4 */
        {SY_G0 + 14, 2, 1, 0},
        {SY_G0 + 6, 1, 0, 0},      /* width */
        {SY_G0 + 7, 1, 0, 0},      /* height */
        {SY_G0 + 10, 1, 1, 0},     /* stride */
        {SY_G0 + 4, 2, 7, 0},      /* bitmap length */
        {SY_G0, 4, 8, 0},          /* inside the glyph table */
        {SY_G0, 4, 48, 0},         /* past the page */
        {SY_G1 + 4, 2, 1, 0},      /* empty glyph with data */
        {SY_G1 + 6, 1, 1, 0},
        {SY_G1 + 7, 1, 1, 0},
        {SY_G0 + 11, 1, 0x06, 0},  /* A8 bitmap in an A4 instance */
        {SY_G0 + 6, 1, 3, 0},      /* odd A4 width: the padding nibble is not zero */
        {SY_BM0 + 4, 1, 0, 0},     /* payload: page checksum */
    };
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    for (int mode = SRC_READ; mode <= SRC_MAP; mode++) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            synth(d);
            put(d + cases[i].at, cases[i].value, cases[i].width);
            if (i + 1 < sizeof(cases) / sizeof(cases[0])) seal(d);
            tsrc s = {d, SY_SIZE, .mode = mode};
            shr_status st = pkg_status(ctx, &s);
            if (st != SHR_E_CHECKSUM) fprintf(stderr, "mode %d case %zu\n", mode, i);
            ASSERT_EQ_LL(st, SHR_E_CHECKSUM);
            ASSERT_EQ_LL(s.calls, mode == SRC_READ ? 3 + 4 : 0); /* the page is read once, then retried 3 times */
        }
        synth_as(d, ROLE_LATIN, "", 'A', 'B', SY_A8);
        d[SY_G0 + 11] = 0x05; /* A4 bitmap in an A8 instance */
        seal(d);
        tsrc a8 = {d, SY_SIZE, .mode = mode};
        ASSERT_EQ_LL(pkg_status(ctx, &a8), SHR_E_CHECKSUM);
        synth(d);
        d[SY_G0 + 6] = 3;
        for (int y = 0; y < 4; y++) d[SY_BM0 + 2 * y + 1] = 0xF0; /* odd A4 width, zero padding */
        seal(d);
        tsrc s = {d, SY_SIZE, .mode = mode};
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_OK);
    }
    harness_close(&h);
    PASS();
}

TEST test_synthetic_glyphs(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__resolved r, a, other;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &a), SHR_OK);
    ASSERT(!a.provisional);
    ASSERT(a.image.pixels == d + SY_BM0); /* mapped: used in place */
    ASSERT_EQ_LL(a.image.width, 4);
    ASSERT_EQ_LL(a.image.stride, 2);
    ASSERT_EQ_LL(a.image.format, SHR_FORMAT_A4);
    ASSERT_EQ_LL(a.offset.x, 1);
    ASSERT_EQ_LL(a.offset.y, 0);
    ASSERT_EQ_LL(a.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
    ASSERT_EQ_LL(a.slant_axis, CH); /* twice the line centre's row in the bitmap */
    ASSERT(!f->pinned && !f->lru); /* mapped pages are never pinned or cached */
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'B'), &r), SHR_E_NOT_FOUND); /* empty glyph */
    uint32_t sa[2] = {'A', 0x301}, sb[2] = {'B', 0x301}, sc[2] = {'C', 0x301}, sel[2] = {'A', 0xFE0E};
    ASSERT_EQ_LL(load(ctx, f, glyph(f, sa, 2), &r), SHR_OK);
    ASSERT_EQ_LL(load(ctx, f, glyph(f, sb, 2), &other), SHR_OK);
    ASSERT(r.image.pixels == d + SY_BM2 && other.image.pixels == r.image.pixels); /* shared slot, second page */
    ASSERT_EQ_LL(r.offset.x, 0);
    ASSERT_EQ_LL(r.offset.y, 3);
    ASSERT_EQ_LL(r.slant_axis, CH - 6);
    ASSERT_EQ_LL(load(ctx, f, glyph(f, sc, 2), &r), SHR_OK); /* no such sequence: U+FFFD */
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xFFFD), &other), SHR_OK);
    ASSERT(same_image(&r, &other));
    ASSERT_EQ_LL(load(ctx, f, glyph(f, sel, 2), &r), SHR_OK); /* one visible scalar: its base glyph */
    ASSERT(r.image.pixels == a.image.pixels);
    font_free_now(ctx, f);

    synth_as(d, ROLE_LATIN, "", 'A', 'B', SY_A8);
    f = h.font = font_new(ctx, &l, NULL);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT(r.image.pixels == d + SY_BM0);
    ASSERT_EQ_LL(r.image.stride, 4);
    ASSERT_EQ_LL(r.image.format, SHR_FORMAT_A8);
    for (int i = 0; i < 2; i++) { /* rows above the line, then a bitmap wholly below the baseline */
        font_free_now(ctx, f);
        synth(d);
        d[SY_G0 + 9] = (uint8_t)(i ? -1 : SY_BASE + 2);
        seal(d);
        f = h.font = font_new(ctx, &l, NULL);
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
        ASSERT_EQ_LL(r.offset.y, i ? SY_BASE + 1 : -2);
        ASSERT_EQ_LL(r.slant_axis, i ? CH - 2 * (SY_BASE + 1) : CH + 4);
    }
    harness_close(&h);
    PASS();
}

/* ===== Asynchronous loading ===== */

TEST test_deferred_reads(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .script = {SHR_IN_PROGRESS, SHR_IN_PROGRESS, SHR_IN_PROGRESS, SHR_IN_PROGRESS}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    uint64_t id = glyph1(f, 'A');
    shr__resolved r, fffd;
    ASSERT_EQ_LL(resolve(f, id, &r), SHR_OK);
    ASSERT(r.provisional && inside(&r, shr__builtin_package, shr__builtin_package_size)); /* the built-in 'A' */
    ASSERT_EQ_LL(resolve(f, glyph1(f, 0x2630), &r), SHR_OK);
    ASSERT_EQ_LL(resolve(f, glyph1(f, 0xFFFD), &fffd), SHR_OK);
    ASSERT(r.provisional && same_image(&r, &fffd)); /* not in the built-in package either: its U+FFFD */
    ASSERT(res->ops->has_work(res));
    shr_pump(ctx);
    for (int step = 0; step < 4; step++) { /* header, section table, index, page */
        ASSERT_EQ_LL(s.pending, 1);
        ASSERT(!res->ops->has_work(res)); /* nothing to do while the read is out */
        shr_deadline dl;
        shr_next_deadline(ctx, &dl);
        ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_NONE); /* a non-cancellable read has no watchdog */
        ts_complete(ctx, &s, SHR_OK);
        shr_pump(ctx);
        ASSERT_EQ_LL(resolve(f, id, &r), SHR_OK);
        ASSERT_EQ_LL(r.provisional, step < 3);
        if (step == 2) shr_pump(ctx); /* the page is wanted now */
    }
    ASSERT(r.image.height == 4);
    ASSERT_EQ_LL(s.calls, 4);
    harness_close(&h);
    PASS();
}

TEST test_read_retries_and_limit(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    /* The table read fails twice, the index and page reads once: each is retried after io_retry_ns. */
    tsrc s = {d, SY_SIZE, .script = {0, SHR_E_IO, SHR_E_IO, 0, SHR_E_IO, 0, SHR_E_IO}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, NULL);
    uint64_t id = glyph1(f, 'A');
    shr__resolved r;
    resolve(f, id, &r);
    settle(ctx);
    shr_deadline dl;
    shr_next_deadline(ctx, &dl);
    ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_AT);
    ASSERT_EQ_LL(dl.at_ns, 50 * MS);
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT(r.image.height == 4);
    ASSERT_EQ_LL(s.calls, 8);
    ASSERT_EQ_LL(last_failure(ctx), SHR_OK);
    font_free_now(ctx, f);

    /* Beyond io_retry_limit the package closes and its glyphs fall back, until a later frame wants it
     * after io_retry_ns. */
    tsrc bad = {d, SY_SIZE, .script = {SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
    l.src[ROLE_LATIN] = &bad;
    f = h.font = font_new(ctx, &l, NULL);
    resolve(f, id = glyph1(f, 'A'), &r);
    for (int i = 0; i < 8; i++) {
        settle(ctx);
        if (bad.calls == 4) break;
        fake_now += 50 * MS;
        resolve(f, id, &r);
    }
    resolve(f, id, &r);
    ASSERT(r.provisional && inside(&r, shr__builtin_package, shr__builtin_package_size)); /* redrawn later */
    ASSERT_EQ_LL(bad.calls, 4);
    ASSERT_EQ_LL(bad.closes, 1);
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_IO);
    resolve(f, id, &r);
    settle(ctx);
    int opens = l.opens;
    ASSERT(r.provisional && bad.calls == 4); /* no retry before io_retry_ns */
    fake_now += 50 * MS;
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT(!r.provisional && r.image.height == 4);
    ASSERT_EQ_LL(l.opens, opens + 1);

    /* A failing page read, the same way. */
    tsrc page = {d, SY_SIZE, .script = {0, 0, 0, SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
    l.src[ROLE_LATIN] = &page;
    shr_pl_res_bitmap_font *g = font_new(ctx, &l, NULL);
    resolve(g, id = glyph1(g, 'A'), &r);
    for (int i = 0; i < 8; i++) {
        settle(ctx);
        if (page.calls == 7) break;
        fake_now += 50 * MS;
        resolve(g, id, &r);
    }
    resolve(g, id, &r);
    ASSERT(r.provisional && r.image.height != 4);
    ASSERT_EQ_LL(page.calls, 7);
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_IO);
    shr_next_deadline(ctx, &dl);
    ASSERT(dl.kind == SHR_DEADLINE_AT && dl.at_ns == fake_now + 50 * MS); /* the end of the cool-down redraws */
    resolve(g, id, &r);
    settle(ctx);
    ASSERT_EQ_LL(page.calls, 7); /* not before io_retry_ns */
    fake_now += 50 * MS;
    ASSERT_EQ_LL(load(ctx, g, id, &r), SHR_OK);
    ASSERT(!r.provisional && r.image.height == 4);
    ASSERT_EQ_LL(page.calls, 8);
    font_free_now(ctx, g);
    harness_close(&h);
    PASS();
}

static void tweak_no_clock(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)drv;
    d->now_ns = NULL, d->io_retry_ns = 0, d->io_timeout_ns = 0;
}

TEST test_would_block_backs_off(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .script = {SHR_E_WOULD_BLOCK, 0, 0, 0, SHR_E_WOULD_BLOCK}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    uint64_t id = glyph1(f, 'A');
    shr__resolved r;
    resolve(f, id, &r);
    shr_pump(ctx);
    ASSERT_EQ_LL(s.calls, 1);
    shr_deadline dl;
    shr_next_deadline(ctx, &dl);
    ASSERT(dl.kind == SHR_DEADLINE_AT && dl.at_ns == 50 * MS); /* a full source queue backs off, no polling */
    fake_now = 50 * MS;
    settle(ctx);
    resolve(f, id, &r);
    shr_pump(ctx);
    ASSERT_EQ_LL(s.calls, 5);
    shr_next_deadline(ctx, &dl);
    ASSERT(dl.kind == SHR_DEADLINE_AT && dl.at_ns == 100 * MS);
    fake_now = 100 * MS;
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT(r.image.height == 4);
    ASSERT_EQ_LL(s.calls, 6);
    harness_close(&h);

    /* Without a clock a refused read waits for shr_asset_ready(), without polling. */
    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_no_clock);
    tsrc s1 = {d, SY_SIZE, .script = {SHR_E_WOULD_BLOCK, 0, 0, 0, SHR_E_WOULD_BLOCK}};
    lib l2 = {.src = {[ROLE_LATIN] = &s1}};
    f = h.font = font_new(ctx, &l2, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    resolve(f, id, &r);
    shr_pump(ctx);
    ASSERT_EQ_LL(s1.calls, 1);
    ASSERT(!res->ops->has_work(res));
    shr_next_deadline(ctx, &dl);
    ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_NONE);
    settle(ctx);
    ASSERT_EQ_LL(s1.calls, 1);
    ASSERT_EQ_LL(shr_asset_ready(ctx), SHR_OK);
    ASSERT(res->ops->has_work(res));
    settle(ctx);
    ASSERT_EQ_LL(f->pkg[ROLE_LATIN].state, PKG_READY);
    resolve(f, id, &r);
    shr_pump(ctx);
    ASSERT_EQ_LL(s1.calls, 5); /* the page read is refused, */
    settle(ctx);
    ASSERT_EQ_LL(s1.calls, 5);
    shr_asset_ready(ctx); /* then retried once */
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT(r.image.height == 4);
    ASSERT_EQ_LL(s1.calls, 6);
    harness_close(&h);
    PASS();
}

/* Without a clock a ready signal that arrives before any frame ran into the cool-down still ends it. */
TEST test_ready_signal_before_cooling_frame(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_no_clock);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .script = {SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    uint64_t id = glyph1(f, 'A');
    shr__resolved r;
    resolve(f, id, &r);
    settle(ctx);
    ASSERT_EQ_LL(s.calls, 4); /* cooling down */
    shr_asset_ready(ctx);
    shr_pump(ctx); /* no frame has run into the cool-down yet */
    resolve(f, id, &r);
    ASSERT(r.provisional);
    ASSERT(res->ops->has_work(res));
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT(!r.provisional && r.image.height == 4);
    harness_close(&h);
    PASS();
}

/* A source that keeps failing is given up after io_retry_limit cool-downs, until shr_asset_ready(). */
TEST test_failing_source_given_up(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .script = {SHR_E_IO}};
    for (int i = 0; i < 32; i++) s.script[i] = SHR_E_IO;
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    uint64_t id = glyph1(f, 'A');
    shr__resolved r;
    for (int i = 0; i < 64; i++) {
        resolve(f, id, &r);
        drain(ctx);
    }
    ASSERT_EQ_LL(s.calls, 16); /* four cool-downs of four failed opens */
    ASSERT(r.provisional);
    ASSERT(fake_now < UINT64_MAX);
    resolve(f, id, &r); /* runs into the cool-down */
    shr_deadline dl;
    shr_next_deadline(ctx, &dl);
    ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_NONE); /* time no longer retries */
    fake_now = UINT64_MAX;
    settle(ctx);
    ASSERT_EQ_LL(s.calls, 16);
    shr_asset_ready(ctx);
    for (int i = 0; i < 4; i++) {
        resolve(f, id, &r);
        drain(ctx);
    }
    ASSERT(s.calls > 16);
    harness_close(&h);
    PASS();
}

static void tweak_endless_retry(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)drv;
    d->io_retry_ns = UINT64_MAX;
}

TEST test_retry_time_saturates(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_endless_retry);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .script = {SHR_E_WOULD_BLOCK}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__resolved r;
    fake_now = 1;
    resolve(f, glyph1(f, 'A'), &r);
    shr_pump(ctx);
    ASSERT_EQ_LL(s.calls, 1);
    shr_deadline dl;
    shr_next_deadline(ctx, &dl);
    ASSERT(dl.kind == SHR_DEADLINE_AT && dl.at_ns == UINT64_MAX); /* neither wrapped to "due now" nor lost */
    shr_pump(ctx);
    ASSERT_EQ_LL(s.calls, 1);
    harness_close(&h);
    PASS();
}

/* A frame that first runs into a cool-down another one's ready signal already ended wakes the pump. */
TEST test_second_cooldown_after_signal(void) {
    for (int clock = 0; clock < 2; clock++) {
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, clock ? NULL : tweak_no_clock);
        uint8_t dl[SY_SIZE], dc[SY_SIZE];
        synth(dl);
        synth_as(dc, ROLE_CJK, "ko", 0x4E00, 0x4E01, SY_A4);
        tsrc sl = {dl, SY_SIZE, .script = {SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
        tsrc sc = {dc, SY_SIZE, .mode = SRC_READ};
        int given_up = clock ? 16 : 4; /* with a clock, only a given-up package waits for the signal */
        for (int i = 0; i < given_up; i++) sc.script[i] = SHR_E_IO;
        lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_CJK] = &sc}};
        shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
        shr__res *res = shr__bitmap_font_res(f);
        uint64_t ida = glyph1(f, 'A'), idc = glyph1(f, 0x4E00);
        shr__resolved r;
        for (int i = 0; i < 64 && sc.calls < given_up; i++) {
            resolve(f, idc, &r);
            settle(ctx);
            fake_now += 50 * MS;
        }
        ASSERT_EQ_LL(sc.calls, given_up);
        ASSERT(f->pkg[ROLE_CJK].retry.cooling);
        for (int i = 0; i < 8 && sl.calls < 4; i++) {
            fake_now += 50 * MS;
            resolve(f, ida, &r);
            settle(ctx);
        }
        ASSERT_EQ_LL(sl.calls, 4);
        resolve(f, ida, &r); /* runs into the latin cool-down */
        settle(ctx);
        shr_asset_ready(ctx);
        settle(ctx); /* the signal ends it */
        ASSERT_EQ_LL(load(ctx, f, ida, &r), SHR_OK);
        ASSERT(!r.provisional && r.image.height == 4);
        resolve(f, idc, &r); /* the first frame to run into the CJK cool-down, which is over */
        ASSERT(r.provisional);
        ASSERT(res->ops->has_work(res));
        ASSERT(res->ops->pump(res)); /* redraws, so the next frame tries it again */
        ASSERT(!res->ops->has_work(res));
        ASSERT_EQ_LL(load(ctx, f, idc, &r), SHR_OK);
        ASSERT(!r.provisional && r.image.height == 4);
        ASSERT(sc.calls > given_up);
        harness_close(&h);
    }
    PASS();
}

static void tweak_signal_only(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)drv;
    d->io_retry_ns = 0;
}

/* With a clock but no io_retry_ns, refused and cooling packages report no deadline: only a signal wakes them. */
TEST test_signal_only_has_no_deadline(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_signal_only);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .script = {SHR_E_WOULD_BLOCK, SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    uint64_t id = glyph1(f, 'A');
    shr__resolved r;
    resolve(f, id, &r);
    settle(ctx);
    ASSERT_EQ_LL(s.calls, 1);
    shr_deadline dl;
    shr_next_deadline(ctx, &dl);
    ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_NONE); /* refused */
    shr_asset_ready(ctx);
    ASSERT(res->ops->has_work(res));
    settle(ctx);
    ASSERT_EQ_LL(s.calls, 5);
    ASSERT(f->pkg[ROLE_LATIN].retry.cooling);
    resolve(f, id, &r); /* runs into the cool-down */
    settle(ctx);
    shr_next_deadline(ctx, &dl);
    ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_NONE); /* cooling */
    fake_now = UINT64_MAX;
    ASSERT(!res->ops->has_work(res));
    shr_asset_ready(ctx);
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT(!r.provisional && r.image.height == 4);
    harness_close(&h);
    PASS();
}

/* Failed reads count for the package until it is ready, not per load step. */
TEST test_package_failures_count_until_ready(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .script = {SHR_E_IO, SHR_E_IO, 0, SHR_E_IO, SHR_E_IO}}; /* header, then table */
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__resolved r;
    resolve(f, glyph1(f, 'A'), &r);
    drain(ctx);
    ASSERT_EQ_LL(s.calls, 5);
    ASSERT(f->pkg[ROLE_LATIN].retry.cooling);
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_IO);
    resolve(f, glyph1(f, 'A'), &r);
    ASSERT(r.provisional && inside(&r, shr__builtin_package, shr__builtin_package_size));
    harness_close(&h);
    PASS();
}

/* A package not installed stays absent, also after shr_asset_ready(); a new font finds it once installed. */
TEST test_absent_package_stays_absent(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .open_result = SHR_OK};
    lib l = {0};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    uint64_t id = glyph1(f, 'A');
    shr__resolved r;
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT(!r.provisional && inside(&r, shr__builtin_package, shr__builtin_package_size));
    ASSERT_EQ_LL(f->pkg[ROLE_LATIN].state, PKG_ABSENT);
    l.src[ROLE_LATIN] = &s; /* installed now */
    int opens = l.opens;
    shr_asset_ready(ctx);
    ASSERT(!res->ops->has_work(res));
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT_EQ_LL(l.opens, opens);
    ASSERT(!r.provisional && inside(&r, shr__builtin_package, shr__builtin_package_size));
    font_free_now(ctx, f);
    f = h.font = font_new(ctx, &l, NULL);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT(!r.provisional && r.image.height == 4);
    harness_close(&h);
    PASS();
}

static void tweak_one_read(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)drv;
    d->max_reads = 1;
}

TEST test_read_slots_exhausted(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_one_read);
    uint8_t d[SY_SIZE];
    synth(d);
    uint8_t k[SY_SIZE];
    synth_as(k, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    tsrc a = {k, SY_SIZE, .script = {SHR_IN_PROGRESS}}, c = {d, SY_SIZE, .script = {[3] = SHR_IN_PROGRESS}};
    lib l = {.src = {[ROLE_LATIN] = &c, [ROLE_CJK] = &a}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    shr__resolved r;
    resolve(f, glyph1(f, 'A'), &r);
    settle(ctx);
    resolve(f, glyph1(f, 'A'), &r);
    settle(ctx);
    ASSERT_EQ_LL(c.pending, 1); /* the latin page read holds the only slot */
    resolve(f, glyph1(f, 0xAC00), &r);
    shr_pump(ctx);
    ASSERT_EQ_LL(a.calls, 0);
    ASSERT(!res->ops->has_work(res)); /* blocked until a read ends */
    ts_complete(ctx, &c, SHR_OK);
    shr_pump(ctx);
    ASSERT_EQ_LL(a.pending, 1);
    resolve(f, glyph2(f), &r); /* a page read finds no free slot */
    shr_pump(ctx);
    ASSERT_EQ_LL(c.calls, 4);
    ASSERT(!res->ops->has_work(res));
    ts_complete(ctx, &a, SHR_OK);
    ASSERT_EQ_LL(load(ctx, f, glyph2(f), &r), SHR_OK);
    ASSERT(!r.provisional && r.image.height == 4);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &r), SHR_OK);
    ASSERT_EQ_LL(r.image.height, 4);
    harness_close(&h);
    PASS();
}

TEST test_read_timeout(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .cancellable = true};
    for (int i = 0; i < 4; i++) s.script[i] = SHR_IN_PROGRESS;
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__resolved r;
    resolve(f, glyph1(f, 'A'), &r);
    shr_pump(ctx);
    shr_deadline dl;
    shr_next_deadline(ctx, &dl);
    ASSERT(dl.kind == SHR_DEADLINE_AT && dl.at_ns == 1000 * MS); /* the compositor's watchdog */
    drain(ctx);
    ASSERT_EQ_LL(s.cancels, 4); /* the first read and three retries */
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_TIMEOUT);
    ASSERT_EQ_LL(resolve(f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT(r.provisional && inside(&r, shr__builtin_package, shr__builtin_package_size)); /* cooling down */
    harness_close(&h);
    PASS();
}

TEST test_open_failures(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .open_result = SHR_E_IO};
    ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_IO);
    ASSERT_EQ_LL(s.closes, 0); /* nothing was opened */
    s = (tsrc){d, SY_SIZE, .mode = SRC_NONE};
    ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(s.closes, 1);
    harness_close(&h);
    PASS();
}

/* A failed package lets go of its buffers and source at once; the font keeps working. */
TEST test_failed_package_released(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    put(d + SY_PG(1) + 24, 2, 4); /* glyphs not covered: fails after the index was read */
    seal(d);
    tsrc s = {d, SY_SIZE, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__resolved r;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_FORMAT);
    shr__pkg *pkg = &f->pkg[ROLE_LATIN];
    ASSERT(pkg->state == PKG_FAILED && !pkg->buf && !pkg->pages && !pkg->src.read);
    ASSERT_EQ_LL(s.closes, 1);
    int opens = l.opens;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK); /* never reopened */
    ASSERT_EQ_LL(l.opens, opens);
    harness_close(&h);
    ASSERT_EQ_LL(s.closes, 1);
    PASS();
}

/* Frames never call open() or verify anything: that waits for the pump. */
TEST test_resolve_defers_open_and_checks(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    d[SY_BM0 + 4] ^= 0xFF; /* the first page is damaged */
    tsrc s = {d, SY_SIZE, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__resolved r;
    uint64_t id = glyph1(f, 'A');
    ASSERT_EQ_LL(resolve(f, id, &r), SHR_OK);
    ASSERT(r.provisional && l.opens == 0);
    shr_pump(ctx);
    ASSERT_EQ_LL(l.opens, 1);
    ASSERT_EQ_LL(f->pkg[ROLE_LATIN].state, PKG_READY);
    ASSERT_EQ_LL(resolve(f, id, &r), SHR_OK);
    ASSERT(r.provisional && last_failure(ctx) == SHR_OK); /* the page is checked by the pump */
    shr_pump(ctx);
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_CHECKSUM);
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT(!r.provisional && inside(&r, shr__builtin_package, shr__builtin_package_size));
    ASSERT_EQ_LL(load(ctx, f, glyph2(f), &r), SHR_OK);
    ASSERT(inside(&r, d, SY_SIZE)); /* the second page is fine */
    harness_close(&h);
    PASS();
}

TEST test_cjk_locale(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t ko[SY_SIZE], ja[SY_SIZE];
    synth_as(ko, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    synth_as(ja, ROLE_CJK, "ja", 'A', 0xAC00, SY_A4);
    tsrc sko = {ko, SY_SIZE, .mode = SRC_MAP}, sja = {ja, SY_SIZE, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_CJK] = &sko}};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, "ja");
    shr__resolved r;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &r), SHR_OK);
    ASSERT_STR_EQ(l.first, "shiroko-cjk-ja.shrf");
    ASSERT(!inside(&r, ko, SY_SIZE));
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_FORMAT); /* a Korean package under the Japanese name */
    font_free_now(ctx, f);
    l.src[ROLE_CJK] = &sja;
    f = font_new(ctx, &l, "ja");
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &r), SHR_OK);
    ASSERT(inside(&r, ja, SY_SIZE));
    font_free_now(ctx, f);
    l.src[ROLE_CJK] = &sko;
    l.opens = 0;
    f = h.font = font_new(ctx, &l, NULL); /* Korean by default */
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &r), SHR_OK);
    ASSERT_STR_EQ(l.first, "shiroko-cjk-ko.shrf");
    ASSERT(inside(&r, ko, SY_SIZE));
    harness_close(&h);
    PASS();
}

/* ===== Routing between roles ===== */

TEST test_routing(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t latin[SY_SIZE], cjk[SY_SIZE], latin2[SY_SIZE], nerd[SY_SIZE], sym[SY_SIZE];
    synth_as(latin, ROLE_LATIN, "", 'A', 0xAC00, SY_A4);
    synth_as(cjk, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    synth_as(latin2, ROLE_LATIN, "", 'A', 0xE0B0, SY_A4);
    synth_as(nerd, ROLE_NERD, "", 0xE0B0, 0xE0B1, SY_A4);
    synth_as(sym, ROLE_SYMBOLS, "", 0x2630, 0xAC01, SY_A4);
    tsrc s[5];
    uint8_t *data[5] = {latin, cjk, latin2, nerd, sym};
    for (int i = 0; i < 5; i++) s[i] = (tsrc){data[i], SY_SIZE, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_LATIN] = &s[0], [ROLE_CJK] = &s[1]}};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, NULL);
    shr__resolved r;
    load(ctx, f, glyph1(f, 'A'), &r);
    ASSERT(inside(&r, latin, SY_SIZE)); /* latin first */
    load(ctx, f, glyph1(f, 0xAC00), &r);
    ASSERT(inside(&r, cjk, SY_SIZE)); /* CJK scalars: CJK first */
    ASSERT_EQ_LL(r.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC); /* text: bold and italic are drawn */
    font_free_now(ctx, f);

    lib l2 = {.src = {[ROLE_LATIN] = &s[2], [ROLE_NERD] = &s[3], [ROLE_SYMBOLS] = &s[4]}};
    f = h.font = font_new(ctx, &l2, NULL);
    load(ctx, f, glyph1(f, 0xE0B0), &r);
    ASSERT(inside(&r, nerd, SY_SIZE)); /* Nerd private use: the Nerd package first */
    ASSERT_EQ_LL(r.synth, 0); /* icons are drawn as they are */
    load(ctx, f, glyph1(f, 0x2630), &r);
    ASSERT(inside(&r, sym, SY_SIZE));
    ASSERT_EQ_LL(r.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
    load(ctx, f, glyph1(f, 0xAC01), &r);
    ASSERT(inside(&r, sym, SY_SIZE)); /* after CJK and latin */
    load(ctx, f, glyph1(f, 'A'), &r);
    ASSERT(inside(&r, latin2, SY_SIZE));
    harness_close(&h);
    PASS();
}

TEST test_emoji_fallbacks(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t latin[SY_SIZE], emoji[SY_SIZE], sym[SY_SIZE];
    synth_as(latin, ROLE_LATIN, "", 0x2764, 0x2765, SY_A4);
    synth_as(emoji, ROLE_EMOJI, "", 0x1F600, 0x1F601, SY_A4);
    synth_as(sym, ROLE_SYMBOLS, "", 0x1F600, 0x1F602, SY_A4);
    tsrc sl = {latin, SY_SIZE, .mode = SRC_MAP}, se = {emoji, SY_SIZE, .mode = SRC_MAP},
         ss = {sym, SY_SIZE, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_EMOJI] = &se}};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, NULL);
    shr__resolved r, fffd;
    uint32_t heart[2] = {0x2764, 0xFE0F}, smile_zw[2] = {0x1F600, 0x200B}, smile_vs[2] = {0x1F600, 0xFE0F},
             zwj[3] = {0x1F469, 0x200D, 0x1F4BB};
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xFFFD), &fffd), SHR_OK);
    load(ctx, f, glyph1(f, 0x1F600), &r);
    ASSERT(inside(&r, emoji, SY_SIZE));
    ASSERT_EQ_LL(r.synth, 0);
    load(ctx, f, glyph(f, heart, 2), &r);
    ASSERT(inside(&r, latin, SY_SIZE)); /* registered VS16 base without an emoji glyph: its text glyph */
    ASSERT_EQ_LL(r.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC); /* styled like text */
    load(ctx, f, glyph(f, smile_zw, 2), &r);
    ASSERT(inside(&r, emoji, SY_SIZE)); /* one visible emoji scalar: its emoji glyph */
    load(ctx, f, glyph(f, smile_vs, 2), &r);
    ASSERT(inside(&r, emoji, SY_SIZE)); /* VS16 on a base without variation sequences: the same */
    load(ctx, f, glyph(f, zwj, 3), &r);
    ASSERT(same_image(&r, &fffd)); /* unknown sequence */
    load(ctx, f, glyph1(f, 0x1F602), &r);
    ASSERT(!r.provisional && same_image(&r, &fffd)); /* in no package */
    font_free_now(ctx, f);

    l.src[ROLE_SYMBOLS] = &ss; /* an emoji scalar the emoji package lacks: the text chain */
    f = font_new(ctx, &l, NULL);
    load(ctx, f, glyph1(f, 0x1F602), &r);
    ASSERT(!r.provisional && inside(&r, sym, SY_SIZE));
    ASSERT_EQ_LL(r.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
    load(ctx, f, glyph1(f, 0x1F600), &r);
    ASSERT(inside(&r, emoji, SY_SIZE));
    font_free_now(ctx, f);

    l.src[ROLE_EMOJI] = NULL; /* not installed */
    f = font_new(ctx, &l, NULL);
    load(ctx, f, glyph1(f, 0x1F600), &r);
    ASSERT(!r.provisional && inside(&r, sym, SY_SIZE));
    load(ctx, f, glyph(f, smile_vs, 2), &r);
    ASSERT(!r.provisional && inside(&r, sym, SY_SIZE));
    load(ctx, f, glyph1(f, 0x1F601), &r);
    ASSERT(!r.provisional && same_image(&r, &fffd));
    font_free_now(ctx, f);

    tsrc pending = {emoji, SY_SIZE, .script = {SHR_IN_PROGRESS}};
    l.src[ROLE_EMOJI] = &pending;
    f = h.font = font_new(ctx, &l, NULL);
    resolve(f, glyph1(f, 0x1F600), &r);
    shr_pump(ctx);
    resolve(f, glyph(f, heart, 2), &r);
    ASSERT(r.provisional && same_image(&r, &fffd)); /* loading emoji: provisional U+FFFD, not a text fallback */
    ASSERT_EQ_LL(r.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC); /* the built-in fallback is text */
    ts_complete(ctx, &pending, SHR_OK);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0x1F600), &r), SHR_OK);
    ASSERT(!r.provisional && r.image.height == 4);
    harness_close(&h);
    PASS();
}

TEST test_deadline_is_the_earliest_retry(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t latin[SY_SIZE], cjk[SY_SIZE], emoji[SY_SIZE];
    synth(latin);
    synth_as(cjk, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    synth_as(emoji, ROLE_EMOJI, "", 0x1F600, 0x1F601, SY_A4);
    const shr_status wb = SHR_E_WOULD_BLOCK;
    tsrc sl = {latin, SY_SIZE, .script = {wb, 0, 0, 0, wb, wb}}, sc = {cjk, SY_SIZE, .script = {wb, 0, 0, 0, wb}},
         se = {emoji, SY_SIZE, .script = {wb}};
    lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_CJK] = &sc, [ROLE_EMOJI] = &se}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    const uint32_t cps[3] = {0xAC00, 'A', 0x1F600}; /* packages back off until 50, 60 and 70 ms */
    shr__resolved r;
    for (int i = 0; i < 3; i++) {
        fake_now = 10 * MS * (uint64_t)i;
        resolve(f, glyph1(f, cps[i]), &r);
        shr_pump(ctx);
    }
    ASSERT_EQ_LL(res->ops->deadline(res), 50 * MS);
    fake_now = 100 * MS;
    settle(ctx);
    const uint64_t ids[3] = {glyph2(f), glyph1(f, 'A'), glyph1(f, 0xAC00)};
    for (int i = 0; i < 3; i++) { /* pages of one frame back off until 150, 160 and 170 ms */
        fake_now = 100 * MS + 10 * MS * (uint64_t)i;
        res->ops->resolve(res, ids[i], frame_no + 1, &r);
        shr_pump(ctx);
    }
    res->ops->frame_end(res, ++frame_no);
    ASSERT_EQ_LL(res->ops->deadline(res), 150 * MS);
    ASSERT_EQ_LL(sl.calls + sc.calls + se.calls, 6 + 5 + 4);
    harness_close(&h);
    PASS();
}

/* ===== Page cache ===== */

TEST test_page_cache_budget(void) {
    uint8_t d[SY_SIZE];
    synth(d); /* pages of 52 and 36 bytes */
    harness h;
    cache_limit = 80;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    tsrc s = {d, SY_SIZE, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    uint64_t first = glyph1(f, 'A'), second = glyph2(f);
    shr__resolved r;
    ASSERT_EQ_LL(load(ctx, f, first, &r), SHR_OK);
    uint64_t frame = ++frame_no;
    ASSERT_EQ_LL(res->ops->resolve(res, first, frame, &r), SHR_OK); /* pinned by the frame */
    ASSERT_EQ_LL(res->ops->resolve(res, second, frame, &r), SHR_OK);
    ASSERT(r.provisional);
    settle(ctx);
    ASSERT_EQ_LL(f->page_bytes, 52); /* no room while the first page is pinned */
    shr_deadline dl;
    shr_next_deadline(ctx, &dl);
    ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_NONE); /* waits for the unpin, no polling */
    res->ops->frame_end(res, frame);
    ASSERT(res->ops->has_work(res));
    settle(ctx);
    ASSERT_EQ_LL(f->page_bytes, 36); /* the first page was evicted, its descriptor freed */
    ASSERT(f->pkg[ROLE_LATIN].pages[0] == NULL);
    ASSERT_EQ_LL(load(ctx, f, second, &r), SHR_OK);
    ASSERT_EQ_LL(r.offset.y, 3);
    ASSERT_EQ_LL(load(ctx, f, first, &r), SHR_OK); /* loaded again */
    ASSERT_EQ_LL(r.offset.y, 0);
    ASSERT_EQ_LL(f->page_bytes, 52);
    harness_close(&h);

    cache_limit = 50; /* a page larger than the whole cache fails */
    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    f = h.font = font_new(ctx, &l, NULL);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT(!r.provisional && r.image.height != 4);
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_LIMIT);
    harness_close(&h);
    PASS();
}

TEST test_page_lru_order(void) {
    uint8_t latin[SY_SIZE], cjk[SY_SIZE];
    synth(latin);
    synth_as(cjk, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    harness h;
    cache_limit = 52 + 36 + 35;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    tsrc sl = {latin, SY_SIZE, .mode = SRC_READ}, sc = {cjk, SY_SIZE, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_CJK] = &sc}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__resolved r;
    uint64_t first = glyph1(f, 'A');
    load(ctx, f, first, &r);
    load(ctx, f, glyph2(f), &r);
    resolve(f, first, &r); /* used again: now the newest */
    ASSERT_EQ_LL(f->page_bytes, 52 + 36);
    load(ctx, f, glyph1(f, 0xAC00), &r);
    ASSERT(r.image.height == 4);
    shr__page **pages = f->pkg[ROLE_LATIN].pages;
    ASSERT_EQ_LL(pages[0]->state, PAGE_READY);
    ASSERT(pages[1] == NULL); /* the least recently used */
    ASSERT_EQ_LL(f->page_bytes, 52 + 36);
    harness_close(&h);
    PASS();
}

/* Only pages the latest frame wanted are loaded; descriptors exist only for pages in use. */
TEST test_stale_wants_dropped(void) {
    uint8_t d[SY_SIZE];
    synth(d);
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    tsrc s = {d, SY_SIZE, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    uint64_t first = glyph1(f, 'A'), second = glyph2(f);
    shr__resolved r;
    resolve(f, first, &r);
    settle(ctx);
    ASSERT_EQ_LL(s.calls, 3);
    resolve(f, first, &r);
    resolve(f, second, &r); /* the next frame no longer shows the first glyph */
    ASSERT_EQ_LL(f->wants.len, 2);
    ASSERT(res->ops->has_work(res));
    settle(ctx);
    ASSERT_EQ_LL(s.calls, 4);
    ASSERT(f->pkg[ROLE_LATIN].pages[0] == NULL && f->pkg[ROLE_LATIN].pages[1]->state == PAGE_READY);
    ASSERT_EQ_LL(f->wants.len, 0);
    ASSERT(!res->ops->has_work(res));

    /* A page whose read failed is not retried once no frame wants it, and forgotten. */
    s.script[4] = SHR_E_IO;
    resolve(f, first, &r);
    settle(ctx);
    ASSERT_EQ_LL(s.calls, 5);
    ASSERT_EQ_LL(f->pkg[ROLE_LATIN].pages[0]->retry.count, 1);
    resolve(f, second, &r);
    fake_now += 50 * MS;
    settle(ctx);
    ASSERT_EQ_LL(s.calls, 5);
    ASSERT(f->pkg[ROLE_LATIN].pages[0] == NULL);
    ASSERT_EQ_LL(load(ctx, f, first, &r), SHR_OK);
    ASSERT_EQ_LL(r.image.height, 4);
    harness_close(&h);
    PASS();
}

/* A page that became ready and is evicted before the pump drops it from the wants. */
TEST test_evict_wanted_page(void) {
    uint8_t d[SY_SIZE];
    synth(d);
    harness h;
    cache_limit = 80;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    tsrc s = {d, SY_SIZE, .script = {[3] = SHR_E_WOULD_BLOCK, [4] = SHR_IN_PROGRESS}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    const uint64_t ids[2] = {glyph1(f, 'A'), glyph2(f)};
    shr__resolved r;
    resolve(f, ids[0], &r);
    settle(ctx);
    resolve_all(f, ids, 2);
    shr_pump(ctx); /* the first page backs off, the second is read */
    ASSERT_EQ_LL(s.pending, 1);
    fake_now = 50 * MS;
    ts_complete(ctx, &s, SHR_OK);
    shr_pump(ctx); /* the second becomes ready, the first evicts it, then the reverse: each is read once */
    shr__page **pages = f->pkg[ROLE_LATIN].pages;
    ASSERT_EQ_LL(s.calls, 7);
    ASSERT(!pages[0] && pages[1]->state == PAGE_READY && f->wants.len == 0);
    harness_close(&h);
    PASS();
}

/* Packages open in the pump even while every read slot is taken. */
TEST test_open_while_blocked(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_one_read);
    uint8_t d[SY_SIZE], k[SY_SIZE], n[SY_SIZE];
    synth(d);
    synth_as(k, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    synth_as(n, ROLE_NERD, "", 0xE0B0, 0xE0B1, SY_A4);
    tsrc sl = {d, SY_SIZE, .script = {SHR_IN_PROGRESS}}, sc = {k, SY_SIZE, .mode = SRC_READ}, sn = {n, SY_SIZE, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_CJK] = &sc, [ROLE_NERD] = &sn}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    const uint64_t ids[3] = {glyph1(f, 'A'), glyph1(f, 0xAC00), glyph1(f, 0xE0B0)};
    resolve_all(f, ids, 3);
    shr_pump(ctx);
    ASSERT_EQ_LL(l.opens, 3);
    ASSERT(sl.calls == 1 && sc.calls == 0 && sn.calls == 0);
    ASSERT_EQ_LL(f->pkg[ROLE_NERD].state, PKG_LOADING);
    ts_complete(ctx, &sl, SHR_OK);
    shr__resolved r;
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_LL(load(ctx, f, ids[i], &r), SHR_OK);
        ASSERT_EQ_LL(r.image.height, 4);
    }
    harness_close(&h);
    PASS();
}

TEST test_pins_per_frame(void) {
    uint8_t d[SY_SIZE];
    synth(d);
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    tsrc s = {d, SY_SIZE, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    uint64_t a = glyph1(f, 'A'), second = glyph2(f);
    uint32_t sel[2] = {'A', 0xFE0E};
    uint64_t sa = glyph(f, sel, 2); /* another id for the same glyph */
    shr__resolved r;
    load(ctx, f, a, &r);
    load(ctx, f, second, &r);
    ASSERT_EQ_LL(load(ctx, f, sa, &r), SHR_OK); /* once the other packages are known to be absent */
    ASSERT(!r.provisional && r.image.height == 4);
    shr__page *p = f->pkg[ROLE_LATIN].pages[0], *q = f->pkg[ROLE_LATIN].pages[1];
    for (int i = 0; i < 4; i++) res->ops->resolve(res, i % 2 ? sa : a, 5, &r); /* one stamp per frame */
    res->ops->resolve(res, a, 6, &r);
    res->ops->resolve(res, second, 6, &r);
    res->ops->resolve(res, sa, 6, &r);
    ASSERT(p->pin[0] == 5 && p->pin[1] == 6 && q->pin[0] == 6 && q->pin[1] == 0);
    ASSERT_EQ_LL(f->lru_bytes, 0);
    ASSERT_EQ_LL(res->ops->resolve(res, a, 7, &r), SHR_E_LIMIT); /* a third frame in flight */
    res->ops->frame_end(res, 5);
    ASSERT(p->pin[0] == 0 && p->pin[1] == 6);
    ASSERT_EQ_LL(f->lru_bytes, 0);
    ASSERT_EQ_LL(res->ops->resolve(res, a, 7, &r), SHR_OK);
    ASSERT_EQ_LL(p->pin[0], 7);
    res->ops->frame_end(res, 6);
    ASSERT_EQ_LL(f->lru_bytes, 36);
    res->ops->frame_end(res, 7);
    ASSERT(f->pinned == NULL);
    ASSERT_EQ_LL(f->lru_bytes, 52 + 36);
    harness_close(&h);
    PASS();
}

/* ===== Destroy and shutdown with reads outstanding ===== */

TEST test_destroy_with_outstanding_reads(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    for (int cancellable = 0; cancellable < 2; cancellable++) {
        tsrc s = {d, SY_SIZE, .cancellable = cancellable, .script = {0, 0, 0, SHR_IN_PROGRESS}};
        lib l = {.src = {[ROLE_LATIN] = &s}};
        shr_pl_res_bitmap_font *f = font_new(ctx, &l, NULL);
        shr__resolved r;
        for (int i = 0; i < 2; i++) {
            resolve(f, glyph1(f, 'A'), &r);
            settle(ctx);
        }
        ASSERT_EQ_LL(s.pending, 1); /* the page read */
        ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(f), SHR_OK);
        ASSERT_EQ_LL(s.cancels, cancellable);
        shr_deadline dl;
        ASSERT_EQ_LL(shr_next_deadline(ctx, &dl), SHR_OK); /* a destroyed font has no work */
        shr_pump(ctx);
        ASSERT_EQ_LL(s.closes, cancellable); /* freed once no read can write any more */
        if (!cancellable) {
            ts_complete(ctx, &s, SHR_OK);
            shr_pump(ctx);
            ASSERT_EQ_LL(s.closes, 1);
        }
    }
    harness_close(&h);
    PASS();
}

TEST test_shutdown_with_outstanding_reads(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .cancellable = true, .script = {SHR_IN_PROGRESS}};
    tsrc u = {d, SY_SIZE, .script = {0, 0, 0, SHR_IN_PROGRESS}};
    lib l = {.src = {[ROLE_LATIN] = &u, [ROLE_CJK] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    shr__resolved r;
    resolve(f, glyph1(f, 'A'), &r);
    settle(ctx);
    const uint64_t both[2] = {glyph1(f, 'A'), glyph1(f, 0xAC00)};
    resolve_all(f, both, 2);
    settle(ctx);
    ASSERT_EQ_LL(u.pending + s.pending, 2); /* a page read and a load step */
    resolve(f, glyph2(f), &r); /* wanted, never scheduled */
    s.destroy_on_cancel = f;
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(s.cancels, 1);
    ASSERT_EQ_LL(s.destroyed, SHR_E_STATE); /* host callbacks are refused while shutting down too */
    ASSERT(!res->dead);
    ASSERT(!res->ops->has_work(res));
    ASSERT_EQ_LL(res->ops->deadline(res), 0);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_E_WOULD_BLOCK);
    ASSERT_EQ_LL(resolve(f, glyph1(f, 0xE0B0), &r), SHR_OK); /* no package opens any more */
    ASSERT_EQ_LL(l.opens, 2);
    ts_complete(ctx, &u, SHR_OK);
    shr_pump(ctx);
    ASSERT_EQ_LL(u.calls, 4);
    ASSERT_EQ_LL(f->pkg[ROLE_LATIN].pages[0]->state, PAGE_LOADING); /* not collected: freed with the font */
    harness_close(&h);
    ASSERT_EQ_LL(u.closes + s.closes, 2);
    PASS();
}

/* ===== Through frames ===== */

static int lit(const uint8_t *buf, int x0, int x1, int y0, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) n += px(buf, x, y) != 0;
    return n;
}

TEST test_provisional_redrawn_when_ready(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    tsrc s = {d, SY_SIZE, .mode = SRC_READ};
    for (int i = 0; i < 4; i++) s.script[i] = SHR_IN_PROGRESS;
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr_lyr *layer;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &layer), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, f, 1, 2, NULL), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(layer, 0, 0, "A", 1, 1, (shr_text_style){SHR_RGB(255, 255, 255), 0, 0}),
                 SHR_OK);
    shr_submit(ctx);
    settle(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    int first = lit(h.out.shown, 0, CW, 0, CH);
    ASSERT(first > 0 && lit(h.out.shown, 1, 5, 0, 4) != 16); /* the built-in 'A' */
    while (s.pending) {
        ts_complete(ctx, &s, SHR_OK);
        settle(ctx);
    }
    ASSERT(h.out.presents > 1); /* redrawn by itself */
    ASSERT_EQ_LL(lit(h.out.shown, 0, CW, 0, CH), 16);
    ASSERT_EQ_LL(lit(h.out.shown, 1, 5, 0, 4), 16);
    ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
    harness_close(&h);
    PASS();
}

static shr_framebuffer_driver cached_driver;
static void tweak_cached(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)d;
    shr_software_driver_create(NULL, 1u << 20, &cached_driver);
    *drv = cached_driver;
}

/* A package that cools down after I/O errors draws the fallback provisionally, outside the row cache, and
 * the real glyph appears by itself once the cool-down ends: after io_retry_ns, or at shr_asset_ready(). */
TEST test_cooled_package_redrawn(void) {
    for (int by_signal = 0; by_signal < 2; by_signal++) {
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cached);
        uint8_t d[SY_SIZE];
        synth(d);
        tsrc s = {d, SY_SIZE, .script = {SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
        lib l = {.src = {[ROLE_LATIN] = &s}};
        shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
        shr_lyr *layer;
        const shr_color bg = SHR_RGB(0, 0, 0);
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &layer), SHR_OK);
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, f, 1, 2, &bg), SHR_OK); /* an opaque, cacheable row */
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(layer, 0, 0, "A", 1, 1, (shr_text_style){SHR_RGB(255, 255, 255), 0, 0}),
                     SHR_OK);
        shr_submit(ctx);
        for (int i = 0; i < 8; i++) {
            settle(ctx);
            if (s.calls == 4) break;
            fake_now += 50 * MS;
        }
        ASSERT_EQ_LL(s.calls, 4);
        ASSERT_EQ_LL(last_failure(ctx), SHR_E_IO);
        ASSERT(lit(h.out.shown, 1, 5, 0, 4) != 16); /* the built-in 'A' */
        int presents = h.out.presents;
        shr_deadline dl;
        shr_next_deadline(ctx, &dl);
        ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_AT); /* the end of the cool-down */
        if (by_signal)
            shr_asset_ready(ctx);
        else
            fake_now = dl.at_ns;
        ASSERT(shr__bitmap_font_res(f)->ops->has_work(shr__bitmap_font_res(f)));
        for (int i = 0; i < 8 && lit(h.out.shown, 1, 5, 0, 4) != 16; i++) settle(ctx);
        ASSERT(h.out.presents > presents); /* redrawn by itself */
        ASSERT_EQ_LL(lit(h.out.shown, 1, 5, 0, 4), 16); /* reopened and read */
        ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
        harness_close(&h);
        ASSERT_EQ_LL(shr_software_driver_destroy(&cached_driver), SHR_OK);
    }
    PASS();
}

TEST test_failed_page_event_in_frames(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    uint8_t d[SY_SIZE];
    synth(d);
    d[SY_BM0 + 4] ^= 0xFF;
    tsrc s = {d, SY_SIZE, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr_lyr *layer;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &layer), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, f, 1, 1, NULL), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(layer, 0, 0, "A", 1, 1, (shr_text_style){SHR_RGB(255, 255, 255), 0, 0}),
                 SHR_OK);
    shr_submit(ctx);
    settle(ctx);
    shr_event ev;
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_RESOURCE_FAILED, &ev), 1);
    ASSERT_EQ_LL(ev.status, SHR_E_CHECKSUM);
    ASSERT(lit(h.out.shown, 0, CW, 0, CH) > 0); /* the built-in 'A' */
    ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
    harness_close(&h);
    PASS();
}

/* ===== Real baked packages ===== */

typedef struct blob {
    uint8_t *data;
    size_t size;
} blob;

static blob read_package(const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/shiroko-%s.shrf", SHR_FONT_DIR, name);
    blob b = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    b.size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b.data = malloc(b.size);
    if (fread(b.data, 1, b.size, f) != b.size) b.size = 0;
    fclose(f);
    return b;
}

static shr_status real_load(shr_context *ctx, shr_pl_res_bitmap_font *f, const uint32_t *cps, size_t n,
                            shr__resolved *out) {
    return load(ctx, f, glyph(f, cps, n), out);
}

/* ppem_26_6 of the package's instance */
static uint32_t package_ppem(const char *name) {
    blob b = read_package(name);
    uint32_t ppem = 0;
    for (uint32_t i = 0; b.size >= 128 && i < shr__rd32(b.data + 16); i++)
        if (shr__rd32(b.data + 128 + 32 * i) == 4) ppem = shr__rd32(b.data + shr__rd64(b.data + 128 + 32 * i + 8) + 8);
    free(b.data);
    return ppem;
}

TEST test_real_packages(void) {
    uint32_t cjk = package_ppem("cjk-ko"), latin = package_ppem("latin");
    ASSERT(latin > 0 && cjk <= latin && 5 * cjk >= 4 * latin); /* CJK at the latin em, or a little smaller */
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    shr_pl_res_bitmap_font *f = harness_font(&h);
    uint32_t fffd = 0xFFFD, lvt[3] = {0x1100, 0x1161, 0x11A8}, lv_t[2] = {0xAC00, 0x11A8}, gak = 0xAC01,
             nfd[2] = {'e', 0x301}, nfc = 0xE9, unq[3] = {0x1F441, 0x200D, 0x1F5E8},
             full[5] = {0x1F441, 0xFE0F, 0x200D, 0x1F5E8, 0xFE0F}, sel[2] = {0x1F600, 0xFE00}, smile = 0x1F600;
    const uint32_t singles[] = {0xAC00, 0x1F600, 0xE0B0, 0x2630, 0x6F22, 0x2588};
    const uint32_t synth[] = {SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC, 0, 0, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC, 0};
    shr__resolved a, b;
    ASSERT_EQ_LL(real_load(ctx, f, &fffd, 1, &a), SHR_OK);
    for (size_t i = 0; i < sizeof(singles) / sizeof(singles[0]); i++) {
        ASSERT_EQ_LL(real_load(ctx, f, &singles[i], 1, &b), SHR_OK);
        ASSERT(!b.provisional && !same_image(&a, &b)); /* Hangul, emoji, Nerd, symbols, Han, a block */
        ASSERT_EQ_LL(b.synth, synth[i]);
        if (singles[i] == 0xAC00 || singles[i] == 0x6F22) /* inside their two cells */
            ASSERT(b.offset.x >= 0 && b.offset.x + b.image.width <= 2 * CW && b.offset.y >= 0 &&
                   b.offset.y + b.image.height <= CH);
    }
    const struct {
        const uint32_t *a, *b;
        size_t an, bn;
    } alias[] = {{lvt, &gak, 3, 1}, {lv_t, &gak, 2, 1}, {nfd, &nfc, 2, 1}, {unq, full, 3, 5}, {sel, &smile, 2, 1}};
    for (size_t i = 0; i < sizeof(alias) / sizeof(alias[0]); i++) {
        ASSERT_EQ_LL(real_load(ctx, f, alias[i].a, alias[i].an, &a), SHR_OK);
        ASSERT_EQ_LL(real_load(ctx, f, alias[i].b, alias[i].bn, &b), SHR_OK);
        ASSERT(same_image(&a, &b));
    }
    uint32_t cap = 'A';
    ASSERT_EQ_LL(real_load(ctx, f, &cap, 1, &a), SHR_OK);
    ASSERT_EQ_LL(a.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
    harness_close(&h);

    /* Not installed: every glyph falls back to the built-in package. */
    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = font_dir_open, fd.user = (void *)"/nonexistent";
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(ctx, &fd, &h.font), SHR_OK);
    ASSERT_EQ_LL(real_load(ctx, h.font, &fffd, 1, &a), SHR_OK);
    ASSERT_EQ_LL(real_load(ctx, h.font, singles, 1, &b), SHR_OK);
    ASSERT(!b.provisional && same_image(&a, &b) && inside(&b, shr__builtin_package, shr__builtin_package_size));
    ASSERT_EQ_LL(last_failure(ctx), SHR_OK);
    harness_close(&h);
    PASS();
}

/* The emoji package's own sequences, not the U+FFFD or text fallbacks, draw these clusters. */
TEST test_real_emoji_sequences(void) {
    blob e = read_package("emoji");
    ASSERT(e.size > 0);
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    tsrc s = {e.data, e.size, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_EMOJI] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    const struct {
        uint32_t cps[3];
        size_t n;
    } seqs[] = {{{0x2764, 0xFE0F}, 2}, {{0x1F469, 0x200D, 0x1F4BB}, 3}, {{0x1F1F0, 0x1F1F7}, 2}};
    shr__resolved fffd, woman, r;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xFFFD), &fffd), SHR_OK);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0x1F469), &woman), SHR_OK);
    for (size_t i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        uint64_t id = glyph(f, seqs[i].cps, seqs[i].n);
        ASSERT(id & SHR_ID_EMOJI);
        ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
        ASSERT(!r.provisional && inside(&r, e.data, e.size) && !same_image(&r, &fffd) && !same_image(&r, &woman));
    }
    harness_close(&h);
    free(e.data);
    PASS();
}

TEST test_powerline_meets_cell_edge(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    shr_pl_res_bitmap_font *f = harness_font(&h);
    shr_lyr *layer;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &layer), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, f, 1, 1, NULL), SHR_OK);
    ASSERT_EQ_LL(
        shr_pl_lyr_tilemap_set_cell(layer, 0, 0, "\xEE\x82\xB0", 3, 1, (shr_text_style){SHR_RGB(255, 255, 255), 0, 0}),
        SHR_OK);
    shr_submit(ctx);
    settle(ctx);
    ASSERT_EQ_LL(lit(h.out.shown, 0, 1, 0, CH), CH); /* every row of the cell */
    ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
    harness_close(&h);
    PASS();
}

/* ===== Activation records ===== */

TEST test_activation_select(void) {
    uint8_t a[64] = "SHRFACT2", b[64] = "SHRFACT2";
    put(a + 8, 1, 8), put(b + 8, 2, 8);
    memset(a + 16, 0xAA, 32), memset(b + 16, 0xBB, 32);
    put(a + 48, 1234, 8), put(b + 48, 1234, 8);
    put(a + 56, XXH3_64bits(a, 56), 8), put(b + 56, XXH3_64bits(b, 56), 8);
    shr_activation act = {0};
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_activation_select(a, 64, b, 64, &act), SHR_OK);
    ASSERT_EQ_LL(act.generation, 2);
    ASSERT_EQ_LL(act.package_id[0], 0xBB);
    ASSERT_EQ_LL(act.package_size, 1234);
    b[30] ^= 1; /* torn write of the newer record */
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_activation_select(a, 64, b, 64, &act), SHR_OK);
    ASSERT_EQ_LL(act.generation, 1);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_activation_select(a, 64, a, 64, &act), SHR_OK); /* equal: the first */
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_activation_select(NULL, 0, a, 64, &act), SHR_OK);
    ASSERT_EQ_LL(act.package_id[0], 0xAA);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_activation_select(a, 63, NULL, 0, &act), SHR_E_NOT_FOUND);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_activation_select(a, 64, NULL, 0, NULL), SHR_E_INVALID_ARG);
    a[0] = 0;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_activation_select(a, 64, b, 64, &act), SHR_E_NOT_FOUND);
    /* tools/fontpack/fontpack.py activation_record(5, bytes(range(32)), 4096) */
    static const uint8_t py[64] = {'S', 'H', 'R', 'F', 'A', 'C', 'T', '2', 5, 0, 0, 0, 0, 0, 0, 0,
                                   0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
                                   0x00, 0x10, 0, 0, 0, 0, 0, 0, 0x20, 0x82, 0x3A, 0x46, 0x51, 0xFD, 0x2B, 0xE5};
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_activation_select(py, 64, NULL, 0, &act), SHR_OK);
    ASSERT_EQ_LL(act.generation, 5);
    ASSERT_EQ_LL(act.package_id[31], 31);
    ASSERT_EQ_LL(act.package_size, 4096);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(test_create_and_destroy_rules);
    RUN_TEST(test_out_of_memory);
    RUN_TEST(test_builtin_without_packages);
    RUN_TEST(test_glyph_ids);
    RUN_TEST(test_package_validation);
    RUN_TEST(test_index_size_limit);
    RUN_TEST(test_table_changed_between_reads);
    RUN_TEST(test_page_validation);
    RUN_TEST(test_synthetic_glyphs);
    RUN_TEST(test_deferred_reads);
    RUN_TEST(test_read_retries_and_limit);
    RUN_TEST(test_retry_time_saturates);
    RUN_TEST(test_failing_source_given_up);
    RUN_TEST(test_second_cooldown_after_signal);
    RUN_TEST(test_signal_only_has_no_deadline);
    RUN_TEST(test_package_failures_count_until_ready);
    RUN_TEST(test_absent_package_stays_absent);
    RUN_TEST(test_ready_signal_before_cooling_frame);
    RUN_TEST(test_would_block_backs_off);
    RUN_TEST(test_read_slots_exhausted);
    RUN_TEST(test_read_timeout);
    RUN_TEST(test_open_failures);
    RUN_TEST(test_failed_package_released);
    RUN_TEST(test_resolve_defers_open_and_checks);
    RUN_TEST(test_cjk_locale);
    RUN_TEST(test_routing);
    RUN_TEST(test_emoji_fallbacks);
    RUN_TEST(test_deadline_is_the_earliest_retry);
    RUN_TEST(test_page_cache_budget);
    RUN_TEST(test_page_lru_order);
    RUN_TEST(test_stale_wants_dropped);
    RUN_TEST(test_open_while_blocked);
    RUN_TEST(test_evict_wanted_page);
    RUN_TEST(test_pins_per_frame);
    RUN_TEST(test_destroy_with_outstanding_reads);
    RUN_TEST(test_shutdown_with_outstanding_reads);
    RUN_TEST(test_provisional_redrawn_when_ready);
    RUN_TEST(test_cooled_package_redrawn);
    RUN_TEST(test_failed_page_event_in_frames);
    RUN_TEST(test_real_packages);
    RUN_TEST(test_real_emoji_sequences);
    RUN_TEST(test_powerline_meets_cell_edge);
    RUN_TEST(test_activation_select);
    GREATEST_MAIN_END();
}
