#include "compositor.h"

static bool draws(const shr__lcmd *c) { return c->kind <= SHR__LCMD_IMAGE; }
static bool uses_res(const shr__lcmd *c) { return c->kind == SHR__LCMD_GLYPH || c->kind == SHR__LCMD_IMAGE; }

static void users(const shr__lcmd *c, size_t n, bool add) {
    for (size_t i = 0; i < n; i++)
        if (uses_res(&c[i])) add ? c[i].res->users++ : c[i].res->users--;
}

static bool lcmd_equal(const shr__lcmd *a, const shr__lcmd *b) {
    return a->kind == b->kind && a->flags == b->flags && !memcmp(&a->dst, &b->dst, sizeof(a->dst)) &&
           a->anchor.x == b->anchor.x && a->anchor.y == b->anchor.y && a->color == b->color && a->res == b->res &&
           a->id == b->id && a->key[0] == b->key[0] && a->key[1] == b->key[1];
}

/* `r` in layer coordinates; changes of hidden layers need no damage. */
static void damage(shr_lyr *l, shr_rect r) {
    if (!l->visible) return;
    shr__damage_add(l->ctx, &l->ctx->staged,
                    shr__rect_intersect(shr__rect_move(r, l->rect.x0, l->rect.y0), l->rect));
}

static shr_rect content(const shr_lyr *l) {
    shr_rect r = {0, 0, 0, 0};
    for (size_t i = 0; i < l->groups.len; i++) r = shr__rect_union(r, SHR_VEC_AT(&l->groups, shr__group, i)->bounds);
    return r;
}

static void link(shr_lyr *l) {
    shr_lyr **pp = &l->ctx->layers;
    while (*pp && ((*pp)->z < l->z || ((*pp)->z == l->z && (*pp)->seq < l->seq))) pp = &(*pp)->next;
    l->next = *pp;
    *pp = l;
}

static void unlink_layer(shr_lyr *l) {
    shr_lyr **pp = &l->ctx->layers;
    while (*pp != l) pp = &(*pp)->next;
    *pp = l->next;
}

static shr_status check(const shr_lyr *l) {
    if (!l) return SHR_E_INVALID_ARG;
    return shr__ctx_refused(l->ctx) ? SHR_E_STATE : SHR_OK;
}

shr_status shr_lyr_create(shr_context *ctx, int32_t z, shr_rect rect, shr_lyr **out) {
    if (out) *out = NULL;
    if (!ctx || !out || !shr__rect_valid(rect)) return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    shr_lyr *l = SHR_NEW(&ctx->al, shr_lyr);
    if (!l) return SHR_E_NO_MEMORY;
    *l = (shr_lyr){.ctx = ctx, .z = z, .seq = ctx->next_layer_seq++, .rect = rect, .visible = true};
    SHR_VEC_INIT(&l->groups, shr__group);
    SHR_VEC_INIT(&l->pending, shr__lcmd);
    link(l);
    *out = l;
    return SHR_OK;
}

shr_status shr_lyr_set_rect(shr_lyr *l, shr_rect rect) {
    shr_status st = check(l);
    if (st != SHR_OK) return st;
    if (!shr__rect_valid(rect)) return SHR_E_INVALID_ARG;
    shr_rect c = content(l);
    damage(l, c);
    l->rect = rect;
    damage(l, c);
    return SHR_OK;
}

shr_status shr_lyr_set_z(shr_lyr *l, int32_t z) {
    shr_status st = check(l);
    if (st != SHR_OK || l->z == z) return st;
    unlink_layer(l);
    l->z = z;
    link(l);
    damage(l, content(l));
    return SHR_OK;
}

shr_status shr_lyr_set_visible(shr_lyr *l, bool visible) {
    shr_status st = check(l);
    if (st != SHR_OK || l->visible == visible) return st;
    l->visible = true;
    damage(l, content(l));
    l->visible = visible;
    return SHR_OK;
}

static void pending_clear(shr_lyr *l) {
    users(l->pending.data, l->pending.len, false);
    l->pending.len = 0;
}

static size_t group_bytes(size_t n) { return n * sizeof(shr__lcmd) + (n + SHR__BLOCK - 1) / SHR__BLOCK * sizeof(shr_rect); }

static void group_free(const shr__alloc *al, shr__group *g) {
    shr__free(al, g->cmds, group_bytes(g->n), SHR_ALIGNOF(shr__lcmd), SHR_ALLOC_PAYLOAD);
}

static void groups_clear(shr_lyr *l) {
    damage(l, content(l));
    for (size_t i = 0; i < l->groups.len; i++) {
        shr__group *g = SHR_VEC_AT(&l->groups, shr__group, i);
        users(g->cmds, g->n, false);
        group_free(&l->ctx->al, g);
    }
    l->groups.len = 0;
}

