#ifndef SHIROKO_SHR_RECT_H
#define SHIROKO_SHR_RECT_H

#include <stdbool.h>
#include <stdint.h>

#include <shiroko/shiroko_driver.h>

static inline bool shr__fits_i32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

static inline bool shr__rect_empty(shr_rect r) { return r.x0 >= r.x1 || r.y0 >= r.y1; }
static inline bool shr__rect_valid(shr_rect r) { return r.x0 <= r.x1 && r.y0 <= r.y1; }
static inline shr_rect shr__rect_intersect(shr_rect a, shr_rect b) {
    shr_rect r = {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1,
                  a.y1 < b.y1 ? a.y1 : b.y1};
    if (shr__rect_empty(r)) r.x1 = r.x0, r.y1 = r.y0;
    return r;
}

static inline shr_rect shr__rect_union(shr_rect a, shr_rect b) {
    if (shr__rect_empty(a)) return b;
    if (shr__rect_empty(b)) return a;
    return (shr_rect){a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1,
                      a.y1 > b.y1 ? a.y1 : b.y1};
}

/* Callers pass non-empty rectangles. */
static inline int64_t shr__rect_area(shr_rect r) { return ((int64_t)r.x1 - r.x0) * ((int64_t)r.y1 - r.y0); }

#endif
