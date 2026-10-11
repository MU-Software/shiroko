#include "shr_package.h"

const char shr__box_types[] = "manistrssrcsinstctriseqspoolptabcovr"; /* by BOX_ */
#define PACKED_BOXES (1u << BOX_CTRI | 1u << BOX_SEQS | 1u << BOX_POOL) /* may be zstd */

bool shr__raw_bad(uint32_t method, uint64_t n, uint32_t raw) { return raw < n || (raw > n) != method; }

bool shr__box_header_is(const shr__box *b, const char *type, const uint8_t *x) {
    return shr__rd32(x) == b->size && !memcmp(x + 4, type, 4) && !shr__rd16(x + 8) && shr__rd16(x + 10) == b->flags &&
           shr__rd32(x + 12) == b->raw;
}

shr_status shr__pkg_header(const shr__pkg_index *ix, const char **why) {
    const uint8_t *h = ix->hdr;
    if (memcmp(h, "\x80\0\0\0shrf", 8)) return *why = "bad magic", SHR_E_FORMAT;
    if (shr__rd16(h + 8) || shr__rd16(h + 10) != SHR_PKG_REQUIRED || shr__rd32(h + 12) != SHR_PKG_HEADER - 16 ||
        shr__rd16(h + 16) != SHR_PKG_VERSION || shr__rd16(h + 18))
        return *why = "header version", SHR_E_FORMAT;
    if (XXH3_64bits(h, 120) != shr__rd64(h + 120)) return *why = "header checksum", SHR_E_CHECKSUM;
    if (shr__rd32(h + 20) & ~SHR_PKG_FEATURES) return *why = "unsupported required feature", SHR_E_UNSUPPORTED;
    for (int i = 88; i < 120; i++)
        if (h[i]) return *why = "reserved header bytes", SHR_E_FORMAT;
    uint32_t n = shr__rd32(h + 28);
    uint64_t fsize = shr__rd64(h + 32);
    if (fsize != ix->size || shr__rd64(h + 40) != SHR_PKG_HEADER || n < 24 || (n - 24) % SHR_PKG_ENTRY ||
        n > SHR_PKG_MAX_INDEX || fsize - SHR_PKG_HEADER < n)
        return *why = "file size or index", SHR_E_FORMAT;
    if (memcmp(h + 48, shr__text_profile_id, 32)) return *why = "text profile mismatch", SHR_E_PROFILE_MISMATCH;
    return SHR_OK;
}

