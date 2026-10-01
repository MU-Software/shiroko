#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <shiroko/port_software.h>

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "raster.h"
#include "shr_glyph.h"

static const shr_pixel_format FMTS[2] = {SHR_FORMAT_RGB565, SHR_FORMAT_RGBX8888};

static size_t bpp(shr_pixel_format f) { return f == SHR_FORMAT_RGB565 ? 2 : 4; }

static shr_surface surf(void *buf, shr_pixel_format f, int32_t w, int32_t h, size_t stride) {
    return (shr_surface){buf, w, h, stride, stride * (size_t)h, f, 1, SHR_MEMORY_CPU, 0};
}

static shr_surface packed(void *buf, shr_pixel_format f, int32_t w, int32_t h) {
    return surf(buf, f, w, h, (size_t)w * bpp(f));
}

static shr_image as_image(const shr_surface *s) {
    return (shr_image){s->pixels, s->width, s->height, s->stride, s->byte_length, s->format, s->domain};
}

/* Raw pixel: the u16 of RGB565, or the bytes R, G, B, X of RGBX8888 as 0xRRGGBBXX. */
static uint32_t raw(const shr_surface *s, int32_t x, int32_t y) {
    const uint8_t *p = (const uint8_t *)s->pixels + (size_t)y * s->stride + (size_t)x * bpp(s->format);
    if (s->format == SHR_FORMAT_RGB565) {
        uint16_t v;
        memcpy(&v, p, 2);
        return v;
    }
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put(const shr_surface *s, int32_t x, int32_t y, uint32_t v) {
    uint8_t *p = (uint8_t *)s->pixels + (size_t)y * s->stride + (size_t)x * bpp(s->format);
    if (s->format == SHR_FORMAT_RGB565) {
        uint16_t u = (uint16_t)v;
        memcpy(p, &u, 2);
    } else {
        p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
    }
}

/* Reference conversions: quantise (c * m + 127) / 255, expand (v * 255 + m / 2) / m, blend rounded. */
static uint32_t q(uint32_t c, uint32_t m) { return (c * m + 127) / 255; }
static uint32_t ex(uint32_t v, uint32_t m) { return (v * 255 + m / 2) / m; }
static uint32_t blend8(uint32_t fg, uint32_t bg, uint32_t a) { return (fg * a + bg * (255 - a) + 127) / 255; }

static uint32_t enc(shr_pixel_format f, uint32_t rgb) {
    uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    return f == SHR_FORMAT_RGB565 ? q(r, 31) << 11 | q(g, 63) << 5 | q(b, 31) : r << 24 | g << 16 | b << 8 | 255;
}

static uint32_t dec(shr_pixel_format f, uint32_t v) {
    if (f == SHR_FORMAT_RGB565) return ex(v >> 11 & 31, 31) << 16 | ex(v >> 5 & 63, 63) << 8 | ex(v & 31, 31);
    return v >> 8;
}

/* RGB565 blends round once: fg * a + d * (1 - a) on unorm values, d the raw 5/6-bit level. */
static uint32_t mix565(uint32_t fg, uint32_t d, uint32_t a, uint32_t m) {
    return (uint32_t)(m * (fg / 255.0) * (a / 255.0) + d * (1 - a / 255.0) + 0.5);
}

static uint32_t mix(shr_pixel_format f, uint32_t fg, uint32_t bg_raw, uint32_t a) {
    if (f == SHR_FORMAT_RGB565)
        return mix565(fg >> 16 & 255, bg_raw >> 11 & 31, a, 31) << 11 | mix565(fg >> 8 & 255, bg_raw >> 5 & 63, a, 63) << 5 |
               mix565(fg & 255, bg_raw & 31, a, 31);
    uint32_t bg = dec(f, bg_raw);
    return enc(f, blend8(fg >> 16 & 255, bg >> 16 & 255, a) << 16 | blend8(fg >> 8 & 255, bg >> 8 & 255, a) << 8 |
                      blend8(fg & 255, bg & 255, a));
}

static shr_draw_cmd cmd(shr_cmd_kind kind, shr_rect dst) {
    shr_draw_cmd c;
    memset(&c, 0, sizeof(c));
    c.kind = kind;
    c.dst = dst;
    return c;
}

static shr_draw_cmd fill(shr_rect r, shr_color color) {
    shr_draw_cmd c = cmd(SHR_CMD_FILL, r);
    c.color = color;
    return c;
}

/* The stateless path's buffer table: GLYPH and IMAGE sources go into the next slot, ids rotating through 1..NBUF. */
#define NBUF 8
static shr_image bufs[NBUF];
static uint32_t last_id;

static uint32_t use(shr_image m) {
    last_id = last_id % NBUF + 1;
    bufs[last_id - 1] = m;
    return last_id;
}

static shr_status exec(const shr_surface *dst, const shr_draw_cmd *c, size_t n) {
    return shr_software_execute(dst, c, n, bufs, NBUF);
}

/* Buffer command `kind` naming `id`; REGISTER hands the driver what the stateless table holds for it. */
static shr_draw_cmd buf_cmd(shr_cmd_kind kind, uint32_t id) {
    shr_draw_cmd c = cmd(kind, (shr_rect){0, 0, 0, 0});
    c.buffer = id;
    if (kind == SHR_CMD_BUFFER_REGISTER) c.src = bufs[id - 1];
    return c;
}

static shr_draw_cmd reg(uint32_t id) { return buf_cmd(SHR_CMD_BUFFER_REGISTER, id); }

/* GLYPH and IMAGE draw all of `src` as a buffer of its own; COPY and ROTATE take it as their surface. */
static shr_draw_cmd with_src(shr_cmd_kind kind, shr_rect r, shr_image src, shr_point origin) {
    shr_draw_cmd c = cmd(kind, r);
    if (kind == SHR_CMD_GLYPH || kind == SHR_CMD_IMAGE)
        c.buffer = use(src), c.src_rect = (shr_rect){0, 0, src.width, src.height};
    else
        c.src = src;
    c.src_origin = origin;
    return c;
}

/* `rect` of buffer `buf`. */
static shr_draw_cmd region(shr_cmd_kind kind, shr_rect r, shr_image buf, shr_rect rect, shr_point origin) {
    shr_draw_cmd c = with_src(kind, r, buf, origin);
    c.src_rect = rect, c.color = SHR_RGB(255, 255, 255);
    return c;
}

static shr_draw_cmd glyph(shr_rect r, shr_color color, shr_image src, shr_point origin) {
    shr_draw_cmd c = with_src(SHR_CMD_GLYPH, r, src, origin);
    c.color = color;
    return c;
}

static shr_draw_cmd rotate(const shr_surface *src, shr_rect r, shr_rotation rot) {
    shr_draw_cmd c = with_src(SHR_CMD_ROTATE, r, as_image(src), (shr_point){0, 0});
    c.rotation = rot;
    return c;
}

static shr_draw_cmd begin(uint64_t key, shr_rect dst, shr_rect clip) {
    shr_draw_cmd c = cmd(SHR_CMD_CACHE_BEGIN, dst);
    c.key[0] = key, c.key[1] = ~key;
    c.cache_clip = clip;
    return c;
}

static shr_draw_cmd end(void) { return cmd(SHR_CMD_CACHE_END, (shr_rect){0, 0, 0, 0}); }

TEST encoding_anchors(void) {
    ASSERT_EQ_LL(enc(SHR_FORMAT_RGB565, 0xFF00FF), 0xF81F);
    ASSERT_EQ_LL(enc(SHR_FORMAT_RGB565, 0xFF8040), 0xFC08);
    ASSERT_EQ_LL(dec(SHR_FORMAT_RGB565, 0xFC08), 0xFF8242);
    ASSERT_EQ_LL(enc(SHR_FORMAT_RGBX8888, 0x0A141E), 0x0A141EFF);
    PASS();
}

TEST fill_is_opaque_and_respects_stride(void) {
    for (int i = 0; i < 2; i++) {
        uint8_t buf[16 * 3];
        memset(buf, 0xAB, sizeof(buf));
        shr_surface s = surf(buf, FMTS[i], 2, 3, 16); /* rows padded */
        shr_draw_cmd c = fill((shr_rect){0, 0, 2, 3}, 0x00FF8040); /* alpha byte ignored */
        ASSERT_EQ_LL(exec(&s, &c, 1), SHR_OK);
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 2; x++) ASSERT_EQ_LL(raw(&s, x, y), enc(FMTS[i], 0xFF8040));
        for (int y = 0; y < 3; y++)
            for (size_t k = 2 * bpp(FMTS[i]); k < 16; k++) ASSERT_EQ_LL(buf[16 * y + k], 0xAB);
    }
    PASS();
}

TEST fill_dim_blends_at_half_strength(void) {
    for (int i = 0; i < 2; i++) {
        uint8_t buf[4 * 4];
        shr_surface s = packed(buf, FMTS[i], 4, 1);
        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, 4, 1}, SHR_RGB(20, 200, 90)), fill((shr_rect){1, 0, 3, 1}, SHR_RGB(255, 0, 255))};
        c[1].flags = SHR_GLYPH_DIM;
        ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
        uint32_t bg = enc(FMTS[i], 0x14C85A);
        ASSERT_EQ_LL(raw(&s, 0, 0), bg);
        ASSERT_EQ_LL(raw(&s, 1, 0), mix(FMTS[i], 0xFF00FF, bg, 128));
        ASSERT_EQ_LL(raw(&s, 2, 0), mix(FMTS[i], 0xFF00FF, bg, 128));
        ASSERT_EQ_LL(raw(&s, 3, 0), bg);
    }
    uint8_t px[4] = {0};
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 1, 1);
    shr_draw_cmd c = fill((shr_rect){0, 0, 1, 1}, SHR_RGB(255, 255, 255));
    c.flags = SHR_GLYPH_DIM;
    ASSERT_EQ_LL(exec(&s, &c, 1), SHR_OK);
    ASSERT_EQ_LL(raw(&s, 0, 0), 0x808080FF);
    PASS();
}

TEST glyph_a8_blends_every_coverage(void) {
    uint8_t mask[256];
    for (int a = 0; a < 256; a++) mask[a] = (uint8_t)a;
    shr_image m = {mask, 256, 1, 256, 256, SHR_FORMAT_A8, SHR_MEMORY_CPU};
    for (int i = 0; i < 2; i++) {
        uint8_t buf[256 * 4];
        shr_surface s = packed(buf, FMTS[i], 256, 1);
        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, 256, 1}, SHR_RGB(10, 60, 250)),
                             glyph((shr_rect){0, 0, 256, 1}, SHR_RGB(255, 128, 7), m, (shr_point){0, 0})};
        ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
        uint32_t bg = enc(FMTS[i], 0x0A3CFA);
        ASSERT_EQ_LL(raw(&s, 0, 0), bg); /* zero coverage leaves the pixel */
        for (uint32_t a = 1; a < 256; a++) ASSERT_EQ_LL(raw(&s, (int32_t)a, 0), mix(FMTS[i], 0xFF8007, bg, a));
        ASSERT_EQ_LL(raw(&s, 255, 0), enc(FMTS[i], 0xFF8007));
    }
    PASS();
}

TEST rgb565_blends_round_once_for_every_input(void) {
    uint8_t cov[255];
    for (int a = 0; a < 255; a++) cov[a] = (uint8_t)(a + 1);
    uint16_t px[255];
    shr_surface s = packed(px, SHR_FORMAT_RGB565, 255, 1);
    shr_draw_cmd c = with_src(SHR_CMD_GLYPH, (shr_rect){0, 0, 255, 1},
                              (shr_image){cov, 255, 1, 255, 255, SHR_FORMAT_A8, SHR_MEMORY_CPU}, (shr_point){0, 0});
    for (uint32_t d = 0; d < 64; d++)
        for (uint32_t fg = 0; fg < 256; fg++) {
            uint32_t bg = (d & 31) << 11 | d << 5 | (31 - (d & 31));
            for (int x = 0; x < 255; x++) px[x] = (uint16_t)bg;
            c.color = SHR_RGB(fg, 255 - fg, fg);
            ASSERT_EQ_LL(exec(&s, &c, 1), SHR_OK);
            for (uint32_t a = 1; a < 256; a++) ASSERT_EQ_LL(px[a - 1], mix(SHR_FORMAT_RGB565, c.color, bg, a));
        }
    PASS();
}

