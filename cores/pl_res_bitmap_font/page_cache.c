#include "font_core.h"

static void lru_remove(shr_pl_res_bitmap_font *f, shr__page *p) {
    shr__lru_remove(&f->lru, &p->lru);
    f->lru_bytes -= shr__page_length(p);
}

static void lru_push(shr_pl_res_bitmap_font *f, shr__page *p) {
    shr__lru_push(&f->lru, &p->lru);
    f->lru_bytes += shr__page_length(p);
}

static void page_free(shr_pl_res_bitmap_font *f, shr__page *p) {
    if (!p->pkg->mapped) shr__free(&f->al, (void *)p->data, shr__page_length(p), 8, SHR_ALLOC_PAYLOAD);
    p->pkg->pages[p->index] = NULL;
    SHR_DELETE(&f->al, p, shr__page);
}

/* Frees the descriptor of a page nothing refers to any more, with its failed reads unless it cools down. */
static void page_forget(shr_pl_res_bitmap_font *f, shr__page *p) {
    if (p->state == PAGE_ABSENT && !p->queued && !p->retry.cooling) page_free(f, p);
}

/* Drops the data of a read page that is not in a list. */
static void page_drop(shr_pl_res_bitmap_font *f, shr__page *p) {
    if (p->pkg->mapped) return;
    f->wake++;
    shr__free(&f->al, (void *)p->data, shr__page_length(p), 8, SHR_ALLOC_PAYLOAD);
    f->page_bytes -= shr__page_length(p);
    p->data = NULL;
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

void shr__page_done(shr__page *p, shr_status result) {
    shr_pl_res_bitmap_font *f = p->pkg->font;
    if (result == SHR_OK && shr__page_valid(p)) {
        p->state = PAGE_READY;
        p->retry.count = p->retry.cools = 0;
        if (!p->pkg->mapped) lru_push(f, p);
        f->changed = true;
        shr__ctx_trace(f->res.ctx, SHR_TRACE_PAGE_READY, shr__rd32(shr__page_rec(p) + 20), shr__page_length(p), 0);
        return;
    }
    page_drop(f, p);
    p->state = PAGE_ABSENT;
    if (result == SHR_OK && p->pkg->mapped)
        page_fail(p, SHR_E_CHECKSUM, "font page invalid");
    else if (!shr__retry_failed(f, &p->retry))
        return;
    else if (result == SHR_OK)
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
    uint32_t length = shr__rd32(rec + 16);
    if (pkg->mapped) {
        p->data = pkg->hdr + shr__rd64(rec);
        shr__page_done(p, SHR_OK);
        return;
    }
    if (length > f->cache_bytes) {
        page_fail(p, SHR_E_LIMIT, "font page larger than the page cache");
        return;
    }
    if (f->page_bytes - f->lru_bytes + length > f->cache_bytes) {
        shr__retry_memory(f, &p->retry); /* until pages are unpinned */
        return;
    }
    while (f->page_bytes + length > f->cache_bytes) {
        shr__page *victim = SHR_CONTAINER(shr__lru_oldest(f->lru), shr__page, lru);
        shr__ctx_trace(f->res.ctx, SHR_TRACE_PAGE_EVICT, shr__rd32(shr__page_rec(victim) + 20),
                       shr__page_length(victim), 0);
        lru_remove(f, victim);
        page_drop(f, victim);
        victim->state = PAGE_ABSENT;
        page_forget(f, victim);
    }
    uint8_t *data = shr__malloc(&f->al, length, 8, SHR_ALLOC_PAYLOAD);
    if (!data) {
        shr__retry_memory(f, &p->retry);
        return;
    }
    p->data = data;
    f->page_bytes += length;
    p->state = PAGE_LOADING;
    shr__retry_begin(f, &p->retry);
    shr_status st = shr__ctx_read(f->res.ctx, &f->res, &pkg->src, shr__rd64(rec), length, data, shr__io_tag(pkg, p->index + 1));
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
    const uint8_t *data, *entry; /* page payload, 16-byte glyph header inside it */
    shr__page *pin;              /* read pages are pinned by the frame */
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
    const uint8_t *rec = pkg->page_recs + SHR_PKG_ENTRY * (uint64_t)lo, *data;
    shr__page *p = NULL;
    if (role == ROLE_BUILTIN) { /* baked into the library: trusted */
        data = pkg->hdr + shr__rd64(rec);
    } else {
        p = pkg->pages ? pkg->pages[lo] : NULL;
        if (!p || p->state != PAGE_READY) return want_page(f, pkg, lo, frame);
        data = p->data;
    }
    *out = (hit){data, data + 4 + 16 * (slot - shr__rd32(rec + 20)), pkg->mapped ? NULL : p, pkg};
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
    uint8_t fmt = e[11] & 3;
    if (!fmt) return SHR_E_NOT_FOUND;
    shr__page *p = h.pin;
    if (p && p->pin[0] != frame && p->pin[1] != frame) {
        if (p->pin[0] && p->pin[1]) return SHR_E_LIMIT; /* more frames in flight than the compositor keeps */
        if (!p->pin[0] && !p->pin[1]) {
            lru_remove(f, p);
            shr__lru_push(&f->pinned, &p->lru);
        }
        p->pin[p->pin[0] ? 1 : 0] = frame;
    }
    out->image = (shr_image){h.data + shr__rd32(e), e[6], e[7], e[10], shr__rd16(e + 4),
                             fmt == 1 ? SHR_FORMAT_A4 : SHR_FORMAT_A8, SHR_MEMORY_CPU};
    out->offset = (shr_point){(int8_t)e[8], h.pkg->baseline - (int8_t)e[9]};
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
