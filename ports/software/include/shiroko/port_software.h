#ifndef SHIROKO_PORT_SOFTWARE_H
#define SHIROKO_PORT_SOFTWARE_H

#include <shiroko/shiroko.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous CPU driver. `cache_bytes` bounds its cache for CACHE_BEGIN/END groups, bookkeeping included
 * (0 = draw them directly); a group larger than half of what the hash table leaves of it is drawn directly.
 * Buffer ids are 1..max_buffers; the driver draws from registered memory in place (no SHR_BUFFER_COPIES).
 * The driver keeps its state until shr_software_driver_destroy(). */
shr_status shr_software_driver_create(const shr_allocator *allocator, uint64_t cache_bytes, uint32_t max_buffers,
                                      shr_framebuffer_driver *out);
shr_status shr_software_driver_destroy(shr_framebuffer_driver *driver);
/* Stateless CPU execution for device ports that run some commands on the CPU; groups are drawn directly,
 * limited to their `cache_clip`. Buffer commands are not executed: GLYPH and IMAGE draw from the port's table,
 * id k at buffers[k - 1] (format 0 = not registered), entries being memory a REGISTER would accept. */
shr_status shr_software_execute(const shr_surface *dst, const shr_draw_cmd *cmds, size_t count, const shr_image *buffers,
                                uint32_t nbuffers) SHR_NONBLOCKING;
/* Synchronous source for a regular file; reads may run concurrently; close() closes the file.
 * SHR_E_INVALID_ARG for NULL arguments, SHR_E_NOT_FOUND when the path does not exist (ENOENT, ENOTDIR),
 * SHR_E_IO for other failures and anything but a regular file. Reads fail with SHR_E_IO. */
shr_status shr_asset_source_file(const char *path, shr_asset_source *out);

#ifdef __cplusplus
}
#endif

#endif
