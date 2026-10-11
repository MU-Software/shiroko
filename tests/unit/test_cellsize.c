#include "compositor.h"
#include "font_core.h"
#include "shr_text.h"
#define HW 128 /* 4 columns of the widest cell, 2 rows of the tallest */
#define HH 254
#include "harness.h"

#include "shr_bitmap_font.h"

#define CW SHR_CELL_WIDTH
#define CH SHR_CELL_HEIGHT
#define WHITE SHR_RGB(255, 255, 255)
#define PRESERVED (SHR_OUTPUT_PRESERVES_CONTENT | SHR_OUTPUT_RELEASE_ON_PRESENT)
#define CHECK(c)                                \
    do {                                        \
        if (!(c)) FAIL_WITH_LONGJMPm(#c);       \
    } while (0)

/* Cell sizes other than the build's, chosen from it: one a little larger and the extremes, those in range. */
typedef struct cell {
    int32_t w, h;
} cell;

static cell sizes[3];
static int nsizes;

static bool cell_ok(cell c) { return c.w >= 6 && c.w <= 32 && c.h >= 8 && c.h <= 127 && !(c.w == CW && c.h == CH); }

static void sizes_init(void) {
    const cell c[3] = {{CW + 4, CH + 8}, {32, 127}, {6, 8}};
    for (int i = 0; i < 3; i++)
        if (cell_ok(c[i])) sizes[nsizes++] = c[i];
}

/* ===== Synthetic Latin packages =====
 * One w x h A8 instance: baseline h - 3, underline h - 2, strike h / 2 - 1; one 64 x 64 page with 'A' (4 x 4, one
 * pixel in from the cell's left, its top four rows above the baseline) and U+FFFD (3 x 3, two pixels in). */
enum { PKG_CAP = 8192, PAGE = 64 };

typedef struct spec {
    cell size;
    uint8_t role;
    const char *locale;
    bool no_fffd;
    uint32_t mani_len; /* 0 = 36 */
    int32_t underline; /* 0 = h - 2 */
} spec;

typedef struct pkg {
    _Alignas(256) uint8_t d[PKG_CAP];
    size_t n;
    uint64_t mani, inst, page; /* box offsets */
} pkg;

static int32_t base_of(int32_t h) { return h - 3; }
static int32_t under_of(int32_t h) { return h - 2; }
static int32_t strike_of(int32_t h) { return h / 2 - 1; }

static void put(uint8_t *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> 8 * i);
}

/* A required box of `n` payload bytes at `at`, indexed by entry `e`; returns its end. */
static size_t box(uint8_t *d, uint8_t *e, size_t at, const char *type, const uint8_t *data, size_t n) {
    put(d + at, 16 + n, 4), memcpy(d + at + 4, type, 4), put(d + at + 10, 1, 2), put(d + at + 12, n, 4);
    memcpy(d + at + 16, data, n);
    memcpy(e, type, 4), put(e + 4, 1, 2), put(e + 8, at, 8), put(e + 16, 16 + n, 4), put(e + 20, n, 4);
    return at + 16 + n;
}

/* A free box up to where the box after it has its payload, `head` bytes in, at a multiple of `align`. */
static size_t pad(uint8_t *d, size_t at, size_t head, size_t align) {
    size_t g = (align - (at + head) % align) % align;
    if (g && g < 16) g += align;
    if (g) put(d + at, g, 4), memcpy(d + at + 4, "free", 4), put(d + at + 12, g - 16, 4);
    return at + g;
}

/* A ctri of `n` scalars and their glyphs; returns its size. */
static size_t ctri(uint8_t *out, const uint32_t (*map)[2], int n) {
    uint16_t l1[1088] = {0}, l2[4][64] = {{0}};
    uint32_t data[4][16], nl2 = 1, nd = 1;
    memset(data, 0xFF, sizeof(data));
    for (int i = 0; i < n; i++) {
        uint32_t cp = map[i][0];
        if (!l1[cp >> 10]) l1[cp >> 10] = (uint16_t)nl2++;
        uint16_t *b = &l2[l1[cp >> 10]][cp >> 4 & 63];
        if (!*b) *b = (uint16_t)nd++;
        data[*b][cp & 15] = map[i][1];
    }
    size_t d = (SHR_PKG_CTRI + 128 * nl2 + 63) / 64 * 64;
    memset(out, 0, d);
    put(out, nl2, 4), put(out + 4, nd, 4);
    for (int i = 0; i < 1088; i++) put(out + 16 + 2 * i, l1[i], 2);
    for (uint32_t b = 0; b < nl2; b++)
        for (int k = 0; k < 64; k++) put(out + SHR_PKG_CTRI + 128 * b + 2 * k, l2[b][k], 2);
    for (uint32_t b = 0; b < nd; b++)
        for (int k = 0; k < 16; k++) put(out + d + 64 * b + 4 * k, data[b][k], 4);
    return d + 64 * nd;
}

static void seal_header(pkg *p) { put(p->d + 120, XXH3_64bits(p->d, 120), 8); }

static void seal_index(pkg *p) {
    put(p->d + 80, XXH3_64bits(p->d + 128, shr__rd32(p->d + 28)), 8);
    seal_header(p);
}

/* Every checksum: the page's, the boxes', the index's, the header's. */
static void seal(pkg *p) {
    uint8_t *d = p->d, *e = d + 152;
    while (memcmp(e, "ptab", 4)) e += 32;
    uint8_t *t = d + shr__rd64(e + 8) + 16;
    put(t + 8, XXH3_64bits(d + p->page, shr__rd32(t + 16)), 8);
    for (e = d + 152; e < d + 128 + shr__rd32(d + 28); e += 32)
        put(e + 24, XXH3_64bits(d + shr__rd64(e + 8), shr__rd32(e + 16)), 8);
    seal_index(p);
}

static void pkg_make(pkg *p, spec s) {
    static uint8_t ct[4096];
    uint8_t man[64] = {0}, in[48] = {0}, pt[24] = {0}, *d = p->d, *e = d + 152;
    int32_t w = s.size.w, h = s.size.h;
    memset(p, 0, sizeof(*p));
    man[0] = s.role ? s.role : ROLE_LATIN;
    if (s.locale) memcpy(man + 4, s.locale, strlen(s.locale));
    put(man + 28, 2, 4), put(man + 32, 1, 4);
    in[3] = 2;
    put(in + 4, (uint64_t)h, 2), put(in + 6, (uint64_t)w, 2), put(in + 12, (uint64_t)base_of(h), 2);
    put(in + 14, (uint64_t)(s.underline ? s.underline : under_of(h)), 2), put(in + 16, (uint64_t)strike_of(h), 2);
    put(in + 36, PAGE, 2), put(in + 38, PAGE, 2);
    const uint32_t map[2][2] = {{'A', 0}, {0xFFFD, 1}};
    size_t at = 152 + 4 * 32;
    p->mani = at, at = box(d, e, at, "mani", man, s.mani_len ? s.mani_len : 36);
    p->inst = at, at = box(d, e + 32, at, "inst", in, 48);
    at = pad(d, at, 16, 64);
    at = box(d, e + 64, at, "ctri", ct, ctri(ct, map, s.no_fffd ? 1 : 2));
    size_t ptab = at;
    at = box(d, e + 96, at, "ptab", pt, 24);
    at = pad(d, at, 24, 256);
    uint8_t *b = d + (p->page = at), *atlas = b + 24, *rec = atlas + PAGE * PAGE;
    put(b, 24 + PAGE * PAGE + 32, 4), memcpy(b + 4, "page", 4), put(b + 10, 1, 2);
    put(b + 12, 8 + PAGE * PAGE + 32, 4), put(b + 20, PAGE * PAGE, 4);
    for (int y = 0; y < 4; y++) memset(atlas + y * PAGE, 0xFF, 4);
    for (int y = 0; y < 3; y++) memset(atlas + y * PAGE + 8, 0xFF, 3);
    put(rec, 0, 2), rec[4] = 4, rec[5] = 4, rec[6] = 1, rec[7] = 4, rec[8] = 2 | 4;
    put(rec + 16, 8, 2), rec[20] = 3, rec[21] = 3, rec[22] = 2, rec[23] = 3, rec[24] = 2 | 4;
    at += 24 + PAGE * PAGE + 32;
    uint8_t *t = d + ptab + 16;
    put(t, p->page, 8), put(t + 16, 24 + PAGE * PAGE + 32, 4), put(t + 20, 2, 2), put(t + 22, PAGE, 2);
    memcpy(d, "\x80\0\0\0shrf", 8), put(d + 10, 1, 2), put(d + 12, 112, 4), put(d + 16, 6, 2), put(d + 20, 2, 4);
    put(d + 28, 24 + 4 * 32, 4), put(d + 32, at, 8), put(d + 40, 128, 8), memcpy(d + 48, shr__text_profile_id, 32);
    put(d + 128, 24 + 4 * 32, 4), memcpy(d + 132, "sidx", 4), put(d + 138, 1, 2), put(d + 140, 8 + 4 * 32, 4);
    put(d + 144, 4, 4);
    p->n = at;
    seal(p);
}

