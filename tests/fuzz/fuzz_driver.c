/* Software raster with arbitrary command batches, including CACHE_BEGIN/END groups and BOLD/ITALIC glyphs.
 * Invariants:
 *   - a batch is accepted exactly when the rules predict it (batches holding ROTATE are not predicted); a rejected
 *     batch writes nothing
 *   - an accepted batch writes only inside its destinations (group commands only inside cache_clip)
 *   - the driver with a row cache (hits, misses, evictions) draws exactly what the stateless path draws */
#include "fuzz_common.h"

#define SW 24
#define SH 20
#define SRC_BYTES 2048
#define MAX_CMDS 32

static uint8_t init_buf[SW * SH * 4], plain[SW * SH * 4], cached[SW * SH * 4];
static uint8_t src_buf[SRC_BYTES];
static uint8_t rot_buf[SW * SH * 4];

static bool inside(shr_rect r, int32_t w, int32_t h) {
    return r.x0 >= 0 && r.y0 >= 0 && r.x0 <= r.x1 && r.y0 <= r.y1 && r.x1 <= w && r.y1 <= h;
}

/* Keys identify group content exactly, as the contract requires: the commands relative to the group origin. */
#define MAX_KEYS 64
#define KEY_FIELDS 17
static int64_t keys[MAX_KEYS][MAX_CMDS * KEY_FIELDS];
static size_t key_len[MAX_KEYS], nkeys;

