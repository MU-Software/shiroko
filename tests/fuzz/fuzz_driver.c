/* Software raster with arbitrary command batches, including buffer commands, CACHE_BEGIN/END groups and BOLD/ITALIC
 * glyphs drawn from buffer regions.
 * Invariants:
 *   - a batch is accepted exactly when the rules predict it (batches holding ROTATE are not predicted, but the driver
 *     accepts exactly what the stateless path accepts when its buffer commands are valid); a rejected batch writes
 *     nothing
 *   - an accepted batch writes only inside its destinations (group commands only inside cache_clip)
 *   - the driver with buffer registrations and a row cache (hits, misses, evictions) draws exactly what the stateless
 *     path draws from the registrations the harness keeps */
#include "fuzz_common.h"

#define SW 24
#define SH 20
#define SRC_BYTES 2048
#define MAX_CMDS 32
#define MAX_BUFFERS 6

static uint8_t init_buf[SW * SH * 4], plain[SW * SH * 4], cached[SW * SH * 4];
static uint8_t src_buf[SRC_BYTES];
static uint8_t rot_buf[SW * SH * 4];
/* The registrations as the driver should hold them: the stateless path's table. */
static shr_image model[MAX_BUFFERS], before[MAX_BUFFERS];

static bool inside(shr_rect r, int32_t w, int32_t h) {
    return r.x0 >= 0 && r.y0 >= 0 && r.x0 <= r.x1 && r.y0 <= r.y1 && r.x1 <= w && r.y1 <= h;
}

static bool registered(uint32_t id) { return id >= 1 && id <= MAX_BUFFERS && model[id - 1].format; }

/* Keys identify group content exactly, as the contract requires: the commands relative to the group origin, with the
 * memory GLYPH and IMAGE buffers name. */
#define MAX_KEYS 64
#define KEY_FIELDS 21
static int64_t keys[MAX_KEYS][MAX_CMDS * KEY_FIELDS];
static size_t key_len[MAX_KEYS], nkeys;

static uint64_t group_key(const shr_draw_cmd *c, size_t n) {
    int64_t v[MAX_CMDS * KEY_FIELDS];
    size_t len = 0;
    for (size_t i = 1; i < n; i++) {
        const shr_draw_cmd *k = &c[i];
        bool region = k->kind == SHR_CMD_GLYPH || k->kind == SHR_CMD_IMAGE;
        shr_image m = region ? (registered(k->buffer) ? model[k->buffer - 1] : (shr_image){0}) : k->src;
        int64_t f[] = {k->kind, k->flags, k->dst.x0 - c->dst.x0, k->dst.y0 - c->dst.y0, k->dst.x1 - c->dst.x0,
                       k->dst.y1 - c->dst.y0, k->color, m.pixels ? (const uint8_t *)m.pixels - src_buf : -1, m.width,
                       m.height, (int64_t)m.stride, (int64_t)m.byte_length, m.format, k->src_rect.x0, k->src_rect.y0,
                       k->src_rect.x1, k->src_rect.y1, k->src_origin.x, k->src_origin.y, k->rotation, k->slant_axis};
        memcpy(v + len, f, sizeof(f));
        len += KEY_FIELDS;
    }
    for (size_t i = 0; i < nkeys; i++)
        if (key_len[i] == len && !memcmp(keys[i], v, len * sizeof(v[0]))) return i + 1;
    FUZZ_CHECK(nkeys < MAX_KEYS);
    memcpy(keys[nkeys], v, len * sizeof(v[0]));
    key_len[nkeys] = len;
    return ++nkeys;
}

/* Whole pixels of the italic shift of rect row y, rounded down, and whether a fraction remains (shiroko_driver.h). */
static int32_t slant(int32_t axis, int32_t y, bool *frac) {
    int32_t t = SHR_GLYPH_SLANT / 2 * (axis - 2 * y - 1), k = t >= 0 ? t / 256 : -((255 - t) / 256);
    *frac = t != 256 * k;
    return k;
}

static void set_key(shr_draw_cmd *c, size_t n) {
    uint64_t k = group_key(c, n);
    c->key[0] = k, c->key[1] = ~k;
}

/* Brings the driver's registrations to `t`, as the compositor does after a failed batch. */
static void resync(shr_framebuffer_driver *drv, const shr_surface *dst, const shr_image *t) {
    shr_draw_cmd c[MAX_BUFFERS];
    for (uint32_t i = 0; i < MAX_BUFFERS; i++)
        c[i] = (shr_draw_cmd){.kind = t[i].format ? SHR_CMD_BUFFER_REGISTER : SHR_CMD_BUFFER_RELEASE, .buffer = i + 1,
                              .src = t[i]};
    FUZZ_CHECK(drv->execute(drv->user, dst, c, MAX_BUFFERS, 1) == SHR_OK);
}

