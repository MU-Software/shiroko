#include "font_core.h"

/* Frees what the package holds and closes its source (no read of it is outstanding). */
void shr__pkg_release(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    shr__pages_free(pkg);
    shr__free(&f->al, pkg->buf, pkg->buf_len, 8, SHR_ALLOC_PAYLOAD);
    shr__free(&f->al, pkg->meta, pkg->ix.meta_len, 64, SHR_ALLOC_PAYLOAD);
    if (pkg->src.close) SHR_HOST(f->res.ctx, pkg->src.close(pkg->src.user));
    *pkg = (shr__pkg){.font = f, .role = pkg->role, .state = pkg->state, .retry = pkg->retry, .preload = pkg->preload};
}

static void pkg_fail(shr__pkg *pkg, shr_status st, const char *why) {
    shr__pkg_release(pkg);
    pkg->state = PKG_FAILED;
    shr__font_report(pkg->font, st, why);
}

/* A transient failure: retried, then closed and reopened by a later want. */
static void pkg_retry(shr__pkg *pkg, shr_status st, const char *why) {
    if (!shr__retry_failed(pkg->font, &pkg->retry)) return;
    pkg->state = PKG_UNOPENED;
    shr__pkg_release(pkg);
    shr__retry_cool(pkg->font, &pkg->retry, st, why);
}

/* Neither MISS, BLANK nor a record of a page. */
static bool glyph_bad(const uint8_t *ptab, uint32_t npages, uint32_t g) {
    return g < SHR_GID_BLANK &&
           (g >> 12 >= npages || (g & 4095) >= shr__rd16(ptab + SHR_PKG_PTAB * (uint64_t)(g >> 12) + 20));
}

