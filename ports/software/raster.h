#ifndef SHIROKO_RASTER_H
#define SHIROKO_RASTER_H

#include <shiroko/shiroko_driver.h>

/* Whether a driver draws into or reads a valid buffer: SHR_OK or SHR_E_UNSUPPORTED. */
typedef shr_status (*shr__reach_fn)(const void *user, const void *pixels, int32_t width, int32_t height,
                                    shr_pixel_format format, shr_memory_domain domain) SHR_NONBLOCKING;

/* Validates a whole batch (CACHE_BEGIN/END pairs included) so a bad command never leaves a partial write. `fn`
 * decides which destination and source buffers the driver reaches; NULL: the software port's, all but DEVICE.
 * DEVICE buffers are identified by `pixels` and overlap only when it is the same. */
shr_status shr__raster_check(const shr_surface *dst, const shr_draw_cmd *cmds, size_t count, shr__reach_fn fn,
                             const void *user) SHR_NONBLOCKING;
/* The source shares bytes, or the DEVICE buffer, with the destination. */
bool shr__raster_reads_dst(const shr_surface *dst, const shr_image *m) SHR_NONBLOCKING;
/* Draws one checked FILL/GLYPH/IMAGE/COPY/ROTATE command moved by -origin and limited to `clip`. */
void shr__raster_draw(const shr_surface *dst, const shr_draw_cmd *c, shr_point origin, shr_rect clip) SHR_NONBLOCKING;

static inline size_t shr__px_bytes(shr_pixel_format f) { return f == SHR_FORMAT_RGB565 ? 2 : 4; }
static inline uint32_t shr__quantize(uint32_t c, uint32_t m) { return (c * m + 127) / 255; }

#endif
