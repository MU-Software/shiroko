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
#include "synth.h"

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
    c.kind = (uint8_t)kind;
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
    if (kind == SHR_CMD_BUFFER_REGISTER) c.src = img_ref(bufs[id - 1]);
    return c;
}

static shr_draw_cmd reg(uint32_t id) { return buf_cmd(SHR_CMD_BUFFER_REGISTER, id); }

/* GLYPH and IMAGE draw all of `src` as a buffer of its own; COPY and ROTATE take it as their surface. */
static shr_draw_cmd with_src(shr_cmd_kind kind, shr_rect r, shr_image src, shr_point origin) {
    shr_draw_cmd c = cmd(kind, r);
    if (kind == SHR_CMD_GLYPH || kind == SHR_CMD_IMAGE)
        c.buffer = use(src), c.src_rect = (shr_rect){0, 0, src.width, src.height};
    else
        c.src = img_ref(src);
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
    c.rotation = (uint8_t)rot;
    return c;
}

static shr_draw_cmd kbegin(uint32_t id, shr_rect dst) {
    shr_draw_cmd c = cmd(SHR_CMD_KEEP_BEGIN, dst);
    c.buffer = id;
    return c;
}

static shr_draw_cmd kend(void) { return cmd(SHR_CMD_KEEP_END, (shr_rect){0, 0, 0, 0}); }

static shr_draw_cmd kdraw(uint32_t id, shr_rect dst, shr_point origin) {
    shr_draw_cmd c = cmd(SHR_CMD_KEEP_DRAW, dst);
    c.buffer = id, c.src_origin = origin;
    return c;
}

static shr_draw_cmd krelease(uint32_t id) {
    shr_draw_cmd c = cmd(SHR_CMD_KEEP_RELEASE, (shr_rect){0, 0, 0, 0});
    c.buffer = id;
    return c;
}

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

/* FILL rows of every width up to 21 from every 4-byte alignment, in buffers at odd addresses with odd strides: each
 * pixel takes the colour and the bytes around stay. */
TEST fill_rows_of_any_width_and_alignment(void) {
    enum { SW = 26, H = 2, ROW = SW * 4 + 2 };
    static uint8_t buf[1 + ROW * H], want[sizeof(buf)];
    for (int i = 0; i < 2; i++)
        for (size_t off = 0; off < 2; off++)
            for (size_t pad = 0; pad < 3; pad++)
                for (int32_t x0 = 0; x0 < 4; x0++)
                    for (int32_t w = 1; w <= 21; w++) {
                        memset(buf, 0xAB, sizeof(buf)), memset(want, 0xAB, sizeof(want));
                        shr_surface s = surf(buf + off, FMTS[i], SW, H, SW * bpp(FMTS[i]) + pad);
                        shr_surface ws = surf(want + off, FMTS[i], SW, H, s.stride);
                        for (int32_t y = 0; y < H; y++)
                            for (int32_t x = x0; x < x0 + w; x++) put(&ws, x, y, enc(FMTS[i], 0x2468AC));
                        shr_draw_cmd c = fill((shr_rect){x0, 0, x0 + w, H}, 0xFF2468AC);
                        ASSERT_EQ_LL(exec(&s, &c, 1), SHR_OK);
                        ASSERT_MEM_EQ(want, buf, sizeof(buf));
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

/* Coverage runs of 0, 255 and partial values at every 4-pixel alignment, A8 and A4 from even and odd columns, rows
 * across a 64-pixel span, with and without DIM and ON_FILL, against the reference blend: the loops that skip zero and
 * store full coverage (SHR_SCALAR_BLEND) give the same bytes as blending every pixel. */
TEST glyph_coverage_runs_blend_exactly(void) {
    enum { W = 72, H = 8, SW = W + 4 };
    static const int32_t widths[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 13, 63, 64, 65, W};
    static const uint32_t colors[][2] = {{0xFF8007, 0x0A3CFA}, {0xFFFFFF, 0x000000}, {0x000000, 0xFFFFFF}};
    static uint8_t a8[W * H], a4[(W / 2 + 1) * H], buf[SW * H * 4];
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W;) {
            uint32_t v = rnd(), run = y < H / 2 ? 1 : 1 + v % 9, kind = v >> 4 & 3;
            for (; run && x < W; run--, x++)
                a8[y * W + x] = (uint8_t)(y < H / 2 ? (uint32_t)(y * W + x) : kind == 0 ? 0 : kind == 1 ? 255 : v >> 8);
        }
    for (int32_t x = 0; x < W; x++) a8[(H - 1) * W + x] = (uint8_t)(x % 5 ? 0 : 17 * (x / 5 % 3) + 1); /* faint dots */
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x += 2)
            a4[y * (W / 2 + 1) + x / 2] = (uint8_t)(a8[y * W + x] / 17 << 4 | a8[y * W + x + 1] / 17);
    const shr_image src[2] = {{a8, W, H, W, sizeof(a8), SHR_FORMAT_A8, 0},
                              {a4, W, H, W / 2 + 1, sizeof(a4), SHR_FORMAT_A4, 0}};
    for (int sf = 0; sf < 2; sf++)
        for (int fi = 0; fi < 2; fi++)
            for (uint32_t flags = 0; flags < 2; flags++)
                for (int on = 0; on < 2; on++)
                    for (size_t ci = 0; ci < sizeof(colors) / sizeof(colors[0]); ci++)
                        for (int32_t sx = 0; sx < 6; sx++)
                            for (size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
                                int32_t w = widths[wi] < W - sx ? widths[wi] : W - sx, ox = sx & 3;
                                shr_surface s = packed(buf, FMTS[fi], SW, H);
                                uint32_t fg = colors[ci][0], bg = enc(FMTS[fi], colors[ci][1]);
                                shr_draw_cmd c[2] = {fill((shr_rect){0, 0, SW, H}, colors[ci][1]),
                                                     glyph((shr_rect){ox, 0, ox + w, H}, fg, src[sf],
                                                           (shr_point){sx, 0})};
                                c[1].flags = (uint16_t)(flags | (on ? SHR_GLYPH_ON_FILL : 0)), c[1].bg = colors[ci][1];
                                ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
                                for (int32_t y = 0; y < H; y++)
                                    for (int32_t x = 0; x < SW; x++) {
                                        uint32_t a = x < ox || x >= ox + w ? 0 : ref_in(&src[sf], sx + x - ox, y);
                                        ASSERT_EQ_LL(raw(&s, x, y), mix(FMTS[fi], fg, bg, flags ? (a + 1) >> 1 : a));
                                    }
                            }
    PASS();
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
        c[1].flags = (uint16_t)v[i].flags, c[1].slant_axis = v[i].axis;
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
                            c[1].flags = (uint16_t)flags, c[1].slant_axis = axis;
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

/* Rows wider than the driver's 64-pixel spans. */
TEST synth_wide_rows_match_the_spec_reference(void) {
    enum { W = 150, H = 3, SW = W + 8 };
    static const shr_pixel_format srcs[2] = {SHR_FORMAT_A8, SHR_FORMAT_A4};
    static uint8_t px[W * H], eb[(W + 4) * (H + 2)], buf[SW * H * 4];
    uint32_t want[64];
    for (int sf = 0; sf < 2; sf++) {
        coverage_noise(px, sizeof(px));
        size_t stride = sf ? (W + 1) / 2 : W;
        shr_image m = {px, W, H, stride, stride * H, srcs[sf], 0};
        shr_rect rect;
        shr_image b = embed(&m, eb, &rect);
        for (uint32_t flags = 1; flags < 8; flags++) {
            int32_t x0, x1;
            shr__glyph_footprint(W, H, flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC), 1, &x0, &x1);
            int32_t fw = x1 - x0;
            shr_surface s = packed(buf, FMTS[sf], fw, H);
            shr_draw_cmd c[2] = {fill((shr_rect){0, 0, fw, H}, 0),
                                 region(SHR_CMD_GLYPH, (shr_rect){0, 0, fw, H}, b, rect, (shr_point){x0, 0})};
            c[1].flags = (uint16_t)flags, c[1].slant_axis = 1;
            ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
            for (int32_t y = 0; y < H; y++)
                for (int32_t x = 0; x < fw; x += 64) {
                    int32_t n = fw - x < 64 ? fw - x : 64;
                    ref_row(&m, flags, 1, y, x0 + x, n, want);
                    for (int32_t i = 0; i < n; i++)
                        ASSERT_EQ_LL(raw(&s, x + i, y), mix(FMTS[sf], 0xFFFFFF, enc(FMTS[sf], 0), want[i]));
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
        /* Moved by an origin and limited to a clip, as a keep is stored. */
        uint8_t win[5 * 4 * 4];
        shr_surface ws = packed(win, FMTS[fi], 5, 4);
        for (int32_t y = 0; y < 4; y++) memcpy(win + (size_t)y * ws.stride, init + (size_t)(y + 2) * rs.stride + 3 * bpp(FMTS[fi]), ws.stride);
        shr__raster_draw(&ws, &whole, bufs, (shr_point){3, 2}, (shr_rect){0, 0, 5, 4}, NULL);
        for (int32_t y = 0; y < 4; y++)
            for (int32_t x = 0; x < 5; x++) ASSERT_EQ_LL(raw(&ws, x, y), raw(&rs, x + 3, y + 2));
        /* Stored in a keep, then drawn from it in a clip. */
        shr_rect clip = {3, 2, 9, 6};
        shr_draw_cmd g[6] = {reg(whole.buffer), kbegin(1, (shr_rect){0, 0, SW, SH}),
                             fill((shr_rect){0, 0, SW, SH}, SHR_RGB(9, 40, 90)), whole, kend(),
                             kdraw(1, clip, (shr_point){clip.x0, clip.y0})};
        memcpy(ref, init, sizeof(init));
        ASSERT_EQ_LL(exec(&rs, g + 2, 2), SHR_OK);
        shr_framebuffer_driver drv;
        ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 1, NBUF, &drv), SHR_OK);
        for (int pass = 0; pass < 2; pass++) {
            memcpy(out, init, sizeof(init));
            ASSERT_EQ_LL(pass ? drv.execute(drv.user, &os, g + 5, 1, 2) : drv.execute(drv.user, &os, g, 6, 1), SHR_OK);
            for (int32_t y = 0; y < SH; y++)
                for (int32_t x = 0; x < SW; x++) {
                    bool in = x >= clip.x0 && x < clip.x1 && y >= clip.y0 && y < clip.y1;
                    ASSERT_EQ_LL(raw(&os, x, y), in ? raw(&rs, x, y) : raw(&(shr_surface){init, SW, SH, os.stride, 0, FMTS[fi], 0, 0, 0}, x, y));
                }
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
                c[1].flags = (uint16_t)styles[i], c[1].slant_axis = axes[a];
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
        c.flags = (uint16_t)lim[k].flags, c.slant_axis = axis;
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

/* IMAGE pixels of alpha 0, 255 and between in runs, rows of widths up to 13 and the whole row from every source and
 * destination column mod 4: groups of 4 all 0 or all 255 give the bytes blending gives. */
TEST image_alpha_runs_blend_exactly(void) {
    enum { W = 44, SW = W + 4 };
    static uint8_t rgba[W * 4], buf[SW * 4];
    for (int32_t x = 0; x < W;) {
        uint32_t v = rnd(), run = x < 16 ? 8 : 1 + v % 6, kind = x < 16 ? (uint32_t)x / 8 : v >> 4 & 3;
        for (; run && x < W; run--, x++) {
            uint8_t *q = rgba + 4 * x;
            q[0] = (uint8_t)rnd(), q[1] = (uint8_t)rnd(), q[2] = (uint8_t)rnd();
            q[3] = (uint8_t)(kind == 0 ? 0 : kind == 1 ? 255 : 1 + (v >> 8) % 254);
        }
    }
    shr_image m = {rgba, W, 1, sizeof(rgba), sizeof(rgba), SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU};
    for (int i = 0; i < 2; i++)
        for (int32_t sx = 0; sx < 4; sx++)
            for (int32_t ox = 0; ox < 4; ox++)
                for (int32_t w = 1; w <= 14; w++) {
                    int32_t n = w == 14 ? W - sx : w;
                    shr_surface s = packed(buf, FMTS[i], SW, 1);
                    shr_draw_cmd c[2] = {fill((shr_rect){0, 0, SW, 1}, 0x3C5A96),
                                         region(SHR_CMD_IMAGE, (shr_rect){ox, 0, ox + n, 1}, m, (shr_rect){0, 0, W, 1},
                                                (shr_point){sx, 0})};
                    ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
                    uint32_t bg = enc(FMTS[i], 0x3C5A96);
                    for (int32_t x = 0; x < SW; x++) {
                        bool in = x >= ox && x < ox + n;
                        const uint8_t *q = in ? rgba + 4 * (sx + x - ox) : (const uint8_t[4]){0};
                        ASSERT_EQ_LL(raw(&s, x, 0), mix(FMTS[i], (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2], bg, q[3]));
                    }
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

/* Down by k rows in runs of k from the bottom, also with a shorter last run, as one forward memmove would not do. */
TEST copy_down_in_runs(void) {
    for (int i = 0; i < 2; i++)
        for (int32_t k = 2; k <= 3; k++) {
            uint8_t buf[2 * 8 * 4];
            shr_surface s = packed(buf, FMTS[i], 2, 8);
            for (int32_t y = 0; y < 8; y++) put(&s, 0, y, enc(FMTS[i], (uint32_t)(y + 1) * 20));
            shr_draw_cmd c = with_src(SHR_CMD_COPY, (shr_rect){0, k, 1, 8}, as_image(&s), (shr_point){0, 0});
            ASSERT_EQ_LL(exec(&s, &c, 1), SHR_OK);
            for (int32_t y = k; y < 8; y++) ASSERT_EQ_LL(raw(&s, 0, y), enc(FMTS[i], (uint32_t)(y - k + 1) * 20));
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
    cp.src = img_ref((shr_image){buf + 4, 2, 2, 12, 24, SHR_FORMAT_RGBX8888, SHR_MEMORY_CPU});
    ASSERT_EQ_LL(exec(&x, &cp, 1), SHR_E_UNSUPPORTED);
    cp.src = img_ref((shr_image){buf + 64, 2, 2, 8, 16, SHR_FORMAT_RGBX8888, SHR_MEMORY_DEVICE});
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
            r = rotate(&s, (shr_rect){1, 0, 3, 2}, SHR_ROTATE_180); /* columns 1..2 of the source into part of e */
            size_t bpp = FMTS[i] == SHR_FORMAT_RGB565 ? 2 : 4;
            r.src.pixels = (const uint8_t *)r.src.pixels + bpp, r.src.width = 2;
            ASSERT_EQ_LL(exec(&e, &r, 1), SHR_OK);
            for (int y = 0; y < 2; y++) {
                ASSERT_EQ_LL(raw(&e, 0, y), 0);
                for (int x = 1; x < 3; x++) ASSERT_EQ_LL(raw(&e, x, y), SRC(3 - x, 1 - y));
            }
            uint8_t big[3 * 4 * 4] = {0};
            shr_surface g = packed(big, FMTS[k], 3, 4);
            r = rotate(&s, (shr_rect){1, 1, 3, 4}, SHR_ROTATE_90_CW); /* away from the origin */
            ASSERT_EQ_LL(exec(&g, &r, 1), SHR_OK);
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 3; x++) ASSERT_EQ_LL(raw(&g, 2 - y, 1 + x), SRC(x, y));
            ASSERT_EQ_LL(raw(&g, 0, 0), 0);
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
    r.src = img_ref((shr_image){a8, 3, 2, 3, 6, SHR_FORMAT_A8, SHR_MEMORY_CPU});
    ASSERT_EQ_LL(exec(&d, &r, 1), SHR_E_UNSUPPORTED);
    const shr_rotation bad_rot[2] = {SHR_ROTATE_NONE, (shr_rotation)7};
    for (int i = 0; i < 2; i++) {
        r = rotate(&s, all, bad_rot[i]);
        ASSERT_EQ_LL(exec(&d, &r, 1), SHR_E_INVALID_ARG);
    }
    r = rotate(&s, all, SHR_ROTATE_90_CW); /* needs a 2x3 rect */
    ASSERT_EQ_LL(exec(&d, &r, 1), SHR_E_INVALID_ARG);
    r = rotate(&s, (shr_rect){0, 0, 2, 2}, SHR_ROTATE_180);
    ASSERT_EQ_LL(exec(&d, &r, 1), SHR_E_INVALID_ARG);
    r.dst = (shr_rect){0, 0, 3, 1};
    ASSERT_EQ_LL(exec(&d, &r, 1), SHR_E_INVALID_ARG);
    r.dst = all;
    ASSERT_EQ_LL(exec(&s, &r, 1), SHR_E_UNSUPPORTED); /* in place */
    r.src.width = 0, r.dst = (shr_rect){1, 0, 1, 2}; /* an empty source turns into an empty rect */
    ASSERT_EQ_LL(exec(&d, &r, 1), SHR_OK);
    ASSERT_MEM_EQ(zero, dst, sizeof(zero));
    /* An offset view of the same pixels overlaps; a disjoint one is fine. */
    uint16_t px[16] = {0};
    shr_surface a = packed(px, SHR_FORMAT_RGB565, 2, 2), b = packed(px + 2, SHR_FORMAT_RGB565, 2, 2);
    r = rotate(&a, (shr_rect){0, 0, 2, 2}, SHR_ROTATE_180);
    ASSERT_EQ_LL(exec(&b, &r, 1), SHR_E_UNSUPPORTED);
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
    ASSERT_EQ_LL(shr__raster_check(&dev, &fill, 1, NULL, 0, NULL, NULL, NULL), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr__raster_check(&dev, &fill, 1, NULL, 0, NULL, device_reach, &handle), SHR_OK);
    ASSERT_EQ_LL(shr__raster_check(&dev, &fill, 1, NULL, 0, NULL, device_reach, &unknown), SHR_E_UNSUPPORTED);
    /* DEVICE buffers overlap when they are the same one, never with CPU memory. */
    shr_draw_cmd copy = {.kind = SHR_CMD_COPY, .dst = r, .src = img_ref(self), .src_origin = {1, 1}};
    ASSERT_EQ_LL(shr__raster_check(&dev, &copy, 1, NULL, 0, NULL, device_reach, &handle), SHR_OK);
    copy.src = img_ref(wide);
    ASSERT_EQ_LL(shr__raster_check(&dev, &copy, 1, NULL, 0, NULL, device_reach, &handle), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr__raster_check(&cpu, &copy, 1, NULL, 0, NULL, device_reach, &handle), SHR_OK);
    copy.src = img_ref(gone);
    ASSERT_EQ_LL(shr__raster_check(&cpu, &copy, 1, NULL, 0, NULL, device_reach, &handle), SHR_E_UNSUPPORTED);
    shr_draw_cmd rot = {.kind = SHR_CMD_ROTATE, .dst = {0, 0, 16, 8}, .src = img_ref(self), .rotation = SHR_ROTATE_180};
    ASSERT_EQ_LL(shr__raster_check(&dev, &rot, 1, NULL, 0, NULL, device_reach, &handle), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr__raster_check(&cpu, &rot, 1, NULL, 0, NULL, device_reach, &handle), SHR_OK);
    rot.src = img_ref(gone);
    ASSERT_EQ_LL(shr__raster_check(&cpu, &rot, 1, NULL, 0, NULL, device_reach, &handle), SHR_E_UNSUPPORTED);
    shr__keep k[1] = {{0}};
    uint32_t stores[1];
    shr__keeps keeps = {k, stores, 1, 0, 0, 0};
    shr_draw_cmd group[] = {{.kind = SHR_CMD_KEEP_BEGIN, .dst = {0, 0, 8, 8}, .buffer = 1},
                            {.kind = SHR_CMD_COPY, .dst = r, .src = img_ref(self), .src_origin = {8, 0}},
                            {.kind = SHR_CMD_KEEP_END}};
    ASSERT_EQ_LL(shr__raster_check(&dev, group, 3, NULL, 0, &keeps, device_reach, &handle), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__raster_check(&cpu, group, 3, NULL, 0, &keeps, device_reach, &handle), SHR_OK);
    group[1].src = img_ref(as_image(&cpu));
    ASSERT_EQ_LL(shr__raster_check(&dev, group, 3, NULL, 0, &keeps, device_reach, &handle), SHR_OK);
    ASSERT(keeps.nstores == 1 && stores[0] == 1 && k[0].store_w == 8 && keeps.batch == 3);
    ASSERT_EQ_LL(shr__raster_reads_dst(&cpu, &rot.src), false);
    /* What a source spans: its rows, or its first byte alone when it is empty. */
    shr_image_ref part = {px + 16 * 8, 4, 0, 32, SHR_FORMAT_RGB565, 0, 0};
    ASSERT_EQ_LL(shr__raster_reads_dst(&cpu, &part), false);
    part.pixels = px + 16 * 2, part.width = 0, part.height = 4;
    ASSERT_EQ_LL(shr__raster_reads_dst(&cpu, &part), true);
    part.pixels = px + 16 * 8 - 1, part.width = 1, part.height = 1;
    ASSERT_EQ_LL(shr__raster_reads_dst(&cpu, &part), true);
    /* REGISTER asks the same reach. */
    shr_image tbl[1] = {{0}};
    shr_draw_cmd r1 = {.kind = SHR_CMD_BUFFER_REGISTER, .buffer = 1, .src = img_ref(self)};
    r1.src.format = SHR_FORMAT_A8;
    shr_image mem;
    ASSERT_EQ_LL(shr__raster_buffer_check(&r1, tbl, 1, NULL, NULL, &mem), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr__raster_buffer_check(&r1, tbl, 1, device_reach, &handle, &mem), SHR_OK);
    ASSERT(mem.pixels == &handle && mem.format == SHR_FORMAT_A8 && !mem.byte_length);
    ASSERT_EQ_LL(shr__raster_buffer_check(&r1, tbl, 1, device_reach, &unknown, &mem), SHR_E_UNSUPPORTED);
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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 0, 3, &drv), SHR_OK);
    ASSERT_EQ_LL(drv.caps.max_buffers, 3);
    ASSERT(!drv.caps.max_buffer_width && !drv.caps.max_buffer_height && !drv.caps.buffer_bytes && !drv.caps.buffer_flags);
    ASSERT(!drv.caps.max_keeps && !drv.caps.keep_bytes);
    ASSERT_EQ_LL(drv.caps.flags, SHR_DRIVER_CHEAP_MOVE);
    shr_draw_cmd img = cmd(SHR_CMD_IMAGE, (shr_rect){0, 0, 1, 1});
    img.buffer = 3, img.src_rect = (shr_rect){0, 0, 1, 1};
    shr_draw_cmd c[3] = {cmd(SHR_CMD_BUFFER_REGISTER, (shr_rect){0, 0, 0, 0}), img};
    c[0].buffer = 3, c[0].src = img_ref(r);
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 2, 1), SHR_OK);
    ASSERT_EQ_LL(at(&s, 0), 0xFF0000);
    /* Registrations last across batches, and the driver reads the memory in place. */
    red[1] = 128;
    img.dst = (shr_rect){1, 0, 2, 1};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, &img, 1, 2), SHR_OK);
    ASSERT_EQ_LL(at(&s, 1), 0xFF8000);
    /* REGISTER replaces; UPDATE inside the buffer is accepted; RELEASE of ids naming nothing is no error. */
    c[0].src = img_ref(b);
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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 0, 2, &drv), SHR_OK);
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
        c[0].buffer = regs[i].id, c[0].src = img_ref(regs[i].mem);
        memset(px, 0, sizeof(px));
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 2, 1), regs[i].want);
        if (regs[i].want != SHR_OK) ASSERT_MEM_EQ(zero, px, sizeof(px));
    }
    c[0].buffer = 1, c[0].src = img_ref(ok);
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
    late[1].src = img_ref(ok);
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

