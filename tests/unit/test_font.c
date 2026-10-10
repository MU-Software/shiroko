#include "compositor.h"
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

/* resolve() of `res`, its answer copied to *out. */
static shr_status resolve_at(shr__res *res, uint64_t id, uint64_t frame, shr__resolved *out) {
    const shr__resolved *r = NULL;
    shr_status st = res->ops->resolve(res, id, frame, &r);
    if (st == SHR_OK || st == SHR_E_NOT_FOUND) *out = *r;
    return st;
}

/* Resolves in a frame of its own, which then ends. */
static shr_status resolve(shr_pl_res_bitmap_font *f, uint64_t id, shr__resolved *out) {
    shr__res *res = shr__bitmap_font_res(f);
    *out = (shr__resolved){0};
    shr_status st = resolve_at(res, id, ++frame_no, out);
    res->ops->frame_end(res, frame_no);
    return st;
}

/* Resolves several glyphs in one frame, which then ends. */
static void resolve_all(shr_pl_res_bitmap_font *f, const uint64_t *ids, size_t n) {
    shr__res *res = shr__bitmap_font_res(f);
    shr__resolved r;
    frame_no++;
    for (size_t i = 0; i < n; i++) resolve_at(res, ids[i], frame_no, &r);
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

/* The buffer is memory inside `data` (drawn from in place). */
static bool inside(const shr__resolved *r, const uint8_t *data, size_t size) {
    const uint8_t *p = r->buf->mem.pixels;
    return p >= data && p < data + size;
}

static int32_t rw(const shr__resolved *r) { return r->rect.x1 - r->rect.x0; }
static int32_t rh(const shr__resolved *r) { return r->rect.y1 - r->rect.y0; }

/* Coverage of buffer pixel (x, y), widened to 8 bits. */
static int cov(const shr__buf *b, int32_t x, int32_t y) {
    const uint8_t *row = (const uint8_t *)b->mem.pixels + (size_t)y * b->mem.stride;
    return b->mem.format == SHR_FORMAT_A4 ? (row[x / 2] >> (x % 2 ? 0 : 4) & 15) * 17 : row[x];
}

static bool same_image(const shr__resolved *a, const shr__resolved *b) {
    if (rw(a) != rw(b) || rh(a) != rh(b) || a->offset.x != b->offset.x || a->offset.y != b->offset.y)
        return false;
    for (int32_t y = 0; y < rh(a); y++)
        for (int32_t x = 0; x < rw(a); x++)
            if (cov(a->buf, a->rect.x0 + x, a->rect.y0 + y) != cov(b->buf, b->rect.x0 + x, b->rect.y0 + y))
                return false;
    return true;
}

static shr_status last_failure(shr_context *ctx) {
    shr_event ev = {.status = SHR_OK};
    count_events(ctx, SHR_EVENT_RESOURCE_FAILED, &ev);
    return ev.status;
}

/* ===== Synthetic packages =====
 * One CWxCH instance (A4 or A8) of SY_NG 4x4 glyphs on each of two 64x64 pages. Page 0: record 0 (cp0) at (SY_X0, 1).
 * Page 1, trimmed to SY_H1 rows: record 0 (cp1 other than 'B', which is blank, and the sequences A+U+0301 and
 * B+U+0301) two cells wide at (0, 4). The other records repeat record 0 of their page and are mapped from U+F0002 on,
 * so that the records compress. Atlases hold 0x5A around the glyphs of 0xFF. A package is assembled from these raw
 * contents, stored or with ctri and the pages zstd, then sealed. */
enum {
    SY_W = 64, SY_H = 64, SY_H1 = 8, SY_NG = 16, SY_NMAP = 32, SY_X0 = 2, SY_BASE = CH - 4,
    SY_A4 = 1, SY_A8 = 2,
    SY_SLOT = SY_H * SY_W / 2, SY_RECS = 16 * SY_NG, SY_RES = SY_SLOT + SY_RECS, /* A4: a slot, the records, both */
    SY_G2 = 1 << 12, /* page 1, record 0 */
    SY_D = 2624,     /* the data blocks of the default ctri: 'A' in block 1, U+F0000..F001F in blocks 2 and 3 */
    SY_BOX = 4608, SY_CAP = 16384
};
enum { SY_STORED, SY_ZSTD };
#ifdef SHR_ZSTD
#define SY_METHODS 2
#else
#define SY_METHODS 1
#endif
#define FCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static const char *const sy_types[BOX_COUNT] = {"mani", "strs", "srcs", "inst", "ctri", "seqs", "pool", "ptab", "covr"};

typedef struct sy_raw {
    uint32_t features;
    int32_t len[BOX_COUNT]; /* payload bytes; -1 = absent */
    uint8_t box[BOX_COUNT][SY_BOX];
    const uint8_t *payload[BOX_COUNT]; /* instead of `box` */
    uint32_t map[40][2], nmap;         /* the scalars of ctri and their glyph values */
    uint8_t atlas[2][SY_H * SY_W], recs[2][SY_RECS];
    const uint8_t *stream[5]; /* zstd streams to use instead: ctri, then the atlas and records of each page */
    size_t stream_n[5];
    uint32_t pad, shift, cshift; /* bytes after page box 0; added before it and before ctri when stored */
    char extra[5];               /* an extra indexed box after the others */
    uint16_t extra_flags;
} sy_raw;

typedef struct spkg {
    _Alignas(256) uint8_t d[SY_CAP];
    size_t n;
} spkg;

static void put(uint8_t *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> 8 * i);
}

static uint8_t *sy_entry(uint8_t *d, const char *type) {
    uint8_t *e = d + 152;
    while (memcmp(e, type, 4)) e += 32;
    return e;
}

static uint8_t *sy_box(uint8_t *d, const char *type) { return d + shr__rd64(sy_entry(d, type) + 8); }
static uint8_t *sy_ptab(uint8_t *d, int i) { return sy_box(d, "ptab") + 16 + SHR_PKG_PTAB * i; }
static uint8_t *sy_page(uint8_t *d, int i) { return d + shr__rd64(sy_ptab(d, i)); }
static uint32_t sy_size(uint8_t *d, int i) { return shr__rd32(sy_ptab(d, i) + 16); } /* of page box i */

/* Recomputes the checksums of a package of `n` bytes: pages, index entries, the index, the header; checksums of
 * ranges outside the package stay. */
static void seal(uint8_t *d, size_t n) {
    uint64_t sn = shr__rd32(d + 28);
    for (int pass = 0; pass < 2; pass++)
        for (uint64_t e = 152; e + 32 <= 128 + sn && e + 32 <= n; e += 32) {
            uint64_t off = shr__rd64(d + e + 8), len = shr__rd32(d + e + 16);
            if (off > n || n - off < len) continue;
            if (pass) put(d + e + 24, XXH3_64bits(d + off, len), 8);
            for (uint64_t r = off + 16; !pass && !memcmp(d + e, "ptab", 4) && r + 24 <= off + len; r += 24) {
                uint64_t po = shr__rd64(d + r), pl = shr__rd32(d + r + 16);
                if (po <= n && n - po >= pl) put(d + r + 8, XXH3_64bits(d + po, pl), 8);
            }
        }
    if (128 + sn <= n) put(d + 80, XXH3_64bits(d + 128, sn), 8);
    put(d + 120, XXH3_64bits(d, 120), 8);
}

/* A ctri of `n` scalars and their glyph values in `out`: a level-2 block per 1024 scalars and a data block per 16
 * scalars in use, numbered in order of appearance. Returns its size. */
static size_t sy_ctri(uint8_t *out, uint32_t (*map)[2], size_t n) {
    static uint16_t l1[1088], l2[8][64];
    static uint32_t data[16][16];
    uint32_t nl2 = 1, nd = 1;
    memset(l1, 0, sizeof(l1)), memset(l2, 0, sizeof(l2)), memset(data, 0xFF, sizeof(data));
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = map[i][0];
        ASSERT_EQ_LL(nl2 < 8 && nd < 16, 1);
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

/* Maps one more scalar of `r`. */
static void sy_map(sy_raw *r, uint32_t cp, uint32_t gid) {
    r->map[r->nmap][0] = cp, r->map[r->nmap++][1] = gid;
    r->len[BOX_CTRI] = (int32_t)sy_ctri(r->box[BOX_CTRI], r->map, r->nmap);
}

static void sy_init(sy_raw *r, uint8_t role, const char *locale, uint32_t cp0, uint32_t cp1, uint8_t fmt) {
    static const int32_t lens[BOX_COUNT] = {36, 8, 48, 48, 0, 24, 16, 2 * SHR_PKG_PTAB, -1};
    static const uint32_t pool[4] = {'A', 0x301, 'B', 0x301};
    memset(r, 0, sizeof(*r));
    memcpy(r->len, lens, sizeof(lens));
    r->features = fmt | 4u;
    uint8_t *m = r->box[BOX_MANI], *in = r->box[BOX_INST];
    m[0] = role;
    memcpy(m + 4, locale, strlen(locale));
    put(m + 16, 4, 4), put(m + 20, 4, 4), put(m + 24, 4, 4), put(m + 28, 2 * SY_NG, 4), put(m + 32, 2, 4);
    memcpy(r->box[BOX_STRS], "synthpkg", 8);
    in[3] = fmt;
    put(in + 4, CH, 2), put(in + 6, CW, 2), put(in + 12, SY_BASE, 2), put(in + 14, CH - 2, 2), put(in + 16, CH / 2, 2);
    put(in + 36, SY_W, 2), put(in + 38, SY_H, 2);
    r->map[0][0] = cp0, r->map[1][0] = cp1, r->map[1][1] = cp1 == 'B' ? SHR_GID_BLANK : SY_G2;
    for (uint32_t i = 2; i < SY_NMAP; i++) r->map[i][0] = 0xF0000 + i, r->map[i][1] = (i / SY_NG) << 12 | i % SY_NG;
    r->nmap = SY_NMAP;
    r->len[BOX_CTRI] = (int32_t)sy_ctri(r->box[BOX_CTRI], r->map, r->nmap);
    for (int i = 0; i < 2; i++) {
        uint8_t *q = r->box[BOX_SEQS] + 12 * i;
        q[0] = 2, q[1] = 1, put(q + 4, 2 * (uint64_t)i, 4), put(q + 8, SY_G2, 4);
    }
    for (int i = 0; i < 4; i++) put(r->box[BOX_POOL] + 4 * i, pool[i], 4);
    uint32_t stride = fmt == SY_A4 ? SY_W / 2 : SY_W;
    for (int i = 0; i < 2; i++) { /* record 0 at x SY_X0 (A8: 0) on the baseline; on page 1 two cells wide, lower */
        uint32_t x = i || fmt == SY_A8 ? 0 : SY_X0, y = i ? 4 : 1;
        memset(r->atlas[i], 0x5A, sizeof(r->atlas[i]));
        for (uint32_t k = 0; k < 4; k++)
            memset(r->atlas[i] + (y + k) * stride + (fmt == SY_A4 ? x / 2 : x), 0xFF, fmt == SY_A4 ? 2 : 4);
        for (int g = 0; g < SY_NG; g++) {
            uint8_t *e = r->recs[i] + 16 * g;
            put(e, x, 2), put(e + 2, y, 2);
            e[4] = 4, e[5] = 4, e[6] = (uint8_t)(i ? 0 : 1), e[7] = (uint8_t)(i ? SY_BASE - 3 : SY_BASE);
            e[8] = (uint8_t)(fmt | (i ? 8 : 4));
        }
    }
}

/* A box at `at` of `n` payload bytes; returns its end. */
static size_t box_put(uint8_t *d, size_t at, const char *type, uint32_t flags, uint32_t raw, const uint8_t *data,
                      size_t n) {
    put(d + at, 16 + n, 4), memcpy(d + at + 4, type, 4), put(d + at + 10, flags, 2), put(d + at + 12, raw, 4);
    memcpy(d + at + 16, data, n);
    return at + 16 + n;
}

/* A free box before a box whose payload starts at a multiple of `align` bytes, `shift` bytes later; returns its end. */
static size_t free_put(uint8_t *d, size_t at, size_t head, size_t align, size_t shift) {
    static const uint8_t zero[512];
    size_t g = (align - (at + head) % align) % align;
    if (g && g < 16) g += align;
    g += shift;
    return g ? box_put(d, at, "free", 0, (uint32_t)g - 16, zero, g - 16) : at;
}

#ifdef SHR_ZSTD
#include <zstd.h>

static uint8_t zbuf[3][SY_CAP];

/* Stream `k` of `r` (its replacement, else `n` bytes of `src` compressed into `out`). */
static size_t zpack(const sy_raw *r, int k, uint8_t *out, const uint8_t *src, size_t n, const uint8_t **data) {
    if (r->stream[k]) return *data = r->stream[k], r->stream_n[k];
    size_t z = ZSTD_compress(out, SY_CAP, src, n, 19);
    ASSERT_EQ_LL(!ZSTD_isError(z) && z < n, 1);
    return *data = out, z;
}
#endif

/* The package of `r` in `d` (`cap` bytes); returns its size. */
static size_t assemble_to(uint8_t *d, size_t cap, const sy_raw *r, int method) {
    static const uint8_t zero[8];
    uint32_t stride = r->box[BOX_INST][3] == SY_A4 ? SY_W / 2 : SY_W, count = r->extra[0] ? 1 : 0;
    const uint32_t heights[2] = {SY_H, SY_H1};
    size_t ptab = 0;
    memset(d, 0, cap);
    for (int k = 0; k < BOX_COUNT; k++) count += r->len[k] >= 0;
    size_t at = 128 + 24 + 32 * (size_t)count;
    uint8_t *e = d + 152;
    for (int k = 0; k < BOX_COUNT + 1; k++) {
        bool extra = k == BOX_COUNT;
        if (extra ? !r->extra[0] : r->len[k] < 0) continue;
        const char *type = extra ? r->extra : sy_types[k];
        const uint8_t *data = extra ? zero : r->payload[k] ? r->payload[k] : r->box[k];
        uint32_t raw = extra ? 8 : (uint32_t)r->len[k], flags = extra ? r->extra_flags : k == BOX_COVR ? 0 : 1;
        size_t n = raw;
#ifdef SHR_ZSTD
        if (k == BOX_CTRI && method) n = zpack(r, 0, zbuf[0], data, raw, &data), flags |= 0x10;
#endif
        if (k == BOX_CTRI && !(flags >> 4)) at = free_put(d, at, 16, 64, r->cshift); /* stored: data blocks aligned */
        memcpy(e, type, 4), put(e + 4, flags, 2), put(e + 8, at, 8), put(e + 16, 16 + n, 4), put(e + 20, raw, 4);
        e += 32;
        if (k == BOX_PTAB) ptab = at;
        at = box_put(d, at, type, flags, raw, data, n);
    }
    for (int i = 0; i < 2; i++) {
        size_t an = heights[i] * stride, rn = SY_RECS;
        const uint8_t *a = r->atlas[i], *rc = r->recs[i];
        uint32_t m = 0;
#ifdef SHR_ZSTD
        if (method) an = zpack(r, 1 + 2 * i, zbuf[1], a, an, &a), rn = zpack(r, 2 + 2 * i, zbuf[2], rc, rn, &rc), m = 1;
#endif
        if (!m) at = free_put(d, at, 24, 256, i ? 0 : r->shift); /* a stored atlas starts at a multiple of 256 bytes */
        size_t size = 24 + an + rn + (i ? 0 : r->pad);
        uint8_t *b = d + at, *t = d + ptab + 16 + SHR_PKG_PTAB * i;
        put(b, size, 4), memcpy(b + 4, "page", 4), put(b + 10, 1 | m << 4, 2);
        put(b + 12, 8 + heights[i] * stride + SY_RECS, 4), put(b + 16, (uint64_t)i, 4), put(b + 20, an, 4);
        memcpy(b + 24, a, an), memcpy(b + 24 + an, rc, rn);
        put(t, at, 8), put(t + 16, size, 4), put(t + 20, SY_NG, 2), put(t + 22, heights[i], 2);
        at += size;
    }
    memcpy(d, "\x80\0\0\0shrf", 8), put(d + 10, 1, 2), put(d + 12, 112, 4), put(d + 16, 6, 2);
    put(d + 20, r->features | (method ? 8u : 0u), 4), put(d + 28, 24 + 32 * (uint64_t)count, 4), put(d + 32, at, 8);
    put(d + 40, 128, 8), memcpy(d + 48, shr__text_profile_id, 32);
    memcpy(d + 128 + 4, "sidx", 4), put(d + 128, 24 + 32 * (uint64_t)count, 4), put(d + 138, 1, 2);
    put(d + 140, 8 + 32 * (uint64_t)count, 4), put(d + 144, count, 4);
    seal(d, at);
    return at;
}

static void assemble(spkg *p, const sy_raw *r, int method) { p->n = assemble_to(p->d, SY_CAP, r, method); }

static void synth_m(spkg *p, uint8_t role, const char *locale, uint32_t cp0, uint32_t cp1, uint8_t fmt, int method) {
    static sy_raw r;
    sy_init(&r, role, locale, cp0, cp1, fmt);
    assemble(p, &r, method);
}

static void synth_as(spkg *p, uint8_t role, const char *locale, uint32_t cp0, uint32_t cp1, uint8_t fmt) {
    synth_m(p, role, locale, cp0, cp1, fmt, SY_STORED);
}

static void synth(spkg *p) { synth_as(p, ROLE_LATIN, "", 'A', 'B', SY_A4); }

/* A change of a package: of its raw contents before it is assembled, or of its bytes after (then sealed again). */
enum { AT_FILE, AT_ENTRY, AT_BOX, AT_PTAB, AT_PAGE, AT_LEN, AT_RAW, AT_ATLAS, AT_RECS };

typedef struct mutation {
    uint8_t where, which; /* box or page */
    uint16_t at;
    uint8_t width;
    uint64_t value;
    shr_status want;
} mutation;

static void mutate(spkg *p, const sy_raw *base, int method, const mutation *m) {
    static sy_raw r;
    r = *base;
    uint8_t *raw[] = {[AT_RAW] = r.box[m->which % BOX_COUNT], [AT_ATLAS] = r.atlas[m->which % 2],
                      [AT_RECS] = r.recs[m->which % 2]};
    if (m->where == AT_LEN) r.len[m->which] = (int32_t)m->value;
    if (m->where > AT_LEN) put(raw[m->where] + m->at, m->value, m->width);
    assemble(p, &r, method);
    if (m->where >= AT_LEN) return;
    uint8_t *d = p->d, *at[] = {d, NULL, NULL, NULL, NULL};
    if (m->where == AT_ENTRY) at[AT_ENTRY] = sy_entry(d, sy_types[m->which]);
    if (m->where == AT_BOX) at[AT_BOX] = sy_box(d, sy_types[m->which]);
    if (m->where == AT_PTAB) at[AT_PTAB] = sy_ptab(d, m->which);
    if (m->where == AT_PAGE) at[AT_PAGE] = sy_page(d, m->which);
    put(at[m->where] + m->at, m->value, m->width);
    seal(d, p->n);
}

/* How a latin package ends: the RESOURCE_FAILED status, SHR_OK when 'A' (page 0) and A+U+0301 (page 1) resolve
 * from it. */
static shr_status pkg_status(shr_context *ctx, tsrc *s) {
    lib l = {.src = {[ROLE_LATIN] = s}};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, NULL);
    shr__resolved r, q;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT_EQ_LL(load(ctx, f, glyph2(f), &q), SHR_OK);
    shr_status st = last_failure(ctx);
    ASSERT_EQ_LL(r.provisional, st == SHR_E_IO); /* a package cooling down after I/O errors is retried */
    if (st == SHR_OK && (rh(&r) != 4 || rh(&q) != 4)) st = SHR_E_NOT_FOUND;
    font_free_now(ctx, f);
    return st;
}