static uint64_t group_key(const shr_draw_cmd *c, size_t n) {
    int64_t v[MAX_CMDS * KEY_FIELDS];
    size_t len = 0;
    for (size_t i = 1; i < n; i++) {
        const shr_draw_cmd *k = &c[i];
        int64_t f[] = {k->kind, k->flags, k->dst.x0 - c->dst.x0, k->dst.y0 - c->dst.y0, k->dst.x1 - c->dst.x0,
                       k->dst.y1 - c->dst.y0, k->color, (const uint8_t *)k->src.pixels - src_buf, k->src.width,
                       k->src.height, (int64_t)k->src.stride, (int64_t)k->src.byte_length, k->src.format,
                       k->src_origin.x, k->src_origin.y, k->rotation, k->slant_axis};
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

/* Whole pixels of the italic shift of src row y, rounded down, and whether a fraction remains (shiroko_driver.h). */
static int32_t slant(int32_t axis, int32_t y, bool *frac) {
    int32_t t = SHR_GLYPH_SLANT / 2 * (axis - 2 * y - 1), k = t >= 0 ? t / 256 : -((255 - t) / 256);
    *frac = t != 256 * k;
    return k;
}

static void set_key(shr_draw_cmd *c, size_t n) {
    uint64_t k = group_key(c, n);
    c->key[0] = k, c->key[1] = ~k;
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
    FUZZ_CHECK(shr_software_driver_create(NULL, budgets[(cfg >> 1) & 3], &drv) == SHR_OK);
    nkeys = 0;
    for (size_t i = 0; i < sizeof(src_buf); i++) src_buf[i] = (uint8_t)(i * 13u + 1u);
    for (size_t i = 0; i < sizeof(rot_buf); i++) rot_buf[i] = (uint8_t)(i * 5u + 3u);
    uint8_t seed = fr_u8(&r);
    for (size_t i = 0; i < sizeof(init_buf); i++) init_buf[i] = (uint8_t)(i * 7u + seed);

    while (r.n > 0) {
        if (nkeys > MAX_KEYS - MAX_CMDS / 2) { /* a fresh cache before keys run out */
            FUZZ_CHECK(shr_software_driver_destroy(&drv) == SHR_OK);
            FUZZ_CHECK(shr_software_driver_create(NULL, budgets[(cfg >> 1) & 3], &drv) == SHR_OK);
            nkeys = 0;
        }
        shr_draw_cmd cmds[MAX_CMDS];
        size_t n = 0, group = SIZE_MAX;
        bool expect_ok = true, predicted = true;
        for (uint8_t count = 1 + fr_u8(&r) % MAX_CMDS; n < count && r.n > 0;) {
            shr_draw_cmd *c = &cmds[n];
            memset(c, 0, sizeof(*c));
            uint8_t kind = fr_u8(&r) % 8;
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
            static const shr_pixel_format fmts[] = {SHR_FORMAT_A4, SHR_FORMAT_A8, SHR_FORMAT_RGBA8888,
                                                    SHR_FORMAT_RGB565, SHR_FORMAT_RGBX8888};
            shr_pixel_format sf = fmts[fr_u8(&r) % 5];
            int32_t sw = fr_i8(&r), sh = fr_i8(&r);
            size_t sstride = fr_u8(&r), slen = fr_u16(&r) % (SRC_BYTES + 1); /* never beyond the real buffer */
            c->src = (shr_image){src_buf, sw, sh, sstride, slen, sf, SHR_MEMORY_CPU};
            c->src_origin = (shr_point){fr_i8(&r), fr_i8(&r)};
            if (c->kind == SHR_CMD_GLYPH) { /* past +-100 in steps of 40 across the +-4096 bound, then the extremes */
                int32_t a = fr_i8(&r);
                c->slant_axis = a == 127 ? INT32_MAX : a <= -127 ? INT32_MIN : a > 100 || a < -100 ? 40 * a : a;
            }
            if (c->kind == SHR_CMD_ROTATE) {
                c->rotation = (shr_rotation)(fr_u8(&r) % 4);
                bool quarter = c->rotation == SHR_ROTATE_90_CW || c->rotation == SHR_ROTATE_90_CCW;
                int32_t rw = quarter ? h : w, rh = quarter ? w : h;
                c->src = (shr_image){rot_buf, rw, rh, (size_t)rw * bpp, sizeof(rot_buf), dfmt, SHR_MEMORY_CPU};
            }
            shr_rect lim = group != SIZE_MAX ? cmds[group].dst : (shr_rect){0, 0, w, h};
            bool in = inside(c->dst, w, h) && c->dst.x0 >= lim.x0 && c->dst.y0 >= lim.y0 && c->dst.x1 <= lim.x1 &&
                      c->dst.y1 <= lim.y1;
            /* Columns a BOLD or ITALIC GLYPH may name (its sizes, i8, never exceed SHR_GLYPH_SYNTH_MAX). */
            uint32_t syn = c->kind == SHR_CMD_GLYPH ? c->flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC) : 0;
            bool italic = syn & SHR_GLYPH_ITALIC, frac = false, unused;
            bool axis_ok = !italic || (c->slant_axis >= -4 * SHR_GLYPH_SYNTH_MAX && c->slant_axis <= 4 * SHR_GLYPH_SYNTH_MAX);
            int64_t x0 = 0, x1 = sw;
            if (syn && axis_ok) {
                int32_t k0 = italic ? slant(c->slant_axis, 0, &frac) : 0;
                x0 = italic ? slant(c->slant_axis, sh - 1, &unused) : 0;
                x1 = sw + (syn & SHR_GLYPH_BOLD ? 1 : 0) + k0 + frac;
            }
            bool src_ok = shr_image_validate(&c->src) == SHR_OK && axis_ok && c->src_origin.x >= x0 && c->src_origin.y >= 0 &&
                          (int64_t)c->src_origin.x + (c->dst.x1 - c->dst.x0) <= x1 &&
                          (int64_t)c->src_origin.y + (c->dst.y1 - c->dst.y0) <= sh;
            bool coverage = sf == SHR_FORMAT_A4 || sf == SHR_FORMAT_A8,
                 screen = sf == SHR_FORMAT_RGB565 || sf == SHR_FORMAT_RGBX8888;
            if (c->kind == SHR_CMD_FILL) expect_ok &= in;
            else if (c->kind == SHR_CMD_GLYPH) expect_ok &= in && src_ok && coverage;
            else if (c->kind == SHR_CMD_IMAGE) expect_ok &= in && src_ok && sf == SHR_FORMAT_RGBA8888;
            else if (c->kind == SHR_CMD_COPY) expect_ok &= in && src_ok && screen;
            else if (c->kind == SHR_CMD_ROTATE) predicted = false;
            else expect_ok = false;
            n++;
        }
        expect_ok &= group == SIZE_MAX;
        if (group != SIZE_MAX) set_key(&cmds[group], n - group);

        shr_surface dst = {plain, w, h, stride, stride * (size_t)h, dfmt, 0, SHR_MEMORY_CPU, 0};
        memcpy(plain, init_buf, sizeof(plain));
        shr_status st = shr_software_execute(&dst, cmds, n);
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
                    else
                        hit = x >= cmds[i].dst.x0 && x < cmds[i].dst.x1 && y >= cmds[i].dst.y0 && y < cmds[i].dst.y1 &&
                              x >= clip.x0 && x < clip.x1 && y >= clip.y0 && y < clip.y1;
                }
                FUZZ_CHECK(hit);
            }
        }
        /* Twice through the caching driver: the second run may hit what the first stored. */
        for (int pass = 0; pass < 2; pass++) {
            memcpy(cached, init_buf, sizeof(cached));
            dst.pixels = cached;
            FUZZ_CHECK(drv.execute(drv.user, &dst, cmds, n, 1) == st);
            FUZZ_CHECK(memcmp(cached, plain, sizeof(cached)) == 0);
        }
    }
    FUZZ_CHECK(shr_software_driver_destroy(&drv) == SHR_OK);
    return 0;
}