/* The metadata boxes in `region` (from the first box), decoded, then their contents. */
static shr_status parse_meta(shr__pkg *pkg, const uint8_t *region, const char **why) {
    shr_pl_res_bitmap_font *f = pkg->font;
    const shr__pkg_index *ix = &pkg->ix;
    const uint8_t *d[BOX_COUNT] = {0};
    uint32_t len[BOX_COUNT];
    uint8_t *m = pkg->meta;
    for (uint32_t i = 0; i < ix->nbox; i++) {
        const shr__box *b = &ix->box[ix->order[i]];
        const uint8_t *x = region + (b->off - ix->lo);
        if (XXH3_64bits(x, b->size) != b->xxh3) return *why = "box checksum", SHR_E_CHECKSUM;
        if (!(d[ix->order[i]] = shr__box_payload(f, b, shr__box_types + 4 * ix->order[i], x, m, ix->mapped)))
            return *why = "box header or payload", SHR_E_FORMAT;
        m += shr__box_room(ix, b);
    }
    for (int k = 0; k < BOX_COUNT; k++) len[k] = ix->box[k].raw;
    const uint8_t *man = d[BOX_MANI], *in = d[BOX_INST], *ct = d[BOX_CTRI], *seqs = d[BOX_SEQS], *pool = d[BOX_POOL],
                  *ptab = d[BOX_PTAB];
    uint32_t features = shr__rd32(ix->hdr + 20), strs = len[BOX_STRS], nsrc = len[BOX_SRCS] / 48;
    shr_status st = shr__pkg_mani(ix, man, pkg->role, f->locale, why);
    if (st != SHR_OK) return st;
    uint32_t nglyphs = shr__rd32(man + 28), npages = shr__rd32(man + 32);
    if (len[BOX_SRCS] % 48) return *why = "srcs size", SHR_E_FORMAT;
    if (ix->box[BOX_COVR].size && len[BOX_COVR] != 36) return *why = "covr size", SHR_E_FORMAT;
    for (uint32_t i = 0; i < nsrc; i++) {
        const uint8_t *r = d[BOX_SRCS] + 48ull * i;
        if ((uint64_t)shr__rd32(r) + shr__rd32(r + 4) > strs || (uint64_t)shr__rd32(r + 40) + shr__rd32(r + 44) > strs)
            return *why = "source string reference", SHR_E_FORMAT;
    }
    if ((st = shr__pkg_inst(ix, in, why)) != SHR_OK) return st;
    uint16_t lh = shr__rd16(in + 4), cw = shr__rd16(in + 6), aw = shr__rd16(in + 36), ah = shr__rd16(in + 38);
    int16_t baseline = (int16_t)shr__rd16(in + 12);
    uint8_t format = in[3];
    if (lh != f->ch || cw != f->cw) return *why = "no instance for the cell size", SHR_E_UNSUPPORTED;
    if (len[BOX_CTRI] < SHR_PKG_CTRI) return *why = "ctri size", SHR_E_FORMAT;
    uint32_t l2 = shr__rd32(ct), dc = shr__rd32(ct + 4);
    uint64_t at = SHR_PKG_CTRI + 128ull * l2, data = (at + 63) & ~63ull;
    if (!l2 || l2 > 65536 || !dc || dc > 65536 || shr__rd64(ct + 8) || len[BOX_CTRI] != data + 64ull * dc)
        return *why = "ctri size", SHR_E_FORMAT;
    for (uint64_t i = at; i < data; i++)
        if (ct[i]) return *why = "ctri padding", SHR_E_FORMAT;
    for (uint32_t i = 0; i < 1088; i++)
        if (shr__rd16(ct + 16 + 2 * i) >= l2) return *why = "ctri level 1", SHR_E_FORMAT;
    for (uint32_t i = 0; i < 64 * l2; i++) {
        uint32_t v = shr__rd16(ct + SHR_PKG_CTRI + 2ull * i);
        if (v >= dc || (i < 64 && v)) return *why = "ctri level 2", SHR_E_FORMAT;
    }
    for (uint32_t i = 0; i < 16; i++)
        if (shr__rd32(ct + data + 4 * i) != SHR_GID_MISS) return *why = "ctri data block 0", SHR_E_FORMAT;
    const shr__box *cb = &ix->box[BOX_CTRI];
    if (((cb->off + 16) % 64 != 0) > (cb->flags >> 4)) return *why = "ctri alignment", SHR_E_FORMAT; /* when stored */
    pkg->ctri = ct, pkg->ctri_data = ct + data;
    uint32_t npool = len[BOX_POOL] / 4, nseqs = len[BOX_SEQS] / 12;
    if (len[BOX_POOL] % 4 || npool > SHR_PKG_MAX_RECORDS) return *why = "pool size", SHR_E_FORMAT;
    if (ix->box[BOX_SEQS].size) {
        if (!(features & 4) || !ix->box[BOX_POOL].size || len[BOX_SEQS] % 12 || nseqs > SHR_PKG_MAX_RECORDS)
            return *why = "seqs size", SHR_E_FORMAT;
        for (uint32_t i = 0; i < npool; i++) {
            uint32_t cp = shr__rd32(pool + 4ull * i);
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return *why = "sequence scalar", SHR_E_FORMAT;
        }
        for (uint32_t i = 0; i < nseqs; i++) {
            const uint8_t *r = seqs + 12ull * i;
            uint64_t idx = shr__rd32(r + 4);
            if (r[0] < 2 || r[0] > shr__cluster_max_scalars || r[1] < 1 || r[1] > 4 || shr__rd16(r + 2) ||
                idx + r[0] > npool)
                return *why = "sequence record", SHR_E_FORMAT;
            const uint8_t *q = r - 12;
            if (i && shr__seq_cmp_pool(pool + 4 * (uint64_t)shr__rd32(q + 4), q[0], pool + 4 * idx, NULL, r[0]) >= 0)
                return *why = "sequence order", SHR_E_FORMAT;
        }
    }
    if (!npages || npages > SHR_PKG_MAX_PAGES || npages != ix->npages || len[BOX_PTAB] % SHR_PKG_PTAB)
        return *why = "ptab size", SHR_E_FORMAT;
    uint64_t glyph = 0, prev = ix->end, fsize = shr__rd64(ix->hdr + 32);
    for (uint32_t i = 0; i < npages; i++) {
        const uint8_t *r = ptab + (uint64_t)SHR_PKG_PTAB * i;
        uint64_t off = shr__rd64(r);
        uint32_t size = shr__rd32(r + 16), count = shr__rd16(r + 20), height = shr__rd16(r + 22);
        if (!count || count > SHR_PKG_MAX_PAGE_GLYPHS || !height || height > ah || size < 24 ||
            size > SHR_PKG_MAX_PAGE_BYTES || off < prev || off > fsize || fsize - off < size)
            return *why = "page record", SHR_E_FORMAT;
        glyph += count;
        prev = off + size;
    }
    if (glyph != nglyphs) return *why = "pages do not cover every glyph", SHR_E_FORMAT;
    for (uint64_t i = 0; i < 16ull * dc; i++)
        if (glyph_bad(ptab, npages, shr__rd32(pkg->ctri_data + 4 * i))) return *why = "ctri glyph", SHR_E_FORMAT;
    for (uint32_t i = 0; i < nseqs; i++)
        if (glyph_bad(ptab, npages, shr__rd32(seqs + 12ull * i + 8))) return *why = "sequence glyph", SHR_E_FORMAT;
    if (shr__ctri_get(pkg, ' ') < SHR_GID_BLANK) return *why = "U+0020 has a bitmap", SHR_E_FORMAT;
    pkg->format = format;
    pkg->baseline = baseline;
    pkg->atlas_w = aw, pkg->atlas_h = ah, pkg->stride = format == 1 ? aw / 2u : aw;
    pkg->slot_shape = (uint64_t)format << 56 | (uint64_t)aw << 40 | (uint64_t)ah << 24;
    pkg->seqs = seqs, pkg->nseqs = nseqs;
    pkg->pool = pool;
    pkg->ptab = ptab;
    return SHR_OK;
}

