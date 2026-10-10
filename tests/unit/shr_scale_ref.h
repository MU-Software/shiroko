/* Kitty image scaling, the e0r reference (FILTER.md). Source: straight RGBA8888 (bytes R, G, B, A), w x h, `stride`
 * bytes per row; source rect (sx, sy, sw, sh), scaled to dw x dh. Texels are clamped to the image (a rect past it, from
 * Ghostty's placeholder rounding, repeats the edge). shr_scale_px gives scaled pixel (i, j)
 * from (i, j) alone, so every clipped part of a scaled image (bands, damage, cells) has the same pixels. */
#ifndef SHR_SCALE_REF_H
#define SHR_SCALE_REF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SHR_FILTER_BILINEAR, SHR_FILTER_BOX }; /* the e0r reference's nearest left out: not a Shiroko filter */

typedef struct shr_scale_src {
    const uint8_t *px;
    size_t stride;
    int32_t w, h, sx, sy, sw, sh, dw, dh;
} shr_scale_src;

static inline int32_t shr__clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Texels k0, k1 (clamped to [0, n)) and the weight of k1 in 1/256 for t = s0 + (i + 1/2) * s / d - 1/2, in 16.16 one
 * pixel up so that the division stays positive. */
static inline void shr_bilinear(int32_t s0, int32_t s, int32_t d, int32_t n, int32_t i, int32_t *k0, int32_t *k1,
                                uint32_t *f) {
    int64_t t = (((int64_t)(2 * i + 1) * s + d) << 15) / d;
    int32_t k = s0 + (int32_t)(t >> 16) - 1;
    *k0 = shr__clampi(k, 0, n - 1), *k1 = shr__clampi(k + 1, 0, n - 1), *f = (uint32_t)(t >> 8) & 255;
}

/* Box along one axis (s > d): dst index i covers [a, b) in 1/256 px of the source span. */
static inline void shr_box_span(int32_t s, int32_t d, int32_t i, int32_t *a, int32_t *b) {
    *a = (int32_t)(((int64_t)i * s * 256) / d), *b = (int32_t)(((int64_t)(i + 1) * s * 256) / d);
}

static inline uint32_t shr_box_w(int32_t a, int32_t b, int32_t k) {
    int32_t lo = a > k * 256 ? a : k * 256, hi = b < (k + 1) * 256 ? b : (k + 1) * 256;
    return (uint32_t)(hi - lo);
}

static inline void shr_scale_px(const shr_scale_src *m, int filter, int32_t i, int32_t j, uint8_t out[4]) {
    int32_t xk[2], yk[2];
    uint32_t fx, fy;
    shr_bilinear(m->sx, m->sw, m->dw, m->w, i, &xk[0], &xk[1], &fx);
    shr_bilinear(m->sy, m->sh, m->dh, m->h, j, &yk[0], &yk[1], &fy);
    bool bx = filter == SHR_FILTER_BOX && m->sw > m->dw, by = filter == SHR_FILTER_BOX && m->sh > m->dh;
    if (!bx && !by) {
        const uint8_t *r0 = m->px + (size_t)yk[0] * m->stride, *r1 = m->px + (size_t)yk[1] * m->stride;
        for (int c = 0; c < 4; c++) {
            uint32_t top = r0[xk[0] * 4 + c] * (256 - fx) + r0[xk[1] * 4 + c] * fx;
            uint32_t bot = r1[xk[0] * 4 + c] * (256 - fx) + r1[xk[1] * 4 + c] * fx;
            out[c] = (uint8_t)((top * (256 - fy) + bot * fy + 32768) >> 16);
        }
        return;
    }
    /* Box: per axis the overlap weights (s > d) or the two bilinear taps; out = round(sum w c / sum w). */
    int32_t ax, bxe, ay, bye, kx0, kx1, ky0, ky1;
    if (bx) shr_box_span(m->sw, m->dw, i, &ax, &bxe), kx0 = ax >> 8, kx1 = (bxe - 1) >> 8;
    else kx0 = 0, kx1 = 1;
    if (by) shr_box_span(m->sh, m->dh, j, &ay, &bye), ky0 = ay >> 8, ky1 = (bye - 1) >> 8;
    else ky0 = 0, ky1 = 1;
    uint64_t acc[4] = {0, 0, 0, 0}, den = 0;
    for (int32_t ky = ky0; ky <= ky1; ky++) {
        uint32_t wy = by ? shr_box_w(ay, bye, ky) : ky ? fy : 256 - fy;
        int32_t row = by ? shr__clampi(m->sy + ky, 0, m->h - 1) : yk[ky];
        for (int32_t kx = kx0; kx <= kx1; kx++) {
            uint32_t wx = bx ? shr_box_w(ax, bxe, kx) : kx ? fx : 256 - fx;
            const uint8_t *p = m->px + (size_t)row * m->stride + (size_t)(bx ? shr__clampi(m->sx + kx, 0, m->w - 1) : xk[kx]) * 4;
            uint64_t w = (uint64_t)wx * wy;
            den += w;
            for (int c = 0; c < 4; c++) acc[c] += w * p[c];
        }
    }
    for (int c = 0; c < 4; c++) out[c] = (uint8_t)((acc[c] + den / 2) / den);
}

/* The software driver's IMAGE blend of straight RGBA (r, g, b, a) onto a target pixel, every a in 0..255 (0 leaves
 * it, 255 stores it): ports/software/raster.c image_px, blend_px, blend565, shr__raster_image give these bytes. */
static inline uint16_t shr_blend565(uint16_t d, const uint8_t px[4]) {
    uint32_t a = px[3], w = 255 * (255 - a);
    uint32_t r = (31 * px[0] * a + (d >> 11) * w + 32512) / 65025;
    uint32_t g = (63 * px[1] * a + ((d >> 5) & 63) * w + 32512) / 65025;
    uint32_t b = (31 * px[2] * a + (d & 31) * w + 32512) / 65025;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static inline void shr_blend8888(uint8_t d[4], const uint8_t px[4]) {
    uint32_t a = px[3];
    for (int c = 0; c < 3; c++) d[c] = (uint8_t)((px[c] * a + d[c] * (255 - a) + 127) / 255);
    d[3] |= (uint8_t)((a != 0) * 255u);
}

#endif
