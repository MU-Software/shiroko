#include "compositor.h"
#include "shr_glyph.h"

static void discard(shr_context *ctx, const shr_surface *s) {
    SHR_HOST(ctx, ctx->output.discard(ctx->output.user, s));
}

static void resolved_end(shr__vec *v, uint64_t frame) {
    for (size_t i = 0; i < v->len; i++) {
        shr__res *r = *SHR_VEC_AT(v, shr__res *, i);
        if (r->ops->frame_end) r->ops->frame_end(r, frame);
    }
    v->len = 0;
}

static void frame_reset(shr_context *ctx) {
    shr__frame *f = &ctx->frame;
    f->cmds.len = f->damage.len = f->provisional.len = 0;
    f->state = FRAME_IDLE;
    f->has_output = f->built = f->running = f->took_damage = f->prov_stale = false;
    f->refused = false;
    f->target_rec = -1;
}

/* ===== Buffer plan: built with the frame, taken by the registry once the driver accepted it ===== */

/* Ids with `releasing` set were stamped by plan_begin(). */
static bool slot_free(const shr__slot *s, uint64_t frame) { return s->stamp == frame ? !s->taken : !s->buf; }

static shr_status prologue_push(shr_context *ctx, shr__frame *f, shr_draw_cmd c) {
    shr_draw_cmd *p = shr__vec_push(&f->prologue, &ctx->al);
    if (!p) return SHR_E_NO_MEMORY;
    *p = c;
    return SHR_OK;
}

static void lru_touch(shr_context *ctx, shr__buf *b) {
    shr__lru_remove(&ctx->lru, &b->lru);
    shr__lru_push(&ctx->lru, &b->lru);
}

/* Pending RELEASEs open the plan, so their ids are free for it. */
static shr_status plan_begin(shr_context *ctx, shr__frame *f) {
    shr_status st = SHR_OK;
    f->resident = ctx->resident;
    for (uint32_t i = 0; st == SHR_OK && i < ctx->nreleased; i++) {
        shr__slot *s = &ctx->slots[ctx->released[i] - 1];
        s->stamp = f->frame_id, s->taken = false;
        f->resident -= s->bytes;
        st = prologue_push(ctx, f, (shr_draw_cmd){.kind = SHR_CMD_BUFFER_RELEASE, .buffer = ctx->released[i]});
    }
    return st;
}

/* A free id for `b`, evicting registered buffers the frame does not draw from, least recently used first.
 * Touched buffers move to the front of the LRU, so the oldest one is untouched unless none is. */
static shr_status plan_take(shr_context *ctx, shr__frame *f, shr__buf *b) {
    const shr_driver_caps *k = &ctx->driver.caps;
    uint64_t budget = (k->buffer_flags & SHR_BUFFER_COPIES) && k->buffer_bytes ? k->buffer_bytes : UINT64_MAX;
    uint32_t id = 0;
    for (uint32_t i = 0; i < k->max_buffers && !id; i++)
        if (slot_free(&ctx->slots[i], f->frame_id)) id = i + 1;
    while (!id || f->resident + b->mem.byte_length > budget) {
        shr__lru_node *n = shr__lru_oldest(ctx->lru);
        shr__buf *v = n ? SHR_CONTAINER(n, shr__buf, lru) : NULL;
        shr__slot *s = v ? &ctx->slots[v->id - 1] : NULL;
        if (!s || s->stamp == f->frame_id) return SHR_E_LIMIT;
        shr_status st = prologue_push(ctx, f, (shr_draw_cmd){.kind = SHR_CMD_BUFFER_RELEASE, .buffer = v->id});
        if (st != SHR_OK) return st;
        s->stamp = f->frame_id, s->taken = false;
        f->resident -= s->bytes;
        lru_touch(ctx, v);
        if (!id) id = v->id;
    }
    ctx->slots[id - 1].stamp = f->frame_id, ctx->slots[id - 1].taken = true;
    f->resident += b->mem.byte_length;
    b->id = id;
    return prologue_push(ctx, f, (shr_draw_cmd){.kind = SHR_CMD_BUFFER_REGISTER, .buffer = id, .src = b->mem});
}

/* From the first use in a frame on, b->id is the buffer's id in the frame's plan. */
static shr_status frame_use(shr_context *ctx, shr__frame *f, shr__buf *b) {
    if (b->used == f->frame_id) return SHR_OK;
    shr__planned *p = shr__vec_push(&f->planned, &ctx->al);
    if (!p) return SHR_E_NO_MEMORY;
    *p = (shr__planned){b, b->id};
    b->used = f->frame_id;
    shr__slot *s = b->id ? &ctx->slots[b->id - 1] : NULL;
    if (!s || s->stamp == f->frame_id) return plan_take(ctx, f, b); /* new, or evicted earlier in this plan */
    s->stamp = f->frame_id, s->taken = true;
    lru_touch(ctx, b);
    if (!s->known)
        return prologue_push(ctx, f, (shr_draw_cmd){.kind = SHR_CMD_BUFFER_REGISTER, .buffer = b->id, .src = b->mem});
    if (shr__rect_empty(b->dirty)) return SHR_OK;
    return prologue_push(ctx, f, (shr_draw_cmd){.kind = SHR_CMD_BUFFER_UPDATE, .buffer = b->id, .src_rect = b->dirty});
}

