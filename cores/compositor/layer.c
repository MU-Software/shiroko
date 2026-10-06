#include "compositor.h"

_Static_assert(offsetof(shr__lcmd, end) + sizeof(size_t) <= offsetof(shr__lcmd, res) + sizeof(shr__res *),
               "a CACHE_BEGIN's end fits beside its key");
_Static_assert(sizeof(shr__rcmd) == 20 && SHR_CELL_HEIGHT <= UINT8_MAX &&
                   (SHR__LCMD_DIM | SHR__LCMD_BOLD | SHR__LCMD_ITALIC | SHR__LCMD_ON_FILL | SHR__LCMD_BLINK) <= UINT8_MAX,
               "row commands hold row pixels and flags");

static bool uses_res(const shr__lcmd *c) { return c->kind == SHR__LCMD_GLYPH || c->kind == SHR__LCMD_IMAGE; }

/* Counted per run of one resource: a row's glyphs share a font. */
static void users(const shr__lcmd *c, size_t n, bool add) {
    shr__res *res = NULL;
    uint32_t k = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && !uses_res(&c[i])) continue;
        if (i < n && c[i].res == res) {
            k++;
            continue;
        }
        if (res) res->users = add ? res->users + k : res->users - k;
        if (i < n) res = c[i].res, k = 1;
    }
}

/* Unions built from `nothing` by taking the non-empty rects where `take`. */
static const shr_rect nothing = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
static shr_rect grow(shr_rect a, shr_rect r, bool take) {
    take &= (r.x0 < r.x1) & (r.y0 < r.y1);
    return (shr_rect){(take & (r.x0 < a.x0)) ? r.x0 : a.x0, (take & (r.y0 < a.y0)) ? r.y0 : a.y0,
                      (take & (r.x1 > a.x1)) ? r.x1 : a.x1, (take & (r.y1 > a.y1)) ? r.y1 : a.y1};
}
static shr_rect grown(shr_rect a) { return a.x0 < a.x1 ? a : (shr_rect){0, 0, 0, 0}; }
static bool covers(shr_rect a, shr_rect b) { return (a.x0 <= b.x0) & (a.y0 <= b.y0) & (a.x1 >= b.x1) & (a.y1 >= b.y1); }

/* The kind of command i of `g`, its dst in *dst. */
static uint32_t cmd_at(const shr__group *g, size_t i, shr_rect *dst) {
    if (g->compact) return *dst = shr__rcmd_dst(&g->rows[i]), g->rows[i].kind;
    return *dst = g->cmds[i].dst, g->cmds[i].kind;
}

/* What commands [lo, hi) of `g` draw or cache, by whole blocks where they fit or add nothing. */
static shr_rect span(const shr__group *g, size_t lo, size_t hi) {
    shr_rect u = nothing;
    for (size_t i = lo; i < hi; i++) {
        const shr_rect b = g->blocks[i / SHR__BLOCK];
        if ((i % SHR__BLOCK == 0 && i + SHR__BLOCK <= hi) || covers(u, b)) {
            u = grow(u, b, true);
            i = i / SHR__BLOCK * SHR__BLOCK + SHR__BLOCK - 1;
        } else {
            shr_rect d;
            uint32_t kind = cmd_at(g, i, &d);
            u = grow(u, d, (kind <= SHR__LCMD_IMAGE) | (kind == SHR__LCMD_CACHE_BEGIN));
        }
    }
    return u;
}

static bool lcmd_equal(const shr__lcmd *a, const shr__lcmd *b) {
    if (a->kind != b->kind || a->flags != b->flags || a->color != b->color || memcmp(&a->dst, &b->dst, sizeof(a->dst)))
        return false;
    if (a->kind == SHR__LCMD_CACHE_BEGIN) return a->key[0] == b->key[0] && a->key[1] == b->key[1];
    return !uses_res(a) || (a->anchor.x == b->anchor.x && a->anchor.y == b->anchor.y && a->bg == b->bg &&
                            a->id == b->id && a->res == b->res);
}

