#include "compositor.h"

const shr__alloc *shr__ctx_alloc(const shr_context *ctx) { return &ctx->al; }
const shr_context_desc *shr__ctx_desc(const shr_context *ctx) { return &ctx->desc; }
uint64_t shr__ctx_now(const shr_context *ctx) { return ctx->desc.now_ns ? ctx->desc.now_ns(ctx->desc.user) : 0; }
bool shr__ctx_refused(const shr_context *ctx) { return ctx->shutting_down || ctx->in_callback; }

bool shr__ctx_in_callback(const shr_context *ctx) { return ctx->in_callback; }

void shr__ctx_host_enter(shr_context *ctx, bool *saved) {
    *saved = ctx->in_callback;
    ctx->in_callback = true;
}

void shr__ctx_host_leave(shr_context *ctx, bool saved) { ctx->in_callback = saved; }

void shr__ctx_log(shr_context *ctx, shr_status status, const char *message) {
    if (ctx->desc.log) SHR_HOST(ctx, ctx->desc.log(ctx->desc.user, status, message));
}

void shr__ctx_trace(shr_context *ctx, shr_trace_kind kind, uint64_t id, uint64_t v0, uint64_t v1) {
    if (!ctx->desc.trace) return;
    shr_trace_event ev = {kind, shr__ctx_now(ctx), id, v0, v1};
    SHR_HOST(ctx, ctx->desc.trace(ctx->desc.user, &ev));
}

void shr__ctx_resource_failed(shr_context *ctx, shr_status status) {
    shr_event ev = {.kind = SHR_EVENT_RESOURCE_FAILED, .status = status};
    shr__push_event(ctx, &ev);
}

void **shr__ctx_plugin_slot(shr_context *ctx, const void *kind) {
    if (!kind) return NULL;
    for (int i = 0; i < SHR_PLUGIN_SLOTS; i++) {
        if (!ctx->plugin_kind[i]) ctx->plugin_kind[i] = kind;
        if (ctx->plugin_kind[i] == kind) return &ctx->plugin_state[i];
    }
    return NULL;
}

shr_status shr__res_attach(shr_context *ctx, shr__res *res, const shr__res_ops *ops) {
    if (!ctx || !res || !ops || !ops->resolve || !ops->free) return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    static _Atomic uint64_t serial;
    *res = (shr__res){ops, ctx, 0, false, atomic_fetch_add(&serial, 1) + 1, ctx->resources};
    ctx->resources = res;
    return SHR_OK;
}

static bool resolved_in(const shr__vec *v, const shr__res *res) {
    for (size_t i = 0; i < v->len; i++)
        if (*SHR_VEC_AT(v, shr__res *, i) == res) return true;
    return false;
}

void shr__res_collect(shr_context *ctx) {
    for (shr__res **pp = &ctx->resources; *pp;) {
        shr__res *r = *pp;
        if (r->dead && !r->users && !shr__io_busy(ctx, r) && !resolved_in(&ctx->frame.resolved, r)) {
            *pp = r->next;
            r->ops->free(r);
        } else {
            pp = &r->next;
        }
    }
}

shr_status shr_output_init(shr_output *o) {
    if (!o) return SHR_E_INVALID_ARG;
    *o = (shr_output){0};
    return SHR_OK;
}

shr_status shr_context_desc_init(shr_context_desc *d) {
    if (!d) return SHR_E_INVALID_ARG;
    *d = (shr_context_desc){.blink = {.start_visible = true},
                            .event_capacity = 64,
                            .max_unreleased_frames = 2,
                            .max_commands = 16384,
                            .max_reads = 4,
                            .page_cache_bytes = 3u << 20,
                            .image_bytes = 4u << 20,
                            .io_retry_limit = 3,
                            .io_retry_ns = 50000000,
                            .io_timeout_ns = 1000000000};
    return SHR_OK;
}

shr_status shr_screen_desc_init(shr_screen_desc *d) {
    if (!d) return SHR_E_INVALID_ARG;
    *d = (shr_screen_desc){0};
    return SHR_OK;
}

shr_status shr_asset_source_init(shr_asset_source *s) {
    if (!s) return SHR_E_INVALID_ARG;
    *s = (shr_asset_source){0};
    return SHR_OK;
}

