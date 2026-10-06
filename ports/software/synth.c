#include <stddef.h>

#include "shr_glyph.h"
#include "shr_rect.h"
#include "synth.h"

#define SYNTH_MIN (16u << 10)
#define SYNTH_MAX ((size_t)1 << 30)
#define SYNTH_ALIGN 8

/* The key (the fields up to x0) and the A8 coverage of footprint columns [x0, x0 + w) of h rows after it. */
typedef struct entry {
    uint32_t buffer; /* 0: dropped */
    uint32_t style;
    int32_t axis;
    shr_rect rect;
    int32_t x0, w, h;
    uint32_t bytes; /* with the coverage, a multiple of SYNTH_ALIGN */
} entry;

static entry *at(const shr__synth *s, size_t off) { return (entry *)(void *)(s->arena + off); }

static bool same(const entry *a, const entry *b) {
    return !((a->buffer ^ b->buffer) | (a->style ^ b->style) | (uint32_t)(a->axis ^ b->axis) |
             (uint32_t)(a->rect.x0 ^ b->rect.x0) | (uint32_t)(a->rect.y0 ^ b->rect.y0) |
             (uint32_t)(a->rect.x1 ^ b->rect.x1) | (uint32_t)(a->rect.y1 ^ b->rect.y1));
}

static uint32_t mul(uint32_t x, uint32_t k) { return (uint32_t)((uint64_t)x * k); }

/* The index slot of the key's entry, else the empty slot it would take. The hash leaves out what tells apart only sizes
 * and slants of one glyph. */
static uint32_t *slot_of(const shr__synth *s, const entry *k) {
    uint32_t h = mul(mul(k->buffer ^ k->style << 28, 0x9E3779B1u) ^ (uint32_t)k->rect.x0, 0x85EBCA77u);
    h = mul(h ^ h >> 15 ^ (uint32_t)k->rect.y0, 0xC2B2AE3Du);
    uint32_t mask = s->slots - 1;
    for (uint32_t i = (h ^ h >> 16) & mask;; i = (i + 1) & mask) {
        uint32_t *p = &s->index[i];
        if (!*p || same(at(s, *p - 1), k)) return p;
    }
}

/* An index of the live entries alone: dropped ones would lengthen the probes of the keys taking their place. */
static void reindex(shr__synth *s) {
    memset(s->index, 0, s->slots * sizeof(*s->index));
    s->count = 0;
    for (size_t off = 0; off < s->used; off += at(s, off)->bytes)
        if (at(s, off)->buffer) *slot_of(s, at(s, off)) = (uint32_t)off + 1, s->count++;
}

/* Moves the entries into an arena of `size` bytes with a new index. */
static bool resize(shr__synth *s, const shr__alloc *al, size_t size) {
    uint32_t slots = 16;
    while (slots < size / 64) slots *= 2;
    shr__synth n = {.arena = shr__malloc(al, size, SYNTH_ALIGN, SHR_ALLOC_PAYLOAD),
                    .index = SHR_NEW_ARRAY(al, uint32_t, slots), .size = size, .used = s->used, .cap = s->cap,
                    .slots = slots};
    if (!n.arena || !n.index) {
        shr__free(al, n.arena, size, SYNTH_ALIGN, SHR_ALLOC_PAYLOAD);
        SHR_FREE_ARRAY(al, n.index, uint32_t, slots);
        return false;
    }
    if (s->used) memcpy(n.arena, s->arena, s->used);
    reindex(&n);
    n.luts = s->luts, s->luts = NULL;
    shr__synth_reset(s, al, s->cap);
    *s = n;
    return true;
}

/* Room for `bytes` more and one more index slot: the arena doubles up to the cap (which drops to the size reached
 * when that fails), else it empties. */
static bool make_room(shr__synth *s, const shr__alloc *al, size_t bytes) {
    while (s->used + bytes > s->size || 2 * (s->count + 1) > s->slots) {
        size_t next = s->size ? 2 * s->size : SYNTH_MIN;
        if (s->size < s->cap && resize(s, al, next < s->cap ? next : s->cap)) continue;
        s->cap = s->size < s->cap ? s->size : s->cap;
        if (!s->used) return false;
        s->used = 0, s->count = 0;
        memset(s->index, 0, s->slots * sizeof(*s->index));
    }
    return true;
}