/* ===== Sources ===== */

enum { SRC_READ, SRC_MAP, SRC_NONE };

typedef struct pending_read {
    uint64_t request, off;
    uint32_t len;
    void *dst;
} pending_read;

typedef struct src {
    const pkg *p;
    uint64_t claim; /* reported size; 0 = the package's */
    int mode;
    bool cancellable;
    shr_status open_result;
    shr_status script[8];   /* read call i: 0 served now, SHR_IN_PROGRESS deferred, else failed */
    shr_status page_result; /* the same for reads of the page */
    int calls, page_reads, closes, cancels, pending;
    pending_read q[4];
} src;

static shr_status src_read(void *user, uint64_t off, uint32_t len, void *dst, uint64_t request) {
    src *s = user;
    shr_status st = off >= s->p->page ? s->page_result : s->calls < 8 ? s->script[s->calls] : SHR_OK;
    s->calls++, s->page_reads += off >= s->p->page;
    if (st == SHR_IN_PROGRESS) s->q[s->pending++] = (pending_read){request, off, len, dst};
    if (st == SHR_OK) memcpy(dst, s->p->d + off, len);
    return st;
}

static void src_cancel(void *user, uint64_t request) {
    src *s = user;
    s->cancels++;
    for (int i = 0; i < s->pending; i++)
        if (s->q[i].request == request) {
            memmove(&s->q[i], &s->q[i + 1], (size_t)(--s->pending - i) * sizeof(s->q[0]));
            return;
        }
}

static void src_close(void *user) { ((src *)user)->closes++; }

/* Completes the oldest deferred read. */
static void src_complete(shr_context *ctx, src *s, shr_status result) {
    pending_read r = s->q[0];
    memmove(&s->q[0], &s->q[1], (size_t)--s->pending * sizeof(s->q[0]));
    if (result == SHR_OK) memcpy(r.dst, s->p->d + r.off, r.len);
    ASSERT_EQ_LL(shr_asset_complete(ctx, r.request, result), SHR_OK);
}

/* What open() serves: `latin` first, `later` (when set) to the opens after; `cjk` for any CJK package. */
typedef struct lib {
    src *latin, *later, *cjk;
    int opens, latin_opens;
    void *user;
    int logs;
    shr_status log_status;
    char why[80];
} lib;

static shr_status lib_open(void *user, const char *name, shr_asset_source *out) {
    lib *l = user;
    l->opens++, l->user = user;
    src *s = NULL;
    if (!strcmp(name, "shiroko-latin.shrf")) s = l->latin_opens++ && l->later ? l->later : l->latin;
    if (!strncmp(name, "shiroko-cjk-", 12)) s = l->cjk;
    if (!s) return SHR_E_NOT_FOUND;
    if (s->open_result) return s->open_result;
    shr_asset_source_init(out);
    out->user = s;
    out->size = s->claim ? s->claim : s->p->n;
    if (s->mode == SRC_MAP) out->data = s->p->d;
    if (s->mode == SRC_READ) out->read = src_read;
    out->cancel = s->cancellable ? src_cancel : NULL;
    out->close = src_close;
    return SHR_OK;
}

static void lib_log(void *user, shr_status status, const char *message) {
    lib *l = user;
    l->logs++, l->log_status = status;
    snprintf(l->why, sizeof(l->why), "%s", message);
}

/* ===== Contexts and fonts ===== */

static harness H;
static cell g_cell;
static lib *g_lib;
static void (*g_more)(shr_context_desc *, shr_framebuffer_driver *);

static void tweak_cell(shr_context_desc *d, shr_framebuffer_driver *drv) {
    d->cell_width = (uint32_t)g_cell.w, d->cell_height = (uint32_t)g_cell.h;
    d->user = g_lib, d->open = g_lib ? lib_open : NULL, d->log = g_lib ? lib_log : NULL;
    if (g_more) g_more(d, drv);
}

/* H opened at cell size `c` (0 x 0: the build's) with `l` as open(), and a font opening packages from `l`. */
static shr_context *open_at(cell c, lib *l, uint32_t flags, void (*more)(shr_context_desc *, shr_framebuffer_driver *)) {
    g_cell = c, g_lib = l, g_more = more;
    harness_open(&H, flags, tweak_cell);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.user = l, fd.open = l ? lib_open : NULL;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(H.ctx, &fd, &H.font), SHR_OK);
    return H.ctx;
}

static void ctx_free(shr_context *ctx) {
    shr_begin_shutdown(ctx);
    for (int i = 0; i < 8; i++)
        if (shr_destroy(ctx) == SHR_OK) return;
    FAIL_WITH_LONGJMPm("context did not shut down");
}

static uint64_t frame_no = 1000;
static void drain(shr_context *ctx);

static uint64_t glyph(shr_pl_res_bitmap_font *f, uint32_t cp) {
    shr__cluster_class cls;
    shr__classify(&cp, 1, &cls);
    uint64_t id = 0;
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, &cp, 1, &cls, &id), SHR_OK);
    return id;
}

/* Resolves in a frame of its own, which then ends. */
static shr_status resolve(shr_pl_res_bitmap_font *f, uint32_t cp, shr__resolved *out) {
    shr__res *res = shr__bitmap_font_res(f);
    const shr__resolved *r = NULL;
    *out = (shr__resolved){0};
    shr_status st = res->ops->resolve(res, glyph(f, cp), ++frame_no, &r);
    if (st == SHR_OK) *out = *r;
    res->ops->frame_end(res, frame_no);
    return st;
}

/* Resolves, pumping in between, until the answer is final (bounded). */
static void resolve_final(shr_context *ctx, shr_pl_res_bitmap_font *f, uint32_t cp, shr__resolved *out) {
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ_LL(resolve(f, cp, out), SHR_OK);
        if (!out->provisional) return;
        drain(ctx);
    }
    FAIL_WITH_LONGJMPm("glyph stays provisional");
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

static int cov(const shr__buf *b, int32_t x, int32_t y) {
    return ((const uint8_t *)b->mem.pixels)[(size_t)y * b->mem.stride + (size_t)x];
}

/* The box's pixel (x, y) in a w x h cell, its stroke t thick. */
static bool box_at(cell c, int32_t t, int32_t x, int32_t y) {
    return x >= 1 && x < c.w - 1 && (y < t || y >= c.h - t || x < 1 + t || x >= c.w - 1 - t);
}

static int32_t stroke(int32_t w) { return w < 14 ? 1 : w < 28 ? 2 : 3; }

/* `r` is the placeholder of a `c` cell. */
static void assert_box(const shr__resolved *r, cell c) {
    ASSERT_EQ_LL(r->buf->mem.format, SHR_FORMAT_A8);
    CHECK(r->rect.x0 == 0 && r->rect.y0 == 0 && r->rect.x1 == c.w && r->rect.y1 == c.h);
    CHECK(r->offset.x == 0 && r->offset.y == 0 && r->synth == 0);
    for (int32_t y = 0; y < c.h; y++)
        for (int32_t x = 0; x < c.w; x++) ASSERT_EQ_LL(cov(r->buf, x, y), box_at(c, stroke(c.w), x, y) ? 255 : 0);
}