shr_status shr_lyr_destroy(shr_lyr *l) {
    if (!l) return SHR_E_INVALID_ARG;
    if (l->ctx->in_callback) return SHR_E_STATE;
    shr__alloc al = l->ctx->al;
    pending_clear(l);
    groups_clear(l);
    unlink_layer(l);
    if (l->destroy) l->destroy(l->state);
    shr__vec_free(&l->groups, &al);
    shr__vec_free(&l->pending, &al);
    SHR_DELETE(&al, l, shr_lyr);
    return SHR_OK;
}

static shr_status validate(const shr__lcmd *c, size_t n) {
    bool open = false;
    for (size_t i = 0; i < n; i++) {
        if (!shr__rect_valid(c[i].dst) ||
            (c[i].flags & ~(uint32_t)(SHR__LCMD_DIM | SHR__LCMD_BOLD | SHR__LCMD_ITALIC | SHR__LCMD_BLINK)) ||
            (uses_res(&c[i]) && !c[i].res))
            return SHR_E_INVALID_ARG;
        if (c[i].kind == SHR__LCMD_CACHE_BEGIN || c[i].kind == SHR__LCMD_CACHE_END) {
            if (open != (c[i].kind == SHR__LCMD_CACHE_END)) return SHR_E_INVALID_ARG;
            open = !open;
        } else if (!draws(&c[i]) || !c[i].kind) {
            return SHR_E_INVALID_ARG;
        }
    }
    return open ? SHR_E_INVALID_ARG : SHR_OK;
}

static shr_status group_set(shr_lyr *l, uint32_t id, const shr__lcmd *cmds, size_t n) {
    shr_status st = validate(cmds, n);
    if (st != SHR_OK) return st;
    const shr__alloc *al = &l->ctx->al;
    size_t pos = 0, end = l->groups.len;
    while (pos < end) {
        size_t mid = pos + (end - pos) / 2;
        if (SHR_VEC_AT(&l->groups, shr__group, mid)->id < id)
            pos = mid + 1;
        else
            end = mid;
    }
    shr__group *g = pos < l->groups.len && SHR_VEC_AT(&l->groups, shr__group, pos)->id == id
                        ? SHR_VEC_AT(&l->groups, shr__group, pos)
                        : NULL;
    if (!g && !n) return SHR_OK;
    shr__lcmd *copy = n ? shr__malloc(al, group_bytes(n), SHR_ALIGNOF(shr__lcmd), SHR_ALLOC_PAYLOAD) : NULL;
    if (n && !copy) return SHR_E_NO_MEMORY;
    if (n) memcpy(copy, cmds, n * sizeof(*copy));
    if (!g) {
        if (!shr__vec_reserve(&l->groups, al, 1)) {
            shr__free(al, copy, group_bytes(n), SHR_ALIGNOF(shr__lcmd), SHR_ALLOC_PAYLOAD);
            return SHR_E_NO_MEMORY;
        }
        g = SHR_VEC_AT(&l->groups, shr__group, pos);
        memmove(g + 1, g, (l->groups.len++ - pos) * sizeof(*g));
        *g = (shr__group){.id = id};
    }
    const shr_rect none = {0, 0, 0, 0};
    shr_rect run = none; /* neighbouring changes are recorded together */
    for (size_t i = 0; i < g->n || i < n; i++) {
        const shr__lcmd *a = i < g->n ? &g->cmds[i] : NULL, *b = i < n ? &cmds[i] : NULL;
        if (a && b && lcmd_equal(a, b)) continue;
        const shr_rect parts[2] = {a && draws(a) ? a->dst : none, b && draws(b) ? b->dst : none};
        for (int k = 0; k < 2; k++) {
            shr_rect u = shr__rect_union(run, parts[k]);
            if (shr__rect_area(u) > shr__rect_area(run) + shr__rect_area(parts[k])) damage(l, run), u = parts[k];
            run = u;
        }
    }
    damage(l, run);
    users(copy, n, true);
    users(g->cmds, g->n, false);
    group_free(al, g);
    if (!n) {
        memmove(g, g + 1, (--l->groups.len - pos) * sizeof(*g));
        return SHR_OK;
    }
    g->cmds = copy, g->blocks = (shr_rect *)(copy + n), g->n = n;
    g->bounds = g->blink = g->opaque = (shr_rect){0, 0, 0, 0};
    for (size_t i = 0; i < n; i++) {
        shr_rect *block = &g->blocks[i / SHR__BLOCK];
        if (i % SHR__BLOCK == 0) *block = (shr_rect){0, 0, 0, 0};
        if (draws(&copy[i]) || copy[i].kind == SHR__LCMD_CACHE_BEGIN) *block = shr__rect_union(*block, copy[i].dst);
        bool solid = copy[i].kind == SHR__LCMD_CACHE_BEGIN || /* cache groups are opaque */
                     (copy[i].kind == SHR__LCMD_FILL && !(copy[i].flags & (SHR__LCMD_DIM | SHR__LCMD_BLINK)));
        if (solid && shr__rect_area(copy[i].dst) > shr__rect_area(g->opaque)) g->opaque = copy[i].dst;
        if (!draws(&copy[i])) continue;
        g->bounds = shr__rect_union(g->bounds, copy[i].dst);
        if (copy[i].flags & SHR__LCMD_BLINK) g->blink = shr__rect_union(g->blink, copy[i].dst);
    }
    return SHR_OK;
}

