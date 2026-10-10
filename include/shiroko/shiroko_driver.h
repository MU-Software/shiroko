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
    SHR_ALLOC_DMA,            /* buffers a device may access */
    /* A hint OR-ed into one of the above: memory used every frame (rows of commands, a band's command list, per-frame
     * state), worth the fastest RAM. Passed only to an allocator whose flags ask for it. */
    SHR_ALLOC_HOT = 1 << 8
} shr_alloc_kind;

/* Both functions or neither (NULL allocator = malloc/free). SHR_ALLOC_DMA
 * requests must return memory a DMA-only driver can access. flags: the hints (SHR_ALLOC_HOT) `kind` may carry; 0 =
 * plain kinds. free() receives the kind alloc() received. Other bits are SHR_E_INVALID_ARG. */
typedef struct shr_allocator {
    void *user;
    void *(*alloc)(void *user, size_t size, size_t align, shr_alloc_kind kind);
    void (*free)(void *user, void *ptr, size_t size, size_t align, shr_alloc_kind kind);
    uint32_t flags;
} shr_allocator;

typedef struct shr_rect {
    int32_t x0, y0, x1, y1; /* [x0,x1) x [y0,y1) */
} shr_rect;

typedef struct shr_point {
    int32_t x, y;
} shr_point;

/* 0xAARRGGBB; drivers ignore the alpha byte: colours draw opaque. */
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

/* Read-only pixels: the memory of a buffer (A4, A8, RGBA8888, RGB565) or a COPY/ROTATE source surface. */
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

/* An image inside a command: a COPY or ROTATE source, the memory of a REGISTER. It spans
 * (height - 1) * stride + the row bytes of width, nothing when it is empty or DEVICE. */
typedef struct shr_image_ref {
    const void *pixels;
    int32_t width;
    int32_t height;
    uint32_t stride;
    uint8_t format;    /* shr_pixel_format */
    uint8_t domain;    /* shr_memory_domain, 0 means SHR_MEMORY_CPU */
    uint16_t reserved; /* unspecified */
} shr_image_ref;

/* The image `ref` names, byte_length what it spans; SHR_E_INVALID_ARG (or SHR_E_OVERFLOW) where shr_image_validate
 * would refuse that image. */
shr_status shr_image_ref_get(const shr_image_ref *ref, shr_image *out) SHR_NONBLOCKING;

typedef enum shr_cmd_kind {
    SHR_CMD_FILL = 1,        /* dst <- color */
    SHR_CMD_GLYPH,           /* dst <- color through the A4/A8 coverage of `src_rect` of `buffer` */
    SHR_CMD_IMAGE,           /* dst <- RGBA8888 `src_rect` of `buffer`, source-over; RGB565: copied */
    SHR_CMD_COPY,            /* dst <- src surface, format converted */
    SHR_CMD_ROTATE,          /* dst <- rotated src image */
    SHR_CMD_KEEP_BEGIN,      /* commands up to KEEP_END draw `dst` into keep `buffer` instead of the destination */
    SHR_CMD_KEEP_END,
    SHR_CMD_KEEP_DRAW,       /* dst <- keep `buffer`, copied exactly */
    SHR_CMD_BUFFER_REGISTER, /* id `buffer` names memory `src` from now on, replacing what it named */
    SHR_CMD_BUFFER_UPDATE,   /* the pixels of `buffer` inside `src_rect` changed */
    SHR_CMD_BUFFER_RELEASE,  /* id `buffer` names nothing; no effect when it named nothing */
    SHR_CMD_KEEP_RELEASE,    /* keep `buffer` holds nothing; no effect when it held nothing */
    SHR_CMD_LINE             /* dst <- color through the coverage of a line pattern repeated along x */
} shr_cmd_kind;

/* DIM: GLYPH coverage, or a FILL, at half strength. BOLD, ITALIC: GLYPH style synthesized from the coverage.
 * ON_FILL: hint that the GLYPH's `dst` holds the FILL colour `bg` (shr_draw_cmd). */
