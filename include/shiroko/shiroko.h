#ifndef SHIROKO_H
#define SHIROKO_H

#include "shiroko_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shr_error_info {
    shr_status status;
    size_t byte_offset; /* SIZE_MAX = not applicable */
    size_t item_index;  /* SIZE_MAX = not applicable */
    const char *reason; /* static string or NULL */
} shr_error_info;

/* ===== Context (compositor) =====
 * All calls come from one owner thread except shr_fence_signal(), shr_asset_complete(), shr_driver_ready()
 * and shr_asset_ready(). The first two must not be called for a submission after the driver's reset()
 * returned SHR_OK or for a read after the source's cancel() returned, and none of the four during or after
 * shr_destroy(): stop completion threads first.
 * Host callbacks (output, driver, asset source, log, trace) must not call back into the context: such
 * calls fail with SHR_E_STATE, except those four.
 * now_ns and the allocator must not call Shiroko at all (this is not checked). */

typedef struct shr_context shr_context;

enum {
    SHR_OUTPUT_RELEASE_ON_PRESENT = 1u << 0, /* present() consumed the frame; no shr_output_released() */
    SHR_OUTPUT_PRESERVES_CONTENT = 1u << 1   /* an acquired surface still holds what was last written to it
                                                unless its generation changed */
};

typedef enum shr_timestamp_kind {
    SHR_TIMESTAMP_NONE = 0,
    SHR_TIMESTAMP_SCANOUT, /* the frame started scanning out */
    SHR_TIMESTAMP_VSYNC,   /* the vsync at which the frame became visible */
    SHR_TIMESTAMP_COMPOSITOR
} shr_timestamp_kind;

typedef struct shr_output {
    void *user;
    uint32_t flags;
    shr_timestamp_kind timestamp;
    shr_status (*acquire)(void *user, shr_surface *out);
    /* SHR_OK = accepted, SHR_E_WOULD_BLOCK = retry later, other = failed. */
    shr_status (*present)(void *user, const shr_surface *surface, uint64_t frame_id);
    void (*discard)(void *user, const shr_surface *surface); /* returns an acquired, unpresented surface */
} shr_output;

shr_status shr_output_init(shr_output *output);

/* The phase follows the clock alone; shr_submit() does not restart it. */
typedef struct shr_blink_profile {
    uint64_t interval_ns; /* 0 = never blinks (always visible) */
    uint64_t epoch_ns;
    bool start_visible;
} shr_blink_profile;

typedef enum shr_trace_kind {
    SHR_TRACE_SUBMIT = 1,
    SHR_TRACE_RASTER_BEGIN,   /* value1 = damaged pixels */
    SHR_TRACE_RASTER_END,     /* value0 = commands the frame's batches held */
    SHR_TRACE_CONVERT,        /* value0 = bytes read + written; with bands once all are converted, for all */
    SHR_TRACE_PRESENT,
    SHR_TRACE_DISPLAYED,      /* value0 = output timestamp */
    SHR_TRACE_IO_BEGIN,       /* value0 = bytes */
    SHR_TRACE_IO_END,         /* value1 = shr_status */
    SHR_TRACE_PAGE_READY,
    SHR_TRACE_PAGE_EVICT,
    SHR_TRACE_FENCE_WAIT      /* value0 = ns */
} shr_trace_kind;

typedef struct shr_trace_event {
    shr_trace_kind kind;
    uint64_t time_ns;
    uint64_t id; /* frame or request id */
    uint64_t value0, value1;
} shr_trace_event;

