#ifndef SHIROKO_RASTER_H
#define SHIROKO_RASTER_H

#include <shiroko/shiroko_driver.h>
#include <stdatomic.h>

/* Whether a driver draws into or reads a valid buffer: SHR_OK or SHR_E_UNSUPPORTED. */
typedef shr_status (*shr__reach_fn)(const void *user, const void *pixels, int32_t width, int32_t height,
                                    shr_pixel_format format, shr_memory_domain domain) SHR_NONBLOCKING;

/* REGISTER, UPDATE, RELEASE or KEEP_RELEASE, which only lead a batch. */
static inline bool shr__buffer_cmd(shr_cmd_kind k) {
    return (k >= SHR_CMD_BUFFER_REGISTER) & (k <= SHR_CMD_KEEP_RELEASE);
}

/* A keep: `width` x `height` pixels of `format` (0: holds nothing, `size` 0) in `size` bytes. `mark`, `stored` and the
 * store size are the check's: the batch that stored or drew it. */
typedef struct shr__keep {
    void *pixels;
    size_t size;
    int32_t width, height;
    shr_pixel_format format;
    uint64_t mark;
    bool stored;
    int32_t store_w, store_h;
} shr__keep;

/* Keep ids 1..n at at[id - 1], each at most `slot` bytes (0 = any). The check bumps `batch` and lists the ids the
 * batch stores in stores[0, nstores). */
typedef struct shr__keeps {
    shr__keep *at;
    uint32_t *stores;
    uint32_t n, nstores;
    uint64_t slot, batch;
} shr__keeps;
/* Buffer tables hold ids 1..n at [id - 1]; an entry with format 0 is not registered.
 * Checks one buffer command against `buffers` as the commands before it left the table; `fn` as below. A REGISTER
 * writes the memory it names to *mem. */
shr_status shr__raster_buffer_check(const shr_draw_cmd *c, const shr_image *buffers, uint32_t n, shr__reach_fn fn,
                                    const void *user, shr_image *mem) SHR_NONBLOCKING;
/* Validates a whole batch (keep groups included) so a bad command never leaves a partial write. Leading buffer
 * commands are skipped: draws are checked against `buffers` and `keeps` (NULL: none) as they leave them. `fn` decides
 * which destination and source buffers the driver reaches; NULL: the software port's, all but DEVICE. DEVICE buffers
 * are identified by `pixels` and overlap only when it is the same. */
shr_status shr__raster_check(const shr_surface *dst, const shr_draw_cmd *cmds, size_t count, const shr_image *buffers,
                             uint32_t n, shr__keeps *keeps, shr__reach_fn fn, const void *user) SHR_NONBLOCKING;
/* The source shares bytes, or the DEVICE buffer, with the destination. */
bool shr__raster_reads_dst(const shr_surface *dst, const shr_image_ref *m) SHR_NONBLOCKING;
/* ON_FILL GLYPH palettes a driver keeps between commands, by the key of shr__pals (never 0), direct-mapped. */
#define SHR__MEMO_PALS 16
typedef struct shr__pal_memo {
    uint32_t key[SHR__MEMO_PALS][2];
    uint32_t pal[SHR__MEMO_PALS][16];
} shr__pal_memo;

/* Draws one checked FILL/GLYPH/IMAGE/COPY/ROTATE/LINE command moved by -origin and limited to `clip`; ON_FILL GLYPH
 * palettes through `memo` (NULL: none). */
void shr__raster_draw(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *buffers, shr_point origin,
                      shr_rect clip, shr__pal_memo *memo) SHR_NONBLOCKING;

/* The coverage (before DIM) of the w x h LINE pattern cell of `shape`, as shiroko_driver.h defines it, row by row into
 * out[w * h]. DOTTED and CURLY take some 10^5 float operations: drivers keep them. */
