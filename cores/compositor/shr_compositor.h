#ifndef SHIROKO_SHR_COMPOSITOR_H
#define SHIROKO_SHR_COMPOSITOR_H

#include <shiroko/shiroko.h>

#include "shr_alloc.h"
#include "shr_err.h"
#include "shr_lru.h"
#include "shr_rect.h"
#include "shr_vec.h"

/* ===== Context services for plugins ===== */

const shr__alloc *shr__ctx_alloc(const shr_context *ctx);
/* The descriptor given to shr_create() (budgets, retry policy); pointers in it are not kept. */
const shr_context_desc *shr__ctx_desc(const shr_context *ctx);
uint32_t shr__ctx_driver_flags(const shr_context *ctx); /* the caps flags shr_create() read */
uint64_t shr__ctx_now(const shr_context *ctx); /* 0 without a clock */
uint64_t shr__sat_add(uint64_t a, uint64_t b); /* deadlines: UINT64_MAX on overflow */
/* Bumped by shr_asset_ready(): read it before a read or open, retry what a source refused once it changed. */
uint32_t shr__ctx_asset_ready(const shr_context *ctx);
/* Shutting down or inside a host callback: mutating plugin calls return SHR_E_STATE. */
bool shr__ctx_refused(const shr_context *ctx);
bool shr__ctx_in_callback(const shr_context *ctx); /* releases stay allowed while shutting down */
void shr__ctx_log(shr_context *ctx, shr_status status, const char *message);
void shr__ctx_trace(shr_context *ctx, shr_trace_kind kind, uint64_t id, uint64_t v0, uint64_t v1);
/* Queues SHR_EVENT_RESOURCE_FAILED (droppable). */
void shr__ctx_resource_failed(shr_context *ctx, shr_status status);
/* Per-context state of a plugin (NULL until set); `kind` is an address unique to the plugin.
 * The compositor keeps at most 8 kinds and never frees the state. */
void **shr__ctx_plugin_slot(shr_context *ctx, const void *kind);

/* Runs a host callback (application code); context calls from it fail with SHR_E_STATE. */
void shr__ctx_host_enter(shr_context *ctx, bool *saved);
void shr__ctx_host_leave(shr_context *ctx, bool saved);
#define SHR_HOST(ctx, call)                    \
    do {                                       \
        bool shr_saved_;                       \
        shr__ctx_host_enter((ctx), &shr_saved_); \
        call;                                  \
        shr__ctx_host_leave((ctx), shr_saved_); \
    } while (0)

/* ===== Buffers ===== */

/* Pixels plugins draw from, embedded in plugin state; the driver knows them by `id`. The plugin writes `mem`
 * pixels only while no frame resolved to the buffer is pinned. */
typedef struct shr__buf {
    shr_image mem;
    uint32_t id;        /* driver id, 0 = not registered; compositor-owned like the fields below */
    shr_rect dirty;     /* written since the driver last saw it */
    uint64_t used;      /* last frame that drew from it */
    bool owned;         /* `mem` came from shr__buf_alloc */
    shr__lru_node lru;
} shr__buf;

/* Memory the driver reaches (caps.domains, alignments); SHR_E_UNSUPPORTED beyond caps.max_buffer_width/height. */
shr_status shr__buf_alloc(shr_context *ctx, shr_pixel_format format, int32_t width, int32_t height, shr__buf *out);
/* Memory the caller keeps (a mapped or builtin page); SHR_E_UNSUPPORTED when the driver cannot reach it. */
shr_status shr__buf_wrap(shr_context *ctx, const shr_image *mem, shr__buf *out);
/* The plugin wrote `area` (buffer pixels); the driver sees it before the next frame draws from the buffer. */
void shr__buf_changed(shr__buf *buf, shr_rect area);
/* Frees owned memory and gives the id back; only while no frame resolved to the buffer is pinned. */
void shr__buf_free(shr_context *ctx, shr__buf *buf);

/* ===== Resources ===== */

typedef struct shr__res shr__res;