shr_status shr_framebuffer_driver_init(shr_framebuffer_driver *d) {
    if (!d) return SHR_E_INVALID_ARG;
    *d = (shr_framebuffer_driver){.caps.domains = SHR_MEMORY_CPU};
    return SHR_OK;
}

static shr_alloc_kind dma_kind(const shr_surface *s) {
    return s->domain == SHR_MEMORY_DMA ? SHR_ALLOC_DMA : SHR_ALLOC_PAYLOAD;
}

static void composition_free(shr_context *ctx) {
    if (ctx->composition_owned)
        shr__free(&ctx->al, ctx->composition.pixels, ctx->composition.byte_length, 64, dma_kind(&ctx->composition));
    ctx->composition_owned = false;
}

static void context_free(shr_context *ctx) {
    shr__alloc al = ctx->al;
    shr__frame *f = &ctx->frame;
    for (int i = 0; i < SHR_TARGETS; i++) {
        shr__vec_free(&ctx->targets[i].damage.rects, &al);
        shr__vec_free(&ctx->targets[i].provisional, &al);
    }
    shr__vec_free(&ctx->staged.rects, &al);
    shr__vec_free(&f->cmds, &al);
    shr__vec_free(&f->damage, &al);
    shr__vec_free(&f->provisional, &al);
    shr__vec_free(&f->resolved, &al);
    composition_free(ctx);
    shr__free(&al, ctx->io, ctx->nio * sizeof(shr__io), SHR_ALIGNOF(shr__io), SHR_ALLOC_DESCRIPTOR);
    shr__free(&al, ctx->events, ctx->event_cap * sizeof(shr_event), SHR_ALIGNOF(shr_event), SHR_ALLOC_DESCRIPTOR);
    shr__free(&al, ctx->unreleased, ctx->desc.max_unreleased_frames * sizeof(uint64_t), 8, SHR_ALLOC_DESCRIPTOR);
    SHR_DELETE(&al, ctx, shr_context);
}

shr_status shr_create(const shr_context_desc *d, shr_context **out) {
    shr__alloc al;
    if (out) *out = NULL;
    if (!d || !out || !d->driver || !d->output) return SHR_E_INVALID_ARG;
    const shr_framebuffer_driver *drv = d->driver;
    const shr_output *o = d->output;
    if (!drv->execute || !o->acquire || !o->present || !o->discard || !d->max_commands || !d->max_unreleased_frames ||
        d->event_capacity < 2 || d->event_capacity - 2 < d->max_unreleased_frames || !d->max_reads ||
        (unsigned)d->blink.restart > SHR_BLINK_RESTART_ON_SUBMIT || (unsigned)o->timestamp > SHR_TIMESTAMP_COMPOSITOR ||
        (o->flags & ~(uint32_t)(SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT)) ||
        (!d->now_ns && (d->io_retry_ns || d->io_timeout_ns || drv->caps.timeout_ns)) ||
        !shr__alloc_init(&al, d->allocator))
        return SHR_E_INVALID_ARG;
    if (d->max_reads > 0xFFFF) return SHR_E_LIMIT;
    shr_context *ctx = SHR_NEW(&al, shr_context);
    if (!ctx) return SHR_E_NO_MEMORY;
    ctx->al = al;
    ctx->dma_alloc = d->allocator && d->allocator->alloc;
    ctx->desc = *d;
    ctx->desc.allocator = NULL, ctx->desc.driver = NULL, ctx->desc.output = NULL;
    if (ctx->desc.io_retry_limit > 0xFFFF) ctx->desc.io_retry_limit = 0xFFFF;
    ctx->driver = *drv;
    ctx->output = *o;
    ctx->blink = d->blink;
    ctx->blink_shown = true;
    ctx->nio = d->max_reads;
    ctx->event_cap = d->event_capacity;
    ctx->next_frame_id = 1;
    ctx->frame.target_rec = -1;
    atomic_init(&ctx->fence_state, 0);
    for (int i = 0; i < SHR_TARGETS; i++) {
        SHR_VEC_INIT(&ctx->targets[i].damage.rects, shr_rect);
        SHR_VEC_INIT(&ctx->targets[i].provisional, shr_rect);
    }
    SHR_VEC_INIT(&ctx->staged.rects, shr_rect);
    SHR_VEC_INIT(&ctx->frame.cmds, shr_draw_cmd);
    SHR_VEC_INIT(&ctx->frame.damage, shr_rect);
    SHR_VEC_INIT(&ctx->frame.provisional, shr_rect);
    SHR_VEC_INIT(&ctx->frame.resolved, shr__res *);
    ctx->io = shr__calloc(&al, ctx->nio, sizeof(shr__io), SHR_ALIGNOF(shr__io), SHR_ALLOC_DESCRIPTOR);
    ctx->events = shr__calloc(&al, ctx->event_cap, sizeof(shr_event), SHR_ALIGNOF(shr_event), SHR_ALLOC_DESCRIPTOR);
    ctx->unreleased = shr__calloc(&al, d->max_unreleased_frames, sizeof(uint64_t), 8, SHR_ALLOC_DESCRIPTOR);
    if (!ctx->io || !ctx->events || !ctx->unreleased) {
        context_free(ctx);
        return SHR_E_NO_MEMORY;
    }
    for (uint32_t i = 0; i < ctx->nio; i++) atomic_init(&ctx->io[i].state, 0);
    *out = ctx;
    return SHR_OK;
}