static int lit(const uint8_t *buf, int x0, int x1, int y0, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) n += px(buf, x, y) != 0;
    return n;
}

static uint64_t cache_limit;
static void tweak_cache(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)drv;
    d->page_cache_bytes = cache_limit;
}

/* A page cache for one A4 page of package `d` being read, too small for a second one besides it. */
static uint64_t one_page(spkg *d) { return SY_RES + sy_size(d->d, 0) + 64; }

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

static void tweak_oom_stride(shr_context_desc *d, shr_framebuffer_driver *drv) {
    d->allocator = &oom_allocator;
    drv->caps.stride_align = 64; /* mapped pages are decoded into slots */
}

/* Every allocation of loading a package through reads or a mapping (the zstd decoder too), interning and pinning
 * may fail. */
TEST test_out_of_memory(void) {
    oom_allocator = fail_allocator(&oom);
    for (int method = 0; method < SY_METHODS; method++) {
        spkg d;
        synth_m(&d, ROLE_LATIN, "", 'A', 'B', SY_A4, method);
        for (int mode = 0; mode < 3; mode++) { /* read, mapped, mapped where the driver cannot draw from */
            bool done = false;
            for (long budget = 0; !done; budget++) {
                oom = (fail_alloc){-1, 0};
                harness h;
                shr_context *ctx =
                    harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, mode == 2 ? tweak_oom_stride : tweak_oom);
                oom.budget = budget;
                tsrc s = {d.d, d.n, .mode = mode ? SRC_MAP : SRC_READ};
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
                    done = st == SHR_OK && inside(&r, d.d, d.n) == (mode == 1 && method == SY_STORED) &&
                           rh(&r) == 4 && (method == SY_STORED) == !f->zdc;
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
    }
    PASS();
}

/* A mapped package, or a page of one, that runs out of memory waits and is decoded again later. */
TEST test_mapped_waits_for_memory(void) {
    oom_allocator = fail_allocator(&oom);
    for (int method = 0; method < SY_METHODS; method++) {
        spkg d;
        synth_m(&d, ROLE_LATIN, "", 'A', 'B', SY_A4, method);
        for (int step = 0; step < 2; step++) { /* the package, then its page */
            oom = (fail_alloc){-1, 0};
            harness h;
            shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_oom);
            tsrc s = {d.d, d.n, .mode = SRC_MAP};
            lib l = {.src = {[ROLE_LATIN] = &s}};
            shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
            shr__res *res = shr__bitmap_font_res(f);
            uint64_t id = glyph1(f, 'A');
            shr__resolved r;
            resolve(f, id, &r);
            if (step) {
                ASSERT(res->ops->pump(res));
                ASSERT_EQ_LL(f->pkg[ROLE_LATIN].state, PKG_READY);
                resolve(f, id, &r);
            }
            oom.budget = 0;
            res->ops->pump(res);
            ASSERT_EQ_LL(step ? f->pkg[ROLE_LATIN].pages[0]->state : f->pkg[ROLE_LATIN].state,
                         step ? PAGE_ABSENT : PKG_LOADING);
            ASSERT(!res->ops->has_work(res)); /* waits for memory, no polling */
            res->ops->pump(res);
            ASSERT_EQ_LL(s.closes, 0);
            oom.budget = -1;
            ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK); /* a later frame tries again */
            ASSERT(!r.provisional && rh(&r) == 4 && inside(&r, d.d, d.n) == (method == SY_STORED));
            ASSERT_EQ_LL(last_failure(ctx), SHR_OK);
            harness_close(&h);
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
        ASSERT(!a.provisional && rh(&a) > 4 && inside(&a, shr__builtin_package, shr__builtin_package_size));
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
        ASSERT_EQ_LL(load(ctx, f, ' ', &a), SHR_E_NOT_FOUND); /* blank: nothing to draw */
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
    cls.glyph_cp = big; /* the table lookups take scalars only */
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, &a, 1, &cls, &id), SHR_E_INVALID_ARG);
    shr__classify(&zw, 1, &cls);
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, &zw, 1, &cls, &id), SHR_E_NOT_FOUND); /* invisible */
    uint32_t mark = 0x301, sp[2] = {' ', 0x301};
    ASSERT_EQ_LL(glyph(f, &mark, 1), 0xFFFD); /* replacement */
    shr__classify(sp, 1, &cls);
    ASSERT_EQ_LL(shr__bitmap_font_glyph(f, sp, 1, &cls, &id), SHR_E_NOT_FOUND); /* a space: no glyph command */
    ASSERT_EQ_LL(glyph1(f, 'A'), 'A');
    ASSERT_EQ_LL(glyph1(f, 0x1F600), 0x1F600 | SHR_ID_EMOJI);

    uint32_t s1[2] = {'e', 0x301}, s2[2] = {'a', 0x301};
    uint64_t i1 = glyph(f, s1, 2), i2 = glyph(f, s2, 2);
    ASSERT_EQ_LL(i1, SHR_ID_CLUSTER);
    ASSERT_EQ_LL(i2, SHR_ID_CLUSTER | 1);
    ASSERT_EQ_LL(glyph(f, sp, 2), SHR_ID_CLUSTER | 2); /* a space with a mark is a cluster */
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

/* Runs `cases` on stored packages, read and mapped; `pages` expects the page of 'A' or A+U+0301 to fail (else the
 * package): read once, a checksum failure four times. */
static void run_cases(const mutation *cases, size_t n, const sy_raw *base, bool pages) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    for (int mode = SRC_READ; mode <= SRC_MAP; mode++)
        for (size_t i = 0; i < n; i++) {
            mutate(&d, base, SY_STORED, &cases[i]);
            tsrc s = {d.d, d.n, .mode = mode};
            shr_status st = pkg_status(ctx, &s);
            if (st != cases[i].want) fprintf(stderr, "mode %d case %zu\n", mode, i);
            ASSERT_EQ_LL(st, cases[i].want);
            if (pages && mode == SRC_READ) ASSERT_EQ_LL(s.calls, 3 + (st == SHR_E_CHECKSUM ? 4 : 1) + 1);
        }
    harness_close(&h);
}