typedef struct shr_context_desc {
    const shr_allocator *allocator; /* its flags may ask for SHR_ALLOC_HOT: memory used every frame */
    void *user; /* passed to now_ns, log and trace */
    /* Monotonic. NULL means no timers: io_retry_ns, io_timeout_ns, min_frame_interval_ns and the
     * driver's caps.timeout_ns must then be 0, and blinking stays visible. */
    uint64_t (*now_ns)(void *user);
    const shr_framebuffer_driver *driver;
    const shr_output *output;
    shr_blink_profile blink;      /* SHR_STYLE_BLINK cells */
    uint32_t event_capacity;      /* at least max_unreleased_frames + 2; + 3 lets a composed frame render ahead */
    uint32_t max_unreleased_frames;
    uint32_t max_commands;        /* per frame; beyond it the frame fails with SHR_E_LIMIT */
    uint32_t max_reads;           /* outstanding asset reads, at most 65535 */
    uint64_t page_cache_bytes;    /* font pages */
    uint64_t image_bytes;         /* image buffers (padded rows, second buffers while updated); SHR_E_LIMIT beyond */
    /* Failed opens and reads of a package (until it is ready) or page before it cools down, and cool-downs in
     * a row after which only shr_asset_ready() retries it; clamped to 65535. */
    uint32_t io_retry_limit;
    /* Delay before retrying a failed read and before a cooled-down package or page is tried again; also the
     * delay before retrying work a driver or source refused (SHR_E_WOULD_BLOCK). 0 (always without a clock):
     * refused work and cooled-down fonts wait for shr_driver_ready() / shr_asset_ready() instead, and
     * shr_next_deadline() reports no deadline for them. */
    uint64_t io_retry_ns;
    uint64_t io_timeout_ns;       /* reads of cancellable sources; 0 = no watchdog */
    /* Frame-rate cap: a frame starts no sooner than this after the previous one started (frames whose state the
     * output already shows do not count); changes made meanwhile are drawn together by that frame, and
     * shr_next_deadline() reports its start. 0 = uncapped. */
    uint64_t min_frame_interval_ns;
    void (*log)(void *user, shr_status status, const char *message);
    void (*trace)(void *user, const shr_trace_event *event);
} shr_context_desc;

shr_status shr_context_desc_init(shr_context_desc *desc);
shr_status shr_create(const shr_context_desc *desc, shr_context **out_ctx);
shr_status shr_begin_shutdown(shr_context *ctx);
/* Begins shutdown if needed, then frees the context. SHR_E_WOULD_BLOCK while work, reads or presented
 * frames are outstanding (keep pumping) or while layers or resources of the context are alive. */
shr_status shr_destroy(shr_context *ctx);

enum { SHR_SCREEN_COMPOSITION = 1u << 0 };

/* Logical screen in SHR_PIXEL_FORMAT. With a composition surface the frame is composed there and
 * converted (rotation/format) into the output; required whenever rotation != NONE or
 * output_format != SHR_PIXEL_FORMAT, unless bands are given.
 * Bands replace the composition: the damage in screen rows [k * h, k * h + h) (h = the bands' height) is drawn into the
 * next of `bands` in turn, and a batch of ROTATE (COPY without rotation) commands converts what was drawn into the
 * output, which is then tracked as without a composition. A band's commands are built once the band before ran, from
 * the layers as they are then, into one command list allocated here. 1 or 2 app-owned surfaces in SHR_PIXEL_FORMAT,
 * `width` wide, of one height of at most `height`, not SHR_MEMORY_DEVICE: each command reads a part of one. With
 * `band_align` every output rect of those commands has its edges on multiples of it (more than the damage is drawn),
 * and the band height and the screen size must be multiples of it; 0 = any. A part starts a multiple of band_align (or
 * 1) pixels and rows into its band, so the driver's address_align must divide both steps in bytes. */
typedef struct shr_screen_desc {
    int32_t width;
    int32_t height;
    shr_rotation rotation;
    shr_pixel_format output_format; /* 0 = SHR_PIXEL_FORMAT */
    uint32_t flags;
    const shr_surface *composition; /* app-owned buffer, or NULL to allocate */
    shr_color clear;                /* under every layer */
    const shr_surface *bands;       /* band_count surfaces, NULL without bands */
    uint32_t band_count;            /* 0 = no bands, else 1 or 2 */
    uint32_t band_align;
} shr_screen_desc;

shr_status shr_screen_desc_init(shr_screen_desc *desc);
/* SHR_E_UNSUPPORTED when the driver cannot draw into or read from the composition or the bands (caps).
 * SHR_E_NO_MEMORY without memory for the composition or the band command list.
 * SHR_E_WOULD_BLOCK while the driver runs a submission or an isolated frame holds its buffers: pump and
 * retry. A frame waiting for the driver or the output is superseded. On failure the previous
 * configuration stays active. */
