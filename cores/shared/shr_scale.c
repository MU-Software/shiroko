#include "shr_scale.h"

#define INLINE static inline __attribute__((always_inline))

INLINE void texel(uint32_t f, const uint8_t *row, int32_t x, uint8_t out[4]) {
    shr__src_rgba(f, row + (size_t)x * shr__src_bytes(f), out);
}

/* Column taps step t by (sw << 16) / dw with the remainder carried: the same t as shr__scale_t(). */
INLINE void row_bilinear(const shr__scale *s, uint32_t f, int32_t y, int32_t x0, int32_t n, uint8_t *out) {
    int32_t ya, yb, sw = s->src.x1 - s->src.x0, last = s->w - 1;
    uint32_t fy, den = (uint32_t)s->dw, step = (uint32_t)sw << 16, dq = step / den, dr = step % den;
    shr__scale_taps(s->src.y0, s->src.y1 - s->src.y0, s->dh, s->h, y, &ya, &yb, &fy);
    const uint8_t *ra = s->pixels + (size_t)ya * s->stride, *rb = s->pixels + (size_t)yb * s->stride;
    uint64_t num = ((uint64_t)(2 * x0 + 1) * (uint32_t)sw + den) << 15;
    uint32_t t = (uint32_t)(num / den), r = (uint32_t)(num % den);
    for (int32_t i = 0; i < n; i++, out += 4) {
        int32_t k = s->src.x0 + (int32_t)(t >> 16) - 1;
        int32_t xa = k < 0 ? 0 : k > last ? last : k, xb = k + 1 > last ? last : k + 1;
        uint32_t fx = t >> 8 & 255;
        uint8_t p[4], q[4], u[4], v[4];
        texel(f, ra, xa, p), texel(f, ra, xb, q), texel(f, rb, xa, u), texel(f, rb, xb, v);
        for (int c = 0; c < 4; c++) {
            uint32_t top = p[c] * (256 - fx) + q[c] * fx, bot = u[c] * (256 - fx) + v[c] * fx;
            out[c] = (uint8_t)((top * (256 - fy) + bot * fy + 32768) >> 16);
        }
        t += dq, r += dr;
        if (r >= den) r -= den, t++;
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

INLINE void scale_row(const shr__scale *s, uint32_t f, int32_t y, int32_t x0, int32_t n, uint8_t *out) {
    if (s->filter == SHR_SCALE_BOX && (s->src.x1 - s->src.x0 > s->dw || s->src.y1 - s->src.y0 > s->dh))
        for (int32_t i = 0; i < n; i++) px_box(s, f, x0 + i, y, out + 4 * i);
    else
        row_bilinear(s, f, y, x0, n, out);
}

/* RGBA sources (every driver buffer) get their own copy with the format a constant. */
void shr__scale_row(const shr__scale *s, int32_t y, int32_t x0, int32_t n, uint8_t *out) {
    if (s->format == SHR_IMAGE_SRC_RGBA8888)
        scale_row(s, SHR_IMAGE_SRC_RGBA8888, y, x0, n, out);
    else
        scale_row(s, s->format, y, x0, n, out);
}