/* Validates a leading buffer command the way the driver does and applies it to the model. */
static bool apply(const shr_draw_cmd *c) {
    uint32_t id = c->buffer;
    bool in_range = id >= 1 && id <= MAX_BUFFERS;
    if (c->kind == SHR_CMD_BUFFER_RELEASE) {
        if (in_range) model[id - 1] = (shr_image){0};
        return true;
    }
    if (c->kind == SHR_CMD_BUFFER_UPDATE)
        return registered(id) && inside(c->src_rect, model[id - 1].width, model[id - 1].height);
    shr_pixel_format f = c->src.format;
    bool ok = in_range && shr_image_validate(&c->src) == SHR_OK && c->src.domain != SHR_MEMORY_DEVICE &&
              (f == SHR_FORMAT_A4 || f == SHR_FORMAT_A8 || f == SHR_FORMAT_RGBA8888);
    if (ok) model[id - 1] = c->src;
    return ok;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_reader r = {data, size};
    uint8_t cfg = fr_u8(&r);
    shr_pixel_format dfmt = (cfg & 1) ? SHR_FORMAT_RGB565 : SHR_FORMAT_RGBX8888;
    size_t bpp = dfmt == SHR_FORMAT_RGB565 ? 2 : 4;
    int32_t w = 1 + fr_u8(&r) % SW, h = 1 + fr_u8(&r) % SH;
    size_t stride = (size_t)w * bpp + (fr_u8(&r) % 3) * bpp;
    if (stride * (size_t)h > sizeof(init_buf)) stride = (size_t)w * bpp;
    static const uint64_t budgets[4] = {0, 200, 2048, 1u << 20};
    shr_framebuffer_driver drv;
    FUZZ_CHECK(shr_software_driver_create(NULL, budgets[(cfg >> 1) & 3], MAX_BUFFERS, &drv) == SHR_OK);
    FUZZ_CHECK(drv.caps.max_buffers == MAX_BUFFERS);
    nkeys = 0;
    memset(model, 0, sizeof(model));
    for (size_t i = 0; i < sizeof(src_buf); i++) src_buf[i] = (uint8_t)(i * 13u + 1u);
    for (size_t i = 0; i < sizeof(rot_buf); i++) rot_buf[i] = (uint8_t)(i * 5u + 3u);
    uint8_t seed = fr_u8(&r);
    for (size_t i = 0; i < sizeof(init_buf); i++) init_buf[i] = (uint8_t)(i * 7u + seed);
    shr_surface dst = {plain, w, h, stride, stride * (size_t)h, dfmt, 0, SHR_MEMORY_CPU, 0};

    while (r.n > 0) {
        if (nkeys > MAX_KEYS - MAX_CMDS / 2) { /* a fresh cache before keys run out */
            FUZZ_CHECK(shr_software_driver_destroy(&drv) == SHR_OK);
            FUZZ_CHECK(shr_software_driver_create(NULL, budgets[(cfg >> 1) & 3], MAX_BUFFERS, &drv) == SHR_OK);
            nkeys = 0;
        }
        memcpy(before, model, sizeof(model));
        shr_draw_cmd cmds[MAX_CMDS];
        size_t n = 0, group = SIZE_MAX;
        bool expect_ok = true, predicted = true, prologue_ok = true, drawn = false;
        for (uint8_t count = 1 + fr_u8(&r) % MAX_CMDS; n < count && r.n > 0;) {
            shr_draw_cmd *c = &cmds[n];
            memset(c, 0, sizeof(*c));
            uint8_t kind = fr_u8(&r) % 11;
            if (kind >= 8) { /* REGISTER, UPDATE, RELEASE of ids 0..MAX_BUFFERS + 1 */
                c->kind = (shr_cmd_kind)(SHR_CMD_BUFFER_REGISTER + kind - 8);
                c->buffer = fr_u8(&r) % (MAX_BUFFERS + 2);
                if (c->kind == SHR_CMD_BUFFER_REGISTER) {
                    static const shr_pixel_format fmts[] = {SHR_FORMAT_A4, SHR_FORMAT_A8, SHR_FORMAT_RGBA8888,
                                                            SHR_FORMAT_RGB565, SHR_FORMAT_RGBX8888};
                    uint8_t fb = fr_u8(&r);
                    int32_t bw = fr_i8(&r), bh = fr_i8(&r);
                    size_t bstride = fr_u8(&r), off = (size_t)fr_u8(&r) * 8, len = fr_u16(&r) % (SRC_BYTES + 1);
                    if (len > SRC_BYTES - off) len = SRC_BYTES - off; /* never beyond the real buffer */
                    c->src = (shr_image){src_buf + off, bw, bh, bstride, len, fmts[(fb & 0x3F) % 5],
                                         fb & 0x80 ? SHR_MEMORY_DEVICE : fb & 0x40 ? SHR_MEMORY_DMA : SHR_MEMORY_CPU};
                } else if (c->kind == SHR_CMD_BUFFER_UPDATE) {
                    c->src_rect = fr_rect(&r);
                }
                if (drawn) expect_ok = false;
                else if (prologue_ok) prologue_ok = apply(c);
                n++;
                continue;
            }
            drawn = true;
            if (kind == 6) { /* a group that starts by covering its area, as the hint requires */
                if (n + 2 > count) break;
                shr_rect d = fr_rect(&r), clip = fr_rect(&r);
                c->kind = SHR_CMD_CACHE_BEGIN, c->dst = d, c->cache_clip = clip;
                c[1] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = d, .color = SHR_RGB(fr_u8(&r), 9, 99)};
                expect_ok &= group == SIZE_MAX && inside(d, w, h) && inside(clip, d.x1 - d.x0, d.y1 - d.y0);
                c->cache_clip = (shr_rect){clip.x0 + d.x0, clip.y0 + d.y0, clip.x1 + d.x0, clip.y1 + d.y0};
                group = n;
                n += 2;
                continue;
            }
            if (kind == 7) {
                c->kind = SHR_CMD_CACHE_END;
                expect_ok &= group != SIZE_MAX;
                if (group != SIZE_MAX) set_key(&cmds[group], n - group);
                group = SIZE_MAX;
                n++;
                continue;
            }
            c->kind = (shr_cmd_kind)kind; /* 0 is invalid on purpose */
            c->flags = fr_u8(&r) & 7;
            c->dst = fr_rect(&r);
            c->color = SHR_RGB(fr_u8(&r), fr_u8(&r), fr_u8(&r));
            shr_rect lim = group != SIZE_MAX ? cmds[group].dst : (shr_rect){0, 0, w, h};
            bool in = inside(c->dst, w, h) && c->dst.x0 >= lim.x0 && c->dst.y0 >= lim.y0 && c->dst.x1 <= lim.x1 &&
                      c->dst.y1 <= lim.y1;
            int64_t dw = c->dst.x1 - c->dst.x0, dh = c->dst.y1 - c->dst.y0;
            if (c->kind == SHR_CMD_GLYPH || c->kind == SHR_CMD_IMAGE) {
                c->buffer = fr_u8(&r) % (MAX_BUFFERS + 2);
                c->src_rect = fr_rect(&r);
                c->src_origin = (shr_point){fr_i8(&r), fr_i8(&r)};
                if (c->kind == SHR_CMD_GLYPH) { /* past +-100 in steps of 40 across the +-4096 bound, then the extremes */
                    int32_t a = fr_i8(&r);
                    c->slant_axis = a == 127 ? INT32_MAX : a <= -127 ? INT32_MIN : a > 100 || a < -100 ? 40 * a : a;
                }
                shr_image b = registered(c->buffer) ? model[c->buffer - 1] : (shr_image){0};
                shr_rect s = c->src_rect;
                int32_t rw = s.x1 - s.x0, rh = s.y1 - s.y0;
                bool fmt_ok = c->kind == SHR_CMD_IMAGE ? b.format == SHR_FORMAT_RGBA8888
                                                       : b.format == SHR_FORMAT_A4 || b.format == SHR_FORMAT_A8;
                bool rect_ok = inside(s, b.width, b.height) && (b.format != SHR_FORMAT_A4 || s.x0 % 2 == 0);
                /* Columns a BOLD or ITALIC GLYPH may take (rect sizes, at most 127, never exceed SHR_GLYPH_SYNTH_MAX). */
                uint32_t syn = c->kind == SHR_CMD_GLYPH ? c->flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC) : 0;
                bool italic = syn & SHR_GLYPH_ITALIC, frac = false, unused;
                bool axis_ok = !italic || (c->slant_axis >= -4 * SHR_GLYPH_SYNTH_MAX && c->slant_axis <= 4 * SHR_GLYPH_SYNTH_MAX);
                int64_t x0 = 0, x1 = rw;
                if (syn && axis_ok) {
                    int32_t k0 = italic ? slant(c->slant_axis, 0, &frac) : 0;
                    x0 = italic ? slant(c->slant_axis, rh - 1, &unused) : 0;
                    x1 = rw + (syn & SHR_GLYPH_BOLD ? 1 : 0) + k0 + frac;
                }
                expect_ok &= in && b.format && fmt_ok && rect_ok && axis_ok && c->src_origin.x >= x0 &&
                             c->src_origin.y >= 0 && c->src_origin.x + dw <= x1 && c->src_origin.y + dh <= rh;
                n++;
                continue;
            }
            static const shr_pixel_format fmts[] = {SHR_FORMAT_A4, SHR_FORMAT_A8, SHR_FORMAT_RGBA8888,
                                                    SHR_FORMAT_RGB565, SHR_FORMAT_RGBX8888};
            shr_pixel_format sf = fmts[fr_u8(&r) % 5];
            int32_t sw = fr_i8(&r), sh = fr_i8(&r);
            size_t sstride = fr_u8(&r), slen = fr_u16(&r) % (SRC_BYTES + 1); /* never beyond the real buffer */
            c->src = (shr_image){src_buf, sw, sh, sstride, slen, sf, SHR_MEMORY_CPU};
            c->src_origin = (shr_point){fr_i8(&r), fr_i8(&r)};
            if (c->kind == SHR_CMD_ROTATE) {
                c->rotation = (shr_rotation)(fr_u8(&r) % 4);
                bool quarter = c->rotation == SHR_ROTATE_90_CW || c->rotation == SHR_ROTATE_90_CCW;
                int32_t rw = quarter ? h : w, rh = quarter ? w : h;
                c->src = (shr_image){rot_buf, rw, rh, (size_t)rw * bpp, sizeof(rot_buf), dfmt, SHR_MEMORY_CPU};
            }
            bool src_ok = shr_image_validate(&c->src) == SHR_OK && c->src_origin.x >= 0 && c->src_origin.y >= 0 &&
                          c->src_origin.x + dw <= sw && c->src_origin.y + dh <= sh;
            bool screen = sf == SHR_FORMAT_RGB565 || sf == SHR_FORMAT_RGBX8888;
            if (c->kind == SHR_CMD_FILL) expect_ok &= in;
            else if (c->kind == SHR_CMD_COPY) expect_ok &= in && src_ok && screen;
            else if (c->kind == SHR_CMD_ROTATE) predicted = false;
            else expect_ok = false;
            n++;
        }
        expect_ok &= group == SIZE_MAX;
        if (group != SIZE_MAX) set_key(&cmds[group], n - group);

        dst.pixels = plain;
        memcpy(plain, init_buf, sizeof(plain));
        shr_status st = shr_software_execute(&dst, cmds, n, model, MAX_BUFFERS);
        if (predicted) FUZZ_CHECK((st == SHR_OK) == expect_ok);
        if (st != SHR_OK) {
            FUZZ_CHECK(memcmp(plain, init_buf, sizeof(plain)) == 0);
        } else {
            /* Nothing outside the written areas changed (stride padding and bytes past the surface included). */
            for (size_t off = 0; off < sizeof(plain); off++) {
                if (plain[off] == init_buf[off]) continue;
                FUZZ_CHECK(off < dst.byte_length && off % stride < (size_t)w * bpp);
                int32_t x = (int32_t)(off % stride / bpp), y = (int32_t)(off / stride);
                bool hit = false;
                shr_rect clip = {0, 0, w, h};
                for (size_t i = 0; i < n && !hit; i++) {
                    if (cmds[i].kind == SHR_CMD_CACHE_BEGIN) clip = cmds[i].cache_clip;
                    else if (cmds[i].kind == SHR_CMD_CACHE_END) clip = (shr_rect){0, 0, w, h};
                    else if (cmds[i].kind < SHR_CMD_BUFFER_REGISTER)
                        hit = x >= cmds[i].dst.x0 && x < cmds[i].dst.x1 && y >= cmds[i].dst.y0 && y < cmds[i].dst.y1 &&
                              x >= clip.x0 && x < clip.x1 && y >= clip.y0 && y < clip.y1;
                }
                FUZZ_CHECK(hit);
            }
        }
        /* Twice through the caching driver from the registrations before the batch: the second run may hit what the
         * first stored. */
        for (int pass = 0; pass < 2; pass++) {
            memcpy(cached, init_buf, sizeof(cached));
            dst.pixels = cached;
            resync(&drv, &dst, before);
            shr_status got = drv.execute(drv.user, &dst, cmds, n, 1);
            FUZZ_CHECK((got == SHR_OK) == (prologue_ok && st == SHR_OK));
            FUZZ_CHECK(memcmp(cached, got == SHR_OK ? plain : init_buf, sizeof(cached)) == 0);
        }
    }
    FUZZ_CHECK(shr_software_driver_destroy(&drv) == SHR_OK);
    return 0;
}