shr_status shr_screen_configure(shr_context *ctx, const shr_screen_desc *desc);

/* Asks for a frame of the current layer state. Never blocks: while the output is busy the request
 * is kept and the newest state is drawn once it is free; a frame already drawn is presented first.
 * Layer plugins (tilemap) bring their layers up to date here. If one fails, the other layers are
 * still submitted, the failed part stays pending for the next shr_submit(), and the first error is
 * returned. */
shr_status shr_submit(shr_context *ctx);
shr_status shr_pump(shr_context *ctx);

typedef enum shr_deadline_kind {
    SHR_DEADLINE_NONE = 0,
    SHR_DEADLINE_NOW,
    SHR_DEADLINE_AT
} shr_deadline_kind;

typedef struct shr_deadline {
    shr_deadline_kind kind;
    uint64_t at_ns;
} shr_deadline;

/* When shr_pump() has work next. Work a driver or source refused (SHR_E_WOULD_BLOCK) is due io_retry_ns
 * later or at shr_driver_ready() / shr_asset_ready(), whichever comes first; a frame the output refused
 * waits for shr_output_ready() alone. Never AT without a clock. */
shr_status shr_next_deadline(const shr_context *ctx, shr_deadline *out);

typedef enum shr_event_kind {
    SHR_EVENT_PRESENT_ACCEPTED = 1,
    SHR_EVENT_PRESENT_FAILED,  /* a frame failed (raster, convert, acquire, present, limit); status says why */
    SHR_EVENT_FRAME_SUPERSEDED, /* not presented: replaced by a newer submission while it waited for the
                                   driver or the output, the screen was reconfigured, shut down, or the
                                   output already showed its state */
    SHR_EVENT_FRAME_RELEASED,
    SHR_EVENT_FRAME_DISPLAYED,
    SHR_EVENT_DRIVER_TIMEOUT,
    SHR_EVENT_OUTPUT_ISOLATED,
    SHR_EVENT_RESOURCE_FAILED, /* a font package or other resource failed; status says why */
    SHR_EVENT_OVERFLOW         /* RESOURCE_FAILED events were dropped */
} shr_event_kind;

typedef struct shr_event {
    shr_event_kind kind;
    uint64_t frame_id;
    uint64_t timestamp_ns;
    shr_status status;
} shr_event;

/* SHR_E_NOT_FOUND when the queue is empty. Every frame (the id SHR_TRACE_SUBMIT reports) ends with
 * exactly one PRESENT_ACCEPTED, PRESENT_FAILED or FRAME_SUPERSEDED. Only RESOURCE_FAILED may be
 * dropped; OVERFLOW then stands where the first dropped one would have been. */
shr_status shr_poll_event(shr_context *ctx, shr_event *out);

shr_status shr_output_released(shr_context *ctx, uint64_t frame_id);
/* Once per presented frame, for the last few presented frames only. */
shr_status shr_output_displayed(shr_context *ctx, uint64_t frame_id, uint64_t timestamp_ns);
/* The output can take work again after acquire() or present() returned SHR_E_WOULD_BLOCK; until then
 * nothing is acquired from or presented to it (io_retry_ns does not apply). Wakes nothing by itself:
 * shr_next_deadline() then reports NOW and the application calls shr_pump(). */
shr_status shr_output_ready(shr_context *ctx);
/* The driver can take work again after refusing it with SHR_E_WOULD_BLOCK. Thread-safe; wakes nothing
 * by itself: shr_next_deadline() then reports NOW and the application calls shr_pump(). */
shr_status shr_driver_ready(shr_context *ctx);
/* Device failure after a present was accepted: presents stop until recovery. */
shr_status shr_output_error(shr_context *ctx, shr_status status);
shr_status shr_output_recover(shr_context *ctx);
/* Redraws the whole screen on the next frame, e.g. after the output lost its contents. */
shr_status shr_request_redraw(shr_context *ctx);

