#include "font_core.h"

static void lru_remove(shr_pl_res_bitmap_font *f, shr__page *p) {
    shr__lru_remove(&f->lru, &p->lru);
    f->lru_bytes -= p->slot->bytes;
}

static void lru_push(shr_pl_res_bitmap_font *f, shr__page *p) {
    shr__lru_push(&f->lru, &p->lru);
    f->lru_bytes += p->slot->bytes;
}

static shr_pixel_format pkg_format(const shr__pkg *pkg) { return pkg->format == 1 ? SHR_FORMAT_A4 : SHR_FORMAT_A8; }

static shr_rect atlas_rect(const shr__pkg *pkg) { return (shr_rect){0, 0, pkg->atlas_w, pkg->atlas_h}; }

static void slot_free(shr_pl_res_bitmap_font *f, shr__page_slot *s) {
    s->buf.mem.height = s->rows;
    shr__buf_free(f->res.ctx, &s->buf);
    SHR_DELETE(&f->al, s, shr__page_slot);
}

/* Driver memory of `rows` rows for the package's atlas; the buffer covers the atlas rows. */
static shr_status slot_new(shr_pl_res_bitmap_font *f, const shr__pkg *pkg, uint32_t rows, shr__page_slot **out) {
    shr__page_slot *s = SHR_NEW(&f->al, shr__page_slot);
    if (!s) return SHR_E_NO_MEMORY;
    shr_status st = shr__buf_alloc(f->res.ctx, pkg_format(pkg), pkg->atlas_w, (int32_t)rows, &s->buf);
    if (st != SHR_OK) {
        SHR_DELETE(&f->al, s, shr__page_slot);
        return st;
    }
    s->rows = (int32_t)rows;
    s->buf.mem.height = pkg->atlas_h;
    *out = s;
    return SHR_OK;
}

/* Rows of `stride` bytes into rows of `to` bytes, then `tail` bytes after them; in place when `to` >= `stride`. */
static void restride(uint8_t *dst, size_t to, const uint8_t *src, size_t stride, uint32_t rows, size_t tail) {
    memmove(dst + rows * to, src + rows * stride, tail);
    for (size_t y = rows; y > 0; y--) memmove(dst + (y - 1) * to, src + (y - 1) * stride, stride);
}

static void page_free(shr_pl_res_bitmap_font *f, shr__page *p) {
    if (p->slot) slot_free(f, p->slot);
    p->pkg->pages[p->index] = NULL;
    SHR_DELETE(&f->al, p, shr__page);
}

/* Frees the descriptor of a page nothing refers to any more, with its failed reads unless it cools down. */
static void page_forget(shr_pl_res_bitmap_font *f, shr__page *p) {
    if (p->state == PAGE_ABSENT && !p->queued && !p->retry.cooling) page_free(f, p);
}

/* Frees the slot of a page of the cache that is not in a list. */
static void page_drop(shr_pl_res_bitmap_font *f, shr__page *p) {
    f->wake++;
    f->page_bytes -= p->slot->bytes;
    slot_free(f, p->slot);
    p->slot = NULL;
}

/* A slot of the cache for page `p`: the slot of an evicted page if one fits, else a new one.
 * SHR_E_NO_MEMORY: wait until pages are unpinned or memory frees up. */
static shr_status slot_take(shr_pl_res_bitmap_font *f, shr__page *p, shr__page_slot **out) {
    const shr__pkg *pkg = p->pkg;
    uint64_t need = (uint64_t)pkg->slot_rows * pkg->stride;
    if (need > f->cache_bytes) return SHR_E_LIMIT;
    if (f->page_bytes - f->lru_bytes + need > f->cache_bytes) return SHR_E_NO_MEMORY;
    while (f->page_bytes + need > f->cache_bytes) {
        shr__page *victim = SHR_CONTAINER(shr__lru_oldest(f->lru), shr__page, lru);
        shr__page_slot *s = victim->slot;
        shr__ctx_trace(f->res.ctx, SHR_TRACE_PAGE_EVICT, shr__rd32(shr__page_rec(victim) + 20),
                       shr__page_length(victim), 0);
        lru_remove(f, victim);
        victim->state = PAGE_ABSENT;
        if (s->shape == pkg->slot_shape) {
            victim->slot = NULL;
            page_forget(f, victim);
            *out = s;
            return SHR_OK;
        }
        page_drop(f, victim);
        page_forget(f, victim);
    }
    shr_status st = slot_new(f, pkg, pkg->slot_rows, out);
    if (st != SHR_OK) return st;
    (*out)->shape = pkg->slot_shape;
    f->page_bytes += (*out)->bytes = (uint64_t)pkg->slot_rows * (*out)->buf.mem.stride;
    return SHR_OK;
}