/* `r` is the synthetic 'A' (4 x 4) or U+FFFD (3 x 3) of a cell `h` tall. */
static void assert_glyph(const shr__resolved *r, int32_t h, int32_t side) {
    CHECK(r->rect.x1 - r->rect.x0 == side && r->rect.y1 - r->rect.y0 == side && !r->provisional);
    CHECK(r->offset.x == (side == 4 ? 1 : 2) && r->offset.y == base_of(h) - side);
    ASSERT_EQ_LL(r->synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
    ASSERT_EQ_LL(r->slant_axis, h - 2 * r->offset.y);
}

/* ===== Created at a cell size ===== */

static bool same_lines(shr__line_metrics a, shr__line_metrics b) {
    return a.baseline == b.baseline && a.underline_y == b.underline_y && a.strike_y == b.strike_y;
}

static shr__line_metrics lines_of(int32_t h) { return (shr__line_metrics){base_of(h), under_of(h), strike_of(h)}; }

/* shr_create() with cell size c and l's open() (when set), on an allocator counting what stays allocated. */
static shr_status create(cell c, lib *l, shr_context **out) {
    static harness h;
    static fail_alloc fa;
    static shr_allocator al;
    shr_output o;
    shr_context_desc d;
    harness_desc(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, &o, &d);
    fa = (fail_alloc){-1, 0}, al = fail_allocator(&fa);
    d.allocator = &al;
    d.cell_width = (uint32_t)c.w, d.cell_height = (uint32_t)c.h;
    if (l) d.user = l, d.open = lib_open, d.log = lib_log;
    *out = (shr_context *)&h;
    shr_status st = shr_create(&d, out);
    ASSERT_EQ_LL(st == SHR_OK, *out != NULL);
    ASSERT_EQ_LL(st == SHR_OK, fa.live > 0);
    return st;
}

TEST create_checks_the_cell_size(void) {
    static pkg p;
    cell s = sizes[0];
    pkg_make(&p, (spec){.size = s});
    src a = {&p, .mode = SRC_MAP};
    lib l = {.latin = &a};
    shr_context *ctx;
    const cell bad[] = {{s.w, 0}, {0, s.h}, {5, s.h}, {33, s.h}, {s.w, 7}, {s.w, 128}, {-1, s.h}, {s.w, -1}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) ASSERT_EQ_LL(create(bad[i], &l, &ctx), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(create(s, NULL, &ctx), SHR_E_INVALID_ARG); /* no open() */
    ASSERT_EQ_LL(l.opens + l.logs, 0);
    const cell built[] = {{0, 0}, {CW, CH}};
    for (int i = 0; i < 2; i++)
        for (int with = 0; with < 2; with++) { /* the build's size needs no package */
            ASSERT_EQ_LL(create(built[i], with ? &l : NULL, &ctx), SHR_OK);
            int32_t w = 0, h = 0;
            ASSERT_EQ_LL(shr_cell_size_get(ctx, &w, &h), SHR_OK);
            ASSERT(w == CW && h == CH);
            ctx_free(ctx);
        }
    ASSERT_EQ_LL(l.opens + l.logs, 0);
    for (int i = 0; i < nsizes; i++) {
        pkg_make(&p, (spec){.size = sizes[i]});
        for (int mode = SRC_READ; mode <= SRC_MAP; mode++) {
            l = (lib){.latin = &a}, a = (src){&p, .mode = mode};
            ASSERT_EQ_LL(create(sizes[i], &l, &ctx), SHR_OK);
            ASSERT(l.opens == 1 && l.user == &l && a.closes == 1 && a.calls == (mode == SRC_READ ? 4 : 0) && !l.logs);
            int32_t w = 0, h = 0;
            ASSERT_EQ_LL(shr_cell_size_get(ctx, &w, NULL), SHR_OK);
            ASSERT_EQ_LL(shr_cell_size_get(ctx, NULL, &h), SHR_OK);
            ASSERT(w == sizes[i].w && h == sizes[i].h);
            ASSERT_EQ_LL(shr_cell_size_get(ctx, NULL, NULL), SHR_OK);
            const shr__cell_info *ci = shr__ctx_cell(ctx);
            ASSERT(ci->w == w && ci->h == h && same_lines(ci->lines, lines_of(h)));
            ctx_free(ctx);
        }
    }
    ASSERT_EQ_LL(shr_cell_size_get(NULL, NULL, NULL), SHR_E_INVALID_ARG);
    PASS();
}

/* At the build's size the line positions are the built-in package's. */
TEST build_size_lines_are_the_builtin_packages(void) {
    shr_asset_source b;
    shr_asset_source_init(&b);
    b.data = shr__builtin_package, b.size = shr__builtin_package_size;
    shr__pkg_desc pd;
    const char *why;
    ASSERT_EQ_LL(shr__pkg_describe(&b, &pd, &why), SHR_OK);
    shr_context *ctx;
    ASSERT_EQ_LL(create((cell){0, 0}, NULL, &ctx), SHR_OK);
    ASSERT(same_lines(shr__ctx_cell(ctx)->lines, pd.lines));
    ctx_free(ctx);
    PASS();
}

/* open()'s error as it is; a read's as shr__pkg_describe() gives it; log() hears why; the source closes once. */
TEST create_reports_open_and_read_failures(void) {
    static pkg p;
    cell s = sizes[0];
    pkg_make(&p, (spec){.size = s});
    shr_context *ctx;
    const shr_status opened[] = {SHR_E_NOT_FOUND, SHR_E_IO, SHR_E_NO_MEMORY};
    for (int i = 0; i < 3; i++) {
        src a = {&p, .open_result = opened[i]};
        lib l = {.latin = &a};
        ASSERT_EQ_LL(create(s, &l, &ctx), opened[i]);
        ASSERT(l.logs == 1 && l.log_status == opened[i] && !strcmp(l.why, "font package open failed") && !a.closes);
    }
    src none = {&p, .mode = SRC_NONE};
    lib l = {.latin = &none};
    ASSERT_EQ_LL(create(s, &l, &ctx), SHR_E_INVALID_ARG);
    ASSERT(l.logs == 1 && !strcmp(l.why, "package source without data or read()") && none.closes == 1);
    const struct {
        shr_status read, want;
        bool cancellable;
    } reads[] = {{SHR_E_IO, SHR_E_IO, false},
                 {SHR_E_DEVICE, SHR_E_IO, false},
                 {SHR_E_WOULD_BLOCK, SHR_E_WOULD_BLOCK, false},
                 {SHR_IN_PROGRESS, SHR_E_UNSUPPORTED, true},
                 {SHR_IN_PROGRESS, SHR_E_UNSUPPORTED, false}};
    for (int k = 0; k < 4; k++) /* the header, the index, mani, inst */
        for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
            src a = {&p, .mode = SRC_READ, .cancellable = reads[i].cancellable};
            a.script[k] = reads[i].read;
            l = (lib){.latin = &a};
            ASSERT_EQ_LL(create(s, &l, &ctx), reads[i].want);
            ASSERT(l.logs == 1 && l.log_status == reads[i].want && !strcmp(l.why, "package read failed"));
            ASSERT(a.calls == k + 1 && a.closes == 1 && a.cancels == (reads[i].cancellable ? 1 : 0));
        }
    src a = {&p, .mode = SRC_READ, .script = {SHR_E_IO}};
    lib quiet = {.latin = &a}; /* without log() */
    shr_output o;
    shr_context_desc d;
    harness_desc(&H, SHR_OUTPUT_RELEASE_ON_PRESENT, &o, &d);
    d.cell_width = (uint32_t)s.w, d.cell_height = (uint32_t)s.h, d.user = &quiet, d.open = lib_open;
    ASSERT_EQ_LL(shr_create(&d, &ctx), SHR_E_IO);
    ASSERT(ctx == NULL && a.closes == 1);
    PASS();
}

typedef struct breakage {
    const char *name;
    void (*apply)(pkg *p);
    shr_status want;
    const char *why;
} breakage;

static void b_magic(pkg *p) { p->d[5] = 'X'; }
static void b_header_sum(pkg *p) { p->d[100] ^= 1; }
static void b_feature(pkg *p) { put(p->d + 20, 2 | 0x100, 4), seal_header(p); }
static void b_reserved(pkg *p) { p->d[100] = 1, seal_header(p); }
static void b_profile(pkg *p) { p->d[48] ^= 1, seal_header(p); }
static void b_index_sum(pkg *p) { p->d[152 + 20] ^= 1; }
static void b_index(pkg *p) { p->d[132] = 'X', seal_index(p); }
static void b_mani_sum(pkg *p) { p->d[p->mani + 16 + 11] = 'x'; }
static void b_inst_sum(pkg *p) { p->d[p->inst + 16 + 40] = 1; }
static void b_box_header(pkg *p) { p->d[p->mani + 8] = 1, seal(p); }
static void b_role_value(pkg *p) { p->d[p->mani + 16] = 9, seal(p); }
static void b_inst_reserved(pkg *p) { p->d[p->inst + 16 + 40] = 1, seal(p); }

/* Every package the check refuses: what the context returns and log() hears. */
TEST create_rejects_packages_it_cannot_use(void) {
    static pkg p;
    cell s = sizes[0];
    const breakage bytes[] = {{"magic", b_magic, SHR_E_FORMAT, "bad magic"},
                              {"header checksum", b_header_sum, SHR_E_CHECKSUM, "header checksum"},
                              {"feature", b_feature, SHR_E_UNSUPPORTED, "unsupported required feature"},
                              {"reserved", b_reserved, SHR_E_FORMAT, "reserved header bytes"},
                              {"profile", b_profile, SHR_E_PROFILE_MISMATCH, "text profile mismatch"},
                              {"index checksum", b_index_sum, SHR_E_CHECKSUM, "index checksum"},
                              {"index", b_index, SHR_E_FORMAT, "index box"},
                              {"mani checksum", b_mani_sum, SHR_E_CHECKSUM, "box checksum"},
                              {"inst checksum", b_inst_sum, SHR_E_CHECKSUM, "box checksum"},
                              {"box header", b_box_header, SHR_E_FORMAT, "box header or payload"},
                              {"role value", b_role_value, SHR_E_FORMAT, "role"},
                              {"inst reserved", b_inst_reserved, SHR_E_FORMAT, "instance reserved bytes"}};
    const struct {
        spec s;
        shr_status want;
        const char *why;
    } specs[] = {{{.size = s, .role = ROLE_CJK, .locale = "ko"}, SHR_E_FORMAT, "package role or locale differs from its file name"},
                 {{.size = s, .role = ROLE_SYMBOLS}, SHR_E_FORMAT, "package role or locale differs from its file name"},
                 {{.size = {s.w + 1, s.h}}, SHR_E_UNSUPPORTED, "no instance for the cell size"},
                 {{.size = {s.w, s.h + 1}}, SHR_E_UNSUPPORTED, "no instance for the cell size"},
                 {{.size = {CW, CH}}, SHR_E_UNSUPPORTED, "no instance for the cell size"},
                 {{.size = s, .mani_len = 60}, SHR_E_FORMAT, "mani or inst size"},
                 {{.size = s, .underline = s.h}, SHR_E_FORMAT, "instance line metrics"}};
    size_t nb = sizeof(bytes) / sizeof(bytes[0]), ns = sizeof(specs) / sizeof(specs[0]);
    for (int mode = SRC_READ; mode <= SRC_MAP; mode++)
        for (size_t i = 0; i < nb + ns + 1; i++) {
            src a = {&p, .mode = mode};
            pkg_make(&p, i < nb ? (spec){.size = s} : i < nb + ns ? specs[i - nb].s : (spec){.size = s});
            if (i < nb) bytes[i].apply(&p);
            if (i == nb + ns) a.claim = SHR_PKG_HEADER - 1;
            shr_status want = i < nb ? bytes[i].want : i < nb + ns ? specs[i - nb].want : SHR_E_FORMAT;
            const char *why = i < nb ? bytes[i].why : i < nb + ns ? specs[i - nb].why : "package too small";
            lib l = {.latin = &a};
            shr_context *ctx;
            if (create(s, &l, &ctx) != want || strcmp(l.why, why) || l.logs != 1 || l.log_status != want || a.closes != 1)
                FAILm(i < nb ? bytes[i].name : why);
        }
    PASS();
}

/* ===== Drawn at a cell size ===== */

static shr_lyr *new_layer(void) {
    shr_lyr *l = NULL;
    ASSERT_EQ_LL(shr_lyr_create(H.ctx, 0, (shr_rect){0, 0, HW, HH}, &l), SHR_OK);
    return l;
}

static const shr__group *group_of(const shr_lyr *l, int32_t row) {
    ASSERT_EQ_LL(l->flush(l->state), SHR_OK);
    for (size_t i = 0; i < l->groups.len; i++) {
        const shr__group *g = SHR_VEC_AT(&l->groups, shr__group, i);
        if (g->id == (uint32_t)row) return g;
    }
    FAIL_WITH_LONGJMPm("no row group");
    return NULL;
}

static shr_status set_cell(shr_lyr *l, int32_t row, int32_t col, const char *s, uint32_t span, shr_text_style st) {
    return shr_pl_lyr_tilemap_set_cell(l, row, col, s, strlen(s), span, st);
}

static bool rect_is(shr_rect r, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    return r.x0 == x0 && r.y0 == y0 && r.x1 == x1 && r.y1 == y1;
}

static int32_t rows_of(cell c) { return HH / c.h < 3 ? HH / c.h : 3; }
static int32_t cols_of(cell c) { return HW / c.w < 8 ? HW / c.w : 8; }

/* Rows sit r cells down; their commands span whole cells; lines take the bands of the Latin package's positions. */
TEST rows_and_lines_take_the_cell_size(void) {
    static pkg p;
    for (int i = 0; i < nsizes; i++) {
        cell c = sizes[i];
        int32_t rows = rows_of(c), cols = cols_of(c), ul = under_of(c.h), sk = strike_of(c.h);
        pkg_make(&p, (spec){.size = c});
        src a = {&p, .mode = SRC_MAP};
        lib l = {.latin = &a};
        open_at(c, &l, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
        shr_lyr *layer = new_layer();
        const shr_color dark = SHR_RGB(10, 20, 30);
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, H.font, rows, cols, &dark), SHR_OK);
        ASSERT_EQ_LL(set_cell(layer, 0, 0, "A", 1, (shr_text_style){WHITE, SHR_RGB(200, 0, 0), 0}), SHR_OK);
        ASSERT_EQ_LL(set_cell(layer, 0, 1, "\xEA\xB0\x80", 2, (shr_text_style){WHITE, 0, 0}), SHR_OK);
        const shr_text_line lines[] = {{0, 1, SHR_LINE_UNDER, SHR_LINE_SINGLE, 0, WHITE},
                                       {1, 1, SHR_LINE_STRIKE, SHR_LINE_SINGLE, 0, WHITE},
                                       {2, 1, SHR_LINE_UNDER, SHR_LINE_DOUBLE, 0, WHITE},
                                       {3, 1, SHR_LINE_OVER, SHR_LINE_SINGLE, 0, WHITE},
                                       {4, 1, SHR_LINE_UNDER, SHR_LINE_CURLY, 0, WHITE},
                                       {5, 1, SHR_LINE_STRIKE, SHR_LINE_DOTTED, 0, WHITE}};
        int nl = cols < 6 ? cols : 6;
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_lines(layer, rows - 1, lines, (size_t)nl, NULL), SHR_OK);
        const int32_t want[6][2] = {{ul, ul + 1}, {sk, sk + 1}, {ul - 1, ul + 2}, {0, 1}, {0, 0}, {sk - 1, sk + 2}};
        uint8_t band[3][5][2];
        shr__line_metrics lm = lines_of(c.h);
        shr__line_bands(band, c.w, c.h, &lm);
        for (int32_t r = 0; r < rows; r++) {
            const shr__group *g = group_of(layer, r);
            ASSERT_EQ_LL(g->oy, r * c.h);
            int glyphs = 0, nline = 0;
            for (size_t k = 0; k < g->n; k++) {
                shr__lcmd x = shr__group_cmd(g, k, c.w);
                if (x.kind == SHR__LCMD_CACHE_END) continue;
                ASSERT(x.dst.x0 % c.w == 0 && x.dst.x1 % c.w == 0 && x.dst.x1 <= cols * c.w);
                if (x.kind == SHR__LCMD_CACHE_BEGIN) ASSERT(rect_is(x.dst, 0, 0, cols * c.w, c.h));
                if (x.kind == SHR__LCMD_FILL) ASSERT(x.dst.y0 == 0 && x.dst.y1 == c.h);
                if (x.kind == SHR__LCMD_GLYPH) {
                    int32_t span = glyphs ? 2 : 1;
                    ASSERT(rect_is(x.dst, glyphs * c.w, 0, (glyphs + span) * c.w, c.h) && r == 0);
                    glyphs++;
                }
                if (x.kind != SHR__LCMD_LINE) continue;
                const shr_text_line *e = &lines[nline++];
                ASSERT(r == rows - 1 && rect_is(x.dst, e->col * c.w, band[e->kind][e->shape][0], (e->col + 1) * c.w,
                                                band[e->kind][e->shape][1]));
                if (e->shape != SHR_LINE_CURLY) ASSERT(x.dst.y0 == want[e->col][0] && x.dst.y1 == want[e->col][1]);
            }
            ASSERT_EQ_LL(glyphs, r == 0 ? 2 : 0);
            ASSERT_EQ_LL(nline, r == rows - 1 ? nl : 0);
        }
        ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
        harness_close(&H);
    }
    PASS();
}