shr_status shr__page_streams(const shr__page *p, const uint8_t *b, shr__streams *out) {
    const shr__pkg *pkg = p->pkg;
    const uint8_t *e = shr__page_rec(p);
    uint32_t size = shr__rd32(e + 16), flags = shr__rd16(b + 10), method = flags >> 4 & 15, raw = shr__rd32(b + 12),
             an = shr__rd32(b + 20), atlas = shr__page_height(p) * pkg->stride;
    if (memcmp(b + 4, "page", 4) || shr__rd16(b + 8) || shr__rd32(b) != size) return SHR_E_FORMAT;
    if (method >= 2) return SHR_E_UNSUPPORTED;
    /* `x > method`: x when stored */
    if ((flags & ~0xF0u) != SHR_PKG_REQUIRED || method > (shr__rd32(pkg->ix.hdr + 20) >> 3 & 1) ||
        raw != 8 + atlas + shr__page_recs(p) || shr__raw_bad(method, size - 16, raw) ||
        ((shr__rd64(e) + 24) % SHR_PKG_PAGE_ALIGN != 0) > method)
        return SHR_E_FORMAT;
    if (shr__rd32(b + 16) != p->index || an > size - 24 || (an != atlas) > method) return SHR_E_FORMAT;
    *out = (shr__streams){b + 24, b + 24 + an, an, size - 24 - an, method};
    return SHR_OK;
}

bool shr__page_valid(const shr__page *p, const uint8_t *atlas, const uint8_t *recs) {
    const shr__pkg *pkg = p->pkg;
    uint32_t count = shr__rd16(shr__page_rec(p) + 20), height = shr__page_height(p);
    for (uint32_t g = 0; g < count; g++) {
        const uint8_t *e = recs + 16ull * g;
        uint32_t x = shr__rd16(e), y = shr__rd16(e + 2), w = e[4], h = e[5], fmt = e[8] & 3u, cells = (e[8] >> 2) & 3u;
        if ((e[8] & 0xF0) | e[9] | shr__rd32(e + 12) || !cells || cells > 2) return false;
        bool a4 = fmt == 1;
        if (fmt != pkg->format || !w || !h || (a4 && x % 2) || x + w > pkg->atlas_w || y + h > height)
            return false; /* an even A4 x in an atlas of even width leaves room for the padding nibble */
        for (uint32_t r = 0; a4 && w % 2 && r < h; r++)
            if (atlas[(size_t)(y + r) * pkg->stride + (x + w) / 2] & 0x0F) return false; /* A4 padding nibble */
    }
    return true;
}

/* What decoding the metadata takes: the region buffer (read packages), `meta`, the decoder, and the page table.
 * SHR_E_INVALID_ARG: the allocator ignored an alignment. */
