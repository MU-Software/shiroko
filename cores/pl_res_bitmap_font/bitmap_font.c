#include "font_core.h"

static shr_pl_res_bitmap_font *font_of(const shr__res *res) { return SHR_CONTAINER(res, shr_pl_res_bitmap_font, res); }

static bool has_async(const shr__pkg *pkg) { return !pkg->mapped && pkg->src.read; }

static void font_free(shr_pl_res_bitmap_font *f) {
    for (int r = 0; r < ROLE_COUNT; r++) shr__pkg_release(&f->pkg[r]);
    shr__vec_free(&f->wants, &f->al);
    shr__vec_free(&f->clusters, &f->al);
    shr__vec_free(&f->pool, &f->al);
    shr__free(&f->al, f->slots, f->nslots * sizeof(uint32_t), SHR_ALIGNOF(uint32_t), SHR_ALLOC_DESCRIPTOR);
    shr__alloc al = f->al;
    SHR_DELETE(&al, f, shr_pl_res_bitmap_font);
}

static shr_status op_resolve(shr__res *res, uint64_t id, uint64_t frame, shr__resolved *out) {
    return shr__font_resolve(font_of(res), id, frame, out);
}

static void op_frame_end(shr__res *res, uint64_t frame) { shr__font_frame_end(font_of(res), frame); }

/* A cool-down a frame ran into is over: the pump redraws its fallback, so the next frame tries it again. */
static bool cool_over(const shr_pl_res_bitmap_font *f, uint64_t now, uint32_t ready) {
    return f->cool_due || (f->cool_at && f->cool_at <= now) || (f->cool_wait && ready != f->ready_seen);
}

static bool op_pump(shr__res *res) {
    shr_pl_res_bitmap_font *f = font_of(res);
    if (res->dead || f->shutting_down) return false;
    f->blocked = false;
    uint32_t ready = shr__ctx_asset_ready(res->ctx);
    if (cool_over(f, shr__font_now(f), ready))
        f->changed = true, f->cool_at = 0, f->cool_wait = f->cool_due = false, f->ready_seen = ready;
    for (int r = ROLE_LATIN; r < ROLE_COUNT; r++) shr__pkg_advance(&f->pkg[r]);
    shr__pages_schedule(f);
    bool changed = f->changed;
    f->changed = false;
    return changed;
}

static bool op_has_work(const shr__res *res) {
    const shr_pl_res_bitmap_font *f = font_of(res);
    if (res->dead || f->shutting_down) return false;
    uint64_t now = shr__font_now(f);
    for (int r = ROLE_LATIN; r < ROLE_COUNT; r++)
        if (shr__pkg_due(&f->pkg[r], now)) return true;
    for (size_t i = 0; i < f->wants.len; i++)
        if (shr__page_due(*SHR_VEC_AT(&f->wants, shr__page *, i), now)) return true;
    /* `changed` is only set and cleared inside the pump */
    return cool_over(f, now, shr__ctx_asset_ready(res->ctx));
}

static uint64_t earlier(uint64_t at, uint64_t t) { return at && at <= t ? at : t; } /* at 0: none yet */

/* The earliest retry of anything wanted: packages to open or read, and pages. Without io_retry_ns only a ready
 * signal ends a wait, as it does for a package or page given up. */
static uint64_t op_deadline(const shr__res *res) {
    const shr_pl_res_bitmap_font *f = font_of(res);
    if (res->dead || f->shutting_down || !f->retry_ns) return 0;
    uint64_t at = f->cool_at;
    for (int r = ROLE_LATIN; r < ROLE_COUNT; r++) {
        const shr__pkg *pkg = &f->pkg[r];
        if (shr__pkg_due(pkg, UINT64_MAX)) at = earlier(at, pkg->retry.at);
    }
    for (size_t i = 0; i < f->wants.len; i++) {
        const shr__page *p = *SHR_VEC_AT(&f->wants, shr__page *, i);
        if (shr__page_due(p, UINT64_MAX)) at = earlier(at, p->retry.at);
    }
    return at;
}

