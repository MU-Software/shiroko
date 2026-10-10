#ifndef SHIROKO_COMPOSITOR_H
#define SHIROKO_COMPOSITOR_H

#include <stdatomic.h>

#include "shr_compositor.h"

#define SHR_TARGETS 4
#define SHR_MAX_DAMAGE 64
#define SHR_DISPLAY_HISTORY 8
#define SHR_PLUGIN_SLOTS 8
#define SHR_MAX_MOVES 8
/* Keep stores a frame past the first 8, in bytes: about 10 ms of a 33 ms frame on the ESP32-P4, whose software driver
 * spends 1.2 ms more on a 40 KiB row stored than drawn, so 8 rows of 1280 RGB565 pixels by 16 there. */
#define SHR__KEEP_STORE_BYTES (320u << 10)

/* The pixels of `area` (screen coordinates) moved down by dy; what moves out of it is lost, what it uncovers keeps
 * the old pixels. */
typedef struct shr__move {
    shr_rect area;
    int32_t dy;
} shr__move;

/* Moves happen first, in order; `rects` are in the coordinates after them. */
typedef struct shr__damage {
    shr__vec rects; /* shr_rect, screen coordinates */
    bool full;
    uint32_t nmoves;
    shr__move moves[SHR_MAX_MOVES];
} shr__damage;

/* A preserved surface: what it shows is the current state except inside `damage`. */
typedef struct shr__target {
    bool used;
    void *pixels;
    uint64_t resource_id;
    uint32_t generation;
    uint64_t last_use;
    shr__damage damage;
    shr__vec provisional; /* shr_rect drawn with fallback pixels */
} shr__target;

/* Keep groups of a frame: drawn from a keep, drawn plainly, stored; stores replaced or released before a later frame
 * drew them (dead), and stores no keep could make room for (refused). */
typedef struct shr__keep_count {
    uint32_t hits, direct, stores, dead, refused;
} shr__keep_count;

/* A group drawn plainly: a 32-bit print of its key and the frame. Each key has two places. */
typedef struct shr__seen {
    uint32_t print, frame;
} shr__seen;

/* ISOLATED: failed after a watchdog timeout the driver could not reset; its buffers and resolved pixels
 * stay held until the fence resolves. */
typedef enum frame_state { FRAME_IDLE = 0, FRAME_RASTER, FRAME_CONVERT, FRAME_PRESENT, FRAME_ISOLATED } frame_state;

typedef struct shr__frame {
    frame_state state;
    uint64_t frame_id;
    bool has_output, built, running;
    bool refused;        /* execute() returned SHR_E_WOULD_BLOCK: retried at `deadline` or shr_driver_ready() */
    uint32_t ready_seen; /* driver_ready before that execute() */
    bool took_damage; /* raster not finished: the damage goes back to the target if it fails */
    bool prov_stale;  /* a resource changed while the frame was built */
    bool blink_visible;
    shr_surface output, target;
    int target_rec; /* index in targets, -1 = not preserved */
    shr__vec cmds, damage, provisional, resolved; /* shr_draw_cmd, shr_rect, shr_rect, shr__res * */
    /* The buffer plan: commands put ahead of `cmds`, and the buffers drawn from with their ids before it. The
     * registry takes the plan once the driver accepted the raster. */
    shr__vec prologue, planned; /* shr_draw_cmd, shr__planned */
    uint64_t resident;          /* driver-held bytes once the prologue ran */
    shr__vec kept;              /* shr__kept: stores of the plan, in batch order */
    uint64_t kept_bytes;        /* their bytes */
    size_t kept_at;             /* the first one not committed */
    uint64_t keep_resident;     /* bytes held keeps take once the frame ran */
    uint32_t keep_scan;         /* free keep ids are searched from here */
    shr__keep_count keep_count; /* the frame's so far: its plan's, and what its accepted batches stored and dropped */
    bool no_credit;             /* the first frame submitted after a pump that had nothing to do */
    shr_draw_cmd convert; /* the ROTATE or COPY into the output once `built` */
    /* The batches `cmds` holds, built a step at a time: the frame, or with bands the moves and then one band each. */
    shr__vec batches;     /* shr__batch */
    size_t batch;         /* the next one to run */
    size_t prologue_at;   /* prologue commands of those steps */
    size_t pushed;        /* commands of the frame, the prologue aside */
    int32_t band_y, oy;   /* with bands: the first row of the next band, of the band being built */
    uint32_t band_k;      /* the band buffer of the next band */
    uint64_t converted;   /* with bands: bytes the conversions read and write */
    uint8_t synced;       /* destinations synced in this frame: 1 << band, 4 = `target` */
    uint64_t fence, deadline, started;
    int64_t damaged_pixels;
    uint32_t nmoves; /* COPYs of the target onto itself ahead of the damage */
    shr__move moves[SHR_MAX_MOVES];
} shr__frame;