/* ---- frames ---- */

static struct {
    shr_draw_cmd cmds[2048];
    size_t n;
} rec;

static shr_status rec_execute(void *user, const shr_surface *dst, const shr_draw_cmd *c, size_t n, shr_fence f) {
    for (size_t i = 0; i < n && rec.n < 2048; i++) rec.cmds[rec.n++] = c[i];
    return md_execute(user, dst, c, n, f);
}

static void tweak_rec(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)d;
    drv->execute = rec_execute;
}

static void tweak_moves(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps.flags = SHR_DRIVER_CHEAP_MOVE;
}

static void tweak_keeps(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    mock_driver *m = drv->user;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 8, HBUFS, &m->sw), SHR_OK);
    drv->caps.max_keeps = 8;
}

static const uint8_t *frame(void) {
    rec.n = 0;
    ASSERT_EQ_LL(shr_submit(H.ctx), SHR_OK);
    settle(H.ctx);
    return H.out.shown;
}

/* The colour filling cell (row, col) exactly, else -1. */
static long cell_fill(const uint8_t *buf, cell c, int32_t row, int32_t col) {
    uint32_t v = px(buf, col * c.w, row * c.h);
    for (int32_t y = row * c.h; y < (row + 1) * c.h; y++)
        for (int32_t x = col * c.w; x < (col + 1) * c.w; x++)
            if (px(buf, x, y) != v) return -1;
    return (long)v;
}

