/* Row groups against the full form: two contexts get the same random row lists, one as row commands, the other unpacked
 * into layer commands, then the same group shifts, removals, clears, resource changes, layer changes and blink time.
 * Invariants:
 *   - both forms accept and reject alike, except that a row whose cache pair does not enclose it is rejected
 *   - every frame sends the driver the same commands and shows the same pixels in both */
#include "fuzz_common.h"

#include "shr_compositor.h"

#define CW SHR_CELL_WIDTH
#define CH SHR_CELL_HEIGHT
#define NGLYPHS 4
#define NGROUPS 6
#define NCMDS 12

/* Glyph id k > 0: cell k % NGLYPHS of an A8 strip; 0: none. */
typedef struct gfake {
    shr__res res;
    shr__buf buf;
    shr__resolved r;
    uint8_t cov[NGLYPHS * CW * CH];
} gfake;

static shr_status gf_resolve(shr__res *r, uint64_t id, uint64_t frame, const shr__resolved **out) {
    (void)frame;
    if (!id) return SHR_E_NOT_FOUND;
    int32_t x = (int32_t)(id % NGLYPHS) * CW;
    gfake *g = (gfake *)r;
    g->r = (shr__resolved){&g->buf, {x, 0, x + CW, CH}, {0, 0}, false, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC, CH};
    *out = &g->r;
    return SHR_OK;
}
static void gf_free(shr__res *r) { shr__buf_free(r->ctx, &((gfake *)r)->buf); }
static const shr__res_ops gf_ops = {.resolve = gf_resolve, .free = gf_free};

typedef struct side {
    shr_framebuffer_driver drv, rec;
    fuzz_output out;
    shr_context *ctx;
    shr_lyr *l;
    gfake g;
    uint64_t hash, count;
} side;

static side sides[2];
static uint64_t now;
static uint64_t clock_fn(void *user) {
    (void)user;
    return now;
}

/* 32-bit FNV-1a, its products computed without wrapping. */
static void mix(uint64_t *h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) *h = ((*h ^ b[i]) * 16777619u) & 0xFFFFFFFFu;
}

/* The fields each kind reads. */
static shr_status rec_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t count, shr_fence fence) {
    side *s = user;
    for (size_t i = 0; i < count; i++) {
        const shr_draw_cmd *c = &cmds[i];
        uint8_t k = c->kind;
        bool glyph = k == SHR_CMD_GLYPH, pixels = glyph || k == SHR_CMD_IMAGE;
        mix(&s->hash, &k, 1);
        if (k != SHR_CMD_FILL && k != SHR_CMD_COPY && k != SHR_CMD_ROTATE && k != SHR_CMD_KEEP_END)
            mix(&s->hash, &c->buffer, sizeof(c->buffer));
        if (glyph || k == SHR_CMD_FILL) mix(&s->hash, &c->color, sizeof(c->color));
        if (glyph) mix(&s->hash, &c->bg, sizeof(c->bg)), mix(&s->hash, &c->slant_axis, sizeof(c->slant_axis));
        if (pixels || k == SHR_CMD_BUFFER_UPDATE) mix(&s->hash, &c->src_rect, sizeof(c->src_rect));
        if (pixels || k == SHR_CMD_COPY || k == SHR_CMD_KEEP_DRAW) mix(&s->hash, &c->src_origin, sizeof(c->src_origin));
        if (k <= SHR_CMD_KEEP_DRAW && k != SHR_CMD_ROTATE && k != SHR_CMD_KEEP_END) {
            mix(&s->hash, &c->dst, sizeof(c->dst));
            mix(&s->hash, &c->flags, sizeof(c->flags));
        }
    }
    s->count += count;
    return s->drv.execute(s->drv.user, dst, cmds, count, fence);
}

static shr_status rec_reset(void *user) {
    side *s = user;
    return s->drv.reset(s->drv.user);
}

