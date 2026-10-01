#include "font_core.h"

/* Frees what the package holds and closes its source (no read of it is outstanding). */
void shr__pkg_release(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    shr__pages_free(pkg);
    shr__free(&f->al, pkg->buf, pkg->buf_len, 8, SHR_ALLOC_PAYLOAD);
    if (pkg->src.close) SHR_HOST(f->res.ctx, pkg->src.close(pkg->src.user));
    *pkg = (shr__pkg){.font = f, .role = pkg->role, .state = pkg->state, .retry = pkg->retry};
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

static const char *parse_header(shr__pkg *pkg, shr_status *st) {
    const uint8_t *h = pkg->hdr;
    *st = SHR_E_FORMAT;
    if (memcmp(h, "SHRFPKG1", 8) || shr__rd16(h + 8) != SHR_PKG_VERSION || shr__rd16(h + 10) != 128)
        return "bad magic or version";
    if (XXH3_64bits(h, 72) != shr__rd64(h + 72)) return *st = SHR_E_CHECKSUM, "header checksum";
    if (shr__rd32(h + 12) & ~7u) return *st = SHR_E_UNSUPPORTED, "unsupported required feature";
    uint32_t nsec = shr__rd32(h + 16);
    uint64_t toff = shr__rd64(h + 24), fsize = shr__rd64(h + 32);
    for (int i = 80; i < 128; i++)
        if (h[i]) return "reserved header bytes";
    if (shr__rd32(h + 20) || nsec == 0 || nsec > SHR_PKG_MAX_SECTIONS) return "section count";
    if (fsize != pkg->src.size || toff < 128 || toff > fsize || fsize - toff < (uint64_t)SHR_PKG_ENTRY * nsec)
        return "file size";
    if (memcmp(h + 40, shr__text_profile_id, 32)) return *st = SHR_E_PROFILE_MISMATCH, "text profile mismatch";
    pkg->file_size = fsize;
    return NULL;
}

/* From the section table: the length of [table, end of the last section). */
static const char *index_bounds(shr__pkg *pkg, const uint8_t *table, shr_status *st) {
    uint32_t nsec = shr__rd32(pkg->hdr + 16);
    uint64_t toff = shr__rd64(pkg->hdr + 24), end = toff + (uint64_t)SHR_PKG_ENTRY * nsec;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *e = table + (uint64_t)SHR_PKG_ENTRY * i;
        uint64_t so = shr__rd64(e + 8);
        if (so > pkg->file_size) return *st = SHR_E_FORMAT, "section out of range";
        if (so + shr__rd32(e + 16) > end) end = so + shr__rd32(e + 16);
    }
    if (end > pkg->file_size || end - toff > SHR_PKG_MAX_INDEX_BYTES) return *st = SHR_E_LIMIT, "package index too large";
    pkg->index_len = (size_t)(end - toff);
    return NULL;
}

typedef struct section {
    uint32_t count;
    const uint8_t *data;
    uint32_t length;
} section;