TEST glyph_a4_nibbles_origin_and_dim(void) {
    /* Two rows of 6 pixels, stride 3: high nibble = left pixel. The rect starts at column 2 and is read from its
     * (1, 1): buffer columns 3 (a low nibble) and 4 (a high one) of row 1. */
    const uint8_t a4[6] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEF, 0x8E};
    shr_image m = {a4, 6, 2, 3, 6, SHR_FORMAT_A4, SHR_MEMORY_CPU};
    for (int i = 0; i < 2; i++) {
        uint8_t buf[3 * 4];
        shr_surface s = packed(buf, FMTS[i], 3, 1);
        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, 3, 1}, SHR_RGB(0, 0, 0)),
                             region(SHR_CMD_GLYPH, (shr_rect){1, 0, 3, 1}, m, (shr_rect){2, 0, 5, 2}, (shr_point){1, 1})};
        ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
        ASSERT_EQ_LL(raw(&s, 0, 0), enc(FMTS[i], 0));
        ASSERT_EQ_LL(raw(&s, 1, 0), enc(FMTS[i], 0xFFFFFF));             /* 15 -> 255 */
        ASSERT_EQ_LL(raw(&s, 2, 0), mix(FMTS[i], 0xFFFFFF, enc(FMTS[i], 0), 8 * 17));
        c[1].flags = SHR_GLYPH_DIM; /* coverage (a + 1) / 2 after expansion, not nibble halving */
        ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
        ASSERT_EQ_LL(raw(&s, 1, 0), mix(FMTS[i], 0xFFFFFF, enc(FMTS[i], 0), 128));
        ASSERT_EQ_LL(raw(&s, 2, 0), mix(FMTS[i], 0xFFFFFF, enc(FMTS[i], 0), (136 + 1) / 2));
    }
    PASS();
}

TEST glyph_a8_dim_keeps_faint_coverage(void) {
    const uint8_t a8[3] = {0, 1, 255};
    shr_image m = {a8, 3, 1, 3, 3, SHR_FORMAT_A8, SHR_MEMORY_CPU};
    for (int i = 0; i < 2; i++) {
        uint8_t buf[3 * 4];
        shr_surface s = packed(buf, FMTS[i], 3, 1);
        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, 3, 1}, SHR_RGB(0, 0, 0)),
                             glyph((shr_rect){0, 0, 3, 1}, SHR_RGB(255, 255, 255), m, (shr_point){0, 0})};
        c[1].flags = SHR_GLYPH_DIM;
        ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
        uint32_t black = enc(FMTS[i], 0);
        ASSERT_EQ_LL(raw(&s, 0, 0), black);
        ASSERT_EQ_LL(raw(&s, 1, 0), mix(FMTS[i], 0xFFFFFF, black, 1));
        ASSERT_EQ_LL(raw(&s, 2, 0), mix(FMTS[i], 0xFFFFFF, black, 128));
    }
    PASS();
}

/* ---- BOLD and ITALIC synthesis ---- */

static uint32_t rng = 0x9E3779B9u;
static uint32_t rnd(void) { return rng ^= (rng & 0x7FFFFu) << 13, rng ^= rng >> 17, rng ^= (rng & 0x7FFFFFFu) << 5; }

/* Coverage bytes with long runs of 0 and 255, so the BOLD gap rule meets every case. */
static void coverage_noise(uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t v = rnd();
        p[i] = (uint8_t)(v % 4 == 0 ? 0 : v % 4 == 1 ? 0xFF : v >> 8);
    }
}

/* The shiroko_driver.h formula, written independently: the BOLD row first, then the shear. */
static uint32_t ref_in(const shr_image *m, int32_t x, int32_t y) {
    if (x < 0 || x >= m->width || y < 0 || y >= m->height) return 0;
    const uint8_t *row = (const uint8_t *)m->pixels + (size_t)y * m->stride;
    return m->format == SHR_FORMAT_A8 ? row[x] : 17u * (x % 2 ? row[x / 2] & 15u : row[x / 2] >> 4u);
}

/* Coverage of columns [xa, xa + n) of src row y; n <= 64. */
static void ref_row(const shr_image *m, uint32_t flags, int32_t axis, int32_t y, int32_t xa, int32_t n, uint32_t *out) {
    int32_t k = 0, f = 0;
    if (flags & SHR_GLYPH_ITALIC) {
        int32_t t = SHR_GLYPH_SLANT / 2 * (axis - 2 * y - 1);
        k = t >= 0 ? t / 256 : -((255 - t) / 256);
        f = t - 256 * k;
    }
    uint32_t b[65]; /* b[i] = b(xa - k - 1 + i) */
    for (int32_t i = 0; i <= n; i++) {
        int32_t x = xa - k - 1 + i;
        uint32_t l = ref_in(m, x - 1, y), c = ref_in(m, x, y), r = ref_in(m, x + 1, y);
        b[i] = !(flags & SHR_GLYPH_BOLD) ? c : r > c ? c : c > l ? c : l;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t c = (b[i + 1] * (uint32_t)(256 - f) + b[i] * (uint32_t)f + 128) >> 8;
        out[i] = flags & SHR_GLYPH_DIM ? (c + 1) >> 1 : c;
    }
}

/* Copies the w x h coverage `m` to column 2, row 1 of a buffer two columns and a row larger on every side whose
 * other pixels are fully covered (for A4 also the column after an odd `m`, from its padding nibble); *rect is where
 * `m` landed. `out` holds (w + 4) x (h + 2) pixels. */
static shr_image embed(const shr_image *m, uint8_t *out, shr_rect *rect) {
    int32_t bw = m->width + 4, bh = m->height + 2;
    bool a4 = m->format == SHR_FORMAT_A4;
    size_t stride = a4 ? (size_t)(bw + 1) / 2 : (size_t)bw, row = a4 ? (size_t)(m->width + 1) / 2 : (size_t)m->width;
    memset(out, 0xFF, stride * (size_t)bh);
    for (int32_t y = 0; y < m->height; y++)
        memcpy(out + (size_t)(y + 1) * stride + (a4 ? 1 : 2), (const uint8_t *)m->pixels + (size_t)y * m->stride, row);
    *rect = (shr_rect){2, 1, 2 + m->width, 1 + m->height};
    return (shr_image){out, bw, bh, stride, stride * (size_t)bh, m->format, 0};
}

TEST synth_hand_vectors(void) {
    int32_t k, f;
    shr__slant(11, 0, &k, &f);
    ASSERT_EQ_LL(k, 1);
    ASSERT_EQ_LL(f, 14);
    static const uint8_t gap[5] = {0, 255, 0, 255, 0}, dot[2] = {255, 255};
    static const uint8_t a4[2] = {0xFF, 0x0F}; /* 255 255 0, pad 15 */
    const uint32_t B = SHR_GLYPH_BOLD, I = SHR_GLYPH_ITALIC, D = SHR_GLYPH_DIM;
    const struct {
        shr_image src;
        uint32_t flags;
        int32_t axis, x0, n;
        uint8_t want[2][6];
    } v[] = {
        {{gap, 5, 1, 5, 5, SHR_FORMAT_A8, 0}, B, 0, 0, 6, {{0, 255, 0, 255, 255, 0}}},
        {{dot, 1, 1, 1, 1, SHR_FORMAT_A8, 0}, I, 11, 1, 2, {{241, 14}}},
        {{dot, 1, 1, 1, 1, SHR_FORMAT_A8, 0}, I | D, 11, 1, 2, {{121, 7}}},
        {{dot, 1, 1, 1, 1, SHR_FORMAT_A8, 0}, B | I, 11, 1, 3, {{241, 255, 14}}},
        {{a4, 3, 1, 2, 2, SHR_FORMAT_A4, 0}, B, 0, 0, 4, {{255, 255, 255, 0}}},
        /* Rows 0 and 1 shift by 1 + 14/256 and 216/256: the footprint takes k(1) on the left, k(0) on the right. */
        {{dot, 1, 2, 1, 2, SHR_FORMAT_A8, 0}, I, 11, 0, 3, {{0, 241, 14}, {40, 215, 0}}},
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        int32_t x0, x1, h = v[i].src.height;
        shr__glyph_footprint(v[i].src.width, h, v[i].flags, v[i].axis, &x0, &x1);
        ASSERT_EQ_LL(x0, v[i].x0);
        ASSERT_EQ_LL(x1, v[i].x0 + v[i].n);
        uint8_t buf[2 * 6 * 4], eb[64];
        shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, v[i].n, h);
        shr_rect rect;
        shr_image b = embed(&v[i].src, eb, &rect);
        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, v[i].n, h}, 0),
                             region(SHR_CMD_GLYPH, (shr_rect){0, 0, v[i].n, h}, b, rect, (shr_point){x0, 0})};
        c[1].flags = v[i].flags, c[1].slant_axis = v[i].axis;
        ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < v[i].n; x++) ASSERT_EQ_LL(buf[4 * (y * v[i].n + x)], v[i].want[y][x]);
    }
    PASS();
}

TEST synth_matches_the_spec_reference(void) {
    static const int32_t widths[] = {1, 2, 5, 7, 8}, heights[] = {1, 4, 9};
    static const shr_pixel_format srcs[2] = {SHR_FORMAT_A8, SHR_FORMAT_A4};
    uint8_t px[9 * 10], ink[sizeof(px)], buf[67 * 9 * 4], eb[12 * 11];
    uint32_t want[64];
    memset(ink, 255, sizeof(ink));
    for (int sf = 0; sf < 2; sf++)
        for (int wi = 0; wi < 5; wi++)
            for (int hi = 0; hi < 3; hi++) {
                int32_t w = widths[wi], h = heights[hi];
                size_t stride = (sf ? (size_t)(w + 1) / 2 : (size_t)w) + rnd() % 3;
                coverage_noise(px, sizeof(px)); /* the A4 padding nibble too */
                shr_image m = {px, w, h, stride, stride * (size_t)h, srcs[sf], 0}, solid = m;
                solid.pixels = ink;
                shr_rect rect; /* the driver reads `m` inside a fully covered buffer */
                shr_image b = embed(&m, eb, &rect);
                const int32_t axes[] = {-40, -1, 0, 1, h, 2 * h - 1, 2 * h, 2 * h + 1, 3 * h + 37};
                for (uint32_t flags = 0; flags < 8; flags++)
                    for (int ai = 0; ai < 9; ai++) {
                        int32_t axis = axes[ai], x0, x1;
                        shr__glyph_footprint(w, h, flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC), axis, &x0, &x1);
                        int32_t fw = x1 - x0;
                        ASSERT(fw + 4 <= 64);
                        /* Nothing is drawn outside the footprint, and a fully inked source fills it to both ends. */
                        int32_t left = INT32_MAX, right = INT32_MIN;
                        for (int32_t y = 0; y < h; y++) {
                            uint32_t edge[2];
                            ref_row(&m, flags, axis, y, x0 - 1, 1, edge), ref_row(&m, flags, axis, y, x1, 1, edge + 1);
                            ASSERT_EQ_LL(edge[0] | edge[1], 0);
                            ref_row(&solid, flags, axis, y, x0 - 2, fw + 4, want);
                            for (int32_t x = x0 - 2; x < x1 + 2; x++)
                                if (want[x - x0 + 2]) left = x < left ? x : left, right = x > right ? x : right;
                        }
                        ASSERT_EQ_LL(left, x0);
                        ASSERT_EQ_LL(right, x1 - 1);
                        for (int fi = 0; fi < 2; fi++) {
                            shr_surface s = packed(buf, FMTS[fi], fw + 3, h);
                            shr_draw_cmd c[2] = {fill((shr_rect){0, 0, fw + 3, h}, 0),
                                                 region(SHR_CMD_GLYPH, (shr_rect){3, 0, fw + 3, h}, b, rect,
                                                        (shr_point){x0, 0})};
                            c[1].flags = flags, c[1].slant_axis = axis;
                            ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
                            for (int32_t y = 0; y < h; y++) {
                                ref_row(&m, flags, axis, y, x0, fw, want);
                                for (int32_t x = 0; x < fw; x++)
                                    ASSERT_EQ_LL(raw(&s, x + 3, y), mix(FMTS[fi], 0xFFFFFF, enc(FMTS[fi], 0), want[x]));
                            }
                        }
                    }
            }
    PASS();
}