shr_status shr__pkg_index_parse(shr__pkg_index *ix, const uint8_t *s, const char **why) {
    const uint8_t *h = ix->hdr;
    uint32_t n = shr__rd32(h + 28), count = (n - 24) / SHR_PKG_ENTRY, zstd = shr__rd32(h + 20) >> 3 & 1;
    uint64_t fsize = shr__rd64(h + 32), end = SHR_PKG_HEADER + n, raw = 0;
    if (XXH3_64bits(s, n) != shr__rd64(h + 80)) return *why = "index checksum", SHR_E_CHECKSUM;
    if (shr__rd32(s) != n || memcmp(s + 4, "sidx", 4) || shr__rd16(s + 8) || shr__rd16(s + 10) != SHR_PKG_REQUIRED ||
        shr__rd32(s + 12) != n - 16 || shr__rd32(s + 16) != count || shr__rd32(s + 20))
        return *why = "index box", SHR_E_FORMAT;
    memset(ix->box, 0, sizeof(ix->box));
    ix->nbox = 0, ix->meta_len = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = s + 24 + SHR_PKG_ENTRY * i;
        uint32_t flags = shr__rd16(e + 4), size = shr__rd32(e + 16), method = flags >> 4 & 15, k = 0;
        uint64_t off = shr__rd64(e + 8);
        if (size < 16 || off < end || off > fsize || fsize - off < size)
            return *why = "index entry out of order or range", SHR_E_FORMAT;
        end = off + size;
        if (!memcmp(e, "shrf", 4) || !memcmp(e, "sidx", 4) || !memcmp(e, "page", 4) || !memcmp(e, "free", 4))
            return *why = "box type in the index", SHR_E_FORMAT;
        while (k < BOX_COUNT && memcmp(e, shr__box_types + 4 * k, 4)) k++;
        if (k == BOX_COUNT) {
            if (flags & SHR_PKG_REQUIRED) return *why = "unsupported required box", SHR_E_UNSUPPORTED;
            continue;
        }
        shr__box *b = &ix->box[k];
        if (b->size) return *why = "duplicate box", SHR_E_FORMAT;
        if (method >= 2) return *why = "unsupported compression", SHR_E_UNSUPPORTED;
        if (flags & ~0xF1u || (!(flags & SHR_PKG_REQUIRED) && k != BOX_COVR) ||
            method > ((PACKED_BOXES >> k) & zstd) || shr__rd16(e + 6))
            return *why = "box flags", SHR_E_FORMAT;
        if (shr__raw_bad(method, size - 16, shr__rd32(e + 20))) return *why = "box raw size", SHR_E_FORMAT;
        *b = (shr__box){off, shr__rd64(e + 24), size, shr__rd32(e + 20), flags};
        ix->lo = ix->nbox ? ix->lo : off;
        ix->hi = end;
        ix->order[ix->nbox++] = (uint8_t)k;
        raw += b->raw;
        ix->meta_len += shr__box_room(ix, b);
    }
    if (!ix->box[BOX_MANI].size || !ix->box[BOX_INST].size || !ix->box[BOX_CTRI].size || !ix->box[BOX_PTAB].size)
        return *why = "missing box", SHR_E_FORMAT;
    if ((ix->hi - ix->lo > SHR_PKG_MAX_META) | (raw > SHR_PKG_MAX_META))
        return *why = "package index too large", SHR_E_LIMIT;
    ix->end = end;
    ix->npages = ix->box[BOX_PTAB].raw / SHR_PKG_PTAB;
    return SHR_OK;
}

shr_status shr__pkg_mani(const shr__pkg_index *ix, const uint8_t *man, uint8_t role, const char *locale,
                         const char **why) {
    uint32_t strs = ix->box[BOX_STRS].raw;
    if (ix->box[BOX_MANI].raw != 36 || man[1] | man[2] | man[3]) return *why = "mani", SHR_E_FORMAT;
    if (man[0] < ROLE_LATIN || man[0] > ROLE_NERD) return *why = "role", SHR_E_FORMAT;
    for (int i = 0; i < 8; i++)
        if (man[4 + i] && (man[4 + i] < 0x20 || man[4 + i] > 0x7E)) return *why = "locale", SHR_E_FORMAT;
    if (role != ROLE_BUILTIN && (man[0] != role || (role == ROLE_CJK && strncmp((const char *)man + 4, locale, 8))))
        return *why = "package role or locale differs from its file name", SHR_E_FORMAT;
    if ((uint64_t)shr__rd32(man + 12) + shr__rd32(man + 16) > strs ||
        (uint64_t)shr__rd32(man + 20) + shr__rd32(man + 24) > strs)
        return *why = "string reference", SHR_E_FORMAT;
    uint32_t nglyphs = shr__rd32(man + 28);
    if (!nglyphs || nglyphs > SHR_PKG_MAX_RECORDS) return *why = "glyph count", SHR_E_FORMAT;
    return SHR_OK;
}

static bool shape_side(uint32_t v) { return v >= 64 && v <= 512 && !(v & (v - 1)); }

shr_status shr__pkg_inst(const shr__pkg_index *ix, const uint8_t *in, const char **why) {
    if (ix->box[BOX_INST].raw != 48) return *why = "inst size", SHR_E_FORMAT;
    uint16_t lh = shr__rd16(in + 4), cw = shr__rd16(in + 6), aw = shr__rd16(in + 36), ah = shr__rd16(in + 38);
    int16_t baseline = (int16_t)shr__rd16(in + 12), under = (int16_t)shr__rd16(in + 14),
            strike = (int16_t)shr__rd16(in + 16);
    uint8_t format = in[3];
    if (ix->box[BOX_SRCS].size && shr__rd16(in) >= ix->box[BOX_SRCS].raw / 48)
        return *why = "instance source", SHR_E_FORMAT;
    if (in[2] || (format != 1 && format != 2) || !(shr__rd32(ix->hdr + 20) & format))
        return *why = "instance style or format", SHR_E_FORMAT;
    if (!lh || lh > SHR_MAX_CELL_SIZE || !cw || cw > SHR_MAX_CELL_SIZE) return *why = "instance size", SHR_E_FORMAT;
    if (baseline < 0 || baseline > lh || under < 0 || under >= lh || strike < 0 || strike >= lh)
        return *why = "instance line metrics", SHR_E_FORMAT;
    if (shr__rd16(in + 18) & ~1u || shr__rd64(in + 40)) return *why = "instance reserved bytes", SHR_E_FORMAT;
    if (!shape_side(aw) || !shape_side(ah)) return *why = "instance page shape", SHR_E_FORMAT;
    return SHR_OK;
}