enum { SHR_GLYPH_DIM = 1u << 0, SHR_GLYPH_BOLD = 1u << 1, SHR_GLYPH_ITALIC = 1u << 2, SHR_GLYPH_ON_FILL = 1u << 3 };
/* COPY onto its own destination: the pixels of the destination outside `dst` become unspecified (shr_draw_cmd). */
enum { SHR_COPY_REST_UNDEFINED = 1u << 4 };
/* IMAGE drawn from `src_rect` scaled bilinearly to scale_w x scale_h (only to SHR_DRIVER_SCALE). */
enum { SHR_IMAGE_SCALED = 1u << 5 };
/* LINE shapes, in flags >> SHR_LINE_SHAPE_SHIFT (with SHR_GLYPH_DIM). */
enum { SHR_LINE_SINGLE = 0, SHR_LINE_DOUBLE, SHR_LINE_CURLY, SHR_LINE_DOTTED, SHR_LINE_DASHED };
#define SHR_LINE_SHAPE_SHIFT 8
#define SHR_LINE_MAX_PERIOD 32 /* largest LINE src_rect width */
#define SHR_LINE_MAX_BAND 8    /* largest LINE src_rect height */
#define SHR_GLYPH_SLANT 54       /* italic slope in 1/256 (about 12 degrees) */
#define SHR_GLYPH_SYNTH_MAX 1024 /* largest src_rect width or height of a BOLD or ITALIC GLYPH */

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