/* A keep driver with ids 1..2, keep 1 holding 4 x 2 pixels of `s` (RGBX8888, 4 x 4). */
static void keep_driver(shr_framebuffer_driver *drv, const shr_surface *s) {
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 2, NBUF, drv), SHR_OK);
    ASSERT_EQ_LL(drv->caps.max_keeps, 2);
    ASSERT_EQ_LL(drv->caps.keep_bytes | drv->caps.max_keep_bytes, 0);
    shr_rect g = {0, 0, 4, 2};
    shr_draw_cmd store[3] = {kbegin(1, g), fill(g, SHR_RGB(7, 7, 7)), kend()};
    ASSERT_EQ_LL(drv->execute(drv->user, s, store, 3, 1), SHR_OK);
}

TEST keep_commands_are_validated(void) {
    uint8_t buf[4 * 4 * 4] = {0}, zero[sizeof(buf)] = {0};
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, 4, 4);
    shr_rect g = {0, 0, 4, 2}, one = {0, 0, 1, 1};
    shr_draw_cmd f = fill(g, SHR_RGB(1, 2, 3));
    uint16_t px[4] = {0};
    shr_surface rs = packed(px, SHR_FORMAT_RGBX8888, 1, 1);
    const shr_draw_cmd bad[][6] = {
        {f, kbegin(2, g), kbegin(1, g), kend(), kend(), f},                    /* nested */
        {f, kend(), f, f, f, f},                                               /* END without BEGIN */
        {f, kbegin(2, g), f, f, f, f},                                         /* never ended */
        {f, kbegin(2, (shr_rect){0, 0, (1 << 24) + 1, 2}), f, kend(), f, f},   /* group too far outside */
        {f, kbegin(2, (shr_rect){2, 0, 1, 1}), kend(), f, f, f},               /* inverted group */
        {f, kbegin(2, g), fill((shr_rect){0, 0, 4, 3}, 0), kend(), f, f},      /* command outside the group */
        {f, kbegin(2, one), rotate(&rs, one, SHR_ROTATE_180), kend(), f, f},
        {f, kbegin(2, g), with_src(SHR_CMD_COPY, (shr_rect){0, 0, 4, 1}, as_image(&s), (shr_point){0, 1}), kend(), f, f},
        {f, kbegin(2, g), kdraw(1, one, (shr_point){0, 0}), kend(), f, f},    /* keep command in a group */
        {f, krelease(2), f, f, f, f},                                          /* KEEP_RELEASE after a draw */
        {kbegin(2, g), krelease(2), f, kend(), f, f},
        {kbegin(2, g), buf_cmd(SHR_CMD_BUFFER_RELEASE, 1), f, kend(), f, f},   /* buffer command in a group */
        {f, kbegin(0, g), f, kend(), f, f},                                    /* ids 1..max_keeps */
        {f, kbegin(3, g), f, kend(), f, f},
        {f, kdraw(0, one, (shr_point){0, 0}), f, f, f, f},
        {f, kdraw(3, one, (shr_point){0, 0}), f, f, f, f},
        {kbegin(2, g), f, kend(), kbegin(2, g), f, kend()},                    /* stored twice */
        {kdraw(1, one, (shr_point){0, 0}), kbegin(1, g), f, kend(), f, f},     /* drawn before it is stored */
        {f, kdraw(2, one, (shr_point){0, 0}), f, f, f, f},                     /* holds nothing */
        {f, kdraw(1, (shr_rect){0, 0, 4, 1}, (shr_point){1, 0}), f, f, f, f}, /* outside the keep */
        {f, kdraw(1, (shr_rect){0, 0, 1, 3}, (shr_point){0, 0}), f, f, f, f},
        {f, kdraw(1, one, (shr_point){-1, 0}), f, f, f, f},
        {f, kdraw(1, one, (shr_point){0, -1}), f, f, f, f},
        {f, kdraw(1, (shr_rect){3, 3, 5, 4}, (shr_point){0, 0}), f, f, f, f}, /* outside the destination */
        {kbegin(2, (shr_rect){0, 0, 1, 1}), kend(), kdraw(2, (shr_rect){0, 0, 2, 1}, (shr_point){0, 0}), f, f, f},
    };
    shr_framebuffer_driver drv;
    keep_driver(&drv, &s);
    memset(buf, 0, sizeof(buf));
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERT_EQ_LL(exec(&s, bad[i], 6), SHR_E_INVALID_ARG);
        ASSERT_EQ_LL(drv.execute(drv.user, &s, bad[i], 6, 1), SHR_E_INVALID_ARG);
    }
    ASSERT_MEM_EQ(zero, buf, sizeof(buf));
    /* The keep survived the rejected batches, which touched no keep (id 1 was stored only in the first). */
    shr_draw_cmd ok[2] = {kdraw(1, (shr_rect){1, 1, 4, 3}, (shr_point){1, 0}), kdraw(1, (shr_rect){0, 3, 1, 4}, (shr_point){3, 1})};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, ok, 2, 1), SHR_OK);
    ASSERT(raw(&s, 1, 1) == 0x070707FF && raw(&s, 3, 2) == 0x070707FF && raw(&s, 0, 3) == 0x070707FF && raw(&s, 0, 0) == 0);
    /* Stored keeps take the destination format: another format cannot draw them. */
    uint16_t other[4 * 4];
    shr_surface s565 = packed(other, SHR_FORMAT_RGB565, 4, 4);
    ASSERT_EQ_LL(drv.execute(drv.user, &s565, ok, 1, 1), SHR_E_INVALID_ARG);
    /* Memory right before and after the destination may be read in a group. */
    uint8_t mem[3 * 16] = {0};
    shr_surface mid = packed(mem + 16, SHR_FORMAT_RGBX8888, 4, 1);
    for (int i = 0; i < 2; i++) {
        shr_surface near = packed(mem + 32 * i, SHR_FORMAT_RGBX8888, 4, 1);
        shr_rect r = {0, 0, 4, 1};
        shr_draw_cmd c[4] = {kbegin(2, r), with_src(SHR_CMD_COPY, r, as_image(&near), (shr_point){0, 0}), kend(),
                             kdraw(2, r, (shr_point){0, 0})};
        ASSERT_EQ_LL(drv.execute(drv.user, &mid, c, 4, 1), SHR_OK);
    }
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