static shr_status plan_place(shr_context *ctx, shr__frame *f) {
    size_t n = f->prologue.len;
    if (!n) return SHR_OK;
    if (!shr__vec_reserve(&f->cmds, &ctx->al, n)) return SHR_E_NO_MEMORY;
    shr_draw_cmd *c = f->cmds.data;
    memmove(c + n, c, f->cmds.len * sizeof(*c));
    memcpy(c, f->prologue.data, n * sizeof(*c));
    f->cmds.len += n;
    return SHR_OK;
}

/* Releases first: a buffer may take an id the plan released. */
static void plan_commit(shr_context *ctx, shr__frame *f) {
    for (size_t i = 0; i < f->prologue.len; i++) {
        const shr_draw_cmd *c = SHR_VEC_AT(&f->prologue, shr_draw_cmd, i);
        if (c->kind != SHR_CMD_BUFFER_RELEASE) continue;
        shr__slot *s = &ctx->slots[c->buffer - 1];
        if (s->buf && s->buf->used != f->frame_id) { /* not freed meanwhile, nor drawn after its eviction */
            s->buf->id = 0;
            shr__lru_remove(&ctx->lru, &s->buf->lru);
        }
        ctx->resident -= s->bytes;
        *s = (shr__slot){0};
    }
    for (size_t i = 0; i < f->planned.len; i++) {
        shr__buf *b = SHR_VEC_AT(&f->planned, shr__planned, i)->buf;
        shr__slot *s = &ctx->slots[b->id - 1];
        ctx->resident += b->mem.byte_length - s->bytes;
        s->buf = b, s->bytes = b->mem.byte_length, s->known = true;
        b->dirty = (shr_rect){0, 0, 0, 0};
        if (!b->lru.prev) shr__lru_push(&ctx->lru, &b->lru);
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < ctx->nreleased; i++)
        if (ctx->slots[ctx->released[i] - 1].releasing) ctx->released[n++] = ctx->released[i];
    ctx->nreleased = n;
    f->planned.len = 0; /* the prologue stays until the raster ends, for batch_lost() */
}

/* Before frame_end: the planned buffers are pinned until then. */
static void plan_restore(shr__frame *f) {
    for (size_t i = 0; i < f->planned.len; i++) {
        const shr__planned *p = SHR_VEC_AT(&f->planned, shr__planned, i);
        p->buf->id = p->old_id;
    }
    f->planned.len = 0;
}

static void plan_drop(shr__frame *f) {
    plan_restore(f);
    f->prologue.len = 0;
}

/* The raster batch failed or timed out: which ids the driver holds is unspecified. Every buffer registers again
 * before it is drawn, and each id the batch registered or released is given up and released first in the next
 * prologue, so a driver keeping copies drops what it may still hold there. */
static void batch_lost(shr_context *ctx, shr__frame *f) {
    plan_restore(f);
    for (uint32_t i = 0; i < ctx->driver.caps.max_buffers; i++) ctx->slots[i].known = false;
    for (size_t i = 0; i < f->prologue.len; i++) {
        const shr_draw_cmd *c = SHR_VEC_AT(&f->prologue, shr_draw_cmd, i);
        shr__slot *s = &ctx->slots[c->buffer - 1];
        if (c->kind == SHR_CMD_BUFFER_UPDATE || s->releasing) continue;
        if (s->buf) {
            s->buf->id = 0;
            shr__lru_remove(&ctx->lru, &s->buf->lru);
            s->buf = NULL;
        }
        s->releasing = true;
        ctx->released[ctx->nreleased++] = c->buffer;
    }
    f->prologue.len = 0;
}

/* The raster writes only inside the damage, so returning it restores what the target shows. */
static void frame_drop(shr_context *ctx) {
    shr__frame *f = &ctx->frame;
    if (f->took_damage && f->target_rec >= 0) {
        shr__target *t = &ctx->targets[f->target_rec];
        for (size_t i = 0; t->used && i < f->damage.len; i++)
            shr__damage_add(ctx, &t->damage, *SHR_VEC_AT(&f->damage, shr_rect, i));
    }
    plan_drop(f);
    resolved_end(&f->resolved, f->frame_id);
    if (f->has_output) discard(ctx, &f->output);
    ctx->stale = true;
    frame_reset(ctx);
}

/* An isolated frame keeps its buffers until its fence resolves. */
static void frame_fail(shr_context *ctx, shr_status st) {
    shr_event ev = {.kind = SHR_EVENT_PRESENT_FAILED, .frame_id = ctx->frame.frame_id, .status = st};
    shr__push_event(ctx, &ev);
    shr__ctx_log(ctx, st, "frame failed");
    ctx->failed = true;
    if (ctx->frame.state != FRAME_ISOLATED) frame_drop(ctx);
}

/* Shutting down, a submission no frame started ends once the queue has room for it. */
static bool submission_ends(const shr_context *ctx) {
    return ctx->shutting_down && ctx->submitted && ctx->event_count + shr__events_reserved(ctx) < ctx->event_cap;
}