/* A command (64 bytes) sets `kind`, `flags` (0 when the kind takes none) and the fields its kind reads: FILL dst,
 * color; GLYPH buffer, dst, src_origin, color, src_rect, with ON_FILL bg, with ITALIC slant_axis; IMAGE buffer, dst,
 * src_origin, src_rect; COPY dst, src_origin, src; ROTATE rotation, dst, src; LINE dst, src_origin, color, src_rect;
 * KEEP_BEGIN buffer, dst; KEEP_END nothing more; KEEP_DRAW buffer, dst, src_origin; BUFFER_REGISTER buffer, src;
 * BUFFER_UPDATE buffer, src_rect; BUFFER_RELEASE and KEEP_RELEASE buffer. Its other fields are unspecified, and a
 * driver reads none of them. Destination rectangles are already clipped by the compositor: each lies inside the
 * destination, except in keep groups.
 * Keeps: pixels the driver holds under an id in 1..caps.max_keeps (none when 0), from KEEP_BEGIN until KEEP_RELEASE or
 * the next KEEP_BEGIN of the id. The commands from KEEP_BEGIN to KEEP_END lie inside its `dst` (W x H, which may reach
 * outside the destination, within 2^24 pixels of its origin) and draw exactly `dst` into keep `buffer` instead of the
 * destination; the keep then holds W x H pixels of the destination format. A keep group is opaque (its commands cover
 * all of `dst` with opaque pixels before anything is blended), reads no destination pixels and holds no ROTATE or
 * keep command. KEEP_DRAW: `dst` takes keep pixels exactly, keep pixel `src_origin` at dst.x0, dst.y0, into a
 * destination of the keep's format. Within a batch an id is stored at most once and never drawn before it is stored
 * there, so a driver may store a batch's groups first; KEEP_DRAW draws what an earlier accepted batch or an earlier
 * group of the batch stored. KEEP_RELEASE comes with the buffer commands at the start of a batch. Held keeps take
 * W * H * bytes per pixel each, at most caps.max_keep_bytes each and in sum at most caps.keep_bytes when these are
 * not 0. After an error or a timeout what every keep id holds is unspecified: the compositor releases each id it may
 * have stored before using it again, as it releases every id before its first frame.
 * After SHR_E_NO_MEMORY from a batch storing keeps, it keeps no more bytes than were held before that batch.
 * ROTATE: `dst` takes `src`, any image, rotated: its size is the rotated size of `src`.
 * A COPY whose `src` is the destination itself (same pixels, format and stride, or the same DEVICE surface) moves
 * pixels: `dst` takes them as they were before the COPY, whatever the overlap. The compositor sends such moves, ahead
 * of the other draws of a batch, only to a driver with SHR_DRIVER_CHEAP_MOVE. With SHR_COPY_REST_UNDEFINED the
 * destination pixels outside `dst` become unspecified as well, since later commands of the frame draw all of them, so
 * a driver may move the whole destination, or where it is scanned out from, instead of `dst` alone.
 * Buffers: GLYPH and IMAGE draw from buffers, memory the driver knows by an id in 1..caps.max_buffers. An id keeps
 * its memory across batches, from BUFFER_REGISTER until RELEASE or the next REGISTER of the id. Buffer commands come
 * only at the start of a batch, before any other command, and take effect in order before its draws. Pixels
 * written after a REGISTER reach the driver only through an UPDATE covering them (or a new REGISTER) at the start
 * of the batch that next draws from them. A driver accesses buffer memory only while it executes a batch naming
 * the buffer in a draw, REGISTER or UPDATE; with SHR_BUFFER_COPIES it reads the memory only for REGISTER and
 * UPDATE and draws from its own copy. Buffer memory never overlaps a destination.
 * GLYPH, IMAGE: `src_rect` (W x H) is a rect of the registered buffer `buffer` and `src_origin` is relative to its
 * top-left: `dst` pixel (x, y) takes rect pixel (src_origin.x + x - dst.x0, src_origin.y + y - dst.y0). For A4,
 * src_rect.x0 is even. A SCALED IMAGE takes that pixel of `src_rect` scaled to scale_w x scale_h (1..32767 each, as
 * is each src_rect side): bilinear taps at pixel centres, ((2x + 1) W + scale_w) 2^15 / scale_w = t in 16.16 one pixel
 * up, columns k = src_rect.x0 + (t >> 16) - 1 and k + 1 clamped to the buffer, weight f = (t >> 8) & 255 of k + 1
 * (rows likewise), each channel (top * (256 - fy) + bottom * fy + 32768) >> 16 with top and bottom the rows'
 * a * (256 - fx) + b * fx. It may read one buffer pixel beyond `src_rect` on each side. A SCALED IMAGE also reads
 * scale_w and scale_h; one reading outside scale_w x scale_h or sized beyond 1..32767 is invalid (SHR_E_INVALID_ARG).
 * IMAGE from an RGB565 buffer (SHR_DRIVER_IMAGE_565): the pixels are opaque, copied as COPY converts formats; a
 * SCALED one widens each pixel to 8 bits a channel (c << 3 | c >> 2, g << 2 | g >> 4, alpha 255) first.
 * GLYPH coverage of rect pixel (x, y), exactly: in(x, y) is the coverage of buffer pixel
 * (src_rect.x0 + x, src_rect.y0 + y) widened to 8 bits (A4 n -> 17n) for 0 <= x < W and 0 <= y < H, and 0
 * elsewhere, also where the buffer has pixels. BOLD: b(x) = in(x + 1) > in(x) ? in(x) : max(in(x), in(x - 1)),
 * else b = in. ITALIC, with t = SHR_GLYPH_SLANT / 2 * (slant_axis - 2y - 1) + 2^23, k = (t >> 8) - 2^15,
 * f = t & 255: c(x) = (b(x - k) * (256 - f) + b(x - k - 1) * f + 128) >> 8, else c = b. DIM: (c + 1) >> 1.
 * A BOLD or ITALIC GLYPH may take rect columns outside the rect, and outside the buffer, within its footprint
 * [x0, x1): x0 = ITALIC ? k(H - 1) : 0, x1 = W + BOLD + (ITALIC ? k(0) + (f(0) != 0) : 0); its rows stay inside
 * the rect.
 * LINE: `src_rect` is the pattern cell (0, 0, W, H), 1 <= W <= SHR_LINE_MAX_PERIOD, 1 <= H <= SHR_LINE_MAX_BAND,
 * repeated along x: `dst` pixel (x, y) takes cell pixel ((src_origin.x + x - dst.x0) mod W, src_origin.y + y - dst.y0),
 * with 0 <= src_origin.x < W and the rows inside the cell. Coverage of cell pixel (x, y) by shape: SINGLE 255; DOUBLE
 * 255 in rows 0 and H - 1, else 0; DASHED 255 where x / (W / 3 + 1) is even, else 0; DOTTED and CURLY sampled: n of the
 * 16 x 16 points (x + (i + 0.5) / 16, y + (j + 0.5) / 16) lie in the shape, a = (255 n + 128) >> 8, coverage
 * 17 ((15 a + 127) / 255). DOTTED: discs of radius r = sqrt(1/2) centred at ((k + 0.5) W / m, H / 2), k < m,
 * m = max(1, min(ceil(W / 4r), floor(W / 3r), floor(W / (2r + 1)))). CURLY: points within 1/2 of the polyline through
 * the cubic Beziers (0, b) (0.4c, b) (c - 0.4c, 0.5) (c, 0.5) and (c, 0.5) (c + 0.4c, 0.5) (W - 0.4c, b) (W, b),
 * c = W / 2, b = H - 0.5, each at t = k / 32, k <= 32, and their copies moved by -W and W. Computed in single
 * precision as shr__raster_line_coverage() of the software port computes it. DIM: (c + 1) >> 1. Blended as GLYPH.
 * ON_FILL GLYPH: for each `dst` pixel, the last earlier command of the batch whose `dst` holds it is a FILL without
 * DIM of colour `bg`, in the same keep group as the GLYPH or, like it, outside any. Blending onto the pixel that FILL
 * writes (RGB565: `bg` quantized to 5/6/5 bits; RGBX8888: its bytes, X included) then equals blending onto `dst`, so a
 * driver may do that without reading `dst`, or ignore the flag.
 * A batch is checked before anything is drawn. SHR_E_INVALID_ARG: a kind outside shr_cmd_kind, a rect outside the
 * destination or its group, a nested or unbalanced group, a COPY in a group reading the destination, ROTATE or a keep
 * command in a group, a buffer command or KEEP_RELEASE after another command, REGISTER of id 0, of an id above
 * caps.max_buffers or of invalid memory, UPDATE or a draw naming an id that is not registered, an UPDATE or draw
 * `src_rect` outside its buffer, an A4 draw `src_rect` with odd x0, a LINE with a shape outside SHR_LINE_*, a flag
 * other than DIM or a `src_rect` or `src_origin` outside the rules above, a draw reading outside its `src_rect` (a
 * styled GLYPH outside its footprint) or a COPY outside `src`, a BOLD or ITALIC GLYPH with a `src_rect` larger than
 * SHR_GLYPH_SYNTH_MAX, an ITALIC GLYPH with |slant_axis| > 4 * SHR_GLYPH_SYNTH_MAX, a bad rotation or ROTATE size, a
 * KEEP_BEGIN or KEEP_DRAW of id 0 or above caps.max_keeps, a second KEEP_BEGIN of an id in the batch, a KEEP_DRAW of an
 * id that holds nothing or is stored later in the batch, reading outside its keep or into a destination of another
 * format, a keep group taking more than caps.max_keep_bytes, stores leaving more than caps.keep_bytes held.
 * SHR_E_UNSUPPORTED: REGISTER of a format other than A4, A8, RGBA8888 and (SHR_DRIVER_IMAGE_565) RGB565 or of
 * memory outside the caps, a buffer or source format the kind does not take, a COPY whose source overlaps `dst` with
 * another format or stride, a ROTATE whose source overlaps `dst`.
 * SHR_E_NO_MEMORY: no memory for a keep the batch stores. */