/* A group whose content depends on its origin: fill, glyph, image and copy at offsets, after the REGISTERs of its
 * buffers: commands [2, 7) of `c` draw it directly, and with c[1] = KEEP_BEGIN and c[7] = KEEP_END they store it. */
static const uint8_t g_a8[4] = {0, 85, 170, 255};
static const uint8_t g_rgba[8] = {255, 255, 0, 255, 0, 255, 255, 100};
static uint8_t g_copy[2 * 4];

static void group(shr_draw_cmd *c, uint32_t id, shr_rect dst, shr_pixel_format f) {
    shr_surface cs = packed(g_copy, f, 2, 1);
    put(&cs, 0, 0, enc(f, 0x804020));
    put(&cs, 1, 0, enc(f, 0x102040));
    int32_t x = dst.x0, y = dst.y0;
    c[2] = kbegin(id, dst);
    c[3] = fill(dst, SHR_RGB(40, 40, 40));
    c[4] = glyph((shr_rect){x + 1, y, x + 5, y + 1}, SHR_RGB(255, 255, 255),
                 (shr_image){g_a8, 4, 1, 4, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, (shr_point){0, 0});
    c[5] = with_src(SHR_CMD_IMAGE, (shr_rect){x, y + 1, x + 2, y + 2},
                    (shr_image){g_rgba, 2, 1, 8, 8, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, (shr_point){0, 0});
    c[6] = with_src(SHR_CMD_COPY, (shr_rect){x + 4, y + 1, x + 6, y + 2}, as_image(&cs), (shr_point){0, 0});
    c[7] = kend();
    c[0] = reg(c[4].buffer), c[1] = reg(c[5].buffer);
}

/* Pixels of `out` inside `r` equal `ref` there, the others `init`. */
static bool drawn_in(const shr_surface *out, const shr_surface *ref, const shr_surface *init, shr_rect r) {
    for (int32_t y = 0; y < out->height; y++)
        for (int32_t x = 0; x < out->width; x++) {
            bool in = x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
            if (raw(out, x, y) != raw(in ? ref : init, x, y)) return false;
        }
    return true;
}

/* KEEP_DRAW copies exactly what the group draws directly, from any part of the keep to any place, in the batch
 * that stores it or later ones, until the id is stored again or released. */
TEST keeps_draw_what_groups_draw(void) {
    for (int i = 0; i < 2; i++) {
        uint8_t ref[8 * 8 * 4], init[8 * 4 * 4], out[sizeof(init)];
        memset(init, 0x11, sizeof(init)), memset(ref, 0x11, sizeof(ref));
        /* The group drawn directly at y 2 of an 8 x 8 surface: rows 2..5 of `ref`. */
        shr_surface sr = packed(ref, FMTS[i], 8, 8), si = packed(init, FMTS[i], 8, 4), so = packed(out, FMTS[i], 8, 4);
        shr_draw_cmd c[9];
        group(c, 1, (shr_rect){1, 2, 7, 6}, FMTS[i]);
        ASSERT_EQ_LL(exec(&sr, c + 3, 4), SHR_OK);
        shr_framebuffer_driver drv;
        ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 4, NBUF, &drv), SHR_OK);
        /* (x, y) of `out` shows group pixel (x - dx, y - dy) inside r, else what it held. */
#define SHOWS(r, dx, dy)                                                                                        \
    for (int32_t y = 0; y < 4; y++)                                                                             \
        for (int32_t x = 0; x < 8; x++) {                                                                       \
            bool in = x >= (r).x0 && x < (r).x1 && y >= (r).y0 && y < (r).y1;                                   \
            ASSERT_EQ_LL(raw(&so, x, y), in ? raw(&sr, x - (dx) + 1, y - (dy) + 2) : raw(&si, x, y));           \
        }
        shr_rect g = {1, 1, 7, 5}, part = {2, 1, 7, 4};
        group(c, 1, g, FMTS[i]);
        c[8] = kdraw(1, part, (shr_point){1, 0});
        memcpy(out, init, sizeof(out));
        ASSERT_EQ_LL(drv.execute(drv.user, &so, c, 9, 1), SHR_OK); /* reaches past the destination */
        SHOWS(part, 1, 1);
        /* Later batches draw the stored pixels, anywhere, whatever the commands now are. */
        shr_rect moved = {0, 0, 6, 4}, low = {3, 2, 6, 4};
        shr_draw_cmd d[2] = {kdraw(1, moved, (shr_point){0, 0}), kdraw(1, low, (shr_point){2, 2})};
        memcpy(out, init, sizeof(out));
        ASSERT_EQ_LL(drv.execute(drv.user, &so, d, 1, 2), SHR_OK);
        SHOWS(moved, 0, 0);
        memcpy(out, init, sizeof(out));
        ASSERT_EQ_LL(drv.execute(drv.user, &so, d + 1, 1, 3), SHR_OK);
        SHOWS(low, 1, 0);
        /* Empty KEEP_DRAWs draw nothing, also from an empty keep. */
        memcpy(out, init, sizeof(out));
        shr_draw_cmd none[5] = {kdraw(1, (shr_rect){3, 3, 3, 4}, (shr_point){6, 0}), kdraw(1, (shr_rect){3, 1, 5, 1}, (shr_point){0, 4}),
                                kbegin(3, (shr_rect){2, 2, 2, 2}), kend(), kdraw(3, (shr_rect){1, 1, 1, 1}, (shr_point){0, 0})};
        ASSERT_EQ_LL(drv.execute(drv.user, &so, none, 5, 4), SHR_OK);
        ASSERT_MEM_EQ(init, out, sizeof(out));
        /* A row crossing a band edge: stored whole by the band above, drawn in part by both. */
        group(c, 2, (shr_rect){1, 2, 7, 6}, FMTS[i]);
        c[8] = kdraw(2, (shr_rect){1, 2, 7, 4}, (shr_point){0, 0});
        memcpy(out, init, sizeof(out));
        ASSERT_EQ_LL(drv.execute(drv.user, &so, c, 9, 5), SHR_OK);
        SHOWS(c[8].dst, 1, 2);
        shr_draw_cmd below = kdraw(2, (shr_rect){1, 0, 7, 2}, (shr_point){0, 2});
        memcpy(out, init, sizeof(out));
        ASSERT_EQ_LL(drv.execute(drv.user, &so, &below, 1, 6), SHR_OK);
        SHOWS(below.dst, 1, -2);
#undef SHOWS
        /* Storing again replaces the pixels; RELEASE forgets them. */
        group(c, 1, g, FMTS[i]);
        c[3].color = SHR_RGB(0, 0, 0);
        c[8] = kdraw(1, (shr_rect){0, 0, 6, 4}, (shr_point){0, 0});
        ASSERT_EQ_LL(drv.execute(drv.user, &so, c, 9, 7), SHR_OK);
        ASSERT_EQ_LL(raw(&so, 2, 3), enc(FMTS[i], 0));
        shr_draw_cmd gone[2] = {krelease(1), kdraw(1, (shr_rect){0, 0, 6, 4}, (shr_point){0, 0})};
        ASSERT_EQ_LL(drv.execute(drv.user, &so, gone, 1, 8), SHR_OK);
        ASSERT_EQ_LL(drv.execute(drv.user, &so, gone + 1, 1, 9), SHR_E_INVALID_ARG);
        shr_draw_cmd unknown[3] = {krelease(0), krelease(5), krelease(1)}; /* ids holding nothing: no error */
        ASSERT_EQ_LL(drv.execute(drv.user, &so, unknown, 3, 10), SHR_OK);
        ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    }
    PASS();
}

/* Keep pixels come from the driver's allocator: the same size reuses them, another size reallocates, and a store the
 * allocator refuses fails the batch with SHR_E_NO_MEMORY before anything is drawn. */
TEST keep_memory(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    uint8_t buf[4 * 2 * 4] = {0}, zero[sizeof(buf)] = {0};
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, 4, 2);
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 0, 2, NBUF, &drv), SHR_OK);
    long base = f.live;
    shr_rect a = {0, 0, 4, 1}, b = {0, 0, 2, 2};
    shr_draw_cmd c[8] = {fill((shr_rect){0, 0, 4, 2}, SHR_RGB(1, 1, 1)), kbegin(1, a), fill(a, SHR_RGB(2, 2, 2)), kend(),
                         kbegin(2, b), fill(b, SHR_RGB(3, 3, 3)), kend(), kdraw(2, b, (shr_point){0, 0})};
    for (long fail_at = 0; fail_at < 2; fail_at++) { /* the batch's stores then hold nothing */
        f.budget = fail_at;
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 8, 1), SHR_E_NO_MEMORY);
        ASSERT_MEM_EQ(zero, buf, sizeof(buf));
        ASSERT_EQ_LL(f.live, base);
    }
    f.budget = -1;
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 8, 1), SHR_OK);
    ASSERT(f.live == base + 2 && raw(&s, 1, 1) == 0x030303FF && raw(&s, 3, 1) == 0x010101FF);
    f.budget = 0;
    c[2].color = SHR_RGB(4, 4, 4), c[7] = kdraw(1, a, (shr_point){0, 0});
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 8, 2), SHR_OK); /* same sizes: no allocation */
    ASSERT_EQ_LL(raw(&s, 3, 0), 0x040404FF);
    c[1].dst = c[2].dst = (shr_rect){0, 0, 3, 1}, c[7] = kdraw(1, (shr_rect){0, 1, 3, 2}, (shr_point){0, 0});
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 8, 3), SHR_E_NO_MEMORY);
    ASSERT_EQ_LL(f.live, base + 1); /* keep 2, stored after the one that failed */
    f.budget = -1;
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 8, 4), SHR_OK);
    ASSERT(raw(&s, 2, 1) == 0x040404FF && raw(&s, 3, 1) == 0x010101FF);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

/* The fail allocator, also summing the bytes of live 128-byte aligned allocations (keep pixels). */
typedef struct sized_alloc {
    fail_alloc f;
    size_t bytes;
    bool misaligned;
} sized_alloc;

static void *sa_alloc(void *user, size_t size, size_t align, shr_alloc_kind kind) {
    sized_alloc *z = user;
    void *p = fa_alloc(&z->f, size, align, kind);
    if (p && align == 128) z->bytes += size, z->misaligned |= (uintptr_t)p % 128 != 0;
    return p;
}

static void sa_free(void *user, void *p, size_t size, size_t align, shr_alloc_kind kind) {
    sized_alloc *z = user;
    if (align == 128) z->bytes -= size;
    fa_free(&z->f, p, size, align, kind);
}

/* With keep_bytes, id k owns slot k of keep_bytes / max_keeps bytes rounded down to 128, all in one block taken at
 * create (refused: SHR_E_NO_MEMORY, nothing kept): a keep larger than the slot is invalid, and stores never allocate,
 * also after KEEP_RELEASE and stores of other sizes. */