/* Pixels a GLYPH or IMAGE command draws: region `rect` of `buf`, placed relative to the command's anchor. */
typedef struct shr__resolved {
    shr__buf *buf;
    shr_rect rect;
    shr_point offset;  /* top-left of `rect` relative to shr__lcmd.anchor */
    bool provisional;  /* temporary fallback: redrawn after pump() reports a change */
    uint32_t synth;    /* SHR_GLYPH_BOLD / SHR_GLYPH_ITALIC the glyph may be drawn with */
    int32_t slant_axis; /* twice the rect y the italic shear turns about */
    int32_t scale_w, scale_h; /* IMAGE: `rect` drawn scaled to this size; 0: not scaled */
} shr__resolved;

typedef struct shr__res_ops {
    /* For a frame being built: the pixels of `id`, pinned until frame_end(frame), in *out until the next resolve().
     * SHR_E_NOT_FOUND = draw nothing; other errors fail the frame. */
    shr_status (*resolve)(shr__res *res, uint64_t id, uint64_t frame, const shr__resolved **out);
    /* Exactly once per frame in which resolve() succeeded, also for failed or superseded frames. */
    void (*frame_end)(shr__res *res, uint64_t frame);
    /* Called from shr_pump(). true: provisional pixels may now resolve differently. */
    bool (*pump)(shr__res *res);
    bool (*has_work)(const shr__res *res); /* true: pump() has something to do now */
    uint64_t (*deadline)(const shr__res *res); /* 0 = none */
    /* A read from shr__ctx_read() ended (SHR_E_TIMEOUT after io_timeout_ns, SHR_E_IO when cancelled). */
    void (*io_done)(shr__res *res, uint64_t tag, shr_status status);
    void (*shutdown)(shr__res *res);           /* begin_shutdown: stop new work */
    void (*free)(shr__res *res);               /* once `dead` is set, no read is outstanding and no command refers to it */
} shr__res_ops;

struct shr__res {
    const shr__res_ops *ops;
    shr_context *ctx;
    uint32_t users;   /* layer commands and plugins referring to it; maintained by the compositor and plugins */
    bool dead;        /* set by the plugin when it may be freed */
    uint64_t serial;  /* unique in its context (set by shr__res_attach): part of cache keys */
    shr__res *next;
};

shr_status shr__res_attach(shr_context *ctx, shr__res *res, const shr__res_ops *ops);
/* The pixels of `res` changed inside `area` (relative to the anchor of the commands referring to it).
 * Every such command counts as changed inside (anchor + area) clipped to its dst: recorded as damage
 * for every output buffer, drawn with the next frame. */
void shr__res_changed(shr__res *res, shr_rect area);

/* Asset reads, at most desc.max_reads outstanding per context. Completion (also synchronous ones)
 * reaches res->ops->io_done() from shr_pump(). SHR_E_LIMIT: no free slot; SHR_E_WOULD_BLOCK: the
 * source's queue is full. A source with `data` is never read through here. */
shr_status shr__ctx_read(shr_context *ctx, shr__res *res, const shr_asset_source *src, uint64_t offset,
                         uint32_t length, void *dst, uint64_t tag);
/* Cancels every outstanding read of `src`; their io_done() still arrives. true: some read of a source
 * without cancel() may still write (keep the buffers). */
bool shr__ctx_read_cancel(shr_context *ctx, const shr_asset_source *src);

/* ===== Layers ===== */

typedef enum shr__lcmd_kind {
    SHR__LCMD_FILL = 1,
    SHR__LCMD_GLYPH,
    SHR__LCMD_IMAGE,
    SHR__LCMD_LINE, /* row groups only */
    SHR__LCMD_CACHE_BEGIN,
    SHR__LCMD_CACHE_END
} shr__lcmd_kind;

enum {
    SHR__LCMD_DIM = SHR_GLYPH_DIM, /* GLYPH or FILL at half strength */
    SHR__LCMD_BOLD = SHR_GLYPH_BOLD, /* GLYPH styles, synthesized where the resource allows */
    SHR__LCMD_ITALIC = SHR_GLYPH_ITALIC,
    /* GLYPH: an earlier FILL of the group, without DIM or BLINK, of colour `bg` covers `dst`, and no command
     * between them draws into `dst` */
    SHR__LCMD_ON_FILL = SHR_GLYPH_ON_FILL,
    SHR__LCMD_BLINK = 1u << 7 /* hidden while the blink phase is off */
};