TEST synth_is_clip_invariant(void) {
    enum { W = 7, H = 6, SW = 16, SH = 9 };
    uint8_t px[5 * H], eb[6 * (H + 2)];
    coverage_noise(px, sizeof(px));
    shr_image m = {px, W, H, 5, sizeof(px), SHR_FORMAT_A4, 0};
    shr_rect rect;
    shr_image b = embed(&m, eb, &rect);
    const uint32_t flags = SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC | SHR_GLYPH_DIM;
    int32_t x0, x1;
    shr__glyph_footprint(W, H, flags, 5, &x0, &x1);
    shr_rect d = {2, 1, 2 + x1 - x0, 1 + H};
    ASSERT(d.x1 <= SW);
    shr_draw_cmd whole = region(SHR_CMD_GLYPH, d, b, rect, (shr_point){x0, 0});
    whole.color = SHR_RGB(250, 200, 30);
    whole.flags = flags, whole.slant_axis = 5;
    for (int fi = 0; fi < 2; fi++) {
        uint8_t init[SW * SH * 4], ref[sizeof(init)], out[sizeof(init)];
        coverage_noise(init, sizeof(init));
        memcpy(ref, init, sizeof(init)), memcpy(out, init, sizeof(init));
        shr_surface rs = packed(ref, FMTS[fi], SW, SH), os = packed(out, FMTS[fi], SW, SH);
        ASSERT_EQ_LL(exec(&rs, &whole, 1), SHR_OK);
        /* The same glyph in 3 x 2 tiles, each with its own src_origin. */
        shr_draw_cmd tiles[16];
        size_t n = 0;
        for (int32_t y = d.y0; y < d.y1; y += 2)
            for (int32_t x = d.x0; x < d.x1; x += 3) {
                shr_rect t = {x, y, x + 3 < d.x1 ? x + 3 : d.x1, y + 2};
                tiles[n] = whole;
                tiles[n].dst = t, tiles[n++].src_origin = (shr_point){x0 + x - d.x0, y - d.y0};
            }
        ASSERT_EQ_LL(exec(&os, tiles, n), SHR_OK);
        ASSERT_MEM_EQ(ref, out, sizeof(ref));
        /* Moved by an origin and limited to a clip, as a cached group is drawn. */
        uint8_t win[5 * 4 * 4];
        shr_surface ws = packed(win, FMTS[fi], 5, 4);
        for (int32_t y = 0; y < 4; y++) memcpy(win + (size_t)y * ws.stride, init + (size_t)(y + 2) * rs.stride + 3 * bpp(FMTS[fi]), ws.stride);
        shr__raster_draw(&ws, &whole, bufs, (shr_point){3, 2}, (shr_rect){0, 0, 5, 4});
        for (int32_t y = 0; y < 4; y++)
            for (int32_t x = 0; x < 5; x++) ASSERT_EQ_LL(raw(&ws, x, y), raw(&rs, x + 3, y + 2));
        /* Through the caching driver, missing and then hitting. */
        shr_draw_cmd g[5] = {reg(whole.buffer), begin(77, (shr_rect){0, 0, SW, SH}, (shr_rect){3, 2, 9, 6}),
                             fill((shr_rect){0, 0, SW, SH}, SHR_RGB(9, 40, 90)), whole, end()};
        memcpy(ref, init, sizeof(init));
        ASSERT_EQ_LL(exec(&rs, g, 5), SHR_OK);
        shr_framebuffer_driver drv;
        ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 16, NBUF, &drv), SHR_OK);
        for (int pass = 0; pass < 2; pass++) {
            memcpy(out, init, sizeof(init));
            ASSERT_EQ_LL(drv.execute(drv.user, &os, g, 5, 1), SHR_OK);
            ASSERT_MEM_EQ(ref, out, sizeof(ref));
        }
        ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    }
    PASS();
}

TEST synth_sources_stay_in_the_footprint(void) {
    uint8_t buf[8 * 2 * 4], px[4 * 2], eb[8 * 4];
    coverage_noise(px, sizeof(px));
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, 8, 2);
    shr_image m = {px, 4, 2, 4, sizeof(px), SHR_FORMAT_A8, 0};
    shr_rect rect; /* the footprint bounds the origin, not the larger buffer */
    shr_image b = embed(&m, eb, &rect);
    static const uint32_t styles[3] = {SHR_GLYPH_BOLD, SHR_GLYPH_ITALIC, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC};
    static const int32_t axes[3] = {-9, 3, 20};
    for (int i = 0; i < 3; i++)
        for (int a = 0; a < 3; a++) {
            int32_t x0, x1;
            shr__glyph_footprint(4, 2, styles[i], axes[a], &x0, &x1);
            const struct {
                int32_t ox, oy, w, h;
                shr_status want;
            } e[] = {
                {x0, 0, x1 - x0, 2, SHR_OK},         {x0 - 1, 0, x1 - x0, 2, SHR_E_INVALID_ARG},
                {x0, 0, x1 - x0 + 1, 2, SHR_E_INVALID_ARG}, {x0, -1, x1 - x0, 1, SHR_E_INVALID_ARG},
                {x0, 1, x1 - x0, 2, SHR_E_INVALID_ARG}, {x1, 0, 0, 2, SHR_OK},
            };
            for (size_t k = 0; k < sizeof(e) / sizeof(e[0]); k++) {
                shr_draw_cmd c[2] = {fill((shr_rect){0, 0, 8, 2}, 0),
                                     region(SHR_CMD_GLYPH, (shr_rect){0, 0, e[k].w, e[k].h}, b, rect,
                                            (shr_point){e[k].ox, e[k].oy})};
                c[1].flags = styles[i], c[1].slant_axis = axes[a];
                ASSERT_EQ_LL(exec(&s, c, 2), e[k].want);
            }
        }
    PASS();
}

/* Rect sizes and axis bounds, checked before the footprint is computed; BOLD alone ignores the axis. The buffer
 * is larger than every rect. */
TEST synth_size_and_axis_limits(void) {
    static uint8_t big[1025];
    uint8_t buf[8 * 2 * 4] = {0}, other[sizeof(buf)] = {0}, px[4 * 2];
    coverage_noise(px, sizeof(px));
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, 8, 2), os = packed(other, SHR_FORMAT_RGBX8888, 8, 2);
    const struct {
        int32_t w, h;
        uint32_t flags;
        int32_t axis;
        shr_status want;
    } lim[] = {
        {1024, 1, SHR_GLYPH_BOLD, 0, SHR_OK},
        {1025, 1, SHR_GLYPH_BOLD, 0, SHR_E_INVALID_ARG},
        {1, 1024, SHR_GLYPH_BOLD, 0, SHR_OK},
        {1, 1025, SHR_GLYPH_BOLD, 0, SHR_E_INVALID_ARG},
        {1, 1025, SHR_GLYPH_DIM, 0, SHR_OK},
        {1, 1, SHR_GLYPH_ITALIC, 4096, SHR_OK},
        {1, 1, SHR_GLYPH_ITALIC, -4096, SHR_OK},
        {1, 1, SHR_GLYPH_ITALIC, 4097, SHR_E_INVALID_ARG},
        {1, 1, SHR_GLYPH_ITALIC, -4097, SHR_E_INVALID_ARG},
        {1, 1, SHR_GLYPH_ITALIC, INT32_MAX, SHR_E_INVALID_ARG},
        {1, 1, SHR_GLYPH_ITALIC, INT32_MIN, SHR_E_INVALID_ARG},
        {1, 1, SHR_GLYPH_BOLD, INT32_MAX, SHR_OK},
        {1, 1, SHR_GLYPH_BOLD, INT32_MIN, SHR_OK},
    };
    for (size_t k = 0; k < sizeof(lim) / sizeof(lim[0]); k++) {
        int32_t x0 = 0, x1, axis = lim[k].axis;
        if (axis >= -4097 && axis <= 4097) /* so that only the bound rejects 4097 */
            shr__glyph_footprint(lim[k].w, lim[k].h, lim[k].flags & SHR_GLYPH_ITALIC, axis, &x0, &x1);
        bool row = lim[k].h == 1;
        shr_draw_cmd c = region(SHR_CMD_GLYPH, (shr_rect){0, 0, 1, 1},
                                (shr_image){big, row ? 1025 : 1, row ? 1 : 1025, row ? 1025 : 1, sizeof(big), SHR_FORMAT_A8, 0},
                                (shr_rect){0, 0, lim[k].w, lim[k].h}, (shr_point){x0, 0});
        c.flags = lim[k].flags, c.slant_axis = axis;
        ASSERT_EQ_LL(exec(&s, &c, 1), lim[k].want);
    }
    shr_image m = {px, 4, 2, 4, sizeof(px), SHR_FORMAT_A8, 0};
    shr_draw_cmd b[2] = {fill((shr_rect){0, 0, 8, 2}, 0),
                         glyph((shr_rect){0, 0, 5, 2}, SHR_RGB(255, 255, 255), m, (shr_point){0, 0})};
    b[1].flags = SHR_GLYPH_BOLD;
    ASSERT_EQ_LL(exec(&s, b, 2), SHR_OK);
    b[1].slant_axis = INT32_MIN;
    ASSERT_EQ_LL(exec(&os, b, 2), SHR_OK);
    ASSERT_MEM_EQ(buf, other, sizeof(buf));
    PASS();
}

/* BOLD and ITALIC mean nothing to an IMAGE; a source without columns has no coverage; a rejected batch writes
 * nothing. */
TEST synth_edge_sources(void) {
    uint8_t buf[8 * 2 * 4] = {0}, zero[sizeof(buf)] = {0}, px[4 * 2];
    coverage_noise(px, sizeof(px));
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, 8, 2);
    uint8_t rgba[4] = {1, 2, 3, 255};
    shr_draw_cmd img = with_src(SHR_CMD_IMAGE, (shr_rect){0, 0, 1, 1},
                                (shr_image){rgba, 1, 1, 4, 4, SHR_FORMAT_RGBA8888, 0}, (shr_point){-1, 0});
    img.flags = SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC;
    ASSERT_EQ_LL(exec(&s, &img, 1), SHR_E_INVALID_ARG);
    shr_image none = {NULL, 0, 2, 0, 0, SHR_FORMAT_A8, 0};
    shr_draw_cmd empty[2] = {glyph((shr_rect){0, 0, 1, 2}, SHR_RGB(255, 255, 255), none, (shr_point){0, 0}),
                             glyph((shr_rect){1, 0, 2, 2}, SHR_RGB(255, 255, 255), none, (shr_point){-1, 0})};
    empty[0].flags = SHR_GLYPH_BOLD, empty[1].flags = SHR_GLYPH_ITALIC;
    ASSERT_EQ_LL(exec(&s, empty, 2), SHR_OK);
    ASSERT_MEM_EQ(zero, buf, sizeof(buf));
    shr_image m = {px, 4, 2, 4, sizeof(px), SHR_FORMAT_A8, 0};
    shr_draw_cmd bad[3] = {fill((shr_rect){0, 0, 8, 2}, SHR_RGB(9, 9, 9)),
                           glyph((shr_rect){0, 0, 5, 2}, SHR_RGB(255, 255, 255), m, (shr_point){0, 0})};
    bad[1].flags = SHR_GLYPH_BOLD, bad[2] = bad[1], bad[2].src_origin.x = -1;
    ASSERT_EQ_LL(exec(&s, bad, 3), SHR_E_INVALID_ARG);
    ASSERT_MEM_EQ(zero, buf, sizeof(buf));
    PASS();
}

TEST image_is_straight_alpha_source_over(void) {
    /* The rect {1, 1, 5, 2} of a 5 x 2 buffer: alpha 0, 128, 255, 1. */
    const uint8_t rgba[2 * 20] = {9, 9, 9, 255, 9, 9, 9, 255, 9, 9, 9, 255, 9, 9, 9, 255, 9, 9, 9, 255,
                                  9, 9, 9, 255, 255, 0, 0, 0, 0, 255, 0, 128, 12, 34, 56, 255, 255, 255, 255, 1};
    shr_image m = {rgba, 5, 2, 20, 40, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU};
    for (int i = 0; i < 2; i++) {
        uint8_t buf[4 * 4];
        shr_surface s = packed(buf, FMTS[i], 4, 1);
        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, 4, 1}, SHR_RGB(100, 50, 200)),
                             region(SHR_CMD_IMAGE, (shr_rect){0, 0, 4, 1}, m, (shr_rect){1, 1, 5, 2}, (shr_point){0, 0})};
        ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
        uint32_t bg = enc(FMTS[i], 0x6432C8);
        ASSERT_EQ_LL(raw(&s, 0, 0), bg);
        ASSERT_EQ_LL(raw(&s, 1, 0), mix(FMTS[i], 0x00FF00, bg, 128));
        ASSERT_EQ_LL(raw(&s, 2, 0), enc(FMTS[i], 0x0C2238));
        ASSERT_EQ_LL(raw(&s, 3, 0), mix(FMTS[i], 0xFFFFFF, bg, 1));
    }
    PASS();
}

