#include "font_core.h"

/* Frees what the package holds and closes its source (no read of it is outstanding). */
void shr__pkg_release(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    shr__pages_free(pkg);
    shr__free(&f->al, pkg->buf, pkg->buf_len, 8, SHR_ALLOC_PAYLOAD);
    shr__free(&f->al, pkg->meta, pkg->meta_len, 8, SHR_ALLOC_PAYLOAD);
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

static const char box_types[] = "manistrssrcsinstcmapseqspoolptabcovr"; /* by BOX_ */
#define PACKED_BOXES (1u << BOX_CMAP | 1u << BOX_SEQS | 1u << BOX_POOL) /* may be zstd */

/* A stored payload of `n` bytes is raw_size, a zstd one smaller. */
static bool raw_bad(uint32_t method, uint64_t n, uint32_t raw) { return raw < n || (raw > n) != method; }

static const char *parse_header(shr__pkg *pkg, shr_status *st) {
    const uint8_t *h = pkg->hdr;
    *st = SHR_E_FORMAT;
    if (memcmp(h, "\x80\0\0\0shrf", 8)) return "bad magic";
    if (shr__rd16(h + 8) || shr__rd16(h + 10) != SHR_PKG_REQUIRED || shr__rd32(h + 12) != SHR_PKG_HEADER - 16 ||
        shr__rd16(h + 16) != SHR_PKG_VERSION || shr__rd16(h + 18))
        return "header version";
    if (XXH3_64bits(h, 120) != shr__rd64(h + 120)) return *st = SHR_E_CHECKSUM, "header checksum";
    if (shr__rd32(h + 20) & ~SHR_PKG_FEATURES) return *st = SHR_E_UNSUPPORTED, "unsupported required feature";
    for (int i = 88; i < 120; i++)
        if (h[i]) return "reserved header bytes";
    uint32_t n = shr__rd32(h + 28);
    uint64_t fsize = shr__rd64(h + 32);
    if (fsize != pkg->src.size || shr__rd64(h + 40) != SHR_PKG_HEADER || n < 24 || (n - 24) % SHR_PKG_ENTRY ||
        n > 24 + SHR_PKG_ENTRY * SHR_PKG_MAX_BOXES || fsize - SHR_PKG_HEADER < n)
        return "file size or index";
    if (memcmp(h + 48, shr__text_profile_id, 32)) return *st = SHR_E_PROFILE_MISMATCH, "text profile mismatch";
    return NULL;
}

/* The index `s`: the metadata boxes and their region. */
static const char *parse_index(shr__pkg *pkg, const uint8_t *s, shr_status *st) {
    const uint8_t *h = pkg->hdr;
    uint32_t n = shr__rd32(h + 28), count = (n - 24) / SHR_PKG_ENTRY, zstd = shr__rd32(h + 20) >> 3 & 1;
    uint64_t fsize = shr__rd64(h + 32), end = SHR_PKG_HEADER + n, raw = 0;
    *st = SHR_E_CHECKSUM;
    if (XXH3_64bits(s, n) != shr__rd64(h + 80)) return "index checksum";
    *st = SHR_E_FORMAT;
    if (shr__rd32(s) != n || memcmp(s + 4, "sidx", 4) || shr__rd16(s + 8) || shr__rd16(s + 10) != SHR_PKG_REQUIRED ||
        shr__rd32(s + 12) != n - 16 || shr__rd32(s + 16) != count || shr__rd32(s + 20))
        return "index box";
    memset(pkg->box, 0, sizeof(pkg->box));
    pkg->nbox = 0, pkg->meta_len = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = s + 24 + SHR_PKG_ENTRY * i;
        uint32_t flags = shr__rd16(e + 4), size = shr__rd32(e + 16), method = flags >> 4 & 15, k = 0;
        uint64_t off = shr__rd64(e + 8);
        if (size < 16 || off < end || off > fsize || fsize - off < size) return "index entry out of order or range";
        end = off + size;
        if (!memcmp(e, "shrf", 4) || !memcmp(e, "sidx", 4) || !memcmp(e, "page", 4) || !memcmp(e, "free", 4))
            return "box type in the index";
        while (k < BOX_COUNT && memcmp(e, box_types + 4 * k, 4)) k++;
        if (k == BOX_COUNT) {
            if (flags & SHR_PKG_REQUIRED) return *st = SHR_E_UNSUPPORTED, "unsupported required box";
            continue;
        }
        shr__box *b = &pkg->box[k];
        if (b->size) return "duplicate box";
        if (method >= 2) return *st = SHR_E_UNSUPPORTED, "unsupported compression";
        if (flags & ~0xF1u || (!(flags & SHR_PKG_REQUIRED) && k != BOX_COVR) ||
            method > ((PACKED_BOXES >> k) & zstd) || shr__rd16(e + 6))
            return "box flags";
        if (raw_bad(method, size - 16, shr__rd32(e + 20))) return "box raw size";
        *b = (shr__box){off, shr__rd64(e + 24), size, shr__rd32(e + 20), flags};
        pkg->lo = pkg->nbox ? pkg->lo : off;
        pkg->hi = end;
        pkg->order[pkg->nbox++] = (uint8_t)k;
        raw += b->raw;
        pkg->meta_len += shr__box_room(pkg, b);
    }
    if (!pkg->box[BOX_MANI].size || !pkg->box[BOX_INST].size || !pkg->box[BOX_CMAP].size || !pkg->box[BOX_PTAB].size)
        return "missing box";
    if ((pkg->hi - pkg->lo > SHR_PKG_MAX_META) | (raw > SHR_PKG_MAX_META))
        return *st = SHR_E_LIMIT, "package index too large";
    pkg->end = end;
    pkg->npages = pkg->box[BOX_PTAB].raw / SHR_PKG_ENTRY;
    return NULL;
}