void shr__frame_abandon(shr_context *ctx) {
    if (ctx->frame.state != FRAME_IDLE) {
        shr_event ev = {.kind = SHR_EVENT_FRAME_SUPERSEDED, .frame_id = ctx->frame.frame_id};
        shr__push_event(ctx, &ev);
        frame_drop(ctx);
    }
    if (submission_ends(ctx)) {
        shr_event ev = {.kind = SHR_EVENT_FRAME_SUPERSEDED, .frame_id = ctx->next_frame_id++};
        shr__push_event(ctx, &ev);
        ctx->submitted = false;
    }
}

static int target_for(shr_context *ctx, const shr_surface *s) {
    if (ctx->composing) return 0;
    if (!(ctx->output.flags & SHR_OUTPUT_PRESERVES_CONTENT) || (!s->pixels && !s->resource_id)) return -1;
    int pick = 0;
    for (int i = 0; i < SHR_TARGETS; i++) {
        const shr__target *t = &ctx->targets[i];
        if (t->used && t->resource_id == s->resource_id && (s->resource_id || t->pixels == s->pixels)) return i;
        const shr__target *p = &ctx->targets[pick];
        if (p->used && (!t->used || t->last_use < p->last_use)) pick = i;
    }
    shr__target *t = &ctx->targets[pick];
    t->used = false;
    t->damage.rects.len = 0;
    t->damage.full = false;
    t->provisional.len = 0;
    return pick;
}

/* Moves the target's damage (and its provisional pixels) into the frame; beyond 3/4 of the screen the
 * whole screen is drawn. On failure the target is forgotten, so the next frame redraws it whole. */
static bool take_damage(shr_context *ctx, shr__frame *f) {
    shr__target *t = f->target_rec >= 0 ? &ctx->targets[f->target_rec] : NULL;
    shr_rect full = {0, 0, ctx->screen.width, ctx->screen.height};
    f->damage.len = 0;
    shr__damage d = {f->damage, !t || !t->used || t->generation != f->target.generation || t->damage.full};
    if (!d.full) {
        d.rects = t->damage.rects;
        t->damage.rects = f->damage;
        for (size_t i = 0; i < t->provisional.len; i++)
            shr__damage_add(ctx, &d, *SHR_VEC_AT(&t->provisional, shr_rect, i));
    }
    int64_t area = 0;
    for (size_t i = 0; i < d.rects.len; i++) area += shr__rect_area(*SHR_VEC_AT(&d.rects, shr_rect, i));
    if (d.full || area * 4 > shr__rect_area(full) * 3) {
        d.rects.len = 0;
        shr_rect *r = shr__vec_push(&d.rects, &ctx->al);
        f->damage = d.rects;
        if (!r) {
            if (t) t->used = false;
            return false;
        }
        *r = full;
        area = shr__rect_area(full);
    }
    f->damage = d.rects;
    f->damaged_pixels = area;
    if (t) {
        *t = (shr__target){true, f->target.pixels, f->target.resource_id, f->target.generation, f->frame_id,
                           {t->damage.rects, false}, t->provisional};
        t->damage.rects.len = t->provisional.len = 0;
    }
    f->took_damage = true;
    return true;
}

static shr_draw_cmd *push_cmd(shr_context *ctx, shr__frame *f, shr_status *st) {
    if (f->cmds.len >= ctx->desc.max_commands) {
        *st = SHR_E_LIMIT;
        return NULL;
    }
    shr_draw_cmd *c = shr__vec_push(&f->cmds, &ctx->al);
    if (!c) *st = SHR_E_NO_MEMORY;
    return c;
}

/* Pixels of one GLYPH/IMAGE command inside `clip`; *provisional is set when a fallback was drawn. */
static shr_status emit_resolved(shr_context *ctx, shr__frame *f, const shr_lyr *l, const shr__lcmd *lc, shr_rect clip,
                                bool *provisional) {
    shr__resolved r;
    shr_status st = lc->res->ops->resolve(lc->res, lc->id, f->frame_id, &r);
    if (st == SHR_E_NOT_FOUND) return SHR_OK;
    if (st != SHR_OK) return st;
    bool known = false;
    for (size_t i = 0; i < f->resolved.len && !known; i++) known = *SHR_VEC_AT(&f->resolved, shr__res *, i) == lc->res;
    shr__res **slot = known ? NULL : shr__vec_push(&f->resolved, &ctx->al);
    if (!known && !slot) {
        if (lc->res->ops->frame_end) lc->res->ops->frame_end(lc->res, f->frame_id);
        return SHR_E_NO_MEMORY;
    }
    if (slot) *slot = lc->res;
    bool glyph = lc->kind == SHR__LCMD_GLYPH;
    uint32_t syn = glyph ? lc->flags & r.synth & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC) : 0;
    int32_t w = r.rect.x1 - r.rect.x0, h = r.rect.y1 - r.rect.y0, x0 = 0, x1 = w;
    if (syn) shr__glyph_footprint(w, h, syn, r.slant_axis, &x0, &x1);
    int64_t x = (int64_t)l->rect.x0 + lc->anchor.x + r.offset.x, y = (int64_t)l->rect.y0 + lc->anchor.y + r.offset.y;
    shr_rect full = {shr__clamp32(x + x0), shr__clamp32(y), shr__clamp32(x + x1), shr__clamp32(y + h)};
    shr_rect d = shr__rect_intersect(full, clip);
    if (shr__rect_empty(d)) return SHR_OK;
    *provisional = r.provisional;
    if ((st = frame_use(ctx, f, r.buf)) != SHR_OK) return st;
    shr_draw_cmd *c = push_cmd(ctx, f, &st);
    if (!c) return st;
    *c = (shr_draw_cmd){.kind = glyph ? SHR_CMD_GLYPH : SHR_CMD_IMAGE,
                        .flags = glyph ? (lc->flags & SHR_GLYPH_DIM) | syn : 0,
                        .dst = d,
                        .color = lc->color,
                        .buffer = r.buf->id,
                        .src_rect = r.rect,
                        .src_origin = {(int32_t)(d.x0 - x), (int32_t)(d.y0 - y)},
                        .slant_axis = syn & SHR_GLYPH_ITALIC ? r.slant_axis : 0};
    return SHR_OK;
}

