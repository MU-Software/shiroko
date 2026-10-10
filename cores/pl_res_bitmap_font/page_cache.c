#include "font_core.h"


static shr_pixel_format pkg_format(const shr__pkg *pkg) { return pkg->format == 1 ? SHR_FORMAT_A4 : SHR_FORMAT_A8; }

static void slot_free(shr_pl_res_bitmap_font *f, shr__page_slot *s) {
    shr__buf_free(f->res.ctx, &s->buf);
    SHR_DELETE(&f->al, s, shr__page_slot);
}

/* Driver memory of the package's page shape. */
static shr_status slot_new(shr_pl_res_bitmap_font *f, const shr__pkg *pkg, shr__page_slot **out) {
    shr__page_slot *s = SHR_NEW(&f->al, shr__page_slot);
    if (!s) return SHR_E_NO_MEMORY;
    shr_status st = shr__buf_alloc(f->res.ctx, pkg_format(pkg), pkg->atlas_w, pkg->atlas_h, &s->buf);
    if (st != SHR_OK) {
        SHR_DELETE(&f->al, s, shr__page_slot);
        return st;
    }
    *out = s;
    return SHR_OK;
}

/* `n` bytes of the page, charged to the page cache with a slot of it. */
static uint8_t *page_alloc(shr_pl_res_bitmap_font *f, shr__page *p, size_t n) {
    uint8_t *m = shr__malloc(&f->al, n, 8, SHR_ALLOC_PAYLOAD);
    uint64_t c = n * (uint64_t)(p->slot->bytes != 0);
    if (m) f->page_bytes += c, p->bytes += c;
    return m;
}

/* Frees the page's records, staging buffer and slot (the page is in no list). */
static void page_unload(shr_pl_res_bitmap_font *f, shr__page *p) {
    f->page_bytes -= p->bytes;
    shr__free(&f->al, p->own, shr__page_recs(p), 8, SHR_ALLOC_PAYLOAD);
    shr__free(&f->al, p->stage, shr__page_length(p), 8, SHR_ALLOC_PAYLOAD);
    p->own = p->stage = NULL, p->recs = NULL, p->bytes = 0;
    if (!p->slot) return;
    f->wake++;
    f->page_bytes -= p->slot->bytes;
    slot_free(f, p->slot);
    p->slot = NULL;
}

static void page_free(shr_pl_res_bitmap_font *f, shr__page *p) {
    if (p->slot) slot_free(f, p->slot);
    shr__free(&f->al, p->own, shr__page_recs(p), 8, SHR_ALLOC_PAYLOAD);
    shr__free(&f->al, p->stage, shr__page_length(p), 8, SHR_ALLOC_PAYLOAD);
    p->pkg->pages[p->index] = NULL;
    SHR_DELETE(&f->al, p, shr__page);
}

/* Frees the descriptor of a page nothing refers to any more, with its failed reads unless it cools down. */
static void page_forget(shr_pl_res_bitmap_font *f, shr__page *p) {
    if (p->state == PAGE_ABSENT && !p->queued && !p->retry.cooling) page_free(f, p);
}

/* A slot of the cache for page `p` with room for `extra` bytes of it: the slot of an evicted page of the same shape if
 * there is one, else a new one. Only the oldest unpinned pages the latest frame neither drew from nor wanted are
 * evicted, so the pages of one frame never evict each other. SHR_E_NO_MEMORY: wait until pages are unpinned, a later
 * frame wants glyphs or memory frees up. */
