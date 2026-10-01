#ifndef SHIROKO_COMPOSITOR_H
#define SHIROKO_COMPOSITOR_H

#include <stdatomic.h>

#include "shr_compositor.h"

#define SHR_TARGETS 4
#define SHR_MAX_DAMAGE 64
#define SHR_DISPLAY_HISTORY 8
#define SHR_PLUGIN_SLOTS 8

typedef struct shr__damage {
    shr__vec rects; /* shr_rect, screen coordinates */
    bool full;
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
    shr_draw_cmd convert; /* the ROTATE or COPY into the output once `built` */
    uint64_t fence, deadline, started;
    int64_t damaged_pixels;
} shr__frame;

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

typedef struct shr__group {
    uint32_t id;
    shr__lcmd *cmds; /* followed by `blocks` in the same allocation */
    shr_rect *blocks; /* bounds of each SHR__BLOCK commands */
    size_t n;
    shr_rect bounds, blink; /* layer coordinates */
    shr_rect opaque;        /* a part its commands surely cover with opaque pixels */
} shr__group;

struct shr_lyr {
    shr_context *ctx;
    shr_lyr *next; /* ascending z, then creation order */
    int32_t z;
    uint64_t seq;
    shr_rect rect;
    bool visible, building;
    shr__vec groups;  /* shr__group, ascending id */
    shr__vec pending; /* shr__lcmd between shr_lyr_cmd_begin and commit */
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
    shr_blink_profile blink;
    bool shutting_down, in_callback;

    const void *plugin_kind[SHR_PLUGIN_SLOTS];
    void *plugin_state[SHR_PLUGIN_SLOTS];
    shr__res *resources;
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
    shr__target targets[SHR_TARGETS]; /* 0 is the composition surface while composing */
    shr__damage staged;               /* layer changes not yet submitted */

    bool submitted, has_submitted;
    bool stale;  /* the output may not show the current state */
    bool failed; /* the last frame failed: no automatic frames until something changes */
    bool blink_shown, last_provisional;
    bool output_blocked, output_isolated;
    uint64_t next_frame_id;
    shr__frame frame;

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

static inline shr_rect shr__rect_move(shr_rect r, int32_t dx, int32_t dy) {
    return (shr_rect){shr__clamp32((int64_t)r.x0 + dx), shr__clamp32((int64_t)r.y0 + dy),
                      shr__clamp32((int64_t)r.x1 + dx), shr__clamp32((int64_t)r.y1 + dy)};
}

void shr__push_event(shr_context *ctx, const shr_event *ev);
bool shr__blink_visible(const shr_context *ctx, uint64_t *next_boundary);
void shr__damage_add(const shr_context *ctx, shr__damage *d, shr_rect r);
void shr__damage_targets(shr_context *ctx, shr_rect r);
void shr__targets_invalidate(shr_context *ctx);
void shr__res_collect(shr_context *ctx);

bool shr__io_pump(shr_context *ctx);
bool shr__io_busy(const shr_context *ctx, const shr__res *res); /* res NULL: any */
bool shr__io_ready(const shr_context *ctx);
uint64_t shr__io_deadline(const shr_context *ctx);
void shr__io_cancel_all(shr_context *ctx);

bool shr__blink_any(const shr_context *ctx);
void shr__blink_damage(shr_context *ctx);

/* Ends the frame as superseded (the driver must not run it: not running, not isolated) and, shutting down,
 * a pending submission once the queue has room. */
void shr__frame_abandon(shr_context *ctx);
/* The driver can draw into and read from `s` (domain, alignment, size). */
bool shr__driver_reaches(const shr_driver_caps *caps, const shr_surface *s);

#endif