TEST keep_slots(void) {
    sized_alloc z = {{-1, 0}, 0, false};
    shr_allocator al = {&z, sa_alloc, sa_free, 0};
    uint8_t buf[4 * 4 * 4] = {0};
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, 4, 4);
    shr_framebuffer_driver drv;
    for (long budget = 0; budget < 7; budget++) { /* the driver, its tables, then the slots */
        z.f.budget = budget;
        ASSERT_EQ_LL(shr_software_driver_create(&al, 3 * 128 + 50, 3, NBUF, &drv), SHR_E_NO_MEMORY);
        ASSERT(z.f.live == 0 && z.bytes == 0 && !drv.execute);
    }
    z.f.budget = -1;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 3 * 128 + 50, 3, NBUF, &drv), SHR_OK);
    ASSERT(drv.caps.max_keeps == 3 && drv.caps.keep_bytes == 3 * 128 + 50 && drv.caps.max_keep_bytes == 128);
    ASSERT_EQ_LL(z.bytes, 3 * 128);
    z.f.budget = 0;
    shr_rect full = {0, 0, 4, 8}, tall = {0, 0, 4, 9}, wide = {0, 0, 33, 1}, dot = {0, 0, 1, 1}, sq = {0, 0, 2, 2};
    shr_draw_cmd c[6] = {kbegin(1, full), fill(full, SHR_RGB(1, 1, 1)), kend(), kbegin(2, dot), fill(dot, SHR_RGB(2, 2, 2)),
                         kend()};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 6, 1), SHR_OK); /* 128 bytes, past the destination */
    for (int i = 0; i < 2; i++) {
        shr_rect big = i ? wide : tall;
        shr_draw_cmd over[3] = {kbegin(3, big), fill(big, 0), kend()};
        ASSERT_EQ_LL(drv.execute(drv.user, &s, over, 3, 2), SHR_E_INVALID_ARG); /* 144, 132 bytes */
    }
    shr_draw_cmd d[2] = {kdraw(1, (shr_rect){0, 0, 4, 4}, (shr_point){0, 4}), kdraw(2, (shr_rect){3, 3, 4, 4}, (shr_point){0, 0})};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, d, 2, 3), SHR_OK);
    ASSERT(raw(&s, 0, 0) == 0x010101FF && raw(&s, 3, 3) == 0x020202FF);
    shr_draw_cmd gone[2] = {krelease(1), krelease(2)};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, gone, 2, 4), SHR_OK);
    ASSERT_EQ_LL(drv.execute(drv.user, &s, d, 1, 5), SHR_E_INVALID_ARG); /* holds nothing */
    shr_draw_cmd again[4] = {kbegin(1, sq), fill(sq, SHR_RGB(3, 3, 3)), kend(), kdraw(1, (shr_rect){1, 1, 3, 3}, (shr_point){0, 0})};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, again, 4, 6), SHR_OK); /* another size in the same slot */
    ASSERT(raw(&s, 2, 2) == 0x030303FF && raw(&s, 3, 3) == 0x020202FF);
    shr_draw_cmd more[7] = {kbegin(2, sq), fill(sq, SHR_RGB(4, 4, 4)), kend(), kbegin(3, dot), fill(dot, 0), kend(),
                            kdraw(2, sq, (shr_point){0, 0})};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, more, 7, 7), SHR_OK);
    ASSERT(raw(&s, 1, 1) == 0x040404FF && z.bytes == 3 * 128 && z.f.live == 7 && !z.misaligned);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT(z.bytes == 0 && z.f.live == 0);
    PASS();
}

/* A copier whose copies run only when waited for, the latest first, every `refuse`-th refused: pixels come out right only if
 * the driver waits before it touches what a copy reads or writes. */
typedef struct late_op {
    void *dst;
    const void *src;
    size_t dst_stride, src_stride, bytes;
    int32_t rows;
    shr_software_trim trim;
} late_op;

typedef struct late_copier {
    late_op q[64];
    uint64_t started, done;
    int refuse, calls;
    size_t bytes; /* asked to copy */
    bool bad;     /* a full queue, an empty copy or a ticket never given */
} late_copier;

static uint64_t late_trim(void *user, void *dst, size_t dst_stride, const void *src, size_t src_stride, size_t bytes,
                          int32_t rows, const shr_software_trim *trim) {
    late_copier *l = user;
    l->bytes += bytes * (size_t)rows;
    if (l->refuse && ++l->calls % l->refuse == 0) return 0;
    l->bad |= l->started - l->done == 64 || !rows || !bytes;
    l->q[l->started % 64] =
        (late_op){dst, src, dst_stride, src_stride, bytes, rows, trim ? *trim : (shr_software_trim){0}};
    return ++l->started;
}

static uint64_t late_copy(void *user, void *dst, size_t dst_stride, const void *src, size_t src_stride, size_t bytes,
                          int32_t rows) {
    return late_trim(user, dst, dst_stride, src, src_stride, bytes, rows, NULL);
}

static void late_wait(void *user, uint64_t ticket) {
    late_copier *l = user;
    l->bad |= ticket > l->started;
    for (uint64_t t = ticket; t > l->done; t--) { /* the latest first: only waits order copies */
        const late_op *c = &l->q[(t - 1) % 64];
        shr_software_trim_fill(c->dst, c->dst_stride, c->bytes, c->rows, &c->trim);
        for (int32_t y = 0; y < c->rows; y++)
            memcpy((uint8_t *)c->dst + (size_t)y * c->dst_stride, (const uint8_t *)c->src + (size_t)y * c->src_stride,
                   c->bytes);
    }
    if (ticket > l->done) l->done = ticket;
}

/* Appends group `id` at `g` to a batch: its registrations to `lead`, its commands to `body`. */
static void add_group(shr_draw_cmd *lead, size_t *nl, shr_draw_cmd *body, size_t *nb, uint32_t id, shr_rect g,
                      shr_pixel_format f) {
    shr_draw_cmd c[8];
    group(c, id, g, f);
    lead[(*nl)++] = c[0], lead[(*nl)++] = c[1];
    memcpy(body + *nb, c + 2, 6 * sizeof(c[0]));
    *nb += 6;
}

/* Batches into two surfaces through a driver with the late copier equal those through one without: KEEP_DRAWs and
 * stores (drawn into the destination first, or into the keep) overlapped by later draws, a COPY reading copied rows,
 * keeps drawn while stored or read, released and stored again while copies of an earlier batch still run, a reset, a
 * copier change and destroy with copies running. The port waits for a surface's copies before it reads it or draws
 * into it again, never for the other surface's. A copier makes stores cheap (SHR_DRIVER_CHEAP_STORE). */