static void op_io_done(shr__res *res, uint64_t tag, shr_status status) {
    shr_pl_res_bitmap_font *f = font_of(res);
    uint32_t role = (uint32_t)(tag >> 32), page = (uint32_t)tag;
    if (res->dead) return;
    shr__pkg *pkg = &f->pkg[role];
    f->wake++;
    if (!page) {
        if (f->shutting_down)
            pkg->step_busy = false;
        else
            shr__pkg_load_done(pkg, status);
        return;
    }
    if (!f->shutting_down) shr__page_done(pkg->pages[page - 1], status); /* else freed with the font */
}

static void cancel_reads(shr_pl_res_bitmap_font *f) {
    for (int r = ROLE_LATIN; r < ROLE_COUNT; r++)
        if (has_async(&f->pkg[r])) shr__ctx_read_cancel(f->res.ctx, &f->pkg[r].src);
}

static void op_shutdown(shr__res *res) {
    shr_pl_res_bitmap_font *f = font_of(res);
    f->shutting_down = true;
    cancel_reads(f);
}

static void op_free(shr__res *res) { font_free(font_of(res)); }

static const shr__res_ops font_ops = {op_resolve, op_frame_end, op_pump, op_has_work, op_deadline,
                                      op_io_done, op_shutdown, op_free};

shr_status shr_pl_res_bitmap_font_desc_init(shr_pl_res_bitmap_font_desc *desc) {
    if (!desc) return SHR_E_INVALID_ARG;
    *desc = (shr_pl_res_bitmap_font_desc){0};
    return SHR_OK;
}

shr_status shr_pl_res_bitmap_font_create(shr_context *ctx, const shr_pl_res_bitmap_font_desc *desc,
                                         shr_pl_res_bitmap_font **out) {
    static const char *const locales[] = {"ko", "ja", "zh-Hans", "zh-Hant", "zh-HK"};
    if (out) *out = NULL;
    if (!ctx || !desc || !out) return SHR_E_INVALID_ARG;
    const char *locale = desc->locale ? desc->locale : "ko";
    size_t k = 0;
    while (k < sizeof(locales) / sizeof(locales[0]) && strcmp(locale, locales[k])) k++;
    if (k == sizeof(locales) / sizeof(locales[0])) return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    const shr__alloc *al = shr__ctx_alloc(ctx);
    shr_pl_res_bitmap_font *f = SHR_NEW(al, shr_pl_res_bitmap_font);
    if (!f) return SHR_E_NO_MEMORY;
    const shr_context_desc *cd = shr__ctx_desc(ctx);
    f->al = *al;
    f->res.ctx = ctx;
    f->user = desc->user;
    f->open = desc->open;
    f->locale = locales[k];
    f->cache_bytes = cd->page_cache_bytes;
    f->retry_ns = cd->io_retry_ns;
    f->retry_limit = cd->io_retry_limit;
    f->wake = 1;
    f->ready_seen = shr__ctx_asset_ready(ctx);
    SHR_VEC_INIT(&f->wants, shr__page *);
    SHR_VEC_INIT(&f->clusters, shr__cluster);
    SHR_VEC_INIT(&f->pool, uint32_t);
    for (int r = 0; r < ROLE_COUNT; r++) f->pkg[r] = (shr__pkg){.font = f, .role = (uint8_t)r};
    shr__pkg *builtin = &f->pkg[ROLE_BUILTIN];
    builtin->src.data = shr__builtin_package;
    builtin->src.size = shr__builtin_package_size;
    const char *why;
    shr__pkg_map(builtin, shr__builtin_package, shr__builtin_package_size, &why); /* checked by the build */
    shr__res_attach(ctx, &f->res, &font_ops);                                     /* not refused: checked above */
    *out = f;
    return SHR_OK;
}