TEST test_package_validation(void) {
    static const mutation cases[] = {
        {AT_FILE, 0, 0, 1, 'X', SHR_E_FORMAT},                           /* magic */
        {AT_FILE, 0, 8, 2, 1, SHR_E_FORMAT},                             /* header box: version, flags, raw_size */
        {AT_FILE, 0, 10, 2, 0, SHR_E_FORMAT},
        {AT_FILE, 0, 12, 4, 113, SHR_E_FORMAT},
        {AT_FILE, 0, 16, 2, 4, SHR_E_FORMAT},                            /* format version */
        {AT_FILE, 0, 16, 2, 5, SHR_E_FORMAT},
        {AT_FILE, 0, 18, 2, 1, SHR_E_FORMAT},
        {AT_FILE, 0, 20, 4, 0x15, SHR_E_UNSUPPORTED},                    /* unknown required feature */
        {AT_FILE, 0, 24, 4, 0xFFFFFFFF, SHR_OK},                         /* optional features */
        {AT_FILE, 0, 88, 1, 1, SHR_E_FORMAT},                            /* reserved */
        {AT_FILE, 0, 119, 1, 1, SHR_E_FORMAT},
        {AT_FILE, 0, 32, 8, 1 << 20, SHR_E_FORMAT},                      /* file size */
        {AT_FILE, 0, 40, 8, 129, SHR_E_FORMAT},                          /* sidx offset */
        {AT_FILE, 0, 28, 4, 0, SHR_E_FORMAT},                            /* sidx size */
        {AT_FILE, 0, 28, 4, 25, SHR_E_FORMAT},
        {AT_FILE, 0, 28, 4, 24 + 32 * 33, SHR_E_FORMAT},
        {AT_FILE, 0, 128, 4, 0, SHR_E_FORMAT},                           /* sidx box */
        {AT_FILE, 0, 132, 1, 'X', SHR_E_FORMAT},
        {AT_FILE, 0, 136, 2, 1, SHR_E_FORMAT},
        {AT_FILE, 0, 138, 2, 0, SHR_E_FORMAT},
        {AT_FILE, 0, 140, 4, 0, SHR_E_FORMAT},
        {AT_FILE, 0, 144, 4, 7, SHR_E_FORMAT},
        {AT_FILE, 0, 148, 4, 1, SHR_E_FORMAT},
        {AT_ENTRY, BOX_MANI, 16, 4, 15, SHR_E_FORMAT},                   /* entries: size, order, range */
        {AT_ENTRY, BOX_MANI, 8, 8, 400, SHR_E_FORMAT},
        {AT_ENTRY, BOX_STRS, 8, 8, 440, SHR_E_FORMAT},
        {AT_ENTRY, BOX_PTAB, 8, 8, 1ull << 40, SHR_E_FORMAT},
        {AT_ENTRY, BOX_PTAB, 16, 4, 1u << 20, SHR_E_FORMAT},
        {AT_ENTRY, BOX_STRS, 0, 4, FCC('s', 'h', 'r', 'f'), SHR_E_FORMAT}, /* never indexed */
        {AT_ENTRY, BOX_STRS, 0, 4, FCC('s', 'i', 'd', 'x'), SHR_E_FORMAT},
        {AT_ENTRY, BOX_STRS, 0, 4, FCC('p', 'a', 'g', 'e'), SHR_E_FORMAT},
        {AT_ENTRY, BOX_STRS, 0, 4, FCC('f', 'r', 'e', 'e'), SHR_E_FORMAT},
        {AT_ENTRY, BOX_STRS, 0, 4, FCC('z', 'z', 'z', 'z'), SHR_E_UNSUPPORTED}, /* unknown and REQUIRED */
        {AT_ENTRY, BOX_SRCS, 0, 8, FCC('z', 'z', 'z', 'z'), SHR_OK},           /* unknown and optional: skipped */
        {AT_ENTRY, BOX_STRS, 0, 4, FCC('m', 'a', 'n', 'i'), SHR_E_FORMAT},     /* duplicate */
        {AT_ENTRY, BOX_MANI, 4, 2, 0x21, SHR_E_UNSUPPORTED},             /* method 2 */
        {AT_ENTRY, BOX_MANI, 4, 2, 0xF1, SHR_E_UNSUPPORTED},
        {AT_ENTRY, BOX_MANI, 4, 2, 0x03, SHR_E_FORMAT},                  /* flags */
        {AT_ENTRY, BOX_MANI, 4, 2, 0x101, SHR_E_FORMAT},
        {AT_ENTRY, BOX_MANI, 4, 2, 0, SHR_E_FORMAT},                     /* not REQUIRED */
        {AT_ENTRY, BOX_MANI, 4, 2, 0x11, SHR_E_FORMAT},                  /* zstd where only stored is allowed */
        {AT_ENTRY, BOX_CTRI, 4, 2, 0x11, SHR_E_FORMAT},                  /* zstd without required feature bit3 */
        {AT_ENTRY, BOX_MANI, 6, 2, 1, SHR_E_FORMAT},                     /* version */
        {AT_ENTRY, BOX_MANI, 20, 4, 35, SHR_E_FORMAT},                   /* stored raw_size */
        {AT_ENTRY, BOX_MANI, 20, 4, 37, SHR_E_FORMAT},
        {AT_ENTRY, BOX_MANI, 0, 8, FCC('z', 'z', 'z', 'z'), SHR_E_FORMAT}, /* missing boxes */
        {AT_ENTRY, BOX_INST, 0, 8, FCC('z', 'z', 'z', 'z'), SHR_E_FORMAT},
        {AT_ENTRY, BOX_CTRI, 0, 8, FCC('z', 'z', 'z', 'z'), SHR_E_FORMAT},
        {AT_ENTRY, BOX_PTAB, 0, 8, FCC('z', 'z', 'z', 'z'), SHR_E_FORMAT},
        {AT_BOX, BOX_MANI, 0, 4, 53, SHR_E_FORMAT},                      /* box header unlike its entry */
        {AT_BOX, BOX_MANI, 4, 1, 'X', SHR_E_FORMAT},
        {AT_BOX, BOX_MANI, 8, 2, 1, SHR_E_FORMAT},
        {AT_BOX, BOX_MANI, 10, 2, 0, SHR_E_FORMAT},
        {AT_BOX, BOX_MANI, 12, 4, 35, SHR_E_FORMAT},
        {AT_LEN, BOX_MANI, 0, 0, 35, SHR_E_FORMAT},                      /* mani */
        {AT_RAW, BOX_MANI, 1, 1, 1, SHR_E_FORMAT},
        {AT_RAW, BOX_MANI, 0, 1, 0, SHR_E_FORMAT},                       /* role */
        {AT_RAW, BOX_MANI, 0, 1, 6, SHR_E_FORMAT},
        {AT_RAW, BOX_MANI, 0, 1, ROLE_CJK, SHR_E_FORMAT},                /* not the role of its file name */
        {AT_RAW, BOX_MANI, 4, 1, 0x1F, SHR_E_FORMAT},                    /* locale */
        {AT_RAW, BOX_MANI, 4, 1, 0x7F, SHR_E_FORMAT},
        {AT_RAW, BOX_MANI, 4, 1, 'k', SHR_OK},                           /* a latin package's locale is not checked */
        {AT_RAW, BOX_MANI, 16, 4, 9, SHR_E_FORMAT},                      /* string references */
        {AT_RAW, BOX_MANI, 24, 4, 5, SHR_E_FORMAT},
        {AT_LEN, BOX_STRS, 0, 0, (uint64_t)-1, SHR_E_FORMAT},
        {AT_RAW, BOX_MANI, 28, 4, 5000000, SHR_E_FORMAT},                /* glyph count */
        {AT_RAW, BOX_MANI, 28, 4, 0, SHR_E_FORMAT},
        {AT_LEN, BOX_INST, 0, 0, 47, SHR_E_FORMAT},                      /* box sizes */
        {AT_LEN, BOX_SRCS, 0, 0, 47, SHR_E_FORMAT},
        {AT_LEN, BOX_COVR, 0, 0, 35, SHR_E_FORMAT},
        {AT_LEN, BOX_COVR, 0, 0, 36, SHR_OK},
        {AT_LEN, BOX_SRCS, 0, 0, (uint64_t)-1, SHR_OK},
        {AT_RAW, BOX_SRCS, 0, 4, 9, SHR_E_FORMAT},                       /* source strings */
        {AT_RAW, BOX_SRCS, 44, 4, 9, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 0, 2, 1, SHR_E_FORMAT},                       /* inst */
        {AT_RAW, BOX_INST, 2, 1, 1, SHR_E_FORMAT},                       /* a style other than regular */
        {AT_RAW, BOX_INST, 3, 1, 3, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 3, 1, 0, SHR_E_FORMAT},
        {AT_FILE, 0, 20, 4, 6, SHR_E_FORMAT},                            /* A4 instance without the feature */
        {AT_RAW, BOX_INST, 4, 2, 0, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 4, 2, 1025, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 6, 2, 0, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 6, 2, 1025, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 12, 2, 0xFFFF, SHR_E_FORMAT},                 /* line metrics */
        {AT_RAW, BOX_INST, 12, 2, CH + 1, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 14, 2, 0xFFFF, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 14, 2, CH, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 16, 2, 0xFFFF, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 16, 2, CH, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 18, 2, 2, SHR_E_FORMAT},                      /* unknown raster flag */
        {AT_RAW, BOX_INST, 18, 2, 1, SHR_OK},
        {AT_RAW, BOX_INST, 40, 1, 1, SHR_E_FORMAT},                      /* reserved bytes after the page shape */
        {AT_RAW, BOX_INST, 47, 1, 1, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 36, 2, 32, SHR_E_FORMAT},                     /* page shape */
        {AT_RAW, BOX_INST, 36, 2, 1024, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 36, 2, 96, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 38, 2, 32, SHR_E_FORMAT},
        {AT_RAW, BOX_INST, 6, 2, CW + 1, SHR_E_UNSUPPORTED},             /* another cell size */
        {AT_RAW, BOX_INST, 4, 2, CH + 1, SHR_E_UNSUPPORTED},
        {AT_LEN, BOX_CTRI, 0, 0, SHR_PKG_CTRI - 1, SHR_E_FORMAT},         /* ctri */
        {AT_RAW, BOX_CTRI, 0, 4, 0, SHR_E_FORMAT},                        /* level-2 blocks */
        {AT_RAW, BOX_CTRI, 0, 4, 65537, SHR_E_FORMAT},
        {AT_RAW, BOX_CTRI, 0, 4, 4, SHR_E_FORMAT},                        /* then the data blocks start later */
        {AT_RAW, BOX_CTRI, 4, 4, 0, SHR_E_FORMAT},                        /* data blocks */
        {AT_RAW, BOX_CTRI, 4, 4, 65537, SHR_E_FORMAT},
        {AT_RAW, BOX_CTRI, 4, 4, 5, SHR_E_FORMAT},
        {AT_RAW, BOX_CTRI, 8, 4, 1, SHR_E_FORMAT},                        /* reserved */
        {AT_RAW, BOX_CTRI, 12, 4, 1, SHR_E_FORMAT},
        {AT_LEN, BOX_CTRI, 0, 0, SY_D + 5 * 64, SHR_E_FORMAT},           /* raw size */
        {AT_RAW, BOX_CTRI, SY_D - 1, 1, 1, SHR_E_FORMAT},                 /* padding */
        {AT_RAW, BOX_CTRI, 16 + 2 * 1087, 2, 3, SHR_E_FORMAT},            /* level 1 */
        {AT_RAW, BOX_CTRI, 16 + 2 * 1087, 2, 2, SHR_OK},                  /* shared level-2 block */
        {AT_RAW, BOX_CTRI, SHR_PKG_CTRI + 128 + 2 * 63, 2, 4, SHR_E_FORMAT}, /* level 2 */
        {AT_RAW, BOX_CTRI, SHR_PKG_CTRI + 128 + 2 * 63, 2, 3, SHR_OK},    /* shared data block */
        {AT_RAW, BOX_CTRI, SHR_PKG_CTRI + 2 * 5, 2, 1, SHR_E_FORMAT},     /* level-2 block 0 all 0 */
        {AT_RAW, BOX_CTRI, SY_D + 4 * 15, 4, 0, SHR_E_FORMAT},            /* data block 0 all MISS */
        {AT_RAW, BOX_CTRI, SY_D + 4 * 15, 4, SHR_GID_BLANK, SHR_E_FORMAT},
        {AT_LEN, BOX_POOL, 0, 0, 15, SHR_E_FORMAT},                      /* seqs and pool */
        {AT_FILE, 0, 20, 4, 1, SHR_E_FORMAT},
        {AT_LEN, BOX_POOL, 0, 0, (uint64_t)-1, SHR_E_FORMAT},
        {AT_LEN, BOX_SEQS, 0, 0, 23, SHR_E_FORMAT},
        {AT_RAW, BOX_POOL, 4, 4, 0x110000, SHR_E_FORMAT},
        {AT_RAW, BOX_POOL, 4, 4, 0xDC00, SHR_E_FORMAT},
        {AT_RAW, BOX_SEQS, 0, 1, 1, SHR_E_FORMAT},
        {AT_RAW, BOX_SEQS, 0, 1, 17, SHR_E_FORMAT},
        {AT_RAW, BOX_SEQS, 1, 1, 0, SHR_E_FORMAT},
        {AT_RAW, BOX_SEQS, 1, 1, 5, SHR_E_FORMAT},
        {AT_RAW, BOX_SEQS, 2, 2, 1, SHR_E_FORMAT},
        {AT_RAW, BOX_SEQS, 4, 4, 3, SHR_E_FORMAT},
        {AT_RAW, BOX_POOL, 8, 4, 'A', SHR_E_FORMAT},                     /* order */
        {AT_LEN, BOX_SEQS, 0, 0, 0, SHR_E_NOT_FOUND},                    /* no sequences: A+U+0301 draws U+FFFD */
        {AT_RAW, BOX_MANI, 32, 4, 0, SHR_E_FORMAT},                      /* ptab */
        {AT_RAW, BOX_MANI, 32, 4, 70000, SHR_E_FORMAT},
        {AT_RAW, BOX_MANI, 32, 4, 3, SHR_E_FORMAT},
        {AT_LEN, BOX_PTAB, 0, 0, 49, SHR_E_FORMAT},
        {AT_PTAB, 1, 20, 2, 0, SHR_E_FORMAT},                            /* page records */
        {AT_PTAB, 1, 20, 2, 4097, SHR_E_FORMAT},
        {AT_PTAB, 1, 22, 2, 0, SHR_E_FORMAT},
        {AT_PTAB, 1, 22, 2, SY_H + 1, SHR_E_FORMAT},
        {AT_PTAB, 0, 16, 4, 23, SHR_E_FORMAT},
        {AT_PTAB, 1, 16, 4, (1u << 20) + 1, SHR_E_FORMAT},
        {AT_PTAB, 0, 0, 8, 400, SHR_E_FORMAT},                           /* inside the index */
        {AT_PTAB, 1, 0, 8, 1024, SHR_E_FORMAT},                          /* before the end of page 0 */
        {AT_PTAB, 1, 0, 8, 1ull << 40, SHR_E_FORMAT},                    /* past the file */
        {AT_PTAB, 1, 16, 4, 1u << 20, SHR_E_FORMAT},
        {AT_RAW, BOX_MANI, 28, 4, 2 * SY_NG + 1, SHR_E_FORMAT},          /* glyphs not covered */
        {AT_PTAB, 1, 20, 2, SY_NG - 1, SHR_E_FORMAT},
        {AT_RAW, BOX_CTRI, SY_D + 64 + 4, 4, 2 << 12, SHR_E_FORMAT},     /* 'A' on a page past the last */
        {AT_RAW, BOX_CTRI, SY_D + 64 + 4, 4, SY_NG, SHR_E_FORMAT},       /* past the records of its page */
        {AT_RAW, BOX_CTRI, SY_D + 64 + 4, 4, SY_G2 | (SY_NG - 1), SHR_OK}, /* the last record of page 1 */
        {AT_RAW, BOX_CTRI, SY_D + 64 + 4, 4, SHR_GID_MISS, SHR_E_NOT_FOUND},
        {AT_RAW, BOX_SEQS, 8, 4, 2 << 12, SHR_E_FORMAT},                 /* sequence glyphs */
        {AT_RAW, BOX_SEQS, 8, 4, SY_NG, SHR_E_FORMAT},
        {AT_RAW, BOX_SEQS, 8, 4, SHR_GID_MISS, SHR_E_NOT_FOUND},         /* A+U+0301 draws U+FFFD */
    };
    static sy_raw base;
    sy_init(&base, ROLE_LATIN, "", 'A', 'B', SY_A4);
    run_cases(cases, sizeof(cases) / sizeof(cases[0]), &base, false);
    static sy_raw r;
    const mutation none = {AT_FILE, 0, 0, 1, 0x80, SHR_E_FORMAT};
    r = base;
    r.cshift = 16; /* a stored ctri whose data blocks are not aligned */
    run_cases(&none, 1, &r, false);
    const uint32_t spaces[3] = {SHR_GID_MISS, SHR_GID_BLANK, 0}; /* U+0020 is MISS or BLANK */
    for (int i = 0; i < 3; i++) {
        r = base;
        sy_map(&r, ' ', spaces[i]);
        const mutation same = {AT_FILE, 0, 0, 1, 0x80, i < 2 ? SHR_OK : SHR_E_FORMAT};
        run_cases(&same, 1, &r, false);
    }
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    for (int mode = SRC_READ; mode <= SRC_MAP; mode++) {
        synth(&d);
        tsrc s = {d.d, d.n, .mode = mode};
        memcpy(d.d, "SHRFPKG1", 8); /* a v4 package */
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT);
        ASSERT_EQ_LL(s.calls, mode == SRC_READ);
        synth(&d);
        d.d[48] ^= 1;
        seal(d.d, d.n);
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_PROFILE_MISMATCH);
        synth(&d);
        d.d[50] ^= 1;
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_CHECKSUM); /* header checksum */
        synth(&d);
        sy_entry(d.d, "mani")[24] ^= 1;
        put(d.d + 120, XXH3_64bits(d.d, 120), 8);
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_CHECKSUM); /* index checksum */
        synth(&d);
        sy_box(d.d, "mani")[20] ^= 1;
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_CHECKSUM); /* box checksum */
        synth(&d);
        put(d.d + 28, 24 + 32 * 32, 4), put(d.d + 32, 1000, 8);
        seal(d.d, d.n);
        s.claim = 1000;
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT); /* the index past the end */
        synth(&d);
        s.claim = d.n - 1;
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT); /* source shorter than the header says */
        put(d.d + 32, d.n - 1, 8);
        seal(d.d, d.n);
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT); /* the last page past the end */
        s.claim = 100;
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT); /* shorter than a header */
        s.claim = 0;
    }
    harness_close(&h);
    PASS();
}

/* Indexed boxes of a type this reader does not know: skipped unless REQUIRED. */
TEST test_unknown_boxes(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static sy_raw r;
    static spkg d;
    for (int method = 0; method < SY_METHODS; method++)
        for (int mode = SRC_READ; mode <= SRC_MAP; mode++)
            for (uint16_t flags = 0; flags < 2; flags++) {
                sy_init(&r, ROLE_LATIN, "", 'A', 'B', SY_A4);
                memcpy(r.extra, "zzzz", 4), r.extra_flags = flags;
                assemble(&d, &r, method);
                tsrc s = {d.d, d.n, .mode = mode};
                ASSERT_EQ_LL(pkg_status(ctx, &s), flags ? SHR_E_UNSUPPORTED : SHR_OK);
            }
    harness_close(&h);
    PASS();
}

/* pool and seqs hold at most 2^22 records each. */
TEST test_record_limits(void) {
    static const struct {
        int box;
        uint32_t size;
    } boxes[2] = {{BOX_POOL, 4}, {BOX_SEQS, 12}};
    size_t big = 12 * ((size_t)SHR_PKG_MAX_RECORDS + 1), cap = (big + SY_CAP + 255) / 256 * 256;
    uint8_t *zeros = calloc(big, 1), *d = aligned_alloc(256, cap);
    ASSERT(zeros && d);
    static sy_raw r;
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    for (int i = 0; i < 2; i++) {
        sy_init(&r, ROLE_LATIN, "", 'A', 'B', SY_A4);
        r.payload[boxes[i].box] = zeros;
        r.len[boxes[i].box] = (int32_t)(boxes[i].size * (SHR_PKG_MAX_RECORDS + 1));
        tsrc s = {d, assemble_to(d, cap, &r, SY_STORED), .mode = SRC_MAP};
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT);
        r.len[boxes[i].box] -= (int32_t)boxes[i].size; /* 2^22 of them: then their contents are checked */
        memcpy(zeros, r.box[BOX_POOL], 16 * (i == 0)); /* a pool that holds the sequences' scalars */
        s = (tsrc){d, assemble_to(d, cap, &r, SY_STORED), .mode = SRC_MAP};
        ASSERT_EQ_LL(pkg_status(ctx, &s), i == 0 ? SHR_OK : SHR_E_FORMAT);
    }
    harness_close(&h);
    free(zeros);
    free(d);
    PASS();
}

/* A zstd package is rejected at its header without zstd, nothing else read. */
TEST test_zstd_needs_decoder(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    synth(&d);
    put(d.d + 20, 1 | 4 | 8, 4); /* bit3 without a zstd box */
    seal(d.d, d.n);
    tsrc s = {d.d, d.n, .mode = SRC_READ};
#ifdef SHR_ZSTD
    ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_OK);
#else
    ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(s.calls, 1);
#endif
    harness_close(&h);
    PASS();
}

TEST test_index_size_limit(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static sy_raw r;
    static spkg d;
    sy_init(&r, ROLE_LATIN, "", 'A', 'B', SY_A4);
    r.len[BOX_COVR] = 36;
    assemble(&d, &r, SY_STORED);
    uint64_t size = 200ull << 20;
    put(sy_entry(d.d, "covr") + 8, 129ull << 20, 8); /* the region of the known boxes is too large */
    put(d.d + 32, size, 8);
    seal(d.d, d.n);
    tsrc s = {d.d, d.n, .claim = size};
    ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_LIMIT);
    ASSERT_EQ_LL(s.closes, 1);