/* Cell (row, col) shows white exactly at the pixels lit(x, y) gives, else black. */
static bool cell_shows(const uint8_t *buf, cell c, int32_t row, int32_t col, bool (*lit)(cell, int32_t, int32_t)) {
    for (int32_t y = 0; y < c.h; y++)
        for (int32_t x = 0; x < c.w; x++)
            if (px(buf, col * c.w + x, row * c.h + y) != (lit(c, x, y) ? 0xFFFFFFu : 0)) return false;
    return true;
}

static bool lit_box(cell c, int32_t x, int32_t y) { return box_at(c, stroke(c.w), x, y); }
static bool lit_a(cell c, int32_t x, int32_t y) { return x >= 1 && x < 5 && y >= base_of(c.h) - 4 && y < base_of(c.h); }
static bool lit_none(cell c, int32_t x, int32_t y) { return (void)c, (void)x, (void)y, false; }
static bool lit_under(cell c, int32_t x, int32_t y) { return (void)x, y == under_of(c.h) || lit_a(c, x, y); }
static bool lit_strike(cell c, int32_t x, int32_t y) { return (void)x, y == strike_of(c.h); }

/* Backgrounds fill whole cells, glyphs and lines sit where the cell size and the Latin package put them. A LINE
 * command drawing part of its cells starts its pattern at the column in the cell. */
TEST frames_draw_cells_of_the_cell_size(void) {
    static pkg p;
    for (int i = 0; i < nsizes; i++) {
        cell c = sizes[i];
        int32_t rows = rows_of(c), cols = cols_of(c);
        pkg_make(&p, (spec){.size = c});
        src a = {&p, .mode = SRC_MAP};
        lib l = {.latin = &a};
        open_at(c, &l, PRESERVED, tweak_rec);
        shr_lyr *layer = new_layer();
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, H.font, rows, cols, NULL), SHR_OK);
        for (int32_t r = 0; r < rows; r++)
            ASSERT_EQ_LL(set_cell(layer, r, cols - 1, " ", 1, (shr_text_style){WHITE, SHR_RGB(0, 0, 40 + 60 * r), 0}),
                         SHR_OK);
        ASSERT_EQ_LL(set_cell(layer, 0, 0, "A", 1, (shr_text_style){WHITE, 0, 0}), SHR_OK);
        const shr_text_line lines[2] = {{0, 1, SHR_LINE_UNDER, SHR_LINE_SINGLE, 0, WHITE},
                                        {1, (uint16_t)(cols - 2), SHR_LINE_STRIKE, SHR_LINE_SINGLE, 0, WHITE}};
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_lines(layer, 0, lines, 2, NULL), SHR_OK);
        const uint8_t *s = frame();
        for (int32_t r = 0; r < rows; r++) /* each row's own blue, filling the cell */
            ASSERT(cell_fill(s, c, r, cols - 1) > 0 && (r == 0 || cell_fill(s, c, r, cols - 1) > cell_fill(s, c, r - 1, cols - 1)));
        ASSERT(cell_shows(s, c, 0, 0, lit_under));
        for (int32_t k = 1; k < cols - 1; k++) ASSERT(cell_shows(s, c, 0, k, lit_strike));
        for (int32_t k = 0; k < cols - 1; k++) ASSERT(rows < 2 || cell_shows(s, c, 1, k, lit_none));
        shr__damage_add(H.ctx, &H.ctx->staged, (shr_rect){c.w + 3, 0, HW, c.h});
        frame();
        int part = 0;
        for (size_t k = 0; k < rec.n; k++) {
            const shr_draw_cmd *x = &rec.cmds[k];
            if (x->kind != SHR_CMD_LINE) continue;
            ASSERT(rect_is(x->src_rect, 0, 0, c.w, 1) && x->src_origin.x == (x->dst.x0 - c.w) % c.w);
            part += x->dst.x0 == c.w + 3 && x->src_origin.x == 3;
        }
        ASSERT_EQ_LL(part, 1);
        ASSERT(cell_shows(H.out.shown, c, 0, 1, lit_strike));
        ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
        harness_close(&H);
    }
    PASS();
}

static shr_color row_bg(int32_t r) { return SHR_RGB(0, 0, 40 + 37 * r % 200); }