/* One retained layer command, in the coordinates of its group.
 * FILL: `dst` <- color. GLYPH/IMAGE: resolve(res, id) placed at anchor + offset, drawn only inside
 * `dst` (the cells or the image area). CACHE_BEGIN: `dst` is the cached area, `key` its content; the compositor sets
 * `end`, the distance to its CACHE_END, when it takes the list.
 * Every command sets kind, flags, dst and color; only GLYPH/IMAGE read the anchor fields, only CACHE_BEGIN `key`. */
typedef struct shr__lcmd {
    uint8_t kind;
    uint16_t flags;
    shr_rect dst;
    shr_color color;
    union {
        struct {
            shr_point anchor;
            shr_color bg; /* ON_FILL */
            uint32_t id; /* 32 bits for every resource */
            shr__res *res;
        };
        struct {
            uint64_t key[2];
            size_t end;
        };
    };
} shr__lcmd;

/* A row command: the compact form of a layer command that row groups keep. x in cells, y in pixels of the group.
 * GLYPH: `id` of the group's resource anchored at (x0, y0) of `dst`, `bg` with ON_FILL; LINE: `id` its SHR_LINE_* shape
 * over the pixel rows of `dst`, the pattern a cell wide from x0, bg 0; FILL and CACHE_END: id and bg 0;
 * CACHE_END: all 0 but the kind. A row's cache pair, if any, is its first and last command, its key the group's. */
typedef struct shr__rcmd {
    uint16_t x0, x1;
    uint8_t y0, y1;
    uint8_t kind, flags;
    shr_color color;
    uint32_t id;
    shr_color bg;
} shr__rcmd;

/* Replaces group `group` (groups draw in ascending order, commands in list order); n = 0 removes it. Its commands
 * are in layer coordinates. The compositor records the bounds of the old and new commands that differ as damage and
 * keeps res->users of referenced resources. */
shr_status shr__lyr_group_set(shr_lyr *layer, uint32_t group, const shr__lcmd *cmds, size_t n);
/* The same for a row group without a copy, the commands in group coordinates: layer coordinates moved up by `oy`, its
 * GLYPHs drawn from `res`, its CACHE_BEGIN with `key`. The caller writes all n commands into shr__lyr_row_begin(layer,
 * n) (NULL: no memory) and hands them to shr__lyr_row_commit() before the next begin. Group memory stays with the
 * layer until shr__lyr_groups_clear(): what replaced and removed groups leave serves the groups built next (in three
 * sizes up to the most commands asked for), so rebuilt groups allocate nothing. */
shr__rcmd *shr__lyr_row_begin(shr_lyr *layer, size_t n);
shr_status shr__lyr_row_commit(shr_lyr *layer, uint32_t group, int32_t oy, shr__res *res, const uint64_t key[2],
                               shr__rcmd *cmds, size_t n);
/* Removes all groups and their memory; the groups built next are expected to ask for up to `most` commands. */
shr_status shr__lyr_groups_clear(shr_lyr *layer, size_t most);
/* Groups [first, last) take ids id + shift and move down by dy pixels; those shifted out of [first, last) are removed.
 * They lie inside `area` (layer coordinates), whose pixels move along: where the driver moves pixels cheaply and the
 * layer covers the moved part with opaque groups, the compositor records a move instead of damage. */
void shr__lyr_groups_shift(shr_lyr *layer, uint32_t first, uint32_t last, int32_t shift, shr_rect area, int32_t dy);

/* Appends to an application layer's list between shr_lyr_cmd_begin() and shr_lyr_cmd_commit()
 * (shr_lyr_cmd_image is implemented by the image plugin through this). SHR_E_STATE outside. */
shr_status shr__lyr_cmd_add(shr_lyr *layer, const shr__lcmd *cmd);
shr_rect shr__lyr_rect(const shr_lyr *layer);

/* One plugin per layer. `kind` is any address unique to the plugin; `destroy` runs with the layer.
 * `flush` (optional) brings the layer's commands up to date at shr_submit(). On error it keeps what it
 * could not flush pending; shr_submit() still flushes the other layers and returns the first error. */
shr_status shr__lyr_attach(shr_lyr *layer, const void *kind, void *state, void (*destroy)(void *state),
                           shr_status (*flush)(void *state));
void *shr__lyr_state(const shr_lyr *layer, const void *kind); /* NULL: not attached to `kind` */
shr_context *shr__lyr_ctx(const shr_lyr *layer);

#endif