/* Page `p` of mapped memory `d`, drawn from in place or else from a copy of its atlas in a slot of the cache
 * (`cached`) or of its own. */
static shr_status page_map(shr_pl_res_bitmap_font *f, shr__page *p, const uint8_t *d, bool cached) {
    const shr__pkg *pkg = p->pkg;
    size_t atlas = (size_t)pkg->atlas_h * pkg->stride;
    shr__page_slot *s = SHR_NEW(&f->al, shr__page_slot);
    if (!s) return SHR_E_NO_MEMORY;
    shr_image m = {d, pkg->atlas_w, pkg->atlas_h, pkg->stride, atlas, pkg_format(pkg), SHR_MEMORY_CPU};
    s->rows = pkg->atlas_h;
    if (shr__buf_wrap(f->res.ctx, &m, &s->buf) != SHR_OK) {
        SHR_DELETE(&f->al, s, shr__page_slot);
        shr_status st = cached ? slot_take(f, p, &s) : slot_new(f, pkg, pkg->atlas_h, &s);
        if (st != SHR_OK) return st;
        restride((uint8_t *)s->buf.mem.pixels, s->buf.mem.stride, d, pkg->stride, pkg->atlas_h, 0);
        shr__buf_changed(&s->buf, atlas_rect(pkg));
    }
    p->slot = s;
    p->recs = d + atlas;
    return SHR_OK;
}

/* The built-in package's pages, ready for the font's lifetime outside the page cache. */
shr_status shr__pages_builtin(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    pkg->pages = shr__calloc(&f->al, pkg->npages, sizeof(shr__page *), SHR_ALIGNOF(shr__page *), SHR_ALLOC_PAYLOAD);
    if (!pkg->pages) return SHR_E_NO_MEMORY;
    for (uint32_t i = 0; i < pkg->npages; i++) {
        shr__page *p = pkg->pages[i] = SHR_NEW(&f->al, shr__page);
        if (!p) return SHR_E_NO_MEMORY;
        *p = (shr__page){.pkg = pkg, .index = i, .state = PAGE_READY};
        shr_status st = page_map(f, p, pkg->hdr + shr__rd64(shr__page_rec(p)), false);
        if (st != SHR_OK) return st;
    }
    return SHR_OK;
}

/* With the package: no page is in a list (the package never became ready) or the font goes away. */
void shr__pages_free(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    for (uint32_t i = 0; pkg->pages && i < pkg->npages; i++)
        if (pkg->pages[i]) page_free(f, pkg->pages[i]);
    shr__free(&f->al, pkg->pages, pkg->npages * sizeof(shr__page *), SHR_ALIGNOF(shr__page *), SHR_ALLOC_PAYLOAD);
}

static void page_fail(shr__page *p, shr_status st, const char *why) {
    p->state = PAGE_FAILED;
    shr__font_report(p->pkg->font, st, why);
}

static void page_ready(shr_pl_res_bitmap_font *f, shr__page *p) {
    p->state = PAGE_READY;
    p->retry.count = p->retry.cools = 0;
    if (p->slot->bytes) lru_push(f, p);
    f->changed = true;
    shr__ctx_trace(f->res.ctx, SHR_TRACE_PAGE_READY, shr__rd32(shr__page_rec(p) + 20), shr__page_length(p), 0);
}