/* Scrolled rows move by whole cells: what shows equals a full redraw, and the rows were moved, not drawn again. */
TEST scrolls_move_rows_by_the_cell_height(void) {
    static pkg p;
    static uint8_t moved[sizeof(H.out.shown)];
    for (int i = 0; i < nsizes; i++) {
        cell c = sizes[i];
        int32_t rows = HH / c.h, cols = cols_of(c);
        pkg_make(&p, (spec){.size = c});
        src a = {&p, .mode = SRC_MAP};
        lib l = {.latin = &a};
        open_at(c, &l, PRESERVED, tweak_moves);
        shr_lyr *layer = new_layer();
        const shr_color dark = SHR_RGB(10, 20, 30);
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, H.font, rows, cols, &dark), SHR_OK);
        for (int32_t r = 0; r < rows; r++)
            ASSERT_EQ_LL(set_cell(layer, r, 0, "A", 1, (shr_text_style){WHITE, row_bg(r), 0}), SHR_OK);
        static uint8_t before[sizeof(H.out.shown)];
        memcpy(before, frame(), sizeof(before));
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_scroll(layer, 0, rows, 1, (shr_text_style){WHITE, 0, 0}), SHR_OK);
        memcpy(moved, frame(), sizeof(moved));
        int moves = 0;
        for (size_t k = 0; k < rec.n; k++) {
            const shr_draw_cmd *x = &rec.cmds[k];
            if (x->kind != SHR_CMD_COPY) continue;
            ASSERT(x->dst.y0 % c.h == 0 && x->dst.y1 % c.h == 0 && x->src_origin.y == x->dst.y0 + c.h);
            moves++;
        }
        ASSERT_EQ_LL(moves > 0, rows > 2); /* two rows are drawn again */
        for (int32_t r = 0; r + 1 < rows; r++) ASSERT_EQ_LL(cell_fill(moved, c, r, 0) == -1, true);
        for (int32_t r = 0; r + 1 < rows; r++)
            ASSERT(!memcmp(moved + (size_t)r * c.h * HW * SCREEN_BPP, before + (size_t)(r + 1) * c.h * HW * SCREEN_BPP,
                           (size_t)c.h * HW * SCREEN_BPP));
        ASSERT_EQ_LL(shr_request_redraw(H.ctx), SHR_OK);
        settle(H.ctx);
        ASSERT(memcmp(moved, H.out.shown, sizeof(moved)) == 0);
        ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
        harness_close(&H);
    }
    PASS();
}

static const shr_draw_cmd *nth_cmd(uint8_t kind, int nth) {
    for (size_t i = 0; i < rec.n; i++)
        if (rec.cmds[i].kind == kind && !nth--) return &rec.cmds[i];
    return NULL;
}

static int count_cmds(uint8_t kind) {
    int n = 0;
    while (nth_cmd(kind, n)) n++;
    return n;
}

/* Rows are kept as row rects of the cell size; scrolled, the rows draw the keeps of the rows they show. */
TEST rows_keep_and_draw_at_the_cell_size(void) {
    static pkg p;
    for (int i = 0; i < nsizes; i++) {
        cell c = sizes[i];
        int32_t rows = rows_of(c), cols = cols_of(c);
        pkg_make(&p, (spec){.size = c});
        src a = {&p, .mode = SRC_MAP};
        lib l = {.latin = &a};
        open_at(c, &l, PRESERVED, tweak_keeps);
        shr_lyr *layer = new_layer();
        const shr_color black = SHR_RGB(0, 0, 0);
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, H.font, rows, cols, &black), SHR_OK);
        for (int32_t r = 0; r < rows; r++)
            ASSERT_EQ_LL(set_cell(layer, r, r % cols, "A", 1, (shr_text_style){WHITE, 0, 0}), SHR_OK);
        shr__resolved g;
        resolve_final(H.ctx, H.font, 'A', &g); /* drawn final from the first frame on */
        frame();
        ASSERT_EQ_LL(count_cmds(SHR_CMD_KEEP_BEGIN), 0);
        ASSERT_EQ_LL(shr_request_redraw(H.ctx), SHR_OK);
        frame();
        ASSERT_EQ_LL(count_cmds(SHR_CMD_KEEP_BEGIN), rows);
        uint32_t ids[3];
        for (int32_t r = 0; r < rows; r++) {
            const shr_draw_cmd *b = nth_cmd(SHR_CMD_KEEP_BEGIN, r), *d = nth_cmd(SHR_CMD_KEEP_DRAW, r);
            ASSERT(rect_is(b->dst, 0, r * c.h, cols * c.w, (r + 1) * c.h) && d && d->buffer == b->buffer);
            ASSERT(rect_is(d->dst, 0, r * c.h, cols * c.w, (r + 1) * c.h));
            ids[r] = b->buffer;
        }
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_scroll(layer, 0, rows, 1, (shr_text_style){WHITE, 0, 0}), SHR_OK);
        frame();
        ASSERT_EQ_LL(count_cmds(SHR_CMD_KEEP_DRAW), rows - 1);
        for (int32_t r = 0; r + 1 < rows; r++) {
            const shr_draw_cmd *d = nth_cmd(SHR_CMD_KEEP_DRAW, r);
            ASSERT(d->buffer == ids[r + 1] && rect_is(d->dst, 0, r * c.h, cols * c.w, (r + 1) * c.h));
            ASSERT(cell_shows(H.out.shown, c, r, (r + 1) % cols, lit_a));
        }
        ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
        harness_close(&H);
    }
    PASS();
}

/* With bands the command list is reserved for two commands per cell of a band, in cells of the context's size. */
TEST band_lists_hold_cells_of_the_cell_size(void) {
    static pkg p;
    _Alignas(64) static uint8_t px_band[HW * (127 + 3) * 4];
    for (int i = -1; i < nsizes; i++) {
        cell c = i < 0 ? (cell){CW, CH} : sizes[i];
        pkg_make(&p, (spec){.size = c});
        src a = {&p, .mode = SRC_MAP};
        lib l = {.latin = &a};
        shr_context *ctx = open_at(i < 0 ? (cell){0, 0} : c, &l, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
        int32_t bh = c.h + 3;
        shr_surface band = {px_band, HW, bh, (size_t)HW * SCREEN_BPP, (size_t)HW * bh * SCREEN_BPP, SHR_PIXEL_FORMAT,
                            1, SHR_MEMORY_CPU, 0};
        shr_screen_desc sd;
        shr_screen_desc_init(&sd);
        sd.width = HW, sd.height = HH, sd.bands = &band, sd.band_count = 1;
        ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_OK);
        ASSERT_EQ_LL(ctx->frame.cmds.cap, 2 * ((HW + c.w - 1) / c.w) * ((bh + c.h - 1) / c.h) + 32);
        harness_close(&H);
    }
    PASS();
}

/* ===== The placeholder ===== */

/* Until the Latin page is read the box stands in, provisional; then the glyph. Without the package the box is final. */
TEST box_stands_in_while_latin_pages_load(void) {
    static pkg p;
    for (int i = 0; i < nsizes; i++) {
        cell c = sizes[i];
        pkg_make(&p, (spec){.size = c});
        src a = {&p, .mode = SRC_READ, .page_result = SHR_IN_PROGRESS};
        lib l = {.latin = &a};
        shr_context *ctx = open_at(c, &l, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
        shr__resolved r;
        ASSERT_EQ_LL(resolve(H.font, 'A', &r), SHR_OK); /* the package is not open yet */
        ASSERT(r.provisional);
        assert_box(&r, c);
        drain(ctx);
        ASSERT_EQ_LL(resolve(H.font, 'A', &r), SHR_OK); /* asks for the page */
        drain(ctx);
        ASSERT_EQ_LL(a.pending, 1);
        ASSERT_EQ_LL(resolve(H.font, 'A', &r), SHR_OK); /* being read */
        ASSERT(r.provisional);
        assert_box(&r, c);
        src_complete(ctx, &a, SHR_OK);
        drain(ctx);
        ASSERT_EQ_LL(resolve(H.font, 'A', &r), SHR_OK);
        assert_glyph(&r, c.h, 4);
        resolve_final(ctx, H.font, 0x100, &r); /* nowhere: the package's U+FFFD */
        assert_glyph(&r, c.h, 3);
        harness_close(&H);

        l = (lib){.latin = &a}, a = (src){&p, .mode = SRC_MAP};
        g_cell = c, g_lib = &l, g_more = NULL;
        harness_open(&H, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cell);
        shr_pl_res_bitmap_font_desc fd; /* a font without open() */
        shr_pl_res_bitmap_font_desc_init(&fd);
        ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(H.ctx, &fd, &H.font), SHR_OK);
        ASSERT_EQ_LL(resolve(H.font, 'A', &r), SHR_OK);
        assert_box(&r, c);
        resolve_final(H.ctx, H.font, 'A', &r);
        assert_box(&r, c);
        harness_close(&H);
    }
    PASS();
}