/* Command i of `a` and j of `b`, of one form and (rows) one resource, draw and cache alike. */
static bool cmd_equal(const shr__group *a, size_t i, const shr__group *b, size_t j) {
    if (!a->compact) return lcmd_equal(&a->cmds[i], &b->cmds[j]);
    uint32_t x[5], y[5];
    memcpy(x, &a->rows[i], sizeof(x)), memcpy(y, &b->rows[j], sizeof(y));
    return (((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3]) | (x[4] ^ y[4])) == 0) &
           ((a->rows[i].kind != SHR__LCMD_CACHE_BEGIN) | ((a->key[0] == b->key[0]) & (a->key[1] == b->key[1])));
}

/* `r` in layer coordinates; changes of hidden layers need no damage. */
static void damage(shr_lyr *l, shr_rect r) {
    if (!l->visible) return;
    shr__damage_add(l->ctx, &l->ctx->staged,
                    shr__rect_intersect(shr__rect_move(r, l->rect.x0, l->rect.y0), l->rect));
}

static shr_rect content(const shr_lyr *l) {
    shr_rect r = {0, 0, 0, 0};
    for (size_t i = 0; i < l->groups.len; i++) {
        const shr__group *g = SHR_VEC_AT(&l->groups, shr__group, i);
        r = shr__rect_union(r, shr__rect_move(g->bounds, 0, g->oy));
    }
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
    SHR_VEC_INIT_HOT(&l->groups, shr__group);
    SHR_VEC_INIT_HOT(&l->pending, shr__lcmd);
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

static size_t unit_of(bool compact) { return compact ? sizeof(shr__rcmd) : sizeof(shr__lcmd); }

static size_t group_bytes(size_t unit, size_t n) { return n * unit + (n + SHR__BLOCK - 1) / SHR__BLOCK * sizeof(shr_rect); }

static void spare_free(shr_lyr *l, void *cmds, size_t cap, size_t unit) {
    l->held -= cmds ? group_bytes(unit, cap) : 0;
    shr__free(&l->ctx->al, cmds, group_bytes(unit, cap), SHR_ALIGNOF(shr__lcmd), SHR__HOT);
}

static void **spare_next(void *p) { return (void **)p; }

static int class_of(const shr_lyr *l, size_t n) { return (n > l->class_cap[0]) + (n > l->class_cap[1]); }

/* Group memory no longer used: kept idle in its class, freed when no class has its size (from before a growth) or
 * its commands are of the other form. */
static void spare_put(shr_lyr *l, void *cmds, size_t cap, size_t unit) {
    int k = class_of(l, cap);
    if (cmds && (cap == l->class_cap[k]) & (unit == l->unit))
        *spare_next(cmds) = l->spares[k], l->spares[k] = cmds;
    else
        spare_free(l, cmds, cap, unit);
}

static void *spare_pop(shr_lyr *l, int k) {
    void *p = l->spares[k];
    l->spares[k] = *spare_next(p);
    return p;
}

static void spares_free(shr_lyr *l) {
    spare_free(l, l->spare, l->spare_cap, l->unit);
    for (int k = 0; k < GROUP_CLASSES; k++)
        while (l->spares[k]) spare_free(l, spare_pop(l, k), l->class_cap[k], l->unit);
    l->spare = NULL;
}

/* Classes hold 5/16, 10/16 and all of `most` commands, in steps of 16. */
static void classes_set(shr_lyr *l, size_t most) {
    spares_free(l);
    for (int k = 0; k < GROUP_CLASSES - 1; k++) {
        size_t c = most * (size_t)(5 << k) / 16 & ~(size_t)15;
        l->class_cap[k] = c < 16 ? (most < 16 ? most : 16) : c;
    }
    l->class_cap[GROUP_CLASSES - 1] = most;
}

/* Memory for at least n commands of `unit` bytes lent as the spare (NULL: no memory); idle memory of the other form is
 * freed first. More than the largest class grows the classes by a quarter at least. Idle memory of the class serves
 * first; past the memory of peak + 1 largest groups, idle memory of a larger class instead, else idle memory of smaller
 * classes is freed. */
static void *spare_take(shr_lyr *l, size_t n, size_t unit) {
    if (unit != l->unit) spares_free(l), l->unit = unit;
    size_t most = l->class_cap[GROUP_CLASSES - 1];
    n = n ? n : 1;
    if (n > most) classes_set(l, ((n > most + most / 4 ? n : most + most / 4) + 15) & ~(size_t)15);
    int k = class_of(l, n), j = k;
    if (l->spare && l->spare_cap == l->class_cap[k]) return l->spare;
    spare_put(l, l->spare, l->spare_cap, unit);
    size_t need = group_bytes(unit, l->class_cap[k]), limit = (l->peak + 1) * group_bytes(unit, l->class_cap[GROUP_CLASSES - 1]);
    while (!l->spares[j] && j < GROUP_CLASSES - 1 && l->held + need > limit) j++;
    j = l->spares[j] ? j : k;
    for (int i = 0; i < k && !l->spares[j]; i++)
        while (l->spares[i] && l->held + need > limit) spare_free(l, spare_pop(l, i), l->class_cap[i], unit);
    l->spare_cap = l->class_cap[j];
    if (l->spares[j]) return l->spare = spare_pop(l, j);
    l->spare = shr__malloc(&l->ctx->al, need, SHR_ALIGNOF(shr__lcmd), SHR__HOT);
    l->held += l->spare ? need : 0;
    return l->spare;
}

static void group_release(const shr__group *g) {
    if (g->res)
        g->res->users -= g->uses;
    else if (g->uses)
        users(g->cmds, g->n, false);
}

static void groups_clear(shr_lyr *l, size_t most) {
    damage(l, content(l));
    for (size_t i = 0; i < l->groups.len; i++) {
        shr__group *g = SHR_VEC_AT(&l->groups, shr__group, i);
        group_release(g);
        spare_free(l, g->cmds, g->cap, unit_of(g->compact));
    }
    l->groups.len = 0;
    l->peak = 0;
    classes_set(l, most);
}

shr_status shr_lyr_destroy(shr_lyr *l) {
    if (!l) return SHR_E_INVALID_ARG;
    if (l->ctx->in_callback) return SHR_E_STATE;
    shr__alloc al = l->ctx->al;
    pending_clear(l);
    groups_clear(l, 0);
    unlink_layer(l);
    if (l->destroy) l->destroy(l->state);
    shr__vec_free(&l->groups, &al);
    shr__vec_free(&l->pending, &al);
    SHR_DELETE(&al, l, shr_lyr);
    return SHR_OK;
}

/* A list is valid when no command is bad and none is left `open`: CACHE_BEGIN and CACHE_END alternate. */
static inline __attribute__((always_inline)) bool cmd_bad(uint32_t kind, uint32_t flags, shr_rect dst, bool no_res,
                                                          bool *open) {
    bool begin = kind == SHR__LCMD_CACHE_BEGIN, end = kind == SHR__LCMD_CACHE_END;
    bool bad = !shr__rect_valid(dst) ||
               (flags & ~(uint32_t)(SHR__LCMD_DIM | SHR__LCMD_BOLD | SHR__LCMD_ITALIC | SHR__LCMD_ON_FILL |
                                    SHR__LCMD_BLINK)) ||
               ((begin || end) ? *open != end : !kind || kind > SHR__LCMD_IMAGE) || no_res;
    *open = *open != (begin || end);
    return bad;
}

/* Changes of one group in place are damaged in this many separate rects, then the rest as one. */
#define GROUP_RUNS 8

/* The list `in` describes (cmds or rows, n, and a row's res and key) becomes group `id`: in the spare as it is, else
 * copied into the spare once it changes the group. */
static shr_status group_set(shr_lyr *l, uint32_t id, int32_t oy, shr__group in) {
    const shr__alloc *al = &l->ctx->al;
    size_t pos = 0, end = l->groups.len, n = in.n, unit = unit_of(in.compact);
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
    /* else every command changed */
    bool same = !g || ((g->oy == oy) & (g->compact == in.compact) & (!in.compact | (g->res == in.res)));
    size_t lo = 0, ha = g ? g->n : 0, hb = n;
    while (same && lo < ha && lo < hb && cmd_equal(g, lo, &in, lo)) lo++;
    if (lo == ha && lo == hb) return SHR_OK; /* unchanged */
    void *own = in.cmds;
    if (n && own != l->spare) {
        const void *from = own;
        if (!(own = spare_take(l, n, unit))) return SHR_E_NO_MEMORY;
        memcpy(own, from, n * unit);
    }
    const shr_rect none = {0, 0, 0, 0};
    shr_rect bounds = nothing, blink = nothing, block = nothing, opaque = none;
    in.cmds = own, in.blocks = n ? (shr_rect *)(void *)((char *)own + l->spare_cap * unit) : NULL;
    int64_t most = 0;
    shr__res *one = NULL;
    uint32_t uses = 0;
    bool mixed = false, bad = false, open = false;
    size_t begin_at = 0;
    for (size_t i = 0; i < n; i++) {
        const shr__rcmd *r = in.compact ? &in.rows[i] : NULL;
        const shr__lcmd *c = in.compact ? NULL : &in.cmds[i];
        uint32_t kind = r ? r->kind : c->kind, flags = r ? r->flags : c->flags;
        shr_rect dst = r ? shr__rcmd_dst(r) : c->dst;
        bool begin = kind == SHR__LCMD_CACHE_BEGIN, finish = kind == SHR__LCMD_CACHE_END;
        bool draw = kind <= SHR__LCMD_IMAGE, res_cmd = kind == SHR__LCMD_GLYPH || kind == SHR__LCMD_IMAGE;
        shr__res *res = !res_cmd ? NULL : r ? in.res : c->res;
        bad |= cmd_bad(kind, flags, dst, res_cmd && !res, &open);
        bad |= in.compact & ((begin & (i != 0)) | (finish & (i != n - 1))); /* a row's pair encloses it */
        if (begin)
            begin_at = i;
        else if (finish & !in.compact) /* a bad list, where begin_at may name another kind, is dropped */
            in.cmds[begin_at].end = i - begin_at;
        if (res_cmd) mixed |= one && res != one, one = res, uses++;
        bool solid = begin || /* cache groups are opaque */
                     (kind == SHR__LCMD_FILL && !(flags & (SHR__LCMD_DIM | SHR__LCMD_BLINK)));
        block = grow(block, dst, draw | begin);
        if (flags & SHR__LCMD_BLINK) blink = grow(blink, dst, draw);
        if (solid && (!most || !covers(opaque, dst)) && shr__rect_area(dst) > most)
            opaque = dst, most = shr__rect_area(dst);
        if (i % SHR__BLOCK == SHR__BLOCK - 1 || i == n - 1) {
            bounds = grow(bounds, block, true);
            in.blocks[i / SHR__BLOCK] = grown(block), block = nothing;
        }
    }
    if (bad || open) return SHR_E_INVALID_ARG;
    if (!g && !shr__vec_reserve(&l->groups, al, 1)) return SHR_E_NO_MEMORY;
    if (!g) {
        g = SHR_VEC_AT(&l->groups, shr__group, pos);
        memmove(g + 1, g, (l->groups.len++ - pos) * sizeof(*g));
        l->peak = l->groups.len > l->peak ? l->groups.len : l->peak;
        *g = (shr__group){.id = id, .oy = oy};
    }
    /* Past the equal ends, lists of one length change in place: neighbouring changes are damaged together, past
     * GROUP_RUNS rects the rest as one. A length change shifts what follows, so the changed middle, with the areas it
     * caches, is damaged as one. */
    /* In the coordinates of the new commands. */
    while (same && ha > lo && hb > lo && cmd_equal(g, ha - 1, &in, hb - 1)) ha--, hb--;
    shr_rect run = same && ha == hb ? none
                                    : grown(grow(shr__rect_move(span(g, lo, ha), 0, shr__clamp32((int64_t)g->oy - oy)),
                                                 span(&in, lo, hb), true));
    int runs = 0;
    for (size_t i = lo; same && ha == hb && i < ha; i++) {
        if (cmd_equal(g, i, &in, i)) continue;
        if (runs == GROUP_RUNS) {
            run = shr__rect_union(run, grown(grow(span(g, i, ha), span(&in, i, hb), true)));
            break;
        }
        shr_rect parts[2];
        if (cmd_at(g, i, &parts[0]) > SHR__LCMD_IMAGE) parts[0] = none;
        if (cmd_at(&in, i, &parts[1]) > SHR__LCMD_IMAGE) parts[1] = none;
        for (int k = 0; k < 2; k++) {
            shr_rect u = shr__rect_union(run, parts[k]);
            if (shr__rect_area(u) > shr__rect_area(run) + shr__rect_area(parts[k]))
                damage(l, shr__rect_move(run, 0, oy)), u = parts[k], runs++;
            run = u;
        }
    }
    damage(l, shr__rect_move(run, 0, oy));
    one = mixed ? NULL : one;
    if (one)
        one->users += uses;
    else if (uses)
        users(own, n, true);
    group_release(g);
    void *old = g->cmds;
    size_t old_cap = g->cap, old_unit = unit_of(g->compact);
    if (!n) {
        memmove(g, g + 1, (--l->groups.len - pos) * sizeof(*g));
    } else {
        in.id = id, in.cap = l->spare_cap, in.oy = oy, in.bounds = grown(bounds), in.blink = grown(blink);
        in.opaque = opaque, in.res = in.compact ? in.res : one, in.uses = uses;
        *g = in;
        l->spare = NULL;
    }
    spare_put(l, old, old_cap, old_unit);
    return SHR_OK;
}

shr_status shr__lyr_group_set(shr_lyr *l, uint32_t group, const shr__lcmd *cmds, size_t n) {
    shr_status st = check(l);
    if (st != SHR_OK) return st;
    if (n && !cmds) return SHR_E_INVALID_ARG;
    return group_set(l, group, 0, (shr__group){.cmds = (shr__lcmd *)cmds, .n = n});
}

shr__rcmd *shr__lyr_row_begin(shr_lyr *l, size_t n) { return spare_take(l, n, sizeof(shr__rcmd)); }

shr_status shr__lyr_row_commit(shr_lyr *l, uint32_t group, int32_t oy, shr__res *res, const uint64_t key[2],
                               shr__rcmd *cmds, size_t n) {
    return shr__ctx_refused(l->ctx) ? SHR_E_STATE
                                    : group_set(l, group, oy,
                                                (shr__group){.compact = true, .rows = cmds, .n = n, .res = res,
                                                             .key = {key[0], key[1]}});
}

shr_status shr__lyr_groups_clear(shr_lyr *l, size_t most) {
    shr_status st = check(l);
    if (st == SHR_OK) groups_clear(l, most);
    return st;
}

/* Above a move, layers whose pixels the frame redraws beyond 1/MOVE_COVER of what moves make it a redraw instead: the
 * rows under them would be drawn in many pieces besides the move. */
#define MOVE_COVER 16

/* The part of `a` that layers above `l` cover, and where their pixels move to by dy. */
static int64_t covered_above(const shr_lyr *l, shr_rect a, int32_t dy, bool add) {
    int64_t sum = 0;
    for (const shr_lyr *u = l->next; u; u = u->next) {
        shr_rect b = shr__rect_intersect(shr__rect_move(content(u), u->rect.x0, u->rect.y0), u->rect);
        b = u->visible ? b : (shr_rect){0, 0, 0, 0};
        const shr_rect parts[2] = {shr__rect_intersect(b, a), shr__rect_intersect(shr__rect_move(b, 0, dy), a)};
        for (int k = 0; k < 2; k++) {
            sum += shr__rect_area(parts[k]);
            if (add) shr__damage_add(l->ctx, &l->ctx->staged, parts[k]);
        }
    }
    return sum;
}

/* `a` (screen coordinates, inside the layer) shows the layer's pixels moved down by dy. A move needs the driver's cheap
 * moves, the layer to cover what moves with opaque groups and few layers above it; it keeps what those show with
 * damage where they are and where their pixels moved to, and the uncovered rows with damage. Else `a` is damage. */
static void move_record(shr_lyr *l, uint32_t first, uint32_t last, shr_rect a, int32_t dy) {
    shr_context *ctx = l->ctx;
    shr__damage *s = &ctx->staged;
    shr_rect dst = shr__rect_intersect(a, shr__rect_move(a, 0, dy));
    int64_t h = (int64_t)a.y1 - a.y0, d = dy < 0 ? -(int64_t)dy : dy;
    if (!(ctx->driver.caps.flags & SHR_DRIVER_CHEAP_MOVE) || 2 * d >= h || !shr__hides(l, dst) ||
        MOVE_COVER * covered_above(l, a, dy, false) > shr__rect_area(dst)) {
        shr__damage_add(ctx, s, a);
        return;
    }
    shr__damage_move(ctx, s, (shr__move){a, dy});
    covered_above(l, a, dy, true);
    shr__damage_add(ctx, s, (shr_rect){a.x0, dy > 0 ? a.y0 : a.y1 + dy, a.x1, dy > 0 ? a.y0 + dy : a.y1});
    for (size_t i = 0; i < l->groups.len; i++) {
        const shr__group *g = SHR_VEC_AT(&l->groups, shr__group, i);
        if (g->id >= first && g->id < last) shr__keep_moved(ctx, g);
    }
}

void shr__lyr_groups_shift(shr_lyr *l, uint32_t first, uint32_t last, int32_t shift, shr_rect area, int32_t dy) {
    size_t n = 0;
    for (size_t i = 0; i < l->groups.len; i++) {
        shr__group g = *SHR_VEC_AT(&l->groups, shr__group, i);
        int64_t id = (int64_t)g.id + shift;
        bool in = g.id >= first && g.id < last;
        if (in && (id < first || id >= last)) {
            group_release(&g);
            spare_put(l, g.cmds, g.cap, unit_of(g.compact));
            continue;
        }
        if (in) g.id = (uint32_t)id, g.oy = shr__clamp32((int64_t)g.oy + dy);
        *SHR_VEC_AT(&l->groups, shr__group, n++) = g;
    }
    l->groups.len = n;
    shr_rect screen = {0, 0, l->ctx->screen.width, l->ctx->screen.height};
    shr_rect a = shr__rect_intersect(shr__rect_move(area, l->rect.x0, l->rect.y0), shr__rect_intersect(l->rect, screen));
    if (l->visible) move_record(l, first, last, a, dy);
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
    bool open = false;
    if (!cmd || cmd_bad(cmd->kind, cmd->flags, cmd->dst, uses_res(cmd) && !cmd->res, &open) || open)
        return SHR_E_INVALID_ARG;
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
    if ((st = group_set(l, 0, 0, (shr__group){.cmds = l->pending.data, .n = l->pending.len})) != SHR_OK) return st;
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
            if (g->res ? g->res != res : !g->uses) continue;
            for (size_t i = 0; i < g->n; i++) {
                const shr__lcmd c = shr__group_cmd(g, i);
                if (!uses_res(&c) || c.res != res) continue;
                shr_point at = shr__group_at(l, g);
                shr_rect r = shr__rect_intersect(shr__rect_move(area, c.anchor.x, c.anchor.y), c.dst);
                shr__damage_targets(ctx, shr__rect_intersect(shr__rect_move(r, at.x, at.y), l->rect));
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
            const shr__group *g = SHR_VEC_AT(&l->groups, shr__group, i);
            shr_point at = shr__group_at(l, g);
            if (!shr__rect_empty(g->blink))
                shr__damage_targets(ctx, shr__rect_intersect(shr__rect_move(g->blink, at.x, at.y), l->rect));
        }
}