shr_status shr_begin_shutdown(shr_context *ctx) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    if (ctx->shutting_down) return SHR_OK;
    ctx->shutting_down = true;
    for (shr__res *r = ctx->resources; r; r = r->next)
        if (r->ops->shutdown) r->ops->shutdown(r);
    shr__io_cancel_all(ctx);
    return SHR_OK;
}

shr_status shr_destroy(shr_context *ctx) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    shr_begin_shutdown(ctx);
    if (ctx->frame.running || ctx->frame.state == FRAME_ISOLATED || ctx->unreleased_count || shr__io_busy(ctx, NULL))
        return SHR_E_WOULD_BLOCK;
    shr__frame_abandon(ctx);
    shr__res_collect(ctx);
    if (ctx->layers || ctx->resources) return SHR_E_WOULD_BLOCK;
    context_free(ctx);
    return SHR_OK;
}

void shr__targets_invalidate(shr_context *ctx) {
    for (int i = 0; i < SHR_TARGETS; i++) {
        shr__target *t = &ctx->targets[i];
        t->used = false;
        t->damage.rects.len = 0;
        t->damage.full = false;
        t->provisional.len = 0;
    }
    ctx->last_provisional = false;
}

/* Adjacent or overlapping rectangles merge; beyond SHR_MAX_DAMAGE the cheapest merge is taken. */
void shr__damage_add(const shr_context *ctx, shr__damage *d, shr_rect r) {
    r = shr__rect_intersect(r, (shr_rect){0, 0, ctx->screen.width, ctx->screen.height});
    if (d->full || shr__rect_empty(r)) return;
    shr_rect *v = d->rects.data;
    size_t best = 0;
    int64_t best_growth = INT64_MAX;
    for (size_t i = 0; i < d->rects.len; i++) {
        int64_t growth = shr__rect_area(shr__rect_union(v[i], r)) - shr__rect_area(v[i]);
        if (growth <= shr__rect_area(r)) {
            v[i] = shr__rect_union(v[i], r);
            return;
        }
        if (growth < best_growth) best = i, best_growth = growth;
    }
    if (d->rects.len >= SHR_MAX_DAMAGE) {
        v[best] = shr__rect_union(v[best], r);
        return;
    }
    shr_rect *p = shr__vec_push(&d->rects, &ctx->al);
    if (p)
        *p = r;
    else
        d->full = true;
}

void shr__damage_targets(shr_context *ctx, shr_rect r) {
    for (int i = 0; i < SHR_TARGETS; i++)
        if (ctx->targets[i].used) shr__damage_add(ctx, &ctx->targets[i].damage, r);
}

