#include "shr_scale.h"

#include <string.h>

#define INLINE static inline __attribute__((always_inline))

INLINE void texel(uint32_t f, const uint8_t *row, int32_t x, uint8_t out[4]) {
    shr__src_rgba(f, row + (size_t)x * shr__src_bytes(f), out);
}

#define LANES 0x00FF00FFu

void shr__bilin_start(shr__bilin *b, const shr__scale *s, int32_t x0, int32_t n, int32_t y) {
    int32_t sw = s->src.x1 - s->src.x0, sh = s->src.y1 - s->src.y0, last = s->w - 1;
    uint32_t den = (uint32_t)s->dw, step = (uint32_t)sw << 16, dq = step / den, dr = step % den;
    uint64_t num = ((uint64_t)(2 * x0 + 1) * (uint32_t)sw + den) << 15;
    uint32_t t = (uint32_t)(num / den), r = (uint32_t)(num % den);
    for (int32_t i = 0; i < n; i++) {
        int32_t k = s->src.x0 + (int32_t)(t >> 16) - 1;
        int32_t xa = k < 0 ? 0 : k > last ? last : k, xb = k + 1 > last ? last : k + 1;
        b->tap[i] = (uint32_t)xa << 9 | (uint32_t)(xb - xa) << 8 | (t >> 8 & 255);
        t += dq, r += dr;
        if (r >= den) r -= den, t++;
    }
    den = (uint32_t)s->dh, step = (uint32_t)sh << 16;
    num = ((uint64_t)(2 * y + 1) * (uint32_t)sh + den) << 15;
    b->s = s, b->n = n, b->held[0] = b->held[1] = -1;
    b->t = (uint32_t)(num / den), b->r = (uint32_t)(num % den), b->dq = step / den, b->dr = step % den;
}

/* Source pixel p of format f as an RGBA8888 word (R in the low byte). */
INLINE uint32_t texel_word(uint32_t f, const uint8_t *p) {
    uint32_t v;
    if (f == SHR_IMAGE_SRC_RGBA8888) {
        memcpy(&v, p, 4);
    } else {
        uint8_t c[4];
        shr__src_rgba(f, p, c);
        v = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24;
    }
    return v;
}

INLINE void fill_as(const shr__bilin *b, const uint8_t *row, uint32_t *restrict h, uint32_t f) {
    size_t bpp = shr__src_bytes(f);
    for (int32_t i = 0; i < b->n; i++) {
        uint32_t t = b->tap[i], fx = t & 255, gx = 256 - fx;
        const uint8_t *a = row + (size_t)(t >> 9) * bpp;
        uint32_t p = texel_word(f, a), q = texel_word(f, a + (t >> 8 & 1) * bpp);
        h[2 * i] = (p & LANES) * gx + (q & LANES) * fx;
        h[2 * i + 1] = (p >> 8 & LANES) * gx + (q >> 8 & LANES) * fx;
    }
}

/* The horizontal blend of source row y into h; RGBA sources (every driver buffer) with the format a constant. */
static void bilin_fill(const shr__bilin *b, int32_t y, uint32_t *restrict h) {
    const uint8_t *row = b->s->pixels + (size_t)y * b->s->stride;
    if (b->s->format == SHR_IMAGE_SRC_RGBA8888)
        fill_as(b, row, h, SHR_IMAGE_SRC_RGBA8888);
    else
        fill_as(b, row, h, b->s->format);
}

const uint32_t *shr__bilin_rows(shr__bilin *b, const uint32_t **bot, uint32_t *fy) {
    const shr__scale *s = b->s;
    int32_t k = s->src.y0 + (int32_t)(b->t >> 16) - 1, last = s->h - 1;
    int32_t ya = k < 0 ? 0 : k > last ? last : k, yb = k + 1 > last ? last : k + 1;
    *fy = b->t >> 8 & 255;
    b->t += b->dq, b->r += b->dr;
    if (b->r >= (uint32_t)s->dh) b->r -= (uint32_t)s->dh, b->t++;
    int ia = b->held[0] == ya ? 0 : b->held[1] == ya ? 1 : -1, ib = b->held[0] == yb ? 0 : b->held[1] == yb ? 1 : -1;
    if (ia < 0) ia = ib == 0, bilin_fill(b, ya, b->h[ia]), b->held[ia] = ya;
    if (ib < 0 && ya == yb) ib = ia;
    if (ib < 0) ib = !ia, bilin_fill(b, yb, b->h[ib]), b->held[ib] = yb;
    *bot = b->h[ib];
    return b->h[ia];
}

void shr__bilin_next(shr__bilin *b, uint32_t *out) {
    const uint32_t *bot;
    uint32_t fy;
    const uint32_t *top = shr__bilin_rows(b, &bot, &fy);
    for (int32_t i = 0; i < b->n; i++)
        out[i] = shr__bilin_lerp(top[2 * i], bot[2 * i], fy) | shr__bilin_lerp(top[2 * i + 1], bot[2 * i + 1], fy) << 8;
}

