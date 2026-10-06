#ifndef SHIROKO_SHR_ALLOC_H
#define SHIROKO_SHR_ALLOC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <shiroko/shiroko_driver.h>

#define SHR_ALIGNOF(t) _Alignof(t)

static inline bool shr__add_size(size_t a, size_t b, size_t *out) { return !__builtin_add_overflow(a, b, out); }
static inline bool shr__mul_size(size_t a, size_t b, size_t *out) { return !__builtin_mul_overflow(a, b, out); }

typedef struct shr__alloc {
    shr_allocator a;
} shr__alloc;

static inline void *shr__default_alloc(void *user, size_t size, size_t align, shr_alloc_kind kind) {
    (void)user, (void)kind;
    if (align <= _Alignof(max_align_t)) return malloc(size);
    return aligned_alloc(align, (size + align - 1) / align * align);
}

static inline void shr__default_free(void *user, void *p, size_t size, size_t align, shr_alloc_kind kind) {
    (void)user, (void)size, (void)align, (void)kind;
    free(p);
}

/* Payload used every frame. */
enum { SHR__HOT = SHR_ALLOC_PAYLOAD | SHR_ALLOC_HOT };

/* false for a half-filled allocator or unknown flags. */
static inline bool shr__alloc_init(shr__alloc *out, const shr_allocator *in) {
    out->a = (in && in->alloc) ? *in : (shr_allocator){NULL, shr__default_alloc, shr__default_free, 0};
    return !in || (!in->alloc == !in->free && !(in->flags & ~(uint32_t)SHR_ALLOC_HOT));
}

/* The hints the allocator did not ask for are dropped. */
static inline shr_alloc_kind shr__kind(const shr__alloc *al, shr_alloc_kind kind) {
    return (shr_alloc_kind)((uint32_t)kind & (al->a.flags | ~(uint32_t)SHR_ALLOC_HOT));
}

static inline void *shr__malloc(const shr__alloc *al, size_t size, size_t align, shr_alloc_kind kind) {
    return al->a.alloc(al->a.user, size ? size : 1, align, shr__kind(al, kind));
}

static inline void *shr__calloc(const shr__alloc *al, size_t count, size_t size, size_t align, shr_alloc_kind kind) {
    size_t total;
    if (!shr__mul_size(count, size, &total)) return NULL;
    void *p = shr__malloc(al, total, align, kind);
    if (p) memset(p, 0, total ? total : 1);
    return p;
}

static inline void shr__free(const shr__alloc *al, void *p, size_t size, size_t align, shr_alloc_kind kind) {
    if (p) al->a.free(al->a.user, p, size ? size : 1, align, shr__kind(al, kind));
}

#define SHR_NEW(al, T) ((T *)shr__calloc((al), 1, sizeof(T), SHR_ALIGNOF(T), SHR_ALLOC_DESCRIPTOR))
#define SHR_DELETE(al, p, T) shr__free((al), (p), sizeof(T), SHR_ALIGNOF(T), SHR_ALLOC_DESCRIPTOR)
#define SHR_NEW_ARRAY(al, T, n) ((T *)shr__calloc((al), (n), sizeof(T), SHR_ALIGNOF(T), SHR_ALLOC_PAYLOAD))
#define SHR_FREE_ARRAY(al, p, T, n) shr__free((al), (p), sizeof(T) * (n), SHR_ALIGNOF(T), SHR_ALLOC_PAYLOAD)
#define SHR_NEW_HOT_ARRAY(al, T, n) ((T *)shr__calloc((al), (n), sizeof(T), SHR_ALIGNOF(T), SHR__HOT))
#define SHR_FREE_HOT_ARRAY(al, p, T, n) shr__free((al), (p), sizeof(T) * (n), SHR_ALIGNOF(T), SHR__HOT)

#endif