#ifdef SHR_ZSTD
    synth_m(&d, ROLE_LATIN, "", 'A', 'B', SY_A4, SY_ZSTD);
    put(sy_entry(d.d, "ctri") + 20, 129u << 20, 4); /* so is their raw size */
    seal(d.d, d.n);
    s = (tsrc){d.d, d.n, .mode = SRC_READ};
    ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_LIMIT);
#endif
    harness_close(&h);
    PASS();
}

/* The index and the metadata, read once each, are checked against their checksums. */
TEST test_index_changed_between_reads(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    synth(&d);
    for (int call = 2; call <= 3; call++) { /* the index, then the metadata region */
        tsrc s = {d.d, d.n, .poke_call = call, .poke_at = 30};
        ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_CHECKSUM);
        ASSERT_EQ_LL(s.calls, call);
    }
    harness_close(&h);
    PASS();
}

TEST test_page_validation(void) {
    static const mutation boxes[] = {
        {AT_PAGE, 0, 4, 1, 'X', SHR_E_FORMAT},          /* type, version, size */
        {AT_PAGE, 0, 8, 2, 1, SHR_E_FORMAT},
        {AT_PAGE, 0, 0, 4, 100, SHR_E_FORMAT},
        {AT_PAGE, 0, 10, 2, 0x21, SHR_E_UNSUPPORTED},   /* method 2 */
        {AT_PAGE, 0, 10, 2, 0, SHR_E_FORMAT},           /* not REQUIRED */
        {AT_PAGE, 0, 10, 2, 0x03, SHR_E_FORMAT},
        {AT_PAGE, 0, 10, 2, 0x11, SHR_E_FORMAT},        /* zstd without required feature bit3 */
        {AT_PAGE, 0, 12, 4, 0, SHR_E_FORMAT},           /* raw size */
        {AT_PAGE, 0, 16, 4, 1, SHR_E_FORMAT},           /* page index */
        {AT_PAGE, 0, 20, 4, 0, SHR_E_FORMAT},           /* atlas stream bytes */
        {AT_PAGE, 0, 20, 4, 1u << 20, SHR_E_FORMAT},
    };
    static const mutation records[] = {
        {AT_RECS, 0, 8, 1, 0x04, SHR_E_FORMAT},         /* format 0: no bitmap */
        {AT_RECS, 0, 8, 1, 0x07, SHR_E_FORMAT},         /* format 3 */
        {AT_RECS, 0, 8, 1, 0x01, SHR_E_FORMAT},         /* 0 cells */
        {AT_RECS, 0, 8, 1, 0x0D, SHR_E_FORMAT},         /* 3 cells */
        {AT_RECS, 0, 8, 1, 0x25, SHR_E_FORMAT},         /* reserved flag */
        {AT_RECS, 0, 8, 1, 0x15, SHR_E_FORMAT},         /* flag bit 4 */
        {AT_RECS, 0, 9, 1, 1, SHR_E_FORMAT},            /* reserved bytes */
        {AT_RECS, 0, 12, 4, 1, SHR_E_FORMAT},
        {AT_RECS, 0, 4, 1, 0, SHR_E_FORMAT},            /* width */
        {AT_RECS, 0, 5, 1, 0, SHR_E_FORMAT},            /* height */
        {AT_RECS, 0, 0, 2, 3, SHR_E_FORMAT},            /* odd A4 x */
        {AT_RECS, 0, 0, 2, SY_W - 2, SHR_E_FORMAT},     /* past the atlas */
        {AT_RECS, 0, 2, 2, SY_H - 3, SHR_E_FORMAT},
        {AT_RECS, 1, 2, 2, SY_H1 - 3, SHR_E_FORMAT},    /* below the trimmed height */
        {AT_RECS, 0, 16 * SY_NG - 8, 1, 0x04, SHR_E_FORMAT}, /* the last record */
        {AT_RECS, 0, 8, 1, 0x06, SHR_E_FORMAT},         /* A8 bitmap in an A4 instance */
        {AT_RECS, 0, 4, 1, 3, SHR_E_FORMAT},            /* odd A4 width: the padding nibble is not zero */
        {AT_ATLAS, 0, 3 * SY_W / 2 + 1, 1, 0xF0, SHR_OK}, /* drawn pixels are not checked */
    };
    static sy_raw base, r;
    sy_init(&base, ROLE_LATIN, "", 'A', 'B', SY_A4);
    run_cases(boxes, sizeof(boxes) / sizeof(boxes[0]), &base, true);
    for (int i = 0; i < 2; i++) { /* a stored page box: size, alignment */
        r = base;
        r.pad = i ? 0 : 1, r.shift = i ? 16 : 0;
        const mutation none = {AT_FILE, 0, 0, 1, 0x80, SHR_E_FORMAT};
        run_cases(&none, 1, &r, true);
    }
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    for (int method = 0; method < SY_METHODS; method++)
        for (int mode = SRC_READ; mode <= SRC_MAP; mode++) {
            for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); i++) {
                mutate(&d, &base, method, &records[i]);
                tsrc s = {d.d, d.n, .mode = mode};
                shr_status st = pkg_status(ctx, &s);
                if (st != records[i].want) fprintf(stderr, "method %d mode %d case %zu\n", method, mode, i);
                ASSERT_EQ_LL(st, records[i].want);
                ASSERT_EQ_LL(s.calls, mode == SRC_READ ? 5 : 0); /* each page read once */
            }
            synth_m(&d, ROLE_LATIN, "", 'A', 'B', SY_A4, method);
            sy_page(d.d, 0)[sy_size(d.d, 0) - 1] ^= 1;
            tsrc s = {d.d, d.n, .mode = mode};
            ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_CHECKSUM);
            ASSERT_EQ_LL(s.calls, mode == SRC_READ ? 3 + 4 + 1 : 0); /* read once, then retried 3 times */
            sy_init(&r, ROLE_LATIN, "", 'A', 'B', SY_A8);
            r.recs[0][8] = 0x05; /* A4 bitmap in an A8 instance */
            assemble(&d, &r, method);
            s = (tsrc){d.d, d.n, .mode = mode};
            ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT);
            r = base;
            r.recs[0][4] = 3;
            for (int y = 1; y < 5; y++) r.atlas[0][SY_W / 2 * y + 2] = 0xF0; /* odd A4 width, zero padding */
            assemble(&d, &r, method);
            s = (tsrc){d.d, d.n, .mode = mode};
            ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_OK);
        }
    harness_close(&h);
    PASS();
}

static bool rect_is(const shr__resolved *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    return r->rect.x0 == x0 && r->rect.y0 == y0 && r->rect.x1 == x1 && r->rect.y1 == y1;
}

/* Three reads from a scalar to its glyph value, at block edges too; blocks may be shared. */
TEST test_ctri_lookup(void) {
    static uint32_t map[7][2] = {{0x10, 1},      {0x1F, 2},      {0x3F0, 3}, {0x3FF, SHR_GID_BLANK},
                                 {0x400, 1 << 12}, {0x10FFF0, 5}, {0x10FFFF, 6}};
    static uint8_t ct[SY_BOX];
    size_t n = sy_ctri(ct, map, 7);
    uint32_t nd = shr__rd32(ct + 4);
    shr__pkg pkg = {.ctri = ct, .ctri_data = ct + n - 64 * nd};
    ASSERT_EQ_LL((n - 64 * nd) % 64, 0);
    static const uint32_t miss[] = {0, 0xF, 0x20, 0x3EF, 0x401, 0x7FF, 0x800, 0x10FFEF, 0x10FFFE, 0xD800};
    for (size_t i = 0; i < sizeof(miss) / sizeof(miss[0]); i++)
        ASSERT_EQ_LL(shr__ctri_get(&pkg, miss[i]), SHR_GID_MISS);
    for (int i = 0; i < 7; i++) ASSERT_EQ_LL(shr__ctri_get(&pkg, map[i][0]), map[i][1]);
    put(ct + 16 + 2 * 5, shr__rd16(ct + 16), 2); /* U+1400..17FF share the level-2 block of U+0000..03FF */
    put(ct + SHR_PKG_CTRI + 128 + 2 * 5, shr__rd16(ct + SHR_PKG_CTRI + 128 + 2), 2); /* U+0050.. the data of U+0010.. */
    ASSERT_EQ_LL(shr__ctri_get(&pkg, 0x1410), 1);
    ASSERT_EQ_LL(shr__ctri_get(&pkg, 0x17FF), SHR_GID_BLANK);
    ASSERT_EQ_LL(shr__ctri_get(&pkg, 0x50), 1);
    ASSERT_EQ_LL(shr__ctri_get(&pkg, 0x5F), 2);
    ASSERT_EQ_LL(shr__ctri_get(&pkg, 0x1450), 1);
    PASS();
}

/* Blank glyphs (no bitmap) and U+0020 draw nothing and never touch pages. */
TEST test_blank_glyphs(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static sy_raw raw;
    static spkg d;
    sy_init(&raw, ROLE_LATIN, "", 'A', 'B', SY_A4);
    put(raw.box[BOX_SEQS] + 12 + 8, SHR_GID_BLANK, 4); /* B+U+0301 */
    sy_map(&raw, 0x3000, SHR_GID_BLANK);
    assemble(&d, &raw, SY_STORED);
    tsrc s = {d.d, d.n, .mode = SRC_READ, .script = {SHR_IN_PROGRESS}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    shr__resolved r;
    ASSERT_EQ_LL(resolve(f, ' ', &r), SHR_E_NOT_FOUND); /* latin loads: the built-in U+0020, blank too */
    shr_pump(ctx);
    ts_complete(ctx, &s, SHR_OK);
    const uint32_t sb[2] = {'B', 0x301};
    const uint64_t ids[4] = {glyph1(f, 'B'), glyph1(f, 0x3000), glyph(f, sb, 2), ' '};
    for (int i = 0; i < 4; i++) ASSERT_EQ_LL(load(ctx, f, ids[i], &r), SHR_E_NOT_FOUND);
    shr__pkg *pkg = &f->pkg[ROLE_LATIN];
    ASSERT(pkg->state == PKG_READY && !pkg->pages[0] && !pkg->pages[1]);
    drain(ctx); /* the other packages asked for U+0020 open */
    ASSERT(f->wants.len == 0 && !f->pinned && !f->lru && !res->ops->has_work(res));
    ASSERT_EQ_LL(s.calls, 3); /* header, index, metadata: no page */
    ASSERT_EQ_LL(SHR_VEC_AT(&f->clusters, shr__cluster, 0)->gid[ROLE_LATIN], SHR_GID_BLANK);
    harness_close(&h);
    PASS();
}

/* A cluster is looked up once per ready package; absent, failed and loading packages leave it to a later lookup. */
TEST test_cluster_memo(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d, bad, em;
    static sy_raw raw;
    synth(&d);
    synth_as(&em, ROLE_EMOJI, "", 0x1F600, 0x1F601, SY_A4);
    sy_init(&raw, ROLE_LATIN, "", 'A', 'B', SY_A4);
    put(raw.box[BOX_MANI] + 28, 2 * SY_NG + 1, 4);
    assemble(&bad, &raw, SY_STORED);
    tsrc s = {d.d, d.n, .mode = SRC_READ, .script = {SHR_IN_PROGRESS}}, sb = {bad.d, bad.n, .mode = SRC_MAP},
         se = {em.d, em.n, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_LATIN] = &s, [ROLE_EMOJI] = &se}};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, NULL);
    const uint32_t smile[2] = {0x1F600, 0xFE0F};
    uint64_t id = glyph2(f), emoji = glyph(f, smile, 2);
    shr__cluster *c = SHR_VEC_AT(&f->clusters, shr__cluster, 0);
    for (int i = 0; i < ROLE_COUNT; i++) ASSERT_EQ_LL(c->gid[i], SHR_GID_UNKNOWN);
    shr__resolved r, a;
    ASSERT_EQ_LL(resolve(f, id, &r), SHR_OK);
    shr_pump(ctx);
    ASSERT_EQ_LL(f->pkg[ROLE_LATIN].state, PKG_LOADING);
    ASSERT_EQ_LL(resolve(f, id, &r), SHR_OK);
    ASSERT(r.provisional && c->gid[ROLE_LATIN] == SHR_GID_UNKNOWN && c->gid[ROLE_BUILTIN] == SHR_GID_UNKNOWN);
    ts_complete(ctx, &s, SHR_OK);
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT(!r.provisional && rect_is(&r, 0, 4, 4, 8));
    ASSERT_EQ_LL(c->gid[ROLE_LATIN], SY_G2);
    ASSERT_EQ_LL(c->gid[ROLE_CJK], SHR_GID_UNKNOWN); /* not asked: latin had it */
    c->gid[ROLE_LATIN] = 0;                          /* later lookups take the memo: now page 0's glyph */
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &a), SHR_OK);
    ASSERT(rect_is(&r, SY_X0, 1, SY_X0 + 4, 5) && r.buf == a.buf);
    shr__cluster *e = SHR_VEC_AT(&f->clusters, shr__cluster, 1);
    ASSERT_EQ_LL(load(ctx, f, emoji, &r), SHR_OK); /* no such emoji sequence: the emoji glyph of its base */
    ASSERT_EQ_LL(e->gid[ROLE_EMOJI], SHR_GID_MISS);
    font_free_now(ctx, f);

    l = (lib){.src = {[ROLE_LATIN] = &sb}}; /* latin fails, the others are absent */
    f = h.font = font_new(ctx, &l, NULL);
    id = glyph2(f);
    c = SHR_VEC_AT(&f->clusters, shr__cluster, 0);
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_FORMAT);
    ASSERT_EQ_LL(load(ctx, f, id, &r), SHR_OK);
    ASSERT(!r.provisional && inside(&r, shr__builtin_package, shr__builtin_package_size)); /* U+FFFD */
    for (int i = ROLE_LATIN; i < ROLE_COUNT; i++) ASSERT_EQ_LL(c->gid[i], SHR_GID_UNKNOWN);
    ASSERT_EQ_LL(c->gid[ROLE_BUILTIN], SHR_GID_MISS);
    harness_close(&h);
    PASS();
}

