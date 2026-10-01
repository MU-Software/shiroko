#ifndef SHIROKO_DRIVER_H
#define SHIROKO_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(SHR_REALTIME_CHECKS) && defined(__clang__) && defined(__has_attribute)
#if __has_attribute(nonblocking)
#define SHR_NONBLOCKING __attribute__((nonblocking))
#endif
#endif
#ifndef SHR_NONBLOCKING
#define SHR_NONBLOCKING
#endif

/* Build configuration, set by CMake (SHIROKO_CELL_WIDTH, SHIROKO_CELL_HEIGHT, SHIROKO_PIXEL_FORMAT). */
#if !defined(SHR_CELL_WIDTH) || !defined(SHR_CELL_HEIGHT) || !defined(SHR_PIXEL_FORMAT)
#error "link against the shiroko CMake target: SHR_CELL_WIDTH, SHR_CELL_HEIGHT and SHR_PIXEL_FORMAT are not defined"
#endif

/* Functions return shr_status unless noted; creating functions set their out pointer to NULL on
 * failure, and a failing call has no effect unless its documentation says otherwise. */

typedef enum shr_status {
    SHR_OK = 0,
    SHR_IN_PROGRESS,         /* accepted; completion is signalled later */
    SHR_E_INVALID_ARG,
    SHR_E_INVALID_UTF8,
    SHR_E_CONTROL_CHAR,
    SHR_E_STYLE_BOUNDARY,
    SHR_E_UNKNOWN_STYLE,
    SHR_E_LIMIT,
    SHR_E_OVERFLOW,
    SHR_E_WRAP_NO_SPACE,
    SHR_E_CLUSTER_TOO_WIDE,
    SHR_E_PROFILE_MISMATCH,
    SHR_E_FORMAT,            /* malformed package or record */
    SHR_E_CHECKSUM,
    SHR_E_IO,
    SHR_E_NO_MEMORY,
    SHR_E_WOULD_BLOCK,       /* not accepted now; retry later */
    SHR_E_STATE,
    SHR_E_NOT_FOUND,
    SHR_E_UNSUPPORTED,
    SHR_E_TIMEOUT,
    SHR_E_DEVICE
} shr_status;

/* Never NULL; "UNKNOWN" for values outside shr_status. */
const char *shr_status_name(shr_status status);

typedef enum shr_alloc_kind {
    SHR_ALLOC_DESCRIPTOR = 0, /* descriptors, small state */
    SHR_ALLOC_PAYLOAD,        /* large buffers: pages, images, command lists */
    SHR_ALLOC_DMA             /* buffers a device may access */
} shr_alloc_kind;

/* Both functions or neither (NULL allocator = malloc/free). SHR_ALLOC_DMA
 * requests must return memory a DMA-only driver can access. */
typedef struct shr_allocator {
    void *user;
    void *(*alloc)(void *user, size_t size, size_t align, shr_alloc_kind kind);
    void (*free)(void *user, void *ptr, size_t size, size_t align, shr_alloc_kind kind);
} shr_allocator;

typedef struct shr_rect {
    int32_t x0, y0, x1, y1; /* [x0,x1) x [y0,y1) */
} shr_rect;

typedef struct shr_point {
    int32_t x, y;
} shr_point;

/* 0xAARRGGBB; the alpha byte is ignored for now: colours draw opaque. */
typedef uint32_t shr_color;

#define SHR_RGB(r, g, b) \
    ((shr_color)(0xFF000000u | (((uint32_t)(r) & 0xFF) << 16) | (((uint32_t)(g) & 0xFF) << 8) | ((uint32_t)(b) & 0xFF)))

typedef enum shr_pixel_format {
    SHR_FORMAT_RGB565 = 1,   /* native-endian u16: R 15..11, G 10..5, B 4..0 */
    SHR_FORMAT_RGBX8888 = 2, /* bytes R, G, B, X; X is ignored on read and unspecified on write */
    SHR_FORMAT_A4 = 3,       /* glyph coverage, high nibble = left pixel, n -> 17 * n */
    SHR_FORMAT_A8 = 4,       /* glyph coverage */
    SHR_FORMAT_RGBA8888 = 5  /* image pixels: bytes R, G, B, A (straight alpha) */
} shr_pixel_format;

/* SHR_PIXEL_FORMAT is the screen format (RGB565 or RGBX8888). Outputs may use the other one. */
shr_status shr_format_row_bytes(shr_pixel_format format, int32_t width, size_t *out);