/* Thread-safe completion of the submission execute() accepted with SHR_IN_PROGRESS. SHR_E_NOT_FOUND
 * for any other fence and after the first signal; SHR_E_INVALID_ARG for 0 and values >= 2^62. */
shr_status shr_fence_signal(shr_context *ctx, shr_fence fence, shr_fence_state state);

/* ===== Layers =====
 * A layer holds a retained command list in its own coordinates, clipped to its rectangle. Layers
 * composite by ascending z, then creation order. Changing a layer only records damage; nothing is
 * drawn before shr_submit(). Automatic frames (blink, shr_request_redraw(), fallback glyphs that
 * resolved) wait while such changes are not submitted. */

typedef struct shr_lyr shr_lyr;

shr_status shr_lyr_create(shr_context *ctx, int32_t z, shr_rect rect, shr_lyr **out);
shr_status shr_lyr_set_rect(shr_lyr *layer, shr_rect rect);
shr_status shr_lyr_set_z(shr_lyr *layer, int32_t z);
shr_status shr_lyr_set_visible(shr_lyr *layer, bool visible);
shr_status shr_lyr_destroy(shr_lyr *layer);

/* Application layers are rewritten whole: begin, add commands, commit. The previous list stays
 * visible until commit; the compositor derives the damage from the difference. SHR_E_STATE on a
 * layer a plugin (tilemap) owns. */
shr_status shr_lyr_cmd_begin(shr_lyr *layer);
shr_status shr_lyr_cmd_fill(shr_lyr *layer, shr_rect rect, shr_color color);

typedef struct shr_pl_res_image shr_pl_res_image;

shr_status shr_lyr_cmd_image(shr_lyr *layer, shr_pl_res_image *image, shr_rect src, shr_point at);
shr_status shr_lyr_cmd_commit(shr_lyr *layer);

/* ===== Image resource =====
 * Pixels are copied into a buffer the driver draws from (RGBA8888, straight alpha) within desc.image_bytes;
 * SHR_E_UNSUPPORTED beyond the driver's buffer limits. */

shr_status shr_pl_res_image_create(shr_context *ctx, int32_t width, int32_t height, const void *rgba,
                                   size_t stride, shr_pl_res_image **out);
/* While a frame reads the image, the update goes to a second buffer that later frames draw from;
 * SHR_E_LIMIT when desc.image_bytes has no room for it. */
shr_status shr_pl_res_image_update(shr_pl_res_image *image, shr_rect rect, const void *rgba, size_t stride);
/* Freed once no layer command refers to it and no frame reads it. */
shr_status shr_pl_res_image_release(shr_pl_res_image *image);

/* ===== Bitmap font resource =====
 * Packages are baked for SHR_CELL_WIDTH x SHR_CELL_HEIGHT and opened on demand. */

/* Bounded byte-range reads of one package. With `data` the package is mapped in memory for the
 * resource's lifetime and read() is not used. read(): SHR_OK = `dst` is filled before returning;
 * SHR_IN_PROGRESS = finish later with shr_asset_complete(); SHR_E_WOULD_BLOCK = queue full, retried
 * io_retry_ns later or after shr_asset_ready().
 * The callbacks run as host callbacks. */
typedef struct shr_asset_source {
    void *user;
    const void *data;
    uint64_t size;
    shr_status (*read)(void *user, uint64_t offset, uint32_t length, void *dst, uint64_t request);
    void (*cancel)(void *user, uint64_t request); /* after it returns, `dst` is no longer written */
    void (*close)(void *user);                    /* after the last read has ended */
} shr_asset_source;

shr_status shr_asset_source_init(shr_asset_source *source);
/* Thread-safe; wakes nothing by itself, the application calls shr_pump(). */
shr_status shr_asset_complete(shr_context *ctx, uint64_t request, shr_status result);
/* A source can take reads again (after SHR_E_WOULD_BLOCK) or has recovered (a package or page that failed
 * and cooled down may be tried again). Thread-safe; wakes nothing by itself, like shr_asset_complete(). */
shr_status shr_asset_ready(shr_context *ctx);