static void open_side(side *s, uint8_t cfg) {
    memset(s, 0, sizeof(*s));
    FUZZ_CHECK(shr_software_driver_create(NULL, 1024u << (cfg >> 5), (cfg & 1) ? 8 : 0, 64, &s->drv) == SHR_OK);
    s->rec = s->drv;
    s->rec.user = s, s->rec.execute = rec_execute, s->rec.reset = rec_reset;
    if (cfg & 16) s->rec.caps.flags |= SHR_DRIVER_CHEAP_MOVE;
    shr_output o;
    fuzz_output_init(&s->out, &o, (cfg & 2) ? SHR_OUTPUT_PRESERVES_CONTENT : 0);
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.driver = &s->rec, cd.output = &o, cd.now_ns = clock_fn;
    if (cfg & 4) cd.blink = (shr_blink_profile){100, 0, (cfg & 8) != 0, SHR_BLINK_RESTART_NONE};
    FUZZ_CHECK(shr_create(&cd, &s->ctx) == SHR_OK);
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = FUZZ_W, sd.height = FUZZ_H, sd.clear = SHR_RGB(1, 2, 3);
    FUZZ_CHECK(shr_screen_configure(s->ctx, &sd) == SHR_OK);
    FUZZ_CHECK(shr__res_attach(s->ctx, &s->g.res, &gf_ops) == SHR_OK);
    for (size_t i = 0; i < sizeof(s->g.cov); i++) s->g.cov[i] = (uint8_t)(i * 37 % 251);
    shr_image m = {s->g.cov, NGLYPHS * CW, CH, NGLYPHS * CW, sizeof(s->g.cov), SHR_FORMAT_A8, SHR_MEMORY_CPU};
    FUZZ_CHECK(shr__buf_wrap(s->ctx, &m, &s->g.buf) == SHR_OK);
    FUZZ_CHECK(shr_lyr_create(s->ctx, 0, (shr_rect){0, 0, FUZZ_W, FUZZ_H}, &s->l) == SHR_OK);
}

static void close_side(side *s) {
    FUZZ_CHECK(shr_lyr_destroy(s->l) == SHR_OK);
    s->g.res.dead = true;
    FUZZ_CHECK(shr_begin_shutdown(s->ctx) == SHR_OK);
    shr_pump(s->ctx);
    FUZZ_CHECK(shr_destroy(s->ctx) == SHR_OK);
    FUZZ_CHECK(shr_software_driver_destroy(&s->drv) == SHR_OK);
}

static void render(void) {
    for (int k = 0; k < 2; k++) {
        FUZZ_CHECK(shr_submit(sides[k].ctx) == SHR_OK);
        fuzz_settle(sides[k].ctx);
    }
    FUZZ_CHECK(sides[0].hash == sides[1].hash && sides[0].count == sides[1].count);
    FUZZ_CHECK(memcmp(sides[0].out.pixels, sides[1].out.pixels, sizeof(sides[0].out.pixels)) == 0);
}

static const uint8_t kinds[8] = {SHR__LCMD_FILL, SHR__LCMD_FILL, SHR__LCMD_GLYPH, SHR__LCMD_GLYPH,
                                 SHR__LCMD_GLYPH, SHR__LCMD_CACHE_BEGIN, SHR__LCMD_CACHE_END, 0};

/* A row as the generator makes it: ids and bg only on glyphs, as row commands keep them; a cache pair's group starts
 * with an opaque FILL of its area, as cache groups must (the keep holds what the group draws). */
static size_t read_row(fuzz_reader *r, shr__rcmd *c, uint64_t key[2], bool *encloses) {
    uint8_t head = fr_u8(r);
    size_t n = head % (NCMDS + 1);
    bool pair = (head & 0x80) && n >= 2;
    key[0] = fr_u8(r) % 4, key[1] = fr_u8(r) % 4;
    *encloses = true;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = fr_u8(r), x = fr_u8(r), y = fr_u8(r), f = fr_u8(r), e = fr_u8(r);
        uint8_t kind = pair && (i == 0 || i == n - 1) ? (i ? SHR__LCMD_CACHE_END : SHR__LCMD_CACHE_BEGIN) : kinds[b % 8];
        bool glyph = kind == SHR__LCMD_GLYPH;
        uint16_t x0 = x % 9;
        uint8_t y0 = (uint8_t)(y % FUZZ_H);
        c[i] = (shr__rcmd){x0, (uint16_t)(x0 + (x >> 4) % 5 - (x == 0xFF)), y0, (uint8_t)(y0 + (b >> 3) % (CH + 1)),
                           kind, (uint8_t)(f & (f & 0x40 ? 0xFF : 0x8F)), SHR_RGB(b & 0xC0, x & 0xC0, 9),
                           glyph ? (uint32_t)e % (NGLYPHS + 1) : 0, glyph ? SHR_RGB(e & 0xC0, 0, 40) : 0};
        if (kind == SHR__LCMD_CACHE_END && !(b & 7)) c[i].x0 = c[i].x1 = 0, c[i].y0 = c[i].y1 = 0;
        *encloses &= (kind != SHR__LCMD_CACHE_BEGIN || i == 0) && (kind != SHR__LCMD_CACHE_END || i == n - 1);
    }
    if (*encloses && n >= 2 && c[0].kind == SHR__LCMD_CACHE_BEGIN && c[n - 1].kind == SHR__LCMD_CACHE_END) {
        if (n == 2)
            c[0].kind = SHR__LCMD_FILL, c[0].flags = 0;
        else
            c[1] = (shr__rcmd){c[0].x0, c[0].x1, c[0].y0, c[0].y1, SHR__LCMD_FILL, 0, c[1].color, 0, 0};
    }
    return n;
}