static bool shape_side(uint32_t v) { return v >= 64 && v <= 512 && !(v & (v - 1)); }

/* The metadata boxes in `region` (from the first box), decoded, then their contents. */
static const char *parse_meta(shr__pkg *pkg, const uint8_t *region, shr_status *st) {
    shr_pl_res_bitmap_font *f = pkg->font;
    const uint8_t *d[BOX_COUNT] = {0};
    uint32_t len[BOX_COUNT];
    uint8_t *m = pkg->meta;
    for (uint32_t i = 0; i < pkg->nbox; i++) {
        const shr__box *b = &pkg->box[pkg->order[i]];
        const uint8_t *x = region + (b->off - pkg->lo);
        if (XXH3_64bits(x, b->size) != b->xxh3) return *st = SHR_E_CHECKSUM, "box checksum";
        if (!(d[pkg->order[i]] = shr__box_payload(f, b, box_types + 4 * pkg->order[i], x, m, pkg->mapped)))
            return *st = SHR_E_FORMAT, "box header or payload";
        m += shr__box_room(pkg, b);
    }
    for (int k = 0; k < BOX_COUNT; k++) len[k] = pkg->box[k].raw;
    *st = SHR_E_FORMAT;
    const uint8_t *man = d[BOX_MANI], *in = d[BOX_INST], *cmap = d[BOX_CMAP], *seqs = d[BOX_SEQS], *pool = d[BOX_POOL],
                  *ptab = d[BOX_PTAB];
    uint32_t features = shr__rd32(pkg->hdr + 20), strs = len[BOX_STRS], nsrc = len[BOX_SRCS] / 48;
    if (len[BOX_MANI] != 36 || man[1] | man[2] | man[3]) return "mani";
    if (man[0] < ROLE_LATIN || man[0] > ROLE_NERD) return "role";
    for (int i = 0; i < 8; i++)
        if (man[4 + i] && (man[4 + i] < 0x20 || man[4 + i] > 0x7E)) return "locale";
    if (pkg->role != ROLE_BUILTIN &&
        (man[0] != pkg->role || (pkg->role == ROLE_CJK && strncmp((const char *)man + 4, f->locale, 8))))
        return "package role or locale differs from its file name";
    if ((uint64_t)shr__rd32(man + 12) + shr__rd32(man + 16) > strs ||
        (uint64_t)shr__rd32(man + 20) + shr__rd32(man + 24) > strs)
        return "string reference";
    uint32_t nglyphs = shr__rd32(man + 28), npages = shr__rd32(man + 32);
    if (!nglyphs || nglyphs > SHR_PKG_MAX_RECORDS) return "glyph count";
    if (len[BOX_INST] != 48) return "inst size";
    if (len[BOX_SRCS] % 48) return "srcs size";
    if (pkg->box[BOX_COVR].size && len[BOX_COVR] != 36) return "covr size";
    for (uint32_t i = 0; i < nsrc; i++) {
        const uint8_t *r = d[BOX_SRCS] + 48ull * i;
        if ((uint64_t)shr__rd32(r) + shr__rd32(r + 4) > strs || (uint64_t)shr__rd32(r + 40) + shr__rd32(r + 44) > strs)
            return "source string reference";
    }
    uint16_t lh = shr__rd16(in + 4), cw = shr__rd16(in + 6), aw = shr__rd16(in + 36), ah = shr__rd16(in + 38);
    int16_t baseline = (int16_t)shr__rd16(in + 12), under = (int16_t)shr__rd16(in + 14),
            strike = (int16_t)shr__rd16(in + 16);
    uint8_t format = in[3];
    if (pkg->box[BOX_SRCS].size && shr__rd16(in) >= nsrc) return "instance source";
    if (in[2] || (format != 1 && format != 2) || !(features & format)) return "instance style or format";
    if (!lh || lh > SHR_MAX_CELL_SIZE || !cw || cw > SHR_MAX_CELL_SIZE) return "instance size";
    if (baseline < 0 || baseline > lh || under < 0 || under >= lh || strike < 0 || strike >= lh)
        return "instance line metrics";
    if (shr__rd16(in + 18) & ~1u || shr__rd64(in + 40)) return "instance reserved bytes";
    if (!shape_side(aw) || !shape_side(ah)) return "instance page shape";
    if (lh != SHR_CELL_HEIGHT || cw != SHR_CELL_WIDTH) return *st = SHR_E_UNSUPPORTED, "no instance for the cell size";
    uint32_t ncmap = len[BOX_CMAP] / 8, npool = len[BOX_POOL] / 4, nseqs = len[BOX_SEQS] / 12;
    if (len[BOX_CMAP] % 8 || ncmap > SHR_PKG_MAX_RECORDS) return "cmap size";
    for (uint32_t i = 0; i < ncmap; i++) {
        uint32_t cp = shr__rd32(cmap + 8ull * i);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || shr__rd32(cmap + 8ull * i + 4) >= nglyphs)
            return "cmap record";
        if (i && cp <= shr__rd32(cmap + 8ull * (i - 1))) return "cmap order";
    }
    if (len[BOX_POOL] % 4 || npool > SHR_PKG_MAX_RECORDS) return "pool size";
    if (pkg->box[BOX_SEQS].size) {
        if (!(features & 4) || !pkg->box[BOX_POOL].size || len[BOX_SEQS] % 12 || nseqs > SHR_PKG_MAX_RECORDS)
            return "seqs size";
        for (uint32_t i = 0; i < npool; i++) {
            uint32_t cp = shr__rd32(pool + 4ull * i);
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return "sequence scalar";
        }
        for (uint32_t i = 0; i < nseqs; i++) {
            const uint8_t *r = seqs + 12ull * i;
            uint64_t idx = shr__rd32(r + 4);
            if (r[0] < 2 || r[0] > shr__cluster_max_scalars || r[1] < 1 || r[1] > 4 || shr__rd16(r + 2) ||
                idx + r[0] > npool || shr__rd32(r + 8) >= nglyphs)
                return "sequence record";
            const uint8_t *q = r - 12;
            if (i && shr__seq_cmp_pool(pool + 4 * (uint64_t)shr__rd32(q + 4), q[0], pool + 4 * idx, NULL, r[0]) >= 0)
                return "sequence order";
        }
    }
    if (!npages || npages > SHR_PKG_MAX_PAGES || npages != pkg->npages || len[BOX_PTAB] % SHR_PKG_ENTRY)
        return "ptab size";
    uint64_t glyph = 0, prev = pkg->end, fsize = shr__rd64(pkg->hdr + 32);
    for (uint32_t i = 0; i < npages; i++) {
        const uint8_t *r = ptab + (uint64_t)SHR_PKG_ENTRY * i;
        uint64_t off = shr__rd64(r);
        uint32_t size = shr__rd32(r + 16), count = shr__rd32(r + 24), height = shr__rd16(r + 28);
        if (shr__rd32(r + 20) != glyph || !count || count > SHR_PKG_MAX_PAGE_GLYPHS || !height || height > ah ||
            shr__rd16(r + 30) || size < 24 || size > SHR_PKG_MAX_PAGE_BYTES || off < prev || off > fsize ||
            fsize - off < size)
            return "page record";
        glyph += count;
        prev = off + size;
    }
    if (glyph != nglyphs) return "pages do not cover every glyph";
    pkg->format = format;
    pkg->baseline = baseline;
    pkg->atlas_w = aw, pkg->atlas_h = ah, pkg->stride = format == 1 ? aw / 2u : aw;
    pkg->slot_shape = (uint64_t)format << 56 | (uint64_t)aw << 40 | (uint64_t)ah << 24;
    pkg->nglyphs = nglyphs;
    pkg->cmap = cmap, pkg->ncmap = ncmap;
    pkg->seqs = seqs, pkg->nseqs = nseqs;
    pkg->pool = pool;
    pkg->ptab = ptab;
    return NULL;
}