/* Commands [at, at + n) of `cmds` into band `band`, or into the output when -1. */
typedef struct shr__batch {
    size_t at, n;
    int band;
} shr__batch;

typedef struct shr__planned {
    shr__buf *buf;
    uint32_t old_id;
} shr__planned;

/* What a keep holds: the content hash of a group and its size. */
typedef struct shr__keep_key {
    uint64_t hash[2];
    int32_t w, h;
} shr__keep_key;

/* A store the plan holds for keep `id`, replacing what the id holds when `replaces`; `borrowed`: on credit. */
typedef struct shr__kept {
    uint32_t id;
    shr__keep_key key;
    uint64_t bytes;
    bool replaces, borrowed;
} shr__kept;

typedef enum keep_plan { KEEP_PINNED = 1, KEEP_FREED, KEEP_STORED } keep_plan;

/* Keep id k is keeps[k - 1]: what the driver holds once the accepted batches ran (bytes 0 = nothing), chained by key
 * from keep_heads. While `stamp` is the building frame, `plan` says what that frame does with the id. */
typedef struct shr__keep {
    shr__keep_key key;
    uint64_t bytes;
    uint32_t next;
    shr__lru_node lru; /* held keeps */
    uint64_t stamp;
    keep_plan plan;
    bool releasing; /* the driver may hold anything: a RELEASE is due */
    bool drawn;     /* drawn by a frame after the one that stored it */
    bool borrowed;  /* stored on credit */
} shr__keep;

/* Buffer id k is slots[k - 1]. A plan marks an id with `stamp` = its frame: `taken` by a buffer or freed. */
typedef struct shr__slot {
    shr__buf *buf;   /* registered under the id */
    uint64_t bytes;  /* what the driver holds under the id */
    uint64_t stamp;
    bool taken;
    bool known;      /* the driver holds `buf`: false once a batch failed or timed out */
    bool releasing;  /* `buf` was freed: a RELEASE is due */
} shr__slot;

typedef struct shr__io {
    _Atomic uint64_t state; /* generation << 8 | shr_status << 2 | IO_* */
    shr__res *res;
    const shr_asset_source *key;
    shr_asset_source src;
    uint64_t tag;
    uint32_t length;
    uint64_t deadline;
} shr__io;

#define SHR__BLOCK 64
#define GROUP_CLASSES 3

typedef struct shr__group {
    uint32_t id;
    bool compact; /* a row group */
    union {
        shr__lcmd *cmds; /* room for `cap`, followed by `blocks` in the same allocation */
        shr__rcmd *rows; /* the same, compact */
    };
    shr_rect *blocks; /* bounds of each SHR__BLOCK commands */
    size_t n, cap;
    int32_t oy;             /* group coordinates are layer coordinates moved up by oy */
    shr_rect bounds, blink; /* group coordinates, like its commands */
    shr_rect opaque;        /* a part its commands surely cover with opaque pixels */
    shr__res *res;          /* the one resource of its GLYPH/IMAGE commands, NULL: none or several; a row's resource */
    uint32_t uses;          /* the number of its GLYPH/IMAGE commands */
    uint64_t key[2];        /* a row's CACHE_BEGIN key */
} shr__group;

static inline shr_rect shr__rcmd_dst(const shr__rcmd *r) {
    return (shr_rect){r->x0 * SHR_CELL_WIDTH, r->y0, r->x1 * SHR_CELL_WIDTH, r->y1};
}

/* Command i of `g` in the full form. */
static inline shr__lcmd shr__group_cmd(const shr__group *g, size_t i) {
    if (!g->compact) return g->cmds[i];
    const shr__rcmd *r = &g->rows[i];
    shr__lcmd c = {.kind = r->kind, .flags = r->flags, .color = r->color, .dst = shr__rcmd_dst(r)};
    if (r->kind == SHR__LCMD_CACHE_BEGIN)
        c.key[0] = g->key[0], c.key[1] = g->key[1], c.end = g->n - 1 - i;
    else
        c.anchor = (shr_point){c.dst.x0, c.dst.y0}, c.bg = r->bg, c.id = r->id, c.res = g->res;
    return c;
}

