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

static uint32_t mix(shr_pixel_format f, uint32_t fg, uint32_t bg_raw, uint32_t a) {
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

static shr_draw_cmd with_src(shr_cmd_kind kind, shr_rect r, shr_image src, shr_point origin) {
    shr_draw_cmd c = cmd(kind, r);
    c.src = src;
    c.src_origin = origin;
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
        ASSERT_EQ_LL(shr_software_execute(&s, &c, 1), SHR_OK);
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
        ASSERT_EQ_LL(shr_software_execute(&s, c, 2), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_execute(&s, &c, 1), SHR_OK);
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
        ASSERT_EQ_LL(shr_software_execute(&s, c, 2), SHR_OK);
        uint32_t bg = enc(FMTS[i], 0x0A3CFA);
        ASSERT_EQ_LL(raw(&s, 0, 0), bg); /* zero coverage leaves the pixel */
        for (uint32_t a = 1; a < 256; a++) ASSERT_EQ_LL(raw(&s, (int32_t)a, 0), mix(FMTS[i], 0xFF8007, bg, a));
        ASSERT_EQ_LL(raw(&s, 255, 0), enc(FMTS[i], 0xFF8007));
    }
    PASS();
}

TEST glyph_a4_nibbles_origin_and_dim(void) {
    /* Two rows of 3 pixels, stride 3: high nibble = left pixel; the source is read from (1, 1). */
    const uint8_t a4[6] = {0x00, 0x00, 0x00, 0x0F, 0x81, 0x00};
    shr_image m = {a4, 3, 2, 3, 6, SHR_FORMAT_A4, SHR_MEMORY_CPU};
    for (int i = 0; i < 2; i++) {
        uint8_t buf[3 * 4];
        shr_surface s = packed(buf, FMTS[i], 3, 1);
        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, 3, 1}, SHR_RGB(0, 0, 0)),
                             glyph((shr_rect){1, 0, 3, 1}, SHR_RGB(255, 255, 255), m, (shr_point){1, 1})};
        ASSERT_EQ_LL(shr_software_execute(&s, c, 2), SHR_OK);
        ASSERT_EQ_LL(raw(&s, 0, 0), enc(FMTS[i], 0));
        ASSERT_EQ_LL(raw(&s, 1, 0), enc(FMTS[i], 0xFFFFFF));             /* 15 -> 255 */
        ASSERT_EQ_LL(raw(&s, 2, 0), mix(FMTS[i], 0xFFFFFF, enc(FMTS[i], 0), 8 * 17));
        c[1].flags = SHR_GLYPH_DIM; /* coverage (a + 1) / 2 after expansion, not nibble halving */
        ASSERT_EQ_LL(shr_software_execute(&s, c, 2), SHR_OK);
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
        ASSERT_EQ_LL(shr_software_execute(&s, c, 2), SHR_OK);
        uint32_t black = enc(FMTS[i], 0);
        ASSERT_EQ_LL(raw(&s, 0, 0), black);
        ASSERT_EQ_LL(raw(&s, 1, 0), mix(FMTS[i], 0xFFFFFF, black, 1));
        ASSERT_EQ_LL(raw(&s, 2, 0), mix(FMTS[i], 0xFFFFFF, black, 128));
    }
    PASS();
}