TEST copy_converts_between_formats(void) {
    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < 2; k++) {
            uint8_t sb[3 * 4], db[3 * 4];
            memset(db, 0, sizeof(db));
            shr_surface src = packed(sb, FMTS[i], 3, 1), dst = packed(db, FMTS[k], 3, 1);
            const uint32_t colors[3] = {0xFF00FF, 0x123456, 0xFFFFFF};
            for (int x = 0; x < 3; x++) put(&src, x, 0, enc(FMTS[i], colors[x]));
            shr_draw_cmd c = with_src(SHR_CMD_COPY, (shr_rect){1, 0, 3, 1}, as_image(&src), (shr_point){0, 0});
            ASSERT_EQ_LL(exec(&dst, &c, 1), SHR_OK);
            ASSERT_EQ_LL(raw(&dst, 0, 0), 0);
            for (int x = 1; x < 3; x++) ASSERT_EQ_LL(raw(&dst, x, 0), enc(FMTS[k], dec(FMTS[i], raw(&src, x - 1, 0))));
        }
    }
    uint8_t in[4] = {255, 0, 255, 255};
    uint16_t out = 0;
    shr_surface d = packed(&out, SHR_FORMAT_RGB565, 1, 1);
    shr_draw_cmd c = with_src(SHR_CMD_COPY, (shr_rect){0, 0, 1, 1},
                              (shr_image){in, 1, 1, 4, 4, SHR_FORMAT_RGBX8888, SHR_MEMORY_CPU}, (shr_point){0, 0});
    ASSERT_EQ_LL(exec(&d, &c, 1), SHR_OK);
    ASSERT_EQ_LL(out, 0xF81F);
    PASS();
}

TEST copy_scrolls_within_one_surface(void) {
    for (int i = 0; i < 2; i++) {
        uint8_t buf[4 * 4 * 4];
        shr_surface s = packed(buf, FMTS[i], 4, 4);
        for (int y = 0; y < 4; y++) {
            shr_draw_cmd f = fill((shr_rect){0, y, 4, y + 1}, SHR_RGB(y * 60, 0, 0));
            exec(&s, &f, 1);
        }
        size_t row = s.stride;
        shr_draw_cmd up = with_src(SHR_CMD_COPY, (shr_rect){0, 0, 4, 3},
                                   (shr_image){buf + row, 4, 3, row, row * 3, FMTS[i], SHR_MEMORY_CPU}, (shr_point){0, 0});
        ASSERT_EQ_LL(exec(&s, &up, 1), SHR_OK);
        for (int y = 0; y < 3; y++) ASSERT_EQ_LL(raw(&s, 3, y), enc(FMTS[i], (uint32_t)(y + 1) * 60 << 16));
        shr_draw_cmd down = with_src(SHR_CMD_COPY, (shr_rect){0, 1, 4, 4},
                                     (shr_image){buf, 4, 3, row, row * 3, FMTS[i], SHR_MEMORY_CPU}, (shr_point){0, 0});
        ASSERT_EQ_LL(exec(&s, &down, 1), SHR_OK);
        for (int y = 1; y < 4; y++) ASSERT_EQ_LL(raw(&s, 0, y), enc(FMTS[i], (uint32_t)y * 60 << 16));
        /* Horizontal overlap on one row: memmove semantics. */
        shr_draw_cmd right = with_src(SHR_CMD_COPY, (shr_rect){1, 0, 4, 1}, as_image(&s), (shr_point){0, 0});
        put(&s, 0, 0, enc(FMTS[i], 0x0000FF));
        ASSERT_EQ_LL(exec(&s, &right, 1), SHR_OK);
        ASSERT_EQ_LL(raw(&s, 1, 0), enc(FMTS[i], 0x0000FF));
        ASSERT_EQ_LL(raw(&s, 2, 0), enc(FMTS[i], 0x3C0000));
    }
    PASS();
}

TEST copy_overlap_rules(void) {
    _Alignas(8) uint8_t buf[128];
    memset(buf, 7, sizeof(buf));
    /* A converting copy would overwrite source bytes it has not read yet. */
    shr_surface d = surf(buf, SHR_FORMAT_RGB565, 4, 2, 8);
    shr_draw_cmd cp = with_src(SHR_CMD_COPY, (shr_rect){0, 0, 2, 1},
                               (shr_image){buf + 2, 2, 1, 8, 8, SHR_FORMAT_RGBX8888, SHR_MEMORY_CPU}, (shr_point){0, 0});
    ASSERT_EQ_LL(exec(&d, &cp, 1), SHR_E_UNSUPPORTED);
    cp.src.pixels = buf + 64; /* disjoint, after the destination */
    ASSERT_EQ_LL(exec(&d, &cp, 1), SHR_OK);
    shr_surface high = d;
    high.pixels = buf + 64;
    cp.src.pixels = buf; /* disjoint, before it */
    ASSERT_EQ_LL(exec(&high, &cp, 1), SHR_OK);
    /* Empty rectangles touch nothing, so they never overlap. */
    cp.src.pixels = buf + 2;
    cp.dst = (shr_rect){1, 0, 1, 1};
    ASSERT_EQ_LL(exec(&d, &cp, 1), SHR_OK);
    cp.dst = (shr_rect){0, 1, 2, 1};
    ASSERT_EQ_LL(exec(&d, &cp, 1), SHR_OK);
    /* Same format but another stride: row order cannot avoid clobbering. */
    shr_surface x = surf(buf, SHR_FORMAT_RGBX8888, 4, 4, 16);
    cp.dst = (shr_rect){0, 0, 2, 2};
    cp.src = (shr_image){buf + 4, 2, 2, 12, 24, SHR_FORMAT_RGBX8888, SHR_MEMORY_CPU};
    ASSERT_EQ_LL(exec(&x, &cp, 1), SHR_E_UNSUPPORTED);
    cp.src = (shr_image){buf + 64, 2, 2, 8, 16, SHR_FORMAT_RGBX8888, SHR_MEMORY_DEVICE};
    ASSERT_EQ_LL(exec(&x, &cp, 1), SHR_E_UNSUPPORTED);
    PASS();
}

TEST rotate_maps_every_rotation(void) {
    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < 2; k++) {
            uint8_t sb[6 * 4], db[6 * 4], back[6 * 4];
            shr_surface s = packed(sb, FMTS[i], 3, 2), d = packed(db, FMTS[k], 2, 3);
            for (int p = 0; p < 6; p++) put(&s, p % 3, p / 3, enc(FMTS[i], (uint32_t)(p + 1) * 0x2A1B0C));
#define SRC(x, y) enc(FMTS[k], dec(FMTS[i], raw(&s, x, y)))
            shr_draw_cmd r = rotate(&s, (shr_rect){0, 0, 2, 3}, SHR_ROTATE_90_CW);
            ASSERT_EQ_LL(exec(&d, &r, 1), SHR_OK);
            /* Logical (x, y) lands on output (h - 1 - y, x). */
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 3; x++) ASSERT_EQ_LL(raw(&d, 1 - y, x), SRC(x, y));
            r = rotate(&s, (shr_rect){0, 0, 2, 3}, SHR_ROTATE_90_CCW);
            ASSERT_EQ_LL(exec(&d, &r, 1), SHR_OK);
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 3; x++) ASSERT_EQ_LL(raw(&d, y, 2 - x), SRC(x, y));
            shr_surface e = packed(back, FMTS[k], 3, 2);
            memset(back, 0, sizeof(back));
            r = rotate(&s, (shr_rect){1, 0, 3, 2}, SHR_ROTATE_180); /* only part of the output */
            ASSERT_EQ_LL(exec(&e, &r, 1), SHR_OK);
            for (int y = 0; y < 2; y++) {
                ASSERT_EQ_LL(raw(&e, 0, y), 0);
                for (int x = 1; x < 3; x++) ASSERT_EQ_LL(raw(&e, x, y), SRC(2 - x, 1 - y));
            }
#undef SRC
        }
    }
    uint16_t src[6] = {1, 2, 3, 4, 5, 6}, dst[6] = {0};
    shr_surface s = packed(src, SHR_FORMAT_RGB565, 3, 2), d = packed(dst, SHR_FORMAT_RGB565, 2, 3);
    shr_draw_cmd r = rotate(&s, (shr_rect){0, 0, 2, 3}, SHR_ROTATE_90_CW);
    ASSERT_EQ_LL(exec(&d, &r, 1), SHR_OK);
    const uint16_t cw[6] = {4, 1, 5, 2, 6, 3};
    ASSERT_MEM_EQ(cw, dst, sizeof(cw));
    PASS();
}