void shr__raster_line_coverage(uint8_t *out, uint32_t shape, int32_t w, int32_t h) SHR_NONBLOCKING;
/* The CURLY and DOTTED cells a driver keeps between commands: key w << 8 | h, 0 for none. */
typedef struct shr__line_memo {
    uint32_t key[2];
    uint8_t cov[2][SHR_LINE_MAX_PERIOD * SHR_LINE_MAX_BAND];
} shr__line_memo;
/* The cell of a checked LINE, from `memo` (NULL: computed into `tmp`). */
const uint8_t *shr__raster_line_cell(const shr_draw_cmd *c, shr__line_memo *memo, uint8_t *tmp) SHR_NONBLOCKING;
/* Draws a checked LINE as shr__raster_draw does, its cell through `memo`. */
void shr__raster_line(const shr_surface *dst, const shr_draw_cmd *c, shr_point origin, shr_rect clip,
                      shr__line_memo *memo) SHR_NONBLOCKING;

/* RGBA8888 buffer `b` prepared for IMAGE into RGB565: per pixel the words n_r << 16 | n_b and n_g << 8 | A, with
 * A = 255 - alpha and n = (m * c * alpha + 32512) / 255 + 1 for colour c of m + 1 levels, so that blending onto 565
 * value d gives (n - 1 + d * A) / 255 exactly; after them, per row the columns [x0, x1) from its first to past its last
 * alpha. */
static inline size_t shr__prep_bytes(const shr_image *b) {
    return ((size_t)b->width * (size_t)b->height + (size_t)b->height) * 8;
}
/* Prepares rect `r` of `b` into `plane`. */
void shr__raster_prep(uint32_t *plane, const shr_image *b, shr_rect r) SHR_NONBLOCKING;
/* Draws a checked IMAGE into an RGB565 `dst` as shr__raster_draw does, from the plane of its buffer `b`. */
void shr__raster_image(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *b, const uint32_t *plane,
                       shr_point origin, shr_rect clip) SHR_NONBLOCKING;

/* The BOLD/ITALIC coverage (before DIM) of `flags` and `axis` from `rect` of A4/A8 buffer `b`, for rect columns
 * [x0, x0 + w) of every rect row, into w bytes per row at `out`. */
void shr__raster_synth(uint8_t *out, const shr_image *b, shr_rect rect, uint32_t flags, int32_t axis, int32_t x0,
                       int32_t w) SHR_NONBLOCKING;

/* The 256 pixels of format f that coverage 0..255 of `color` (DIM: halved) leaves on a `bg` pixel, as GLYPH draws. */
void shr__raster_lut(uint8_t *lut, shr_pixel_format f, shr_color color, shr_color bg, uint32_t dim) SHR_NONBLOCKING;
/* Draws A8 coverage rows `src`, `stride` bytes apart, onto rect `r` of `dst` through the LUT of a GLYPH's colour, DIM
 * and bg, as the GLYPH draws them where `dst` holds the ON_FILL colour. */
void shr__raster_glyph_lut(const shr_surface *dst, shr_rect r, const uint8_t *src, size_t stride,
                           const uint8_t *lut) SHR_NONBLOCKING;

/* ON_FILL GLYPH palettes a SHR_SCALAR_BLEND build keeps for every driver in the process, by colour, bg, DIM and format:
 * an entry's `seq` is odd while it is written, a set's `next` names the way it replaces next. */
#define SHR__PAL_SETS 8
#define SHR__PAL_WAYS 8
typedef struct shr__pal {
    _Atomic uint32_t seq, key[2], pal[16];
} shr__pal;
typedef struct shr__pal_set {
    shr__pal way[SHR__PAL_WAYS];
    _Atomic uint32_t next;
} shr__pal_set;
extern shr__pal_set shr__pals[SHR__PAL_SETS];

static inline size_t shr__px_bytes(shr_pixel_format f) { return f == SHR_FORMAT_RGB565 ? 2 : 4; }
static inline uint32_t shr__quantize(uint32_t c, uint32_t m) { return (c * m + 127) / 255; }

#endif