TEST copier_orders_keep_copies(void) {
    for (int fi = 0; fi < 2; fi++)
        for (int mode = 0; mode < 4; mode++) { /* slots or not, refusing every third copy or not */
            shr_pixel_format f = FMTS[fi];
            uint8_t px[2][2][8 * 8 * 4];
            memset(px, 0, sizeof(px));
            shr_framebuffer_driver drv[2];
            late_copier l = {.refuse = mode & 1 ? 3 : 0};
            const shr_software_copier lc = {.user = &l, .copy = late_copy, .wait = late_wait};
            for (int d = 0; d < 2; d++)
                ASSERT_EQ_LL(shr_software_driver_create(NULL, mode & 2 ? 4 * 256 : 0, 4, NBUF, &drv[d]), SHR_OK);
            ASSERT_EQ_LL(shr_software_driver_set_copier(&drv[1], &lc), SHR_OK);
            ASSERT(drv[1].caps.flags == (SHR_DRIVER_CHEAP_MOVE | SHR_DRIVER_CHEAP_STORE));
            ASSERT_EQ_LL(drv[0].caps.flags, SHR_DRIVER_CHEAP_MOVE);
            uint64_t last[2] = {0, 0};
            shr_draw_cmd lead[96], body[64];
            size_t nl, nb;
#define BEGIN() (nl = 0, nb = 0, last_id = 0)
#define PUT(c) (body[nb++] = (c))
#define RUN(s, fence)                                                                                           \
    do {                                                                                                        \
        memcpy(lead + nl, body, nb * sizeof(body[0]));                                                          \
        for (int d = 0; d < 2; d++) {                                                                           \
            shr_surface t = packed(px[d][s], f, 8, 8);                                                          \
            for (size_t i = 0; i < nl + nb; i++) /* a COPY reads its own destination */                         \
                if (lead[i].kind == SHR_CMD_COPY && lead[i].src.width == 8) lead[i].src.pixels = px[d][s];     \
            if (d) late_wait(&l, last[s]);                                                                      \
            ASSERT_EQ_LL(drv[d].execute(drv[d].user, &t, lead, nl + nb, fence), SHR_OK);                        \
        }                                                                                                       \
        last[s] = l.started;                                                                                    \
    } while (0)
#define SAME(s)                                                                                                 \
    do {                                                                                                        \
        late_wait(&l, last[s]);                                                                                 \
        ASSERT_MEM_EQ(px[0][s], px[1][s], sizeof(px[0][s]));                                                    \
    } while (0)
            uint8_t other[8 * 8 * 4] = {0};
            shr_surface a = packed(other, f, 8, 8);
            shr_rect g1 = {0, 0, 8, 2}, g2 = {0, 2, 8, 4}, g3 = {0, 4, 8, 6};
            BEGIN();
            PUT(fill((shr_rect){0, 0, 8, 8}, SHR_RGB(1, 2, 3)));
            add_group(lead, &nl, body, &nb, 1, g1, f);      /* drawn here, copied into keep 1 */
            PUT(kdraw(1, g1, (shr_point){0, 0}));
            PUT(fill((shr_rect){2, 1, 4, 2}, SHR_RGB(9, 9, 9))); /* over the copy's rows */
            add_group(lead, &nl, body, &nb, 2, g2, f);      /* into keep 2, drawn in part */
            PUT(kdraw(2, (shr_rect){1, 2, 5, 4}, (shr_point){1, 0}));
            PUT(kdraw(1, g3, (shr_point){0, 0}));           /* keep 1 while it is stored */
            PUT(fill((shr_rect){0, 7, 8, 8}, SHR_RGB(4, 4, 4))); /* rows below and above the copies */
            PUT(fill((shr_rect){0, 0, 8, 1}, SHR_RGB(5, 5, 5)));
            PUT(with_src(SHR_CMD_COPY, (shr_rect){0, 6, 8, 7}, as_image(&a), (shr_point){0, 4})); /* reads copied rows */
            PUT(kdraw(2, (shr_rect){0, 6, 4, 8}, (shr_point){0, 0}));
            PUT(fill((shr_rect){3, 7, 6, 8}, SHR_RGB(6, 6, 6)));
            RUN(0, 1);
            SAME(0);
            BEGIN();
            add_group(lead, &nl, body, &nb, 3, g3, f);
            PUT(kdraw(3, g3, (shr_point){0, 0}));
            PUT(kdraw(3, (shr_rect){0, 6, 8, 8}, (shr_point){0, 0}));
            PUT(kdraw(1, g1, (shr_point){0, 0}));
            PUT(kdraw(1, g2, (shr_point){0, 0})); /* keep 1 read twice */
            RUN(1, 2); /* copies left running */
            BEGIN();
            lead[nl++] = krelease(1);
            add_group(lead, &nl, body, &nb, 1, (shr_rect){0, 0, 6, 2}, f); /* another size, read by surface 1 */
            PUT(kdraw(1, (shr_rect){0, 0, 6, 2}, (shr_point){0, 0}));
            PUT(kdraw(3, g2, (shr_point){0, 0})); /* stored by surface 1's batch */
            add_group(lead, &nl, body, &nb, 4, g3, f);
            PUT(kdraw(4, g3, (shr_point){0, 0}));
            RUN(0, 3);
            SAME(0);
            SAME(1);
            BEGIN();
            PUT(kdraw(4, (shr_rect){0, 0, 8, 2}, (shr_point){0, 0}));
            RUN(1, 4);
            ASSERT_EQ_LL(drv[1].reset(drv[1].user), SHR_OK);
            ASSERT_EQ_LL(l.done, l.started);
            RUN(0, 5);
            ASSERT_EQ_LL(shr_software_driver_set_copier(&drv[1], NULL), SHR_OK);
            ASSERT_EQ_LL(l.done, l.started);
            ASSERT_EQ_LL(drv[1].caps.flags, SHR_DRIVER_CHEAP_MOVE);
            SAME(0);
            SAME(1);
            RUN(1, 6);
            SAME(1);
            ASSERT_EQ_LL(shr_software_driver_set_copier(&drv[1], &lc), SHR_OK);
            RUN(1, 7);
            ASSERT(l.started > 8 && (!(mode & 1) || (uint64_t)l.calls > l.started));
            for (int d = 0; d < 2; d++) ASSERT_EQ_LL(shr_software_driver_destroy(&drv[d]), SHR_OK);
            ASSERT(l.done == l.started && !l.bad);
            ASSERT_MEM_EQ(px[0][1], px[1][1], sizeof(px[0][1]));
#undef BEGIN
#undef PUT
#undef RUN
#undef SAME
        }
    shr_framebuffer_driver drv, other;
    shr_framebuffer_driver_init(&other);
    late_copier l = {0};
    shr_software_copier lc = {.user = &l, .copy = late_copy, .wait = late_wait};
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 1, NBUF, &drv), SHR_OK);
    ASSERT_EQ_LL(shr_software_driver_set_copier(NULL, &lc), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_driver_set_copier(&other, &lc), SHR_E_INVALID_ARG);
    shr_framebuffer_driver no_user = drv;
    no_user.user = NULL;
    ASSERT_EQ_LL(shr_software_driver_set_copier(&no_user, &lc), SHR_E_INVALID_ARG);
    lc.copy = NULL;
    ASSERT_EQ_LL(shr_software_driver_set_copier(&drv, &lc), SHR_E_INVALID_ARG);
    lc.copy = late_copy, lc.wait = NULL;
    ASSERT_EQ_LL(shr_software_driver_set_copier(&drv, &lc), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

static bool column_is(const shr_surface *s, int32_t x, uint32_t v) {
    for (int32_t y = 0; y < s->height; y++)
        if (raw(s, x, y) != v) return false;
    return true;
}

/* Copies a group (KEEP_BEGIN .. KEEP_END) into `out`, `n` commands, at the start of a batch of `extra` more. */
static size_t trim_batch(shr_draw_cmd *out, const shr_draw_cmd *grp, size_t n, const shr_draw_cmd *extra, size_t ne) {
    memcpy(out, grp, n * sizeof(*grp));
    if (ne) memcpy(out + n, extra, ne * sizeof(*extra));
    return n + ne;
}

static shr_draw_cmd dim_fill(shr_rect r, shr_color c) {
    shr_draw_cmd d = fill(r, c);
    d.flags = SHR_GLYPH_DIM;
    return d;
}

/* Runs `n` commands into surface `sd` of both drivers (late copier on the second), waiting for its copies after;
 * the bytes the second asked to copy. */
static size_t trim_run(shr_framebuffer_driver *drv, late_copier *l, uint8_t (*px)[2][48 * 8 * 4], int sd,
                       shr_pixel_format f, const shr_draw_cmd *c, size_t n, int *fails) {
    size_t before = 0;
    for (int d = 0; d < 2; d++) {
        shr_surface s = packed(px[d][sd], f, 48, 8);
        before = d ? l->bytes : 0;
        *fails += drv[d].execute(drv[d].user, &s, c, n, 1) != SHR_OK;
        late_wait(l, l->started);
    }
    return l->bytes - before;
}

/* Groups of FILLs and COPYs, stored from the destination and into the keep, drawn whole and in parts at any alignment
 * equal those of a driver without a copier; drawn whole at the alignment of a pixel, the copies take the columns
 * between those an opaque FILL of the group's height last wrote at its edges (at least one, at most all, never fewer
 * than the pixels allow). After a copier change a keep is copied whole. */
TEST copier_trims_keep_draws(void) {
    static const shr_color cols[3] = {SHR_RGB(10, 20, 30), SHR_RGB(200, 100, 50), SHR_RGB(10, 20, 30) | 0x40};
    static const size_t aligns[4] = {0, 8, 16, 64};
    int fails = 0;
    for (int fi = 0; fi < 2; fi++)
        for (int trial = 0; trial < 400; trial++) {
            shr_pixel_format f = FMTS[fi];
            size_t b = bpp(f);
            int32_t w = 1 + (int32_t)(rnd() % 40), h = 1 + (int32_t)(rnd() % 4);
            uint8_t kp[40 * 4 * 4], px[2][2][48 * 8 * 4];
            shr_surface ks = packed(kp, f, w, h);
            for (int32_t y = 0; y < h; y++)
                for (int32_t x = 0; x < w; x++) put(&ks, x, y, rnd());
            int32_t gx = (int32_t)(rnd() % (uint32_t)(48 - w + 1)), gy = (int32_t)(rnd() % (uint32_t)(8 - h + 1));
            shr_rect g = {gx, gy, gx + w, gy + h};
            shr_draw_cmd grp[10];
            size_t ng = 0;
            grp[ng++] = kbegin(1, g);
            if (trial % 6) grp[ng++] = fill(g, cols[rnd() % 3]);
            for (int e = (int)(rnd() % 5); e > 0; e--) {
                int32_t x0 = gx + (int32_t)(rnd() % (uint32_t)w);
                int32_t x1 = x0 + 1 + (int32_t)(rnd() % (uint32_t)(gx + w - x0)), y0 = gy + (int32_t)(e & 1) * h / 2;
                shr_rect r = {x0, gy, x1, gy + h};
                switch (rnd() % 6) {
                case 0: grp[ng++] = fill(r, cols[rnd() % 3]); break;
                case 1: grp[ng++] = fill((shr_rect){x0, y0, x1, y0 + (h + 1) / 2}, cols[rnd() % 3]); break;
                case 2: grp[ng++] = dim_fill(r, cols[rnd() % 3]); break;
                case 3: grp[ng++] = with_src(SHR_CMD_COPY, r, as_image(&ks), (shr_point){0, 0}); break;
                case 4: grp[ng++] = fill((shr_rect){gx, gy, x1, gy + h}, cols[rnd() % 3]); break;
                default: grp[ng++] = fill((shr_rect){x0, gy, gx + w, gy + h}, cols[rnd() % 3]);
                }
            }
            grp[ng++] = kend();
            late_copier l = {.refuse = trial & 1 ? 0 : 3};
            size_t align = trial % 4 ? aligns[trial % 4] : b * (size_t)(trial & 4) / 4;
            shr_software_copier lc = {&l, late_copy, late_wait, late_trim, align};
            shr_framebuffer_driver drv[2];
            for (int d = 0; d < 2; d++) ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 2, NBUF, &drv[d]), SHR_OK);
            ASSERT_EQ_LL(shr_software_driver_set_copier(&drv[1], &lc), SHR_OK);
            for (size_t i = 0; i < sizeof(px[0]); i++)
                px[0][i / sizeof(px[0][0])][i % sizeof(px[0][0])] = (uint8_t)rnd();
            memcpy(px[1], px[0], sizeof(px[0]));
            int32_t px2 = (int32_t)(rnd() % (uint32_t)(48 - w + 1)), py2 = (int32_t)(rnd() % (uint32_t)(8 - h + 1));
            int32_t sx = (int32_t)(rnd() % (uint32_t)w), sw = 1 + (int32_t)(rnd() % (uint32_t)(w - sx));
            shr_draw_cmd c[16], whole = kdraw(1, (shr_rect){px2, py2, px2 + w, py2 + h}, (shr_point){0, 0});
            shr_draw_cmd parts[2] = {kdraw(1, (shr_rect){px2 + sx, 0, px2 + sx + sw, 1}, (shr_point){sx, h - 1}),
                                     kdraw(1, (shr_rect){0, 7, sw, 8}, (shr_point){sx, 0})};
            for (int into = 0; into < 2; into++) { /* stored from the destination, then into the keep */
                shr_draw_cmd draw_g = kdraw(1, g, (shr_point){0, 0});
                trim_run(drv, &l, px, 0, f, c, trim_batch(c, grp, ng, &draw_g, (size_t)!into), &fails);
                shr_surface ref = packed(px[0][0], f, 48, 8), ks2 = packed(kp, f, w, h);
                if (into) /* what the group drew, through a driver without keeps */
                    trim_run(drv, &l, px, 0, f, c, trim_batch(c, grp + 1, ng - 2, NULL, 0), &fails);
                for (int32_t y = 0; y < h; y++)
                    for (int32_t x = 0; x < w; x++) put(&ks2, x, y, raw(&ref, gx + x, gy + y));
                int32_t lo = 0, x1 = w;
                while (lo < w && column_is(&ks2, lo, raw(&ks2, 0, 0))) lo++;
                while (x1 > 0 && column_is(&ks2, x1 - 1, raw(&ks2, w - 1, 0))) x1--;
                size_t got = trim_run(drv, &l, px, 1, f, &whole, 1, &fails);
                size_t least = (size_t)(x1 > lo ? x1 - lo : 1) * b * (size_t)h;
                if (!(trial % 4)) ASSERT(got >= least && got <= (size_t)w * b * (size_t)h);
                trim_run(drv, &l, px, 1, f, parts, 2, &fails);
                ASSERT_MEM_EQ(px[0], px[1], sizeof(px[0]));
            }
            ASSERT_EQ_LL(shr_software_driver_set_copier(&drv[1], &lc), SHR_OK); /* trims dropped */
            ASSERT_EQ_LL(trim_run(drv, &l, px, 1, f, &whole, 1, &fails), (size_t)w * b * (size_t)h);
            ASSERT_MEM_EQ(px[0], px[1], sizeof(px[0]));
            for (int d = 0; d < 2; d++) ASSERT_EQ_LL(shr_software_driver_destroy(&drv[d]), SHR_OK);
            ASSERT(!l.bad && !fails);
        }
    /* 20 x 2 groups: the columns copied */
    uint8_t src[20 * 2 * 4] = {0}, px[2][2][48 * 8 * 4] = {{{0}}};
    for (int fi = 0; fi < 2; fi++) {
        shr_pixel_format f = FMTS[fi];
        shr_surface ss = packed(src, f, 20, 2);
        const shr_rect g = {0, 0, 20, 2}, mid = {8, 0, 12, 2};
        const shr_draw_cmd a = fill(g, cols[0]), cp = with_src(SHR_CMD_COPY, mid, as_image(&ss), (shr_point){0, 0});
        const struct {
            shr_draw_cmd c[4];
            size_t n;
            size_t copied;
        } cases[] = {{{a, fill((shr_rect){5, 0, 9, 2}, cols[1])}, 2, 4},
                     {{a}, 1, 1},
                     {{a, cp, fill((shr_rect){0, 0, 4, 2}, cols[1])}, 3, 8},
                     {{a, dim_fill((shr_rect){10, 0, 20, 2}, cols[1])}, 2, 10},
                     {{a, fill((shr_rect){0, 0, 20, 1}, cols[1])}, 2, 20},
                     {{fill((shr_rect){0, 0, 20, 1}, cols[0]), fill((shr_rect){0, 1, 20, 2}, cols[0]), cp,
                       fill((shr_rect){12, 0, 20, 2}, cols[1])},
                      4, 12}};
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            late_copier l = {0};
            shr_software_copier lc = {.user = &l, .copy = late_copy, .wait = late_wait, .copy_trim = late_trim};
            shr_framebuffer_driver drv[2];
            for (int d = 0; d < 2; d++) ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 1, NBUF, &drv[d]), SHR_OK);
            ASSERT_EQ_LL(shr_software_driver_set_copier(&drv[1], &lc), SHR_OK);
            shr_draw_cmd c[8] = {kbegin(1, g)}, kd = kdraw(1, (shr_rect){4, 3, 24, 5}, (shr_point){0, 0});
            memcpy(c + 1, cases[i].c, cases[i].n * sizeof(c[0]));
            c[cases[i].n + 1] = kend();
            trim_run(drv, &l, px, 0, f, c, cases[i].n + 2, &fails);
            ASSERT_EQ_LL(trim_run(drv, &l, px, 1, f, &kd, 1, &fails), cases[i].copied * bpp(f) * 2);
            ASSERT_MEM_EQ(px[0], px[1], sizeof(px[0]));
            for (int d = 0; d < 2; d++) ASSERT_EQ_LL(shr_software_driver_destroy(&drv[d]), SHR_OK);
        }
    }
    /* A group of no rows stores no pixels, so nothing at its edges is read (a 128-byte slot, 48 columns). */
    late_copier l = {0};
    const shr_software_copier lc = {.user = &l, .copy = late_copy, .wait = late_wait, .copy_trim = late_trim};
    shr_framebuffer_driver drv[2];
    for (int d = 0; d < 2; d++) ASSERT_EQ_LL(shr_software_driver_create(NULL, 128, 1, NBUF, &drv[d]), SHR_OK);
    ASSERT_EQ_LL(shr_software_driver_set_copier(&drv[1], &lc), SHR_OK);
    shr_draw_cmd empty[2] = {kbegin(1, (shr_rect){0, 2, 48, 2}), kend()};
    trim_run(drv, &l, px, 0, FMTS[0], empty, 2, &fails);
    for (int d = 0; d < 2; d++) ASSERT_EQ_LL(shr_software_driver_destroy(&drv[d]), SHR_OK);
    ASSERT(!fails && !l.bad);
    /* RGBX8888 rows starting 2 bytes off a pixel boundary (by the address, or every other row by the stride): the
     * keep is copied whole, the same pixels. */
    const shr_rect og = {0, 0, 8, 2};
    const shr_draw_cmd grp2[4] = {kbegin(1, og), fill(og, cols[0]), fill((shr_rect){3, 0, 5, 2}, cols[1]), kend()};
    const shr_draw_cmd od = kdraw(1, (shr_rect){0, 2, 8, 4}, (shr_point){0, 0});
    for (int lay = 0; lay < 2; lay++) {
        late_copier off = {.refuse = 1};
        const shr_software_copier oc = {.user = &off, .copy = late_copy, .wait = late_wait, .copy_trim = late_trim};
        uint32_t ob[2][9 * 4 + 1]; /* 4-byte aligned, so ob + 2 bytes is not */
        size_t stride = lay ? 8 * 4 + 2 : 8 * 4;
        for (int d = 0; d < 2; d++) {
            memset(ob[d], 0, sizeof(ob[d]));
            ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 1, NBUF, &drv[d]), SHR_OK);
        }
        ASSERT_EQ_LL(shr_software_driver_set_copier(&drv[1], &oc), SHR_OK);
        for (int d = 0; d < 2; d++) {
            shr_surface os = packed((uint8_t *)ob[d] + (lay ? 0 : 2), SHR_FORMAT_RGBX8888, 8, 4);
            os.stride = stride, os.byte_length = 3 * stride + 8 * 4;
            ASSERT_EQ_LL(drv[d].execute(drv[d].user, &os, grp2, 4, 1), SHR_OK);
            ASSERT_EQ_LL(drv[d].execute(drv[d].user, &os, &od, 1, 2), SHR_OK);
            late_wait(&off, off.started);
        }
        ASSERT_MEM_EQ(ob[0], ob[1], sizeof(ob[0]));
        ASSERT_EQ_LL(off.bytes, 2 * 8 * 4);
        for (int d = 0; d < 2; d++) ASSERT_EQ_LL(shr_software_driver_destroy(&drv[d]), SHR_OK);
        ASSERT(!off.bad);
    }
    PASS();
}

/* An ON_FILL GLYPH blended against `bg` equals the GLYPH blended onto the FILL under it, for every source format and
 * style, odd starts and rows wider than a span, beside pixels the FILL did not write, in and out of keep groups. */
TEST glyphs_on_fills_match_blending(void) {
    enum { W = 150, H = 5, SW = 170 };
    static const int32_t widths[] = {0, 1, 5, 8, 13, 70, W};
    static uint8_t a4[(W + 4) / 2 * H], a8[(W + 4) * H], ref[SW * H * 4], out[sizeof(ref)], noise[sizeof(ref)];
    coverage_noise(a4, sizeof(a4)), coverage_noise(a8, sizeof(a8));
    for (size_t i = 0; i < sizeof(noise); i++) noise[i] = (uint8_t)rnd();
    const shr_image g[2] = {{a4, W + 4, H, (W + 4) / 2, sizeof(a4), SHR_FORMAT_A4, 0},
                            {a8, W + 4, H, W + 4, sizeof(a8), SHR_FORMAT_A8, 0}};
    const shr_color bg = 0x5A0AC84Du;
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 1, NBUF, &drv), SHR_OK);
    for (int k = 0; k < 2; k++)
        for (int sf = 0; sf < 2; sf++)
            for (uint32_t flags = 0; flags < 8; flags++)
                for (size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++)
                    for (int32_t ox = 0; ox < 2; ox++) {
                        int32_t w = widths[wi], x0 = 0, x1 = w;
                        shr__glyph_footprint(w, H, flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC), 3, &x0, &x1);
                        if (x0 + ox > x1) continue;
                        shr_rect cell = {7, 0, 7 + x1 - x0 - ox, H}, all = {0, 0, SW, H}, clip = {9, 1, SW, H};
                        shr_draw_cmd c[7] = {cmd(SHR_CMD_BUFFER_REGISTER, all), kbegin(1, all),
                                             fill(all, SHR_RGB(1, 2, 3)), fill(cell, bg),
                                             region(SHR_CMD_GLYPH, cell, g[sf], (shr_rect){2, 0, 2 + w, H},
                                                    (shr_point){x0 + ox, 0}),
                                             kend(), kdraw(1, clip, (shr_point){clip.x0, clip.y0})};
                        c[0] = reg(c[4].buffer);
                        c[4].flags = (uint16_t)flags, c[4].slant_axis = 3, c[4].color = SHR_RGB(250, 30 * flags, 9);
                        shr_surface sr = packed(ref, FMTS[k], SW, H), so = packed(out, FMTS[k], SW, H);
                        memcpy(ref, noise, sizeof(ref));
                        ASSERT_EQ_LL(exec(&sr, c + 3, 2), SHR_OK);
                        c[4].flags |= SHR_GLYPH_ON_FILL, c[4].bg = bg;
                        memcpy(out, noise, sizeof(out));
                        ASSERT_EQ_LL(exec(&so, c + 3, 2), SHR_OK);
                        ASSERT_MEM_EQ(ref, out, sizeof(ref));
                        /* Stored whole, then drawn in a clip. */
                        c[4].flags &= ~(uint32_t)SHR_GLYPH_ON_FILL;
                        memcpy(ref, noise, sizeof(ref)), memcpy(out, noise, sizeof(out));
                        ASSERT_EQ_LL(exec(&sr, c + 2, 3), SHR_OK);
                        c[4].flags |= SHR_GLYPH_ON_FILL;
                        ASSERT_EQ_LL(drv.execute(drv.user, &so, c, 7, 1), SHR_OK);
                        shr_surface sn = packed(noise, FMTS[k], SW, H);
                        ASSERT(drawn_in(&so, &sr, &sn, clip));
                    }
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

