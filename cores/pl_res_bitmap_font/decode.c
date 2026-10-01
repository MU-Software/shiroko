#include "font_core.h"

static bool header_is(const shr__box *b, const char *type, const uint8_t *x) {
    return shr__rd32(x) == b->size && !memcmp(x + 4, type, 4) && !shr__rd16(x + 8) && shr__rd16(x + 10) == b->flags &&
           shr__rd32(x + 12) == b->raw;
}

#ifdef SHR_ZSTD
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

_Static_assert(ZSTD_VERSION_NUMBER >= 10505, "zstd 1.5.5 or later");

shr_status shr__zinit(shr_pl_res_bitmap_font *f, uint32_t features) {
    if (!(features & SHR_PKG_ZSTD) || f->zdc) return SHR_OK;
    size_t n = ZSTD_estimateDCtxSize();
    void *ws = shr__malloc(&f->al, n, 8, SHR_ALLOC_PAYLOAD);
    if (!ws) return SHR_E_NO_MEMORY;
    f->zdc = ZSTD_initStaticDCtx(ws, n); /* ws itself: zstd never allocates */
    return SHR_OK;
}

void shr__zfree(shr_pl_res_bitmap_font *f) { shr__free(&f->al, f->zdc, ZSTD_estimateDCtxSize(), 8, SHR_ALLOC_PAYLOAD); }

/* One zstd frame of `n` bytes that decodes to exactly `expected` bytes, its window bounded by the loader. */
static bool unzstd(shr_pl_res_bitmap_font *f, const uint8_t *src, size_t n, uint8_t *dst, size_t expected) {
    ZSTD_frameHeader h;
    return n >= 4 && shr__rd32(src) == ZSTD_MAGICNUMBER && !ZSTD_getFrameHeader(&h, src, n) &&
           ZSTD_findFrameCompressedSize(src, n) == n && h.frameContentSize == expected &&
           h.windowSize <= SHR_PKG_MAX_WINDOW && !h.dictID && !h.checksumFlag && n < expected &&
           ZSTD_decompressDCtx(f->zdc, dst, expected, src, n) == expected;
}

const uint8_t *shr__box_payload(shr_pl_res_bitmap_font *f, const shr__box *b, const char *type, const uint8_t *x,
                                uint8_t *dst, bool place) {
    if (!header_is(b, type, x)) return NULL;
    if (b->flags >> 4) return unzstd(f, x + 16, b->size - 16, dst, b->raw) ? dst : NULL;
    return place ? x + 16 : memcpy(dst, x + 16, b->raw);
}

bool shr__page_decode(shr_pl_res_bitmap_font *f, const shr__page *p, const shr__streams *s, uint8_t *atlas,
                      uint8_t *recs) {
    size_t n = (size_t)shr__rd16(shr__page_rec(p) + 28) * p->pkg->stride;
    if (s->method)
        return unzstd(f, s->atlas, s->atlas_n, atlas, n) && unzstd(f, s->recs, s->recs_n, recs, shr__page_recs(p)) &&
               shr__page_valid(p, atlas, recs);
    memcpy(atlas, s->atlas, s->atlas_n);
    memcpy(recs, s->recs, s->recs_n);
    return shr__page_valid(p, atlas, recs);
}

#else

shr_status shr__zinit(shr_pl_res_bitmap_font *f, uint32_t features) {
    (void)f, (void)features;
    return SHR_OK;
}

void shr__zfree(shr_pl_res_bitmap_font *f) { (void)f; }

const uint8_t *shr__box_payload(shr_pl_res_bitmap_font *f, const shr__box *b, const char *type, const uint8_t *x,
                                uint8_t *dst, bool place) {
    (void)f;
    if (!header_is(b, type, x)) return NULL;
    return place ? x + 16 : memcpy(dst, x + 16, b->raw);
}

bool shr__page_decode(shr_pl_res_bitmap_font *f, const shr__page *p, const shr__streams *s, uint8_t *atlas,
                      uint8_t *recs) {
    (void)f;
    memcpy(atlas, s->atlas, s->atlas_n);
    memcpy(recs, s->recs, s->recs_n);
    return shr__page_valid(p, atlas, recs);
}

#endif