TEST test_synthetic_glyphs(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__resolved r, a, other;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &a), SHR_OK);
    ASSERT(!a.provisional);
    ASSERT(a.buf->mem.pixels == sy_page(d.d, 0) + 24); /* mapped: the atlas is drawn from in place */
    ASSERT(a.buf->mem.width == SY_W && a.buf->mem.height == SY_H && a.buf->mem.stride == SY_W / 2);
    ASSERT_EQ_LL(a.buf->mem.format, SHR_FORMAT_A4);
    ASSERT(rect_is(&a, SY_X0, 1, SY_X0 + 4, 5));
    ASSERT(cov(a.buf, SY_X0 - 1, 1) && cov(a.buf, SY_X0 + 4, 4) && cov(a.buf, SY_X0, 0) && cov(a.buf, SY_X0 + 3, 5));
    ASSERT_EQ_LL(cov(a.buf, SY_X0, 1), 255); /* neighbours all around the rect: drivers read only the rect */
    ASSERT_EQ_LL(a.offset.x, 1);
    ASSERT_EQ_LL(a.offset.y, 0);
    ASSERT_EQ_LL(a.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
    ASSERT_EQ_LL(a.slant_axis, CH); /* twice the line centre's row in the rect */
    ASSERT(!f->pinned && !f->lru && f->page_bytes == 0); /* mapped pages are never pinned or cached */
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'B'), &r), SHR_E_NOT_FOUND); /* blank */
    uint32_t sa[2] = {'A', 0x301}, sb[2] = {'B', 0x301}, sc[2] = {'C', 0x301}, sel[2] = {'A', 0xFE0E};
    ASSERT_EQ_LL(load(ctx, f, glyph(f, sa, 2), &r), SHR_OK);
    ASSERT_EQ_LL(load(ctx, f, glyph(f, sb, 2), &other), SHR_OK);
    ASSERT(r.buf->mem.pixels == sy_page(d.d, 1) + 24 && other.buf == r.buf); /* shared slot, second page */
    ASSERT_EQ_LL(r.buf->mem.height, SY_H1); /* trimmed */
    ASSERT(rect_is(&r, 0, 4, 4, 8) && rect_is(&other, 0, 4, 4, 8));
    ASSERT_EQ_LL(r.offset.x, 0);
    ASSERT_EQ_LL(r.offset.y, 3);
    ASSERT_EQ_LL(r.slant_axis, CH - 6);
    ASSERT_EQ_LL(load(ctx, f, glyph(f, sc, 2), &r), SHR_OK); /* no such sequence: U+FFFD */
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xFFFD), &other), SHR_OK);
    ASSERT(same_image(&r, &other));
    ASSERT_EQ_LL(load(ctx, f, glyph(f, sel, 2), &r), SHR_OK); /* one visible scalar: its base glyph */
    ASSERT(r.buf == a.buf && rect_is(&r, SY_X0, 1, SY_X0 + 4, 5));
    font_free_now(ctx, f);

    synth_as(&d, ROLE_LATIN, "", 'A', 'B', SY_A8);
    s = (tsrc){d.d, d.n, .mode = SRC_MAP};
    f = h.font = font_new(ctx, &l, NULL);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT(r.buf->mem.pixels == sy_page(d.d, 0) + 24 && rect_is(&r, 0, 1, 4, 5));
    ASSERT(r.buf->mem.width == SY_W && r.buf->mem.stride == SY_W);
    ASSERT_EQ_LL(r.buf->mem.format, SHR_FORMAT_A8);
    ASSERT(cov(r.buf, 0, 0) == 0x5A && cov(r.buf, 0, 1) == 255);
    static sy_raw raw;
    for (int i = 0; i < 2; i++) { /* rows above the line, then a bitmap wholly below the baseline */
        font_free_now(ctx, f);
        sy_init(&raw, ROLE_LATIN, "", 'A', 'B', SY_A4);
        raw.recs[0][7] = (uint8_t)(i ? -1 : SY_BASE + 2);
        assemble(&d, &raw, SY_STORED);
        s = (tsrc){d.d, d.n, .mode = SRC_MAP};
        f = h.font = font_new(ctx, &l, NULL);
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
        ASSERT_EQ_LL(r.offset.y, i ? SY_BASE + 1 : -2);
        ASSERT_EQ_LL(r.slant_axis, i ? CH - 2 * (SY_BASE + 1) : CH + 4);
    }
    harness_close(&h);
    PASS();
}

/* zstd packages, read and mapped, draw the same pixels as stored ones; their pages are decoded into the cache. */
TEST test_zstd_round_trip(void) {
#ifdef SHR_ZSTD
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg stored, packed;
    for (uint8_t fmt = SY_A4; fmt <= SY_A8; fmt++) {
        synth_as(&stored, ROLE_LATIN, "", 'A', 'B', fmt);
        synth_m(&packed, ROLE_LATIN, "", 'A', 'B', fmt, SY_ZSTD);
        ASSERT(packed.n < stored.n / 2 && shr__rd16(sy_box(packed.d, "ctri") + 10) == 0x11);
        tsrc ss = {stored.d, stored.n, .mode = SRC_MAP};
        lib ls = {.src = {[ROLE_LATIN] = &ss}};
        shr_pl_res_bitmap_font *g = font_new(ctx, &ls, NULL);
        for (int mode = SRC_READ; mode <= SRC_MAP; mode++) {
            tsrc sp = {packed.d, packed.n, .mode = mode};
            lib lp = {.src = {[ROLE_LATIN] = &sp}};
            shr_pl_res_bitmap_font *f = font_new(ctx, &lp, NULL);
            const uint32_t sa[2] = {'A', 0x301};
            const uint64_t ids[2][2] = {{glyph1(f, 'A'), glyph(f, sa, 2)}, {glyph1(g, 'A'), glyph(g, sa, 2)}};
            for (int i = 0; i < 2; i++) {
                shr__resolved a, b;
                ASSERT_EQ_LL(load(ctx, f, ids[0][i], &a), SHR_OK);
                ASSERT_EQ_LL(load(ctx, g, ids[1][i], &b), SHR_OK);
                ASSERT(!a.provisional && rh(&a) == 4 && same_image(&a, &b) && !inside(&a, packed.d, packed.n));
                ASSERT(a.buf->mem.height == SY_H && (cov(a.buf, 0, SY_H - 1) == 0) == (i == 1)); /* trimmed: 0 */
            }
            ASSERT(f->zdc && f->pkg[ROLE_LATIN].meta);
            ASSERT_EQ_LL(f->page_bytes, 2 * (SY_H * (fmt == SY_A4 ? SY_W / 2 : SY_W) + SY_RECS));
            ASSERT_EQ_LL(sp.calls, mode == SRC_READ ? 5 : 0);
            font_free_now(ctx, f);
        }
        font_free_now(ctx, g);
    }
    harness_close(&h);
#endif
    PASS();
}

/* A stream is decoded only when it is one zstd frame with the loader's limits that decodes to exactly the bytes it
 * must; otherwise its box or page fails at once, without a re-read. */
TEST test_zstd_frames(void) {
#ifdef SHR_ZSTD
    enum { M0 = 0x28, M1 = 0xB5, M2 = 0x2F, M3 = 0xFD };
    static uint8_t raw_block[7 + 3 + 256] = {M0, M1, M2, M3, 0x60, 0, 0, 0x01, 0x08, 0x00};
    static const struct {
        uint8_t b[24];
        size_t n;
        shr_status want;
    } frames[] = {
        {{M0, M1, M2, M3, 0x60, 0, 0, 0x03, 0x08, 0, 0x5A}, 11, SHR_OK},           /* 256 bytes of one RLE block */
        {{M0, M1, M2, M3, 0x40, 0x40, 0, 0, 0x03, 0x08, 0, 0x5A}, 12, SHR_OK},     /* a window of 2^18 */
        {{M0, M1, M2}, 3, SHR_E_FORMAT},                                           /* shorter than a magic */
        {{0x50, 0x2A, 0x4D, 0x18, 1, 0, 0, 0, 0}, 9, SHR_E_FORMAT},                /* a skippable frame */
        {{0x27, M1, M2, M3, 0x60, 0, 0, 0x03, 0x08, 0, 0x5A}, 11, SHR_E_FORMAT},   /* a legacy magic */
        {{M0, M1, M2, M3, 0xE0}, 5, SHR_E_FORMAT},                                 /* a cut frame header */
        {{M0, M1, M2, M3, 0x60, 0, 0, 0x03, 0x08, 0, 0x5A, 0}, 12, SHR_E_FORMAT},  /* a byte after the frame */
        {{M0, M1, M2, M3, 0x60, 0, 0, 0x03, 0x08, 0, 0x5A, M0, M1, M2, M3, 0x60, 0, 0, 0x03, 0x08, 0, 0x5A}, 22,
         SHR_E_FORMAT},                                                            /* two frames */
        {{M0, M1, M2, M3, 0x60, 0, 0, 0x03, 0x08, 0}, 10, SHR_E_FORMAT},           /* a cut block */
        {{M0, M1, M2, M3, 0x20, 0xFF, 0xFB, 0x07, 0, 0x5A}, 10, SHR_E_FORMAT},     /* content size 255 */
        {{M0, M1, M2, M3, 0x00, 0x40, 0x03, 0x08, 0, 0x5A}, 10, SHR_E_FORMAT},     /* no content size */
        {{M0, M1, M2, M3, 0x40, 0x48, 0, 0, 0x03, 0x08, 0, 0x5A}, 12, SHR_E_FORMAT}, /* a window of 2^19 */
        {{M0, M1, M2, M3, 0x61, 7, 0, 0, 0x03, 0x08, 0, 0x5A}, 12, SHR_E_FORMAT},  /* a dictionary */
        {{M0, M1, M2, M3, 0x64, 0, 0, 0x03, 0x08, 0, 0x5A, 1, 2, 3, 4}, 15, SHR_E_FORMAT}, /* a checksum */
        {{M0, M1, M2, M3, 0x60, 0, 0, 0xFB, 0x07, 0, 0x5A}, 11, SHR_E_FORMAT},     /* decodes to 255 bytes */
        {{0}, 0, SHR_E_FORMAT},                                                    /* not smaller: raw_block */
    };
    memset(raw_block + 10, 0x5A, 256);
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static sy_raw r;
    static spkg d;
    static const int streams[3] = {0, 2, 3}; /* ctri, then page 0's records and page 1's atlas of 256 bytes each */
    for (int k = 0; k < 3; k++)
        for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++)
            for (int mode = SRC_READ; mode <= SRC_MAP; mode++) {
                int stream = streams[k];
                sy_init(&r, ROLE_LATIN, "", 'A', 'B', SY_A4);
                r.stream[stream] = frames[i].n ? frames[i].b : raw_block;
                r.stream_n[stream] = frames[i].n ? frames[i].n : sizeof(raw_block);
                bool ok = frames[i].want == SHR_OK && stream == 3; /* else not ctri's size, records of 0x5A invalid */
                assemble(&d, &r, SY_ZSTD);
                tsrc s = {d.d, d.n, .mode = mode};
                shr_status st = pkg_status(ctx, &s);
                if (st != (ok ? SHR_OK : SHR_E_FORMAT))
                    fprintf(stderr, "stream %d frame %zu mode %d\n", stream, i, mode);
                ASSERT_EQ_LL(st, ok ? SHR_OK : SHR_E_FORMAT);
                ASSERT_EQ_LL(s.calls, mode == SRC_READ ? (stream ? 5 : 3) : 0);
            }
    synth_m(&d, ROLE_LATIN, "", 'A', 'B', SY_A4, SY_ZSTD);
    put(sy_entry(d.d, "ctri") + 20, shr__rd32(sy_entry(d.d, "ctri") + 16) - 16, 4); /* raw_size not above the box's */
    seal(d.d, d.n);
    tsrc s = {d.d, d.n, .mode = SRC_READ};
    ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_FORMAT);
    harness_close(&h);
#endif
    PASS();
}

/* A page's slot is of its package's page shape; a trimmed page fills its top rows, and the rows below are zeroed
 * after every decode. Only a stored page drawn from in place is a buffer of its own height. */
TEST test_trimmed_page(void) {
    for (int method = 0; method < SY_METHODS; method++) {
        static spkg d;
        synth_m(&d, ROLE_LATIN, "", 'A', 'B', SY_A4, method);
        harness h;
        cache_limit = one_page(&d);
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
        tsrc s = {d.d, d.n, .mode = SRC_READ};
        lib l = {.src = {[ROLE_LATIN] = &s}};
        shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
        shr__resolved a, b;
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &a), SHR_OK);
        ASSERT_EQ_LL(cov(a.buf, 0, SY_H - 1), 0x5 * 17);
        ASSERT_EQ_LL(load(ctx, f, glyph2(f), &b), SHR_OK); /* takes page 0's slot */
        ASSERT(b.buf == a.buf && !b.provisional && b.buf->mem.height == SY_H);
        for (int y = SY_H1; y < SY_H; y++) ASSERT_EQ_LL(cov(b.buf, 0, y) | cov(b.buf, SY_W - 1, y), 0);
        ASSERT(cov(b.buf, 0, SY_H1 - 1) && cov(b.buf, 3, 7) == 255);
        harness_close(&h);
    }
    PASS();
}

/* A page being read charges its staging buffer to the page cache until the read ends. */
TEST test_staging_charged(void) {
    for (int method = 0; method < SY_METHODS; method++) {
        static spkg d;
        synth_m(&d, ROLE_LATIN, "", 'A', 'B', SY_A4, method);
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
        tsrc s = {d.d, d.n, .script = {[3] = SHR_IN_PROGRESS, [4] = SHR_E_WOULD_BLOCK}};
        lib l = {.src = {[ROLE_LATIN] = &s}};
        shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
        shr__resolved r;
        resolve(f, glyph1(f, 'A'), &r);
        settle(ctx);
        resolve(f, glyph1(f, 'A'), &r);
        shr_pump(ctx);
        ASSERT_EQ_LL(s.pending, 1);
        ASSERT_EQ_LL(f->page_bytes, SY_RES + sy_size(d.d, 0));
        ts_complete(ctx, &s, SHR_OK);
        shr_pump(ctx);
        ASSERT_EQ_LL(f->page_bytes, SY_RES);
        resolve(f, glyph2(f), &r); /* refused: nothing stays charged */
        shr_pump(ctx);
        ASSERT_EQ_LL(s.calls, 5);
        ASSERT_EQ_LL(f->page_bytes, SY_RES);
        harness_close(&h);

        cache_limit = SY_RES + sy_size(d.d, 0) - 1; /* the page fits, its read does not */
        ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
        s = (tsrc){d.d, d.n, .mode = SRC_READ};
        f = h.font = font_new(ctx, &l, NULL);
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
        ASSERT(!r.provisional && rh(&r) != 4);
        ASSERT_EQ_LL(last_failure(ctx), SHR_E_LIMIT);
        harness_close(&h);
    }
    PASS();
}
/* ===== Asynchronous loading ===== */

TEST test_deferred_reads(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .script = {SHR_IN_PROGRESS, SHR_IN_PROGRESS, SHR_IN_PROGRESS, SHR_IN_PROGRESS}};
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
    ASSERT(rh(&r) == 4);
    ASSERT_EQ_LL(s.calls, 4);
    harness_close(&h);
    PASS();
}

TEST test_read_retries_and_limit(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    synth(&d);
    /* The table read fails twice, the index and page reads once: each is retried after io_retry_ns. */
    tsrc s = {d.d, d.n, .script = {0, SHR_E_IO, SHR_E_IO, 0, SHR_E_IO, 0, SHR_E_IO}};
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
    ASSERT(rh(&r) == 4);
    ASSERT_EQ_LL(s.calls, 8);
    ASSERT_EQ_LL(last_failure(ctx), SHR_OK);
    font_free_now(ctx, f);

    /* Beyond io_retry_limit the package closes and its glyphs fall back, until a later frame wants it
     * after io_retry_ns. */
    tsrc bad = {d.d, d.n, .script = {SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
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
    ASSERT(!r.provisional && rh(&r) == 4);
    ASSERT_EQ_LL(l.opens, opens + 1);

    /* A failing page read, the same way. */
    tsrc page = {d.d, d.n, .script = {0, 0, 0, SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
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
    ASSERT(r.provisional && rh(&r) != 4);
    ASSERT_EQ_LL(page.calls, 7);
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_IO);
    shr_next_deadline(ctx, &dl);
    ASSERT(dl.kind == SHR_DEADLINE_AT && dl.at_ns == fake_now + 50 * MS); /* the end of the cool-down redraws */
    resolve(g, id, &r);
    settle(ctx);
    ASSERT_EQ_LL(page.calls, 7); /* not before io_retry_ns */
    fake_now += 50 * MS;
    ASSERT_EQ_LL(load(ctx, g, id, &r), SHR_OK);
    ASSERT(!r.provisional && rh(&r) == 4);
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
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .script = {SHR_E_WOULD_BLOCK, 0, 0, 0, SHR_E_WOULD_BLOCK}};
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
    ASSERT(rh(&r) == 4);
    ASSERT_EQ_LL(s.calls, 6);
    harness_close(&h);

    /* Without a clock a refused read waits for shr_asset_ready(), without polling. */
    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_no_clock);
    tsrc s1 = {d.d, d.n, .script = {SHR_E_WOULD_BLOCK, 0, 0, 0, SHR_E_WOULD_BLOCK}};
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
    ASSERT(rh(&r) == 4);
    ASSERT_EQ_LL(s1.calls, 6);
    harness_close(&h);
    PASS();
}

/* Without a clock a ready signal that arrives before any frame ran into the cool-down still ends it. */
TEST test_ready_signal_before_cooling_frame(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_no_clock);
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .script = {SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
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
    ASSERT(!r.provisional && rh(&r) == 4);
    harness_close(&h);
    PASS();
}

/* A source that keeps failing is given up after io_retry_limit cool-downs, until shr_asset_ready(). */
TEST test_failing_source_given_up(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .script = {SHR_E_IO}};
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
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .script = {SHR_E_WOULD_BLOCK}};
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
        static spkg dl, dc;
        synth(&dl);
        synth_as(&dc, ROLE_CJK, "ko", 0x4E00, 0x4E01, SY_A4);
        tsrc sl = {dl.d, dl.n, .script = {SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
        tsrc sc = {dc.d, dc.n, .mode = SRC_READ};
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
        ASSERT(!r.provisional && rh(&r) == 4);
        resolve(f, idc, &r); /* the first frame to run into the CJK cool-down, which is over */
        ASSERT(r.provisional);
        ASSERT(res->ops->has_work(res));
        ASSERT(res->ops->pump(res)); /* redraws, so the next frame tries it again */
        ASSERT(!res->ops->has_work(res));
        ASSERT_EQ_LL(load(ctx, f, idc, &r), SHR_OK);
        ASSERT(!r.provisional && rh(&r) == 4);
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
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .script = {SHR_E_WOULD_BLOCK, SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
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
    ASSERT(!r.provisional && rh(&r) == 4);
    harness_close(&h);
    PASS();
}

/* Failed reads count for the package until it is ready, not per load step. */
TEST test_package_failures_count_until_ready(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .script = {SHR_E_IO, SHR_E_IO, 0, SHR_E_IO, SHR_E_IO}}; /* header, then table */
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
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .open_result = SHR_OK};
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
    ASSERT(!r.provisional && rh(&r) == 4);
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
    static spkg d;
    synth(&d);
    static spkg k;
    synth_as(&k, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    tsrc a = {k.d, k.n, .script = {SHR_IN_PROGRESS}}, c = {d.d, d.n, .script = {[3] = SHR_IN_PROGRESS}};
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
    ASSERT(!r.provisional && rh(&r) == 4);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &r), SHR_OK);
    ASSERT_EQ_LL(rh(&r), 4);
    harness_close(&h);
    PASS();
}

