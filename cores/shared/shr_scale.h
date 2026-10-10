#ifndef SHIROKO_SHR_SCALE_H
#define SHIROKO_SHR_SCALE_H

#include <stddef.h>
#include <stdint.h>

#include <shiroko/shiroko.h>

#define SHR__SCALE_MAX 32767 /* largest scaled or source side: drivers may pass them as int16 */

/* Beside the SHR_IMAGE_SRC_* formats: an opaque image kept as RGB565 (the driver's buffer format), read as RGBA with
 * each field widened to 8 bits by repeating its top bits. */
#define SHR__SRC_RGB565 4u

static inline size_t shr__src_bytes(uint32_t format) {
    return format == SHR_IMAGE_SRC_RGBA8888 ? 4 : format == SHR_IMAGE_SRC_RGB888 ? 3
           : format == SHR__SRC_RGB565 ? 2 : (size_t)format - 1;
}

/* The source format of a driver buffer: RGBA8888 or RGB565. */
static inline uint32_t shr__src_of(shr_pixel_format f) {
    return f == SHR_FORMAT_RGB565 ? SHR__SRC_RGB565 : SHR_IMAGE_SRC_RGBA8888;
}

/* `src` of a w x h source of `format` scaled to dw x dh; scaled pixel (x, y) depends on x, y and these alone.
 * BILINEAR taps, per axis, scaled column i of source columns [s0, s0 + s) over d: t = ((2i + 1) s + d) 2^15 / d (16.16
 * position of the column's centre in the source, one pixel up), columns a = k and b = k + 1 (k = s0 + (t >> 16) - 1)
 * clamped to the source, fx = (t >> 8) & 255 the weight of b in 1/256. It blends each channel
 * (top * (256 - fy) + bottom * fy + 32768) >> 16 with top and bottom the rows' a * (256 - fx) + b * fx. BOX averages
 * the covered source pixels on axes that shrink and blends bilinearly on the others. */
typedef struct shr__scale {
    const uint8_t *pixels;
    size_t stride;
    int32_t w, h;
    uint32_t format; /* SHR_IMAGE_SRC_* */
    shr_rect src;
    int32_t dw, dh;
    uint32_t filter; /* SHR_SCALE_* */
} shr__scale;

/* Scaled pixels [x0, x0 + n) of row y as RGBA8888 into `out` (4n bytes); 0 <= x0, x0 + n <= dw, 0 <= y < dh. */
void shr__scale_row(const shr__scale *s, int32_t y, int32_t x0, int32_t n, uint8_t *out);

/* Every scaled row, row y at out + y * stride. */
void shr__scale_rows(const shr__scale *s, uint8_t *out, size_t stride);

#define SHR__SCALE_SPAN 64

/* BILINEAR rows y, y + 1, ... of scaled columns [x0, x0 + n), n <= SHR__SCALE_SPAN: the column taps are computed once
 * and each source row's horizontal blend once, kept while the next rows read it. Blends hold two channels in 16-bit
 * lanes (R | B << 16 and G | A << 16): a lane's blend is at most 255 * 256. */
typedef struct shr__bilin {
    const shr__scale *s;
    int32_t n, held[2], x; /* x: the first tap's column */
    uint32_t t, r, dq, dr; /* the next row's t, stepped exactly */
    uint32_t tap[SHR__SCALE_SPAN]; /* column a - x << 9 | (b - a) << 8 | weight of b */
    uint32_t h[2][2 * SHR__SCALE_SPAN];
} shr__bilin;

void shr__bilin_start(shr__bilin *b, const shr__scale *s, int32_t x0, int32_t n, int32_t y);

/* The next row as n RGBA8888 words (R in the low byte). */
void shr__bilin_next(shr__bilin *b, uint32_t *out);

/* The next row as blends: returns its top source row's (2n words: R | B, then G | A, per column), *bot the bottom's
 * and *fy the bottom's weight; shr__bilin_lerp() of a top and a bottom word gives two channels of a scaled pixel. */
const uint32_t *shr__bilin_rows(shr__bilin *b, const uint32_t **bot, uint32_t *fy);

/* Lanes of (top (256 - fy) + bottom fy + 32768) >> 16, split at bit 8 so that each product stays in its lane:
 * with top = 256 th + tl and likewise bottom, that is (hi + (lo >> 8) + 128) >> 8. */
static inline uint32_t shr__bilin_lerp(uint32_t top, uint32_t bot, uint32_t fy) {
    uint32_t gy = 256 - fy, hi = (top >> 8 & 0x00FF00FFu) * gy + (bot >> 8 & 0x00FF00FFu) * fy;
    uint32_t lo = (top & 0x00FF00FFu) * gy + (bot & 0x00FF00FFu) * fy;
    return (hi + (lo >> 8 & 0x00FF00FFu) + 0x00800080u) >> 8 & 0x00FF00FFu;
}

#endif