typedef struct shr_pl_res_bitmap_font shr_pl_res_bitmap_font;

typedef struct shr_pl_res_bitmap_font_desc {
    void *user;
    /* Opens a baked package by file name (e.g. "shiroko-latin.shrf") from shr_pump() once a glyph needs it.
     * SHR_E_NOT_FOUND: the package is not installed and its glyphs fall back for the font's lifetime (to use
     * a package installed later, create a new font and pass it to the tilemaps); other errors are retried like
     * reads, and while the package cools down its glyphs are drawn with the provisional fallback. Runs as a
     * host callback. */
    shr_status (*open)(void *user, const char *package, shr_asset_source *out);
    const char *locale;       /* CJK glyph variant: "ko", "ja", "zh-Hans", "zh-Hant", "zh-HK" */
} shr_pl_res_bitmap_font_desc;

shr_status shr_pl_res_bitmap_font_desc_init(shr_pl_res_bitmap_font_desc *desc);
shr_status shr_pl_res_bitmap_font_create(shr_context *ctx, const shr_pl_res_bitmap_font_desc *desc,
                                         shr_pl_res_bitmap_font **out);
/* SHR_E_STATE while a tilemap uses the font. */
shr_status shr_pl_res_bitmap_font_destroy(shr_pl_res_bitmap_font *font);
/* Loads the first `pages` pages of `package` (a name open() receives) from shr_pump(), opening it if needed: one page
 * per pump while no frame waits for pages, until the page cache would have to evict. fontpack places the most used
 * glyphs first and reports how many pages hold them ("hot"). Pages whose read cannot start are left to the frames.
 * SHR_E_NOT_FOUND: not a package of this font; SHR_E_STATE: the font is destroyed or the context shutting down, or
 * called from one of its callbacks. */
shr_status shr_pl_res_bitmap_font_preload(shr_pl_res_bitmap_font *font, const char *package, uint32_t pages);

typedef struct shr_activation {
    uint64_t generation;
    uint8_t package_id[32];
    uint64_t package_size;
} shr_activation;

/* Picks the valid A/B record with the higher generation (64-byte records). */
shr_status shr_pl_res_bitmap_font_activation_select(const uint8_t *a, size_t a_length, const uint8_t *b,
                                                    size_t b_length, shr_activation *out);

/* ===== Tilemap layer =====
 * A grid of SHR_CELL_WIDTH x SHR_CELL_HEIGHT cells in a layer, updated cell by cell. */

enum {
    SHR_STYLE_BOLD = 1u << 0,
    SHR_STYLE_ITALIC = 1u << 1,
    SHR_STYLE_BLINK = 1u << 2
};
#define SHR_STYLE_KNOWN_FLAGS 0x7u

/* FG/BG are resolved colours: no inverse flag, no terminal colour modes. Their alpha byte says how they draw: fg 255
 * full, 128 dim (half strength), 0 concealed (no glyph); bg 255 painted under the occupied cells, 0 none (the tilemap
 * background or transparent). Other alpha values are SHR_E_UNKNOWN_STYLE for now. A colour without alpha (0xRRGGBB)
 * is concealed or unpainted: build colours with SHR_RGB. */
typedef struct shr_text_style {
    shr_color fg;
    shr_color bg;
    uint32_t flags;
} shr_text_style;

/* A line drawn over cells [col, col + cols) of a row, above the glyphs: under at the font's underline, strike at its
 * strikeout, over at the cell top; `shape` one of SHR_LINE_* (shiroko_driver.h). `color` alpha as fg's: 255 full, 128
 * dim, 0 not drawn. */
enum { SHR_LINE_UNDER = 0, SHR_LINE_STRIKE, SHR_LINE_OVER };
enum { SHR_TEXT_LINE_BLINK = 1u << 0 }; /* hidden while the blink phase is off, as SHR_STYLE_BLINK */
typedef struct shr_text_line {
    uint16_t col, cols;
    uint8_t kind, shape;
    uint16_t flags;
    shr_color color;
} shr_text_line;