static void unpack(const shr__rcmd *c, size_t n, shr__res *res, const uint64_t key[2], shr__lcmd *out) {
    for (size_t i = 0; i < n; i++) {
        shr_rect dst = {c[i].x0 * CW, c[i].y0, c[i].x1 * CW, c[i].y1};
        out[i] = (shr__lcmd){.kind = c[i].kind, .flags = c[i].flags, .dst = dst, .color = c[i].color};
        if (c[i].kind == SHR__LCMD_CACHE_BEGIN) {
            out[i].key[0] = key[0], out[i].key[1] = key[1];
        } else if (c[i].kind == SHR__LCMD_GLYPH) {
            out[i].anchor = (shr_point){dst.x0, dst.y0}, out[i].bg = c[i].bg, out[i].id = c[i].id, out[i].res = res;
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_reader r = {data, size};
    uint8_t cfg = fr_u8(&r);
    now = 0;
    for (int k = 0; k < 2; k++) open_side(&sides[k], cfg);
    for (int steps = 0; r.n > 0 && steps < 64; steps++) {
        uint8_t op = fr_u8(&r);
        uint32_t g = (uint32_t)(op >> 3) % NGROUPS;
        switch (op % 8) {
        case 0:
        case 1: {
            shr__rcmd c[NCMDS];
            shr__lcmd full[NCMDS];
            uint64_t key[2];
            bool encloses;
            size_t n = read_row(&r, c, key, &encloses);
            shr__rcmd *in = c;
            if (op & 1) { /* written in place */
                FUZZ_CHECK((in = shr__lyr_row_begin(sides[0].l, n)) != NULL);
                memcpy(in, c, n * sizeof(*c));
            }
            shr_status a = shr__lyr_row_commit(sides[0].l, g, 0, &sides[0].g.res, key, in, n);
            if (!encloses) {
                FUZZ_CHECK(a == SHR_E_INVALID_ARG);
                break;
            }
            unpack(c, n, &sides[1].g.res, key, full);
            FUZZ_CHECK(shr__lyr_group_set(sides[1].l, g, full, n) == a);
            break;
        }
        case 2: {
            uint32_t first = fr_u8(&r) % NGROUPS, last = first + fr_u8(&r) % (NGROUPS + 1);
            int32_t shift = fr_i8(&r) % 4, dy = fr_i8(&r) % 24;
            shr_rect area = {0, fr_u8(&r) % FUZZ_H, FUZZ_W, FUZZ_H};
            for (int k = 0; k < 2; k++) shr__lyr_groups_shift(sides[k].l, first, last, shift, area, dy);
            break;
        }
        case 3: render(); break;
        case 4: now += fr_u8(&r) * 10u; break;
        case 5: {
            shr_rect area = fr_rect(&r);
            for (int k = 0; k < 2; k++) shr__res_changed(&sides[k].g.res, area);
            break;
        }
        case 6: {
            size_t most = fr_u8(&r) % 40;
            for (int k = 0; k < 2; k++) FUZZ_CHECK(shr__lyr_groups_clear(sides[k].l, most) == SHR_OK);
            break;
        }
        default: {
            int32_t x = fr_i8(&r) % 16, y = fr_i8(&r) % 16;
            for (int k = 0; k < 2; k++) {
                FUZZ_CHECK(shr_lyr_set_rect(sides[k].l, (shr_rect){x, y, x + FUZZ_W, y + FUZZ_H}) == SHR_OK);
                FUZZ_CHECK(shr_lyr_set_visible(sides[k].l, !(op & 8)) == SHR_OK);
                if (op & 16) FUZZ_CHECK(shr__lyr_group_set(sides[k].l, g, NULL, 0) == SHR_OK);
            }
            break;
        }
        }
    }
    render();
    for (int k = 0; k < 2; k++) close_side(&sides[k]);
    return 0;
}