TEST rotate_rejects_bad_geometry_and_sources(void) {
    uint16_t src[6] = {1, 2, 3, 4, 5, 6}, dst[6] = {0}, zero[6] = {0};
    shr_surface s = packed(src, SHR_FORMAT_RGB565, 3, 2), d = packed(dst, SHR_FORMAT_RGB565, 3, 2);
    shr_rect all = {0, 0, 3, 2};
    shr_draw_cmd r = rotate(&s, all, SHR_ROTATE_180);
    r.src.stride = 2;
    ASSERT_EQ_LL(exec(&d, &r, 1), SHR_E_INVALID_ARG);
    r = rotate(&s, all, SHR_ROTATE_180);
    r.src.domain = SHR_MEMORY_DEVICE;
    ASSERT_EQ_LL(exec(&d, &r, 1), SHR_E_UNSUPPORTED);
    uint8_t a8[6];
    r.src = (shr_image){a8, 3, 2, 3, 6, SHR_FORMAT_A8, SHR_MEMORY_CPU};
    ASSERT_EQ_LL(exec(&d, &r, 1), SHR_E_UNSUPPORTED);
    const shr_rotation bad_rot[2] = {SHR_ROTATE_NONE, (shr_rotation)7};
    for (int i = 0; i < 2; i++) {
        r = rotate(&s, all, bad_rot[i]);
        ASSERT_EQ_LL(exec(&d, &r, 1), SHR_E_INVALID_ARG);
    }
    r = rotate(&s, all, SHR_ROTATE_90_CW); /* needs a 2x3 output */
    ASSERT_EQ_LL(exec(&d, &r, 1), SHR_E_INVALID_ARG);
    r = rotate(&s, (shr_rect){0, 0, 2, 2}, SHR_ROTATE_180);
    shr_surface narrow = packed(dst, SHR_FORMAT_RGB565, 2, 2), flat = packed(dst, SHR_FORMAT_RGB565, 3, 1);
    ASSERT_EQ_LL(exec(&narrow, &r, 1), SHR_E_INVALID_ARG);
    r.dst = (shr_rect){0, 0, 3, 1};
    ASSERT_EQ_LL(exec(&flat, &r, 1), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(exec(&s, &r, 1), SHR_E_UNSUPPORTED); /* in place */
    ASSERT_MEM_EQ(zero, dst, sizeof(zero));
    /* An offset view of the same pixels overlaps; a disjoint one is fine, as is an empty rect. */
    uint16_t px[16] = {0};
    shr_surface a = packed(px, SHR_FORMAT_RGB565, 2, 2), b = packed(px + 2, SHR_FORMAT_RGB565, 2, 2);
    r = rotate(&a, (shr_rect){0, 0, 2, 2}, SHR_ROTATE_180);
    ASSERT_EQ_LL(exec(&b, &r, 1), SHR_E_UNSUPPORTED);
    r.dst = (shr_rect){1, 1, 1, 2};
    ASSERT_EQ_LL(exec(&b, &r, 1), SHR_OK);
    b = packed(px + 4, SHR_FORMAT_RGB565, 2, 2);
    r.dst = (shr_rect){0, 0, 2, 2};
    ASSERT_EQ_LL(exec(&b, &r, 1), SHR_OK);
    PASS();
}

/* A device driver's reach: CPU memory and the DEVICE buffer named `user`. */
static shr_status device_reach(const void *user, const void *pixels, int32_t w, int32_t h, shr_pixel_format f,
                               shr_memory_domain dom) SHR_NONBLOCKING {
    (void)w, (void)h, (void)f;
    return dom != SHR_MEMORY_DEVICE || pixels == user ? SHR_OK : SHR_E_UNSUPPORTED;
}

TEST check_uses_the_driver_reach(void) {
    static const char handle = 0, unknown = 0;
    uint16_t px[16 * 8];
    shr_surface cpu = packed(px, SHR_FORMAT_RGB565, 16, 8);
    shr_surface dev = {(void *)&handle, 16, 8, 32, 256, SHR_FORMAT_RGB565, 0, SHR_MEMORY_DEVICE, 1};
    shr_image self = as_image(&dev), wide = self, gone = self;
    wide.format = SHR_FORMAT_RGBX8888, wide.stride = 64;
    gone.pixels = &unknown;
    shr_rect r = {0, 0, 4, 4};
    shr_draw_cmd fill = {.kind = SHR_CMD_FILL, .dst = r};
    ASSERT_EQ_LL(shr__raster_check(&dev, &fill, 1, NULL, 0, NULL, NULL), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr__raster_check(&dev, &fill, 1, NULL, 0, device_reach, &handle), SHR_OK);
    ASSERT_EQ_LL(shr__raster_check(&dev, &fill, 1, NULL, 0, device_reach, &unknown), SHR_E_UNSUPPORTED);
    /* DEVICE buffers overlap when they are the same one, never with CPU memory. */
    shr_draw_cmd copy = {.kind = SHR_CMD_COPY, .dst = r, .src = self, .src_origin = {1, 1}};
    ASSERT_EQ_LL(shr__raster_check(&dev, &copy, 1, NULL, 0, device_reach, &handle), SHR_OK);
    copy.src = wide;
    ASSERT_EQ_LL(shr__raster_check(&dev, &copy, 1, NULL, 0, device_reach, &handle), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr__raster_check(&cpu, &copy, 1, NULL, 0, device_reach, &handle), SHR_OK);
    copy.src = gone;
    ASSERT_EQ_LL(shr__raster_check(&cpu, &copy, 1, NULL, 0, device_reach, &handle), SHR_E_UNSUPPORTED);
    shr_draw_cmd rot = {.kind = SHR_CMD_ROTATE, .dst = r, .src = self, .rotation = SHR_ROTATE_180};
    ASSERT_EQ_LL(shr__raster_check(&dev, &rot, 1, NULL, 0, device_reach, &handle), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr__raster_check(&cpu, &rot, 1, NULL, 0, device_reach, &handle), SHR_OK);
    rot.src = gone;
    ASSERT_EQ_LL(shr__raster_check(&cpu, &rot, 1, NULL, 0, device_reach, &handle), SHR_E_UNSUPPORTED);
    shr_draw_cmd group[] = {{.kind = SHR_CMD_CACHE_BEGIN, .dst = {0, 0, 8, 8}, .cache_clip = {0, 0, 8, 8}},
                            {.kind = SHR_CMD_COPY, .dst = r, .src = self, .src_origin = {8, 0}},
                            {.kind = SHR_CMD_CACHE_END}};
    ASSERT_EQ_LL(shr__raster_check(&dev, group, 3, NULL, 0, device_reach, &handle), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__raster_check(&cpu, group, 3, NULL, 0, device_reach, &handle), SHR_OK);
    group[1].src = as_image(&cpu);
    ASSERT_EQ_LL(shr__raster_check(&dev, group, 3, NULL, 0, device_reach, &handle), SHR_OK);
    ASSERT_EQ_LL(shr__raster_reads_dst(&cpu, &self), false);
    /* REGISTER asks the same reach. */
    shr_image tbl[1] = {{0}};
    shr_draw_cmd r1 = {.kind = SHR_CMD_BUFFER_REGISTER, .buffer = 1, .src = self};
    r1.src.format = SHR_FORMAT_A8;
    ASSERT_EQ_LL(shr__raster_buffer_check(&r1, tbl, 1, NULL, NULL), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr__raster_buffer_check(&r1, tbl, 1, device_reach, &handle), SHR_OK);
    ASSERT_EQ_LL(shr__raster_buffer_check(&r1, tbl, 1, device_reach, &unknown), SHR_E_UNSUPPORTED);
    PASS();
}

/* The pixels the driver drew at (x, 0) of `s`, as 0xRRGGBB of an RGBX8888 surface. */
static uint32_t at(const shr_surface *s, int32_t x) { return raw(s, x, 0) >> 8; }

TEST buffers_register_replace_and_release(void) {
    uint8_t px[4 * 4];
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 4, 1);
    uint8_t red[4] = {255, 0, 0, 255}, blue[8] = {0, 0, 255, 255, 0, 255, 0, 255};
    shr_image r = {red, 1, 1, 4, 4, SHR_FORMAT_RGBA8888, 0}, b = {blue, 2, 1, 8, 8, SHR_FORMAT_RGBA8888, SHR_MEMORY_DMA};
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 3, &drv), SHR_OK);
    ASSERT_EQ_LL(drv.caps.max_buffers, 3);
    ASSERT(!drv.caps.max_buffer_width && !drv.caps.max_buffer_height && !drv.caps.buffer_bytes && !drv.caps.buffer_flags);
    shr_draw_cmd img = cmd(SHR_CMD_IMAGE, (shr_rect){0, 0, 1, 1});
    img.buffer = 3, img.src_rect = (shr_rect){0, 0, 1, 1};
    shr_draw_cmd c[3] = {cmd(SHR_CMD_BUFFER_REGISTER, (shr_rect){0, 0, 0, 0}), img};
    c[0].buffer = 3, c[0].src = r;
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 2, 1), SHR_OK);
    ASSERT_EQ_LL(at(&s, 0), 0xFF0000);
    /* Registrations last across batches, and the driver reads the memory in place. */
    red[1] = 128;
    img.dst = (shr_rect){1, 0, 2, 1};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, &img, 1, 2), SHR_OK);
    ASSERT_EQ_LL(at(&s, 1), 0xFF8000);
    /* REGISTER replaces; UPDATE inside the buffer is accepted; RELEASE of ids naming nothing is no error. */
    c[0].src = b;
    c[1] = buf_cmd(SHR_CMD_BUFFER_UPDATE, 3), c[1].src_rect = (shr_rect){1, 0, 2, 1};
    img.dst = (shr_rect){2, 0, 4, 1}, img.src_rect = (shr_rect){0, 0, 2, 1};
    c[2] = img;
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 3), SHR_OK);
    ASSERT_EQ_LL(at(&s, 2), 0x0000FF);
    ASSERT_EQ_LL(at(&s, 3), 0x00FF00);
    const uint32_t unknown[3] = {0, 1, 4};
    for (int i = 0; i < 3; i++) {
        c[0] = buf_cmd(SHR_CMD_BUFFER_RELEASE, unknown[i]);
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 1, 4), SHR_OK);
    }
    /* RELEASE forgets the id: drawing from it is rejected, also within the batch that releases it. */
    c[0] = buf_cmd(SHR_CMD_BUFFER_RELEASE, 3), c[1] = img;
    memset(px, 0, sizeof(px));
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 2, 5), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(drv.execute(drv.user, &s, &img, 1, 6), SHR_E_INVALID_ARG);
    for (int x = 0; x < 4; x++) ASSERT_EQ_LL(at(&s, x), 0);
    /* The stateless path draws from the table it is given and never executes buffer commands. */
    shr_image t[3] = {{0}, {0}, b};
    c[0] = buf_cmd(SHR_CMD_BUFFER_RELEASE, 3), c[1] = img;
    ASSERT_EQ_LL(shr_software_execute(&s, c, 2, t, 3), SHR_OK);
    ASSERT_EQ_LL(at(&s, 2), 0x0000FF);
    ASSERT_EQ_LL(shr_software_execute(&s, c, 2, t, 2), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_execute(&s, c, 2, NULL, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

TEST buffer_commands_are_validated(void) {
    uint8_t px[4 * 4] = {0}, zero[sizeof(px)] = {0}, a8[8] = {0}, rgba[4] = {1, 2, 3, 255};
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 4, 1);
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 2, &drv), SHR_OK);
    shr_image ok = {a8, 4, 2, 4, 8, SHR_FORMAT_A8, 0};
    const struct {
        uint32_t id;
        shr_image mem;
        shr_status want;
    } regs[] = {
        {1, ok, SHR_OK},
        {2, {a8, 8, 2, 4, 8, SHR_FORMAT_A4, SHR_MEMORY_DMA}, SHR_OK},
        {2, {rgba, 1, 1, 4, 4, SHR_FORMAT_RGBA8888, 0}, SHR_OK},
        {0, ok, SHR_E_INVALID_ARG},
        {3, ok, SHR_E_INVALID_ARG},
        {1, {a8, 4, 2, 3, 8, SHR_FORMAT_A8, 0}, SHR_E_INVALID_ARG},  /* stride */
        {1, {a8, 4, 2, 4, 8, (shr_pixel_format)0, 0}, SHR_E_INVALID_ARG},
        {1, {a8, 2, 1, 4, 8, SHR_FORMAT_RGB565, 0}, SHR_E_UNSUPPORTED},
        {1, {a8, 1, 1, 4, 8, SHR_FORMAT_RGBX8888, 0}, SHR_E_UNSUPPORTED},
        {1, {a8, 4, 2, 4, 8, SHR_FORMAT_A8, SHR_MEMORY_DEVICE}, SHR_E_UNSUPPORTED},
    };
    shr_draw_cmd fl = fill((shr_rect){0, 0, 4, 1}, SHR_RGB(9, 9, 9)), c[2] = {reg(1), fl};
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        c[0].buffer = regs[i].id, c[0].src = regs[i].mem;
        memset(px, 0, sizeof(px));
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 2, 1), regs[i].want);
        if (regs[i].want != SHR_OK) ASSERT_MEM_EQ(zero, px, sizeof(px));
    }
    c[0].buffer = 1, c[0].src = ok;
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 1, 1), SHR_OK);
    /* UPDATE: a registered id and a rect inside its buffer. */
    const struct {
        uint32_t id;
        shr_rect r;
        shr_status want;
    } ups[] = {
        {1, {0, 0, 4, 2}, SHR_OK},           {1, {2, 1, 2, 1}, SHR_OK},           {1, {0, 0, 5, 2}, SHR_E_INVALID_ARG},
        {1, {0, 0, 4, 3}, SHR_E_INVALID_ARG}, {1, {-1, 0, 1, 1}, SHR_E_INVALID_ARG}, {1, {0, -1, 1, 1}, SHR_E_INVALID_ARG},
        {1, {2, 0, 1, 1}, SHR_E_INVALID_ARG}, {0, {0, 0, 1, 1}, SHR_E_INVALID_ARG}, {3, {0, 0, 1, 1}, SHR_E_INVALID_ARG},
        {2, {0, 0, 1, 1}, SHR_OK},
    };
    for (size_t i = 0; i < sizeof(ups) / sizeof(ups[0]); i++) {
        c[0] = buf_cmd(SHR_CMD_BUFFER_UPDATE, ups[i].id), c[0].src_rect = ups[i].r;
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 2, 1), ups[i].want);
    }
    c[0] = buf_cmd(SHR_CMD_BUFFER_RELEASE, 2);
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 1, 1), SHR_OK);
    c[0] = buf_cmd(SHR_CMD_BUFFER_UPDATE, 2), c[0].src_rect = (shr_rect){0, 0, 0, 0};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 2, 1), SHR_E_INVALID_ARG); /* not registered */
    /* Buffer commands lead the batch; the driver checks the batch even without commands to run. */
    memset(px, 0, sizeof(px));
    shr_draw_cmd late[3] = {fl, reg(1), fl};
    late[1].src = ok;
    ASSERT_EQ_LL(drv.execute(drv.user, &s, late, 3, 1), SHR_E_INVALID_ARG);
    ASSERT_MEM_EQ(zero, px, sizeof(px));
    ASSERT_EQ_LL(drv.execute(drv.user, &s, NULL, 1, 1), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(drv.execute(drv.user, &s, NULL, 0, 1), SHR_OK);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

/* Draw regions: a registered id of a format the kind takes, a rect inside the buffer (A4 from an even column) and
 * a source inside the rect. A rejected batch writes nothing. */
TEST buffer_regions_are_validated(void) {
    uint8_t px[4 * 4] = {0}, zero[sizeof(px)] = {0}, a8[4 * 3], a4[2 * 3], rgba[4 * 4 * 3];
    memset(a8, 255, sizeof(a8)), memset(a4, 255, sizeof(a4)), memset(rgba, 255, sizeof(rgba));
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 4, 1);
    shr_image t[4] = {{a8, 4, 3, 4, sizeof(a8), SHR_FORMAT_A8, 0}, {a4, 4, 3, 2, sizeof(a4), SHR_FORMAT_A4, 0},
                      {rgba, 4, 3, 16, sizeof(rgba), SHR_FORMAT_RGBA8888, 0}, {0}};
    const struct {
        shr_cmd_kind kind;
        uint32_t id;
        shr_rect r;
        shr_point o;
        int32_t w;
        shr_status want;
    } cases[] = {
        {SHR_CMD_GLYPH, 1, {1, 1, 4, 3}, {0, 0}, 3, SHR_OK},
        {SHR_CMD_GLYPH, 1, {1, 1, 4, 3}, {1, 1}, 2, SHR_OK},
        {SHR_CMD_GLYPH, 1, {1, 1, 4, 3}, {1, 0}, 3, SHR_E_INVALID_ARG}, /* inside the buffer, outside the rect */
        {SHR_CMD_GLYPH, 1, {1, 1, 4, 3}, {0, 2}, 1, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 1, {1, 1, 4, 3}, {-1, 0}, 1, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 1, {1, 1, 4, 3}, {0, -1}, 1, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 1, {1, 1, 5, 3}, {0, 0}, 1, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 1, {1, 1, 4, 4}, {0, 0}, 1, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 1, {-1, 0, 1, 1}, {0, 0}, 1, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 1, {0, -1, 1, 1}, {0, 0}, 1, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 1, {2, 0, 1, 1}, {0, 0}, 0, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 1, {0, 2, 1, 1}, {0, 0}, 0, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 2, {2, 0, 3, 3}, {0, 0}, 1, SHR_OK},
        {SHR_CMD_GLYPH, 2, {1, 0, 3, 3}, {0, 0}, 1, SHR_E_INVALID_ARG}, /* A4 from an odd column */
        {SHR_CMD_GLYPH, 2, {3, 0, 3, 3}, {0, 0}, 0, SHR_E_INVALID_ARG},
        {SHR_CMD_IMAGE, 3, {1, 0, 4, 3}, {0, 0}, 3, SHR_OK},
        {SHR_CMD_IMAGE, 3, {1, 0, 4, 3}, {1, 0}, 3, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 3, {0, 0, 1, 1}, {0, 0}, 1, SHR_E_UNSUPPORTED},
        {SHR_CMD_IMAGE, 1, {0, 0, 1, 1}, {0, 0}, 1, SHR_E_UNSUPPORTED},
        {SHR_CMD_IMAGE, 2, {0, 0, 1, 1}, {0, 0}, 1, SHR_E_UNSUPPORTED},
        {SHR_CMD_GLYPH, 4, {0, 0, 0, 0}, {0, 0}, 0, SHR_E_INVALID_ARG}, /* not registered */
        {SHR_CMD_GLYPH, 0, {0, 0, 0, 0}, {0, 0}, 0, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, 5, {0, 0, 0, 0}, {0, 0}, 0, SHR_E_INVALID_ARG},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, 4, 1}, SHR_RGB(9, 9, 9)), cmd(cases[i].kind, (shr_rect){0, 0, cases[i].w, 1})};
        c[1].buffer = cases[i].id, c[1].src_rect = cases[i].r, c[1].src_origin = cases[i].o;
        memset(px, 0, sizeof(px));
        ASSERT_EQ_LL(shr_software_execute(&s, c, 2, t, 4), cases[i].want);
        if (cases[i].want != SHR_OK) ASSERT_MEM_EQ(zero, px, sizeof(px));
    }
    PASS();
}