static const entry *entry_of(shr__synth *s, const shr__alloc *al, const shr_draw_cmd *c, const shr_image *b) {
    uint32_t style = c->flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC);
    entry k = {c->buffer, style, style & SHR_GLYPH_ITALIC ? c->slant_axis : 0, c->src_rect, 0, 0, 0, 0};
    uint32_t *p = s->slots ? slot_of(s, &k) : NULL;
    if (p && *p) return at(s, *p - 1);
    int32_t x1;
    k.h = c->src_rect.y1 - c->src_rect.y0;
    shr__glyph_footprint(c->src_rect.x1 - c->src_rect.x0, k.h, style, k.axis, &k.x0, &x1);
    k.w = x1 - k.x0;
    size_t bytes = (sizeof(entry) + (size_t)k.w * (size_t)k.h + SYNTH_ALIGN - 1) & ~(size_t)(SYNTH_ALIGN - 1);
    if (bytes > s->cap || !make_room(s, al, bytes)) return NULL;
    entry *e = at(s, s->used);
    k.bytes = (uint32_t)bytes;
    *e = k;
    shr__raster_synth((uint8_t *)(e + 1), b, c->src_rect, style, k.axis, k.x0, k.w);
    *slot_of(s, &k) = (uint32_t)s->used + 1;
    s->used += bytes, s->count++;
    return e;
}

/* The LUT of c's colour, DIM and bg in format f; NULL when refused memory. */
static const uint8_t *lut_of(shr__synth *s, const shr__alloc *al, const shr_draw_cmd *c, shr_pixel_format f) {
    shr__synth_luts *t = s->luts ? s->luts : (s->luts = SHR_NEW_ARRAY(al, shr__synth_luts, 1));
    if (!t) return NULL;
    uint32_t dim = c->flags & SHR_GLYPH_DIM;
    uint32_t k0 = (c->color & 0xFFFFFFu) | dim << 24 | (uint32_t)(f == SHR_FORMAT_RGB565) << 25;
    uint32_t k1 = c->bg & 0xFFFFFFu, h = mul(k0 ^ mul(k1, 0x9E3779B1u), 0x85EBCA77u), mask = 2 * SHR__SYNTH_LUTS - 1, i;
    h = (h ^ h >> 16) & mask;
    for (i = h; t->index[i]; i = (i + 1) & mask) {
        uint32_t n = t->index[i] - 1u;
        if (t->key[n][0] == k0 && t->key[n][1] == k1) return t->px[n];
    }
    if (t->count == SHR__SYNTH_LUTS) memset(t->index, 0, sizeof(t->index)), t->count = 0, i = h;
    uint32_t n = t->count++;
    t->index[i] = (uint8_t)(n + 1), t->key[n][0] = k0, t->key[n][1] = k1;
    shr__raster_lut(t->px[n], f, c->color, c->bg, dim);
    return t->px[n];
}

void shr__synth_draw(shr__synth *s, const shr__alloc *al, const shr_surface *dst, const shr_draw_cmd *c,
                     const shr_image *buffers, shr_point origin, shr_rect clip) {
    const entry *e = c->src_rect.x0 < c->src_rect.x1 && !shr__rect_empty(c->dst)
                         ? entry_of(s, al, c, &buffers[c->buffer - 1])
                         : NULL;
    const uint8_t *lut = NULL;
    if (!e || ((c->flags & SHR_GLYPH_ON_FILL) && !(lut = lut_of(s, al, c, dst->format)))) {
        shr__raster_draw(dst, c, buffers, origin, clip, NULL);
        return;
    }
    if (lut) { /* the driver's checks keep dst - origin inside clip */
        shr_rect r = {c->dst.x0 - origin.x, c->dst.y0 - origin.y, c->dst.x1 - origin.x, c->dst.y1 - origin.y};
        const uint8_t *src = (const uint8_t *)(e + 1) + (size_t)c->src_origin.y * (size_t)e->w;
        shr__raster_glyph_lut(dst, r, src + (c->src_origin.x - e->x0), (size_t)e->w, lut);
        return;
    }
    shr_image m = {e + 1, e->w, e->h, (size_t)e->w, (size_t)e->w * (size_t)e->h, SHR_FORMAT_A8, SHR_MEMORY_CPU};
    shr_draw_cmd g = *c;
    g.flags = (uint16_t)(g.flags & ~(SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC));
    g.buffer = 1, g.src_rect = (shr_rect){0, 0, e->w, e->h}, g.src_origin.x -= e->x0;
    shr__raster_draw(dst, &g, &m, origin, clip, NULL);
}

void shr__synth_drop(shr__synth *s, uint32_t buffer, const shr_rect *rect) {
    bool dropped = false;
    for (size_t off = 0; off < s->used; off += at(s, off)->bytes) {
        entry *e = at(s, off);
        if (e->buffer == buffer && (!rect || !shr__rect_empty(shr__rect_intersect(e->rect, *rect))))
            e->buffer = 0, dropped = true;
    }
    if (dropped) reindex(s);
}

void shr__synth_reset(shr__synth *s, const shr__alloc *al, size_t budget) {
    shr__free(al, s->arena, s->size, SYNTH_ALIGN, SHR_ALLOC_PAYLOAD);
    SHR_FREE_ARRAY(al, s->index, uint32_t, s->slots);
    SHR_FREE_ARRAY(al, s->luts, shr__synth_luts, 1);
    *s = (shr__synth){.cap = budget < SYNTH_MAX ? budget : SYNTH_MAX};
}
