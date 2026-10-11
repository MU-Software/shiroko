#ifndef SHIROKO_SHR_BITMAP_FONT_H
#define SHIROKO_SHR_BITMAP_FONT_H

#include "shr_compositor.h"
#include "shr_package.h"
#include "shr_unicode.h"

/* Glyph id of one cluster, below 2^32. Single scalars are encoded directly and longer clusters interned, so ids
 * stay valid for the font's lifetime.
 * `cls` is the cluster's shr__classify() result; SHR_E_NOT_FOUND for invisible clusters and U+0020 (draw nothing). */
shr_status shr__bitmap_font_glyph(shr_pl_res_bitmap_font *font, const uint32_t *cps, size_t n,
                                  const shr__cluster_class *cls, uint64_t *out_id);

/* The resource GLYPH commands refer to (res->users counts the tilemaps using the font). */
shr__res *shr__bitmap_font_res(shr_pl_res_bitmap_font *font);

#endif
