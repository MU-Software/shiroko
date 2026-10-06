#ifndef SHIROKO_TEST_CHECK_H
#define SHIROKO_TEST_CHECK_H

#include <shiroko/shiroko.h>

#include <stdlib.h>

#define GREATEST_USE_LONGJMP 1
#include <greatest.h>

/* Evaluates each side once and longjmps out of the test, so helpers can use it too. */
#define ASSERT_EQ_LL(a, b)                                                                \
    do {                                                                                  \
        long long shr_a_ = (long long)(a), shr_b_ = (long long)(b);                       \
        greatest_info.assertions++;                                                       \
        if (shr_a_ != shr_b_) {                                                           \
            GREATEST_FPRINTF(GREATEST_STDOUT, "\n%lld != %lld\n", shr_a_, shr_b_);        \
            FAIL_WITH_LONGJMPm(#a " == " #b);                                             \
        }                                                                                 \
    } while (0)

/* `m` as a command carries it. */
static inline shr_image_ref img_ref(shr_image m) {
    return (shr_image_ref){m.pixels, m.width, m.height, (uint32_t)m.stride, (uint8_t)m.format, (uint8_t)m.domain, 0};
}

/* Allocator whose allocations fail once `budget` successful ones were made; -1 never fails. */
typedef struct fail_alloc {
    long budget;
    long live;
} fail_alloc;

static void *fa_alloc(void *user, size_t size, size_t align, shr_alloc_kind kind) {
    fail_alloc *f = user;
    (void)kind;
    if (f->budget == 0) return NULL;
    if (f->budget > 0) f->budget--;
    void *p = align <= 16 ? malloc(size) : aligned_alloc(align, (size + align - 1) / align * align);
    if (p) f->live++;
    return p;
}

static void fa_free(void *user, void *p, size_t size, size_t align, shr_alloc_kind kind) {
    (void)size, (void)align, (void)kind;
    ((fail_alloc *)user)->live--;
    free(p);
}

static inline shr_allocator fail_allocator(fail_alloc *f) { return (shr_allocator){f, fa_alloc, fa_free, 0}; }

#endif