shr_status shr_screen_configure(shr_context *ctx, const shr_screen_desc *d) {
    if (!ctx || !d) return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    shr_pixel_format fmt = SHR_PIXEL_FORMAT, out_fmt = d->output_format ? d->output_format : fmt;
    if (d->width <= 0 || d->height <= 0 || d->width > 16384 || d->height > 16384 ||
        (out_fmt != SHR_FORMAT_RGB565 && out_fmt != SHR_FORMAT_RGBX8888) || (unsigned)d->rotation > SHR_ROTATE_90_CCW ||
        (d->flags & ~(uint32_t)SHR_SCREEN_COMPOSITION))
        return SHR_E_INVALID_ARG;
    bool composing = d->rotation != SHR_ROTATE_NONE || out_fmt != fmt || (d->flags & SHR_SCREEN_COMPOSITION) ||
                     d->composition;
    if (d->rotation != SHR_ROTATE_NONE && out_fmt != fmt) return SHR_E_UNSUPPORTED;
    if (ctx->frame.running || ctx->frame.state == FRAME_ISOLATED) return SHR_E_WOULD_BLOCK;
    shr_surface comp = {0};
    bool owned = false;
    if (composing && d->composition) {
        comp = *d->composition;
        if (shr_surface_validate(&comp) != SHR_OK || comp.width != d->width || comp.height != d->height ||
            comp.format != fmt)
            return SHR_E_INVALID_ARG;
    } else if (composing) {
        _Static_assert(16384ull * 16384 * 4 <= SIZE_MAX, "composition size fits size_t");
        size_t row, len;
        shr_format_row_bytes(fmt, d->width, &row);
        len = row * (size_t)d->height;
        uint32_t dom = ctx->driver.caps.domains;
        bool dma = (dom & SHR_MEMORY_DMA) && !(dom & SHR_MEMORY_CPU) && ctx->dma_alloc;
        void *px = shr__malloc(&ctx->al, len, 64, dma ? SHR_ALLOC_DMA : SHR_ALLOC_PAYLOAD);
        if (!px) return SHR_E_NO_MEMORY;
        comp = (shr_surface){px, d->width, d->height, row, len, fmt, 1, dma ? SHR_MEMORY_DMA : SHR_MEMORY_CPU, 0};
        owned = true;
    }
    if (composing && !shr__driver_reaches(&ctx->driver.caps, &comp)) { /* every frame would fail */
        if (owned) shr__free(&ctx->al, comp.pixels, comp.byte_length, 64, dma_kind(&comp));
        return SHR_E_UNSUPPORTED;
    }
    shr__frame_abandon(ctx);
    composition_free(ctx);
    ctx->screen = *d;
    ctx->screen.output_format = out_fmt;
    ctx->screen.composition = NULL;
    ctx->composing = composing;
    ctx->composition = comp;
    ctx->composition_owned = owned;
    ctx->configured = true;
    shr__targets_invalidate(ctx);
    ctx->stale = true;
    ctx->failed = false;
    return SHR_OK;
}

/* Only RESOURCE_FAILED lacks a reserved slot; OVERFLOW is reported where the first one was dropped. */
void shr__push_event(shr_context *ctx, const shr_event *ev) {
    if (ev->kind == SHR_EVENT_RESOURCE_FAILED && ctx->event_count + shr__events_reserved(ctx) >= ctx->event_cap) {
        if (!ctx->event_overflow) ctx->overflow_after = ctx->event_count;
        ctx->event_overflow = true;
        return;
    }
    ctx->events[(ctx->event_head + ctx->event_count++) % ctx->event_cap] = *ev;
}

shr_status shr_poll_event(shr_context *ctx, shr_event *out) {
    if (!ctx || !out) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    shr_event ev = {.kind = SHR_EVENT_OVERFLOW};
    if (ctx->event_overflow && !ctx->overflow_after) {
        ctx->event_overflow = false;
    } else if (ctx->event_count) {
        ev = ctx->events[ctx->event_head];
        ctx->event_head = (ctx->event_head + 1) % ctx->event_cap;
        ctx->event_count--;
        ctx->overflow_after -= ctx->event_overflow;
    } else {
        return SHR_E_NOT_FOUND;
    }
    *out = ev;
    return SHR_OK;
}

shr_status shr_output_released(shr_context *ctx, uint64_t frame_id) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    for (uint32_t i = 0; i < ctx->unreleased_count; i++) {
        if (ctx->unreleased[i] != frame_id) continue;
        ctx->unreleased[i] = ctx->unreleased[--ctx->unreleased_count];
        shr_event ev = {.kind = SHR_EVENT_FRAME_RELEASED, .frame_id = frame_id};
        shr__push_event(ctx, &ev);
        return SHR_OK;
    }
    return SHR_E_NOT_FOUND;
}