/* ---- The driver's cache of synthesized coverage ---- */

/* Clears two SW x SH surfaces of format f, runs batch c[0, n) on the driver and its draws on the stateless path with
 * buffer table `table`: whether both succeed and draw the same bytes. */
static bool driver_draws(shr_framebuffer_driver *drv, shr_pixel_format f, const shr_draw_cmd *c, size_t n,
                         const shr_image *table) {
    enum { SW = 200, SH = 12 };
    static uint8_t a[SW * SH * 4], b[sizeof(a)];
    memset(a, 0, sizeof(a)), memset(b, 0, sizeof(b));
    shr_surface sa = packed(a, f, SW, SH), sb = packed(b, f, SW, SH);
    size_t i = 0;
    while (i < n && shr__buffer_cmd(c[i].kind)) i++;
    return drv->execute(drv->user, &sa, c, n, 1) == SHR_OK && shr_software_execute(&sb, c + i, n - i, table, NBUF) == SHR_OK &&
           !memcmp(a, b, sizeof(a));
}

/* A GLYPH of `rect` of buffer `id` styled `flags` with ITALIC axis `axis`, its whole footprint at (x, y). */
static shr_draw_cmd slanted(uint32_t id, shr_rect rect, uint32_t flags, int32_t axis, int32_t x, int32_t y) {
    int32_t x0, x1;
    shr__glyph_footprint(rect.x1 - rect.x0, rect.y1 - rect.y0, flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC), axis, &x0,
                         &x1);
    shr_draw_cmd c = cmd(SHR_CMD_GLYPH, (shr_rect){x, y, x + x1 - x0, y + rect.y1 - rect.y0});
    c.flags = (uint16_t)flags, c.buffer = id, c.src_rect = rect, c.src_origin = (shr_point){x0, 0}, c.slant_axis = axis;
    c.color = SHR_RGB(240, 120, 9);
    return c;
}

static shr_draw_cmd styled(uint32_t id, shr_rect rect, uint32_t flags, int32_t x, int32_t y) {
    return slanted(id, rect, flags, 5, x, y);
}

/* Cached coverage draws what synthesis at every draw does: A4 and A8, every style with DIM and ON_FILL, whole and
 * clipped, again in later batches and in keep groups. */
TEST synth_cache_draws_what_synthesis_draws(void) {
    enum { W = 70, H = 9 };
    static uint8_t a4[(W + 4) / 2 * (H + 1)], a8[(W + 4) * (H + 1)];
    coverage_noise(a4, sizeof(a4)), coverage_noise(a8, sizeof(a8));
    const shr_image g[2] = {{a4, W + 4, H + 1, (W + 4) / 2, sizeof(a4), SHR_FORMAT_A4, 0},
                            {a8, W + 4, H + 1, W + 4, sizeof(a8), SHR_FORMAT_A8, 0}};
    static const int32_t widths[] = {1, 6, 8, 13, 64, W};
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 1, NBUF, &drv), SHR_OK);
    for (int k = 0; k < 2; k++)
        for (int sf = 0; sf < 2; sf++)
            for (uint32_t flags = SHR_GLYPH_BOLD; flags < 16; flags++)
                for (size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
                    if (!(flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC))) continue;
                    uint32_t id = use(g[sf]);
                    shr_draw_cmd s = styled(id, (shr_rect){2, 1, 2 + widths[wi], 1 + H}, flags, 1, 2), part = s;
                    int32_t fw = s.dst.x1 - s.dst.x0, cut = fw > 3 ? 3 : 1;
                    part.dst = (shr_rect){100, 0, 100 + fw - cut, H - 2};
                    part.src_origin.x += cut, part.color = SHR_RGB(9, 200, 255);
                    s.bg = part.bg = SHR_RGB(200, 9, 50);
                    shr_rect all = {0, 0, 200, 12};
                    shr_draw_cmd c[8] = {reg(id), fill(all, SHR_RGB(30, 60, 90)), fill(s.dst, s.bg), s, fill(part.dst, s.bg), part};
                    ASSERT(driver_draws(&drv, FMTS[k], c, 6, bufs));
                    /* again, as a keep */
                    c[0] = kbegin(1, all), c[6] = kend(), c[7] = kdraw(1, all, (shr_point){0, 0});
                    static uint8_t a[200 * 12 * 4], b[sizeof(a)];
                    shr_surface sa = packed(a, FMTS[k], 200, 12), sb = packed(b, FMTS[k], 200, 12);
                    memset(a, 1, sizeof(a)), memset(b, 1, sizeof(b));
                    ASSERT_EQ_LL(drv.execute(drv.user, &sa, c, 8, 2), SHR_OK);
                    ASSERT_EQ_LL(exec(&sb, c + 1, 5), SHR_OK);
                    ASSERT_MEM_EQ(b, a, sizeof(a));
                }
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

/* Coverage is kept across batches until a REGISTER or RELEASE of its buffer or an UPDATE meeting its rect; pixels
 * changed without one (which the rules forbid) show what is kept. The memory: taken at the first styled GLYPH,
 * doubling up to the size set, emptied when full, synthesis at every draw when the allocator refuses. */
TEST synth_cache_follows_buffers_and_memory(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    enum { W = 74, H = 9 };
    static uint8_t px[W * H], old[sizeof(px)];
    coverage_noise(px, sizeof(px));
    shr_image m = {px, W, H, W, sizeof(px), SHR_FORMAT_A8, 0};
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 0, 0, NBUF, &drv), SHR_OK);
    long base = f.live;
    uint32_t id = use(m);
    shr_image stale[NBUF];
    const uint32_t BI = SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC;
    shr_rect r1 = {0, 0, 6, 6}, r2 = {8, 0, 14, 6};
    shr_draw_cmd c[300] = {reg(id), styled(id, r1, BI, 0, 0), styled(id, r2, SHR_GLYPH_BOLD, 20, 0)};
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c, 3, bufs));
    ASSERT_EQ_LL(f.live, base + 2);
    memcpy(stale, bufs, sizeof(stale));
    stale[id - 1].pixels = old;
    memcpy(old, px, sizeof(px)), memset(px, 0x5A, sizeof(px));
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c + 1, 2, stale));
    ASSERT(!driver_draws(&drv, SHR_FORMAT_RGBX8888, c + 1, 2, bufs));
    c[0] = buf_cmd(SHR_CMD_BUFFER_UPDATE, id), c[0].src_rect = (shr_rect){14, 0, 16, 9};
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c, 3, stale));
    c[0].src_rect = (shr_rect){13, 5, 15, 6};
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c, 1, bufs));
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c + 1, 1, stale)); /* r1 kept, r2 synthesized anew */
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c + 2, 1, bufs));
    c[0] = reg(id);
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c, 3, bufs));
    memcpy(old, px, sizeof(px)), memset(px, 0xA5, sizeof(px));
    shr_draw_cmd rr[4] = {buf_cmd(SHR_CMD_BUFFER_RELEASE, id), reg(id), c[1], c[2]};
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, rr, 4, bufs));
    /* No columns or no pixels: nothing kept. */
    shr_draw_cmd none[2] = {styled(id, (shr_rect){3, 0, 3, 6}, SHR_GLYPH_BOLD, 0, 0), c[1]};
    none[1].dst.x1 = none[1].dst.x0;
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, none, 2, bufs));

    shr_framebuffer_driver other;
    shr_framebuffer_driver_init(&other);
    ASSERT_EQ_LL(shr_software_driver_synth_cache(NULL, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_driver_synth_cache(&other, 0), SHR_E_INVALID_ARG);
    shr_framebuffer_driver no_user = drv;
    no_user.user = NULL;
    ASSERT_EQ_LL(shr_software_driver_synth_cache(&no_user, 0), SHR_E_INVALID_ARG);
    /* 0 keeps nothing; an entry larger than the size is synthesized at every draw. */
    for (size_t bytes = 0; bytes < 100; bytes += 50) {
        ASSERT_EQ_LL(shr_software_driver_synth_cache(&drv, bytes), SHR_OK);
        ASSERT_EQ_LL(f.live, base);
        f.budget = 0;
        ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c + 1, 2, bufs));
        f.budget = -1;
    }
    /* Refused memory: the arena, then the index; nothing is kept and nothing asked for again. */
    for (long budget = 0; budget < 2; budget++) {
        ASSERT_EQ_LL(shr_software_driver_synth_cache(&drv, SIZE_MAX), SHR_OK);
        f.budget = budget;
        ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c + 1, 2, bufs));
        f.budget = -1;
        ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c + 1, 2, bufs));
        ASSERT_EQ_LL(f.live, base);
    }
    /* 1 x 1 rects, B, I and B|I, 288 entries: the index outgrows the arena, which grows also with entries an UPDATE
     * dropped; then growing is refused and the arena empties instead. */
    ASSERT_EQ_LL(shr_software_driver_synth_cache(&drv, 1 << 20), SHR_OK);
    for (uint32_t style = 1; style < 4; style++) {
        size_t n = 0;
        c[n++] = buf_cmd(SHR_CMD_BUFFER_UPDATE, id), c[0].src_rect = (shr_rect){0, 0, 8, 6};
        for (int32_t y = 0; y < 6; y++)
            for (int32_t x = 0; x < 16; x++) c[n++] = styled(id, (shr_rect){x, y, x + 1, y + 1}, style << 1, 4 * x, y);
        ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c, n, bufs));
        ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c + 1, n - 1, bufs));
    }
    ASSERT_EQ_LL(f.live, base + 2);
    ASSERT_EQ_LL(shr_software_driver_synth_cache(&drv, 1 << 20), SHR_OK);
    for (int round = 0; round < 2; round++) {
        size_t n = 0;
        for (int32_t x = 0; x < 100; x++) c[n++] = styled(id, (shr_rect){x % W, round, x % W + 1, round + 1}, x < W ? 2 : 4, x, 0);
        f.budget = round ? 0 : -1;
        ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c, n, bufs));
    }
    f.budget = -1;
    ASSERT_EQ_LL(f.live, base + 2);
    /* A 4 KiB arena: wide entries fill it before the index. */
    ASSERT_EQ_LL(shr_software_driver_synth_cache(&drv, 4096), SHR_OK);
    for (int32_t x = 0; x < 30; x += 2) c[x / 2] = styled(id, (shr_rect){x, 0, x + 40, H}, BI, 0, 0);
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c, 15, bufs));
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c, 15, bufs));
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

/* Entries a RELEASE dropped leave the index: a key drawn again after each release of its buffer takes one slot. */
TEST synth_cache_indexes_live_entries(void) {
    enum { W = 8, H = 6 };
    static uint8_t px[W * H], out[16 * H * 4];
    coverage_noise(px, sizeof(px));
    uint32_t id = use((shr_image){px, W, H, W, sizeof(px), SHR_FORMAT_A8, 0});
    shr__alloc al;
    ASSERT(shr__alloc_init(&al, NULL));
    shr__synth s = {.cap = 1 << 20};
    shr_surface dst = packed(out, SHR_FORMAT_RGBX8888, 16, H);
    shr_draw_cmd c = styled(id, (shr_rect){0, 0, W, H}, SHR_GLYPH_BOLD, 0, 0);
    for (int k = 0; k < 50; k++) {
        shr__synth_draw(&s, &al, &dst, &c, bufs, (shr_point){0, 0}, (shr_rect){0, 0, 16, H});
        ASSERT_EQ_LL(s.count, 1);
        shr__synth_drop(&s, id, NULL);
        ASSERT_EQ_LL(s.count, 0);
    }
    shr__synth_reset(&s, &al, 0);
    PASS();
}

/* Entries whose keys differ in one field each (buffer, style, axis, a rect edge) never stand for one another, in an
 * index of 16 slots where they meet. ON_FILL draws go through LUTs by colour, DIM, bg and format, which empty after 32
 * and are taken with the first such draw, also where no FILL lies under them, as synthesis at every draw does; refused
 * LUT memory synthesizes instead, and a reset frees it. */