/* Byte range [byte_start, byte_end) on cluster boundaries; non-empty, sorted, disjoint. */
typedef struct shr_style_run {
    size_t byte_start;
    size_t byte_end;
    shr_text_style style;
} shr_style_run;

enum { SHR_TEXT_WRAP = 1u << 0 };

typedef struct shr_text_profile_info {
    uint8_t text_profile_id[32];
    const char *unicode_version;
    uint32_t rules_version;
    uint32_t tab_stop;
    uint32_t cluster_max_scalars;
    uint32_t cluster_max_bytes;
} shr_text_profile_info;

shr_status shr_pl_lyr_tilemap_profile_get(shr_text_profile_info *out);

typedef struct shr_text_limits {
    size_t max_text_bytes;
    uint32_t max_cell_bytes;   /* one cluster: the profile's limits; longer clusters draw U+FFFD (SHR_CLUSTER_REPLACEMENT) */
    uint32_t max_cell_scalars;
    uint32_t max_span;
    int32_t max_rows;
    int32_t max_cols;
} shr_text_limits;

shr_status shr_pl_lyr_tilemap_limits_get(shr_text_limits *out);

/* Attaches a tilemap to a layer on first use (the layer then rejects shr_lyr_cmd_*) and sets its size;
 * cells keep their content where the grids overlap. A different font replaces the previous one.
 * `background` (NULL: none) paints every cell without a bg of its own, which makes the rows opaque;
 * without it such cells are transparent. The tilemap is freed with the layer. */
shr_status shr_pl_lyr_tilemap_resize(shr_lyr *layer, shr_pl_res_bitmap_font *font, int32_t rows, int32_t cols,
                                     const shr_color *background);
/* One cluster as the VT engine placed it: never re-segmented, re-measured or combined. It occupies
 * `span` cells from (row, col); cells it overlaps are cleared. Empty UTF-8 draws the background only, and the row's
 * lines stay as they are. A cluster past the profile's limits (shr_text_limits) draws U+FFFD; past max_cell_bytes the
 * cell keeps only U+FFFD. */
shr_status shr_pl_lyr_tilemap_set_cell(shr_lyr *layer, int32_t row, int32_t col, const char *utf8, size_t length,
                                       uint32_t span, shr_text_style style);
/* Replaces the lines of `row` with `lines` (drawn in this order); an equal list changes nothing. Each lies inside the
 * grid with cols >= 1; at most 4 per column of the grid (SHR_E_LIMIT). SHR_E_UNKNOWN_STYLE: an unknown kind, shape or
 * flag or a colour alpha other than 0, 128 and 255 (err->item_index: the line). On error nothing changes. */
shr_status shr_pl_lyr_tilemap_set_lines(shr_lyr *layer, int32_t row, const shr_text_line *lines, size_t count,
                                        shr_error_info *err);

/* A cluster of a row as the VT engine placed it, by code point. */
typedef struct shr_row_cell {
    uint32_t text;   /* scalars 1: the code point; more: the index of its first code point in shr_row.scalars */
    uint16_t style;  /* index into shr_row.styles */
    uint8_t span;    /* cells it occupies, at least 1 */
    uint8_t scalars; /* code points of the cluster (a longer one may pass its first 255); 0: no text */
} shr_row_cell;

typedef struct shr_row {
    const shr_row_cell *cells; /* placed left to right, each `span` cells after the one before */
    size_t cell_count;
    const shr_text_style *styles;
    size_t style_count;
    const uint32_t *scalars; /* the code points of clusters of more than one */
    size_t scalar_count;
    const shr_text_line *lines; /* NULL: the row keeps its lines; else they replace them as set_lines() does */
    size_t line_count;
} shr_row;

/* As shr_pl_lyr_tilemap_set_cell() for each cell in turn from (row, col), then set_lines() when `lines` is set; cells
 * that equal what the grid holds are left as they are. The whole row is validated first: on error nothing changes and
 * err->item_index is the cell's index (the style's for a bad style, the line's for a bad line). */