static shr_status slot_take(shr_pl_res_bitmap_font *f, shr__page *p, uint64_t extra, shr__page_slot **out) {
    const shr__pkg *pkg = p->pkg;
    uint64_t need = (uint64_t)pkg->atlas_h * pkg->stride + extra, spare = 0;
    shr__page_slot *s = NULL;
    if (need > f->cache_bytes) return SHR_E_LIMIT;
    for (shr__lru_node *n = shr__lru_oldest(f->lru); n; n = n == f->lru ? NULL : n->prev) {
        const shr__page *q = SHR_CONTAINER(n, shr__page, lru);
        if (q->want == f->frame) break;
        spare += q->slot->bytes + q->bytes;
    }
    if (f->page_bytes - spare + need > f->cache_bytes) return SHR_E_NO_MEMORY;
    while (f->page_bytes + need > f->cache_bytes) {
        shr__page *victim = SHR_CONTAINER(shr__lru_oldest(f->lru), shr__page, lru);
        shr__ctx_trace(f->res.ctx, SHR_TRACE_PAGE_EVICT, victim->index, shr__page_length(victim), 0);
        shr__lru_remove(&f->lru, &victim->lru);
        victim->state = PAGE_ABSENT;
        if (!s && victim->slot->shape == pkg->slot_shape) {
            s = victim->slot, victim->slot = NULL;
            f->page_bytes -= s->bytes;
        }
        page_unload(f, victim);
        page_forget(f, victim);
    }
    if (!s) {
        shr_status st = slot_new(f, pkg, &s);
        if (st != SHR_OK) return st;
        s->shape = pkg->slot_shape;
        s->bytes = (uint64_t)pkg->atlas_h * s->buf.mem.stride;
    }
    f->page_bytes += s->bytes;
    *out = s;
    return SHR_OK;
}

/* Page `p` from its page box `b` (its checksum checked): a stored page of a mapped package drawn from in place when
 * the driver can, else decoded into the top rows of its slot (taken now from the cache, `cached`, or of its own) and
 * its records. Leaves what it took to the caller on failure. */
static shr_status page_fill(shr_pl_res_bitmap_font *f, shr__page *p, const uint8_t *b, bool cached, const char **why) {
    const shr__pkg *pkg = p->pkg;
    uint32_t height = shr__page_height(p);
    shr__streams s;
    *why = "font page invalid";
    shr_status st = shr__page_streams(p, b, &s);
    if (st != SHR_OK) return st;
    if (pkg->mapped & (s.method == 0)) {
        if (!shr__page_valid(p, s.atlas, s.recs)) return SHR_E_FORMAT;
        shr__page_slot *w = SHR_NEW(&f->al, shr__page_slot);
        if (!w) return SHR_E_NO_MEMORY;
        shr_image m = {s.atlas, pkg->atlas_w, (int32_t)height, pkg->stride, s.atlas_n, pkg_format(pkg), SHR_MEMORY_CPU};
        if (shr__buf_wrap(f->res.ctx, &m, &w->buf) == SHR_OK) {
            p->slot = w, p->recs = s.recs;
            return SHR_OK;
        }
        SHR_DELETE(&f->al, w, shr__page_slot);
    }
    *why = "font page beyond the page cache or driver";
    if (!p->slot && (st = cached ? slot_take(f, p, shr__page_recs(p), &p->slot) : slot_new(f, pkg, &p->slot)))
        return st;
    if (!p->own && !(p->own = page_alloc(f, p, shr__page_recs(p)))) return SHR_E_NO_MEMORY;
    *why = "font page invalid";
    shr__buf *buf = &p->slot->buf;
    uint8_t *px = (uint8_t *)buf->mem.pixels;
    size_t to = buf->mem.stride;
    if (!shr__page_decode(f, p, &s, px, p->own)) return SHR_E_FORMAT;
    for (size_t y = height; y > 0; y--) memmove(px + (y - 1) * to, px + (y - 1) * pkg->stride, pkg->stride);
    memset(px + height * to, 0, (pkg->atlas_h - height) * to);
    shr__buf_changed(buf, (shr_rect){0, 0, pkg->atlas_w, pkg->atlas_h});
    p->recs = p->own;
    return SHR_OK;
}