void shr__page_done(shr__page *p, shr_status result) {
    shr_pl_res_bitmap_font *f = p->pkg->font;
    const shr__pkg *pkg = p->pkg;
    shr_image *m = &p->slot->buf.mem;
    uint8_t *d = (uint8_t *)m->pixels;
    if (result == SHR_OK && shr__page_valid(p, d)) {
        size_t tail = 4 + 16ull * shr__rd32(shr__page_rec(p) + 24);
        if (m->stride != pkg->stride) restride(d, m->stride, d, pkg->stride, pkg->atlas_h, tail);
        p->recs = d + (size_t)pkg->atlas_h * m->stride;
        shr__buf_changed(&p->slot->buf, atlas_rect(pkg));
        page_ready(f, p);
        return;
    }
    page_drop(f, p);
    p->state = PAGE_ABSENT;
    if (!shr__retry_failed(f, &p->retry)) return;
    if (result == SHR_OK)
        page_fail(p, SHR_E_CHECKSUM, "font page invalid");
    else
        shr__retry_cool(f, &p->retry, result, "font page read failed");
}

bool shr__page_due(const shr__page *p, uint64_t now) {
    const shr_pl_res_bitmap_font *f = p->pkg->font;
    return p->want == f->frame && p->state == PAGE_ABSENT && shr__retry_due(f, &p->retry, now) &&
           (p->pkg->mapped || !f->blocked); /* cooling pages leave the wants in the pump that cools them */
}

static void page_load(shr_pl_res_bitmap_font *f, shr__page *p, uint64_t now) {
    shr__pkg *pkg = p->pkg;
    const uint8_t *rec = shr__page_rec(p);
    shr_status st;
    if (pkg->mapped) {
        const uint8_t *d = pkg->hdr + shr__rd64(rec);
        st = shr__page_valid(p, d) ? page_map(f, p, d, true) : SHR_E_CHECKSUM;
    } else {
        st = slot_take(f, p, &p->slot);
    }
    if (st == SHR_E_NO_MEMORY) {
        shr__retry_memory(f, &p->retry); /* until pages are unpinned */
        return;
    }
    if (st != SHR_OK) {
        page_fail(p, st, st == SHR_E_CHECKSUM ? "font page invalid" : "font page beyond the page cache or driver");
        return;
    }
    if (pkg->mapped) {
        page_ready(f, p);
        return;
    }
    p->state = PAGE_LOADING;
    shr__retry_begin(f, &p->retry);
    st = shr__ctx_read(f->res.ctx, &f->res, &pkg->src, shr__rd64(rec), shr__page_length(p),
                       (void *)p->slot->buf.mem.pixels, shr__io_tag(pkg, p->index + 1));
    if (st == SHR_OK) return;
    p->state = PAGE_ABSENT;
    page_drop(f, p);
    if (st == SHR_E_LIMIT)
        f->blocked = true;
    else /* SHR_E_WOULD_BLOCK */
        shr__retry_refused(f, &p->retry, now);
}

/* Loads the pages the latest frame wanted; forgets the others. */
void shr__pages_schedule(shr_pl_res_bitmap_font *f) {
    uint64_t now = shr__font_now(f);
    size_t k = 0;
    for (size_t i = 0; i < f->wants.len; i++) {
        shr__page *p = *SHR_VEC_AT(&f->wants, shr__page *, i);
        if (p->want != f->frame || p->state == PAGE_READY || p->state == PAGE_FAILED || p->retry.cooling) {
            p->queued = false;
            page_forget(f, p);
            continue;
        }
        *SHR_VEC_AT(&f->wants, shr__page *, k++) = p;
        if (shr__page_due(p, now)) page_load(f, p, now);
    }
    f->wants.len = k;
}