shr_status shr_pl_lyr_tilemap_set_row(shr_lyr *layer, int32_t row, int32_t col, const shr_row *in, shr_error_info *err);
/* Text laid out by the renderer from (row, col) as shr_pl_lyr_tilemap_measure() lays it out in
 * `cols - col` columns: Unicode clusters and widths, LF/CR/CRLF, TAB stops counted from `col` and,
 * with SHR_TEXT_WRAP, wrapping at the last column; lines continue at column `col`. Only what lies in
 * the grid is placed: without wrap a cluster cut by the last column becomes blank cells of its style,
 * later ones are dropped; rows past the grid are dropped. Zero-width clusters occupy no cell; clusters past the
 * profile's limits take the cells of their first max_cell_scalars scalars and draw U+FFFD as in set_cell. The whole
 * text is validated; on error nothing changes. `runs` (NULL: `style` everywhere) restyle byte ranges.
 * The tilemap keeps the memory the cells wait in until the text is valid: 40 bytes per cell of the largest call so
 * far (cells at most the grid's, rounded up to a power of two), so later calls placing no more cells allocate only
 * for clusters over 12 bytes. */
shr_status shr_pl_lyr_tilemap_set_text(shr_lyr *layer, int32_t row, int32_t col, const char *utf8, size_t length,
                                       shr_text_style style, const shr_style_run *runs, size_t run_count,
                                       uint32_t flags, shr_error_info *err);
/* Clears rows x cols cells from (row, col), and the wide cells reaching into them, to style.bg (alpha 0: blank, the
 * tilemap background or transparent), and the lines over them. Other style fields are ignored. SHR_E_NO_MEMORY: a
 * line the cleared columns split in two found no room; nothing changed. */
shr_status shr_pl_lyr_tilemap_clear(shr_lyr *layer, int32_t row, int32_t col, int32_t rows, int32_t cols,
                                    shr_text_style style);

/* Moves the cells of rows [top, bottom) by n rows: up for n > 0 (rows top .. top + n - 1 leave), down for n < 0. The
 * rows it uncovers become cells cleared to `style` as shr_pl_lyr_tilemap_clear() clears them; |n| >= bottom - top
 * clears all of them. Moved rows keep their text, styles and lines, and the renderer moves their pixels instead of
 * drawing them again where it can. */
shr_status shr_pl_lyr_tilemap_scroll(shr_lyr *layer, int32_t top, int32_t bottom, int32_t n, shr_text_style style);

enum {
    SHR_CLUSTER_NEWLINE = 1u << 0,
    SHR_CLUSTER_TAB = 1u << 1,
    SHR_CLUSTER_TAB_CONT = 1u << 2,    /* TAB spaces continued after a wrap */
    SHR_CLUSTER_INVISIBLE = 1u << 3,
    SHR_CLUSTER_REPLACEMENT = 1u << 4,
    SHR_CLUSTER_EMOJI = 1u << 5,
    SHR_CLUSTER_WRAPPED = 1u << 6      /* moved to a new line by wrap */
};

typedef struct shr_text_cluster {
    size_t byte_offset;
    size_t byte_length;
    int32_t row;
    int32_t column;
    int32_t cells;
    uint32_t flags;
} shr_text_cluster;

typedef struct shr_text_extent {
    size_t clusters;
    int32_t rows;
    int32_t cols; /* widest row */
} shr_text_extent;

/* The layout set_text would produce at column 0 of a `cols`-wide grid with unlimited rows, one entry
 * per cluster in text order (TABs split by a wrap give one entry per row). Without SHR_TEXT_WRAP
 * clusters past `cols` keep their columns. Stores the first `capacity` clusters in `out` (NULL when 0)
 * and the complete `extent` (zero on other errors); SHR_E_LIMIT when capacity < extent->clusters, or
 * when this unbounded layout passes the row or column limit. set_text lays out only what reaches its
 * grid, so it accepts such text. */
shr_status shr_pl_lyr_tilemap_measure(const char *utf8, size_t length, int32_t cols, uint32_t flags,
                                      shr_text_cluster *out, size_t capacity, shr_text_extent *extent,
                                      shr_error_info *err);

#ifdef __cplusplus
}
#endif

#endif
