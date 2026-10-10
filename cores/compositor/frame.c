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
    f->cmds.len = f->damage.len = f->provisional.len = f->batches.len = f->kept.len = f->kept_at = 0;
    f->batch = f->prologue_at = f->pushed = 0;
    f->band_y = f->oy = 0, f->band_k = 0;
    f->synced = 0;
    f->nmoves = 0;
    f->taken = f->short_bufs = 0;
    f->state = FRAME_IDLE;
    f->has_output = f->built = f->running = f->took_damage = f->prov_stale = false;
    f->refused = false;
    f->target_rec = -1;
}

/* ===== Buffer plan: built with the frame, taken by the registry once the driver accepted it ===== */

/* An id released after plan_begin() (an image freed while the frame is built) stays taken until a plan releases it. */
static bool slot_free(const shr__slot *s, uint64_t frame) {
    return s->stamp == frame ? !s->taken : !s->buf && !s->releasing;
}

/* A buffer command or KEEP_RELEASE of `id`: a REGISTER names the memory of `b`, an UPDATE its dirty rect. */
static shr_status prologue_push(shr_context *ctx, shr__frame *f, shr_cmd_kind kind, uint32_t id, const shr__buf *b) {
    shr_draw_cmd *c = shr__vec_push(&f->prologue, &ctx->al);
    if (!c) return SHR_E_NO_MEMORY;
    c->kind = (uint8_t)kind, c->flags = 0, c->buffer = id;
    if (kind == SHR_CMD_BUFFER_REGISTER)
        c->src = shr__image_ref(b->mem.pixels, b->mem.width, b->mem.height, b->mem.stride, b->mem.format, b->mem.domain);
    if (kind == SHR_CMD_BUFFER_UPDATE) c->src_rect = b->dirty;
    return SHR_OK;
}

static void lru_touch(shr_context *ctx, shr__buf *b) {
    shr__lru_remove(&ctx->lru, &b->lru);
    shr__lru_push(&ctx->lru, &b->lru);
}

/* Pending RELEASEs open the plan, so their ids are free for it. */
static shr_status plan_begin(shr_context *ctx, shr__frame *f, bool submitted) {
    shr_status st = SHR_OK;
    f->resident = ctx->resident;
    for (uint32_t i = 0; st == SHR_OK && i < ctx->nreleased; i++) {
        shr__slot *s = &ctx->slots[ctx->released[i] - 1];
        s->stamp = f->frame_id, s->taken = false;
        f->resident -= s->bytes;
        st = prologue_push(ctx, f, SHR_CMD_BUFFER_RELEASE, ctx->released[i], NULL);
    }
    f->keep_resident = ctx->keep_resident, f->keep_scan = 0, f->kept_bytes = 0;
    shr__keep_count n = f->keep_count;
    f->no_credit = ctx->keep_still && submitted;
    ctx->keep_run = n.direct + n.stores > n.hits ? ctx->keep_run + (ctx->keep_run < 2) : 0;
    ctx->keep_still &= !submitted;
    f->keep_count = (shr__keep_count){0};
    ctx->keep_credit += ctx->keep_credit < ctx->driver.caps.max_keeps;
    for (uint32_t i = 0; st == SHR_OK && i < ctx->nkeep_released; i++) {
        shr__keep *e = &ctx->keeps[ctx->keep_released[i] - 1];
        e->stamp = f->frame_id, e->plan = KEEP_FREED;
        st = prologue_push(ctx, f, SHR_CMD_KEEP_RELEASE, ctx->keep_released[i], NULL);
    }
    return st;
}

/* A free id for `b`, evicting registered buffers the frame does not draw from, least recently used first.
 * Touched buffers move to the front of the LRU, so the oldest one is untouched unless none is. SHR_E_WOULD_BLOCK: the
 * frame's buffers hold every id or byte; SHR_E_LIMIT: `b` alone takes more bytes than the driver holds. */
static shr_status plan_take(shr_context *ctx, shr__frame *f, shr__buf *b) {
    const shr_driver_caps *k = &ctx->driver.caps;
    uint64_t budget = (k->buffer_flags & SHR_BUFFER_COPIES) && k->buffer_bytes ? k->buffer_bytes : UINT64_MAX;
    uint32_t id = 0;
    if (b->mem.byte_length > budget) return SHR_E_LIMIT;
    for (uint32_t i = 0; i < k->max_buffers && !id; i++)
        if (slot_free(&ctx->slots[i], f->frame_id)) id = i + 1;
    while (!id || f->resident + b->mem.byte_length > budget) {
        shr__lru_node *n = shr__lru_oldest(ctx->lru);
        shr__buf *v = n ? SHR_CONTAINER(n, shr__buf, lru) : NULL;
        shr__slot *s = v ? &ctx->slots[v->id - 1] : NULL;
        if (!s || s->stamp == f->frame_id) return SHR_E_WOULD_BLOCK;
        shr_status st = prologue_push(ctx, f, SHR_CMD_BUFFER_RELEASE, v->id, NULL);
        if (st != SHR_OK) return st;
        s->stamp = f->frame_id, s->taken = false;
        f->resident -= s->bytes;
        lru_touch(ctx, v);
        if (!id) id = v->id;
    }
    ctx->slots[id - 1].stamp = f->frame_id, ctx->slots[id - 1].taken = true;
    f->taken++;
    f->resident += b->mem.byte_length;
    b->id = id;
    return prologue_push(ctx, f, SHR_CMD_BUFFER_REGISTER, id, b);
}

/* From the first use in a frame on, b->id is the buffer's id in the frame's plan. A buffer with a fallback (`spare`)
 * leaves the frame's last id to the others, so the fallback can be drawn. SHR_E_WOULD_BLOCK: no id for `b` in the
 * frame, nor in the plan. */
static shr_status frame_use(shr_context *ctx, shr__frame *f, shr__buf *b, bool spare) {
    if (b->used == f->frame_id) return SHR_OK;
    if (spare && f->taken + 1 >= ctx->driver.caps.max_buffers) return SHR_E_WOULD_BLOCK;
    shr__planned *p = shr__vec_push(&f->planned, &ctx->al);
    if (!p) return SHR_E_NO_MEMORY;
    *p = (shr__planned){b, b->id};
    uint64_t used = b->used;
    b->used = f->frame_id;
    shr__slot *s = b->id ? &ctx->slots[b->id - 1] : NULL;
    if (!s || s->stamp == f->frame_id) { /* new, or evicted earlier in this plan */
        shr_status st = plan_take(ctx, f, b);
        if (st == SHR_E_WOULD_BLOCK) f->planned.len--, b->used = used;
        return st;
    }
    s->stamp = f->frame_id, s->taken = true;
    f->taken++;
    lru_touch(ctx, b);
    if (!s->known) return prologue_push(ctx, f, SHR_CMD_BUFFER_REGISTER, b->id, b);
    if (shr__rect_empty(b->dirty)) return SHR_OK;
    return prologue_push(ctx, f, SHR_CMD_BUFFER_UPDATE, b->id, b);
}