static void row_bilinear(const shr__scale *s, int32_t y, int32_t x0, int32_t n, uint8_t *out) {
    shr__bilin b;
    uint32_t px[SHR__SCALE_SPAN];
    for (int32_t x = 0; x < n; x += SHR__SCALE_SPAN) {
        int32_t m = n - x < SHR__SCALE_SPAN ? n - x : SHR__SCALE_SPAN;
        shr__bilin_start(&b, s, x0 + x, m, y);
        shr__bilin_next(&b, px);
        memcpy(out + (size_t)x * 4, px, (size_t)m * 4);
    }
}

/* Source span [*a, *b) in 1/256 px that scaled index i covers on a shrinking axis. */
static void box_span(int32_t s, int32_t d, int32_t i, int32_t *a, int32_t *b) {
    *a = (int32_t)(((int64_t)i * s * 256) / d), *b = (int32_t)(((int64_t)(i + 1) * s * 256) / d);
}

static uint32_t box_w(int32_t a, int32_t b, int32_t k) {
    int32_t lo = a > k * 256 ? a : k * 256, hi = b < (k + 1) * 256 ? b : (k + 1) * 256;
    return (uint32_t)(hi - lo);
}

/* Per axis the overlap weights where it shrinks, else the two bilinear taps; round(sum w c / sum w). */
INLINE void px_box(const shr__scale *s, uint32_t f, int32_t i, int32_t j, uint8_t *out) {
    int32_t sw = s->src.x1 - s->src.x0, sh = s->src.y1 - s->src.y0, xk[2], yk[2], ax = 0, bx = 0, ay = 0, by = 0;
    uint32_t fx, fy;
    shr__scale_taps(s->src.x0, sw, s->dw, s->w, i, &xk[0], &xk[1], &fx);
    shr__scale_taps(s->src.y0, sh, s->dh, s->h, j, &yk[0], &yk[1], &fy);
    bool hx = sw > s->dw, hy = sh > s->dh;
    int32_t kx0 = 0, kx1 = 1, ky0 = 0, ky1 = 1;
    if (hx) box_span(sw, s->dw, i, &ax, &bx), kx0 = ax >> 8, kx1 = (bx - 1) >> 8;
    if (hy) box_span(sh, s->dh, j, &ay, &by), ky0 = ay >> 8, ky1 = (by - 1) >> 8;
    uint64_t acc[4] = {0, 0, 0, 0}, den = 0;
    for (int32_t ky = ky0; ky <= ky1; ky++) {
        uint32_t wy = hy ? box_w(ay, by, ky) : ky ? fy : 256 - fy;
        const uint8_t *row = s->pixels + (size_t)(hy ? s->src.y0 + ky : yk[ky]) * s->stride;
        for (int32_t kx = kx0; kx <= kx1; kx++) {
            uint64_t w = (uint64_t)(hx ? box_w(ax, bx, kx) : kx ? fx : 256 - fx) * wy;
            uint8_t p[4];
            texel(f, row, hx ? s->src.x0 + kx : xk[kx], p);
            den += w;
            for (int c = 0; c < 4; c++) acc[c] += w * p[c];
        }
    }
    for (int c = 0; c < 4; c++) out[c] = (uint8_t)((acc[c] + den / 2) / den);
}

static bool box_shrinks(const shr__scale *s) {
    return s->filter == SHR_SCALE_BOX && (s->src.x1 - s->src.x0 > s->dw || s->src.y1 - s->src.y0 > s->dh);
}

INLINE void box_row(const shr__scale *s, uint32_t f, int32_t y, int32_t x0, int32_t n, uint8_t *out) {
    for (int32_t i = 0; i < n; i++) px_box(s, f, x0 + i, y, out + 4 * i);
}

/* RGBA sources (every driver buffer) get their own copy with the format a constant. */
void shr__scale_row(const shr__scale *s, int32_t y, int32_t x0, int32_t n, uint8_t *out) {
    if (!box_shrinks(s))
        row_bilinear(s, y, x0, n, out);
    else if (s->format == SHR_IMAGE_SRC_RGBA8888)
        box_row(s, SHR_IMAGE_SRC_RGBA8888, y, x0, n, out);
    else
        box_row(s, s->format, y, x0, n, out);
}

void shr__scale_rows(const shr__scale *s, uint8_t *out, size_t stride) {
    if (box_shrinks(s)) {
        for (int32_t y = 0; y < s->dh; y++) shr__scale_row(s, y, 0, s->dw, out + (size_t)y * stride);
        return;
    }
    shr__bilin b;
    uint32_t px[SHR__SCALE_SPAN];
    for (int32_t x = 0; x < s->dw; x += SHR__SCALE_SPAN) {
        int32_t n = s->dw - x < SHR__SCALE_SPAN ? s->dw - x : SHR__SCALE_SPAN;
        shr__bilin_start(&b, s, x, n, 0);
        for (int32_t y = 0; y < s->dh; y++)
            shr__bilin_next(&b, px), memcpy(out + (size_t)y * stride + (size_t)x * 4, px, (size_t)n * 4);
    }
}
