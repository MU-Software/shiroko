#ifndef SHIROKO_RASTER_H
#define SHIROKO_RASTER_H

#include <shiroko/shiroko_driver.h>

/* Validates a whole batch (CACHE_BEGIN/END pairs included) so a bad command never leaves a partial write. */
shr_status shr__raster_check(const shr_surface *dst, const shr_draw_cmd *cmds, size_t count) SHR_NONBLOCKING;
/* Draws one checked FILL/GLYPH/IMAGE/COPY/ROTATE command moved by -origin and limited to `clip`. */
void shr__raster_draw(const shr_surface *dst, const shr_draw_cmd *c, shr_point origin, shr_rect clip) SHR_NONBLOCKING;

static inline size_t shr__px_bytes(shr_pixel_format f) { return f == SHR_FORMAT_RGB565 ? 2 : 4; }

#endif
