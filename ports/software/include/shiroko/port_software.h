#ifndef SHIROKO_PORT_SOFTWARE_H
#define SHIROKO_PORT_SOFTWARE_H

#include <shiroko/shiroko.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous CPU driver. Keep ids are 1..max_keeps (caps.max_keeps), their pixels allocated from `allocator` as
 * SHR_ALLOC_PAYLOAD, 128-byte aligned. With `keep_bytes` (caps.keep_bytes) id k owns slot k of keep_bytes / max_keeps
 * bytes rounded down to a multiple of 128 (caps.max_keep_bytes; SHR_E_INVALID_ARG when that is 0 with max_keeps),
 * all in one block taken at create (else SHR_E_NO_MEMORY) and freed at destroy; a keep needs W * H * bytes per
 * pixel to fit, so keep_bytes = max_keeps * (row bytes rounded up to 128) holds max_keeps rows. With keep_bytes 0 each
 * keep takes memory of its own size, freed by KEEP_RELEASE. Buffer ids are 1..max_buffers; the driver draws from
 * registered memory in place (no SHR_BUFFER_COPIES), except IMAGE into RGB565 where shr_software_driver_image_planes()
 * lets it draw from a plane. It moves pixels with memmove and says so (SHR_DRIVER_CHEAP_MOVE); a port wrapping it where
 * memory is slow clears that flag. The driver keeps its state until shr_software_driver_destroy(). */
shr_status shr_software_driver_create(const shr_allocator *allocator, uint64_t keep_bytes, uint32_t max_keeps,
                                      uint32_t max_buffers, shr_framebuffer_driver *out);
/* BOLD and ITALIC GLYPHs draw from coverage the driver synthesizes once per buffer region and keeps until a REGISTER,
 * UPDATE or RELEASE of the buffer, in up to `bytes` (default 256 KiB, 0 = synthesize at every draw) plus an index of up
 * to bytes / 8 from the allocator as SHR_ALLOC_PAYLOAD, taken as needed and emptied when full (once the allocator
 * refuses to grow it, it stays at the size reached until this is called again); ON_FILL ones go through colour tables
 * taken at the first of them kept (about 33 KiB more). Frees what it keeps; not while execute() runs. */
shr_status shr_software_driver_synth_cache(shr_framebuffer_driver *driver, size_t bytes);
/* IMAGE into RGB565 draws from a plane the first such draw derives from the RGBA8888 buffer, 8 bytes per pixel and per
 * row from the allocator as SHR_ALLOC_PAYLOAD, while all planes fit in `bytes` (default 8 MiB, about twice the
 * compositor's default image_bytes; 0 = no planes), derived anew where an UPDATE says and freed by a REGISTER or
 * RELEASE of the id. Without one (no room, memory refused) it blends from the buffer, the same pixels. Frees every
 * plane; not while execute() runs. */
shr_status shr_software_driver_image_planes(shr_framebuffer_driver *driver, size_t bytes);
shr_status shr_software_driver_destroy(shr_framebuffer_driver *driver);
/* Copies keep pixels for the driver (default: memcpy): KEEP_DRAW, and a stored group the driver draws straight into the
 * destination when the KEEP_DRAW right after it takes the whole group, then copies into its keep. copy() copies `rows`
 * rows of `bytes` bytes, row y from src + y * src_stride to dst + y * dst_stride, one side a keep and the other the
 * destination; it returns a ticket above every earlier one for a copy it started, or 0 for one it did not (the driver
 * then uses memcpy). wait() returns once copy `ticket` and every copy started before it finished. Until it waited for a
 * copy, the driver touches neither its keep nor its destination rows (all of their width). execute() returns without
 * waiting: before anything else writes the destination or reads rows a copy writes, and before the next batch drawing
 * into it, the port waits for the last ticket the batch got. reset() and destroy wait for every copy. Setting a copier
 * (NULL: memcpy) first waits for the copies of the one before; SHR_E_INVALID_ARG without copy or wait. With a copier
 * the driver sets SHR_DRIVER_CHEAP_STORE in caps.flags (clears it with NULL), which a context reads when created: set
 * it before shr_create(). With copy_trim (optional), a stored group records how many of its first and last columns hold
 * only its first row's first and last pixel; a KEEP_DRAW of it then copies only the columns between them (rows whose
 * address or stride is not a multiple of the pixel size are copied whole), widened to start and end at multiples of
 * `align` bytes of the destination's first row (0: any; later rows follow the stride) and to one pixel at least, by
 * copy_trim(), which also fills the columns around them as `trim` says (shr_software_trim_fill()) by the time the copy
 * is done. Returning 0 it did neither: the driver then fills and copies. The copier's device reads and writes the block
 * of keep_bytes, SHR_ALLOC_PAYLOAD memory of the driver's allocator. */
typedef struct shr_software_trim {
    size_t lo, hi;        /* bytes of each destination row before and after the copied ones */
    uint32_t left, right; /* their pixel, as 4 bytes of memory repeated (RGB565: the pixel twice) */
} shr_software_trim;
typedef struct shr_software_copier {
    void *user;
    uint64_t (*copy)(void *user, void *dst, size_t dst_stride, const void *src, size_t src_stride, size_t bytes,
                     int32_t rows);
    void (*wait)(void *user, uint64_t ticket);
    uint64_t (*copy_trim)(void *user, void *dst, size_t dst_stride, const void *src, size_t src_stride, size_t bytes,
                          int32_t rows, const shr_software_trim *trim);
    size_t align;
} shr_software_copier;
/* Fills the columns `trim` names around the `bytes` bytes at dst + y * stride of each of `rows` rows. */
void shr_software_trim_fill(void *dst, size_t stride, size_t bytes, int32_t rows, const shr_software_trim *trim);
shr_status shr_software_driver_set_copier(shr_framebuffer_driver *driver, const shr_software_copier *copier);
/* Stateless CPU execution for device ports that run some commands on the CPU; it keeps nothing (keep commands are
 * invalid). Buffer commands are not executed: GLYPH and IMAGE draw from the port's table, id k at buffers[k - 1]
 * (format 0 = not registered), entries being memory a REGISTER would accept. */
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