shr_status shr__page_streams(const shr__page *p, const uint8_t *b, shr__streams *out) {
    const shr__pkg *pkg = p->pkg;
    const uint8_t *e = shr__page_rec(p);
    uint32_t size = shr__rd32(e + 16), flags = shr__rd16(b + 10), method = flags >> 4 & 15, raw = shr__rd32(b + 12),
             an = shr__rd32(b + 20), atlas = shr__rd16(e + 28) * pkg->stride;
    if (memcmp(b + 4, "page", 4) || shr__rd16(b + 8) || shr__rd32(b) != size) return SHR_E_FORMAT;
    if (method >= 2) return SHR_E_UNSUPPORTED;
    /* `x > method`: x when stored */
    if ((flags & ~0xF0u) != SHR_PKG_REQUIRED || method > (shr__rd32(pkg->hdr + 20) >> 3 & 1) ||
        raw != 8 + atlas + shr__page_recs(p) || raw_bad(method, size - 16, raw) ||
        ((shr__rd64(e) + 24) % SHR_PKG_PAGE_ALIGN != 0) > method)
        return SHR_E_FORMAT;
    if (shr__rd32(b + 16) != p->index || an > size - 24 || (an != atlas) > method) return SHR_E_FORMAT;
    *out = (shr__streams){b + 24, b + 24 + an, an, size - 24 - an, method};
    return SHR_OK;
}