/* One group inside `clip`. A cache pair is kept only around content that is final and fully drawn:
 * inside it the commands draw the whole cached area and cache_clip says what is written. */
static shr_status emit_group(shr_context *ctx, shr__frame *f, const shr_lyr *l, const shr__group *g, shr_rect clip) {
    shr_status st = SHR_OK;
    shr_rect screen = {0, 0, ctx->screen.width, ctx->screen.height}, cache = {0, 0, 0, 0};
    size_t begin_cmd = 0, begin_at = 0;
    bool caching = false;
    for (size_t i = 0; i < g->n; i++) {
        if (!caching && i % SHR__BLOCK == 0 &&
            shr__rect_empty(shr__rect_intersect(shr__rect_move(g->blocks[i / SHR__BLOCK], l->rect.x0, l->rect.y0), clip))) {
            i += SHR__BLOCK - 1;
            continue;
        }
        const shr__lcmd *lc = &g->cmds[i];
        shr_rect dst = shr__rect_move(lc->dst, l->rect.x0, l->rect.y0);
        shr_draw_cmd *c;
        if (lc->kind == SHR__LCMD_CACHE_BEGIN) {
            shr_rect part = shr__rect_intersect(dst, clip);
            shr_rect inside = shr__rect_intersect(dst, shr__rect_intersect(l->rect, screen));
            if (shr__rect_empty(part) || memcmp(&inside, &dst, sizeof(dst))) continue;
            if (!(c = push_cmd(ctx, f, &st))) return st;
            *c = (shr_draw_cmd){.kind = SHR_CMD_CACHE_BEGIN, .dst = dst, .key = {lc->key[0], lc->key[1]},
                                .cache_clip = part};
            caching = true, cache = dst, begin_cmd = f->cmds.len - 1, begin_at = i;
            continue;
        }
        if (lc->kind == SHR__LCMD_CACHE_END) {
            if (caching && !(c = push_cmd(ctx, f, &st))) return st;
            if (caching) *c = (shr_draw_cmd){.kind = SHR_CMD_CACHE_END};
            caching = false;
            continue;
        }
        shr_rect d = shr__rect_intersect(dst, caching ? cache : clip);
        if (shr__rect_empty(d)) continue;
        bool hidden = (lc->flags & SHR__LCMD_BLINK) && !f->blink_visible, provisional = false;
        if (!hidden && lc->kind == SHR__LCMD_FILL) {
            if (!(c = push_cmd(ctx, f, &st))) return st;
            *c = (shr_draw_cmd){.kind = SHR_CMD_FILL, .flags = lc->flags & SHR_GLYPH_DIM, .dst = d, .color = lc->color};
        } else if (!hidden && (st = emit_resolved(ctx, f, l, lc, d, &provisional)) != SHR_OK) {
            return st;
        }
        if (caching && (hidden || provisional)) { /* redo the pair's commands without the hint */
            f->cmds.len = begin_cmd;
            caching = false;
            i = begin_at;
            continue;
        }
        if (!provisional) continue;
        shr_rect *p = shr__vec_push(&f->provisional, &ctx->al);
        if (!p) return SHR_E_NO_MEMORY;
        *p = d;
    }
    return SHR_OK;
}

/* Whether the opaque parts of `l`'s groups, in order, cover `r` row by row.
 * May miss a cover, never claims a false one. */
static bool hides(const shr_lyr *l, shr_rect r) {
    int32_t y = r.y0;
    for (size_t i = 0; l->visible && i < l->groups.len && y < r.y1; i++) {
        shr_rect o = shr__rect_move(SHR_VEC_AT(&l->groups, shr__group, i)->opaque, l->rect.x0, l->rect.y0);
        o = shr__rect_intersect(o, l->rect);
        if (o.x0 <= r.x0 && o.x1 >= r.x1 && o.y0 <= y && o.y1 > y) y = o.y1;
    }
    return y >= r.y1;
}