/* Without U+FFFD, or once its page failed, a glyph found nowhere is the box for good; a cooling page draws it until
 * the page is read. */
TEST box_is_final_when_fffd_is_missing_or_failed(void) {
    static pkg p, q;
    for (int i = 0; i < nsizes; i++) {
        cell c = sizes[i];
        pkg_make(&p, (spec){.size = c, .no_fffd = true});
        src a = {&p, .mode = SRC_MAP};
        lib l = {.latin = &a};
        shr_context *ctx = open_at(c, &l, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
        shr__resolved r;
        resolve_final(ctx, H.font, 0x100, &r);
        assert_box(&r, c);
        resolve_final(ctx, H.font, 'A', &r);
        assert_glyph(&r, c.h, 4);
        harness_close(&H);

        pkg_make(&p, (spec){.size = c});
        q = p; /* a page of another index: its page fails */
        put(q.d + q.page + 16, 1, 4), seal(&q);
        src good = {&p, .mode = SRC_MAP}, bad = {&q, .mode = SRC_MAP};
        l = (lib){.latin = &good, .later = &bad};
        ctx = open_at(c, &l, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
        resolve_final(ctx, H.font, 'A', &r);
        ASSERT(count_events(ctx, SHR_EVENT_RESOURCE_FAILED, NULL) > 0);
        assert_box(&r, c);
        harness_close(&H);

        src cool = {&p, .mode = SRC_READ, .page_result = SHR_E_IO};
        l = (lib){.latin = &cool};
        ctx = open_at(c, &l, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
        for (int k = 0; k < 8 && !cool.page_reads; k++) {
            ASSERT_EQ_LL(resolve(H.font, 'A', &r), SHR_OK);
            settle(ctx);
        }
        ASSERT_EQ_LL(cool.page_reads, 1);
        ASSERT_EQ_LL(resolve(H.font, 'A', &r), SHR_OK); /* cooling down */
        ASSERT(r.provisional);
        assert_box(&r, c);
        cool.page_result = SHR_OK;
        ASSERT_EQ_LL(shr_asset_ready(ctx), SHR_OK);
        resolve_final(ctx, H.font, 'A', &r);
        assert_glyph(&r, c.h, 4);
        harness_close(&H);
    }
    PASS();
}

/* A font opening a package of another cell size than its context's fails it and draws the box: a CJK package of the
 * build's size, a Latin package other than the one the context checked. */
TEST packages_of_another_size_fail_and_draw_the_box(void) {
    static pkg p, other, cjk;
    for (int i = 0; i < nsizes; i++) {
        cell c = sizes[i];
        pkg_make(&p, (spec){.size = c});
        pkg_make(&other, (spec){.size = {CW, CH}});
        pkg_make(&cjk, (spec){.size = {CW, CH}, .role = ROLE_CJK, .locale = "ko"});
        src a = {&p, .mode = SRC_MAP}, b = {&other, .mode = SRC_MAP}, k = {&cjk, .mode = SRC_MAP};
        lib l = {.latin = &a, .later = &b, .cjk = &k};
        shr_context *ctx = open_at(c, &l, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
        shr__resolved r;
        resolve_final(ctx, H.font, 0xAC00, &r);
        shr_event ev = {0};
        ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_RESOURCE_FAILED, &ev), 2);
        ASSERT_EQ_LL(ev.status, SHR_E_UNSUPPORTED);
        ASSERT(l.latin_opens == 2 && a.closes == 1 && b.closes == 1 && k.closes == 1);
        assert_box(&r, c);
        harness_close(&H);
    }
    PASS();
}

/* The stroke is as thick as fontpack's light lines of the shipped face: 1 to width 13, 2 to 27, 3 from 28. */
TEST box_stroke_follows_the_cell_width(void) {
    static pkg p;
    const int32_t widths[] = {6, 13, 14, 27, 28, 32, CW}, strokes[] = {1, 1, 2, 2, 3, 3, stroke(CW)};
    for (int i = 0; i < 7; i++) { /* the last: the build's width, another height */
        cell c = {widths[i], widths[i] == CW ? (CH < 127 ? CH + 1 : CH - 1) : CH};
        ASSERT_EQ_LL(stroke(c.w), strokes[i]);
        pkg_make(&p, (spec){.size = c});
        src a = {&p, .mode = SRC_MAP};
        lib l = {.latin = &a};
        open_at(c, &l, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
        shr_pl_res_bitmap_font *bare;
        shr_pl_res_bitmap_font_desc fd;
        shr_pl_res_bitmap_font_desc_init(&fd);
        ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(H.ctx, &fd, &bare), SHR_OK);
        shr__resolved r;
        ASSERT_EQ_LL(resolve(bare, 0x100, &r), SHR_OK);
        assert_box(&r, c);
        int32_t mid = c.h / 2, t = strokes[i];
        ASSERT(cov(r.buf, 0, mid) == 0 && cov(r.buf, t, mid) == 255 && cov(r.buf, t + 1, mid) == 0);
        ASSERT(cov(r.buf, c.w - 1, mid) == 0 && cov(r.buf, c.w - 1 - t, mid) == 255 && cov(r.buf, c.w - 2 - t, mid) == 0);
        ASSERT(cov(r.buf, c.w / 2, t - 1) == 255 && cov(r.buf, c.w / 2, t) == 0 && cov(r.buf, c.w / 2, c.h - t) == 255);
        ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(bare), SHR_OK);
        harness_close(&H);
    }
    PASS();
}

/* Drawn: the box fills its cell's outline, one cell of a wide cluster, unstyled by bold and italic; replaced by the
 * glyph once its page is read. */
TEST frames_draw_the_box_then_the_glyph(void) {
    static pkg p;
    for (int i = 0; i < nsizes; i++) {
        cell c = sizes[i];
        pkg_make(&p, (spec){.size = c, .no_fffd = true});
        src a = {&p, .mode = SRC_READ, .page_result = SHR_IN_PROGRESS};
        lib l = {.latin = &a};
        shr_context *ctx = open_at(c, &l, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
        shr_lyr *layer = new_layer();
        int32_t rows = rows_of(c) < 2 ? 1 : 2;
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, H.font, rows, 3, NULL), SHR_OK);
        ASSERT_EQ_LL(set_cell(layer, 0, 0, "A", 1, (shr_text_style){WHITE, 0, 0}), SHR_OK);
        ASSERT_EQ_LL(set_cell(layer, 0, 1, "\xEA\xB0\x80", 2, (shr_text_style){WHITE, 0, 0}), SHR_OK);
        if (rows > 1)
            ASSERT_EQ_LL(set_cell(layer, 1, 0, "A", 1, (shr_text_style){WHITE, 0, SHR_STYLE_BOLD | SHR_STYLE_ITALIC}),
                         SHR_OK);
        const uint8_t *s = frame();
        ASSERT(cell_shows(s, c, 0, 0, lit_box) && cell_shows(s, c, 0, 1, lit_box) && cell_shows(s, c, 0, 2, lit_none));
        ASSERT(rows < 2 || cell_shows(s, c, 1, 0, lit_box));
        for (int k = 0; k < 8 && !a.pending; k++) settle(ctx);
        ASSERT_EQ_LL(a.pending, 1);
        src_complete(ctx, &a, SHR_OK);
        settle(ctx);
        s = H.out.shown;
        ASSERT(cell_shows(s, c, 0, 0, lit_a) && cell_shows(s, c, 0, 1, lit_box) && cell_shows(s, c, 0, 2, lit_none));
        ASSERT(rows < 2 || !cell_shows(s, c, 1, 0, lit_box));
        ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
        harness_close(&H);
    }
    PASS();
}