static shr_status pkg_alloc(shr__pkg *pkg, const char **why) {
    shr_pl_res_bitmap_font *f = pkg->font;
    *why = "allocator did not align the zstd decoder workspace to 8";
    size_t region = pkg->ix.mapped ? 0 : (size_t)(pkg->ix.hi - pkg->ix.lo);
    if (!pkg->buf && region && !(pkg->buf = shr__malloc(&f->al, region, 8, SHR_ALLOC_PAYLOAD))) return SHR_E_NO_MEMORY;
    pkg->buf_len = region;
    if (!pkg->meta && pkg->ix.meta_len && !(pkg->meta = shr__malloc(&f->al, pkg->ix.meta_len, 64, SHR_ALLOC_PAYLOAD)))
        return SHR_E_NO_MEMORY;
    if (!pkg->pages && !(pkg->pages = shr__calloc(&f->al, pkg->ix.npages, sizeof(shr__page *), SHR_ALIGNOF(shr__page *),
                                                  SHR_ALLOC_PAYLOAD)))
        return SHR_E_NO_MEMORY;
    return shr__zinit(f, shr__rd32(pkg->ix.hdr + 20));
}

static void buf_free(shr__pkg *pkg) {
    shr__free(&pkg->font->al, pkg->buf, pkg->buf_len, 8, SHR_ALLOC_PAYLOAD);
    pkg->buf = NULL, pkg->buf_len = 0;
}

static void pkg_ready(shr__pkg *pkg) {
    pkg->state = PKG_READY;
    pkg->retry.count = pkg->retry.cools = 0;
    pkg->font->changed = true;
}

shr_status shr__pkg_map(shr__pkg *pkg, const char **why) {
    shr__pkg_index *ix = &pkg->ix;
    ix->mapped = true, ix->hdr = pkg->src.data, ix->size = pkg->src.size;
    *why = "package too small";
    shr_status st = ix->size < SHR_PKG_HEADER ? SHR_E_FORMAT : shr__pkg_header(ix, why);
    if (st == SHR_OK) st = shr__pkg_index_parse(ix, ix->hdr + SHR_PKG_HEADER, why);
    if (st == SHR_OK) st = pkg_alloc(pkg, why);
    if (st == SHR_OK) st = parse_meta(pkg, ix->hdr + ix->lo, why);
    if (st == SHR_OK) pkg_ready(pkg);
    return st;
}

/* Out of memory waits; any other failure is final. */
static void pkg_settle(shr__pkg *pkg, shr_status st, const char *why) {
    if (st == SHR_E_NO_MEMORY)
        shr__retry_memory(pkg->font, &pkg->retry);
    else
        pkg_fail(pkg, st, why);
}

/* A mapped package, decoded in place of the reads. */
static void pkg_mapped(shr__pkg *pkg) {
    const char *why;
    shr_status st = shr__pkg_map(pkg, &why);
    if (st != SHR_OK) pkg_settle(pkg, st, why);
}

void shr__pkg_name(const shr__pkg *pkg, char name[40]) {
    static const char *const names[ROLE_COUNT] = {NULL, "latin", "cjk-", "symbols", "emoji", "nerd"};
    const char *parts[4] = {"shiroko-", names[pkg->role], pkg->role == ROLE_CJK ? pkg->font->locale : "", ".shrf"};
    for (size_t i = 0, at = 0; i < 4; at += strlen(parts[i++])) memcpy(name + at, parts[i], strlen(parts[i]) + 1);
}

static void pkg_open(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    char name[40];
    shr__pkg_name(pkg, name);
    shr_asset_source src = {0};
    shr_status st = SHR_E_NOT_FOUND;
    if (f->open) SHR_HOST(f->res.ctx, st = f->open(f->user, name, &src));
    if (st == SHR_E_NOT_FOUND) {
        pkg->state = PKG_ABSENT;
        f->changed = true;
        return;
    }
    if (st != SHR_OK) {
        pkg_retry(pkg, st, "font package open failed");
        return;
    }
    pkg->src = src;
    if (!src.data && !src.read) {
        pkg_fail(pkg, SHR_E_INVALID_ARG, "font package source without data or read()");
        return;
    }
    pkg->ix.mapped = src.data != NULL, pkg->ix.hdr = pkg->header, pkg->ix.size = src.size;
    pkg->state = PKG_LOADING;
}

/* An absent package is looked up again after a ready signal. */
bool shr__pkg_due(const shr__pkg *pkg, uint64_t now) {
    const shr_pl_res_bitmap_font *f = pkg->font;
    return shr__retry_due(f, &pkg->retry, now) &&
           ((pkg->state == PKG_UNOPENED && pkg->wanted) ||
            (pkg->state == PKG_LOADING && !pkg->step_busy && (pkg->ix.mapped || !f->blocked)));
}