/* Draws `rect` from the topmost layer that hides it, else from the clear colour up. */
static shr_status emit_rect(shr_context *ctx, shr__frame *f, shr_rect rect) {
    shr_status st = SHR_OK;
    const shr_lyr *from = NULL;
    for (const shr_lyr *l = ctx->layers; l; l = l->next)
        if (hides(l, rect)) from = l;
    if (!from) {
        shr_draw_cmd *c = push_cmd(ctx, f, &st);
        if (!c) return st;
        *c = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = rect, .color = ctx->screen.clear};
        from = ctx->layers;
    }
    for (const shr_lyr *l = from; l; l = l->next) {
        shr_rect clip = shr__rect_intersect(rect, l->rect);
        if (!l->visible || shr__rect_empty(clip)) continue;
        for (size_t i = 0; i < l->groups.len; i++) {
            const shr__group *g = SHR_VEC_AT(&l->groups, shr__group, i);
            if (shr__rect_empty(shr__rect_intersect(shr__rect_move(g->bounds, l->rect.x0, l->rect.y0), clip))) continue;
            if ((st = emit_group(ctx, f, l, g, clip)) != SHR_OK) return st;
        }
    }
    return SHR_OK;
}

bool shr__mem_reaches(const shr_driver_caps *k, shr_memory_domain dom, const void *px, size_t stride) {
    uint32_t domains = k->domains ? k->domains : SHR_MEMORY_CPU;
    return (domains & (dom ? dom : SHR_MEMORY_CPU)) && !(k->address_align && (uintptr_t)px % k->address_align) &&
           !(k->stride_align && stride % k->stride_align);
}

bool shr__driver_reaches(const shr_driver_caps *k, const shr_surface *s) {
    return shr__mem_reaches(k, s->domain, s->pixels, s->stride) && !(k->max_width && s->width > k->max_width) &&
           !(k->max_height && s->height > k->max_height);
}

/* The DMA memory the batch hands the device: the destination, COPY and ROTATE sources and registered buffers
 * whole, the rows of each UPDATE (only committed buffers get one, so their slot names them). */
static void sync_buffers(shr_context *ctx, const shr_surface *dst, const shr_draw_cmd *c, size_t n) {
    if (!ctx->driver.sync) return;
    if (dst->domain == SHR_MEMORY_DMA)
        SHR_HOST(ctx, ctx->driver.sync(ctx->driver.user, dst->pixels, dst->byte_length));
    for (size_t i = 0; i < n; i++) {
        const shr_image *m = &c[i].src;
        size_t off = 0, len = m->byte_length, row;
        switch (c[i].kind) {
        case SHR_CMD_BUFFER_UPDATE:
            m = &ctx->slots[c[i].buffer - 1].buf->mem;
            shr_format_row_bytes(m->format, m->width, &row);
            off = (size_t)c[i].src_rect.y0 * m->stride;
            len = (size_t)(c[i].src_rect.y1 - c[i].src_rect.y0 - 1) * m->stride + row;
            break;
        case SHR_CMD_COPY:
        case SHR_CMD_ROTATE:
        case SHR_CMD_BUFFER_REGISTER: break;
        default: continue;
        }
        if (m->domain == SHR_MEMORY_DMA)
            SHR_HOST(ctx, ctx->driver.sync(ctx->driver.user, (const uint8_t *)m->pixels + off, len));
    }
}

/* No submission follows an isolated one, so the slot always holds the queried fence. */
static shr_fence_state fence_state(const shr_context *ctx) {
    return (shr_fence_state)(atomic_load(&ctx->fence_state) & 3);
}

/* Without a working reset the device may still touch the buffers: the frame isolates them. */
static shr_status handle_timeout(shr_context *ctx, shr__frame *f) {
    shr_event ev = {.kind = SHR_EVENT_DRIVER_TIMEOUT, .frame_id = f->frame_id, .status = SHR_E_TIMEOUT};
    shr__push_event(ctx, &ev);
    shr__ctx_log(ctx, SHR_E_TIMEOUT, "driver submission timed out");
    if (ctx->driver.cancel) SHR_HOST(ctx, ctx->driver.cancel(ctx->driver.user, f->fence));
    shr_status reset = SHR_E_UNSUPPORTED;
    if (ctx->driver.reset) SHR_HOST(ctx, reset = ctx->driver.reset(ctx->driver.user));
    if (reset == SHR_OK)
        atomic_store(&ctx->fence_state, 0);
    else
        f->state = FRAME_ISOLATED;
    return SHR_E_TIMEOUT;
}

/* A refused submission is retried after io_retry_ns (when set) or once the driver said it is ready. */
static bool refusal_over(shr_context *ctx, const shr__frame *f) {
    return (ctx->desc.io_retry_ns && shr__ctx_now(ctx) >= f->deadline) || atomic_load(&ctx->driver_ready) != f->ready_seen;
}