TEST test_read_timeout(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .cancellable = true};
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
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .open_result = SHR_E_IO};
    ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_IO);
    ASSERT_EQ_LL(s.closes, 0); /* nothing was opened */
    s = (tsrc){d.d, d.n, .mode = SRC_NONE};
    ASSERT_EQ_LL(pkg_status(ctx, &s), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(s.closes, 1);
    harness_close(&h);
    PASS();
}

/* A failed package lets go of its buffers and source at once; the font keeps working. */
TEST test_failed_package_released(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    static sy_raw raw;
    sy_init(&raw, ROLE_LATIN, "", 'A', 'B', SY_A4);
    put(raw.box[BOX_MANI] + 28, 2 * SY_NG + 1, 4); /* glyphs not covered: fails after the index was read */
    assemble(&d, &raw, SY_STORED);
    tsrc s = {d.d, d.n, .mode = SRC_READ};
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
    static spkg d;
    synth(&d);
    sy_page(d.d, 0)[100] ^= 0xFF; /* the first page is damaged */
    tsrc s = {d.d, d.n, .mode = SRC_MAP};
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
    ASSERT(inside(&r, d.d, d.n)); /* the second page is fine */
    harness_close(&h);
    PASS();
}

TEST test_cjk_locale(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg ko, ja;
    synth_as(&ko, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    synth_as(&ja, ROLE_CJK, "ja", 'A', 0xAC00, SY_A4);
    tsrc sko = {ko.d, ko.n, .mode = SRC_MAP}, sja = {ja.d, ja.n, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_CJK] = &sko}};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, "ja");
    shr__resolved r;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &r), SHR_OK);
    ASSERT_STR_EQ(l.first, "shiroko-cjk-ja.shrf");
    ASSERT(!inside(&r, ko.d, ko.n));
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_FORMAT); /* a Korean package under the Japanese name */
    font_free_now(ctx, f);
    l.src[ROLE_CJK] = &sja;
    f = font_new(ctx, &l, "ja");
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &r), SHR_OK);
    ASSERT(inside(&r, ja.d, ja.n));
    font_free_now(ctx, f);
    l.src[ROLE_CJK] = &sko;
    l.opens = 0;
    f = h.font = font_new(ctx, &l, NULL); /* Korean by default */
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &r), SHR_OK);
    ASSERT_STR_EQ(l.first, "shiroko-cjk-ko.shrf");
    ASSERT(inside(&r, ko.d, ko.n));
    harness_close(&h);
    PASS();
}

/* ===== Routing between roles ===== */

TEST test_routing(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg latin, cjk, latin2, nerd, sym;
    synth_as(&latin, ROLE_LATIN, "", 'A', 0xAC00, SY_A4);
    synth_as(&cjk, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    synth_as(&latin2, ROLE_LATIN, "", 'A', 0xE0B0, SY_A4);
    synth_as(&nerd, ROLE_NERD, "", 0xE0B0, 0xE0B1, SY_A4);
    synth_as(&sym, ROLE_SYMBOLS, "", 0x2630, 0xAC01, SY_A4);
    tsrc s[5];
    spkg *data[5] = {&latin, &cjk, &latin2, &nerd, &sym};
    for (int i = 0; i < 5; i++) s[i] = (tsrc){data[i]->d, data[i]->n, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_LATIN] = &s[0], [ROLE_CJK] = &s[1]}};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, NULL);
    shr__resolved r;
    load(ctx, f, glyph1(f, 'A'), &r);
    ASSERT(inside(&r, latin.d, latin.n)); /* latin first */
    load(ctx, f, glyph1(f, 0xAC00), &r);
    ASSERT(inside(&r, cjk.d, cjk.n)); /* CJK scalars: CJK first */
    ASSERT_EQ_LL(r.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC); /* text: bold and italic are drawn */
    font_free_now(ctx, f);

    lib l2 = {.src = {[ROLE_LATIN] = &s[2], [ROLE_NERD] = &s[3], [ROLE_SYMBOLS] = &s[4]}};
    f = h.font = font_new(ctx, &l2, NULL);
    load(ctx, f, glyph1(f, 0xE0B0), &r);
    ASSERT(inside(&r, nerd.d, nerd.n)); /* Nerd private use: the Nerd package first */
    ASSERT_EQ_LL(r.synth, 0); /* icons are drawn as they are */
    load(ctx, f, glyph1(f, 0x2630), &r);
    ASSERT(inside(&r, sym.d, sym.n));
    ASSERT_EQ_LL(r.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
    load(ctx, f, glyph1(f, 0xAC01), &r);
    ASSERT(inside(&r, sym.d, sym.n)); /* after CJK and latin */
    load(ctx, f, glyph1(f, 'A'), &r);
    ASSERT(inside(&r, latin2.d, latin2.n));
    harness_close(&h);
    PASS();
}

TEST test_emoji_fallbacks(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg latin, emoji, sym;
    synth_as(&latin, ROLE_LATIN, "", 0x2764, 0x2765, SY_A4);
    synth_as(&emoji, ROLE_EMOJI, "", 0x1F600, 0x1F601, SY_A4);
    synth_as(&sym, ROLE_SYMBOLS, "", 0x1F600, 0x1F602, SY_A4);
    tsrc sl = {latin.d, latin.n, .mode = SRC_MAP}, se = {emoji.d, emoji.n, .mode = SRC_MAP},
         ss = {sym.d, sym.n, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_EMOJI] = &se}};
    shr_pl_res_bitmap_font *f = font_new(ctx, &l, NULL);
    shr__resolved r, fffd;
    uint32_t heart[2] = {0x2764, 0xFE0F}, smile_zw[2] = {0x1F600, 0x200B}, smile_vs[2] = {0x1F600, 0xFE0F},
             zwj[3] = {0x1F469, 0x200D, 0x1F4BB};
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xFFFD), &fffd), SHR_OK);
    load(ctx, f, glyph1(f, 0x1F600), &r);
    ASSERT(inside(&r, emoji.d, emoji.n));
    ASSERT_EQ_LL(r.synth, 0);
    load(ctx, f, glyph(f, heart, 2), &r);
    ASSERT(inside(&r, latin.d, latin.n)); /* registered VS16 base without an emoji glyph: its text glyph */
    ASSERT_EQ_LL(r.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC); /* styled like text */
    load(ctx, f, glyph(f, smile_zw, 2), &r);
    ASSERT(inside(&r, emoji.d, emoji.n)); /* one visible emoji scalar: its emoji glyph */
    load(ctx, f, glyph(f, smile_vs, 2), &r);
    ASSERT(inside(&r, emoji.d, emoji.n)); /* VS16 on a base without variation sequences: the same */
    load(ctx, f, glyph(f, zwj, 3), &r);
    ASSERT(same_image(&r, &fffd)); /* unknown sequence */
    load(ctx, f, glyph1(f, 0x1F602), &r);
    ASSERT(!r.provisional && same_image(&r, &fffd)); /* in no package */
    font_free_now(ctx, f);

    l.src[ROLE_SYMBOLS] = &ss; /* an emoji scalar the emoji package lacks: the text chain */
    f = font_new(ctx, &l, NULL);
    load(ctx, f, glyph1(f, 0x1F602), &r);
    ASSERT(!r.provisional && inside(&r, sym.d, sym.n));
    ASSERT_EQ_LL(r.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
    load(ctx, f, glyph1(f, 0x1F600), &r);
    ASSERT(inside(&r, emoji.d, emoji.n));
    font_free_now(ctx, f);

    l.src[ROLE_EMOJI] = NULL; /* not installed */
    f = font_new(ctx, &l, NULL);
    load(ctx, f, glyph1(f, 0xFFFD), &fffd);
    load(ctx, f, glyph1(f, 0x1F600), &r);
    ASSERT(!r.provisional && inside(&r, sym.d, sym.n));
    load(ctx, f, glyph(f, smile_vs, 2), &r);
    ASSERT(!r.provisional && inside(&r, sym.d, sym.n));
    load(ctx, f, glyph1(f, 0x1F601), &r);
    ASSERT(!r.provisional && same_image(&r, &fffd));
    font_free_now(ctx, f);

    tsrc pending = {emoji.d, emoji.n, .script = {SHR_IN_PROGRESS}};
    l.src[ROLE_EMOJI] = &pending;
    f = h.font = font_new(ctx, &l, NULL);
    load(ctx, f, glyph1(f, 0xFFFD), &fffd);
    resolve(f, glyph1(f, 0x1F600), &r);
    shr_pump(ctx);
    resolve(f, glyph(f, heart, 2), &r);
    ASSERT(r.provisional && same_image(&r, &fffd)); /* loading emoji: provisional U+FFFD, not a text fallback */
    ASSERT_EQ_LL(r.synth, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC); /* the built-in fallback is text */
    ts_complete(ctx, &pending, SHR_OK);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0x1F600), &r), SHR_OK);
    ASSERT(!r.provisional && rh(&r) == 4);
    harness_close(&h);
    PASS();
}

TEST test_deadline_is_the_earliest_retry(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg latin, cjk, emoji;
    synth(&latin);
    synth_as(&cjk, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    synth_as(&emoji, ROLE_EMOJI, "", 0x1F600, 0x1F601, SY_A4);
    const shr_status wb = SHR_E_WOULD_BLOCK;
    tsrc sl = {latin.d, latin.n, .script = {wb, 0, 0, 0, wb, wb}}, sc = {cjk.d, cjk.n, .script = {wb, 0, 0, 0, wb}},
         se = {emoji.d, emoji.n, .script = {wb}};
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
        resolve_at(res, ids[i], frame_no + 1, &r);
        shr_pump(ctx);
    }
    res->ops->frame_end(res, ++frame_no);
    ASSERT_EQ_LL(res->ops->deadline(res), 150 * MS);
    ASSERT_EQ_LL(sl.calls + sc.calls + se.calls, 6 + 5 + 4);
    harness_close(&h);
    PASS();
}

/* ===== Page cache ===== */

/* A page waits while the pages of the cache are pinned or drawn by the latest frame; then it evicts the least recently
 * used one and takes its slot. */
TEST test_page_cache_budget(void) {
    static spkg d;
    synth(&d);
    harness h;
    cache_limit = one_page(&d);
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    tsrc s = {d.d, d.n, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    shr__page **pages = NULL;
    uint64_t first = glyph1(f, 'A'), second = glyph2(f);
    shr__resolved r;
    ASSERT_EQ_LL(load(ctx, f, first, &r), SHR_OK);
    uint64_t frame = ++frame_no;
    ASSERT_EQ_LL(resolve_at(res, first, frame, &r), SHR_OK); /* pinned by the frame */
    ASSERT_EQ_LL(resolve_at(res, second, frame, &r), SHR_OK);
    ASSERT(r.provisional);
    settle(ctx);
    ASSERT_EQ_LL(f->page_bytes, SY_RES); /* no room while the first page is pinned */
    pages = f->pkg[ROLE_LATIN].pages;
    shr__page_slot *slot = pages[0]->slot;
    shr_deadline dl;
    shr_next_deadline(ctx, &dl);
    ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_NONE); /* waits for the unpin, no polling */
    res->ops->frame_end(res, frame);
    ASSERT(res->ops->has_work(res));
    settle(ctx);
    ASSERT(pages[0]->state == PAGE_READY && pages[1]->state == PAGE_ABSENT); /* the frame drew from the first page */
    ASSERT(!res->ops->has_work(res));
    ASSERT_EQ_LL(s.calls, 4);
    ASSERT_EQ_LL(load(ctx, f, second, &r), SHR_OK); /* a frame without it: evicted, its slot reused */
    ASSERT(pages[0] == NULL && r.offset.y == 3 && r.buf == &slot->buf && rect_is(&r, 0, 4, 4, 8));
    ASSERT_EQ_LL(load(ctx, f, first, &r), SHR_OK); /* loaded again */
    ASSERT(r.offset.y == 0 && r.buf == &slot->buf && rect_is(&r, SY_X0, 1, SY_X0 + 4, 5));
    ASSERT(!inside(&r, d.d, d.n) && slot->buf.mem.height == SY_H);
    ASSERT_EQ_LL(f->page_bytes, SY_RES);
    harness_close(&h);

    cache_limit = SY_SLOT - 1; /* a slot larger than the whole cache fails */
    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    f = h.font = font_new(ctx, &l, NULL);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT(!r.provisional && rh(&r) != 4);
    ASSERT_EQ_LL(last_failure(ctx), SHR_E_LIMIT);
    harness_close(&h);
    PASS();
}

TEST test_page_lru_order(void) {
    static spkg latin, cjk;
    synth(&latin);
    synth_as(&cjk, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    harness h;
    cache_limit = 2 * SY_RES + sy_size(latin.d, 1) + 64; /* two pages and the read of a page 1 */
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    tsrc sl = {latin.d, latin.n, .mode = SRC_READ}, sc = {cjk.d, cjk.n, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_CJK] = &sc}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__resolved r;
    uint64_t first = glyph1(f, 'A');
    load(ctx, f, first, &r);
    load(ctx, f, glyph2(f), &r);
    resolve(f, first, &r); /* used again: now the newest */
    ASSERT_EQ_LL(f->page_bytes, 2 * SY_RES);
    load(ctx, f, glyph1(f, 0xAC00), &r);
    ASSERT(rh(&r) == 4);
    shr__page **pages = f->pkg[ROLE_LATIN].pages;
    ASSERT_EQ_LL(pages[0]->state, PAGE_READY);
    ASSERT(pages[1] == NULL); /* the least recently used */
    ASSERT_EQ_LL(f->page_bytes, 2 * SY_RES);
    harness_close(&h);

    synth_as(&cjk, ROLE_CJK, "ko", 0xAC00, 0xAC01, SY_A4);
    cache_limit = 2 * SY_RES + sy_size(latin.d, 0) - 1; /* two pages, or a page 0 being read and one page */
    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    sl = (tsrc){latin.d, latin.n, .mode = SRC_READ}, sc = (tsrc){cjk.d, cjk.n, .mode = SRC_READ};
    f = h.font = font_new(ctx, &l, NULL);
    load(ctx, f, first = glyph1(f, 'A'), &r);
    load(ctx, f, glyph2(f), &r);
    shr__page_slot *slot = f->pkg[ROLE_LATIN].pages[0]->slot;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &r), SHR_OK); /* evicts both, takes the slot of the first */
    ASSERT(!r.provisional && rh(&r) == 4 && r.buf == &slot->buf);
    ASSERT(!f->pkg[ROLE_LATIN].pages[0] && !f->pkg[ROLE_LATIN].pages[1] && f->page_bytes == SY_RES);
    harness_close(&h);
    PASS();
}