TEST image_is_straight_alpha_source_over(void) {
    /* Second row, read from origin (0, 1): alpha 0, 128, 255, 1. */
    const uint8_t rgba[2 * 16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  255, 0, 0, 0, 0, 255, 0, 128, 12, 34, 56, 255, 255, 255, 255, 1};
    shr_image m = {rgba, 4, 2, 16, 32, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU};
    for (int i = 0; i < 2; i++) {
        uint8_t buf[4 * 4];
        shr_surface s = packed(buf, FMTS[i], 4, 1);
        shr_draw_cmd c[2] = {fill((shr_rect){0, 0, 4, 1}, SHR_RGB(100, 50, 200)),
                             with_src(SHR_CMD_IMAGE, (shr_rect){0, 0, 4, 1}, m, (shr_point){0, 1})};
        ASSERT_EQ_LL(shr_software_execute(&s, c, 2), SHR_OK);
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
            ASSERT_EQ_LL(shr_software_execute(&dst, &c, 1), SHR_OK);
            ASSERT_EQ_LL(raw(&dst, 0, 0), 0);
            for (int x = 1; x < 3; x++) ASSERT_EQ_LL(raw(&dst, x, 0), enc(FMTS[k], dec(FMTS[i], raw(&src, x - 1, 0))));
        }
    }
    uint8_t in[4] = {255, 0, 255, 255};
    uint16_t out = 0;
    shr_surface d = packed(&out, SHR_FORMAT_RGB565, 1, 1);
    shr_draw_cmd c = with_src(SHR_CMD_COPY, (shr_rect){0, 0, 1, 1},
                              (shr_image){in, 1, 1, 4, 4, SHR_FORMAT_RGBX8888, SHR_MEMORY_CPU}, (shr_point){0, 0});
    ASSERT_EQ_LL(shr_software_execute(&d, &c, 1), SHR_OK);
    ASSERT_EQ_LL(out, 0xF81F);
    PASS();
}