TEST synth_cache_keys_and_luts(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    enum { W = 24, H = 12 };
    static uint8_t px[7][W * H];
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 0, 0, NBUF, &drv), SHR_OK);
    long base = f.live;
    shr_draw_cmd c[2 * 40];
    for (int i = 0; i < 7; i++) {
        coverage_noise(px[i], sizeof(px[i]));
        c[i] = reg(use((shr_image){px[i], W, H, W, sizeof(px[i]), SHR_FORMAT_A8, 0}));
    }
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c, 7, bufs));
    uint32_t id = c[0].buffer;
    const uint32_t BI = SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC;
    ASSERT_EQ_LL(shr_software_driver_synth_cache(&drv, 1024), SHR_OK);
    for (int field = 0; field < 7; field++)
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < 15; i++) {
                shr_rect r = {2, 2, 5, 5};
                int32_t axis = 3;
                uint32_t style = BI;
                switch (field) {
                case 0: id = c[i % 7].buffer; break;
                case 1: style = (uint32_t)(i % 3 + 1) << 1; break;
                case 2: axis = i - 7; break;
                case 3: r.x0 = i, r.x1 = 20; break;
                case 4: r.y0 = i % 10, r.y1 = 11; break;
                case 5: r.x1 = 5 + i; break;
                default: r.y0 = 0, r.y1 = 1 + i % 11; break;
                }
                c[7] = slanted(field ? c[0].buffer : id, r, style, axis, 0, 0);
                ASSERT(driver_draws(&drv, FMTS[pass], c + 7, 1, bufs));
            }
    id = c[0].buffer;
    ASSERT_EQ_LL(f.live, base + 2);
    /* 40 colours on 2 bgs, with and without DIM, in both formats; one colour on 30 bgs. */
    shr_rect r = {4, 2, 10, 9};
    for (int round = 0; round < 4; round++) {
        size_t n = 0;
        for (int i = 0; i < 40; i++) {
            shr_draw_cmd s = styled(id, r, BI | SHR_GLYPH_ON_FILL | (round & 1 ? SHR_GLYPH_DIM : 0), 4 * i, 0);
            s.color = SHR_RGB(6 * i, 255 - 5 * i, 90), s.bg = i & 1 ? SHR_RGB(10, 20, 30) : SHR_RGB(200, 9, 50);
            c[n++] = fill(s.dst, s.bg), c[n++] = s;
        }
        ASSERT(driver_draws(&drv, FMTS[round / 2], c, n, bufs));
        ASSERT(driver_draws(&drv, FMTS[round / 2], c, 2, bufs));
        ASSERT(driver_draws(&drv, FMTS[1 - round / 2], c, 2, bufs));
        for (int i = 0; i < 30; i++)
            c[2 * i + 1].color = c[1].color, c[2 * i].color = c[2 * i + 1].bg = SHR_RGB(i, 7 * i, 9);
        ASSERT(driver_draws(&drv, FMTS[round / 2], c, 60, bufs));
        ASSERT(driver_draws(&drv, FMTS[round / 2], c, 60, bufs));
        ASSERT_EQ_LL(f.live, base + 3);
    }
    ASSERT_EQ_LL(shr_software_driver_synth_cache(&drv, 1 << 20), SHR_OK);
    ASSERT_EQ_LL(f.live, base);
    f.budget = 2;
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c, 2, bufs));
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c + 1, 1, bufs)); /* no FILL under it: still what synthesis draws */
    ASSERT_EQ_LL(f.live, base + 2);
    f.budget = -1;
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c, 2, bufs));
    for (int fi = 0; fi < 2; fi++) ASSERT(driver_draws(&drv, FMTS[fi], c + 1, 1, bufs));
    ASSERT_EQ_LL(f.live, base + 3);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

/* IMAGE into RGB565 through the driver's plane: every colour level at every alpha onto every 565 level. */
TEST image_planes_blend_exactly(void) {
    enum { N = 256 };
    static uint8_t rgba[N * N * 4], buf[N * N * 2];
    for (uint32_t a = 0; a < N; a++)
        for (uint32_t c = 0; c < N; c++) {
            uint8_t *q = rgba + 4 * (a * N + c);
            q[0] = (uint8_t)c, q[1] = (uint8_t)(255 - c), q[2] = (uint8_t)(c * 7), q[3] = (uint8_t)a;
        }
    shr_image m = {rgba, N, N, 4 * N, sizeof(rgba), SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU};
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 0, NBUF, &drv), SHR_OK);
    uint32_t id = use(m);
    shr_surface s = packed(buf, SHR_FORMAT_RGB565, N, N);
    shr_draw_cmd c[2] = {reg(id), region(SHR_CMD_IMAGE, (shr_rect){0, 0, N, N}, m, (shr_rect){0, 0, N, N}, (shr_point){0, 0})};
    c[1].buffer = id;
    for (uint32_t d = 0; d < 64; d++) {
        uint32_t bg = (d & 31) << 11 | d << 5 | (31 - (d & 31));
        for (int32_t y = 0; y < N; y++)
            for (int32_t x = 0; x < N; x++) put(&s, x, y, bg);
        ASSERT_EQ_LL(drv.execute(drv.user, &s, c + (d > 0), 2 - (d > 0), 1), SHR_OK);
        for (int32_t y = 0; y < N; y++)
            for (int32_t x = 0; x < N; x++) {
                const uint8_t *q = rgba + 4 * (y * N + x);
                uint32_t fg = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
                ASSERT_EQ_LL(raw(&s, x, y), mix(SHR_FORMAT_RGB565, fg, bg, q[3]));
            }
    }
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

/* The plane: taken by the first IMAGE into RGB565 (not RGBX8888), kept across batches until a REGISTER or RELEASE of
 * its buffer, an UPDATE preparing its rect anew; pixels changed without one (which the rules forbid) show what is
 * kept. Without memory the IMAGE blends from the buffer. Rows without alpha, clipped and offset draws, keep groups. */
TEST image_planes_follow_buffers_and_memory(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    enum { W = 23, H = 7, ST = 4 * W + 8 }; /* rows padded */
    static uint8_t px[ST * H], old[sizeof(px)];
    for (size_t i = 0; i < sizeof(px); i++) px[i] = (uint8_t)rnd();
    for (int32_t x = 0; x < W; x++) px[2 * ST + 4 * x + 3] = 0, px[3 * ST + 4 * x + 3] = x == 9 ? 77 : 0;
    px[4 * ST + 3] = 0, px[4 * ST + 4 * (W - 1) + 3] = 0;
    shr_image m = {px, W, H, ST, sizeof(px), SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU};
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 0, 1, NBUF, &drv), SHR_OK);
    long base = f.live;
    uint32_t id = use(m);
    shr_image stale[NBUF];
    shr_draw_cmd c[5] = {reg(id), region(SHR_CMD_IMAGE, (shr_rect){3, 1, 3 + W, 1 + H}, m, (shr_rect){0, 0, W, H}, (shr_point){0, 0}),
                         region(SHR_CMD_IMAGE, (shr_rect){190, 2, 200, 6}, m, (shr_rect){2, 1, 22, 7}, (shr_point){7, 1}),
                         region(SHR_CMD_IMAGE, (shr_rect){50, 0, 55, 3}, m, (shr_rect){4, 2, 13, 7}, (shr_point){4, 0}),
                         region(SHR_CMD_IMAGE, (shr_rect){60, 1, 60, 3}, m, (shr_rect){0, 0, W, H}, (shr_point){0, 0})};
    for (int k = 1; k < 5; k++) c[k].buffer = id;
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGBX8888, c, 4, bufs));
    ASSERT_EQ_LL(f.live, base);
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c + 1, 4, bufs));
    ASSERT_EQ_LL(f.live, base + 1);
    memcpy(stale, bufs, sizeof(stale));
    stale[id - 1].pixels = old;
    memcpy(old, px, sizeof(px));
    for (int32_t x = 0; x < W; x++) px[3 * ST + 4 * x + 3] = (uint8_t)(x * 11), px[5 * ST + 4 * x] ^= 0x55;
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c + 1, 3, stale));
    c[0] = buf_cmd(SHR_CMD_BUFFER_UPDATE, id), c[0].src_rect = (shr_rect){0, 3, W, 6};
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c, 4, bufs));
    ASSERT_EQ_LL(f.live, base + 1);
    /* again in a keep group drawn into its keep, at its origin */
    shr_rect g = {2, 1, 62, 10};
    shr_draw_cmd k[8] = {kbegin(1, g), fill(g, SHR_RGB(20, 90, 160)), c[1], c[2], c[3], kend(),
                         fill((shr_rect){150, 0, 160, 2}, SHR_RGB(1, 2, 3)), kdraw(1, g, (shr_point){0, 0})};
    k[3].dst = (shr_rect){40, 2, 50, 6}, k[4].dst = (shr_rect){50, 1, 55, 4};
    static uint8_t ka[200 * 12 * 2], kb[sizeof(ka)];
    shr_surface sa = packed(ka, SHR_FORMAT_RGB565, 200, 12), sb = packed(kb, SHR_FORMAT_RGB565, 200, 12);
    ASSERT_EQ_LL(drv.execute(drv.user, &sa, k, 8, 1), SHR_OK);
    ASSERT_EQ_LL(exec(&sb, k + 1, 4), SHR_OK);
    ASSERT_EQ_LL(exec(&sb, k + 6, 1), SHR_OK);
    ASSERT_MEM_EQ(kb, ka, sizeof(ka));
    ASSERT_EQ_LL(f.live, base + 2); /* with the keep */
    c[0] = reg(id);
    memcpy(old, px, sizeof(px)), memset(px, 0x9C, sizeof(px));
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c, 4, bufs));
    ASSERT_EQ_LL(f.live, base + 2);
    shr_draw_cmd rel = buf_cmd(SHR_CMD_BUFFER_RELEASE, id);
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, &rel, 1, bufs));
    ASSERT_EQ_LL(f.live, base + 1);
    /* refused memory: blended from the buffer, nothing kept */
    f.budget = 0;
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c, 4, bufs));
    ASSERT_EQ_LL(f.live, base + 1);
    f.budget = -1;
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c + 1, 1, bufs));
    ASSERT_EQ_LL(f.live, base + 2);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

/* Planes within the byte cap: one that would pass it is not made and its draws blend from the buffer (pixels changed
 * without an UPDATE show which). RELEASE and REGISTER give the bytes back, an UPDATE prepares in place, a new cap frees
 * every plane, 0 makes none. */
TEST image_planes_keep_within_the_cap(void) {
    fail_alloc f = {-1, 0};
    shr_allocator al = fail_allocator(&f);
    enum { W = 10, H = 6 };
    static uint8_t pa[W * H * 4], pb[sizeof(pa)], pc[2 * sizeof(pa)], old[sizeof(pa)];
    for (size_t i = 0; i < sizeof(pa); i++) pa[i] = (uint8_t)rnd(), pb[i] = (uint8_t)rnd();
    for (size_t i = 0; i < sizeof(pc); i++) pc[i] = (uint8_t)rnd();
    shr_image ma = {pa, W, H, 4 * W, sizeof(pa), SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, mb = ma,
              mc = {pc, 2 * W, H, 8 * W, sizeof(pc), SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU};
    mb.pixels = pb;
    const size_t one = (W * H + H) * 8;
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 0, 0, NBUF, &drv), SHR_OK);
    ASSERT_EQ_LL(shr_software_driver_image_planes(&drv, one), SHR_OK);
    long base = f.live;
    uint32_t a = use(ma), b = use(mb);
    shr_draw_cmd c[4] = {reg(a), reg(b), region(SHR_CMD_IMAGE, (shr_rect){0, 0, W, H}, ma, (shr_rect){0, 0, W, H}, (shr_point){0, 0}),
                         region(SHR_CMD_IMAGE, (shr_rect){20, 1, 20 + W, 1 + H}, mb, (shr_rect){0, 0, W, H}, (shr_point){0, 0})};
    c[2].buffer = a, c[3].buffer = b;
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c, 4, bufs));
    ASSERT_EQ_LL(f.live, base + 1);
    shr_image stale[NBUF];
    memcpy(stale, bufs, sizeof(stale));
    stale[a - 1].pixels = old;
    memcpy(old, pa, sizeof(pa)), memset(pa, 0x3C, sizeof(pa)), memset(pb, 0xC3, sizeof(pb));
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c + 2, 2, stale)); /* a from its plane, b from the buffer */
    /* RELEASE gives a's bytes to b; a registered again finds no room */
    shr_draw_cmd rel[3] = {buf_cmd(SHR_CMD_BUFFER_RELEASE, a), c[3]};
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, rel, 2, bufs));
    ASSERT_EQ_LL(f.live, base + 1);
    shr_draw_cmd again[3] = {c[0], c[2], c[3]};
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, again, 3, bufs));
    ASSERT_EQ_LL(f.live, base + 1);
    memcpy(stale, bufs, sizeof(stale));
    stale[b - 1].pixels = old;
    memcpy(old, pb, sizeof(pb)), memset(pa, 0x77, sizeof(pa)), memset(pb, 0x99, sizeof(pb));
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, c + 2, 2, stale)); /* b from its plane, a from the buffer */
    rel[0] = buf_cmd(SHR_CMD_BUFFER_UPDATE, b), rel[0].src_rect = (shr_rect){0, 0, W, H};
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, rel, 2, bufs));
    ASSERT_EQ_LL(f.live, base + 1);
    /* b registered larger than the cap: its plane goes, a takes the room */
    bufs[b - 1] = mc;
    rel[0] = reg(b), rel[1] = c[3], rel[1].src_rect = (shr_rect){W / 2, 0, W / 2 + W, H}, rel[2] = c[2];
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, rel, 3, bufs));
    ASSERT_EQ_LL(f.live, base + 1);
    memcpy(stale, bufs, sizeof(stale));
    stale[a - 1].pixels = old;
    memcpy(old, pa, sizeof(pa)), memset(pa, 0x11, sizeof(pa)), memset(pc, 0x22, sizeof(pc));
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, rel + 1, 2, stale));
    /* a new cap frees every plane; one byte short of a plane, or 0, makes none */
    const size_t caps[2] = {one - 1, 0};
    for (int k = 0; k < 2; k++) {
        ASSERT_EQ_LL(shr_software_driver_image_planes(&drv, caps[k]), SHR_OK);
        ASSERT_EQ_LL(f.live, base);
        ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, rel + 1, 2, bufs));
        ASSERT_EQ_LL(f.live, base);
    }
    ASSERT_EQ_LL(shr_software_driver_image_planes(&drv, SIZE_MAX), SHR_OK);
    ASSERT(driver_draws(&drv, SHR_FORMAT_RGB565, rel + 1, 2, bufs));
    ASSERT_EQ_LL(f.live, base + 2);

    shr_framebuffer_driver other;
    shr_framebuffer_driver_init(&other);
    ASSERT_EQ_LL(shr_software_driver_image_planes(NULL, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_driver_image_planes(&other, 0), SHR_E_INVALID_ARG);
    shr_framebuffer_driver no_user = drv;
    no_user.user = NULL;
    ASSERT_EQ_LL(shr_software_driver_image_planes(&no_user, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

/* Entry e of shr__pals as 19 words: seq, key, palette. */
static void pal_words(const shr__pal *e, uint32_t *out) {
    out[0] = atomic_load(&e->seq), out[1] = atomic_load(&e->key[0]), out[2] = atomic_load(&e->key[1]);
    for (int j = 0; j < 16; j++) out[3 + j] = atomic_load(&e->pal[j]);
}

/* ON_FILL palettes kept by colour, bg, DIM and format: GLYPHs of more keys than shr__pals holds, each followed by the
 * one before (kept), the same colours with and without DIM in both formats, then all again while every entry is being
 * written (seq odd), which draws without the entries and leaves them as they were, all blend like the reference. */
TEST on_fill_palettes_are_kept_by_key(void) {
    enum { W = 16, KEYS = 2 * SHR__PAL_SETS * SHR__PAL_WAYS };
    static const uint8_t levels[W / 2] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    static uint32_t before[SHR__PAL_SETS * SHR__PAL_WAYS][19], after[19];
    const shr_image g = {levels, W, 1, W / 2, sizeof(levels), SHR_FORMAT_A4, 0};
    uint8_t buf[W * 4];
    for (int round = 0; round < 3; round++) {
        for (int k = 0; round == 2 && k < SHR__PAL_SETS * SHR__PAL_WAYS; k++) {
            shr__pal *e = &shr__pals[k / SHR__PAL_WAYS].way[k % SHR__PAL_WAYS];
            atomic_fetch_or(&e->seq, 1u);
            pal_words(e, before[k]);
        }
        for (uint32_t k = 1; k <= KEYS; k++)
            for (int i = 0; i < 2; i++)
                for (uint32_t dim = 0; dim < 2; dim++)
                    for (uint32_t back = 0; back < 2; back++) {
                        shr_color fg = (uint32_t)((uint64_t)(k - back) * 0x9E3779B1u);
                        shr_color bg = (uint32_t)((uint64_t)((k - back) / 3) * 0x85EBCA77u);
                        shr_surface s = packed(buf, FMTS[i], W, 1);
                        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, W, 1}, bg),
                                             glyph((shr_rect){0, 0, W, 1}, fg, g, (shr_point){0, 0})};
                        c[1].flags = SHR_GLYPH_ON_FILL | (dim ? SHR_GLYPH_DIM : 0), c[1].bg = bg;
                        ASSERT_EQ_LL(exec(&s, c, 2), SHR_OK);
                        for (int32_t x = 0; x < W; x++)
                            ASSERT_EQ_LL(raw(&s, x, 0), mix(FMTS[i], fg & 0xFFFFFF, enc(FMTS[i], bg & 0xFFFFFF),
                                                            dim ? (17u * (uint32_t)x + 1) >> 1 : 17u * (uint32_t)x));
                    }
    }
    for (int k = 0; k < SHR__PAL_SETS * SHR__PAL_WAYS; k++) {
        shr__pal *e = &shr__pals[k / SHR__PAL_WAYS].way[k % SHR__PAL_WAYS];
        pal_words(e, after);
        ASSERT_MEM_EQ(before[k], after, sizeof(after));
        atomic_fetch_add(&e->seq, 1u);
    }
    PASS();
}