shr_status shr_pl_res_bitmap_font_destroy(shr_pl_res_bitmap_font *font) {
    if (!font) return SHR_E_INVALID_ARG;
    /* Outside callbacks a refused context is shutting down, which the font already follows. */
    if (font->res.users || shr__ctx_in_callback(font->res.ctx)) return SHR_E_STATE;
    if (!font->shutting_down) cancel_reads(font);
    font->res.dead = true;
    return SHR_OK;
}

shr__res *shr__bitmap_font_res(shr_pl_res_bitmap_font *font) { return &font->res; }

static shr_status intern(shr_pl_res_bitmap_font *f, const uint32_t *cps, size_t n, const shr__cluster_class *cls,
                         uint32_t *out) {
    if (n > UINT8_MAX) return SHR_E_LIMIT;
    uint64_t h = XXH3_64bits(cps, n * sizeof(uint32_t));
    bool full = f->clusters.len >= SHR_FONT_MAX_CLUSTERS;
    if (!full && (f->clusters.len + 1) * 2 > f->nslots) {
        size_t cap = f->nslots ? f->nslots * 2 : 64;
        uint32_t *slots = shr__calloc(&f->al, cap, sizeof(uint32_t), SHR_ALIGNOF(uint32_t), SHR_ALLOC_DESCRIPTOR);
        if (!slots) return SHR_E_NO_MEMORY;
        for (size_t k = 0; k < f->clusters.len; k++) {
            size_t j = (size_t)SHR_VEC_AT(&f->clusters, shr__cluster, k)->hash & (cap - 1);
            while (slots[j]) j = (j + 1) & (cap - 1);
            slots[j] = (uint32_t)k + 1;
        }
        shr__free(&f->al, f->slots, f->nslots * sizeof(uint32_t), SHR_ALIGNOF(uint32_t), SHR_ALLOC_DESCRIPTOR);
        f->slots = slots;
        f->nslots = cap;
    }
    size_t mask = f->nslots - 1, i = (size_t)h & mask;
    for (; f->slots[i]; i = (i + 1) & mask) {
        const shr__cluster *c = SHR_VEC_AT(&f->clusters, shr__cluster, f->slots[i] - 1);
        if (c->n == n && !memcmp(SHR_VEC_AT(&f->pool, uint32_t, c->off), cps, n * sizeof(uint32_t))) {
            *out = f->slots[i] - 1;
            return SHR_OK;
        }
    }
    if (full) return SHR_E_LIMIT;
    if (!shr__vec_reserve(&f->clusters, &f->al, 1) || !shr__vec_reserve(&f->pool, &f->al, n)) return SHR_E_NO_MEMORY;
    shr__cluster *c = shr__vec_push(&f->clusters, &f->al);
    *c = (shr__cluster){h, (uint32_t)f->pool.len, (uint8_t)n, cls->glyph_kind, cls->glyph_cp};
    memcpy(SHR_VEC_AT(&f->pool, uint32_t, f->pool.len), cps, n * sizeof(uint32_t));
    f->pool.len += n;
    f->slots[i] = (uint32_t)f->clusters.len;
    *out = (uint32_t)f->clusters.len - 1;
    return SHR_OK;
}

shr_status shr__bitmap_font_glyph(shr_pl_res_bitmap_font *font, const uint32_t *cps, size_t n,
                                  const shr__cluster_class *cls, uint32_t style_flags, uint64_t *out_id) {
    if (!font || !cps || !n || !cls || !out_id || cps[0] > 0x10FFFF) return SHR_E_INVALID_ARG;
    if (cls->glyph_kind == SHR_GLYPH_NONE) return SHR_E_NOT_FOUND;
    uint64_t style = (style_flags & SHR_STYLE_BOLD ? 1u : 0u) | (style_flags & SHR_STYLE_ITALIC ? 2u : 0u);
    uint64_t id = style << SHR_ID_STYLE_SHIFT;
    if (cls->glyph_kind == SHR_GLYPH_REPLACEMENT) {
        *out_id = id | 0xFFFD;
        return SHR_OK;
    }
    if (cls->flags & SHR_CLUSTER_EMOJI) id |= SHR_ID_EMOJI;
    if (n == 1) {
        *out_id = id | cps[0];
        return SHR_OK;
    }
    uint32_t index;
    shr_status st = intern(font, cps, n, cls, &index);
    if (st == SHR_E_LIMIT) { /* the table is full: the visible base, else U+FFFD */
        *out_id = cls->glyph_kind == SHR_GLYPH_SCALAR ? id | cls->glyph_cp : (style << SHR_ID_STYLE_SHIFT) | 0xFFFD;
        return SHR_OK;
    }
    if (st == SHR_OK) *out_id = id | SHR_ID_CLUSTER | index;
    return st;
}