static int64_t cmap_find(const shr__pkg *pkg, uint32_t cp) {
    uint32_t lo = 0, hi = pkg->ncmap;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, v = shr__rd32(pkg->cmap + 8ull * mid);
        if (v == cp) return shr__rd32(pkg->cmap + 8ull * mid + 4);
        if (v < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

static int64_t seq_find(const shr__pkg *pkg, const uint32_t *cps, size_t n) {
    uint32_t lo = 0, hi = pkg->nseqs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *r = pkg->seqs + 12ull * mid;
        int c = shr__seq_cmp_pool(pkg->pool + 4ull * shr__rd32(r + 4), r[0], NULL, cps, n);
        if (c == 0) return shr__rd32(r + 8);
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

typedef struct hit {
    const uint8_t *entry; /* glyph record */
    shr__page *page;
    const shr__pkg *pkg;
} hit;

/* A page the frame needs: loaded by the next pump unless it failed or cools down. */
static int want_page(shr_pl_res_bitmap_font *f, shr__pkg *pkg, uint32_t index, uint64_t frame) {
    if (!pkg->pages &&
        !(pkg->pages = shr__calloc(&f->al, pkg->npages, sizeof(shr__page *), SHR_ALIGNOF(shr__page *), SHR_ALLOC_PAYLOAD)))
        return GLYPH_NO_MEMORY;
    shr__page *p = pkg->pages[index];
    if (!p) {
        if (!(p = SHR_NEW(&f->al, shr__page))) return GLYPH_NO_MEMORY;
        *p = (shr__page){.pkg = pkg, .index = index};
        pkg->pages[index] = p;
    }
    if (p->state == PAGE_FAILED) return GLYPH_MISSING;
    if (!shr__retry_cooled(f, &p->retry, frame)) return GLYPH_PENDING; /* drawn provisionally until it ends */
    if (!p->queued) {
        shr__page **slot = shr__vec_push(&f->wants, &f->al);
        if (!slot) {
            page_forget(f, p);
            return GLYPH_NO_MEMORY;
        }
        *slot = p;
        p->queued = true;
    }
    p->want = frame;
    return GLYPH_PENDING;
}

static int try_package(shr_pl_res_bitmap_font *f, int role, const uint32_t *cps, size_t n, uint64_t frame,
                       hit *out) {
    shr__pkg *pkg = &f->pkg[role];
    if (pkg->state == PKG_UNOPENED) {
        if (f->shutting_down) return GLYPH_MISSING;
        if (!shr__retry_cooled(f, &pkg->retry, frame)) return GLYPH_PENDING; /* drawn provisionally until it ends */
        pkg->wanted = true;
        return GLYPH_PENDING;
    }
    if (pkg->state == PKG_LOADING) return GLYPH_PENDING;
    if (pkg->state != PKG_READY) return GLYPH_MISSING;
    int64_t slot = n == 1 ? cmap_find(pkg, cps[0]) : seq_find(pkg, cps, n);
    if (slot < 0) return GLYPH_MISSING;
    uint32_t lo = 0, hi = pkg->npages;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (shr__rd32(pkg->page_recs + SHR_PKG_ENTRY * (uint64_t)mid + 20) <= slot)
            lo = mid;
        else
            hi = mid;
    }
    shr__page *p = pkg->pages ? pkg->pages[lo] : NULL; /* the built-in pages are always ready */
    if (!p || p->state != PAGE_READY) return want_page(f, pkg, lo, frame);
    *out = (hit){p->recs + 4 + 16 * (slot - shr__rd32(pkg->page_recs + SHR_PKG_ENTRY * (uint64_t)lo + 20)), p, pkg};
    return GLYPH_READY;
}

static int text_chain(shr_pl_res_bitmap_font *f, const uint32_t *cps, size_t n, uint64_t frame, hit *out) {
    int order[5], k = 0;
    uint32_t props = shr__uprops(cps[0]);
    if (n == 1 && (props & SHR_UP_NERD)) order[k++] = ROLE_NERD;
    if (props & SHR_UP_CJK)
        order[k++] = ROLE_CJK, order[k++] = ROLE_LATIN;
    else
        order[k++] = ROLE_LATIN, order[k++] = ROLE_CJK;
    order[k++] = ROLE_SYMBOLS;
    order[k++] = ROLE_BUILTIN;
    for (int i = 0; i < k; i++) {
        int r = try_package(f, order[i], cps, n, frame, out);
        if (r != GLYPH_MISSING) return r;
    }
    return GLYPH_MISSING;
}

shr_status shr__font_resolve(shr_pl_res_bitmap_font *f, uint64_t id, uint64_t frame, shr__resolved *out) {
    uint32_t one = (uint32_t)(id & SHR_ID_VALUE), fffd = 0xFFFD, base = one;
    const uint32_t *cps = &one;
    size_t n = 1;
    uint8_t kind = SHR_GLYPH_SCALAR;
    if (id & SHR_ID_CLUSTER) {
        if (one >= f->clusters.len) return SHR_E_INVALID_ARG;
        const shr__cluster *c = SHR_VEC_AT(&f->clusters, shr__cluster, one);
        cps = SHR_VEC_AT(&f->pool, uint32_t, c->off);
        n = c->n, kind = c->kind, base = c->cp;
    }
    if (frame > f->frame) f->frame = frame;
    hit h = {0};
    int r;
    if (id & SHR_ID_EMOJI) { /* the emoji glyph, else the text glyph of a single visible scalar */
        r = try_package(f, ROLE_EMOJI, cps, n, frame, &h);
        if (r == GLYPH_MISSING && n > 1 && kind == SHR_GLYPH_SCALAR)
            r = try_package(f, ROLE_EMOJI, &base, 1, frame, &h);
        if (r == GLYPH_MISSING && kind == SHR_GLYPH_SCALAR) r = text_chain(f, &base, 1, frame, &h);
    } else {
        r = text_chain(f, cps, n, frame, &h);
        if (r == GLYPH_MISSING && n > 1 && kind == SHR_GLYPH_SCALAR) r = text_chain(f, &base, 1, frame, &h);
    }
    if (r == GLYPH_MISSING) r = text_chain(f, &fffd, 1, frame, &h);
    if (r == GLYPH_NO_MEMORY) return SHR_E_NO_MEMORY;
    out->provisional = r == GLYPH_PENDING;
    if (r == GLYPH_PENDING) {
        r = n == 1 ? try_package(f, ROLE_BUILTIN, cps, 1, frame, &h) : GLYPH_MISSING;
        if (r != GLYPH_READY) try_package(f, ROLE_BUILTIN, &fffd, 1, frame, &h);
    }
    const uint8_t *e = h.entry; /* found: the built-in package always has U+FFFD */
    if (!(e[8] & 3)) return SHR_E_NOT_FOUND;
    shr__page *p = h.page;
    if (p->slot->bytes && p->pin[0] != frame && p->pin[1] != frame) { /* pages of the cache may be evicted */
        if (p->pin[0] && p->pin[1]) return SHR_E_LIMIT; /* more frames in flight than the compositor keeps */
        if (!p->pin[0] && !p->pin[1]) {
            lru_remove(f, p);
            shr__lru_push(&f->pinned, &p->lru);
        }
        p->pin[p->pin[0] ? 1 : 0] = frame;
    }
    int32_t x = shr__rd16(e), y = shr__rd16(e + 2);
    out->buf = &p->slot->buf;
    out->rect = (shr_rect){x, y, x + e[4], y + e[5]};
    out->offset = (shr_point){(int8_t)e[6], h.pkg->baseline - (int8_t)e[7]};
    /* Emoji and Nerd glyphs, and box drawing and block characters (which join their neighbours), stay as drawn. */
    bool plain = h.pkg->role == ROLE_EMOJI || h.pkg->role == ROLE_NERD || (base >= 0x2500 && base < 0x25A0) ||
                 (base >= 0x1FB00 && base < 0x1FC00);
    out->synth = plain ? 0 : SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC;
    out->slant_axis = SHR_CELL_HEIGHT - 2 * out->offset.y;
    return SHR_OK;
}

void shr__font_frame_end(shr_pl_res_bitmap_font *f, uint64_t frame) {
    for (shr__lru_node *n = f->pinned, *next; n; n = next) {
        next = n->next;
        shr__page *p = SHR_CONTAINER(n, shr__page, lru);
        for (int k = 0; k < 2; k++)
            if (p->pin[k] == frame) p->pin[k] = 0;
        if (p->pin[0] || p->pin[1]) continue;
        shr__lru_remove(&f->pinned, n);
        lru_push(f, p);
        f->wake++;
    }
}
