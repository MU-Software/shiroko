#include "shr_vec.h"

bool shr__vec_reserve(shr__vec *v, const shr__alloc *al, size_t extra) {
    size_t need, cap, bytes;
    if (!shr__add_size(v->len, extra, &need)) return false;
    if (need <= v->cap) return true;
    cap = v->cap ? v->cap : need > 8 ? need : 8; /* a first reserve takes what it asks for */
    while (cap < need)
        if (!shr__mul_size(cap, 2, &cap)) return false;
    if (!shr__mul_size(cap, v->elem, &bytes)) return false;
    void *p = shr__malloc(al, bytes, v->align, v->kind);
    if (!p) return false;
    if (v->len) memcpy(p, v->data, v->len * v->elem);
    shr__free(al, v->data, v->cap * v->elem, v->align, v->kind);
    v->data = p;
    v->cap = cap;
    return true;
}

void *shr__vec_push(shr__vec *v, const shr__alloc *al) {
    if (!shr__vec_reserve(v, al, 1)) return NULL;
    return (char *)v->data + v->len++ * v->elem;
}

void shr__vec_free(shr__vec *v, const shr__alloc *al) {
    shr__free(al, v->data, v->cap * v->elem, v->align, v->kind);
    v->data = NULL;
    v->len = v->cap = 0;
}