#if defined(__cplusplus) && defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
typedef struct shr_draw_cmd {
    uint8_t kind;          /* shr_cmd_kind */
    uint8_t rotation;      /* ROTATE: shr_rotation */
    uint16_t flags;
    uint32_t buffer;       /* GLYPH, IMAGE, BUFFER_*: buffer id; KEEP_*: keep id */
    shr_rect dst;
    shr_point src_origin;  /* GLYPH, IMAGE, LINE: rect pixel at dst.x0, y0; COPY: src pixel; KEEP_DRAW: keep pixel */
    union {
        struct {
            shr_color color;   /* FILL, GLYPH, LINE */
            shr_color bg;      /* GLYPH: the colour under `dst` with ON_FILL */
            shr_rect src_rect; /* GLYPH, IMAGE: buffer region drawn from; LINE: pattern cell; UPDATE: region changed */
        };
        shr_image_ref src;     /* COPY, ROTATE: source surface; REGISTER: the buffer's memory */
    };
    union {
        int32_t slant_axis; /* GLYPH with ITALIC: twice the rect y the shear turns about */
        int32_t scale_w;    /* SCALED IMAGE */
    };
    union {
        uint32_t reserved;
        int32_t scale_h;
    };
} shr_draw_cmd;
#if defined(__cplusplus) && defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef enum shr_fence_state {
    SHR_FENCE_PENDING = 0,
    SHR_FENCE_SUCCEEDED,
    SHR_FENCE_FAILED,
    SHR_FENCE_CANCELLED
} shr_fence_state;