/* ON_FILL palettes the driver keeps between commands: three times as many keys as it holds, in both formats, with and
 * without DIM, each drawn twice in a batch and again in a later batch after the others replaced it or not, all blend
 * like the reference. */
TEST on_fill_palettes_are_kept_by_the_driver(void) {
    enum { W = 16, KEYS = 3 * SHR__MEMO_PALS };
    static const uint8_t levels[W / 2] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    static uint8_t buf[W * 2 * 4];
    shr_image g = {levels, W, 1, W / 2, sizeof(levels), SHR_FORMAT_A4, 0};
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 0, 1, &drv), SHR_OK);
    shr_draw_cmd reg = cmd(SHR_CMD_BUFFER_REGISTER, (shr_rect){0, 0, 0, 0});
    reg.buffer = 1, reg.src = img_ref(g);
    shr_surface s = packed(buf, SHR_FORMAT_RGB565, W, 2);
    ASSERT_EQ_LL(drv.execute(drv.user, &s, &reg, 1, 1), SHR_OK);
    for (int round = 0; round < 2; round++)
        for (uint32_t k = 0; k < KEYS; k++)
            for (int i = 0; i < 2; i++)
                for (uint32_t dim = 0; dim < 2; dim++) {
                    shr_color fg = (uint32_t)((uint64_t)(k + 1) * 0x9E3779B1u), bg = (uint32_t)((uint64_t)(k / 5) * 0x85EBCA77u);
                    s = packed(buf, FMTS[i], W, 2);
                    shr_draw_cmd c[3] = {fill((shr_rect){0, 0, W, 2}, bg), cmd(SHR_CMD_GLYPH, (shr_rect){0, 0, W, 1}),
                                         cmd(SHR_CMD_GLYPH, (shr_rect){0, 1, W, 2})};
                    for (int j = 1; j < 3; j++) {
                        c[j].buffer = 1, c[j].src_rect = (shr_rect){0, 0, W, 1}, c[j].color = fg, c[j].bg = bg;
                        c[j].flags = SHR_GLYPH_ON_FILL | (dim ? SHR_GLYPH_DIM : 0);
                    }
                    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 3, 1), SHR_OK);
                    for (int32_t y = 0; y < 2; y++)
                        for (int32_t x = 0; x < W; x++)
                            ASSERT_EQ_LL(raw(&s, x, y), mix(FMTS[i], fg & 0xFFFFFF, enc(FMTS[i], bg & 0xFFFFFF),
                                                            dim ? (17u * (uint32_t)x + 1) >> 1 : 17u * (uint32_t)x));
                }
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    PASS();
}

/* Threads drawing ON_FILL GLYPHs of more keys than shr__pals holds, so entries are rewritten while others read them:
 * every pixel still blends like the reference. */
typedef struct pal_job {
    uint32_t rng;
    int bad;
} pal_job;

static void *pal_loop(void *arg) {
    static const uint8_t levels[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    const shr_image g = {levels, 16, 1, 8, sizeof(levels), SHR_FORMAT_A4, 0};
    pal_job *j = arg;
    uint8_t buf[16 * 4];
    for (int i = 0; i < 4000; i++) {
        uint32_t r = j->rng;
        r ^= (r & 0x7FFFFu) << 13, r ^= r >> 17, r ^= (r & 0x7FFFFFFu) << 5;
        uint32_t k = (j->rng = r) % 40, fi = r >> 8 & 1, dim = r >> 9 & 1;
        shr_color fg = (uint32_t)((uint64_t)(k + 1) * 0x9E3779B1u), bg = (uint32_t)((uint64_t)(k % 7 + 1) * 0x85EBCA77u);
        shr_surface s = packed(buf, FMTS[fi], 16, 1);
        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, 16, 1}, bg), cmd(SHR_CMD_GLYPH, (shr_rect){0, 0, 16, 1})};
        c[1].buffer = 1, c[1].src_rect = (shr_rect){0, 0, 16, 1}, c[1].color = fg, c[1].bg = bg;
        c[1].flags = SHR_GLYPH_ON_FILL | (dim ? SHR_GLYPH_DIM : 0);
        j->bad += shr_software_execute(&s, c, 2, &g, 1) != SHR_OK;
        for (uint32_t x = 0; x < 16; x++)
            j->bad += raw(&s, (int32_t)x, 0) !=
                      mix(FMTS[fi], fg & 0xFFFFFF, enc(FMTS[fi], bg & 0xFFFFFF), dim ? (17 * x + 1) >> 1 : 17 * x);
    }
    return NULL;
}

TEST on_fill_palettes_are_shared_between_threads(void) {
    pal_job jobs[4];
    pthread_t t[4];
    for (int i = 0; i < 4; i++) {
        jobs[i] = (pal_job){0x9E3779B9u + (uint32_t)i, 0};
        ASSERT_EQ_LL(pthread_create(&t[i], NULL, pal_loop, &jobs[i]), 0);
    }
    int bad = 0;
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL), bad += jobs[i].bad;
    ASSERT_EQ_LL(bad, 0);
    PASS();
}

TEST driver_create_and_destroy(void) {
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 4, NBUF, NULL), SHR_E_INVALID_ARG);
    shr_allocator half = {NULL, fa_alloc, NULL, 0};
    ASSERT_EQ_LL(shr_software_driver_create(&half, 0, 4, NBUF, &drv), SHR_E_INVALID_ARG);
    ASSERT(!drv.execute && !drv.user);
    const shr_allocator flagged = {NULL, fa_alloc, fa_free, ~(uint32_t)SHR_ALLOC_HOT};
    ASSERT_EQ_LL(shr_software_driver_create(&flagged, 0, 4, NBUF, &drv), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 4 * 128 - 1, 4, NBUF, &drv), SHR_E_INVALID_ARG); /* slots of 0 */
    ASSERT(!drv.execute && !drv.user);
    fail_alloc f = {0, 0};
    shr_allocator al = fail_allocator(&f);
    for (long budget = 0; budget < 6; budget++) { /* the driver, its buffer and plane tables, keeps, stores, copies */
        f.budget = budget;
        ASSERT_EQ_LL(shr_software_driver_create(&al, 0, 4, NBUF, &drv), SHR_E_NO_MEMORY);
        ASSERT_EQ_LL(f.live, 0);
        ASSERT(!drv.execute);
    }
    f.budget = -1;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 1 << 16, 4, NBUF, &drv), SHR_OK);
    ASSERT(drv.caps.domains == (SHR_MEMORY_CPU | SHR_MEMORY_DMA) && drv.caps.max_keep_bytes == 1 << 14);
    ASSERT(drv.execute && drv.reset && !drv.cancel && !drv.sync);
    ASSERT_EQ_LL(drv.reset(drv.user), SHR_OK);
    uint8_t buf[4 * 4];
    shr_surface s = packed(buf, SHR_FORMAT_RGBX8888, 4, 1);
    shr_rect half_row = {0, 0, 2, 1};
    shr_draw_cmd c[5] = {fill((shr_rect){0, 0, 4, 1}, SHR_RGB(1, 1, 1)), kbegin(1, half_row),
                         fill(half_row, SHR_RGB(2, 2, 2)), kend(), kdraw(1, half_row, (shr_point){0, 0})};
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 5, 1), SHR_OK);
    ASSERT_EQ_LL(raw(&s, 1, 0), 0x020202FF);
    ASSERT_EQ_LL(raw(&s, 2, 0), 0x010101FF);
    ASSERT(f.live > 4); /* the keep's pixels, freed with the driver */

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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1, 0, 0, &drv), SHR_OK); /* default allocator, no keeps, no buffers */
    ASSERT(drv.caps.max_buffers == 0 && drv.caps.max_keeps == 0 && drv.caps.keep_bytes == 1 && drv.caps.max_keep_bytes == 0);
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 1, 1), SHR_OK);
    ASSERT_EQ_LL(drv.execute(drv.user, &s, c, 5, 1), SHR_E_INVALID_ARG);
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
    RUN_TEST(fill_rows_of_any_width_and_alignment);
    RUN_TEST(fill_dim_blends_at_half_strength);
    RUN_TEST(glyph_a8_blends_every_coverage);
    RUN_TEST(rgb565_blends_round_once_for_every_input);
    RUN_TEST(glyph_a4_nibbles_origin_and_dim);
    RUN_TEST(glyph_a8_dim_keeps_faint_coverage);
    RUN_TEST(glyph_coverage_runs_blend_exactly);
    RUN_TEST(synth_hand_vectors);
    RUN_TEST(synth_matches_the_spec_reference);
    RUN_TEST(synth_wide_rows_match_the_spec_reference);
    RUN_TEST(synth_is_clip_invariant);
    RUN_TEST(synth_sources_stay_in_the_footprint);
    RUN_TEST(synth_size_and_axis_limits);
    RUN_TEST(synth_edge_sources);
    RUN_TEST(image_is_straight_alpha_source_over);
    RUN_TEST(image_alpha_runs_blend_exactly);
    RUN_TEST(copy_converts_between_formats);
    RUN_TEST(copy_scrolls_within_one_surface);
    RUN_TEST(copy_down_in_runs);
    RUN_TEST(copy_overlap_rules);
    RUN_TEST(rotate_maps_every_rotation);
    RUN_TEST(rotate_rejects_bad_geometry_and_sources);
    RUN_TEST(check_uses_the_driver_reach);
    RUN_TEST(buffers_register_replace_and_release);
    RUN_TEST(buffer_commands_are_validated);
    RUN_TEST(buffer_regions_are_validated);
    RUN_TEST(execute_rejects_bad_batches_without_drawing);
    RUN_TEST(keep_commands_are_validated);
    RUN_TEST(keeps_draw_what_groups_draw);
    RUN_TEST(keep_memory);
    RUN_TEST(keep_slots);
    RUN_TEST(copier_orders_keep_copies);
    RUN_TEST(copier_trims_keep_draws);
    RUN_TEST(glyphs_on_fills_match_blending);
    RUN_TEST(on_fill_palettes_are_kept_by_key);
    RUN_TEST(on_fill_palettes_are_kept_by_the_driver);
    RUN_TEST(on_fill_palettes_are_shared_between_threads);
    RUN_TEST(synth_cache_draws_what_synthesis_draws);
    RUN_TEST(synth_cache_follows_buffers_and_memory);
    RUN_TEST(synth_cache_keys_and_luts);
    RUN_TEST(synth_cache_indexes_live_entries);
    RUN_TEST(image_planes_blend_exactly);
    RUN_TEST(image_planes_follow_buffers_and_memory);
    RUN_TEST(image_planes_keep_within_the_cap);
    RUN_TEST(driver_create_and_destroy);
    RUN_TEST(asset_source_file_reads_and_closes);
    GREATEST_MAIN_END();
}