static const char *parse_index(shr__pkg *pkg, shr_status *st) {
    shr_pl_res_bitmap_font *f = pkg->font;
    const uint8_t *h = pkg->hdr, *base = pkg->index;
    *st = SHR_E_FORMAT;
    uint32_t nsec = shr__rd32(h + 16), features = shr__rd32(h + 12);
    uint64_t toff = shr__rd64(h + 24), prev_end = toff + (uint64_t)SHR_PKG_ENTRY * nsec,
             index_end = toff + pkg->index_len;
    section sec[11] = {{0}};
    const uint8_t *order[SHR_PKG_MAX_SECTIONS];
    for (uint32_t i = 0; i < nsec; i++) order[i] = base + (uint64_t)SHR_PKG_ENTRY * i;
    for (uint32_t i = 1; i < nsec; i++)
        for (uint32_t k = i; k > 0 && shr__rd64(order[k] + 8) < shr__rd64(order[k - 1] + 8); k--) {
            const uint8_t *t = order[k];
            order[k] = order[k - 1];
            order[k - 1] = t;
        }
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *e = order[i];
        uint32_t type = shr__rd32(e), count = shr__rd32(e + 4), length = shr__rd32(e + 16);
        uint64_t off = shr__rd64(e + 8);
        if (type < 1 || type > 10 || type == 5 || sec[type].data || shr__rd32(e + 20))
            return "unknown or duplicate section";
        if (off < prev_end) return "overlapping sections";
        if (off > index_end || index_end - off < length) return "section out of range"; /* table re-read may differ */
        if (XXH3_64bits(base + (off - toff), length) != shr__rd64(e + 24)) return "section checksum";
        if (count > SHR_PKG_MAX_RECORDS) return "record count limit";
        sec[type] = (section){count, base + (off - toff), length};
        prev_end = off + length;
    }
    if (!sec[1].data || !sec[4].data || !sec[6].data || !sec[9].data) return "missing section";
    const uint8_t *meta = sec[1].data;
    if (sec[1].count != 1 || sec[1].length != 36 || meta[1] | meta[2] | meta[3]) return "MANIFEST";
    if (meta[0] < ROLE_LATIN || meta[0] > ROLE_NERD) return "role";
    for (int i = 0; i < 8; i++)
        if (meta[4 + i] && (meta[4 + i] < 0x20 || meta[4 + i] > 0x7E)) return "locale";
    if (pkg->role != ROLE_BUILTIN &&
        (meta[0] != pkg->role || (pkg->role == ROLE_CJK && strncmp((const char *)meta + 4, f->locale, 8))))
        return "package role or locale differs from its file name";
    uint64_t name_end = (uint64_t)shr__rd32(meta + 12) + shr__rd32(meta + 16);
    uint64_t build_end = (uint64_t)shr__rd32(meta + 20) + shr__rd32(meta + 24);
    if (name_end > sec[2].length || build_end > sec[2].length) return "string reference";
    uint32_t nglyphs = shr__rd32(meta + 28);
    if (nglyphs == 0 || nglyphs > SHR_PKG_MAX_RECORDS || shr__rd32(meta + 32) != 1) return "glyph or instance count";
    if (sec[4].count != 1 || sec[4].length != 48) return "INSTANCES size";
    if (sec[3].data && sec[3].length != 48ull * sec[3].count) return "SOURCES size";
    if (sec[10].data && (sec[10].count != 9 || sec[10].length != 36)) return "COVERAGE size";
    for (uint32_t i = 0; sec[3].data && i < sec[3].count; i++) {
        const uint8_t *r = sec[3].data + 48ull * i;
        if ((uint64_t)shr__rd32(r) + shr__rd32(r + 4) > sec[2].length ||
            (uint64_t)shr__rd32(r + 40) + shr__rd32(r + 44) > sec[2].length)
            return "source string reference";
    }
    const uint8_t *in = sec[4].data;
    uint16_t lh = shr__rd16(in + 4), cw = shr__rd16(in + 6);
    int16_t baseline = (int16_t)shr__rd16(in + 12), under = (int16_t)shr__rd16(in + 14),
            strike = (int16_t)shr__rd16(in + 16);
    uint8_t format = in[3];
    if (sec[3].data && shr__rd16(in) >= sec[3].count) return "instance source";
    if (in[2] || (format != 1 && format != 2) || !(features & format)) return "instance style or format";
    if (!lh || lh > SHR_MAX_CELL_SIZE || !cw || cw > SHR_MAX_CELL_SIZE) return "instance size";
    if (baseline < 0 || baseline > lh || under < 0 || under >= lh || strike < 0 || strike >= lh)
        return "instance line metrics";
    if (shr__rd16(in + 18) & ~1u) return "instance raster flags";
    for (int k = 36; k < 48; k++)
        if (in[k]) return "instance reserved bytes";
    if (lh != SHR_CELL_HEIGHT || cw != SHR_CELL_WIDTH) return *st = SHR_E_UNSUPPORTED, "no instance for the cell size";
    if (sec[6].length != 8ull * sec[6].count) return "CMAP size";
    for (uint32_t i = 0; i < sec[6].count; i++) {
        uint32_t cp = shr__rd32(sec[6].data + 8ull * i);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || shr__rd32(sec[6].data + 8ull * i + 4) >= nglyphs)
            return "cmap record";
        if (i && cp <= shr__rd32(sec[6].data + 8ull * (i - 1))) return "cmap order";
    }
    if (sec[8].data && sec[8].length != 4ull * sec[8].count) return "SEQPOOL size";
    if (sec[7].data && sec[7].count) {
        if (!(features & 4) || !sec[8].data || sec[7].length != 12ull * sec[7].count) return "SEQS size";
        for (uint32_t i = 0; i < sec[8].count; i++) {
            uint32_t cp = shr__rd32(sec[8].data + 4ull * i);
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return "sequence scalar";
        }
        for (uint32_t i = 0; i < sec[7].count; i++) {
            const uint8_t *r = sec[7].data + 12ull * i;
            uint64_t idx = shr__rd32(r + 4);
            if (r[0] < 2 || r[0] > shr__cluster_max_scalars || r[1] < 1 || r[1] > 4 || shr__rd16(r + 2) ||
                idx + r[0] > sec[8].count || shr__rd32(r + 8) >= nglyphs)
                return "sequence record";
            const uint8_t *q = r - 12;
            if (i && shr__seq_cmp_pool(sec[8].data + 4 * (uint64_t)shr__rd32(q + 4), q[0], sec[8].data + 4 * idx, NULL, r[0]) >= 0)
                return "sequence order";
        }
    }
    uint32_t npages = sec[9].count;
    if (!npages || npages > SHR_PKG_MAX_PAGES || sec[9].length != (uint64_t)SHR_PKG_ENTRY * npages)
        return "PAGES size";
    uint64_t glyph = 0, prev = index_end;
    for (uint32_t i = 0; i < npages; i++) {
        const uint8_t *r = sec[9].data + (uint64_t)SHR_PKG_ENTRY * i;
        uint64_t off = shr__rd64(r);
        uint32_t length = shr__rd32(r + 16), count = shr__rd32(r + 24);
        if (shr__rd32(r + 20) != glyph || !count || length < 4 + 16ull * count || length > SHR_PKG_MAX_PAGE_BYTES ||
            shr__rd32(r + 28) || off < prev || off > pkg->file_size || pkg->file_size - off < length)
            return "page record";
        glyph += count;
        prev = off + length;
    }
    if (glyph != nglyphs) return "pages do not cover every glyph";
    pkg->format = format;
    pkg->baseline = baseline;
    pkg->npages = npages;
    pkg->nglyphs = nglyphs;
    pkg->cmap = sec[6].data, pkg->ncmap = sec[6].count;
    pkg->seqs = sec[7].data, pkg->nseqs = sec[7].count;
    pkg->pool = sec[8].data;
    pkg->page_recs = sec[9].data;
    return NULL;
}