/* The step's prologue leads its first batch; with none it waits for the next step. */
static shr_status plan_place(shr_context *ctx, shr__frame *f) {
    size_t n = f->prologue.len - f->prologue_at;
    if (!n || !f->batches.len) return SHR_OK;
    if (!shr__vec_reserve(&f->cmds, &ctx->al, n)) return SHR_E_NO_MEMORY;
    shr_draw_cmd *c = f->cmds.data;
    memmove(c + n, c, f->cmds.len * sizeof(*c));
    memcpy(c, SHR_VEC_AT(&f->prologue, shr_draw_cmd, f->prologue_at), n * sizeof(*c));
    f->cmds.len += n;
    shr__batch *b = f->batches.data;
    for (size_t i = 1; i < f->batches.len; i++) b[i].at += n;
    b[0].n += n;
    return SHR_OK;
}

/* ===== Keep plan: what a frame stores, draws and evicts, marked with its id; the keeps the driver holds change
 * only as its batches are accepted ===== */

#define KEEP_HIDDEN 0x9E3779B97F4A7C15ull /* mixed into the key of a group drawn in the blink phase that hides */
#define KEEP_STORES 8                      /* per frame, whatever their bytes */

static void keep_touch(shr_context *ctx, shr__keep *e) {
    shr__lru_remove(&ctx->keep_lru, &e->lru);
    shr__lru_push(&ctx->keep_lru, &e->lru);
}

/* The driver holds nothing under the id of `e` any more. */
static void keep_forget(shr_context *ctx, shr__keep *e) {
    if (!e->bytes) return;
    uint32_t id = (uint32_t)(e - ctx->keeps) + 1, *p = &ctx->keep_heads[e->key.hash[0] & ctx->keep_mask];
    while (*p != id) p = &ctx->keeps[*p - 1].next;
    *p = e->next;
    shr__lru_remove(&ctx->keep_lru, &e->lru);
    ctx->keep_resident -= e->bytes;
    e->bytes = 0;
}

/* The keep holding `key` for the frame: stored by its plan, or held and not freed by it (then pinned). */
static uint32_t keep_find(shr_context *ctx, shr__frame *f, const shr__keep_key *key) {
    for (size_t i = 0; i < f->kept.len; i++) {
        const shr__kept *p = SHR_VEC_AT(&f->kept, shr__kept, i);
        if (!memcmp(&p->key, key, sizeof(*key))) return p->id;
    }
    for (uint32_t id = ctx->keep_heads[key->hash[0] & ctx->keep_mask]; id; id = ctx->keeps[id - 1].next) {
        shr__keep *e = &ctx->keeps[id - 1];
        if (memcmp(&e->key, key, sizeof(*key))) continue;
        if (e->stamp == f->frame_id && e->plan != KEEP_PINNED) return 0;
        uint32_t max = ctx->driver.caps.max_keeps;
        if (e->borrowed && !e->drawn) ctx->keep_credit = max - ctx->keep_credit < 2 ? max : ctx->keep_credit + 2;
        e->stamp = f->frame_id, e->plan = KEEP_PINNED, e->drawn = true;
        keep_touch(ctx, e);
        return id;
    }
    return 0;
}

void shr__keep_moved(shr_context *ctx, const shr__group *g) {
    const shr__lcmd c = shr__group_cmd(g, 0);
    if (c.kind != SHR__LCMD_CACHE_BEGIN) return;
    for (int hidden = 0; hidden <= !shr__rect_empty(g->blink); hidden++) {
        shr__keep_key key = {{c.key[0], c.key[1] ^ (hidden ? KEEP_HIDDEN : 0)}, c.dst.x1 - c.dst.x0,
                             c.dst.y1 - c.dst.y0};
        uint32_t id = ctx->keep_heads[key.hash[0] & ctx->keep_mask];
        while (id && memcmp(&ctx->keeps[id - 1].key, &key, sizeof(key))) id = ctx->keeps[id - 1].next;
        if (id) keep_touch(ctx, &ctx->keeps[id - 1]);
    }
}

static uint64_t keep_bytes(const shr__keep_key *key) {
    size_t row;
    shr_format_row_bytes(SHR_PIXEL_FORMAT, key->w, &row);
    return (uint64_t)row * (uint64_t)key->h;
}

/* Store policy: a group no keep holds is stored, whole though the damage covers part of it, at second sight: once an
 * earlier frame drew it plainly; the first KEEP_STORES of a frame, then while its stores fit in keep_store_bytes (1).
 * The older of a key's two places takes it. With SHR_DRIVER_CHEAP_STORE any other group, also at first sight, is
 * stored on credit (2) in the first two of frames in a row that draw or store more keep groups than they draw from
 * keeps, and not in the first frame submitted after a pump that found nothing to do: one each, counted among the
 * frame's stores; one comes back at every frame and two when a later frame first draws a keep stored on credit. */
static int keep_now(shr_context *ctx, shr__frame *f, const shr__keep_key *key) {
    shr__seen *a = &ctx->seen[key->hash[0] & ctx->seen_mask], *b = &ctx->seen[key->hash[0] >> 32 & ctx->seen_mask];
    uint32_t print = (uint32_t)(key->hash[1] ^ key->hash[1] >> 32) | 1u, now = (uint32_t)f->frame_id;
    shr__seen *e = b->print == print || (a->print != print && b->frame < a->frame) ? b : a;
    bool seen = e->print == print && e->frame != now;
    *e = (shr__seen){print, now};
    if (seen && (f->kept.len < KEEP_STORES || f->kept_bytes + keep_bytes(key) <= ctx->keep_store_bytes)) return 1;
    return (ctx->driver.caps.flags & SHR_DRIVER_CHEAP_STORE) && ctx->keep_credit && ctx->keep_run < 2 && !f->no_credit
               ? 2
               : 0;
}

/* A held keep the plan neither draws nor replaces after all. */
static void keep_unreplace(shr__frame *f, shr__keep *e) {
    e->stamp = 0;
    f->keep_resident += e->bytes;
}

/* The held keep a store may give up: the least recently used one the frame does not draw; for a store on credit, one
 * stored on credit that no later frame drew. */
static shr__keep *keep_victim(shr_context *ctx, const shr__frame *f, bool borrowed) {
    for (shr__lru_node *ln = shr__lru_oldest(ctx->keep_lru); ln; ln = ln == ctx->keep_lru ? NULL : ln->prev) {
        shr__keep *v = SHR_CONTAINER(ln, shr__keep, lru);
        if (!borrowed) return v->stamp == f->frame_id ? NULL : v;
        if (v->borrowed && !v->drawn && v->stamp != f->frame_id) return v;
    }
    return NULL;
}