/* The built-in package's pages, ready for the font's lifetime outside the page cache. */
shr_status shr__pages_builtin(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    for (uint32_t i = 0; i < pkg->npages; i++) {
        shr__page *p = pkg->pages[i] = SHR_NEW(&f->al, shr__page);
        if (!p) return SHR_E_NO_MEMORY;
        *p = (shr__page){.pkg = pkg, .index = i, .state = PAGE_READY};
        const char *why;
        shr_status st = page_fill(f, p, pkg->hdr + shr__rd64(shr__page_rec(p)), false, &why);
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
    if (p->slot->bytes) shr__lru_push(&f->lru, &p->lru);
    f->changed = true;
    shr__ctx_trace(f->res.ctx, SHR_TRACE_PAGE_READY, p->index, shr__page_length(p), 0);
}

/* A read page: a checksum mismatch is re-read like an I/O error, a page that does not decode or validate fails. */
void shr__page_done(shr__page *p, shr_status result) {
    shr_pl_res_bitmap_font *f = p->pkg->font;
    const uint8_t *e = shr__page_rec(p);
    const char *why = "font page read failed";
    if (result == SHR_OK && XXH3_64bits(p->stage, shr__rd32(e + 16)) != shr__rd64(e + 8)) result = SHR_E_CHECKSUM;
    shr_status st = result == SHR_OK ? page_fill(f, p, p->stage, true, &why) : result;
    if (st == SHR_OK) {
        f->page_bytes -= shr__rd32(e + 16), p->bytes -= shr__rd32(e + 16);
        shr__free(&f->al, p->stage, shr__rd32(e + 16), 8, SHR_ALLOC_PAYLOAD);
        p->stage = NULL;
        page_ready(f, p);
        return;
    }
    page_unload(f, p);
    p->state = PAGE_ABSENT;
    if (result == SHR_OK) {
        page_fail(p, st, why);
        return;
    }
    if (!shr__retry_failed(f, &p->retry)) return;
    if (result == SHR_E_CHECKSUM)
        page_fail(p, result, "font page checksum");
    else
        shr__retry_cool(f, &p->retry, result, why);
}

bool shr__page_due(const shr__page *p, uint64_t now) {
    const shr_pl_res_bitmap_font *f = p->pkg->font;
    return p->want == f->frame && p->state == PAGE_ABSENT && shr__retry_due(f, &p->retry, now) &&
           (p->pkg->mapped || !f->blocked); /* cooling pages leave the wants in the pump that cools them */
}

static void page_load(shr_pl_res_bitmap_font *f, shr__page *p, uint64_t now) {
    shr__pkg *pkg = p->pkg;
    const uint8_t *e = shr__page_rec(p);
    uint32_t size = shr__rd32(e + 16);
    const char *why = "font page checksum";
    shr_status st = SHR_E_CHECKSUM;
    if (pkg->mapped) {
        const uint8_t *b = pkg->hdr + shr__rd64(e);
        if (XXH3_64bits(b, size) == shr__rd64(e + 8)) st = page_fill(f, p, b, true, &why);
    } else {
        why = "font page beyond the page cache or driver";
        st = slot_take(f, p, shr__page_recs(p) + size, &p->slot);
        if (st == SHR_OK && (!(p->own = page_alloc(f, p, shr__page_recs(p))) || !(p->stage = page_alloc(f, p, size))))
            st = SHR_E_NO_MEMORY;
    }
    if (st != SHR_OK) page_unload(f, p);
    if (st == SHR_E_NO_MEMORY) {
        shr__retry_memory(f, &p->retry); /* until pages are unpinned */
        return;
    }
    if (st != SHR_OK) {
        page_fail(p, st, why);
        return;
    }
    if (pkg->mapped) {
        page_ready(f, p);
        return;
    }
    p->state = PAGE_LOADING;
    shr__retry_begin(f, &p->retry);
    st = shr__ctx_read(f->res.ctx, &f->res, &pkg->src, shr__rd64(e), size, p->stage, shr__io_tag(pkg, p->index + 1));
    if (st == SHR_OK) return;
    p->state = PAGE_ABSENT;
    page_unload(f, p);
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

int shr__preload_role(const shr_pl_res_bitmap_font *f) {
    if (f->wants.len || f->preloading || f->delivered) return 0; /* frames first, one page per pump */
    for (int r = ROLE_LATIN; r < ROLE_COUNT; r++) {
        const shr__pkg *pkg = &f->pkg[r];
        if (pkg->state == PKG_READY && pkg->preload_next < pkg->preload && pkg->preload_next < pkg->npages &&
            (pkg->mapped || !f->blocked))
            return r;
    }
    return 0;
}

/* Starts the next page of a preload unless it exists already or the cache has no room for it without evicting
 * (which ends the preload); a page whose read cannot start is left to the frames. */
void shr__pages_preload(shr_pl_res_bitmap_font *f) {
    int role = shr__preload_role(f);
    if (!role) return;
    shr__pkg *pkg = &f->pkg[role];
    uint32_t i = pkg->preload_next++;
    const uint8_t *e = pkg->ptab + SHR_PKG_PTAB * (uint64_t)i;
    uint64_t need = (uint64_t)pkg->atlas_h * pkg->stride + 16ull * shr__rd16(e + 20) + shr__rd32(e + 16) * !pkg->mapped;
    if (f->page_bytes + need > f->cache_bytes) pkg->preload = 0;
    if (pkg->pages[i] || !pkg->preload) return;
    shr__page *p = SHR_NEW(&f->al, shr__page);
    if (!p) return;
    *p = (shr__page){.pkg = pkg, .index = i};
    pkg->pages[i] = p;
    page_load(f, p, shr__font_now(f));
    if (p->state == PAGE_LOADING) f->preloading = p;
    page_forget(f, p);
}

static uint32_t seq_find(const shr__pkg *pkg, const uint32_t *cps, size_t n) {
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
    return SHR_GID_MISS;
}

typedef struct hit {
    const uint8_t *entry; /* glyph record */
    shr__page *page;
    const shr__pkg *pkg;
} hit;

/* A page the frame needs: loaded by the next pump unless it failed or cools down. */
static int want_page(shr_pl_res_bitmap_font *f, shr__pkg *pkg, uint32_t index, uint64_t frame) {
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

/* The glyph of `cps`: a scalar, or the scalars of cluster `c` (looked up once per ready package). */
static int try_package(shr_pl_res_bitmap_font *f, int role, const uint32_t *cps, size_t n, shr__cluster *c,
                       uint64_t frame, hit *out) {
    shr__pkg *pkg = &f->pkg[role];
    if (pkg->state == PKG_UNOPENED) {
        if (f->shutting_down) return GLYPH_MISSING;
        if (!shr__retry_cooled(f, &pkg->retry, frame)) return GLYPH_PENDING; /* drawn provisionally until it ends */
        pkg->wanted = true;
        return GLYPH_PENDING;
    }
    if (pkg->state == PKG_LOADING) return GLYPH_PENDING;
    if (pkg->state != PKG_READY) return GLYPH_MISSING;
    uint32_t gid = n == 1 ? shr__ctri_get(pkg, cps[0]) : c->gid[role];
    if (gid == SHR_GID_UNKNOWN) gid = c->gid[role] = seq_find(pkg, cps, n);
    if (gid == SHR_GID_MISS) return GLYPH_MISSING;
    if (gid == SHR_GID_BLANK) return GLYPH_BLANK;
    shr__page *p = pkg->pages[gid >> 12]; /* the built-in pages are always ready */
    if (!p || p->state != PAGE_READY) return want_page(f, pkg, gid >> 12, frame);
    *out = (hit){p->recs + 16 * (gid & 4095), p, pkg};
    return GLYPH_READY;
}

static int text_chain(shr_pl_res_bitmap_font *f, const uint32_t *cps, size_t n, shr__cluster *c, uint64_t frame,
                      hit *out) {
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
        int r = try_package(f, order[i], cps, n, c, frame, out);
        if (r != GLYPH_MISSING) return r;
    }
    return GLYPH_MISSING;
}

shr_status shr__font_resolve(shr_pl_res_bitmap_font *f, uint64_t id, uint64_t frame, shr__resolved *out) {
    uint32_t one = (uint32_t)(id & SHR_ID_VALUE), fffd = 0xFFFD, base = one;
    const uint32_t *cps = &one;
    size_t n = 1;
    uint8_t kind = SHR_GLYPH_SCALAR;
    shr__cluster *c = NULL;
    if (id & SHR_ID_CLUSTER) {
        if (one >= f->clusters.len) return SHR_E_INVALID_ARG;
        c = SHR_VEC_AT(&f->clusters, shr__cluster, one);
        cps = SHR_VEC_AT(&f->pool, uint32_t, c->off);
        n = c->n, kind = c->kind, base = c->cp;
    }
    if (frame > f->frame) f->frame = frame;
    hit h = {0};
    int r;
    if (id & SHR_ID_EMOJI) { /* the emoji glyph, else the text glyph of a single visible scalar */
        r = try_package(f, ROLE_EMOJI, cps, n, c, frame, &h);
        if (r == GLYPH_MISSING && n > 1 && kind == SHR_GLYPH_SCALAR)
            r = try_package(f, ROLE_EMOJI, &base, 1, NULL, frame, &h);
        if (r == GLYPH_MISSING && kind == SHR_GLYPH_SCALAR) r = text_chain(f, &base, 1, NULL, frame, &h);
    } else {
        r = text_chain(f, cps, n, c, frame, &h);
        if (r == GLYPH_MISSING && n > 1 && kind == SHR_GLYPH_SCALAR) r = text_chain(f, &base, 1, NULL, frame, &h);
    }
    if (r == GLYPH_MISSING) r = text_chain(f, &fffd, 1, NULL, frame, &h);
    if (r == GLYPH_NO_MEMORY) return SHR_E_NO_MEMORY;
    out->provisional = r == GLYPH_PENDING;
    if (r == GLYPH_PENDING) {
        r = n == 1 ? try_package(f, ROLE_BUILTIN, cps, 1, NULL, frame, &h) : GLYPH_MISSING;
        if (r == GLYPH_MISSING) r = try_package(f, ROLE_BUILTIN, &fffd, 1, NULL, frame, &h);
    }
    if (r == GLYPH_BLANK) return SHR_E_NOT_FOUND; /* draws nothing: no page */
    const uint8_t *e = h.entry; /* found: the built-in package always has U+FFFD */
    shr__page *p = h.page;
    p->want = frame;
    if (p->slot->bytes && p->pin[0] != frame && p->pin[1] != frame) { /* pages of the cache may be evicted */
        if (p->pin[0] && p->pin[1]) return SHR_E_LIMIT; /* more frames in flight than the compositor keeps */
        if (!p->pin[0] && !p->pin[1]) {
            shr__lru_remove(&f->lru, &p->lru);
            shr__lru_push(&f->pinned, &p->lru);
        }
        p->pin[p->pin[0] ? 1 : 0] = frame;
    }
    int32_t x = shr__rd16(e), y = shr__rd16(e + 2);
    out->buf = &p->slot->buf;
    out->rect = (shr_rect){x, y, x + e[4], y + e[5]};
    out->offset = (shr_point){(int8_t)e[6], h.pkg->baseline - (int8_t)e[7]};
    /* Emoji and Nerd glyphs, and the glyphs that join their neighbours (box drawing, blocks, Braille, legacy
       computing, branch drawing), stay as drawn. */
    bool plain = h.pkg->role == ROLE_EMOJI || h.pkg->role == ROLE_NERD || (base >= 0x2500 && base < 0x25A0) ||
                 (base >= 0x2800 && base < 0x2900) || (base >= 0xF5D0 && base < 0xF60E) ||
                 (base >= 0x1CC00 && base < 0x1CEC0) || (base >= 0x1FB00 && base < 0x1FC00);
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
        shr__lru_push(&f->lru, &p->lru);
        f->wake++;
    }
}