bool shr__page_valid(const shr__page *p) {
    const uint8_t *d = p->data, *rec = shr__page_rec(p);
    uint32_t length = shr__rd32(rec + 16), count = shr__rd32(rec + 24);
    if (XXH3_64bits(d, length) != shr__rd64(rec + 8) || shr__rd32(d) != count) return false;
    uint64_t table = 4 + 16ull * count;
    for (uint32_t g = 0; g < count; g++) {
        const uint8_t *e = d + 4 + 16ull * g;
        uint32_t off = shr__rd32(e), len = shr__rd16(e + 4);
        uint8_t w = e[6], h = e[7], stride = e[10], flags = e[11], fmt = flags & 3, cells = (flags >> 2) & 3;
        if (cells < 1 || cells > 2 || (flags & 0xF0) || shr__rd16(e + 14)) return false;
        if (fmt) {
            uint64_t row = fmt == 1 ? (uint64_t)w / 2 + w % 2 : w;
            if (fmt != p->pkg->format || !w || !h || stride < row ||
                (uint64_t)(h - 1) * stride + row > len || off < table || (uint64_t)off + len > length)
                return false;
            for (uint32_t y = 0; fmt == 1 && w % 2 && y < h; y++)
                if (d[off + y * stride + w / 2] & 0x0F) return false; /* A4 padding nibble */
        } else if (len || w || h) {
            return false;
        }
    }
    return true;
}

shr_status shr__pkg_map(shr__pkg *pkg, const void *data, uint64_t size, const char **why) {
    shr_status st = SHR_E_FORMAT;
    pkg->mapped = true;
    pkg->hdr = data;
    *why = size < 128 ? "package too small" : parse_header(pkg, &st);
    if (!*why) *why = index_bounds(pkg, pkg->hdr + shr__rd64(pkg->hdr + 24), &st);
    if (!*why) {
        pkg->index = pkg->hdr + shr__rd64(pkg->hdr + 24);
        *why = parse_index(pkg, &st);
    }
    if (*why) return st;
    pkg->state = PKG_READY;
    pkg->retry.count = pkg->retry.cools = 0;
    return SHR_OK;
}