/* An id (*out, 0 = none) for storing `key`: a free one when the bytes allow, else that of a keep keep_victim() gives,
 * which the store replaces without a RELEASE; more such keeps are released while the bytes fall short. A store that
 * does not fit is left out; the frame goes on. */
static shr_status keep_take(shr_context *ctx, shr__frame *f, const shr__keep_key *key, bool borrowed, uint32_t *out) {
    uint64_t bytes = keep_bytes(key);
    uint32_t id = 0, n = ctx->driver.caps.max_keeps;
    shr__keep *replaced = NULL;
    *out = 0;
    if (bytes > ctx->keep_budget || (ctx->driver.caps.max_keep_bytes && bytes > ctx->driver.caps.max_keep_bytes))
        return SHR_OK;
    if (f->keep_resident + bytes <= ctx->keep_budget) {
        for (uint32_t i = f->keep_scan; i < n && !id; i++) {
            const shr__keep *e = &ctx->keeps[i];
            if (e->stamp == f->frame_id ? e->plan == KEEP_FREED : !e->bytes) id = i + 1;
        }
        f->keep_scan = id ? id : n; /* ids freed below it later in the plan wait a frame */
    }
    while (!id || f->keep_resident + bytes > ctx->keep_budget) {
        shr__keep *v = keep_victim(ctx, f, borrowed);
        if (!v) {
            if (replaced) keep_unreplace(f, replaced);
            f->keep_count.refused++;
            return SHR_OK;
        }
        uint32_t vid = (uint32_t)(v - ctx->keeps) + 1;
        shr_status st = id ? prologue_push(ctx, f, SHR_CMD_KEEP_RELEASE, vid, NULL) : SHR_OK;
        if (st != SHR_OK) return st;
        v->stamp = f->frame_id, v->plan = KEEP_FREED;
        f->keep_resident -= v->bytes;
        keep_touch(ctx, v);
        if (!id) id = vid, replaced = v;
    }
    shr__kept *p = shr__vec_push(&f->kept, &ctx->al);
    if (!p) return SHR_E_NO_MEMORY;
    *p = (shr__kept){id, *key, bytes, replaced != NULL, borrowed};
    ctx->keep_credit -= borrowed;
    ctx->keeps[id - 1].stamp = f->frame_id, ctx->keeps[id - 1].plan = KEEP_STORED;
    f->keep_resident += bytes, f->kept_bytes += bytes;
    *out = id;
    return SHR_OK;
}

/* Gives back the id of the last store taken. */
static void keep_untake(shr_context *ctx, shr__frame *f) {
    const shr__kept *p = SHR_VEC_AT(&f->kept, shr__kept, --f->kept.len);
    shr__keep *e = &ctx->keeps[p->id - 1];
    f->keep_resident -= p->bytes, f->kept_bytes -= p->bytes;
    ctx->keep_credit += p->borrowed;
    if (p->replaces) keep_unreplace(f, e);
    else e->plan = KEEP_FREED;
}

/* A keep given up: dead when no frame after the one storing it drew it. */
static void keep_drop(shr_context *ctx, shr__frame *f, shr__keep *e) {
    f->keep_count.dead += e->bytes && !e->drawn;
    keep_forget(ctx, e);
}

/* Batch f->batch was accepted: the driver holds what it stored, the stores not committed yet (the stores of a step
 * all lead its first batch, which draws). */
static void keep_commit(shr_context *ctx, shr__frame *f) {
    for (; f->kept_at < f->kept.len; f->kept_at++) {
        const shr__kept *p = SHR_VEC_AT(&f->kept, shr__kept, f->kept_at);
        shr__keep *e = &ctx->keeps[p->id - 1];
        uint32_t *head = &ctx->keep_heads[p->key.hash[0] & ctx->keep_mask];
        keep_drop(ctx, f, e); /* what the store replaced */
        f->keep_count.stores++;
        e->key = p->key, e->bytes = p->bytes, e->drawn = false, e->borrowed = p->borrowed;
        e->next = *head, *head = p->id;
        shr__lru_push(&ctx->keep_lru, &e->lru);
        ctx->keep_resident += p->bytes;
    }
}

/* The prologue of the step ran. Releases first: a buffer may take an id the plan released. A released id stays marked
 * for the frame, so no later step evicts a buffer an earlier one drew from. */