/* 1 = done, 0 = waiting, -1 = failed (*st). */
static int run(shr_context *ctx, shr__frame *f, shr_status *st) {
    if (f->running) {
        shr_fence_state fs = fence_state(ctx);
        uint64_t now = shr__ctx_now(ctx);
        if (fs == SHR_FENCE_PENDING && (!ctx->driver.caps.timeout_ns || now < f->deadline)) return 0;
        f->running = false;
        if (fs == SHR_FENCE_PENDING) {
            *st = handle_timeout(ctx, f);
        } else {
            shr__ctx_trace(ctx, SHR_TRACE_FENCE_WAIT, f->frame_id, now - f->started, 0);
            if (fs == SHR_FENCE_SUCCEEDED) return 1;
            *st = SHR_E_DEVICE;
        }
        batch_lost(ctx, f);
        return -1;
    }
    if (f->refused && !refusal_over(ctx, f)) return 0;
    const shr_draw_cmd *cmds = f->built ? &f->convert : f->cmds.data; /* the conversion, or the raster */
    size_t n = f->built ? 1 : f->cmds.len;
    if (!n) return 1;
    if (!shr__driver_reaches(&ctx->driver.caps, &f->target)) return *st = SHR_E_UNSUPPORTED, -1;
    sync_buffers(ctx, &f->target, cmds, n);
    f->ready_seen = atomic_load(&ctx->driver_ready); /* before execute(): a ready signal during it counts */
    f->fence = ++ctx->fence_gen;
    atomic_store(&ctx->fence_state, f->fence << 2 | SHR_FENCE_PENDING);
    SHR_HOST(ctx, *st = ctx->driver.execute(ctx->driver.user, &f->target, cmds, n, f->fence));
    if (*st != SHR_IN_PROGRESS) atomic_store(&ctx->fence_state, 0);
    if (*st == SHR_E_WOULD_BLOCK) {
        f->refused = true;
        f->deadline = shr__sat_add(shr__ctx_now(ctx), ctx->desc.io_retry_ns);
        return 0;
    }
    f->refused = false;
    if (*st != SHR_OK && *st != SHR_IN_PROGRESS) return batch_lost(ctx, f), -1;
    plan_commit(ctx, f);
    if (*st == SHR_OK) return 1;
    f->running = true;
    f->started = shr__ctx_now(ctx);
    f->deadline = shr__sat_add(f->started, ctx->driver.caps.timeout_ns);
    return 0;
}

static bool acquire_output(shr_context *ctx, shr__frame *f, int32_t w, int32_t h, shr_pixel_format fmt, shr_status *st) {
    shr_surface s = {0};
    SHR_HOST(ctx, *st = ctx->output.acquire(ctx->output.user, &s));
    if (*st == SHR_E_WOULD_BLOCK) {
        ctx->output_blocked = true;
        return false;
    }
    if (*st == SHR_OK && (shr_surface_validate(&s) != SHR_OK || s.width != w || s.height != h || s.format != fmt)) {
        discard(ctx, &s);
        *st = SHR_E_INVALID_ARG;
        shr__ctx_log(ctx, *st, "output surface does not match the screen configuration");
    }
    if (*st != SHR_OK) return false;
    f->output = s;
    f->has_output = true;
    return true;
}

static void start_frame(shr_context *ctx) {
    shr__frame *f = &ctx->frame;
    shr_status st = SHR_OK;
    bool submitted = ctx->submitted, stale = ctx->stale;
    ctx->submitted = ctx->stale = false;
    f->frame_id = ctx->next_frame_id++;
    f->target_rec = -1;
    if (ctx->composing) {
        f->target = ctx->composition;
    } else if (acquire_output(ctx, f, ctx->screen.width, ctx->screen.height, SHR_PIXEL_FORMAT, &st)) {
        f->target = f->output;
    } else if (st == SHR_E_WOULD_BLOCK) {
        ctx->submitted = submitted, ctx->stale = stale;
        ctx->next_frame_id--;
        return;
    } else {
        f->state = FRAME_RASTER;
        frame_fail(ctx, st);
        return;
    }
    f->state = FRAME_RASTER;
    bool visible = shr__blink_visible(ctx, NULL);
    if (visible != ctx->blink_shown) {
        shr__blink_damage(ctx);
        ctx->blink_shown = visible;
    }
    f->blink_visible = visible;
    f->target_rec = target_for(ctx, &f->target);
    if (!take_damage(ctx, f)) {
        frame_fail(ctx, SHR_E_NO_MEMORY);
        return;
    }
    if (!f->damage.len && !stale) { /* the output already shows this state */
        shr_event ev = {.kind = SHR_EVENT_FRAME_SUPERSEDED, .frame_id = f->frame_id};
        shr__push_event(ctx, &ev);
        if (f->has_output) discard(ctx, &f->output);
        frame_reset(ctx);
        return;
    }
    st = plan_begin(ctx, f);
    for (size_t i = 0; st == SHR_OK && i < f->damage.len; i++)
        st = emit_rect(ctx, f, *SHR_VEC_AT(&f->damage, shr_rect, i));
    if (st == SHR_OK) st = plan_place(ctx, f);
    if (st != SHR_OK) {
        frame_fail(ctx, st);
        return;
    }
    shr__ctx_trace(ctx, SHR_TRACE_RASTER_BEGIN, f->frame_id, f->cmds.len, (uint64_t)f->damaged_pixels);
}