void shr__pkg_advance(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    uint64_t now = shr__font_now(f);
    if (!shr__pkg_due(pkg, now)) return;
    shr__retry_begin(f, &pkg->retry);
    if (pkg->state == PKG_UNOPENED) pkg_open(pkg);
    if (pkg->state != PKG_LOADING) return;
    if (pkg->ix.mapped) {
        pkg_mapped(pkg);
        return;
    }
    if (f->blocked) return;
    uint8_t *dst = pkg->header;
    uint64_t off = 0;
    uint32_t len = SHR_PKG_HEADER;
    switch (pkg->step) {
    case LOAD_HEADER:
        if (pkg->src.size < SHR_PKG_HEADER) {
            pkg_fail(pkg, SHR_E_FORMAT, "package too small");
            return;
        }
        break;
    case LOAD_INDEX:
        off = SHR_PKG_HEADER, len = shr__rd32(pkg->header + 28);
        if (!pkg->buf && (pkg->buf = shr__malloc(&f->al, len, 8, SHR_ALLOC_PAYLOAD))) pkg->buf_len = len;
        dst = pkg->buf;
        break;
    default: { /* LOAD_META */
        off = pkg->ix.lo, len = (uint32_t)(pkg->ix.hi - pkg->ix.lo);
        const char *why;
        shr_status st = pkg_alloc(pkg, &why);
        if (st != SHR_OK) {
            pkg_settle(pkg, st, why);
            return;
        }
        dst = pkg->buf;
        break;
    }
    }
    if (!dst) {
        shr__retry_memory(f, &pkg->retry);
        return;
    }
    pkg->step_busy = true;
    shr_status st = shr__ctx_read(f->res.ctx, &f->res, &pkg->src, off, len, dst, shr__io_tag(pkg, 0));
    if (st == SHR_OK) return;
    pkg->step_busy = false;
    if (st == SHR_E_LIMIT)
        f->blocked = true;
    else /* SHR_E_WOULD_BLOCK: pump() is never refused and the source has read() */
        shr__retry_refused(f, &pkg->retry, now);
}

void shr__pkg_load_done(shr__pkg *pkg, shr_status result) {
    pkg->step_busy = false;
    if (result != SHR_OK) {
        pkg_retry(pkg, result, "font package read failed");
        return;
    }
    shr_status st;
    const char *why;
    switch (pkg->step) {
    case LOAD_HEADER:
        if ((st = shr__pkg_header(&pkg->ix, &why))) break;
        pkg->step = LOAD_INDEX;
        return;
    case LOAD_INDEX:
        if ((st = shr__pkg_index_parse(&pkg->ix, pkg->buf, &why))) break;
        buf_free(pkg);
        pkg->step = LOAD_META;
        return;
    default: /* LOAD_META */
        if ((st = parse_meta(pkg, pkg->buf, &why))) break;
        buf_free(pkg);
        pkg_ready(pkg);
        return;
    }
    pkg_fail(pkg, st, why);
}

shr_status shr_pl_res_bitmap_font_activation_select(const uint8_t *a, size_t a_length, const uint8_t *b,
                                                    size_t b_length, shr_activation *out) {
    if (!out) return SHR_E_INVALID_ARG;
    const uint8_t *rec[2] = {a, b};
    size_t len[2] = {a_length, b_length};
    int best = -1;
    for (int i = 0; i < 2; i++) {
        const uint8_t *r = rec[i];
        if (!r || len[i] != 64 || memcmp(r, "SHRFACT2", 8) || XXH3_64bits(r, 56) != shr__rd64(r + 56)) continue;
        if (best < 0 || shr__rd64(r + 8) > shr__rd64(rec[best] + 8)) best = i;
    }
    if (best < 0) return SHR_E_NOT_FOUND;
    shr_activation act = {shr__rd64(rec[best] + 8), {0}, shr__rd64(rec[best] + 48)};
    memcpy(act.package_id, rec[best] + 16, 32);
    *out = act;
    return SHR_OK;
}

shr_status shr_pl_res_bitmap_font_package_info(const shr_asset_source *source, shr_package_info *out) {
    if (out) *out = (shr_package_info){0};
    shr__pkg_desc d;
    const char *why;
    shr_status st = shr__pkg_describe(source, out ? &d : NULL, &why);
    if (st != SHR_OK) return st;
    *out = (shr_package_info){.role = (shr_package_role)d.role, .cell_width = d.cell_width,
                              .cell_height = d.line_height};
    memcpy(out->locale, d.locale, sizeof(out->locale));
    return SHR_OK;
}