shr_status shr_output_displayed(shr_context *ctx, uint64_t frame_id, uint64_t timestamp_ns) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    if (ctx->output.timestamp == SHR_TIMESTAMP_NONE) return SHR_E_UNSUPPORTED;
    uint64_t *slot = &ctx->undisplayed[frame_id % SHR_DISPLAY_HISTORY];
    if (!frame_id || *slot != frame_id) return SHR_E_NOT_FOUND;
    if (ctx->event_count + shr__events_reserved(ctx) >= ctx->event_cap) return SHR_E_WOULD_BLOCK;
    *slot = 0;
    shr_event ev = {.kind = SHR_EVENT_FRAME_DISPLAYED, .frame_id = frame_id, .timestamp_ns = timestamp_ns};
    shr__push_event(ctx, &ev);
    shr__ctx_trace(ctx, SHR_TRACE_DISPLAYED, frame_id, timestamp_ns, 0);
    return SHR_OK;
}

shr_status shr_output_ready(shr_context *ctx) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    ctx->output_blocked = false;
    return SHR_OK;
}

shr_status shr_driver_ready(shr_context *ctx) {
    if (!ctx) return SHR_E_INVALID_ARG;
    atomic_fetch_add(&ctx->driver_ready, 1);
    return SHR_OK;
}

shr_status shr_asset_ready(shr_context *ctx) {
    if (!ctx) return SHR_E_INVALID_ARG;
    atomic_fetch_add(&ctx->asset_ready, 1);
    return SHR_OK;
}

uint32_t shr__ctx_asset_ready(const shr_context *ctx) { return atomic_load(&ctx->asset_ready); }

shr_status shr_output_error(shr_context *ctx, shr_status status) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    if (ctx->output_isolated) return SHR_OK;
    ctx->output_isolated = true;
    shr_event ev = {.kind = SHR_EVENT_OUTPUT_ISOLATED, .status = status};
    shr__push_event(ctx, &ev);
    shr__ctx_log(ctx, status, "output isolated after a device error");
    return SHR_OK;
}

shr_status shr_output_recover(shr_context *ctx) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    if (!ctx->output_isolated) return SHR_OK;
    if (ctx->event_count + shr__events_reserved(ctx) >= ctx->event_cap) return SHR_E_WOULD_BLOCK;
    ctx->output_isolated = false;
    ctx->output_blocked = false;
    shr__targets_invalidate(ctx);
    ctx->stale = true;
    ctx->failed = false;
    return SHR_OK;
}

shr_status shr_request_redraw(shr_context *ctx) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    shr__targets_invalidate(ctx);
    ctx->stale = true;
    ctx->failed = false;
    return SHR_OK;
}

shr_status shr_fence_signal(shr_context *ctx, shr_fence fence, shr_fence_state state) {
    if (!ctx || !fence || fence >> 62 || state == SHR_FENCE_PENDING || (unsigned)state > SHR_FENCE_CANCELLED)
        return SHR_E_INVALID_ARG;
    uint64_t expect = fence << 2 | SHR_FENCE_PENDING;
    return atomic_compare_exchange_strong(&ctx->fence_state, &expect, fence << 2 | state) ? SHR_OK : SHR_E_NOT_FOUND;
}

uint64_t shr__sat_add(uint64_t a, uint64_t b) {
    uint64_t s;
    return __builtin_add_overflow(a, b, &s) ? UINT64_MAX : s;
}

bool shr__blink_visible(const shr_context *ctx, uint64_t *next) {
    const shr_blink_profile *p = &ctx->blink;
    if (next) *next = 0;
    if (!p->interval_ns || !ctx->desc.now_ns) return true;
    uint64_t now = shr__ctx_now(ctx);
    if (now < p->epoch_ns) {
        if (next) *next = p->epoch_ns;
        return p->start_visible;
    }
    uint64_t k = (now - p->epoch_ns) / p->interval_ns;
    if (next) *next = shr__sat_add(p->epoch_ns + k * p->interval_ns, p->interval_ns);
    return (k % 2 == 0) == p->start_visible;
}