TEST execute_rejects_bad_batches_without_drawing(void) {
    uint8_t buf[4 * 4 * 4] = {0}, zero[sizeof(buf)] = {0};
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, 4, 4);
    shr_draw_cmd ok = fill((shr_rect){0, 0, 4, 4}, SHR_RGB(9, 9, 9));
    const shr_rect outside[7] = {{-1, 0, 1, 1}, {0, -1, 1, 1}, {2, 0, 1, 1}, {0, 2, 1, 1},
                                 {0, 0, 5, 1},  {0, 0, 1, 5},  {4, 4, 5, 5}};
    for (int i = 0; i < 7; i++) {
        shr_draw_cmd c[2] = {ok, fill(outside[i], SHR_RGB(9, 9, 9))};
        ASSERT_EQ_LL(exec(&s, c, 2), SHR_E_INVALID_ARG);
    }
    shr_draw_cmd c[2] = {ok, cmd((shr_cmd_kind)0, (shr_rect){0, 0, 1, 1})};
    ASSERT_EQ_LL(exec(&s, c, 2), SHR_E_INVALID_ARG);
    c[1].kind = (shr_cmd_kind)99;
    ASSERT_EQ_LL(exec(&s, c, 2), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(exec(&s, NULL, 1), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(exec(&s, NULL, 0), SHR_OK);
    ASSERT_EQ_LL(exec(NULL, &ok, 1), SHR_E_INVALID_ARG);
    shr_surface bad = s;
    bad.stride = 8;
    ASSERT_EQ_LL(exec(&bad, &ok, 1), SHR_E_INVALID_ARG);
    bad = s;
    bad.format = SHR_FORMAT_A8;
    ASSERT_EQ_LL(exec(&bad, &ok, 1), SHR_E_INVALID_ARG);
    bad = s;
    bad.domain = SHR_MEMORY_DEVICE;
    ASSERT_EQ_LL(exec(&bad, &ok, 1), SHR_E_UNSUPPORTED);

    uint8_t a8[4] = {0};
    const struct {
        shr_cmd_kind kind;
        shr_image src;
        shr_point origin;
        shr_status want;
    } srcs[] = {
        {SHR_CMD_GLYPH, {a8, 1, 1, 4, 4, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_IMAGE, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_IMAGE, {a8, 4, 1, 2, 2, SHR_FORMAT_A4, SHR_MEMORY_CPU}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_COPY, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_COPY, {a8, 1, 1, 4, 4, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_COPY, {a8, 2, 1, 2, 4, SHR_FORMAT_RGB565, SHR_MEMORY_CPU}, {0, 0}, SHR_E_INVALID_ARG},
        {SHR_CMD_COPY, {a8, 1, 1, 4, 4, SHR_FORMAT_RGBX8888, SHR_MEMORY_DEVICE}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_GLYPH, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {-1, 0}, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {0, -1}, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {2, 0}, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {0, 2}, SHR_E_INVALID_ARG},
        {SHR_CMD_IMAGE, {a8, 1, 1, 4, 4, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, {1, 0}, SHR_E_INVALID_ARG},
    };
    for (size_t i = 0; i < sizeof(srcs) / sizeof(srcs[0]); i++) {
        c[1] = with_src(srcs[i].kind, (shr_rect){0, 0, 1, 1}, srcs[i].src, srcs[i].origin);
        ASSERT_EQ_LL(exec(&s, c, 2), srcs[i].want);
    }
    ASSERT_MEM_EQ(zero, buf, sizeof(buf));
    /* Empty rectangles are valid and draw nothing. */
    shr_draw_cmd empty[2] = {fill((shr_rect){1, 1, 1, 3}, SHR_RGB(9, 9, 9)), fill((shr_rect){1, 1, 3, 1}, SHR_RGB(9, 9, 9))};
    ASSERT_EQ_LL(exec(&s, empty, 2), SHR_OK);
    ASSERT_MEM_EQ(zero, buf, sizeof(buf));
    shr_surface none = packed(buf, SHR_FORMAT_RGB565, 0, 0);
    shr_draw_cmd nothing = fill((shr_rect){0, 0, 0, 0}, 0);
    ASSERT_EQ_LL(exec(&none, &nothing, 1), SHR_OK);
    PASS();
}

TEST cache_hints_are_validated(void) {
    uint8_t buf[4 * 4 * 4] = {0}, zero[sizeof(buf)] = {0};
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, 4, 4);
    shr_rect g = {0, 0, 4, 2};
    shr_draw_cmd f = fill(g, SHR_RGB(1, 2, 3));
    uint16_t px[4] = {0};
    shr_surface rs = packed(px, SHR_FORMAT_RGBX8888, 1, 1);
    const shr_draw_cmd bad[][4] = {
        {f, begin(1, g, g), begin(2, g, g), end()},                       /* nested */
        {f, end(), f, f},                                                 /* END without BEGIN */
        {f, begin(1, g, g), f, f},                                        /* never ended */
        {f, begin(1, (shr_rect){0, 0, 5, 2}, g), f, end()},               /* group outside the target */
        {f, begin(1, g, (shr_rect){0, 0, 4, 3}), f, end()},               /* clip outside the group */
        {f, begin(1, g, (shr_rect){2, 0, 1, 1}), f, end()},               /* inverted clip */
        {f, begin(1, g, g), fill((shr_rect){0, 0, 4, 3}, 0), end()},      /* command outside the group */
        {f, begin(1, (shr_rect){0, 0, 1, 1}, (shr_rect){0, 0, 1, 1}), rotate(&rs, (shr_rect){0, 0, 1, 1}, SHR_ROTATE_180), end()},
        {f, begin(1, g, g), with_src(SHR_CMD_COPY, (shr_rect){0, 0, 4, 1}, as_image(&s), (shr_point){0, 1}), end()}, /* reads the target */
        {f, buf_cmd(SHR_CMD_BUFFER_RELEASE, 1), f, f},                    /* buffer command after a draw */
        {begin(1, g, g), buf_cmd(SHR_CMD_BUFFER_RELEASE, 1), f, end()},   /* buffer command in a group */
    };
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 16, NBUF, &drv), SHR_OK);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERT_EQ_LL(exec(&s, bad[i], 4), SHR_E_INVALID_ARG);
        ASSERT_EQ_LL(drv.execute(drv.user, &s, bad[i], 4, 1), SHR_E_INVALID_ARG);
    }
    ASSERT_MEM_EQ(zero, buf, sizeof(buf));
    /* Memory right before and after the target may be read. */
    uint8_t mem[3 * 16] = {0};
    shr_surface mid = packed(mem + 16, SHR_FORMAT_RGBX8888, 4, 1);
    for (int i = 0; i < 2; i++) {
        shr_surface other = packed(mem + 32 * i, SHR_FORMAT_RGBX8888, 4, 1);
        shr_rect r = {0, 0, 4, 1};
        shr_draw_cmd ok[3] = {begin(1, r, r), with_src(SHR_CMD_COPY, r, as_image(&other), (shr_point){0, 0}), end()};
        ASSERT_EQ_LL(exec(&mid, ok, 3), SHR_OK);
        ASSERT_EQ_LL(drv.execute(drv.user, &mid, ok, 3, 1), SHR_OK);
    }
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

TEST execute_clips_groups_to_cache_clip(void) {
    for (int i = 0; i < 2; i++) {
        uint8_t buf[4 * 3 * 4];
        memset(buf, 0, sizeof(buf));
        shr_surface s = packed(buf, FMTS[i], 4, 3);
        shr_rect g = {0, 0, 4, 2};
        shr_draw_cmd c[5] = {begin(7, g, (shr_rect){1, 1, 3, 2}), fill(g, SHR_RGB(255, 0, 0)), end(),
                             fill((shr_rect){0, 2, 4, 3}, SHR_RGB(0, 0, 255)), fill((shr_rect){0, 0, 1, 1}, SHR_RGB(0, 255, 0))};
        ASSERT_EQ_LL(exec(&s, c, 5), SHR_OK);
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 4; x++) {
                uint32_t want = y == 2 ? 0x0000FF : (x == 0 && y == 0) ? 0x00FF00 : (y == 1 && x >= 1 && x < 3) ? 0xFF0000 : 0;
                ASSERT_EQ_LL(raw(&s, x, y), want ? enc(FMTS[i], want) : 0);
            }
    }
    PASS();
}

/* A group whose content depends on the group origin: fill, glyph, image and copy at offsets, after the
 * REGISTERs of its buffers. */
static const uint8_t g_a8[4] = {0, 85, 170, 255};
static const uint8_t g_rgba[8] = {255, 255, 0, 255, 0, 255, 255, 100};
static uint8_t g_copy[2 * 4];

