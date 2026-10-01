#ifndef SHIROKO_SHR_BITMAP_FONT_H
#define SHIROKO_SHR_BITMAP_FONT_H

#include "shr_compositor.h"
#include "shr_unicode.h"

/* The built-in package (ASCII + U+FFFD), baked by the build into the library for the build's cell size. */
extern const uint8_t shr__builtin_package[];
extern const size_t shr__builtin_package_size;

/* Glyph id of one cluster. Single scalars are encoded directly and longer clusters interned, so ids stay valid
 * for the font's lifetime.
 * `cls` is the cluster's shr__classify() result; SHR_E_NOT_FOUND for invisible clusters (draw nothing). */
shr_status shr__bitmap_font_glyph(shr_pl_res_bitmap_font *font, const uint32_t *cps, size_t n,
                                  const shr__cluster_class *cls, uint64_t *out_id);

/* The resource GLYPH commands refer to (res->users counts the tilemaps using the font). */
shr__res *shr__bitmap_font_res(shr_pl_res_bitmap_font *font);

/* Line positions from the cell top, from the built-in package (same font and cell size). */
typedef struct shr__line_metrics {
    int32_t baseline, underline_y, strike_y;
} shr__line_metrics;

shr__line_metrics shr__bitmap_font_line_metrics(void);

#endif
