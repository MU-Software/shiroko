#ifndef SHIROKO_PORT_ANGLE_H
#define SHIROKO_PORT_ANGLE_H

#include <shiroko/shiroko.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OpenGL ES 3.0 driver on ANGLE. Every call below except the offscreen ones needs the EGL context the driver
 * was created with current on the calling thread (SHR_E_STATE otherwise, before any GL work); they change GL
 * state (program, buffers, textures, framebuffer, blend, scissor, viewport, pixel store). */

typedef struct shr_angle_offscreen shr_angle_offscreen;

/* An EGL pbuffer with a GLES 3.0 context, made current on the calling thread; offscreen calls come from one
 * thread. The backend comes from the environment variable SHIROKO_ANGLE_BACKEND (metal, opengl, vulkan, d3d11 or
 * default; unset: metal on macOS, opengl elsewhere); when it cannot be created the others are tried.
 * SHR_E_DEVICE when none works. */
shr_status shr_angle_offscreen_create(int32_t width, int32_t height, shr_angle_offscreen **out);
/* The backend obtained ("metal", "opengl", "vulkan", "d3d11", "default"); NULL for NULL. */
const char *shr_angle_offscreen_backend(const shr_angle_offscreen *offscreen);
shr_status shr_angle_offscreen_destroy(shr_angle_offscreen *offscreen);

/* Synchronous driver (execute() returns SHR_OK once the GL commands are issued, without glFlush) for CPU and
 * DEVICE destinations and sources, bound to the EGL context current now (SHR_E_STATE without one).
 * `texture_cache_bytes` bounds the textures kept for glyph and image sources (0 = upload them each time).
 * caps.max_width / max_height are GL_MAX_TEXTURE_SIZE, at most 16384. execute() rejects a batch before any GL
 * work, except SHR_E_DEVICE: a GL error after drawing, which may leave the destination partly written. */
shr_status shr_angle_driver_create(const shr_allocator *allocator, uint64_t texture_cache_bytes,
                                   shr_framebuffer_driver *out);
/* Also destroys the surfaces still alive. */
shr_status shr_angle_driver_destroy(shr_framebuffer_driver *driver);

/* A GPU surface (SHR_MEMORY_DEVICE) of RGB565 or RGBX8888, cleared to black. `pixels` holds a handle of the
 * driver (not CPU-accessible) so commands can name the surface as a COPY/ROTATE source; `resource_id` is
 * unique for the driver's lifetime. SHR_E_UNSUPPORTED beyond caps.max_width / max_height. */
shr_status shr_angle_surface_create(shr_framebuffer_driver *driver, int32_t width, int32_t height,
                                    shr_pixel_format format, shr_surface *out);
shr_status shr_angle_surface_destroy(shr_framebuffer_driver *driver, shr_surface *surface);
/* CPU copy of a surface in its own format, rows `stride` bytes apart. */
shr_status shr_angle_surface_read(shr_framebuffer_driver *driver, const shr_surface *surface, void *pixels,
                                  size_t stride);
/* The GL texture of a surface, to draw it elsewhere. Texel row 0 is the top row of the surface. Another context
 * or API reading it needs a glFlush() (or a fence) after execute(). */
shr_status shr_angle_surface_texture(shr_framebuffer_driver *driver, const shr_surface *surface, uint32_t *texture);

#ifdef __cplusplus
}
#endif

#endif