static void plan_commit(shr_context *ctx, shr__frame *f) {
    for (size_t i = f->prologue_at; i < f->prologue.len; i++) {
        const shr_draw_cmd *c = SHR_VEC_AT(&f->prologue, shr_draw_cmd, i);
        if (c->kind == SHR_CMD_KEEP_RELEASE) {
            shr__keep *e = &ctx->keeps[c->buffer - 1];
            keep_drop(ctx, f, e);
            e->releasing = false;
        }
        if (c->kind != SHR_CMD_BUFFER_RELEASE) continue;
        shr__slot *s = &ctx->slots[c->buffer - 1];
        if (s->buf && s->buf->used != f->frame_id) { /* not freed meanwhile, nor drawn after its eviction */
            s->buf->id = 0;
            shr__lru_remove(&ctx->lru, &s->buf->lru);
        }
        ctx->resident -= s->bytes;
        *s = (shr__slot){.stamp = s->stamp, .taken = s->taken};
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
    ctx->nkeep_released = 0; /* all in the prologue */
    f->planned.len = 0; /* the prologue stays until the raster ends, for batch_lost() */
    f->prologue_at = f->prologue.len;
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

/* A batch failed or timed out: which ids the driver holds is unspecified. Every buffer registers again before it is
 * drawn, and each id the batch registered or released is given up and released first in the next prologue, so a
 * driver keeping copies drops what it may still hold there; so is every keep id the driver may hold. Out of memory
 * for the keeps a batch stores, keeps take no more than they did before it, for the context's life. */
static void batch_lost(shr_context *ctx, shr__frame *f, shr_status st) {
    if (st == SHR_E_NO_MEMORY && f->kept_at < f->kept.len) ctx->keep_budget = ctx->keep_resident;
    for (uint32_t i = 0; i < ctx->driver.caps.max_keeps; i++) {
        shr__keep *e = &ctx->keeps[i];
        if (!e->bytes && (e->stamp != f->frame_id || e->plan != KEEP_STORED)) continue;
        keep_forget(ctx, e);
        if (!e->releasing) e->releasing = true, ctx->keep_released[ctx->nkeep_released++] = i + 1;
    }
    plan_restore(f);
    for (uint32_t i = 0; i < ctx->driver.caps.max_buffers; i++) ctx->slots[i].known = false;
    for (size_t i = 0; i < f->prologue.len; i++) {
        const shr_draw_cmd *c = SHR_VEC_AT(&f->prologue, shr_draw_cmd, i);
        if (c->kind == SHR_CMD_BUFFER_UPDATE || c->kind == SHR_CMD_KEEP_RELEASE) continue;
        shr__slot *s = &ctx->slots[c->buffer - 1];
        if (s->releasing) continue;
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
        shr__moves_drop(ctx, &t->damage); /* moves made since the frame began would move what it left */
        for (size_t i = 0; t->used && i < f->damage.len; i++)
            shr__damage_add(ctx, &t->damage, *SHR_VEC_AT(&f->damage, shr_rect, i));
        for (uint32_t i = 0; i < f->nmoves; i++) shr__damage_add(ctx, &t->damage, f->moves[i].area);
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
    t->damage.nmoves = 0;
    t->provisional.len = 0;
    return pick;
}

/* Moves the target's damage (and its provisional pixels) and moves into the frame; beyond 3/4 of the screen the
 * whole screen is drawn, without moves. On failure the target is forgotten, so the next frame redraws it whole. */
static bool take_damage(shr_context *ctx, shr__frame *f) {
    shr__target *t = f->target_rec >= 0 ? &ctx->targets[f->target_rec] : NULL;
    shr_rect full = {0, 0, ctx->screen.width, ctx->screen.height};
    f->damage.len = 0;
    shr__damage d = {.rects = f->damage,
                     .full = !t || !t->used || t->generation != f->target.generation || t->damage.full};
    if (!d.full) {
        d.rects = t->damage.rects;
        t->damage.rects = f->damage;
        f->nmoves = t->damage.nmoves;
        memcpy(f->moves, t->damage.moves, sizeof(f->moves));
        for (size_t i = 0; i < t->provisional.len; i++)
            shr__damage_add(ctx, &d, *SHR_VEC_AT(&t->provisional, shr_rect, i));
    }
    int64_t area = 0;
    for (size_t i = 0; i < d.rects.len; i++) area += shr__rect_area(*SHR_VEC_AT(&d.rects, shr_rect, i));
    if (d.full || area * 4 > shr__rect_area(full) * 3) {
        d.rects.len = 0;
        f->nmoves = 0;
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
                           {.rects = t->damage.rects}, t->provisional};
        t->damage.rects.len = t->provisional.len = 0;
    }
    f->took_damage = true;
    return true;
}

/* A command whose fields the caller sets: those its kind reads. */
static shr_draw_cmd *push_cmd(shr_context *ctx, shr__frame *f, shr_status *st) {
    if (f->pushed >= ctx->desc.max_commands) {
        *st = SHR_E_LIMIT;
        return NULL;
    }
    if (f->cmds.len == f->cmds.cap && !shr__vec_reserve(&f->cmds, &ctx->al, 1)) {
        *st = SHR_E_NO_MEMORY;
        return NULL;
    }
    f->pushed++;
    return SHR_VEC_AT(&f->cmds, shr_draw_cmd, f->cmds.len++);
}

/* Screen rect `r` in the destination: the band being built, else the screen. */
static inline shr_rect band_rect(const shr__frame *f, shr_rect r) {
    return (shr_rect){r.x0, r.y0 - f->oy, r.x1, r.y1 - f->oy};
}

/* Pixels of one GLYPH/IMAGE command inside `clip`; *provisional is set when a fallback was drawn. A buffer that gets
 * no id in the frame draws the resource's fallback, else nothing, and is drawn again once the ids are free. */
static shr_status emit_resolved(shr_context *ctx, shr__frame *f, shr_point o, const shr__lcmd *lc, shr_rect clip,
                                bool *provisional) {
    const shr__resolved *r;
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
    const shr__res_ops *ops = lc->res->ops;
    const bool glyph = lc->kind == SHR__LCMD_GLYPH;
    for (bool spare = ops->fallback && !r->provisional;; spare = false) {
        uint32_t syn = glyph ? lc->flags & r->synth & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC) : 0;
        int32_t w = r->rect.x1 - r->rect.x0, h = r->rect.y1 - r->rect.y0;
        if (r->scale_w) w = r->scale_w, h = r->scale_h;
        int32_t x0 = 0, x1 = w;
        if (syn) shr__glyph_footprint(w, h, syn, r->slant_axis, &x0, &x1);
        int32_t x, y;
        shr_rect full;
        bool far = __builtin_add_overflow(o.x, lc->anchor.x, &x);
        far |= __builtin_add_overflow(x, r->offset.x, &x);
        far |= __builtin_add_overflow(o.y, lc->anchor.y, &y);
        far |= __builtin_add_overflow(y, r->offset.y, &y);
        far |= __builtin_add_overflow(x, x0, &full.x0);
        far |= __builtin_add_overflow(x, x1, &full.x1);
        far |= __builtin_add_overflow(y, h, &full.y1);
        full.y0 = y;
        int64_t fx = x, fy = y;
        if (__builtin_expect(far, 0)) {
            fx = (int64_t)o.x + lc->anchor.x + r->offset.x, fy = (int64_t)o.y + lc->anchor.y + r->offset.y;
            full = (shr_rect){shr__clamp32(fx + x0), shr__clamp32(fy), shr__clamp32(fx + x1), shr__clamp32(fy + h)};
        }
        shr_rect d = shr__rect_intersect(full, clip);
        if (shr__rect_empty(d)) return SHR_OK;
        *provisional = r->provisional;
        if ((st = frame_use(ctx, f, r->buf, spare)) == SHR_E_WOULD_BLOCK) {
            if (r->buf->refused != f->frame_id) r->buf->refused = f->frame_id, f->short_bufs++;
            *provisional = true;
            if (!spare || (st = ops->fallback(lc->res, lc->id, f->frame_id, &r)) == SHR_E_NOT_FOUND) return SHR_OK;
            if (st != SHR_OK) return st;
            continue;
        }
        if (st != SHR_OK) return st;
        shr_draw_cmd *c = push_cmd(ctx, f, &st);
        if (!c) return st;
        c->kind = glyph ? SHR_CMD_GLYPH : SHR_CMD_IMAGE;
        c->flags = (uint16_t)(glyph ? (lc->flags & (SHR_GLYPH_DIM | SHR_GLYPH_ON_FILL)) | syn : 0);
        c->buffer = r->buf->id;
        c->dst = band_rect(f, d);
        c->src_origin = (shr_point){(int32_t)(d.x0 - fx), (int32_t)(d.y0 - fy)};
        c->color = lc->color, c->bg = lc->bg, c->src_rect = r->rect;
        c->slant_axis = syn & SHR_GLYPH_ITALIC ? r->slant_axis : 0;
        if (r->scale_w) c->flags = SHR_IMAGE_SCALED, c->scale_w = r->scale_w, c->scale_h = r->scale_h;
        return SHR_OK;
    }
}

static shr_status keep_draw(shr_context *ctx, shr__frame *f, uint32_t id, shr_rect dst, shr_rect part) {
    shr_status st = SHR_OK;
    shr_draw_cmd *c = push_cmd(ctx, f, &st);
    if (c) {
        c->kind = SHR_CMD_KEEP_DRAW, c->flags = 0, c->buffer = id, c->dst = band_rect(f, part);
        c->src_origin = (shr_point){part.x0 - dst.x0, part.y0 - dst.y0};
    }
    return st;
}

/* One group inside `clip`. A keep group (a CACHE_BEGIN/END pair of the layer) inside its layer and the screen is drawn
 * from a keep, stored first as the store policy says when its pixels are final; else its commands draw like the
 * others. All commands of a group share one clip, so an ON_FILL glyph stays inside its FILL. */
static shr_status emit_group(shr_context *ctx, shr__frame *f, const shr_lyr *l, const shr__group *g, shr_rect clip) {
    const bool compact = g->compact;
    shr_status st = SHR_OK;
    shr_rect screen = {0, 0, ctx->screen.width, ctx->screen.height}, keep = {0, 0, 0, 0}, part = keep;
    size_t begin_cmd = 0, begin_at = 0;
    uint32_t id = 0; /* the keep being stored */
    shr_draw_cmd *c;
    const shr_point o = shr__group_at(l, g);
    for (size_t i = 0; i < g->n; i++) {
        if (!id && i % SHR__BLOCK == 0 &&
            shr__rect_empty(shr__rect_intersect(shr__rect_move(g->blocks[i / SHR__BLOCK], o.x, o.y), clip))) {
            i += SHR__BLOCK - 1;
            continue;
        }
        const shr__rcmd *rc = compact ? &g->rows[i] : NULL;
        const shr__lcmd *lc = compact ? NULL : &g->cmds[i];
        uint32_t kind = compact ? rc->kind : lc->kind, flags = compact ? rc->flags : lc->flags;
        shr_rect dst = shr__rect_move(compact ? shr__rcmd_dst(rc) : lc->dst, o.x, o.y);
        if (kind == SHR__LCMD_CACHE_BEGIN) {
            part = shr__rect_intersect(dst, clip);
            shr_rect inside = shr__rect_intersect(dst, shr__rect_intersect(l->rect, screen));
            if (shr__rect_empty(part) || !ctx->driver.caps.max_keeps || memcmp(&inside, &dst, sizeof(dst))) continue;
            const uint64_t *h = compact ? g->key : lc->key;
            shr__keep_key key = {{h[0], h[1] ^ (!shr__rect_empty(g->blink) && !f->blink_visible ? KEEP_HIDDEN : 0)},
                                 dst.x1 - dst.x0, dst.y1 - dst.y0};
            uint32_t k = keep_find(ctx, f, &key);
            if (k) {
                f->keep_count.hits++;
                if ((st = keep_draw(ctx, f, k, dst, part)) != SHR_OK) return st;
                i += compact ? g->n - 1 - i : lc->end;
                continue;
            }
            int now = keep_now(ctx, f, &key);
            if (now && (st = keep_take(ctx, f, &key, now == 2, &id)) != SHR_OK) return st;
            f->keep_count.direct += !id;
            if (!id) continue;
            if (!(c = push_cmd(ctx, f, &st))) return st;
            c->kind = SHR_CMD_KEEP_BEGIN, c->flags = 0, c->buffer = id, c->dst = band_rect(f, dst);
            keep = dst, begin_cmd = f->cmds.len - 1, begin_at = i;
            continue;
        }
        if (kind == SHR__LCMD_CACHE_END) {
            if (!id) continue;
            if (!(c = push_cmd(ctx, f, &st))) return st;
            c->kind = SHR_CMD_KEEP_END, c->flags = 0;
            if ((st = keep_draw(ctx, f, id, keep, part)) != SHR_OK) return st;
            id = 0;
            continue;
        }
        shr_rect d = shr__rect_intersect(dst, id ? keep : clip);
        if (shr__rect_empty(d) || ((flags & SHR__LCMD_BLINK) && !f->blink_visible)) continue;
        bool provisional = false;
        if (kind == SHR__LCMD_FILL) {
            if (!(c = push_cmd(ctx, f, &st))) return st;
            c->kind = SHR_CMD_FILL, c->flags = flags & SHR_GLYPH_DIM, c->dst = band_rect(f, d);
            c->color = compact ? rc->color : lc->color;
        } else if (kind == SHR__LCMD_LINE) {
            if (!(c = push_cmd(ctx, f, &st))) return st;
            c->kind = SHR_CMD_LINE, c->flags = (uint16_t)((flags & SHR_GLYPH_DIM) | rc->id << SHR_LINE_SHAPE_SHIFT);
            c->dst = band_rect(f, d), c->color = rc->color;
            c->src_origin = (shr_point){(d.x0 - dst.x0) % SHR_CELL_WIDTH, d.y0 - dst.y0};
            c->src_rect = (shr_rect){0, 0, SHR_CELL_WIDTH, dst.y1 - dst.y0};
        } else {
            shr__lcmd row; /* what emit_resolved reads */
            if (compact)
                row.kind = rc->kind, row.flags = rc->flags, row.color = rc->color, row.bg = rc->bg, row.id = rc->id,
                row.res = g->res, row.anchor = (shr_point){rc->x0 * SHR_CELL_WIDTH, rc->y0};
            if ((st = emit_resolved(ctx, f, o, compact ? &row : lc, d, &provisional)) != SHR_OK) return st;
        }
        if (!provisional) continue;
        if (id) { /* fallback pixels are not kept: the group again, drawn like the others */
            f->pushed -= f->cmds.len - begin_cmd;
            f->cmds.len = begin_cmd;
            f->keep_count.direct++;
            keep_untake(ctx, f);
            id = 0;
            i = begin_at;
            continue;
        }
        shr_rect *p = f->provisional.len ? SHR_VEC_AT(&f->provisional, shr_rect, f->provisional.len - 1) : NULL;
        shr_rect u = p ? shr__rect_union(*p, d) : d;
        if (p && shr__rect_area(u) + shr__rect_area(shr__rect_intersect(*p, d)) ==
                     shr__rect_area(*p) + shr__rect_area(d)) {
            *p = u; /* the two make a rectangle, as neighbouring cells of a row do */
            continue;
        }
        if (!(p = shr__vec_push(&f->provisional, &ctx->al))) return SHR_E_NO_MEMORY;
        *p = d;
    }
    return SHR_OK;
}

bool shr__hides(const shr_lyr *l, shr_rect r) {
    int32_t y = r.y0;
    for (size_t i = 0; l->visible && i < l->groups.len && y < r.y1; i++) {
        const shr__group *g = SHR_VEC_AT(&l->groups, shr__group, i);
        shr_point at = shr__group_at(l, g);
        shr_rect o = shr__rect_move(g->opaque, at.x, at.y);
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
        if (shr__hides(l, rect)) from = l;
    if (!from) {
        shr_draw_cmd *c = push_cmd(ctx, f, &st);
        if (!c) return st;
        c->kind = SHR_CMD_FILL, c->flags = 0, c->dst = band_rect(f, rect), c->color = ctx->screen.clear;
        from = ctx->layers;
    }
    for (const shr_lyr *l = from; l; l = l->next) {
        shr_rect clip = shr__rect_intersect(rect, l->rect);
        if (!l->visible || shr__rect_empty(clip)) continue;
        for (size_t i = 0; i < l->groups.len; i++) {
            const shr__group *g = SHR_VEC_AT(&l->groups, shr__group, i);
            shr_point at = shr__group_at(l, g);
            if (shr__rect_empty(shr__rect_intersect(shr__rect_move(g->bounds, at.x, at.y), clip))) continue;
            if ((st = emit_group(ctx, f, l, g, clip)) != SHR_OK) return st;
        }
    }
    return SHR_OK;
}

/* The output rect logical rect `r` turns into. */
static shr_rect out_rect(const shr_screen_desc *sd, shr_rect r) {
    shr_point p, q;
    shr_rotation_map_point(sd->rotation, sd->width, sd->height, (shr_point){r.x0, r.y0}, false, &p);
    shr_rotation_map_point(sd->rotation, sd->width, sd->height, (shr_point){r.x1 - 1, r.y1 - 1}, false, &q);
    return (shr_rect){p.x < q.x ? p.x : q.x, p.y < q.y ? p.y : q.y, (p.x > q.x ? p.x : q.x) + 1, (p.y > q.y ? p.y : q.y) + 1};
}

/* `r` is empty or inside one damage rect. */
static bool covered(const shr__frame *f, shr_rect r) {
    bool in = shr__rect_empty(r);
    for (size_t i = 0; i < f->damage.len; i++) {
        shr_rect q = *SHR_VEC_AT(&f->damage, shr_rect, i);
        in |= (q.x0 <= r.x0) & (q.y0 <= r.y0) & (q.x1 >= r.x1) & (q.y1 >= r.y1);
    }
    return in;
}

/* The moves as COPYs of the target onto itself ahead of everything else (with bands in output coordinates). A lone
 * move whose destination leaves only damage outside it leaves those pixels to the driver. */
static shr_status emit_moves(shr_context *ctx, shr__frame *f) {
    const shr_screen_desc *sd = &ctx->screen;
    const shr_surface *t = &f->target;
    shr_status st = SHR_OK;
    for (uint32_t i = 0; i < f->nmoves; i++) {
        shr__move m = f->moves[i];
        shr_rect d = shr__rect_intersect(m.area, shr__rect_move(m.area, 0, m.dy)), s = shr__rect_move(d, 0, -m.dy);
        shr_rect strips[4] = {{0, 0, sd->width, d.y0}, {0, d.y1, sd->width, sd->height}, {0, d.y0, d.x0, d.y1},
                              {d.x1, d.y0, sd->width, d.y1}};
        bool rest = f->nmoves == 1;
        for (int k = 0; k < 4; k++) rest &= covered(f, strips[k]);
        d = ctx->band_count ? out_rect(sd, d) : d, s = ctx->band_count ? out_rect(sd, s) : s;
        shr_draw_cmd *c = push_cmd(ctx, f, &st);
        if (!c) return st;
        c->kind = SHR_CMD_COPY, c->flags = rest ? SHR_COPY_REST_UNDEFINED : 0, c->dst = d;
        c->src = shr__image_ref(t->pixels, t->width, t->height, t->stride, t->format, t->domain);
        c->src_origin = (shr_point){s.x0, s.y0};
    }
    return SHR_OK;
}

/* The next band with damage from row band_y on: its damage, widened to band_align (band edges are multiples of it) and
 * merged where it overlaps or shares an edge, is drawn into the next band buffer moved up by the band's y, then turned
 * into the output by one ROTATE or COPY per rect. None left: no batches. */
static shr_status emit_band(shr_context *ctx, shr__frame *f) {
    const shr_screen_desc *sd = &ctx->screen;
    int32_t h = ctx->bands[0].height, a = sd->band_align ? (int32_t)sd->band_align : 1;
    size_t bpp, obpp;
    shr_format_row_bytes(SHR_PIXEL_FORMAT, 1, &bpp);
    shr_format_row_bytes(sd->output_format, 1, &obpp);
    shr_status st = SHR_OK;
    f->cmds.len = f->batches.len = f->batch = 0;
    for (int32_t y = f->band_y; y < sd->height; y = f->band_y) {
        shr_rect band = {0, y, sd->width, y + h < sd->height ? y + h : sd->height}, reg[SHR_MAX_DAMAGE];
        size_t n = 0;
        f->band_y = y + h;
        for (size_t i = 0; i < f->damage.len; i++) {
            shr_rect r = shr__rect_intersect(*SHR_VEC_AT(&f->damage, shr_rect, i), band);
            if (shr__rect_empty(r)) continue;
            r = (shr_rect){r.x0 / a * a, r.y0 / a * a, (r.x1 + a - 1) / a * a, (r.y1 + a - 1) / a * a};
            for (size_t j = 0; j < n;) {
                shr_rect q = reg[j];
                bool rows = r.y0 == q.y0 && r.y1 == q.y1 && r.x0 <= q.x1 && q.x0 <= r.x1;
                bool cols = r.x0 == q.x0 && r.x1 == q.x1 && r.y0 <= q.y1 && q.y0 <= r.y1;
                if (rows || cols || !shr__rect_empty(shr__rect_intersect(r, q)))
                    r = shr__rect_union(r, q), reg[j] = reg[--n], j = 0;
                else
                    j++;
            }
            reg[n++] = r;
        }
        if (!n) continue;
        if (!shr__vec_reserve(&f->batches, &ctx->al, 2)) return SHR_E_NO_MEMORY;
        /* Layers moved since the frame began, submitted or not: the band draws them moved. */
        shr__moves_drop(ctx, &ctx->staged);
        if (f->target_rec >= 0) shr__moves_drop(ctx, &ctx->targets[f->target_rec].damage);
        const shr_surface *b = &ctx->bands[f->band_k];
        f->oy = y;
        for (size_t j = 0; j < n; j++)
            if ((st = emit_rect(ctx, f, reg[j])) != SHR_OK) return st;
        shr__batch *bt = f->batches.data;
        bt[0] = (shr__batch){0, f->cmds.len, (int)f->band_k};
        for (size_t j = 0; j < n; j++) {
            shr_rect r = reg[j];
            int32_t w = r.x1 - r.x0, rh = r.y1 - r.y0;
            shr_draw_cmd *c = push_cmd(ctx, f, &st);
            if (!c) return st;
            c->kind = sd->rotation ? SHR_CMD_ROTATE : SHR_CMD_COPY, c->flags = 0, c->rotation = (uint8_t)sd->rotation;
            c->dst = out_rect(sd, r), c->src_origin = (shr_point){0, 0};
            c->src = shr__image_ref((const uint8_t *)b->pixels + (size_t)(r.y0 - y) * b->stride + (size_t)r.x0 * bpp, w,
                                    rh, b->stride, b->format, b->domain);
            f->converted += (uint64_t)w * (uint64_t)rh * (bpp + obpp);
        }
        bt[1] = (shr__batch){bt[0].n, f->cmds.len - bt[0].n, -1};
        f->batches.len = 2;
        f->band_k = (f->band_k + 1) % ctx->band_count;
        return plan_place(ctx, f);
    }
    return SHR_OK;
}

/* Commands carry strides in 32 bits. */
bool shr__mem_reaches(const shr_driver_caps *k, shr_memory_domain dom, const void *px, size_t stride) {
    uint32_t domains = k->domains ? k->domains : SHR_MEMORY_CPU;
#if SIZE_MAX > UINT32_MAX
    if (stride > UINT32_MAX) return false;
#endif
    return (domains & (dom ? dom : SHR_MEMORY_CPU)) && !(k->address_align && (uintptr_t)px % k->address_align) &&
           !(k->stride_align && stride % k->stride_align);
}

bool shr__driver_reaches(const shr_driver_caps *k, const shr_surface *s) {
    return shr__mem_reaches(k, s->domain, s->pixels, s->stride) && !(k->max_width && s->width > k->max_width) &&
           !(k->max_height && s->height > k->max_height);
}

/* The DMA memory the batch hands the device: the destination (NULL: it was synced in this frame, and only the driver
 * wrote it since), COPY and ROTATE sources and registered buffers whole, the rows of each UPDATE (only committed
 * buffers get one, so their slot names them). */
static void sync_buffers(shr_context *ctx, const shr_surface *dst, const shr_draw_cmd *c, size_t n) {
    if (!ctx->driver.sync) return;
    if (dst && dst->domain == SHR_MEMORY_DMA)
        SHR_HOST(ctx, ctx->driver.sync(ctx->driver.user, dst->pixels, dst->byte_length));
    for (size_t i = 0; i < n; i++) {
        shr_image m;
        size_t off = 0, row;
        switch (c[i].kind) {
        case SHR_CMD_BUFFER_UPDATE:
            m = ctx->slots[c[i].buffer - 1].buf->mem;
            shr_format_row_bytes(m.format, m.width, &row);
            off = (size_t)c[i].src_rect.y0 * m.stride;
            m.byte_length = (size_t)(c[i].src_rect.y1 - c[i].src_rect.y0 - 1) * m.stride + row;
            break;
        case SHR_CMD_COPY:
        case SHR_CMD_ROTATE:
        case SHR_CMD_BUFFER_REGISTER:
            if (c[i].src.domain != SHR_MEMORY_DMA) continue;
            shr_image_ref_get(&c[i].src, &m); /* the compositor's refs are valid */
            break;
        default: continue;
        }
        if (m.domain == SHR_MEMORY_DMA)
            SHR_HOST(ctx, ctx->driver.sync(ctx->driver.user, (const uint8_t *)m.pixels + off, m.byte_length));
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
        batch_lost(ctx, f, *st);
        return -1;
    }
    if (f->refused && !refusal_over(ctx, f)) return 0;
    const shr_surface *dst = &f->target;
    const shr_draw_cmd *cmds = f->built ? &f->convert : f->cmds.data; /* the conversion, or the raster */
    size_t n = 1;
    uint8_t bit = 4;
    if (!f->built) {
        const shr__batch *b = SHR_VEC_AT(&f->batches, shr__batch, f->batch);
        cmds += b->at, n = b->n;
        if (b->band >= 0) dst = &ctx->bands[b->band], bit = (uint8_t)(1u << b->band);
    }
    if (!n) return 1;
    if (!shr__driver_reaches(&ctx->driver.caps, dst)) return *st = SHR_E_UNSUPPORTED, -1;
    sync_buffers(ctx, f->synced & bit ? NULL : dst, cmds, n);
    f->synced |= bit;
    f->ready_seen = atomic_load(&ctx->driver_ready); /* before execute(): a ready signal during it counts */
    f->fence = ++ctx->fence_gen;
    atomic_store(&ctx->fence_state, f->fence << 2 | SHR_FENCE_PENDING);
    SHR_HOST(ctx, *st = ctx->driver.execute(ctx->driver.user, dst, cmds, n, f->fence));
    if (*st != SHR_IN_PROGRESS) atomic_store(&ctx->fence_state, 0);
    if (*st == SHR_E_WOULD_BLOCK) {
        f->refused = true;
        f->deadline = shr__sat_add(shr__ctx_now(ctx), ctx->desc.io_retry_ns);
        return 0;
    }
    f->refused = false;
    if (*st != SHR_OK && *st != SHR_IN_PROGRESS) return batch_lost(ctx, f, *st), -1;
    if (!f->batch) plan_commit(ctx, f); /* the step's prologue leads its first batch */
    keep_commit(ctx, f);
    ctx->keep_count = f->keep_count;
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
    shr_rect o = out_rect(&ctx->screen, (shr_rect){0, 0, ctx->screen.width, ctx->screen.height});
    if (ctx->composing) {
        f->target = ctx->composition;
    } else if (acquire_output(ctx, f, o.x1, o.y1, ctx->screen.output_format, &st)) {
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
    shr__moves_drop(ctx, &ctx->staged); /* the layers moved since the submission: the frame draws them moved */
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
    if (!f->damage.len && !stale) { /* the output already shows this state; moves come with damage */
        shr_event ev = {.kind = SHR_EVENT_FRAME_SUPERSEDED, .frame_id = f->frame_id};
        shr__push_event(ctx, &ev);
        if (f->has_output) discard(ctx, &f->output);
        frame_reset(ctx);
        return;
    }
    ctx->next_start_ns = shr__sat_add(shr__ctx_now(ctx), ctx->desc.min_frame_interval_ns);
    /* The first step: the frame, or with bands the moves, which run first; the bands follow one at a time. */
    f->converted = 0;
    st = plan_begin(ctx, f, submitted);
    if (st == SHR_OK) st = emit_moves(ctx, f);
    for (size_t i = 0; st == SHR_OK && !ctx->band_count && i < f->damage.len; i++)
        st = emit_rect(ctx, f, *SHR_VEC_AT(&f->damage, shr_rect, i));
    if (st == SHR_OK && (f->cmds.len || !ctx->band_count)) {
        shr__batch *b = shr__vec_push(&f->batches, &ctx->al);
        if (b) *b = (shr__batch){0, f->cmds.len, -1};
        else st = SHR_E_NO_MEMORY;
    }
    if (st == SHR_OK) st = plan_place(ctx, f);
    if (st != SHR_OK) {
        frame_fail(ctx, st);
        return;
    }
    shr__ctx_trace(ctx, SHR_TRACE_RASTER_BEGIN, f->frame_id, 0, (uint64_t)f->damaged_pixels);
}

static void raster_complete(shr_context *ctx, shr__frame *f) {
    shr__ctx_trace(ctx, SHR_TRACE_RASTER_END, f->frame_id, f->pushed + f->prologue.len, f->short_bufs);
    if (ctx->band_count) shr__ctx_trace(ctx, SHR_TRACE_CONVERT, f->frame_id, f->converted, 0);
    f->prologue.len = 0;
    resolved_end(&f->resolved, f->frame_id);
    f->took_damage = false;
    shr__target *t = f->target_rec >= 0 && ctx->targets[f->target_rec].used ? &ctx->targets[f->target_rec] : NULL;
    if (f->short_bufs && !ctx->short_logged)
        shr__ctx_log(ctx, SHR_E_LIMIT, "driver buffer ids ran short: fallbacks drawn");
    ctx->short_logged = f->short_bufs > 0;
    /* What got no id is drawn again at once, while fewer buffers go without: the next frame draws less. */
    if (f->short_bufs && t && (!ctx->short_bufs || f->short_bufs < ctx->short_bufs)) f->prov_stale = true;
    ctx->short_bufs = f->short_bufs;
    if (t && f->provisional.len) shr__moves_drop(ctx, &t->damage); /* moved since: the rects would be out of place */
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
    shr_rect o = out_rect(&ctx->screen, (shr_rect){0, 0, ctx->screen.width, ctx->screen.height});
    int32_t ow = o.x1, oh = o.y1;
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
        shr_draw_cmd *c = &f->convert;
        c->kind = ctx->screen.rotation != SHR_ROTATE_NONE ? SHR_CMD_ROTATE : SHR_CMD_COPY, c->flags = 0;
        c->rotation = (uint8_t)ctx->screen.rotation, c->dst = (shr_rect){0, 0, ow, oh}, c->src_origin = (shr_point){0, 0};
        c->src = shr__image_ref(s->pixels, s->width, s->height, s->stride, s->format, s->domain);
        f->target = f->output;
        f->synced = 0;
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
    while (f->state == FRAME_RASTER && !ends(ctx, false)) {
        if (f->batch == f->batches.len) { /* the step ran: the next band, or the end */
            if (ctx->band_count && f->band_y < ctx->screen.height && (st = emit_band(ctx, f)) != SHR_OK) {
                frame_fail(ctx, st);
                break;
            }
            if (f->batch == f->batches.len) raster_complete(ctx, f);
            continue;
        }
        int r = run(ctx, f, &st);
        if (r < 0) frame_fail(ctx, st);
        if (r <= 0) break;
        f->batch++;
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

static bool frame_wanted(const shr_context *ctx) { return can_start(ctx) && (ctx->submitted || auto_due(ctx)); }

static bool paced(const shr_context *ctx) {
    return !ctx->desc.min_frame_interval_ns || shr__ctx_now(ctx) >= ctx->next_start_ns;
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
    ctx->short_bufs = 0;
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
        shr__target *t = &ctx->targets[i];
        shr__damage *d = &t->damage;
        if (!t->used) continue;
        d->full |= ctx->staged.full;
        for (uint32_t k = 0; k < ctx->staged.nmoves; k++) {
            shr__move m = ctx->staged.moves[k];
            shr__damage_move(ctx, d, m);
            for (size_t j = 0; j < t->provisional.len; j++) {
                shr_rect *p = SHR_VEC_AT(&t->provisional, shr_rect, j);
                *p = shr__rect_union(*p, shr__rect_intersect(shr__rect_move(shr__rect_intersect(*p, m.area), 0, m.dy), m.area));
            }
        }
        for (size_t k = 0; k < ctx->staged.rects.len; k++)
            shr__damage_add(ctx, d, *SHR_VEC_AT(&ctx->staged.rects, shr_rect, k));
    }
    ctx->staged.rects.len = 0;
    ctx->staged.full = false;
    ctx->staged.nmoves = 0;
    ctx->submitted = ctx->has_submitted = true;
    ctx->failed = false;
    ctx->short_bufs = 0;
    shr__ctx_trace(ctx, SHR_TRACE_SUBMIT, ctx->next_frame_id, 0, 0);
    return first;
}

shr_status shr_pump(shr_context *ctx) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (ctx->in_callback) return SHR_E_STATE;
    bool changed = false, still = ctx->frame.state == FRAME_IDLE && ctx->next_frame_id > 1 && !ctx->submitted &&
                                  !auto_due(ctx) && !shr__io_busy(ctx, NULL);
    for (const shr__res *r = ctx->resources; still && r; r = r->next)
        still = !(r->ops->has_work && r->ops->has_work(r));
    for (int round = 0; round < 64; round++) {
        bool delivered = shr__io_pump(ctx);
        for (shr__res *r = ctx->resources; r; r = r->next)
            if (r->ops->pump && r->ops->pump(r)) changed = true;
        if (!delivered) break;
    }
    if (changed) provisional_changed(ctx);
    progress(ctx);
    ctx->keep_still |= still && !auto_due(ctx);
    if (frame_wanted(ctx) && paced(ctx)) {
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
    bool wanted = frame_wanted(ctx);
    for (const shr__res *r = ctx->resources; r; r = r->next) {
        if (r->ops->has_work && r->ops->has_work(r)) return *out = now, SHR_OK;
        if (r->ops->deadline && (t = r->ops->deadline(r)) && (!at || t < at)) at = t;
    }
    if (shr__io_ready(ctx) ||
        ((f->running || f->state == FRAME_ISOLATED) && fence_state(ctx) != SHR_FENCE_PENDING) ||
        (f->state != FRAME_IDLE && ends(ctx, true)) || submission_ends(ctx) || (f->state == FRAME_PRESENT && out_ok) ||
        (f->state == FRAME_CONVERT && !f->has_output && out_ok &&
         ctx->unreleased_count < ctx->desc.max_unreleased_frames) ||
        (wanted && paced(ctx)) || (f->refused && atomic_load(&ctx->driver_ready) != f->ready_seen))
        return *out = now, SHR_OK;
    /* The driver watchdog, or the retry of a refused submission. */
    if (((f->running && ctx->driver.caps.timeout_ns) || (f->refused && ctx->desc.io_retry_ns)) &&
        (!at || f->deadline < at))
        at = f->deadline;
    if (wanted && (!at || ctx->next_start_ns < at)) at = ctx->next_start_ns; /* held back by the frame-rate cap */
    if (auto_allowed(ctx) && !ctx->shutting_down && shr__blink_any(ctx) && (shr__blink_visible(ctx, &t), t)) {
        if (t < ctx->next_start_ns) t = ctx->next_start_ns;
        if (!at || t < at) at = t;
    }
    if (at && ctx->desc.now_ns) *out = at <= shr__ctx_now(ctx) ? now : (shr_deadline){SHR_DEADLINE_AT, at};
    return SHR_OK;
}