/* An evicted page's slot takes a page of any package of the same shape (format and page size); a page of another
 * shape gets a new slot. */
TEST test_slots_across_packages(void) {
    static spkg latin, cjk, nerd;
    synth(&latin);
    synth_as(&cjk, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    synth_as(&nerd, ROLE_NERD, "", 0xE0B0, 0xE0B1, SY_A8);
    tsrc sl = {latin.d, latin.n, .mode = SRC_READ}, sc = {cjk.d, cjk.n, .mode = SRC_READ},
         sn = {nerd.d, nerd.n, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_CJK] = &sc, [ROLE_NERD] = &sn}};
    for (int other = 0; other < 2; other++) {
        harness h;
        cache_limit = other ? 2 * SY_RES + sy_size(nerd.d, 1) + 64 : one_page(&latin); /* A8: twice the A4 atlas */
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
        shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
        shr__resolved r;
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
        ASSERT_EQ_LL(r.buf->mem.format, SHR_FORMAT_A4);
        shr__page_slot *slot = f->pkg[ROLE_LATIN].pages[0]->slot;
        uint32_t id = slot->buf.id;
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, other ? 0xE0B1 : 0xAC00), &r), SHR_OK);
        ASSERT(!r.provisional && rect_is(&r, 0, 4, 4, 8) && !f->pkg[ROLE_LATIN].pages[0]);
        shr__page *p = f->pkg[other ? ROLE_NERD : ROLE_CJK].pages[1];
        ASSERT_EQ_LL(r.buf->mem.format, other ? SHR_FORMAT_A8 : SHR_FORMAT_A4);
        if (!other) ASSERT(p->slot == slot && p->slot->buf.id == id);
        ASSERT_EQ_LL(f->page_bytes, other ? SY_H * SY_W + SY_RECS : SY_RES);
        harness_close(&h);
    }
    PASS();
}

/* Through frames: the page that takes an evicted page's slot keeps its buffer id, and the driver receives an
 * update of the whole atlas instead of a new registration. */
TEST test_slot_reuse_keeps_buffer_id(void) {
    static spkg d;
    synth(&d);
    harness h;
    cache_limit = one_page(&d);
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    tsrc s = {d.d, d.n, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr_lyr *layer;
    const shr_text_style white = {SHR_RGB(255, 255, 255), 0, 0};
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &layer), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, f, 1, 2, NULL), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(layer, 0, 0, "A", 1, 1, white), SHR_OK);
    shr_submit(ctx);
    settle(ctx);
    ASSERT_EQ_LL(lit(h.out.shown, 0, 2 * CW, 0, CH), 16); /* the glyph rect only, not its atlas neighbours */
    shr__page_slot *slot = f->pkg[ROLE_LATIN].pages[0]->slot;
    uint32_t id = slot->buf.id;
    int registers = h.drv.registers, updates = h.drv.updates;
    ASSERT(id != 0);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(layer, 0, 0, "A\xCC\x81", 3, 1, white), SHR_OK); /* the second page */
    shr_submit(ctx);
    settle(ctx);
    ASSERT(!f->pkg[ROLE_LATIN].pages[0] && f->pkg[ROLE_LATIN].pages[1]->slot == slot);
    ASSERT_EQ_LL(slot->buf.id, id);
    ASSERT_EQ_LL(h.drv.registers, registers);
    ASSERT(h.drv.updates > updates);
    ASSERT_EQ_LL(lit(h.out.shown, 0, 2 * CW, 3, CH), 16); /* three rows lower */
    ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
    harness_close(&h);
    PASS();
}

static void tweak_stride(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)d;
    drv->caps.stride_align = 64;
}

static fail_alloc dma;
static shr_allocator dma_allocator;
static void tweak_dma_only(shr_context_desc *d, shr_framebuffer_driver *drv) {
    dma = (fail_alloc){-1, 0};
    dma_allocator = fail_allocator(&dma);
    d->allocator = &dma_allocator;
    drv->caps.domains = SHR_MEMORY_DMA;
}

static void tweak_narrow(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)d;
    drv->caps.max_buffer_width = 64;
}

/* Pages the driver cannot draw from where they are (here: rows of 32 bytes, the driver wants 64) are decoded into
 * slots of its stride: mapped pages from the mapping, read pages restrided in place after decoding. So are the built-in
 * pages, outside the page cache, when the driver reaches no CPU memory; with no buffer at all the font fails. */
TEST test_pages_the_driver_cannot_reach(void) {
    static spkg latin, cjk;
    synth(&latin);
    synth_as(&cjk, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_stride);
    tsrc sl = {latin.d, latin.n, .mode = SRC_MAP}, sc = {cjk.d, cjk.n, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_CJK] = &sc}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__resolved r;
    const uint32_t cps[3] = {'A', 0xAC00, 'C'};
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_LL(load(ctx, f, glyph1(f, cps[i]), &r), SHR_OK);
        ASSERT(!r.provisional && inside(&r, shr__builtin_package, shr__builtin_package_size) == (i == 2));
        ASSERT(i == 2 || r.buf->mem.stride == 64); /* the built-in atlas rows are multiples of 64 bytes */
    }
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT(!inside(&r, latin.d, latin.n) && rect_is(&r, SY_X0, 1, SY_X0 + 4, 5) && r.offset.x == 1);
    ASSERT(cov(r.buf, SY_X0, 1) == 255 && cov(r.buf, SY_X0 + 3, 4) == 255 && cov(r.buf, SY_X0 - 1, 1) == 0xA * 17);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 0xAC00), &r), SHR_OK); /* read, then re-strided in place */
    ASSERT(rect_is(&r, 0, 4, 4, 8) && r.offset.y == 3 && cov(r.buf, 3, 7) == 255 && cov(r.buf, 4, 7) == 0x5 * 17);
    ASSERT(cov(r.buf, 0, 3) == 0x5 * 17 && cov(r.buf, 0, SY_H1) == 0 && r.buf->mem.height == SY_H);
    ASSERT_EQ_LL(f->page_bytes, 2 * (SY_H * 64 + SY_RECS));
    harness_close(&h);

    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_dma_only);
    sl.mode = SRC_MAP;
    f = h.font = font_new(ctx, &l, NULL);
    shr__resolved a;
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'C'), &r), SHR_OK);
    ASSERT(!inside(&r, shr__builtin_package, shr__builtin_package_size) && r.buf->mem.domain == SHR_MEMORY_DMA);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &a), SHR_OK);
    ASSERT(!a.provisional && !inside(&a, latin.d, latin.n) && a.buf->mem.domain == SHR_MEMORY_DMA);
    ASSERT_EQ_LL(cov(a.buf, SY_X0, 1), 255);
    ASSERT_EQ_LL(f->page_bytes, SY_RES); /* the built-in copies are not charged */
    harness_close(&h);
    ASSERT_EQ_LL(dma.live, 0);

    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_narrow);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    f = (shr_pl_res_bitmap_font *)&h;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(ctx, &fd, &f), SHR_E_UNSUPPORTED);
    ASSERT(f == NULL);
    harness_close(&h);
    PASS();
}

/* Only pages the latest frame wanted are loaded; descriptors exist only for pages in use. */
TEST test_stale_wants_dropped(void) {
    static spkg d;
    synth(&d);
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    tsrc s = {d.d, d.n, .mode = SRC_READ};
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
    ASSERT_EQ_LL(rh(&r), 4);
    harness_close(&h);
    PASS();
}

/* Pages one frame wants do not evict each other: one that does not fit waits for a frame without the other. */
TEST test_frame_pages_kept(void) {
    static spkg d;
    synth(&d);
    harness h;
    cache_limit = one_page(&d);
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    tsrc s = {d.d, d.n, .script = {[3] = SHR_E_WOULD_BLOCK, [4] = SHR_IN_PROGRESS}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    const uint64_t ids[2] = {glyph1(f, 'A'), glyph2(f)};
    shr__resolved r;
    resolve(f, ids[0], &r);
    settle(ctx);
    resolve_all(f, ids, 2);
    shr_pump(ctx); /* the first page backs off, the second is read */
    ASSERT_EQ_LL(s.pending, 1);
    fake_now = 50 * MS;
    ts_complete(ctx, &s, SHR_OK);
    shr_pump(ctx); /* the second becomes ready; the first may not evict it */
    shr__page **pages = f->pkg[ROLE_LATIN].pages;
    ASSERT_EQ_LL(s.calls, 5);
    ASSERT(pages[0]->state == PAGE_ABSENT && pages[1]->state == PAGE_READY && f->wants.len == 1);
    ASSERT(!res->ops->has_work(res));
    settle(ctx);
    ASSERT_EQ_LL(s.calls, 5);
    ASSERT_EQ_LL(load(ctx, f, ids[0], &r), SHR_OK); /* a frame without the second */
    ASSERT(!r.provisional && rh(&r) == 4 && !pages[1] && s.calls == 6);
    harness_close(&h);
    PASS();
}

/* A page that became ready and is evicted before the pump drops it from the wants. */
TEST test_evict_wanted_page(void) {
    static spkg d;
    synth(&d);
    harness h;
    cache_limit = one_page(&d);
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    tsrc s = {d.d, d.n, .script = {[3] = SHR_E_WOULD_BLOCK, [4] = SHR_IN_PROGRESS}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    const uint64_t ids[2] = {glyph2(f), glyph1(f, 'A')};
    shr__resolved r;
    resolve(f, ids[0], &r);
    settle(ctx);
    resolve(f, ids[0], &r);
    shr_pump(ctx); /* the second page backs off */
    resolve_all(f, ids, 2);
    shr_pump(ctx); /* the first is read */
    ASSERT_EQ_LL(s.pending, 1);
    resolve(f, ids[0], &r); /* the latest frame wants only the second */
    fake_now = 50 * MS;
    ts_complete(ctx, &s, SHR_OK);
    shr_pump(ctx); /* the first becomes ready, the second evicts it, then the first leaves the wants */
    shr__page **pages = f->pkg[ROLE_LATIN].pages;
    ASSERT_EQ_LL(s.calls, 6);
    ASSERT(!pages[0] && pages[1]->state == PAGE_READY && f->wants.len == 0);
    harness_close(&h);
    PASS();
}

/* Packages open in the pump even while every read slot is taken. */
TEST test_open_while_blocked(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_one_read);
    static spkg d, k, n;
    synth(&d);
    synth_as(&k, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    synth_as(&n, ROLE_NERD, "", 0xE0B0, 0xE0B1, SY_A4);
    tsrc sl = {d.d, d.n, .script = {SHR_IN_PROGRESS}}, sc = {k.d, k.n, .mode = SRC_READ}, sn = {n.d, n.n, .mode = SRC_READ};
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
        ASSERT_EQ_LL(rh(&r), 4);
    }
    harness_close(&h);
    PASS();
}

TEST test_pins_per_frame(void) {
    static spkg d;
    synth(&d);
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    tsrc s = {d.d, d.n, .mode = SRC_READ};
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
    ASSERT(!r.provisional && rh(&r) == 4);
    shr__page *p = f->pkg[ROLE_LATIN].pages[0], *q = f->pkg[ROLE_LATIN].pages[1];
    for (int i = 0; i < 4; i++) resolve_at(res, i % 2 ? sa : a, 5, &r); /* one stamp per frame */
    resolve_at(res, a, 6, &r);
    resolve_at(res, second, 6, &r);
    resolve_at(res, sa, 6, &r);
    ASSERT(p->pin[0] == 5 && p->pin[1] == 6 && q->pin[0] == 6 && q->pin[1] == 0);
    ASSERT(!f->lru);
    ASSERT_EQ_LL(resolve_at(res, a, 7, &r), SHR_E_LIMIT); /* a third frame in flight */
    res->ops->frame_end(res, 5);
    ASSERT(p->pin[0] == 0 && p->pin[1] == 6);
    ASSERT(!f->lru);
    ASSERT_EQ_LL(resolve_at(res, a, 7, &r), SHR_OK);
    ASSERT_EQ_LL(p->pin[0], 7);
    res->ops->frame_end(res, 6);
    ASSERT(f->lru == &q->lru && !q->lru.next);
    res->ops->frame_end(res, 7);
    ASSERT(f->pinned == NULL && f->lru == &p->lru && p->lru.next == &q->lru);
    harness_close(&h);
    PASS();
}

/* ===== Destroy and shutdown with reads outstanding ===== */

TEST test_destroy_with_outstanding_reads(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    synth(&d);
    for (int cancellable = 0; cancellable < 2; cancellable++) {
        tsrc s = {d.d, d.n, .cancellable = cancellable, .script = {0, 0, 0, SHR_IN_PROGRESS}};
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
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .cancellable = true, .script = {SHR_IN_PROGRESS}};
    tsrc u = {d.d, d.n, .script = {0, 0, 0, SHR_IN_PROGRESS}};
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

TEST test_provisional_redrawn_when_ready(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static spkg d;
    synth(&d);
    tsrc s = {d.d, d.n, .mode = SRC_READ};
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

static shr_framebuffer_driver short_driver;
static int short_frames; /* frames whose buffers got no id */
static void short_trace(void *user, const shr_trace_event *ev) {
    (void)user;
    short_frames += ev->kind == SHR_TRACE_RASTER_END && ev->value1;
}
static void tweak_short_ids(shr_context_desc *d, shr_framebuffer_driver *drv) {
    d->trace = short_trace;
    drv->caps.max_buffers = 3;
}
static void tweak_short_keeps(shr_context_desc *d, shr_framebuffer_driver *drv) {
    d->trace = short_trace;
    shr_software_driver_create(NULL, 1u << 20, 64, 4, &short_driver);
    *drv = short_driver;
}

/* Real glyphs among the cells of test_buffer_ids_run_short. */
static int short_drawn(const harness *h) {
    const int32_t at[4][3] = {{0, 0, 1}, {0, 1, 1}, {0, 2, 2}, {1, 0, 2}}; /* row, column, span */
    int n = 0;
    for (int i = 0; i < 4; i++)
        n += lit(h->out.shown, at[i][1] * CW, (at[i][1] + at[i][2]) * CW, at[i][0] * CH, (at[i][0] + 1) * CH) == 16;
    return n;
}

/* A frame drawing from more pages than the driver has buffer ids is presented: the pages that got an id draw, the
 * others the provisional fallback (one id stays for its page), and on a preserved output the next frame redraws them
 * once the ids are free, also with bands and outside the row cache. It never fails nor keeps redrawing. */
TEST test_buffer_ids_run_short(void) {
    static spkg latin, cjk;
    static uint8_t band_px[2][HW * 8 * SCREEN_BPP];
    synth(&latin);
    synth_as(&cjk, ROLE_CJK, "ko", 0xAC00, 0xAC01, SY_A4);
    for (int mode = 0; mode < 6; mode++) { /* preserved or not; plain, bands, keeps */
        bool preserved = mode & 1;
        tsrc sl = {latin.d, latin.n, .mode = SRC_READ}, sc = {cjk.d, cjk.n, .mode = SRC_READ};
        lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_CJK] = &sc}};
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT | (preserved ? SHR_OUTPUT_PRESERVES_CONTENT : 0),
                                        mode >> 1 == 2 ? tweak_short_keeps : tweak_short_ids);
        if (mode >> 1 == 1) {
            shr_surface b[2];
            for (int i = 0; i < 2; i++)
                b[i] = (shr_surface){band_px[i], HW, 8, HW * SCREEN_BPP, sizeof(band_px[i]), SHR_PIXEL_FORMAT, 1, 0, 0};
            shr_screen_desc sd;
            shr_screen_desc_init(&sd);
            sd.width = HW, sd.height = HH, sd.bands = b, sd.band_count = 2;
            ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_OK);
        }
        shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
        shr_lyr *layer;
        const shr_text_style white = {SHR_RGB(255, 255, 255), 0, 0};
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &layer), SHR_OK);
        const shr_color bg = SHR_RGB(0, 0, 0);
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(layer, f, 2, 4, &bg), SHR_OK); /* opaque, cacheable rows */
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(layer, 0, 0, "A", 1, 1, white), SHR_OK); /* latin page 0 */
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(layer, 0, 1, "\xF3\xB0\x80\x90", 4, 1, white), SHR_OK); /* page 1 */
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(layer, 0, 2, "\xEA\xB0\x80", 3, 2, white), SHR_OK);   /* cjk 0 */
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(layer, 1, 0, "\xEA\xB0\x81", 3, 2, white), SHR_OK);   /* cjk 1 */
        for (int redraw = 0; redraw < 3; redraw++) { /* the rows are stored and drawn from the row cache */
            short_frames = 0;
            if (redraw) shr_request_redraw(ctx);
            shr_submit(ctx);
            settle(ctx);
            shr_deadline dl;
            ASSERT(shr_next_deadline(ctx, &dl) == SHR_OK && dl.kind != SHR_DEADLINE_NOW);
            ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_PRESENT_FAILED, NULL), 0);
            if (mode >> 1 == 2 && redraw) { /* both rows stored whole, only once whole: no buffer ids needed */
                ASSERT(short_drawn(&h) == 4 && !short_frames && ctx->keep_resident);
                continue;
            }
            ASSERT_EQ_LL(short_drawn(&h), preserved ? 4 : 2 + (mode >> 1 == 2));
            ASSERT(lit(h.out.shown, 2 * CW, 4 * CW, 0, CH) > 0 && lit(h.out.shown, 0, 2 * CW, CH, 2 * CH) > 0);
            ASSERT_EQ_LL(short_frames, 1);
            if (mode >> 1 == 2) /* the row with a fallback is not kept */
                ASSERT_EQ_LL(ctx->keep_resident, (preserved ? 2u : 1u) * 4u * CW * CH * SCREEN_BPP);
        }
        ASSERT_EQ_LL(shr_lyr_destroy(layer), SHR_OK);
        harness_close(&h);
        if (mode >> 1 == 2) shr_software_driver_destroy(&short_driver);
    }
    PASS();
}