static void raster_complete(shr_context *ctx, shr__frame *f) {
    shr__ctx_trace(ctx, SHR_TRACE_RASTER_END, f->frame_id, 0, 0);
    f->prologue.len = 0;
    resolved_end(&f->resolved, f->frame_id);
    f->took_damage = false;
    shr__target *t = f->target_rec >= 0 && ctx->targets[f->target_rec].used ? &ctx->targets[f->target_rec] : NULL;
    if (f->prov_stale && f->provisional.len) {
        for (size_t i = 0; t && i < f->provisional.len; i++)
            shr__damage_add(ctx, &t->damage, *SHR_VEC_AT(&f->provisional, shr_rect, i));
        ctx->stale = true;
    } else if (t) {
        shr__vec tmp = t->provisional;
        t->provisional = f->provisional;
        f->provisional = tmp;
    } else if (f->target_rec < 0) {
        ctx->last_provisional = f->provisional.len > 0;
    }
    f->provisional.len = 0;
    f->state = ctx->composing ? FRAME_CONVERT : FRAME_PRESENT;
}

static void convert(shr_context *ctx, shr__frame *f) {
    shr_status st = SHR_OK;
    bool quarter = ctx->screen.rotation == SHR_ROTATE_90_CW || ctx->screen.rotation == SHR_ROTATE_90_CCW;
    int32_t ow = quarter ? ctx->screen.height : ctx->screen.width, oh = quarter ? ctx->screen.width : ctx->screen.height;
    if (!f->has_output) {
        if (ctx->output_isolated || ctx->output_blocked || ctx->unreleased_count >= ctx->desc.max_unreleased_frames)
            return;
        if (!acquire_output(ctx, f, ow, oh, ctx->screen.output_format, &st)) {
            if (st != SHR_E_WOULD_BLOCK) frame_fail(ctx, st);
            return;
        }
    }
    if (!f->built) {
        const shr_surface *s = &ctx->composition;
        f->convert = (shr_draw_cmd){.kind = ctx->screen.rotation != SHR_ROTATE_NONE ? SHR_CMD_ROTATE : SHR_CMD_COPY,
                            .dst = {0, 0, ow, oh},
                            .src = {s->pixels, s->width, s->height, s->stride, s->byte_length, s->format, s->domain},
                            .rotation = ctx->screen.rotation};
        f->target = f->output;
        shr__ctx_trace(ctx, SHR_TRACE_CONVERT, f->frame_id, (uint64_t)s->byte_length + f->output.byte_length, 0);
        f->built = true;
    }
    int r = run(ctx, f, &st);
    if (r < 0) frame_fail(ctx, st);
    if (r > 0) f->state = FRAME_PRESENT;
}

/* A finished frame is presented even when a newer state waits: that one starts right after. */
static void present(shr_context *ctx, shr__frame *f) {
    if (ctx->output_isolated || ctx->output_blocked) return;
    shr_status st;
    SHR_HOST(ctx, st = ctx->output.present(ctx->output.user, &f->output, f->frame_id));
    if (st == SHR_E_WOULD_BLOCK) {
        ctx->output_blocked = true;
        return;
    }
    if (st != SHR_OK) {
        if (!ctx->composing && f->target_rec >= 0) ctx->targets[f->target_rec].used = false;
        frame_fail(ctx, st);
        return;
    }
    ctx->undisplayed[f->frame_id % SHR_DISPLAY_HISTORY] = f->frame_id;
    shr_event ev = {.kind = SHR_EVENT_PRESENT_ACCEPTED, .frame_id = f->frame_id};
    shr__push_event(ctx, &ev);
    shr__ctx_trace(ctx, SHR_TRACE_PRESENT, f->frame_id, 0, 0);
    if (ctx->output.flags & SHR_OUTPUT_RELEASE_ON_PRESENT) {
        shr_event rel = {.kind = SHR_EVENT_FRAME_RELEASED, .frame_id = f->frame_id};
        shr__push_event(ctx, &rel);
    } else {
        ctx->unreleased[ctx->unreleased_count++] = f->frame_id;
    }
    frame_reset(ctx);
}

/* The frame ends as superseded instead of going on: shutting down, or a newer state waits while the driver
 * refused the frame's work or, once its step was `tried`, the output could not take it either (a frame the
 * driver does not run then waits for one of them). Never while the driver runs it. */
static bool ends(const shr_context *ctx, bool tried) {
    const shr__frame *f = &ctx->frame;
    return f->state != FRAME_ISOLATED && !f->running &&
           (ctx->shutting_down || (ctx->submitted && (tried || f->refused)));
}

static void progress(shr_context *ctx) {
    shr__frame *f = &ctx->frame;
    shr_status st = SHR_OK;
    if (f->state == FRAME_ISOLATED) {
        if (fence_state(ctx) != SHR_FENCE_PENDING) frame_drop(ctx);
        return;
    }
    if (f->state == FRAME_RASTER && !ends(ctx, false)) {
        int r = run(ctx, f, &st);
        if (r < 0) frame_fail(ctx, st);
        if (r > 0) raster_complete(ctx, f);
    }
    if (f->state == FRAME_CONVERT && !ends(ctx, false)) convert(ctx, f);
    if (f->state == FRAME_PRESENT && !ends(ctx, false)) present(ctx, f);
    if (ends(ctx, true)) shr__frame_abandon(ctx);
}

/* Automatic frames (blink, redraws, resolved fallbacks) wait while layer changes are not submitted. */
static bool auto_allowed(const shr_context *ctx) {
    return ctx->has_submitted && !ctx->failed && !ctx->staged.full && !ctx->staged.rects.len;
}