typedef enum shr_memory_domain {
    SHR_MEMORY_CPU = 1u << 0,
    SHR_MEMORY_DMA = 1u << 1,    /* CPU-accessible, device-capable (cache sync needed) */
    SHR_MEMORY_DEVICE = 1u << 2  /* not CPU-accessible */
} shr_memory_domain;

typedef struct shr_surface {
    void *pixels;
    int32_t width;
    int32_t height;
    size_t stride;
    size_t byte_length;
    shr_pixel_format format;
    uint32_t generation;       /* changes whenever the contents are no longer what was last written */
    shr_memory_domain domain;  /* 0 means SHR_MEMORY_CPU */
    /* Identity of the underlying buffer, 0 = `pixels`. Preserved content is
     * tracked per identity, so a DEVICE surface without pixels needs one. */
    uint64_t resource_id;
} shr_surface;

shr_status shr_surface_validate(const shr_surface *surface) SHR_NONBLOCKING;

/* Read-only pixels a command draws from: a glyph (A4/A8), an image (RGBA8888) or a surface (COPY/ROTATE). */
typedef struct shr_image {
    const void *pixels;
    int32_t width;
    int32_t height;
    size_t stride;
    size_t byte_length;
    shr_pixel_format format;
    shr_memory_domain domain;  /* 0 means SHR_MEMORY_CPU */
} shr_image;

shr_status shr_image_validate(const shr_image *image) SHR_NONBLOCKING;

typedef enum shr_cmd_kind {
    SHR_CMD_FILL = 1,     /* dst <- color */
    SHR_CMD_GLYPH,        /* dst <- color through A4/A8 coverage of src */
    SHR_CMD_IMAGE,        /* dst <- RGBA8888 src, source-over */
    SHR_CMD_COPY,         /* dst <- src surface, format converted */
    SHR_CMD_ROTATE,       /* dst <- rotated src surface */
    SHR_CMD_CACHE_BEGIN,  /* hint: commands up to CACHE_END draw exactly `dst`, identified by `key` */
    SHR_CMD_CACHE_END
} shr_cmd_kind;

/* DIM: GLYPH coverage, or a FILL, at half strength. BOLD, ITALIC: GLYPH style synthesized from the coverage. */
enum { SHR_GLYPH_DIM = 1u << 0, SHR_GLYPH_BOLD = 1u << 1, SHR_GLYPH_ITALIC = 1u << 2 };
#define SHR_GLYPH_SLANT 54       /* italic slope in 1/256 (about 12 degrees) */
#define SHR_GLYPH_SYNTH_MAX 1024 /* largest src width or height of a BOLD or ITALIC GLYPH */

typedef enum shr_rotation {
    SHR_ROTATE_NONE = 0,
    SHR_ROTATE_90_CW,
    SHR_ROTATE_180,
    SHR_ROTATE_90_CCW
} shr_rotation;

/* Maps a point between logical and rotated output coordinates of a
 * width x height logical screen (inverse: output -> logical). */
shr_status shr_rotation_map_point(shr_rotation rotation, int32_t width, int32_t height, shr_point p, bool inverse,
                                  shr_point *out);

/* Destination rectangles are already clipped by the compositor: each lies inside the destination and inside
 * its group's `dst`. Between CACHE_BEGIN and CACHE_END the commands draw the whole group `dst` (unclipped by
 * damage) and only `cache_clip` of it is written to the destination; equal `key`, `dst` size and destination
 * format mean equal pixels, so a driver may render the group into a separate buffer and copy a cached result.
 * Hence a group is opaque (its commands cover all of `dst` with opaque pixels before anything is blended),
 * reads no destination pixels and holds no ROTATE.
 * ROTATE: `src` is the whole logical surface and `dst` a rect of the output whose size is `src` rotated;
 * `src_origin` is unused.
 * GLYPH coverage of src pixel (x, y), exactly: in(x, y) is the coverage widened to 8 bits (A4 n -> 17n), 0 outside
 * `src` (W x H). BOLD: b(x) = in(x + 1) > in(x) ? in(x) : max(in(x), in(x - 1)), else b = in. ITALIC, with
 * t = SHR_GLYPH_SLANT / 2 * (slant_axis - 2y - 1) + 2^23, k = (t >> 8) - 2^15, f = t & 255:
 * c(x) = (b(x - k) * (256 - f) + b(x - k - 1) * f + 128) >> 8, else c = b. DIM: (c + 1) >> 1. `dst` pixel (x, y)
 * takes the coverage of src (src_origin.x + x - dst.x0, src_origin.y + y - dst.y0). A BOLD or ITALIC GLYPH may
 * name source columns outside `src` within its footprint [x0, x1): x0 = ITALIC ? k(H - 1) : 0,
 * x1 = W + BOLD + (ITALIC ? k(0) + (f(0) != 0) : 0); its rows stay inside `src`.
 * A batch is checked before anything is drawn. SHR_E_INVALID_ARG: a rect outside the destination or its group,
 * a nested or unbalanced group, a group command reading the destination, ROTATE in a group, a source rect
 * outside `src` (or a styled GLYPH's footprint), a BOLD or ITALIC GLYPH with `src` larger than
 * SHR_GLYPH_SYNTH_MAX, an ITALIC GLYPH with |slant_axis| > 4 * SHR_GLYPH_SYNTH_MAX, a bad rotation or ROTATE
 * size. SHR_E_UNSUPPORTED: a source format the kind does not take, a COPY whose source overlaps `dst` with
 * another format or stride, a ROTATE whose source overlaps `dst`. */
