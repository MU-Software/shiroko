#ifndef SHIROKO_SHR_VEC_H
#define SHIROKO_SHR_VEC_H

#include "shr_alloc.h"

typedef struct shr__vec {
    void *data;
    size_t len, cap, elem, align;
    shr_alloc_kind kind; /* of `data` */
} shr__vec;

#define SHR_VEC_INIT(v, T) (*(v) = (shr__vec){NULL, 0, 0, sizeof(T), SHR_ALIGNOF(T), SHR_ALLOC_PAYLOAD})
#define SHR_VEC_INIT_HOT(v, T) (*(v) = (shr__vec){NULL, 0, 0, sizeof(T), SHR_ALIGNOF(T), SHR__HOT})
#define SHR_VEC_AT(v, T, i) (&((T *)(v)->data)[i])

bool shr__vec_reserve(shr__vec *v, const shr__alloc *al, size_t extra);
void *shr__vec_push(shr__vec *v, const shr__alloc *al);
void shr__vec_free(shr__vec *v, const shr__alloc *al);

#endif