TEST copy_scrolls_within_one_surface(void) {
    for (int i = 0; i < 2; i++) {
        uint8_t buf[4 * 4 * 4];
        shr_surface s = packed(buf, FMTS[i], 4, 4);
        for (int y = 0; y < 4; y++) {
            shr_draw_cmd f = fill((shr_rect){0, y, 4, y + 1}, SHR_RGB(y * 60, 0, 0));
            shr_software_execute(&s, &f, 1);
        }
        size_t row = s.stride;
        shr_draw_cmd up = with_src(SHR_CMD_COPY, (shr_rect){0, 0, 4, 3},
                                   (shr_image){buf + row, 4, 3, row, row * 3, FMTS[i], SHR_MEMORY_CPU}, (shr_point){0, 0});
        ASSERT_EQ_LL(shr_software_execute(&s, &up, 1), SHR_OK);
        for (int y = 0; y < 3; y++) ASSERT_EQ_LL(raw(&s, 3, y), enc(FMTS[i], (uint32_t)(y + 1) * 60 << 16));
        shr_draw_cmd down = with_src(SHR_CMD_COPY, (shr_rect){0, 1, 4, 4},
                                     (shr_image){buf, 4, 3, row, row * 3, FMTS[i], SHR_MEMORY_CPU}, (shr_point){0, 0});
        ASSERT_EQ_LL(shr_software_execute(&s, &down, 1), SHR_OK);
        for (int y = 1; y < 4; y++) ASSERT_EQ_LL(raw(&s, 0, y), enc(FMTS[i], (uint32_t)y * 60 << 16));
        /* Horizontal overlap on one row: memmove semantics. */
        shr_draw_cmd right = with_src(SHR_CMD_COPY, (shr_rect){1, 0, 4, 1}, as_image(&s), (shr_point){0, 0});
        put(&s, 0, 0, enc(FMTS[i], 0x0000FF));
        ASSERT_EQ_LL(shr_software_execute(&s, &right, 1), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_execute(&d, &cp, 1), SHR_E_UNSUPPORTED);
    cp.src.pixels = buf + 64; /* disjoint, after the destination */
    ASSERT_EQ_LL(shr_software_execute(&d, &cp, 1), SHR_OK);
    shr_surface high = d;
    high.pixels = buf + 64;
    cp.src.pixels = buf; /* disjoint, before it */
    ASSERT_EQ_LL(shr_software_execute(&high, &cp, 1), SHR_OK);
    /* Empty rectangles touch nothing, so they never overlap. */
    cp.src.pixels = buf + 2;
    cp.dst = (shr_rect){1, 0, 1, 1};
    ASSERT_EQ_LL(shr_software_execute(&d, &cp, 1), SHR_OK);
    cp.dst = (shr_rect){0, 1, 2, 1};
    ASSERT_EQ_LL(shr_software_execute(&d, &cp, 1), SHR_OK);
    /* Same format but another stride: row order cannot avoid clobbering. */
    shr_surface x = surf(buf, SHR_FORMAT_RGBX8888, 4, 4, 16);
    cp.dst = (shr_rect){0, 0, 2, 2};
    cp.src = (shr_image){buf + 4, 2, 2, 12, 24, SHR_FORMAT_RGBX8888, SHR_MEMORY_CPU};
    ASSERT_EQ_LL(shr_software_execute(&x, &cp, 1), SHR_E_UNSUPPORTED);
    cp.src = (shr_image){buf + 64, 2, 2, 8, 16, SHR_FORMAT_RGBX8888, SHR_MEMORY_DEVICE};
    ASSERT_EQ_LL(shr_software_execute(&x, &cp, 1), SHR_E_UNSUPPORTED);
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
            ASSERT_EQ_LL(shr_software_execute(&d, &r, 1), SHR_OK);
            /* Logical (x, y) lands on output (h - 1 - y, x). */
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 3; x++) ASSERT_EQ_LL(raw(&d, 1 - y, x), SRC(x, y));
            r = rotate(&s, (shr_rect){0, 0, 2, 3}, SHR_ROTATE_90_CCW);
            ASSERT_EQ_LL(shr_software_execute(&d, &r, 1), SHR_OK);
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 3; x++) ASSERT_EQ_LL(raw(&d, y, 2 - x), SRC(x, y));
            shr_surface e = packed(back, FMTS[k], 3, 2);
            memset(back, 0, sizeof(back));
            r = rotate(&s, (shr_rect){1, 0, 3, 2}, SHR_ROTATE_180); /* only part of the output */
            ASSERT_EQ_LL(shr_software_execute(&e, &r, 1), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_execute(&d, &r, 1), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_execute(&d, &r, 1), SHR_E_INVALID_ARG);
    r = rotate(&s, all, SHR_ROTATE_180);
    r.src.domain = SHR_MEMORY_DEVICE;
    ASSERT_EQ_LL(shr_software_execute(&d, &r, 1), SHR_E_UNSUPPORTED);
    uint8_t a8[6];
    r.src = (shr_image){a8, 3, 2, 3, 6, SHR_FORMAT_A8, SHR_MEMORY_CPU};
    ASSERT_EQ_LL(shr_software_execute(&d, &r, 1), SHR_E_UNSUPPORTED);
    const shr_rotation bad_rot[2] = {SHR_ROTATE_NONE, (shr_rotation)7};
    for (int i = 0; i < 2; i++) {
        r = rotate(&s, all, bad_rot[i]);
        ASSERT_EQ_LL(shr_software_execute(&d, &r, 1), SHR_E_INVALID_ARG);
    }
    r = rotate(&s, all, SHR_ROTATE_90_CW); /* needs a 2x3 output */
    ASSERT_EQ_LL(shr_software_execute(&d, &r, 1), SHR_E_INVALID_ARG);
    r = rotate(&s, (shr_rect){0, 0, 2, 2}, SHR_ROTATE_180);
    shr_surface narrow = packed(dst, SHR_FORMAT_RGB565, 2, 2), flat = packed(dst, SHR_FORMAT_RGB565, 3, 1);
    ASSERT_EQ_LL(shr_software_execute(&narrow, &r, 1), SHR_E_INVALID_ARG);
    r.dst = (shr_rect){0, 0, 3, 1};
    ASSERT_EQ_LL(shr_software_execute(&flat, &r, 1), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_execute(&s, &r, 1), SHR_E_UNSUPPORTED); /* in place */
    ASSERT_MEM_EQ(zero, dst, sizeof(zero));
    /* An offset view of the same pixels overlaps; a disjoint one is fine, as is an empty rect. */
    uint16_t px[16] = {0};
    shr_surface a = packed(px, SHR_FORMAT_RGB565, 2, 2), b = packed(px + 2, SHR_FORMAT_RGB565, 2, 2);
    r = rotate(&a, (shr_rect){0, 0, 2, 2}, SHR_ROTATE_180);
    ASSERT_EQ_LL(shr_software_execute(&b, &r, 1), SHR_E_UNSUPPORTED);
    r.dst = (shr_rect){1, 1, 1, 2};
    ASSERT_EQ_LL(shr_software_execute(&b, &r, 1), SHR_OK);
    b = packed(px + 4, SHR_FORMAT_RGB565, 2, 2);
    r.dst = (shr_rect){0, 0, 2, 2};
    ASSERT_EQ_LL(shr_software_execute(&b, &r, 1), SHR_OK);
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
        ASSERT_EQ_LL(shr_software_execute(&s, c, 2), SHR_E_INVALID_ARG);
    }
    shr_draw_cmd c[2] = {ok, cmd((shr_cmd_kind)0, (shr_rect){0, 0, 1, 1})};
    ASSERT_EQ_LL(shr_software_execute(&s, c, 2), SHR_E_INVALID_ARG);
    c[1].kind = (shr_cmd_kind)99;
    ASSERT_EQ_LL(shr_software_execute(&s, c, 2), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_execute(&s, NULL, 1), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_software_execute(&s, NULL, 0), SHR_OK);
    ASSERT_EQ_LL(shr_software_execute(NULL, &ok, 1), SHR_E_INVALID_ARG);
    shr_surface bad = s;
    bad.stride = 8;
    ASSERT_EQ_LL(shr_software_execute(&bad, &ok, 1), SHR_E_INVALID_ARG);
    bad = s;
    bad.format = SHR_FORMAT_A8;
    ASSERT_EQ_LL(shr_software_execute(&bad, &ok, 1), SHR_E_INVALID_ARG);
    bad = s;
    bad.domain = SHR_MEMORY_DEVICE;
    ASSERT_EQ_LL(shr_software_execute(&bad, &ok, 1), SHR_E_UNSUPPORTED);

    uint8_t a8[4] = {0};
    const struct {
        shr_cmd_kind kind;
        shr_image src;
        shr_point origin;
        shr_status want;
    } srcs[] = {
        {SHR_CMD_GLYPH, {buf, 1, 1, 4, 4, SHR_FORMAT_RGBX8888, SHR_MEMORY_CPU}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_GLYPH, {a8, 2, 2, 2, 4, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_IMAGE, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_COPY, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_COPY, {a8, 1, 1, 4, 4, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_GLYPH, {a8, 2, 2, 1, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {0, 0}, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_DEVICE}, {0, 0}, SHR_E_UNSUPPORTED},
        {SHR_CMD_GLYPH, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {-1, 0}, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {0, -1}, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {2, 0}, SHR_E_INVALID_ARG},
        {SHR_CMD_GLYPH, {a8, 2, 2, 2, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, {0, 2}, SHR_E_INVALID_ARG},
        {SHR_CMD_IMAGE, {a8, 1, 1, 4, 4, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, {1, 0}, SHR_E_INVALID_ARG},
    };
    for (size_t i = 0; i < sizeof(srcs) / sizeof(srcs[0]); i++) {
        c[1] = with_src(srcs[i].kind, (shr_rect){0, 0, 1, 1}, srcs[i].src, srcs[i].origin);
        ASSERT_EQ_LL(shr_software_execute(&s, c, 2), srcs[i].want);
    }
    ASSERT_MEM_EQ(zero, buf, sizeof(buf));
    /* Empty rectangles are valid and draw nothing. */
    shr_draw_cmd empty[2] = {fill((shr_rect){1, 1, 1, 3}, SHR_RGB(9, 9, 9)), fill((shr_rect){1, 1, 3, 1}, SHR_RGB(9, 9, 9))};
    ASSERT_EQ_LL(shr_software_execute(&s, empty, 2), SHR_OK);
    ASSERT_MEM_EQ(zero, buf, sizeof(buf));
    shr_surface none = packed(buf, SHR_FORMAT_RGB565, 0, 0);
    shr_draw_cmd nothing = fill((shr_rect){0, 0, 0, 0}, 0);
    ASSERT_EQ_LL(shr_software_execute(&none, &nothing, 1), SHR_OK);
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
    };
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 16, &drv), SHR_OK);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERT_EQ_LL(shr_software_execute(&s, bad[i], 4), SHR_E_INVALID_ARG);
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
        ASSERT_EQ_LL(shr_software_execute(&mid, ok, 3), SHR_OK);
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
        ASSERT_EQ_LL(shr_software_execute(&s, c, 5), SHR_OK);
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 4; x++) {
                uint32_t want = y == 2 ? 0x0000FF : (x == 0 && y == 0) ? 0x00FF00 : (y == 1 && x >= 1 && x < 3) ? 0xFF0000 : 0;
                ASSERT_EQ_LL(raw(&s, x, y), want ? enc(FMTS[i], want) : 0);
            }
    }
    PASS();
}