static size_t group(shr_draw_cmd *c, uint64_t key, shr_rect dst, shr_rect clip, shr_pixel_format f) {
    shr_surface cs = packed(g_copy, f, 2, 1);
    put(&cs, 0, 0, enc(f, 0x804020));
    put(&cs, 1, 0, enc(f, 0x102040));
    int32_t x = dst.x0, y = dst.y0;
    c[2] = begin(key, dst, clip);
    c[3] = fill(dst, SHR_RGB(40, 40, 40));
    c[4] = glyph((shr_rect){x + 1, y, x + 5, y + 1}, SHR_RGB(255, 255, 255),
                 (shr_image){g_a8, 4, 1, 4, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, (shr_point){0, 0});
    c[5] = with_src(SHR_CMD_IMAGE, (shr_rect){x, y + 1, x + 2, y + 2},
                    (shr_image){g_rgba, 2, 1, 8, 8, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, (shr_point){0, 0});
    c[6] = with_src(SHR_CMD_COPY, (shr_rect){x + 4, y + 1, x + 6, y + 2}, as_image(&cs), (shr_point){0, 0});
    c[7] = end();
    c[0] = reg(c[4].buffer), c[1] = reg(c[5].buffer);
    return 8;
}

TEST cached_driver_hit_matches_direct_drawing(void) {
    for (int i = 0; i < 2; i++) {
        uint8_t ref[8 * 4 * 4], a[sizeof(ref)], b[sizeof(ref)];
        memset(ref, 0x11, sizeof(ref)), memset(a, 0x11, sizeof(a)), memset(b, 0x11, sizeof(b));
        shr_surface sr = packed(ref, FMTS[i], 8, 4), sa = packed(a, FMTS[i], 8, 4), sb = packed(b, FMTS[i], 8, 4);
        shr_draw_cmd c[8];
        size_t n = group(c, 42, (shr_rect){1, 1, 7, 3}, (shr_rect){2, 1, 7, 3}, FMTS[i]);
        ASSERT_EQ_LL(exec(&sr, c, n), SHR_OK);
        shr_framebuffer_driver drv;
        ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 16, NBUF, &drv), SHR_OK);
        ASSERT_EQ_LL(drv.execute(drv.user, &sa, c, n, 1), SHR_OK); /* miss: renders and stores */
        ASSERT_MEM_EQ(ref, a, sizeof(ref));
        ASSERT_EQ_LL(drv.execute(drv.user, &sb, c, n, 2), SHR_OK); /* hit */
        ASSERT_MEM_EQ(ref, b, sizeof(ref));
        /* A hit copies the stored pixels: same key and size with other commands still draws the old content. */
        c[3].color = SHR_RGB(0, 0, 0);
        memset(b, 0x11, sizeof(b));
        ASSERT_EQ_LL(drv.execute(drv.user, &sb, c, n, 3), SHR_OK);
        ASSERT_MEM_EQ(ref, b, sizeof(ref));
        /* Same key and size at another position still hits. */
        n = group(c, 42, (shr_rect){2, 2, 8, 4}, (shr_rect){2, 2, 8, 4}, FMTS[i]);
        c[3].color = SHR_RGB(0, 0, 0);
        ASSERT_EQ_LL(drv.execute(drv.user, &sb, c, n, 4), SHR_OK);
        ASSERT_EQ_LL(raw(&sb, 2, 2), enc(FMTS[i], 0x282828));
        /* Another size misses. */
        n = group(c, 42, (shr_rect){0, 0, 7, 2}, (shr_rect){0, 0, 7, 2}, FMTS[i]);
        c[3].color = SHR_RGB(0, 0, 0);
        ASSERT_EQ_LL(drv.execute(drv.user, &sb, c, n, 5), SHR_OK);
        ASSERT_EQ_LL(raw(&sb, 0, 0), enc(FMTS[i], 0));
        /* An empty clip draws nothing. */
        memcpy(b, a, sizeof(b));
        n = group(c, 43, (shr_rect){0, 0, 6, 2}, (shr_rect){3, 1, 3, 2}, FMTS[i]);
        ASSERT_EQ_LL(drv.execute(drv.user, &sb, c, n, 6), SHR_OK);
        ASSERT_MEM_EQ(a, b, sizeof(b));
        ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    }
    PASS();
}

TEST cached_entries_depend_on_format(void) {
    uint8_t a[4 * 2], b[4 * 4];
    shr_surface s565 = packed(a, SHR_FORMAT_RGB565, 4, 1), s8888 = packed(b, SHR_FORMAT_RGBX8888, 4, 1);
    shr_rect r = {0, 0, 4, 1};
    shr_draw_cmd c[3] = {begin(5, r, r), fill(r, SHR_RGB(255, 0, 0)), end()};
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 16, NBUF, &drv), SHR_OK);
    ASSERT_EQ_LL(drv.execute(drv.user, &s565, c, 3, 1), SHR_OK);
    c[1].color = SHR_RGB(0, 0, 255);
    ASSERT_EQ_LL(drv.execute(drv.user, &s8888, c, 3, 2), SHR_OK);
    c[1].color = 0;
    ASSERT_EQ_LL(drv.execute(drv.user, &s565, c, 3, 3), SHR_OK);
    ASSERT_EQ_LL(raw(&s565, 3, 0), 0xF800);
    ASSERT_EQ_LL(raw(&s8888, 3, 0), 0x0000FFFF);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

/* Draws a size x size RGBX8888 group of `key` filled with `color`; returns the colour now shown. */
static uint32_t draw_sized(shr_framebuffer_driver *drv, uint64_t key, shr_color color, int32_t size) {
    static uint8_t buf[48 * 48 * 4];
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, size, size);
    shr_rect r = {0, 0, size, size};
    shr_draw_cmd c[3] = {begin(key, r, r), fill(r, color), end()};
    ASSERT_EQ_LL(drv->execute(drv->user, &s, c, 3, 1), SHR_OK);
    return raw(&s, size - 1, size - 1) >> 8;
}

/* 4 KiB of pixels: two fit in the budget below with their bookkeeping, three do not. */
#define KEYED_BUDGET 10000
static uint32_t draw_keyed(shr_framebuffer_driver *drv, uint64_t key, shr_color color) {
    return draw_sized(drv, key, color, 32);
}

TEST cache_evicts_least_recently_used(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(&al, KEYED_BUDGET, NBUF, &drv), SHR_OK);
    ASSERT_EQ_LL(draw_keyed(&drv, 1, 0x010101), 0x010101);
    ASSERT_EQ_LL(draw_keyed(&drv, 2, 0x020202), 0x020202);
    ASSERT_EQ_LL(draw_keyed(&drv, 1, 0xFFFFFF), 0x010101); /* hit, now most recent */
    ASSERT_EQ_LL(draw_keyed(&drv, 3, 0x030303), 0x030303); /* evicts 2 */
    ASSERT_EQ_LL(draw_keyed(&drv, 1, 0xFFFFFF), 0x010101);
    ASSERT_EQ_LL(draw_keyed(&drv, 3, 0xFFFFFF), 0x030303);
    ASSERT_EQ_LL(draw_keyed(&drv, 2, 0x222222), 0x222222); /* re-rendered, evicts 1 */
    ASSERT_EQ_LL(draw_keyed(&drv, 1, 0x111111), 0x111111);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

TEST cache_evicts_only_after_a_new_entry_exists(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(&al, KEYED_BUDGET, NBUF, &drv), SHR_OK);
    ASSERT_EQ_LL(draw_keyed(&drv, 1, 0x010101), 0x010101);
    ASSERT_EQ_LL(draw_keyed(&drv, 2, 0x020202), 0x020202);
    f.budget = 1; /* the entry, not its pixels */
    ASSERT_EQ_LL(draw_keyed(&drv, 3, 0x030303), 0x030303);
    f.budget = -1;
    ASSERT_EQ_LL(draw_keyed(&drv, 1, 0xFFFFFF), 0x010101);
    ASSERT_EQ_LL(draw_keyed(&drv, 2, 0xFFFFFF), 0x020202);
    ASSERT_EQ_LL(draw_keyed(&drv, 3, 0x333333), 0x333333);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

TEST cache_skips_groups_over_half_the_budget(void) {
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, KEYED_BUDGET, NBUF, &drv), SHR_OK);
    ASSERT_EQ_LL(draw_keyed(&drv, 1, 0x010101), 0x010101);
    ASSERT_EQ_LL(draw_sized(&drv, 2, 0x020202, 40), 0x020202); /* fits the budget, but not half of it */
    ASSERT_EQ_LL(draw_sized(&drv, 2, 0x222222, 40), 0x222222);
    ASSERT_EQ_LL(draw_keyed(&drv, 1, 0xFFFFFF), 0x010101);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

/* Counts live bytes; frees must name the size they were allocated with. */
typedef struct byte_alloc {
    size_t live;
} byte_alloc;

static void *ba_alloc(void *user, size_t size, size_t align, shr_alloc_kind kind) {
    (void)align, (void)kind;
    ((byte_alloc *)user)->live += size;
    return malloc(size);
}

static void ba_free(void *user, void *p, size_t size, size_t align, shr_alloc_kind kind) {
    (void)align, (void)kind;
    ((byte_alloc *)user)->live -= size;
    free(p);
}

TEST cache_budget_includes_the_table(void) {
    byte_alloc b = {0};
    shr_allocator al = {&b, ba_alloc, ba_free};
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 20000, NBUF, &drv), SHR_OK);
    size_t state = b.live;
    uint8_t px[4];
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 1, 1);
    shr_rect r = {0, 0, 1, 1};
    shr_draw_cmd c[3] = {begin(0, r, r), fill(r, 0), end()};
    for (uint32_t k = 1; k <= 1000; k++) {
        c[0].key[0] = k, c[1].color = k;
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
        ASSERT(b.live - state <= 20000);
    }
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(b.live, 0);
    PASS();
}

/* A group is either stored for good or never allocated for: under no budget is a new entry evicted at once. */
TEST small_budgets_do_not_store_and_evict(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    uint8_t px[8 * 8 * 4];
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 8, 8);
    shr_rect r = {0, 0, 8, 8};
    shr_draw_cmd c[3] = {begin(1, r, r), fill(r, 0x010101), end()};
    int kept = 0;
    for (uint64_t budget = 0; budget <= 4096; budget += 16) {
        shr_framebuffer_driver drv;
        ASSERT_EQ_LL(shr_software_driver_create(&al, budget, NBUF, &drv), SHR_OK);
        f.budget = LONG_MAX;
        c[1].color = 0x010101;
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
        bool stored = f.budget != LONG_MAX;
        f.budget = LONG_MAX;
        c[1].color = 0x020202;
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
        ASSERT_EQ_LL(f.budget, LONG_MAX);
        ASSERT_EQ_LL(raw(&s, 7, 7) >> 8, stored ? 0x010101 : 0x020202);
        kept += stored;
        f.budget = -1;
        ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
        ASSERT_EQ_LL(f.live, 0);
    }
    ASSERT(kept > 0 && kept <= 4096 / 16); /* both sides of the limit were swept */
    PASS();
}

TEST cache_table_failures_draw_directly(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    uint8_t px[4];
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 1, 1);
    shr_rect r = {0, 0, 1, 1};
    shr_draw_cmd c[3] = {begin(0, r, r), fill(r, 0), end()};
    /* Allocations of a first miss: entry, pixels, table, buckets. */
    for (long fail_at = 2; fail_at <= 3; fail_at++) {
        shr_framebuffer_driver drv;
        ASSERT_EQ_LL(shr_software_driver_create(&al, 1 << 20, NBUF, &drv), SHR_OK);
        f.budget = fail_at;
        c[1].color = 0x123456;
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
        ASSERT_EQ_LL(raw(&s, 0, 0) >> 8, 0x123456);
        f.budget = -1;
        c[1].color = 0x654321; /* nothing was stored */
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
        ASSERT_EQ_LL(raw(&s, 0, 0) >> 8, 0x654321);
        ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
        ASSERT_EQ_LL(f.live, 0);
    }
    /* Keys 32 apart share a bucket; the tenth one doubles the table, which fails once. */
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 1 << 20, NBUF, &drv), SHR_OK);
    for (uint32_t k = 0; k < 300; k++) {
        f.budget = k == 9 ? 2 : -1;
        c[0].key[0] = (uint64_t)k * 32, c[1].color = k;
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
        ASSERT_EQ_LL(raw(&s, 0, 0) >> 8, k);
    }
    f.budget = -1;
    for (uint32_t k = 0; k < 300; k++) {
        c[0].key[0] = (uint64_t)k * 32, c[1].color = 0xFFFFFF;
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
        ASSERT_EQ_LL(raw(&s, 0, 0) >> 8, k == 9 ? 0xFFFFFF : k);
    }
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

TEST cache_table_grows_with_many_entries(void) {
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 20, NBUF, &drv), SHR_OK);
    uint8_t px[4];
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 1, 1);
    shr_rect r = {0, 0, 1, 1};
    shr_draw_cmd c[3] = {begin(0, r, r), fill(r, 0), end()};
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t k = 0; k < 2000; k++) {
            c[0].key[0] = k, c[1].color = pass ? 0 : k;
            ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
            ASSERT_EQ_LL(raw(&s, 0, 0) >> 8, k);
        }
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

TEST cache_keys_are_128_bit(void) {
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 16, NBUF, &drv), SHR_OK);
    uint8_t px[4];
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 1, 1);
    shr_rect r = {0, 0, 1, 1};
    shr_draw_cmd c[3] = {begin(5, r, r), fill(r, SHR_RGB(1, 1, 1)), end()};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
    c[0].key[1] ^= 1, c[1].color = SHR_RGB(2, 2, 2);
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
    ASSERT_EQ_LL(raw(&s, 0, 0), 0x020202FF);
    c[0].key[1] ^= 1, c[1].color = 0;
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
    ASSERT_EQ_LL(raw(&s, 0, 0), 0x010101FF);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