struct shr_lyr {
    shr_context *ctx;
    shr_lyr *next; /* ascending z, then creation order */
    int32_t z;
    uint64_t seq;
    shr_rect rect;
    bool visible, building;
    shr__vec groups;  /* shr__group, ascending id */
    shr__vec pending; /* shr__lcmd between shr_lyr_cmd_begin and commit */
    /* Group memory in GROUP_CLASSES sizes, class_cap commands each (ascending, the last the most a group asked for):
     * `spare` lent by begin for spare_cap commands, idle memory in `spares` per class, linked through its first bytes.
     * held: bytes of all of it; peak: the most groups so far. */
    void *spare, *spares[GROUP_CLASSES];
    size_t unit; /* bytes per command of that memory */
    size_t spare_cap, class_cap[GROUP_CLASSES], held, peak;
    const void *kind;
    void *state;
    void (*destroy)(void *state);
    shr_status (*flush)(void *state);
};

struct shr_context {
    shr__alloc al;
    bool dma_alloc; /* the application's allocator serves SHR_ALLOC_DMA */
    shr_context_desc desc;
    shr_framebuffer_driver driver;
    shr_output output;
    bool shutting_down, in_callback;

    const void *plugin_kind[SHR_PLUGIN_SLOTS];
    void *plugin_state[SHR_PLUGIN_SLOTS];
    shr__res *resources;
    uint64_t res_serial; /* the last shr__res serial */
    shr_lyr *layers;
    uint64_t next_layer_seq;

    shr__io *io;
    uint32_t nio;
    uint64_t io_gen;

    shr_event *events;
    uint32_t event_cap, event_head, event_count;
    bool event_overflow;
    uint32_t overflow_after; /* queued events that precede the dropped ones */
    uint64_t undisplayed[SHR_DISPLAY_HISTORY];
    uint64_t *unreleased;
    uint32_t unreleased_count;

    bool configured, composing, composition_owned;
    shr_screen_desc screen;
    shr_surface composition;
    shr_surface bands[2];
    uint32_t band_count;
    shr__target targets[SHR_TARGETS]; /* 0 is the composition surface while composing */
    shr__damage staged;               /* layer changes not yet submitted */

    bool submitted, has_submitted;
    bool stale;  /* the output may not show the current state */
    bool failed; /* the last frame failed: no automatic frames until something changes */
    bool blink_shown, last_provisional;
    bool output_blocked, output_isolated;
    uint64_t next_frame_id;
    uint64_t next_start_ns; /* earliest start of the next frame under min_frame_interval_ns */
    shr__frame frame;

    shr__slot *slots;     /* caps.max_buffers */
    uint32_t *released;   /* ids with `releasing` set, at most once each */
    uint32_t nreleased;
    uint64_t resident;    /* sum of slot bytes */
    shr__lru_node *lru;   /* registered buffers */

    shr__keep *keeps;          /* caps.max_keeps */
    uint32_t *keep_heads;      /* keep_mask + 1 chains */
    uint32_t keep_mask;
    uint32_t *keep_released;   /* ids with `releasing` set, at most once each */
    uint32_t nkeep_released;
    uint64_t keep_resident;    /* sum of keep bytes */
    uint64_t keep_budget;      /* caps.keep_bytes (UINT64_MAX when 0), lowered for good after SHR_E_NO_MEMORY */
    uint64_t keep_store_bytes; /* stored per frame at most, past the first stores: SHR__KEEP_STORE_BYTES */
    uint32_t keep_credit;      /* stores on credit (SHR_DRIVER_CHEAP_STORE), at most caps.max_keeps */
    uint32_t keep_run;         /* frames in a row, up to 2, drawing or storing more keep groups than drawn from keeps */
    bool keep_still;           /* a pump had nothing to do after a frame: no credit stores in the next submitted one */
    shr__lru_node *keep_lru;
    shr__seen *seen;           /* seen_mask + 1 */
    size_t seen_mask;
    shr__keep_count keep_count; /* of the last frame that ran */