/* A group whose content depends on the group origin: fill, glyph, image and copy at offsets. */
static const uint8_t g_a8[4] = {0, 85, 170, 255};
static const uint8_t g_rgba[8] = {255, 255, 0, 255, 0, 255, 255, 100};
static uint8_t g_copy[2 * 4];

static size_t group(shr_draw_cmd *c, uint64_t key, shr_rect dst, shr_rect clip, shr_pixel_format f) {
    shr_surface cs = packed(g_copy, f, 2, 1);
    put(&cs, 0, 0, enc(f, 0x804020));
    put(&cs, 1, 0, enc(f, 0x102040));
    int32_t x = dst.x0, y = dst.y0;
    c[0] = begin(key, dst, clip);
    c[1] = fill(dst, SHR_RGB(40, 40, 40));
    c[2] = glyph((shr_rect){x + 1, y, x + 5, y + 1}, SHR_RGB(255, 255, 255),
                 (shr_image){g_a8, 4, 1, 4, 4, SHR_FORMAT_A8, SHR_MEMORY_CPU}, (shr_point){0, 0});
    c[3] = with_src(SHR_CMD_IMAGE, (shr_rect){x, y + 1, x + 2, y + 2},
                    (shr_image){g_rgba, 2, 1, 8, 8, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, (shr_point){0, 0});
    c[4] = with_src(SHR_CMD_COPY, (shr_rect){x + 4, y + 1, x + 6, y + 2}, as_image(&cs), (shr_point){0, 0});
    c[5] = end();
    return 6;
}

TEST cached_driver_hit_matches_direct_drawing(void) {
    for (int i = 0; i < 2; i++) {
        uint8_t ref[8 * 4 * 4], a[sizeof(ref)], b[sizeof(ref)];
        memset(ref, 0x11, sizeof(ref)), memset(a, 0x11, sizeof(a)), memset(b, 0x11, sizeof(b));
        shr_surface sr = packed(ref, FMTS[i], 8, 4), sa = packed(a, FMTS[i], 8, 4), sb = packed(b, FMTS[i], 8, 4);
        shr_draw_cmd c[6];
        size_t n = group(c, 42, (shr_rect){1, 1, 7, 3}, (shr_rect){2, 1, 7, 3}, FMTS[i]);
        ASSERT_EQ_LL(shr_software_execute(&sr, c, n), SHR_OK);
        shr_framebuffer_driver drv;
        ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 16, &drv), SHR_OK);
        ASSERT_EQ_LL(drv.execute(drv.user, &sa, c, n, 1), SHR_OK); /* miss: renders and stores */
        ASSERT_MEM_EQ(ref, a, sizeof(ref));
        ASSERT_EQ_LL(drv.execute(drv.user, &sb, c, n, 2), SHR_OK); /* hit */
        ASSERT_MEM_EQ(ref, b, sizeof(ref));
        /* A hit copies the stored pixels: same key and size with other commands still draws the old content. */
        c[1].color = SHR_RGB(0, 0, 0);
        memset(b, 0x11, sizeof(b));
        ASSERT_EQ_LL(drv.execute(drv.user, &sb, c, n, 3), SHR_OK);
        ASSERT_MEM_EQ(ref, b, sizeof(ref));
        /* Same key and size at another position still hits. */
        n = group(c, 42, (shr_rect){2, 2, 8, 4}, (shr_rect){2, 2, 8, 4}, FMTS[i]);
        c[1].color = SHR_RGB(0, 0, 0);
        ASSERT_EQ_LL(drv.execute(drv.user, &sb, c, n, 4), SHR_OK);
        ASSERT_EQ_LL(raw(&sb, 2, 2), enc(FMTS[i], 0x282828));
        /* Another size misses. */
        n = group(c, 42, (shr_rect){0, 0, 7, 2}, (shr_rect){0, 0, 7, 2}, FMTS[i]);
        c[1].color = SHR_RGB(0, 0, 0);
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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 16, &drv), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(&al, KEYED_BUDGET, &drv), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(&al, KEYED_BUDGET, &drv), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, KEYED_BUDGET, &drv), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(&al, 20000, &drv), SHR_OK);
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
        ASSERT_EQ_LL(shr_software_driver_create(&al, budget, &drv), SHR_OK);
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
        ASSERT_EQ_LL(shr_software_driver_create(&al, 1 << 20, &drv), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(&al, 1 << 20, &drv), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 20, &drv), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 16, &drv), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1 << 20, &drv), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(&al, 100 * 80, &drv), SHR_OK); /* about 100 single pixels */
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
            shr_draw_cmd c[6];
            size_t n = group(c, 9, (shr_rect){0, 1, 6, 3}, (shr_rect){1, 1, 5, 2}, FMTS[k]);
            ASSERT_EQ_LL(shr_software_execute(&sr, c, n), SHR_OK);
            f.budget = -1;
            shr_framebuffer_driver drv;
            ASSERT_EQ_LL(shr_software_driver_create(&al, cases[i].budget, &drv), SHR_OK);
            f.budget = cases[i].alloc_budget;
            ASSERT_EQ_LL(drv.execute(drv.user, &so, c, n, 1), SHR_OK);
            ASSERT_MEM_EQ(ref, out, sizeof(ref));
            c[1].color = SHR_RGB(1, 2, 3); /* nothing was stored: the next draw shows the new content */
            ASSERT_EQ_LL(shr_software_execute(&sr, c, n), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, NULL), SHR_E_INVALID_ARG);
    shr_allocator half = {NULL, fa_alloc, NULL};
    ASSERT_EQ_LL(shr_software_driver_create(&half, 0, &drv), SHR_E_INVALID_ARG);
    ASSERT(!drv.execute && !drv.user);
    fail_alloc f = {0, 0};
    shr_allocator al = fail_allocator(&f);
    ASSERT_EQ_LL(shr_software_driver_create(&al, 0, &drv), SHR_E_NO_MEMORY);
    ASSERT_EQ_LL(f.live, 0);
    ASSERT(!drv.execute);
    f.budget = 1; /* the table comes with the first entry */
    ASSERT_EQ_LL(shr_software_driver_create(&al, 1024, &drv), SHR_OK);
    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
    f.budget = -1;
    ASSERT_EQ_LL(shr_software_driver_create(&al, 1 << 16, &drv), SHR_OK);
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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, &drv), SHR_OK); /* default allocator, no cache */
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
    RUN_TEST(glyph_a4_nibbles_origin_and_dim);
    RUN_TEST(glyph_a8_dim_keeps_faint_coverage);
    RUN_TEST(image_is_straight_alpha_source_over);
    RUN_TEST(copy_converts_between_formats);
    RUN_TEST(copy_scrolls_within_one_surface);
    RUN_TEST(copy_overlap_rules);
    RUN_TEST(rotate_maps_every_rotation);
    RUN_TEST(rotate_rejects_bad_geometry_and_sources);
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
