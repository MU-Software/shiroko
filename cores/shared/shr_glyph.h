#ifndef SHR_GLYPH_H
#define SHR_GLYPH_H

#include <shiroko/shiroko_driver.h>

/* Italic shift of src row `y`: whole pixels `k` and 1/256 pixels `f` (shr_draw_cmd). */
static inline void shr__slant(int32_t axis, int32_t y, int32_t *k, int32_t *f) {
    int32_t t = SHR_GLYPH_SLANT / 2 * (axis - 2 * y - 1) + (1 << 23);
    *k = (t >> 8) - (1 << 15);
    *f = t & 255;
}

/* Source columns [*x0, *x1) a BOLD or ITALIC GLYPH of w x h src pixels draws; w, h and an ITALIC axis within the
 * bounds shr_draw_cmd sets. */
static inline void shr__glyph_footprint(int32_t w, int32_t h, uint32_t flags, int32_t axis, int32_t *x0,
                                        int32_t *x1) {
    int32_t k0, f0, kn, fn;
    bool italic = flags & SHR_GLYPH_ITALIC;
    axis = italic ? axis : 0;
    shr__slant(axis, 0, &k0, &f0);
    shr__slant(axis, h - 1, &kn, &fn);
    *x0 = italic ? kn : 0;
    *x1 = w + (flags & SHR_GLYPH_BOLD ? 1 : 0) + (italic ? k0 + (f0 != 0) : 0);
}

#endif