    _Atomic uint64_t fence_state; /* fence << 2 | shr_fence_state; 0 = nothing pending */
    _Atomic uint32_t driver_ready, asset_ready; /* bumped by shr_driver_ready() and shr_asset_ready() */
    uint64_t fence_gen;
};

/* Queue slots promised to events that are never dropped: one per unreleased frame, two for the
 * frame in flight, one for OUTPUT_ISOLATED. Invariant: event_count + reserved <= event_cap. */
static inline uint32_t shr__events_reserved(const shr_context *ctx) {
    frame_state s = ctx->frame.state;
    return ctx->unreleased_count + 2u * ((s != FRAME_IDLE) & (s != FRAME_ISOLATED)) + !ctx->output_isolated;
}


static inline int32_t shr__clamp32(int64_t v) { return v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : (int32_t)v; }

static inline shr_image_ref shr__image_ref(const void *pixels, int32_t w, int32_t h, size_t stride, shr_pixel_format f,
                                           shr_memory_domain dom) {
    return (shr_image_ref){pixels, w, h, (uint32_t)stride, (uint8_t)f, (uint8_t)dom, 0};
}

/* Clamped to int32; the 32-bit sums first, as they nearly always fit. */
static inline shr_rect shr__rect_move(shr_rect r, int32_t dx, int32_t dy) {
    shr_rect m;
    bool over = __builtin_add_overflow(r.x0, dx, &m.x0);
    over |= __builtin_add_overflow(r.y0, dy, &m.y0);
    over |= __builtin_add_overflow(r.x1, dx, &m.x1);
    over |= __builtin_add_overflow(r.y1, dy, &m.y1);
    if (!over) return m;
    return (shr_rect){shr__clamp32((int64_t)r.x0 + dx), shr__clamp32((int64_t)r.y0 + dy),
                      shr__clamp32((int64_t)r.x1 + dx), shr__clamp32((int64_t)r.y1 + dy)};
}

/* Where the group coordinates of `g` start on the screen. */
static inline shr_point shr__group_at(const shr_lyr *l, const shr__group *g) {
    return (shr_point){l->rect.x0, shr__clamp32((int64_t)l->rect.y0 + g->oy)};
}

void shr__push_event(shr_context *ctx, const shr_event *ev);
bool shr__blink_visible(const shr_context *ctx, uint64_t *next_boundary);
void shr__damage_add(const shr_context *ctx, shr__damage *d, shr_rect r);
/* After the rects, a move of `m`: damage inside its area moves along. Same-area moves of one direction merge; beyond
 * SHR_MAX_MOVES the moves give way to damage of their areas. */
void shr__damage_move(const shr_context *ctx, shr__damage *d, shr__move m);
/* The moves of `d` become damage of their areas: nothing outside them changed. */
void shr__moves_drop(const shr_context *ctx, shr__damage *d);
void shr__damage_targets(shr_context *ctx, shr_rect r);
void shr__targets_invalidate(shr_context *ctx);

bool shr__io_pump(shr_context *ctx);
bool shr__io_busy(const shr_context *ctx, const shr__res *res); /* res NULL: any */
bool shr__io_ready(const shr_context *ctx);
uint64_t shr__io_deadline(const shr_context *ctx);
void shr__io_cancel_all(shr_context *ctx);

/* Whether the opaque parts of `l`'s groups, in order, cover `r` (screen coordinates) row by row. May miss a cover,
 * never claims a false one. */
bool shr__hides(const shr_lyr *l, shr_rect r);
/* Group `g` moved on screen: the keeps of its keep group (its first command) stay recent. */
void shr__keep_moved(shr_context *ctx, const shr__group *g);
bool shr__blink_any(const shr_context *ctx);
void shr__blink_damage(shr_context *ctx);

/* Ends the frame as superseded (the driver must not run it: not running, not isolated) and, shutting down,
 * a pending submission once the queue has room. */
void shr__frame_abandon(shr_context *ctx);
/* The driver can draw into and read from `s` (domain, alignment, size). */
bool shr__driver_reaches(const shr_driver_caps *caps, const shr_surface *s);
/* The driver reaches memory in `domain` at `pixels` with `stride` (sizes aside). */
bool shr__mem_reaches(const shr_driver_caps *caps, shr_memory_domain domain, const void *pixels, size_t stride);

#endif