static bool auto_due(const shr_context *ctx) {
    return auto_allowed(ctx) &&
           (ctx->stale || (shr__blink_any(ctx) && shr__blink_visible(ctx, NULL) != ctx->blink_shown));
}

static bool can_start(const shr_context *ctx) {
    return ctx->frame.state == FRAME_IDLE && ctx->configured && !ctx->shutting_down && !ctx->output_isolated &&
           ctx->event_count + shr__events_reserved(ctx) + 2 <= ctx->event_cap &&
           (ctx->composing || (!ctx->output_blocked && ctx->unreleased_count < ctx->desc.max_unreleased_frames));
}

/* Pixels drawn with a fallback may resolve differently now. */
static void provisional_changed(shr_context *ctx) {
    ctx->failed = false;
    for (int i = 0; i < SHR_TARGETS; i++) {
        shr__target *t = &ctx->targets[i];
        for (size_t k = 0; t->used && k < t->provisional.len; k++) {
            shr__damage_add(ctx, &t->damage, *SHR_VEC_AT(&t->provisional, shr_rect, k));
            ctx->stale = true;
        }
        t->provisional.len = 0;
    }
    if (ctx->last_provisional) ctx->stale = true;
    ctx->last_provisional = false;
    if (ctx->frame.state == FRAME_RASTER) ctx->frame.prov_stale = true;
}

shr_status shr_submit(shr_context *ctx) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    shr_status first = SHR_OK;
    for (shr_lyr *l = ctx->layers; l; l = l->next) {
        shr_status st = l->flush ? l->flush(l->state) : SHR_OK;
        if (first == SHR_OK) first = st;
    }
    for (int i = 0; i < SHR_TARGETS; i++) {
        shr__damage *d = &ctx->targets[i].damage;
        if (!ctx->targets[i].used) continue;
        d->full |= ctx->staged.full;
        for (size_t k = 0; k < ctx->staged.rects.len; k++)
            shr__damage_add(ctx, d, *SHR_VEC_AT(&ctx->staged.rects, shr_rect, k));
    }
    ctx->staged.rects.len = 0;
    ctx->staged.full = false;
    ctx->submitted = ctx->has_submitted = true;
    ctx->failed = false;
    if (ctx->blink.restart == SHR_BLINK_RESTART_ON_SUBMIT && ctx->desc.now_ns) {
        ctx->blink.epoch_ns = shr__ctx_now(ctx);
        ctx->blink.start_visible = true;
    }
    shr__ctx_trace(ctx, SHR_TRACE_SUBMIT, ctx->next_frame_id, 0, 0);
    return first;
}

shr_status shr_pump(shr_context *ctx) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    bool changed = false;
    for (int round = 0; round < 64; round++) {
        bool delivered = shr__io_pump(ctx);
        for (shr__res *r = ctx->resources; r; r = r->next)
            if (r->ops->pump && r->ops->pump(r)) changed = true;
        if (!delivered) break;
    }
    if (changed) provisional_changed(ctx);
    progress(ctx);
    if (can_start(ctx) && (ctx->submitted || auto_due(ctx))) {
        start_frame(ctx);
        progress(ctx);
    }
    shr__res_collect(ctx);
    return SHR_OK;
}

shr_status shr_next_deadline(const shr_context *ctx, shr_deadline *out) {
    if (!ctx || !out) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    const shr_deadline now = {SHR_DEADLINE_NOW, 0};
    const shr__frame *f = &ctx->frame;
    *out = (shr_deadline){SHR_DEADLINE_NONE, 0};
    bool out_ok = !ctx->output_blocked && !ctx->output_isolated;
    uint64_t at = shr__io_deadline(ctx), t;
    for (const shr__res *r = ctx->resources; r; r = r->next) {
        if (r->ops->has_work && r->ops->has_work(r)) return *out = now, SHR_OK;
        if (r->ops->deadline && (t = r->ops->deadline(r)) && (!at || t < at)) at = t;
    }
    if (shr__io_ready(ctx) ||
        ((f->running || f->state == FRAME_ISOLATED) && fence_state(ctx) != SHR_FENCE_PENDING) ||
        (f->state != FRAME_IDLE && ends(ctx, true)) || submission_ends(ctx) || (f->state == FRAME_PRESENT && out_ok) ||
        (f->state == FRAME_CONVERT && !f->has_output && out_ok &&
         ctx->unreleased_count < ctx->desc.max_unreleased_frames) ||
        (can_start(ctx) && (ctx->submitted || auto_due(ctx))) ||
        (f->refused && atomic_load(&ctx->driver_ready) != f->ready_seen))
        return *out = now, SHR_OK;
    /* The driver watchdog, or the retry of a refused submission. */
    if (((f->running && ctx->driver.caps.timeout_ns) || (f->refused && ctx->desc.io_retry_ns)) &&
        (!at || f->deadline < at))
        at = f->deadline;
    if (auto_allowed(ctx) && !ctx->shutting_down && shr__blink_any(ctx) && (shr__blink_visible(ctx, &t), t) &&
        (!at || t < at))
        at = t;
    if (at && ctx->desc.now_ns) *out = at <= shr__ctx_now(ctx) ? now : (shr_deadline){SHR_DEADLINE_AT, at};
    return SHR_OK;
}