typedef struct shr_draw_cmd {
    shr_cmd_kind kind;
    uint32_t flags;
    shr_rect dst;
    shr_color color;          /* FILL, GLYPH */
    shr_image src;            /* GLYPH, IMAGE, COPY, ROTATE */
    shr_point src_origin;     /* src pixel drawn at dst.x0, dst.y0 */
    shr_rotation rotation;    /* ROTATE */
    uint64_t key[2];          /* CACHE_BEGIN: 128-bit content hash */
    shr_rect cache_clip;      /* CACHE_BEGIN: part of dst drawn this time */
    int32_t slant_axis;       /* ITALIC GLYPH: twice the src y the shear turns about */
} shr_draw_cmd;

typedef enum shr_fence_state {
    SHR_FENCE_PENDING = 0,
    SHR_FENCE_SUCCEEDED,
    SHR_FENCE_FAILED,
    SHR_FENCE_CANCELLED
} shr_fence_state;

/* Assigned by the renderer: slot and generation of one submission. */
typedef uint64_t shr_fence;

/* Destinations and sources outside these limits make the submission fail with SHR_E_UNSUPPORTED:
 * a driver implements every command kind (a device driver may run some on the software port). */
typedef struct shr_driver_caps {
    uint32_t domains;         /* shr_memory_domain bits of destinations and sources */
    uint32_t address_align;   /* bytes, 0 = any; buffer start of destinations and sources */
    uint32_t stride_align;
    int32_t max_width;        /* destination, 0 = unlimited */
    int32_t max_height;
    uint64_t timeout_ns;      /* watchdog per submission, 0 = none; needs a clock */
} shr_driver_caps;

/* execute(): SHR_OK = finished before returning; SHR_IN_PROGRESS = the device
 * accesses the buffers until the fence resolves (shr_fence_signal);
 * SHR_E_WOULD_BLOCK = nothing accepted, retried io_retry_ns later or after shr_driver_ready(), or
 * replaced at once by a newer submission;
 * other errors = nothing executed. After SHR_IN_PROGRESS, `dst`, `cmds` and every buffer they refer to
 * stay valid and unchanged until the fence resolves (or reset() returned SHR_OK), so a driver thread may
 * read them in place without copying; after SHR_OK or an error they may change at once.
 * A fence resolves only after the device stopped accessing every buffer.
 * cancel() only requests cancellation. reset() stops the device and returns
 * SHR_OK once no submission can access memory any more.
 * sync() (optional) runs before execute() for each SHR_MEMORY_DMA buffer of it
 * (destination and sources): the CPU may have written them last, the device
 * reads or writes them next. The driver makes what it writes coherent for the CPU and
 * the display itself before the submission completes. */
typedef struct shr_framebuffer_driver {
    void *user;
    shr_driver_caps caps;
    shr_status (*execute)(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t count,
                          shr_fence fence);
    void (*cancel)(void *user, shr_fence fence);
    shr_status (*reset)(void *user);
    void (*sync)(void *user, const void *addr, size_t bytes);
} shr_framebuffer_driver;

shr_status shr_framebuffer_driver_init(shr_framebuffer_driver *driver);

#ifdef __cplusplus
}
#endif

#endif