shr_status shr__lyr_group_set(shr_lyr *l, uint32_t group, const shr__lcmd *cmds, size_t n) {
    shr_status st = check(l);
    if (st != SHR_OK) return st;
    if (n && !cmds) return SHR_E_INVALID_ARG;
    return group_set(l, group, cmds, n);
}

shr_status shr__lyr_groups_clear(shr_lyr *l) {
    shr_status st = check(l);
    if (st == SHR_OK) groups_clear(l);
    return st;
}

shr_status shr_lyr_cmd_begin(shr_lyr *l) {
    shr_status st = check(l);
    if (st != SHR_OK) return st;
    if (l->kind) return SHR_E_STATE;
    pending_clear(l);
    l->building = true;
    return SHR_OK;
}

shr_status shr__lyr_cmd_add(shr_lyr *l, const shr__lcmd *cmd) {
    shr_status st = check(l);
    if (st != SHR_OK) return st;
    if (!cmd || validate(cmd, 1) != SHR_OK) return SHR_E_INVALID_ARG;
    if (!l->building) return SHR_E_STATE;
    shr__lcmd *c = shr__vec_push(&l->pending, &l->ctx->al);
    if (!c) return SHR_E_NO_MEMORY;
    *c = *cmd;
    users(c, 1, true);
    return SHR_OK;
}

shr_status shr_lyr_cmd_fill(shr_lyr *l, shr_rect rect, shr_color color) {
    return shr__lyr_cmd_add(l, &(shr__lcmd){.kind = SHR__LCMD_FILL, .dst = rect, .color = color});
}

shr_status shr_lyr_cmd_commit(shr_lyr *l) {
    shr_status st = check(l);
    if (st != SHR_OK) return st;
    if (!l->building) return SHR_E_STATE;
    if ((st = group_set(l, 0, l->pending.data, l->pending.len)) != SHR_OK) return st;
    pending_clear(l);
    l->building = false;
    return SHR_OK;
}

shr_rect shr__lyr_rect(const shr_lyr *l) { return l->rect; }
shr_context *shr__lyr_ctx(const shr_lyr *l) { return l->ctx; }

shr_status shr__lyr_attach(shr_lyr *l, const void *kind, void *state, void (*destroy)(void *state),
                           shr_status (*flush)(void *state)) {
    shr_status st = check(l);
    if (st != SHR_OK) return st;
    if (!kind) return SHR_E_INVALID_ARG;
    if (l->kind || l->building || l->groups.len) return SHR_E_STATE;
    l->kind = kind, l->state = state, l->destroy = destroy, l->flush = flush;
    return SHR_OK;
}

void *shr__lyr_state(const shr_lyr *l, const void *kind) { return l && kind && l->kind == kind ? l->state : NULL; }

void shr__res_changed(shr__res *res, shr_rect area) {
    shr_context *ctx = res->ctx;
    for (shr_lyr *l = ctx->layers; l; l = l->next) {
        if (!l->visible) continue;
        for (size_t gi = 0; gi < l->groups.len; gi++) {
            const shr__group *g = SHR_VEC_AT(&l->groups, shr__group, gi);
            for (size_t i = 0; i < g->n; i++) {
                const shr__lcmd *c = &g->cmds[i];
                if (!uses_res(c) || c->res != res) continue;
                shr_rect r = shr__rect_intersect(shr__rect_move(area, c->anchor.x, c->anchor.y), c->dst);
                shr__damage_targets(ctx, shr__rect_intersect(shr__rect_move(r, l->rect.x0, l->rect.y0), l->rect));
            }
        }
    }
}

bool shr__blink_any(const shr_context *ctx) {
    for (const shr_lyr *l = ctx->layers; l; l = l->next)
        for (size_t i = 0; l->visible && i < l->groups.len; i++)
            if (!shr__rect_empty(SHR_VEC_AT(&l->groups, shr__group, i)->blink)) return true;
    return false;
}

void shr__blink_damage(shr_context *ctx) {
    for (const shr_lyr *l = ctx->layers; l; l = l->next)
        for (size_t i = 0; l->visible && i < l->groups.len; i++) {
            shr_rect b = SHR_VEC_AT(&l->groups, shr__group, i)->blink;
            if (!shr__rect_empty(b))
                shr__damage_targets(ctx, shr__rect_intersect(shr__rect_move(b, l->rect.x0, l->rect.y0), l->rect));
        }
}