static shr_framebuffer_driver cached_driver;
static void tweak_cached(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)d;
    shr_software_driver_create(NULL, 1u << 20, 64, 64, &cached_driver);
    *drv = cached_driver;
}

/* A package that cools down after I/O errors draws the fallback provisionally, outside the row cache, and
 * the real glyph appears by itself once the cool-down ends: after io_retry_ns, or at shr_asset_ready(). */
TEST test_cooled_package_redrawn(void) {
    for (int by_signal = 0; by_signal < 2; by_signal++) {
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cached);
        static spkg d;
        synth(&d);
        tsrc s = {d.d, d.n, .script = {SHR_E_IO, SHR_E_IO, SHR_E_IO, SHR_E_IO}};
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
    static spkg d;
    synth(&d);
    sy_page(d.d, 0)[100] ^= 0xFF;
    tsrc s = {d.d, d.n, .mode = SRC_MAP};
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

/* ===== Preloading ===== */

static shr_status preload(shr_pl_res_bitmap_font *f, const char *package, uint32_t pages) {
    return shr_pl_res_bitmap_font_preload(f, package, pages);
}

/* The leading pages of packages load without frames, one page per pump, opening the packages; a frame then draws them
 * without reads. */
TEST test_preload(void) {
    static spkg latin, cjk;
    synth(&latin);
    synth_as(&cjk, ROLE_CJK, "ko", 'A', 0xAC00, SY_A4);
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    tsrc sl = {latin.d, latin.n, .mode = SRC_READ}, sc = {cjk.d, cjk.n, .mode = SRC_MAP};
    lib l = {.src = {[ROLE_LATIN] = &sl, [ROLE_CJK] = &sc}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    ASSERT_EQ_LL(preload(NULL, "shiroko-latin.shrf", 1), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(preload(f, NULL, 1), SHR_E_INVALID_ARG);
    res->dead = true;
    ASSERT_EQ_LL(preload(f, "shiroko-latin.shrf", 1), SHR_E_STATE); /* destroyed */
    res->dead = false;
    ASSERT_EQ_LL(preload(f, "shiroko-cjk-ja.shrf", 1), SHR_E_NOT_FOUND); /* not this font's locale */
    ASSERT_EQ_LL(preload(f, "shiroko-latin.shrf", 0), SHR_OK);
    ASSERT_EQ_LL(preload(f, "shiroko-cjk-ko.shrf", 0), SHR_OK);
    ASSERT_EQ_LL(preload(f, "shiroko-emoji.shrf", 1), SHR_OK); /* not installed */
    ASSERT(res->ops->has_work(res));
    settle(ctx);
    ASSERT(f->pkg[ROLE_LATIN].state == PKG_READY && f->pkg[ROLE_CJK].state == PKG_READY);
    ASSERT_EQ_LL(f->pkg[ROLE_EMOJI].state, PKG_ABSENT);
    ASSERT_EQ_LL(sl.calls, 3); /* header, index, metadata */
    shr__page **lp = f->pkg[ROLE_LATIN].pages, **cp = f->pkg[ROLE_CJK].pages;
    ASSERT_EQ_LL(preload(f, "shiroko-cjk-ko.shrf", 9), SHR_OK); /* beyond its two pages */
    shr_pump(ctx);
    ASSERT(cp[0]->state == PAGE_READY && !cp[1]);
    shr_pump(ctx);
    ASSERT(cp[1]->state == PAGE_READY);
    ASSERT_EQ_LL(preload(f, "shiroko-latin.shrf", 1), SHR_OK);
    shr_pump(ctx);
    ASSERT(lp[0]->state == PAGE_LOADING && sl.calls == 4);
    ASSERT(!res->ops->has_work(res)); /* the read ends in the next pump */
    settle(ctx);
    ASSERT(lp[0]->state == PAGE_READY && !lp[1] && sl.calls == 4);
    shr__resolved r;
    ASSERT_EQ_LL(resolve(f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT(!r.provisional && rh(&r) == 4 && sl.calls == 4);
    ASSERT_EQ_LL(resolve(f, glyph1(f, 0xAC00), &r), SHR_OK);
    ASSERT(!r.provisional && rh(&r) == 4);
    shr_begin_shutdown(ctx);
    ASSERT_EQ_LL(preload(f, "shiroko-latin.shrf", 1), SHR_E_STATE); /* shutting down */
    harness_close(&h);
    PASS();
}

/* Pages frames wait for go first; one preload read is in flight at a time. */
TEST test_preload_after_frames(void) {
    static spkg d;
    synth(&d);
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    tsrc s = {d.d, d.n, .script = {[3] = SHR_IN_PROGRESS, [4] = SHR_IN_PROGRESS}};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    shr__resolved r;
    resolve(f, glyph2(f), &r);
    settle(ctx);
    resolve(f, glyph2(f), &r);
    ASSERT_EQ_LL(preload(f, "shiroko-latin.shrf", 2), SHR_OK);
    settle(ctx);
    ASSERT(s.calls == 4 && s.pending == 1); /* the frame's page */
    ts_complete(ctx, &s, SHR_OK);
    shr_pump(ctx);
    shr__page **pages = f->pkg[ROLE_LATIN].pages;
    ASSERT(pages[1]->state == PAGE_READY && pages[0]->state == PAGE_LOADING && s.calls == 5);
    ASSERT(!res->ops->has_work(res));
    ASSERT_EQ_LL(resolve(f, glyph1(f, 'A'), &r), SHR_OK); /* wanted while preloading */
    ASSERT(r.provisional);
    ts_complete(ctx, &s, SHR_OK);
    ASSERT_EQ_LL(load(ctx, f, glyph1(f, 'A'), &r), SHR_OK);
    ASSERT(!r.provisional && rh(&r) == 4 && s.calls == 5);
    harness_close(&h);
    PASS();
}

/* A preload ends where the cache would have to evict; it skips pages that exist, whose read is refused or whose
 * descriptor cannot be allocated; read packages wait while reads are blocked, mapped ones go on. */
TEST test_preload_limits(void) {
    static spkg d, cjk, sym, nerd;
    synth(&d);
    synth_as(&cjk, ROLE_CJK, "ko", 0xAC00, 0xAC01, SY_A4);
    synth_as(&sym, ROLE_SYMBOLS, "", 0x2630, 0x2631, SY_A4);
    synth_as(&nerd, ROLE_NERD, "", 0xE0B0, 0xE0B1, SY_A4);
    harness h;
    cache_limit = one_page(&d);
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_cache);
    tsrc s = {d.d, d.n, .mode = SRC_READ};
    lib l = {.src = {[ROLE_LATIN] = &s}};
    shr_pl_res_bitmap_font *f = h.font = font_new(ctx, &l, NULL);
    ASSERT_EQ_LL(preload(f, "shiroko-latin.shrf", 2), SHR_OK);
    settle(ctx);
    ASSERT(f->pkg[ROLE_LATIN].pages[0]->state == PAGE_READY && !f->pkg[ROLE_LATIN].pages[1]);
    ASSERT_EQ_LL(f->pkg[ROLE_LATIN].preload, 0);
    harness_close(&h);

    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    s = (tsrc){d.d, d.n, .script = {[3] = SHR_E_WOULD_BLOCK}};
    f = h.font = font_new(ctx, &l, NULL);
    ASSERT_EQ_LL(preload(f, "shiroko-latin.shrf", 2), SHR_OK);
    settle(ctx);
    ASSERT(!f->pkg[ROLE_LATIN].pages[0] && f->pkg[ROLE_LATIN].pages[1]->state == PAGE_READY && s.calls == 5);
    harness_close(&h);

    oom_allocator = fail_allocator(&oom);
    oom = (fail_alloc){-1, 0};
    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_oom);
    tsrc c = {cjk.d, cjk.n, .mode = SRC_MAP};
    l = (lib){.src = {[ROLE_CJK] = &c}};
    f = h.font = font_new(ctx, &l, NULL);
    shr__res *res = shr__bitmap_font_res(f);
    ASSERT_EQ_LL(preload(f, "shiroko-cjk-ko.shrf", 0), SHR_OK);
    res->ops->pump(res);
    ASSERT_EQ_LL(f->pkg[ROLE_CJK].state, PKG_READY);
    ASSERT_EQ_LL(preload(f, "shiroko-cjk-ko.shrf", 2), SHR_OK);
    oom.budget = 0;
    res->ops->pump(res);
    oom.budget = -1;
    settle(ctx);
    ASSERT(!f->pkg[ROLE_CJK].pages[0] && f->pkg[ROLE_CJK].pages[1]->state == PAGE_READY);
    harness_close(&h);

    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_one_read);
    tsrc sy = {sym.d, sym.n, .script = {SHR_IN_PROGRESS}}, n = {nerd.d, nerd.n, .mode = SRC_READ};
    s = (tsrc){d.d, d.n, .mode = SRC_READ}, c = (tsrc){cjk.d, cjk.n, .mode = SRC_MAP};
    l = (lib){.src = {[ROLE_LATIN] = &s, [ROLE_CJK] = &c, [ROLE_SYMBOLS] = &sy, [ROLE_NERD] = &n}};
    f = h.font = font_new(ctx, &l, NULL);
    res = shr__bitmap_font_res(f);
    shr__resolved r;
    load(ctx, f, glyph1(f, 'A'), &r);
    ASSERT_EQ_LL(preload(f, "shiroko-symbols.shrf", 1), SHR_OK);
    shr_pump(ctx);
    ASSERT_EQ_LL(sy.pending, 1); /* holds the only read */
    ASSERT_EQ_LL(preload(f, "shiroko-latin.shrf", 2), SHR_OK);
    ASSERT_EQ_LL(preload(f, "shiroko-nerd.shrf", 1), SHR_OK);
    ASSERT_EQ_LL(preload(f, "shiroko-cjk-ko.shrf", 1), SHR_OK);
    shr_pump(ctx); /* the Nerd package finds no free read */
    ASSERT(f->pkg[ROLE_CJK].pages[0]->state == PAGE_READY && f->pkg[ROLE_LATIN].preload_next == 0);
    ASSERT(!res->ops->has_work(res));
    ts_complete(ctx, &sy, SHR_OK);
    settle(ctx);
    ASSERT(f->pkg[ROLE_LATIN].pages[1]->state == PAGE_READY && f->pkg[ROLE_NERD].pages[0]->state == PAGE_READY);
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
    for (uint32_t i = 0; b.size >= 128 && i < shr__rd32(b.data + 144); i++)
        if (!memcmp(b.data + 152 + 32 * i, "inst", 4)) ppem = shr__rd32(b.data + shr__rd64(b.data + 160 + 32 * i) + 24);
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
            ASSERT(b.offset.x >= 0 && b.offset.x + rw(&b) <= 2 * CW && b.offset.y >= 0 && b.offset.y + rh(&b) <= CH);
    }
    for (size_t i = 0; i < 2; i++) { /* the atlas has pixels right next to the rect: drivers must read only the rect */
        ASSERT_EQ_LL(real_load(ctx, f, i ? &singles[0] : (const uint32_t[]){'A'}, 1, &b), SHR_OK);
        int n = 0;
        for (int32_t y = b.rect.y0 - 1; y <= b.rect.y1; y++)
            for (int32_t x = b.rect.x0 - 1; x <= b.rect.x1; x++)
                if (x >= 0 && y >= 0 && x < b.buf->mem.width && y < b.buf->mem.height &&
                    (x < b.rect.x0 || x == b.rect.x1 || y < b.rect.y0 || y == b.rect.y1))
                    n += cov(b.buf, x, y) != 0;
        ASSERT(n > 0);
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
        ASSERT(!r.provisional && !inside(&r, shr__builtin_package, shr__builtin_package_size) &&
               !same_image(&r, &fffd) && !same_image(&r, &woman));
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
    RUN_TEST(test_mapped_waits_for_memory);
    RUN_TEST(test_builtin_without_packages);
    RUN_TEST(test_glyph_ids);
    RUN_TEST(test_package_validation);
    RUN_TEST(test_unknown_boxes);
    RUN_TEST(test_record_limits);
    RUN_TEST(test_zstd_needs_decoder);
    RUN_TEST(test_index_size_limit);
    RUN_TEST(test_index_changed_between_reads);
    RUN_TEST(test_page_validation);
    RUN_TEST(test_ctri_lookup);
    RUN_TEST(test_blank_glyphs);
    RUN_TEST(test_cluster_memo);
    RUN_TEST(test_synthetic_glyphs);
    RUN_TEST(test_zstd_round_trip);
    RUN_TEST(test_zstd_frames);
    RUN_TEST(test_trimmed_page);
    RUN_TEST(test_staging_charged);
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
    RUN_TEST(test_slots_across_packages);
    RUN_TEST(test_slot_reuse_keeps_buffer_id);
    RUN_TEST(test_pages_the_driver_cannot_reach);
    RUN_TEST(test_stale_wants_dropped);
    RUN_TEST(test_open_while_blocked);
    RUN_TEST(test_frame_pages_kept);
    RUN_TEST(test_evict_wanted_page);
    RUN_TEST(test_pins_per_frame);
    RUN_TEST(test_destroy_with_outstanding_reads);
    RUN_TEST(test_shutdown_with_outstanding_reads);
    RUN_TEST(test_provisional_redrawn_when_ready);
    RUN_TEST(test_buffer_ids_run_short);
    RUN_TEST(test_cooled_package_redrawn);
    RUN_TEST(test_failed_page_event_in_frames);
    RUN_TEST(test_preload);
    RUN_TEST(test_preload_after_frames);
    RUN_TEST(test_preload_limits);
    RUN_TEST(test_real_packages);
    RUN_TEST(test_real_emoji_sequences);
    RUN_TEST(test_powerline_meets_cell_edge);
    RUN_TEST(test_activation_select);
    GREATEST_MAIN_END();
}