void shr__font_report(shr_pl_res_bitmap_font *f, shr_status st, const char *why) {
    f->changed = true;
    shr__ctx_log(f->res.ctx, st, why);
    shr__ctx_resource_failed(f->res.ctx, st);
}

void shr__retry_begin(shr_pl_res_bitmap_font *f, shr__retry *r) {
    r->ready = shr__ctx_asset_ready(f->res.ctx);
    r->at = r->wake = 0;
}

static uint64_t later(const shr_pl_res_bitmap_font *f, uint64_t now) {
    return f->retry_ns ? shr__sat_add(now, f->retry_ns) : UINT64_MAX;
}

void shr__retry_refused(shr_pl_res_bitmap_font *f, shr__retry *r, uint64_t now) { r->at = later(f, now); }

void shr__retry_memory(shr_pl_res_bitmap_font *f, shr__retry *r) {
    r->wake = f->wake;
    r->frame = f->frame;
}

bool shr__retry_due(const shr_pl_res_bitmap_font *f, const shr__retry *r, uint64_t now) {
    if (r->wake) return r->wake != f->wake || f->frame > r->frame; /* waiting for memory */
    return r->at <= now || r->ready != shr__ctx_asset_ready(f->res.ctx);
}

bool shr__retry_failed(shr_pl_res_bitmap_font *f, shr__retry *r) {
    r->at = shr__sat_add(shr__font_now(f), f->retry_ns);
    if (++r->count <= f->retry_limit) return false;
    r->count = 0;
    return true;
}

void shr__retry_cool(shr_pl_res_bitmap_font *f, shr__retry *r, shr_status st, const char *why) {
    bool give_up = ++r->cools > f->retry_limit;
    r->cooling = true;
    r->at = give_up ? UINT64_MAX : later(f, shr__font_now(f)); /* else without io_retry_ns only a ready signal ends it */
    r->frame = UINT64_MAX;
    shr__font_report(f, st, give_up ? "font source keeps failing: retried at shr_asset_ready()" : why);
}

bool shr__retry_cooled(shr_pl_res_bitmap_font *f, shr__retry *r, uint64_t frame) {
    if (!r->cooling) return true;
    if (r->frame == UINT64_MAX) r->frame = frame;
    bool timed = f->retry_ns && r->cools <= f->retry_limit; /* else only a ready signal ends it */
    bool over = r->ready != shr__ctx_asset_ready(f->res.ctx) || (timed && r->at <= shr__font_now(f));
    if (frame > r->frame && over) {
        r->cooling = false;
        return true;
    }
    f->cool_due |= over;
    f->cool_wait = true;
    if (timed) f->cool_at = earlier(f->cool_at, r->at);
    return false;
}

int shr__seq_cmp_pool(const uint8_t *a, size_t an, const uint8_t *b, const uint32_t *bc, size_t bn) {
    for (size_t k = 0; k < an && k < bn; k++) {
        uint32_t x = shr__rd32(a + 4 * k), y = b ? shr__rd32(b + 4 * k) : bc[k];
        if (x != y) return x < y ? -1 : 1;
    }
    return an == bn ? 0 : (an < bn ? -1 : 1);
}