bool shr__page_valid(const shr__page *p, const uint8_t *atlas, const uint8_t *recs) {
    const shr__pkg *pkg = p->pkg;
    const uint8_t *rec = shr__page_rec(p);
    uint32_t count = shr__rd32(rec + 24), height = shr__rd16(rec + 28);
    for (uint32_t g = 0; g < count; g++) {
        const uint8_t *e = recs + 16ull * g;
        uint32_t x = shr__rd16(e), y = shr__rd16(e + 2), w = e[4], h = e[5], fmt = e[8] & 3u, cells = (e[8] >> 2) & 3u;
        if ((e[8] & 0xF0) | e[9] | shr__rd32(e + 12) || !cells || cells > 2) return false;
        if (!fmt) {
            if (x | y | w | h) return false;
            continue;
        }
        bool a4 = fmt == 1;
        if (fmt != pkg->format || !w || !h || (a4 && x % 2) || x + w > pkg->atlas_w || y + h > height)
            return false; /* an even A4 x in an atlas of even width leaves room for the padding nibble */
        for (uint32_t r = 0; a4 && w % 2 && r < h; r++)
            if (atlas[(size_t)(y + r) * pkg->stride + (x + w) / 2] & 0x0F) return false; /* A4 padding nibble */
    }
    return true;
}