/* One read, finished before read() returns. */
static shr_status sync_read(const shr_asset_source *src, uint64_t off, uint32_t len, uint8_t *dst, const char **why) {
    shr_status st = src->read(src->user, off, len, dst, 0);
    if (st == SHR_IN_PROGRESS && src->cancel) src->cancel(src->user, 0);
    *why = "package read failed";
    return st == SHR_OK || st == SHR_E_WOULD_BLOCK ? st : st == SHR_IN_PROGRESS ? SHR_E_UNSUPPORTED : SHR_E_IO;
}

shr_status shr__pkg_describe(const shr_asset_source *src, shr__pkg_desc *out, const char **why) {
    static const uint8_t which[2] = {BOX_MANI, BOX_INST};
    uint8_t head[SHR_PKG_HEADER], index[SHR_PKG_MAX_INDEX], boxes[2][64];
    const uint8_t *x[2] = {boxes[0], boxes[1]};
    *why = "package source without data or read()";
    if (!src || !out || (!src->data && !src->read)) return SHR_E_INVALID_ARG;
    shr__pkg_index ix = {.hdr = src->data ? src->data : head, .size = src->size, .mapped = src->data != NULL};
    *why = "package too small";
    shr_status st = src->size < SHR_PKG_HEADER ? SHR_E_FORMAT
                    : ix.mapped                ? SHR_OK
                                               : sync_read(src, 0, SHR_PKG_HEADER, head, why);
    if (st == SHR_OK) st = shr__pkg_header(&ix, why);
    if (st == SHR_OK && !ix.mapped) st = sync_read(src, SHR_PKG_HEADER, shr__rd32(ix.hdr + 28), index, why);
    if (st == SHR_OK) st = shr__pkg_index_parse(&ix, ix.mapped ? ix.hdr + SHR_PKG_HEADER : index, why);
    for (int i = 0; i < 2 && st == SHR_OK; i++) {
        const shr__box *b = &ix.box[which[i]];
        if (b->size > sizeof(boxes[i])) return *why = "mani or inst size", SHR_E_FORMAT;
        if (ix.mapped)
            x[i] = ix.hdr + b->off;
        else
            st = sync_read(src, b->off, b->size, boxes[i], why);
        if (st == SHR_OK && XXH3_64bits(x[i], b->size) != b->xxh3) *why = "box checksum", st = SHR_E_CHECKSUM;
        if (st == SHR_OK && !shr__box_header_is(b, shr__box_types + 4 * which[i], x[i]))
            *why = "box header or payload", st = SHR_E_FORMAT;
    }
    const uint8_t *man = x[0] + 16, *in = x[1] + 16;
    if (st == SHR_OK) st = shr__pkg_mani(&ix, man, ROLE_BUILTIN, NULL, why);
    if (st == SHR_OK) st = shr__pkg_inst(&ix, in, why);
    if (st != SHR_OK) return st;
    *out = (shr__pkg_desc){.role = man[0], .cell_width = shr__rd16(in + 6), .line_height = shr__rd16(in + 4),
                           .page_width = shr__rd16(in + 36), .page_height = shr__rd16(in + 38), .format = in[3],
                           .lines = {(int16_t)shr__rd16(in + 12), (int16_t)shr__rd16(in + 14),
                                     (int16_t)shr__rd16(in + 16)}};
    if (man[0] == ROLE_CJK) memcpy(out->locale, man + 4, 8);
    *why = NULL;
    return SHR_OK;
}