/* Assigned by the renderer: slot and generation of one submission. */
typedef uint64_t shr_fence;

enum { SHR_BUFFER_COPIES = 1u << 0 }; /* buffer_flags: the driver draws from its own copies of buffers */
/* flags: moving pixels inside a destination (a COPY onto itself) costs less than drawing them again; without it the
 * compositor draws moved pixels again. */
enum { SHR_DRIVER_CHEAP_MOVE = 1u << 1 };
/* flags: storing a keep group costs about what drawing it plainly does (the copy into the keep runs off the CPU); the
 * compositor then also stores groups it draws for the first time, within a credit that keeps drawn again earn back.
 * Like all caps, read by shr_create(). */
enum { SHR_DRIVER_CHEAP_STORE = 1u << 2 };
/* flags: the driver draws SCALED IMAGEs. */
enum { SHR_DRIVER_SCALE = 1u << 3 };
/* flags: IMAGE also draws from RGB565 buffers; on an RGB565 screen the image resource keeps opaque images so. */
enum { SHR_DRIVER_IMAGE_565 = 1u << 4 };

/* Destinations, sources and buffers outside these limits make the submission fail with SHR_E_UNSUPPORTED:
 * a driver implements every command kind (a device driver may run some on the software port). */
typedef struct shr_driver_caps {
    uint32_t domains;          /* shr_memory_domain bits of destinations, sources and buffers */
    uint32_t address_align;    /* bytes, 0 = any; start of destinations, sources and buffers */
    uint32_t stride_align;
    int32_t max_width;         /* destination, 0 = unlimited */
    int32_t max_height;
    uint64_t timeout_ns;       /* watchdog per submission, 0 = none; needs a clock */
    uint32_t max_buffers;      /* buffer ids are 1..max_buffers */
    int32_t max_buffer_width;  /* 0 = unlimited */
    int32_t max_buffer_height;
    uint64_t buffer_bytes;     /* SHR_BUFFER_COPIES: byte_length sum of registered buffers, 0 = unlimited */
    uint32_t buffer_flags;
    uint32_t max_keeps;        /* keep ids are 1..max_keeps; 0 = the driver keeps nothing */
    uint64_t keep_bytes;       /* bytes held keeps may take, 0 = unlimited */
    uint64_t max_keep_bytes;   /* bytes one keep may take, 0 = up to keep_bytes */
    uint32_t flags;            /* SHR_DRIVER_* */
} shr_driver_caps;

/* execute(): SHR_OK = finished before returning; SHR_IN_PROGRESS = the device
 * accesses the buffers until the fence resolves (shr_fence_signal);
 * SHR_E_WOULD_BLOCK = nothing accepted, retried io_retry_ns later or after shr_driver_ready(), or
 * replaced at once by a newer submission;
 * other errors = nothing drawn. After such an error or a timeout, which buffer ids are registered is unspecified
 * (buffer commands of the batch may have taken effect): the compositor registers buffers again before drawing
 * from them. After SHR_IN_PROGRESS, `dst`, `cmds`, the COPY and ROTATE sources and the memory of every buffer the
 * batch names (with SHR_BUFFER_COPIES: in a REGISTER or UPDATE) stay valid and unchanged until the fence resolves
 * (or reset() returned SHR_OK), so a driver thread may read them in place without copying; after SHR_OK or an
 * error they may change at once.
 * A fence resolves only after the device stopped accessing every buffer.
 * cancel() only requests cancellation. reset() stops the device and returns
 * SHR_OK once no submission can access memory any more.
 * sync() (optional) runs before execute() for the SHR_MEMORY_DMA memory the batch hands the device: the whole
 * destination (once per frame, before its first batch drawing there: only batches write it after that), each whole
 * COPY or ROTATE source, the whole memory of each REGISTER and, for each UPDATE, the rows
 * src_rect.y0 .. y1 - 1 of its buffer. The CPU may have written them last, the device reads or writes them next.
 * The driver makes what it writes coherent for the CPU and the display itself before the submission completes. */
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