static void pkg_open(shr__pkg *pkg) {
    static const char *const names[ROLE_COUNT] = {NULL, "latin", "cjk-", "symbols", "emoji", "nerd"};
    shr_pl_res_bitmap_font *f = pkg->font;
    const char *parts[4] = {"shiroko-", names[pkg->role], pkg->role == ROLE_CJK ? f->locale : "", ".shrf"};
    char name[40];
    for (size_t i = 0, at = 0; i < 4; at += strlen(parts[i++])) memcpy(name + at, parts[i], strlen(parts[i]) + 1);
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
    if (src.data) {
        const char *why;
        if ((st = shr__pkg_map(pkg, src.data, src.size, &why)) != SHR_OK)
            pkg_fail(pkg, st, why);
        else
            f->changed = true;
    } else if (!src.read) {
        pkg_fail(pkg, SHR_E_INVALID_ARG, "font package source without data or read()");
    } else {
        pkg->hdr = pkg->header;
        pkg->state = PKG_LOADING;
    }
}

/* An absent package is looked up again after a ready signal. */
bool shr__pkg_due(const shr__pkg *pkg, uint64_t now) {
    const shr_pl_res_bitmap_font *f = pkg->font;
    return shr__retry_due(f, &pkg->retry, now) &&
           ((pkg->state == PKG_UNOPENED && pkg->wanted) || (pkg->state == PKG_LOADING && !pkg->step_busy && !f->blocked));
}

void shr__pkg_advance(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    uint64_t now = shr__font_now(f);
    if (!shr__pkg_due(pkg, now)) return;
    shr__retry_begin(f, &pkg->retry);
    if (pkg->state == PKG_UNOPENED) pkg_open(pkg);
    if (pkg->state != PKG_LOADING || f->blocked) return;
    uint8_t *dst;
    uint64_t off = shr__rd64(pkg->header + 24);
    uint32_t len;
    switch (pkg->step) {
    case LOAD_HEADER:
        if (pkg->src.size < 128) {
            pkg_fail(pkg, SHR_E_FORMAT, "package too small");
            return;
        }
        dst = pkg->header, off = 0, len = 128;
        break;
    case LOAD_TABLE:
        len = SHR_PKG_ENTRY * shr__rd32(pkg->header + 16);
        if (!pkg->buf && (pkg->buf = shr__malloc(&f->al, len, 8, SHR_ALLOC_PAYLOAD))) pkg->buf_len = len;
        dst = pkg->buf;
        break;
    default: /* LOAD_INDEX */
        if (!pkg->buf && (pkg->buf = shr__malloc(&f->al, pkg->index_len, 8, SHR_ALLOC_PAYLOAD)))
            pkg->buf_len = pkg->index_len;
        dst = pkg->buf, len = (uint32_t)pkg->index_len;
        break;
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
    shr_pl_res_bitmap_font *f = pkg->font;
    pkg->step_busy = false;
    if (result != SHR_OK) {
        pkg_retry(pkg, result, "font package read failed");
        return;
    }
    shr_status st;
    const char *why;
    switch (pkg->step) {
    case LOAD_HEADER:
        if ((why = parse_header(pkg, &st))) break;
        pkg->step = LOAD_TABLE;
        return;
    case LOAD_TABLE:
        if ((why = index_bounds(pkg, pkg->buf, &st))) break;
        shr__free(&f->al, pkg->buf, pkg->buf_len, 8, SHR_ALLOC_PAYLOAD);
        pkg->buf = NULL, pkg->buf_len = 0;
        pkg->step = LOAD_INDEX;
        return;
    default: /* LOAD_INDEX */
        pkg->index = pkg->buf;
        if ((why = parse_index(pkg, &st))) break;
        pkg->state = PKG_READY;
        pkg->retry.count = pkg->retry.cools = 0;
        f->changed = true;
        return;
    }
    pkg_fail(pkg, st, why);
}

/* The built-in package is baked by the build for the cell size: its one instance. */
shr__line_metrics shr__bitmap_font_line_metrics(void) {
    const uint8_t *d = shr__builtin_package, *e = d + shr__rd64(d + 24);
    while (shr__rd32(e) != 4) e += SHR_PKG_ENTRY;
    const uint8_t *s = d + shr__rd64(e + 8);
    return (shr__line_metrics){(int16_t)shr__rd16(s + 12), (int16_t)shr__rd16(s + 14), (int16_t)shr__rd16(s + 16)};
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