/* A font at another size takes one more buffer, the box: without memory for it the font is not created. */
TEST box_without_memory_fails_the_font(void) {
    static pkg p;
    static fail_alloc fa;
    static shr_allocator al;
    cell c = sizes[0];
    pkg_make(&p, (spec){.size = c});
    src a = {&p, .mode = SRC_MAP};
    lib l = {.latin = &a};
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    for (long budget = 0; budget < 64; budget++) {
        fa = (fail_alloc){-1, 0}, al = fail_allocator(&fa);
        shr_output o;
        shr_context_desc d;
        harness_desc(&H, SHR_OUTPUT_RELEASE_ON_PRESENT, &o, &d);
        d.allocator = &al, d.cell_width = (uint32_t)c.w, d.cell_height = (uint32_t)c.h, d.user = &l, d.open = lib_open;
        ASSERT_EQ_LL(shr_create(&d, &H.ctx), SHR_OK);
        long base = fa.live;
        fa.budget = budget;
        shr_pl_res_bitmap_font *f = NULL;
        shr_status st = shr_pl_res_bitmap_font_create(H.ctx, &fd, &f);
        fa.budget = -1;
        if (st == SHR_OK) ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(f), SHR_OK);
        ASSERT(st == SHR_OK || (st == SHR_E_NO_MEMORY && f == NULL && fa.live == base));
        ctx_free(H.ctx);
        ASSERT_EQ_LL(fa.live, 0);
        if (st == SHR_OK) PASS();
    }
    FAIL();
}

/* ===== The build's size, given or not ===== */

static shr_draw_cmd scene_cmds[2][2048];
static size_t scene_n[2];
static uint8_t scene_shown[2][sizeof(H.out.shown)];

static bool pt_eq(shr_point a, shr_point b) { return a.x == b.x && a.y == b.y; }
static bool rc_eq(shr_rect a, shr_rect b) { return rect_is(a, b.x0, b.y0, b.x1, b.y1); }
static bool src_eq(shr_image_ref a, shr_image_ref b) {
    return a.width == b.width && a.height == b.height && a.stride == b.stride && a.format == b.format;
}

/* The fields each kind defines are equal (buffer memory aside). */
static bool cmd_same(const shr_draw_cmd *a, const shr_draw_cmd *b) {
    if (a->kind != b->kind || a->flags != b->flags) return false;
    switch (a->kind) {
    case SHR_CMD_FILL: return rc_eq(a->dst, b->dst) && a->color == b->color;
    case SHR_CMD_LINE:
        return rc_eq(a->dst, b->dst) && a->color == b->color && pt_eq(a->src_origin, b->src_origin) &&
               rc_eq(a->src_rect, b->src_rect);
    case SHR_CMD_GLYPH:
        return rc_eq(a->dst, b->dst) && a->color == b->color && a->buffer == b->buffer &&
               pt_eq(a->src_origin, b->src_origin) && rc_eq(a->src_rect, b->src_rect) &&
               (!(a->flags & SHR_GLYPH_ON_FILL) || a->bg == b->bg) &&
               (!(a->flags & SHR_GLYPH_ITALIC) || a->slant_axis == b->slant_axis);
    case SHR_CMD_IMAGE:
        return rc_eq(a->dst, b->dst) && a->buffer == b->buffer && pt_eq(a->src_origin, b->src_origin) &&
               rc_eq(a->src_rect, b->src_rect);
    case SHR_CMD_COPY: return rc_eq(a->dst, b->dst) && pt_eq(a->src_origin, b->src_origin) && src_eq(a->src, b->src);
    case SHR_CMD_ROTATE: return rc_eq(a->dst, b->dst) && a->rotation == b->rotation && src_eq(a->src, b->src);
    case SHR_CMD_KEEP_BEGIN: return a->buffer == b->buffer && rc_eq(a->dst, b->dst);
    case SHR_CMD_KEEP_DRAW:
        return a->buffer == b->buffer && rc_eq(a->dst, b->dst) && pt_eq(a->src_origin, b->src_origin);
    case SHR_CMD_BUFFER_REGISTER: return a->buffer == b->buffer && src_eq(a->src, b->src);
    case SHR_CMD_BUFFER_UPDATE: return a->buffer == b->buffer && rc_eq(a->src_rect, b->src_rect);
    case SHR_CMD_KEEP_END: return true;
    default: return a->buffer == b->buffer; /* the releases */
    }
}

/* A scene of text, styles, lines, wide cells and scrolls, its commands and what showed in slot `k`. */
static void scene(cell c, lib *l, int k) {
    g_cell = c, g_lib = l, g_more = tweak_moves;
    harness_open(&H, PRESERVED, tweak_cell);
    harness_font(&H);
    shr_lyr *layer = new_layer();
    const shr_color dark = SHR_RGB(10, 20, 30);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, H.font, 3, HW / CW < 8 ? HW / CW : 8, &dark), SHR_OK);
    scene_n[k] = 0;
    const char *text[3] = {"Ab", "\xEA\xB0\x80x", "\xE2\x94\x80\xEF\xBF\xBD"};
    for (int step = 0; step < 4; step++) {
        for (int32_t r = 0; r < 3; r++) {
            shr_text_style st = {WHITE, SHR_RGB(0, 40 * r, 80), (uint32_t)(r + step) % 3 ? SHR_STYLE_BOLD : SHR_STYLE_ITALIC};
            ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_text(layer, r, step, text[(r + step) % 3], strlen(text[(r + step) % 3]),
                                                     st, NULL, 0, 0, NULL),
                         SHR_OK);
        }
        const shr_text_line lines[2] = {{(uint16_t)step, 2, SHR_LINE_UNDER, SHR_LINE_CURLY, 0, WHITE},
                                        {0, 1, SHR_LINE_STRIKE, SHR_LINE_DASHED, 0, WHITE}};
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_lines(layer, step % 3, lines, 2, NULL), SHR_OK);
        if (step == 2) ASSERT_EQ_LL(shr_pl_lyr_tilemap_scroll(layer, 0, 3, 1, (shr_text_style){WHITE, 0, 0}), SHR_OK);
        frame();
        drain(H.ctx);
        for (size_t i = 0; i < rec.n && scene_n[k] < 2048; i++) scene_cmds[k][scene_n[k]++] = rec.cmds[i];
    }
    memcpy(scene_shown[k], H.out.shown, sizeof(H.out.shown));
    ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
    harness_close(&H);
}

/* 0 x 0 and the build's size given draw the same commands; open() is not called. */
TEST build_size_given_or_not_draws_the_same(void) {
    static pkg p;
    pkg_make(&p, (spec){.size = sizes[0]});
    src a = {&p, .mode = SRC_MAP};
    lib l = {.latin = &a};
    scene((cell){0, 0}, NULL, 0);
    scene((cell){CW, CH}, &l, 1);
    ASSERT_EQ_LL(l.opens + l.logs, 0);
    ASSERT(scene_n[0] > 0 && scene_n[0] == scene_n[1]);
    for (size_t i = 0; i < scene_n[0]; i++) ASSERT(cmd_same(&scene_cmds[0][i], &scene_cmds[1][i]));
    ASSERT(memcmp(scene_shown[0], scene_shown[1], sizeof(scene_shown[0])) == 0);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    sizes_init();
    RUN_TEST(create_checks_the_cell_size);
    RUN_TEST(build_size_lines_are_the_builtin_packages);
    RUN_TEST(create_reports_open_and_read_failures);
    RUN_TEST(create_rejects_packages_it_cannot_use);
    RUN_TEST(rows_and_lines_take_the_cell_size);
    RUN_TEST(frames_draw_cells_of_the_cell_size);
    RUN_TEST(scrolls_move_rows_by_the_cell_height);
    RUN_TEST(rows_keep_and_draw_at_the_cell_size);
    RUN_TEST(band_lists_hold_cells_of_the_cell_size);
    RUN_TEST(box_stands_in_while_latin_pages_load);
    RUN_TEST(box_is_final_when_fffd_is_missing_or_failed);
    RUN_TEST(packages_of_another_size_fail_and_draw_the_box);
    RUN_TEST(box_stroke_follows_the_cell_width);
    RUN_TEST(frames_draw_the_box_then_the_glyph);
    RUN_TEST(box_without_memory_fails_the_font);
    RUN_TEST(build_size_given_or_not_draws_the_same);
    GREATEST_MAIN_END();
}