/* What decoding the metadata takes: the region buffer (read packages), `meta`, the decoder, and the page table. */
static shr_status pkg_alloc(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    size_t region = pkg->mapped ? 0 : (size_t)(pkg->hi - pkg->lo);
    if (!pkg->buf && region && !(pkg->buf = shr__malloc(&f->al, region, 8, SHR_ALLOC_PAYLOAD))) return SHR_E_NO_MEMORY;
    pkg->buf_len = region;
    if (!pkg->meta && pkg->meta_len && !(pkg->meta = shr__malloc(&f->al, pkg->meta_len, 8, SHR_ALLOC_PAYLOAD)))
        return SHR_E_NO_MEMORY;
    if (!pkg->pages && !(pkg->pages = shr__calloc(&f->al, pkg->npages, sizeof(shr__page *), SHR_ALIGNOF(shr__page *),
                                                  SHR_ALLOC_PAYLOAD)))
        return SHR_E_NO_MEMORY;
    return shr__zinit(f, shr__rd32(pkg->hdr + 20));
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
    shr_status st = SHR_E_FORMAT;
    pkg->mapped = true;
    pkg->hdr = pkg->src.data;
    *why = pkg->src.size < SHR_PKG_HEADER ? "package too small" : parse_header(pkg, &st);
    if (!*why) *why = parse_index(pkg, pkg->hdr + SHR_PKG_HEADER, &st);
    if (!*why && (st = pkg_alloc(pkg)) != SHR_OK) return st;
    if (!*why) *why = parse_meta(pkg, pkg->hdr + pkg->lo, &st);
    if (*why) return st;
    pkg_ready(pkg);
    return SHR_OK;
}

/* A mapped package, decoded in place of the reads; one out of memory waits. */
static void pkg_mapped(shr__pkg *pkg) {
    const char *why;
    shr_status st = shr__pkg_map(pkg, &why);
    if (st == SHR_E_NO_MEMORY)
        shr__retry_memory(pkg->font, &pkg->retry);
    else if (st != SHR_OK)
        pkg_fail(pkg, st, why);
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
    if (!src.data && !src.read) {
        pkg_fail(pkg, SHR_E_INVALID_ARG, "font package source without data or read()");
        return;
    }
    pkg->mapped = src.data != NULL;
    pkg->hdr = pkg->header;
    pkg->state = PKG_LOADING;
}

/* An absent package is looked up again after a ready signal. */
bool shr__pkg_due(const shr__pkg *pkg, uint64_t now) {
    const shr_pl_res_bitmap_font *f = pkg->font;
    return shr__retry_due(f, &pkg->retry, now) &&
           ((pkg->state == PKG_UNOPENED && pkg->wanted) ||
            (pkg->state == PKG_LOADING && !pkg->step_busy && (pkg->mapped || !f->blocked)));
}

void shr__pkg_advance(shr__pkg *pkg) {
    shr_pl_res_bitmap_font *f = pkg->font;
    uint64_t now = shr__font_now(f);
    if (!shr__pkg_due(pkg, now)) return;
    shr__retry_begin(f, &pkg->retry);
    if (pkg->state == PKG_UNOPENED) pkg_open(pkg);
    if (pkg->state != PKG_LOADING) return;
    if (pkg->mapped) {
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
    default: /* LOAD_META */
        off = pkg->lo, len = (uint32_t)(pkg->hi - pkg->lo);
        dst = pkg_alloc(pkg) == SHR_OK ? pkg->buf : NULL;
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
        pkg->step = LOAD_INDEX;
        return;
    case LOAD_INDEX:
        if ((why = parse_index(pkg, pkg->buf, &st))) break;
        buf_free(pkg);
        pkg->step = LOAD_META;
        return;
    default: /* LOAD_META */
        if ((why = parse_meta(pkg, pkg->buf, &st))) break;
        buf_free(pkg);
        pkg_ready(pkg);
        return;
    }
    pkg_fail(pkg, st, why);
}

/* The built-in package is baked by the build for the cell size: its one instance, stored. */
shr__line_metrics shr__bitmap_font_line_metrics(void) {
    const uint8_t *d = shr__builtin_package, *e = d + SHR_PKG_HEADER + 24;
    while (memcmp(e, "inst", 4)) e += SHR_PKG_ENTRY;
    const uint8_t *s = d + shr__rd64(e + 8) + 16;
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