static size_t format_bits(shr_pixel_format f) SHR_NONBLOCKING {
    switch (f) {
    case SHR_FORMAT_RGB565: return 16;
    case SHR_FORMAT_RGBX8888:
    case SHR_FORMAT_RGBA8888: return 32;
    case SHR_FORMAT_A4: return 4;
    case SHR_FORMAT_A8: return 8;
    }
    return 0;
}

static uint64_t row_bytes(shr_pixel_format f, int32_t width) SHR_NONBLOCKING {
    return ((uint64_t)width * format_bits(f) + 7) / 8;
}

shr_status shr_format_row_bytes(shr_pixel_format f, int32_t width, size_t *out) {
    if (!out || !format_bits(f) || width < 0) return SHR_E_INVALID_ARG;
    uint64_t row = row_bytes(f, width);
#if SIZE_MAX < UINT64_MAX /* a row of an int32 width always fits a 64-bit size_t */
    if (row > SIZE_MAX) return SHR_E_OVERFLOW;
#endif
    *out = (size_t)row;
    return SHR_OK;
}

static shr_status buffer_check(const void *pixels, int32_t w, int32_t h, size_t stride, size_t len, shr_pixel_format f,
                               shr_memory_domain dom) SHR_NONBLOCKING {
    if ((dom && dom != SHR_MEMORY_CPU && dom != SHR_MEMORY_DMA && dom != SHR_MEMORY_DEVICE) || !format_bits(f))
        return SHR_E_INVALID_ARG;
    if (dom == SHR_MEMORY_DEVICE) return w > 0 && h > 0 ? SHR_OK : SHR_E_INVALID_ARG;
    if (w < 0 || h < 0) return SHR_E_INVALID_ARG;
    if (!w || !h) return SHR_OK;
    uint64_t row64 = row_bytes(f, w);
#if SIZE_MAX < UINT64_MAX
    if (row64 > SIZE_MAX) return SHR_E_OVERFLOW;
#endif
    size_t row = (size_t)row64, need;
    if (!pixels || stride < row) return SHR_E_INVALID_ARG;
    if (!shr__mul_size((size_t)(h - 1), stride, &need) || !shr__add_size(need, row, &need)) return SHR_E_OVERFLOW;
    return need <= len ? SHR_OK : SHR_E_INVALID_ARG;
}

shr_status shr_surface_validate(const shr_surface *s) SHR_NONBLOCKING {
    if (!s || (s->format != SHR_FORMAT_RGB565 && s->format != SHR_FORMAT_RGBX8888)) return SHR_E_INVALID_ARG;
    return buffer_check(s->pixels, s->width, s->height, s->stride, s->byte_length, s->format, s->domain);
}

shr_status shr_image_validate(const shr_image *m) SHR_NONBLOCKING {
    return m ? buffer_check(m->pixels, m->width, m->height, m->stride, m->byte_length, m->format, m->domain)
             : SHR_E_INVALID_ARG;
}

shr_status shr_rotation_map_point(shr_rotation r, int32_t w, int32_t h, shr_point p, bool inverse, shr_point *out) {
    if (!out || (unsigned)r > SHR_ROTATE_90_CCW || w <= 0 || h <= 0) return SHR_E_INVALID_ARG;
    bool quarter = r == SHR_ROTATE_90_CW || r == SHR_ROTATE_90_CCW;
    if (inverse && quarter) r = r == SHR_ROTATE_90_CW ? SHR_ROTATE_90_CCW : SHR_ROTATE_90_CW;
    int64_t sw = inverse && quarter ? h : w, sh = inverse && quarter ? w : h, x, y;
    switch (r) {
    case SHR_ROTATE_90_CW: x = sh - 1 - p.y, y = p.x; break;
    case SHR_ROTATE_90_CCW: x = p.y, y = sw - 1 - p.x; break;
    case SHR_ROTATE_180: x = sw - 1 - p.x, y = sh - 1 - p.y; break;
    default: x = p.x, y = p.y; break;
    }
    if (!shr__fits_i32(x) || !shr__fits_i32(y)) return SHR_E_OVERFLOW;
    *out = (shr_point){(int32_t)x, (int32_t)y};
    return SHR_OK;
}