TEST cache_keeps_sizes_and_formats_of_one_key_apart(void) {
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 20, NBUF, &drv), SHR_OK);
    /* Heights 64 apart share a bucket while the table is small. */
    static const int32_t hs[10] = {1, 2, 3, 4, 5, 65, 66, 67, 68, 69};
    uint8_t buf[10 * 69 * 4];
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 2; i++)
            for (int32_t w = 1; w <= 10; w++)
                for (int j = 0; j < 10; j++) {
                    int32_t h = hs[j];
                    shr_surface s = packed(buf, FMTS[i], 10, 69);
                    shr_rect r = {0, 0, w, h};
                    uint32_t color = (uint32_t)(w * 20) << 16 | (uint32_t)(i * 200) << 8 | (uint32_t)(j * 25);
                    shr_draw_cmd c[3] = {begin(77, r, r), fill(r, pass ? 0 : color), end()};
                    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
                    ASSERT_EQ_LL(raw(&s, w - 1, h - 1), enc(FMTS[i], color));
                }
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

TEST cache_evicts_oldest_of_many(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 100 * 80, NBUF, &drv), SHR_OK); /* about 100 single pixels */
    uint8_t px[4];
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 1, 1);
    shr_rect r = {0, 0, 1, 1};
    shr_draw_cmd c[3] = {begin(0, r, r), fill(r, 0), end()};
    for (uint32_t k = 1; k <= 300; k++) {
        c[0].key[0] = k, c[1].color = k;
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
    }
    c[1].color = 0;
    for (uint32_t k = 300; k >= 250; k--) {
        c[0].key[0] = k;
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
        ASSERT_EQ_LL(raw(&s, 0, 0) >> 8, k);
    }
    c[0].key[0] = 1;
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
    ASSERT_EQ_LL(raw(&s, 0, 0) >> 8, 0);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

TEST uncacheable_groups_draw_directly(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    const struct {
        uint64_t budget;
        long alloc_budget; /* allocations allowed after create */
    } cases[] = {{0, -1}, {64, -1}, {1 << 16, 0}, {1 << 16, 1}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int k = 0; k < 2; k++) {
            uint8_t ref[6 * 3 * 4], out[sizeof(ref)];
            memset(ref, 0x5A, sizeof(ref)), memset(out, 0x5A, sizeof(out));
            shr_surface sr = packed(ref, FMTS[k], 6, 3), so = packed(out, FMTS[k], 6, 3);
            shr_draw_cmd c[8];
            size_t n = group(c, 9, (shr_rect){0, 1, 6, 3}, (shr_rect){1, 1, 5, 2}, FMTS[k]);
            ASSERT_EQ_LL(exec(&sr, c, n), SHR_OK);
            f.budget = -1;
            shr_framebuffer_driver drv;
            ASSERT_EQ_LL(shr_software_driver_create(&al, cases[i].budget, NBUF, &drv), SHR_OK);
            f.budget = cases[i].alloc_budget;
            ASSERT_EQ_LL(drv.execute(drv.user, &so, c, n, 1), SHR_OK);
            ASSERT_MEM_EQ(ref, out, sizeof(ref));
            c[3].color = SHR_RGB(1, 2, 3); /* nothing was stored: the next draw shows the new content */
            ASSERT_EQ_LL(exec(&sr, c, n), SHR_OK);
            ASSERT_EQ_LL(drv.execute(drv.user, &so, c, n, 2), SHR_OK);
            ASSERT_MEM_EQ(ref, out, sizeof(ref));
            f.budget = -1;
            ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
            ASSERT_EQ_LL(f.live, 0);
        }
    }
    PASS();
}

TEST driver_create_and_destroy(void) {
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, NBUF, NULL), SHR_E_INVALID_ARG);
    shr_allocator half = {NULL, fa_alloc, NULL};
    ASSERT_EQ_LL(shr_software_driver_create(&half, 0, NBUF, &drv), SHR_E_INVALID_ARG);
    ASSERT(!drv.execute && !drv.user);
    fail_alloc f = {0, 0};
    shr_allocator al = fail_allocator(&f);
    for (long budget = 0; budget < 2; budget++) { /* the driver, its buffer table */
        f.budget = budget;
        ASSERT_EQ_LL(shr_software_driver_create(&al, 0, NBUF, &drv), SHR_E_NO_MEMORY);
        ASSERT_EQ_LL(f.live, 0);
        ASSERT(!drv.execute);
    }
    f.budget = 2; /* the hash table comes with the first entry */
    ASSERT_EQ_LL(shr_software_driver_create(&al, 1024, NBUF, &drv), SHR_OK);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    f.budget = -1;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 1 << 16, NBUF, &drv), SHR_OK);
    ASSERT_EQ_LL(drv.caps.domains, SHR_MEMORY_CPU | SHR_MEMORY_DMA);
    ASSERT(drv.execute && drv.reset && !drv.cancel && !drv.sync);
    ASSERT_EQ_LL(drv.reset(drv.user), SHR_OK);
    uint8_t buf[4 * 4];
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, 4, 1);
    shr_draw_cmd c[4] = {fill((shr_rect){0, 0, 4, 1}, SHR_RGB(1, 1, 1)), begin(1, (shr_rect){0, 0, 2, 1}, (shr_rect){0, 0, 2, 1}),
                         fill((shr_rect){0, 0, 2, 1}, SHR_RGB(2, 2, 2)), end()};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 4, 1), SHR_OK);
    ASSERT_EQ_LL(raw(&s, 1, 0), 0x020202FF);
    ASSERT_EQ_LL(raw(&s, 2, 0), 0x010101FF);
    ASSERT(f.live > 1);

    shr_framebuffer_driver other;
    shr_framebuffer_driver_init(&other);
    ASSERT_EQ_LL(shr_software_driver_destroy(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_driver_destroy(&other), SHR_E_INVALID_ARG);
    shr_framebuffer_driver no_user = drv;
    no_user.user = NULL;
    ASSERT_EQ_LL(shr_software_driver_destroy(&no_user), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    ASSERT(!drv.execute && !drv.user);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 0, &drv), SHR_OK); /* default allocator, no cache, no buffers */
    ASSERT_EQ_LL(drv.caps.max_buffers, 0);
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 4, 1), SHR_OK);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

typedef struct reader {
    shr_asset_source *src;
    uint64_t offset;
    int bad;
} reader;

static void *read_loop(void *arg) {
    reader *r = arg;
    char got[3];
    for (int i = 0; i < 2000; i++) {
        uint64_t off = r->offset + (uint64_t)(i % 4);
        if (r->src->read(r->src->user, off, 3, got, 0) != SHR_OK || memcmp(got, &"0123456789"[i % 4], 3)) r->bad++;
    }
    return NULL;
}

TEST asset_source_file_reads_and_closes(void) {
    char dir[] = "/tmp/shr-test-XXXXXX", path[64], fifo[64], big[64];
    ASSERT(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/data", dir);
    snprintf(fifo, sizeof(fifo), "%s/fifo", dir);
    snprintf(big, sizeof(big), "%s/big", dir);
    FILE *f = fopen(path, "wb");
    ASSERT(f && fwrite("abcdef", 1, 6, f) == 6);
    fclose(f);
    shr_asset_source src;
    ASSERT_EQ_LL(shr_asset_source_file(NULL, &src), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_asset_source_file(path, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_asset_source_file("/nonexistent/shiroko", &src), SHR_E_NOT_FOUND);
    char below_file[80], long_name[PATH_MAX + 16];
    snprintf(below_file, sizeof(below_file), "%s/x", path);
    ASSERT_EQ_LL(shr_asset_source_file(below_file, &src), SHR_E_NOT_FOUND); /* ENOTDIR */
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    ASSERT_EQ_LL(shr_asset_source_file(long_name, &src), SHR_E_IO); /* ENAMETOOLONG */
    ASSERT_EQ_LL(shr_asset_source_file(dir, &src), SHR_E_IO); /* not a regular file */
    ASSERT_EQ_LL(mkfifo(fifo, 0600), 0);
    ASSERT_EQ_LL(shr_asset_source_file(fifo, &src), SHR_E_IO); /* no writer: must not block either */

    ASSERT_EQ_LL(shr_asset_source_file(path, &src), SHR_OK);
    ASSERT_EQ_LL(src.size, 6);
    ASSERT(!src.data && src.read && src.close && !src.cancel);
    char got[4] = {0};
    ASSERT_EQ_LL(src.read(src.user, 2, 3, got, 0), SHR_OK);
    ASSERT_MEM_EQ("cde", got, 3);
    ASSERT_EQ_LL(src.read(src.user, 0, 0, got, 7), SHR_OK);
    ASSERT_EQ_LL(src.read(src.user, 4, 3, got, 0), SHR_E_IO); /* short read */
    ASSERT_EQ_LL(src.read(src.user, 6, 1, got, 0), SHR_E_IO);  /* past the end */
    ASSERT_EQ_LL(src.read(src.user, (uint64_t)INT64_MAX, 1, got, 0), SHR_E_IO);
    ASSERT_EQ_LL(src.read(src.user, UINT64_MAX, 1, got, 0), SHR_E_IO);
    close((int)(intptr_t)src.user); /* `user` is the descriptor: a read that fails */
    ASSERT_EQ_LL(src.read(src.user, 0, 1, got, 0), SHR_E_IO);

    /* Offsets past 4 GiB (a sparse file), read from two threads at once. */
    const uint64_t far = (5ull << 30) + 10;
    int fd = open(big, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT(fd >= 0);
    ASSERT_EQ_LL(pwrite(fd, "0123456789", 10, 0), 10);
    ASSERT_EQ_LL(pwrite(fd, "0123456789", 10, (off_t)far), 10);
    close(fd);
    ASSERT_EQ_LL(shr_asset_source_file(big, &src), SHR_OK);
    ASSERT_EQ_LL(src.size == far + 10, 1);
    ASSERT_EQ_LL(src.read(src.user, far + 7, 3, got, 0), SHR_OK);
    ASSERT_MEM_EQ("789", got, 3);
    reader rs[2] = {{&src, 0, 0}, {&src, far, 0}};
    pthread_t t[2];
    for (int i = 0; i < 2; i++) ASSERT_EQ_LL(pthread_create(&t[i], NULL, read_loop, &rs[i]), 0);
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
    ASSERT_EQ_LL(rs[0].bad + rs[1].bad, 0);
    src.close(src.user);
    unlink(big);
    unlink(fifo);
    unlink(path);
    rmdir(dir);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(encoding_anchors);
    RUN_TEST(fill_is_opaque_and_respects_stride);
    RUN_TEST(fill_dim_blends_at_half_strength);
    RUN_TEST(glyph_a8_blends_every_coverage);
    RUN_TEST(rgb565_blends_round_once_for_every_input);
    RUN_TEST(glyph_a4_nibbles_origin_and_dim);
    RUN_TEST(glyph_a8_dim_keeps_faint_coverage);
    RUN_TEST(synth_hand_vectors);
    RUN_TEST(synth_matches_the_spec_reference);
    RUN_TEST(synth_is_clip_invariant);
    RUN_TEST(synth_sources_stay_in_the_footprint);
    RUN_TEST(synth_size_and_axis_limits);
    RUN_TEST(synth_edge_sources);
    RUN_TEST(image_is_straight_alpha_source_over);
    RUN_TEST(copy_converts_between_formats);
    RUN_TEST(copy_scrolls_within_one_surface);
    RUN_TEST(copy_overlap_rules);
    RUN_TEST(rotate_maps_every_rotation);
    RUN_TEST(rotate_rejects_bad_geometry_and_sources);
    RUN_TEST(check_uses_the_driver_reach);
    RUN_TEST(buffers_register_replace_and_release);
    RUN_TEST(buffer_commands_are_validated);
    RUN_TEST(buffer_regions_are_validated);
    RUN_TEST(execute_rejects_bad_batches_without_drawing);
    RUN_TEST(cache_hints_are_validated);
    RUN_TEST(execute_clips_groups_to_cache_clip);
    RUN_TEST(cached_driver_hit_matches_direct_drawing);
    RUN_TEST(cached_entries_depend_on_format);
    RUN_TEST(cache_evicts_least_recently_used);
    RUN_TEST(cache_evicts_only_after_a_new_entry_exists);
    RUN_TEST(cache_skips_groups_over_half_the_budget);
    RUN_TEST(cache_budget_includes_the_table);
    RUN_TEST(small_budgets_do_not_store_and_evict);
    RUN_TEST(cache_table_failures_draw_directly);
    RUN_TEST(cache_table_grows_with_many_entries);
    RUN_TEST(cache_keys_are_128_bit);
    RUN_TEST(cache_keeps_sizes_and_formats_of_one_key_apart);
    RUN_TEST(cache_evicts_oldest_of_many);
    RUN_TEST(uncacheable_groups_draw_directly);
    RUN_TEST(driver_create_and_destroy);
    RUN_TEST(asset_source_file_reads_and_closes);
    GREATEST_MAIN_END();
}
