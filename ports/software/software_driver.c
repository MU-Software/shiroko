#include <shiroko/port_software.h>

#include "raster.h"
#include "shr_alloc.h"
#include "shr_rect.h"
#include "synth.h"

#define KEEP_ALIGN 128
#define PREP_DEFAULT (8u << 20)

/* With a copier's copy_trim: the keep's first `lo` and last `hi` columns hold only the pixel of the pattern `left`, and
 * `right`. */
typedef struct kcopy {
    uint64_t ticket;
    bool writes;
    int32_t lo, hi;
    uint32_t left, right;
} kcopy;

/* `copies`: per keep, its last copy; `pend`: the rows of the destination this batch's copies since `pend_ticket` took. */
typedef struct sw {
    shr__alloc al;
    shr_image *buffers;
    uint32_t **preps; /* per buffer: its plane for IMAGE into RGB565, made by the first such draw within prep_cap */
    size_t prep_used, prep_cap;
    uint32_t nbuffers;
    shr__keeps keeps;
    uint8_t *slots; /* with keeps.slot: id k's at (k - 1) * keeps.slot */
    kcopy *copies;
    shr_software_copier copier;
    uint64_t last, pend_ticket;
    int32_t pend_y0, pend_y1;
    shr__synth synth;
    shr__pal_memo pals;
    shr__line_memo lines;
} sw;

shr_status shr_software_execute(const shr_surface *dst, const shr_draw_cmd *cmds, size_t count, const shr_image *buffers,
                                uint32_t nbuffers) SHR_NONBLOCKING {
    shr_status st = shr__raster_check(dst, cmds, count, buffers, nbuffers, NULL, NULL, NULL);
    if (st != SHR_OK) return st;
    shr_rect all = {0, 0, dst->width, dst->height};
    for (size_t i = 0; i < count; i++)
        if (!shr__buffer_cmd(cmds[i].kind)) shr__raster_draw(dst, &cmds[i], buffers, (shr_point){0, 0}, all, NULL);
    return SHR_OK;
}

static void prep_free(sw *s, uint32_t id) {
    size_t bytes = shr__prep_bytes(&s->buffers[id - 1]);
    shr__free(&s->al, s->preps[id - 1], bytes, 8, SHR_ALLOC_PAYLOAD);
    s->preps[id - 1] = NULL, s->prep_used -= bytes;
}

static void preps_free(sw *s) {
    for (uint32_t i = 0; s->preps && i < s->nbuffers; i++)
        if (s->preps[i]) prep_free(s, i + 1);
}

/* IMAGE into RGB565 from the plane of its buffer, made by the first one that finds room; from the buffer without.
 * Out of line, so draw() stays inlined. */
static __attribute__((noinline)) void image(sw *s, const shr_surface *dst, const shr_draw_cmd *c, shr_point origin,
                                            shr_rect clip) {
    const shr_image *b = &s->buffers[c->buffer - 1];
    uint32_t **p = &s->preps[c->buffer - 1];
    uint64_t bytes = ((uint64_t)b->width * (uint64_t)b->height + (uint64_t)b->height) * 8;
    if (!*p && bytes <= s->prep_cap - s->prep_used && (*p = shr__malloc(&s->al, (size_t)bytes, 8, SHR_ALLOC_PAYLOAD))) {
        s->prep_used += (size_t)bytes;
        shr__raster_prep(*p, b, (shr_rect){0, 0, b->width, b->height});
    }
    if (*p)
        shr__raster_image(dst, c, b, *p, origin, clip);
    else
        shr__raster_draw(dst, c, s->buffers, origin, clip, NULL);
}

/* BOLD and ITALIC GLYPHs from the cache, unscaled IMAGEs into RGB565 from planes, LINE cells kept, the rest as they
 * come. */
static void draw(sw *s, const shr_surface *dst, const shr_draw_cmd *c, shr_point origin, shr_rect clip) {
    if (c->kind == SHR_CMD_GLYPH && (c->flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC)))
        shr__synth_draw(&s->synth, &s->al, dst, c, s->buffers, origin, clip);
    else if (c->kind == SHR_CMD_IMAGE && dst->format == SHR_FORMAT_RGB565 && !(c->flags & SHR_IMAGE_SCALED))
        image(s, dst, c, origin, clip);
    else if (c->kind == SHR_CMD_LINE)
        shr__raster_line(dst, c, origin, clip, &s->lines);
    else
        shr__raster_draw(dst, c, s->buffers, origin, clip, &s->pals);
}

static void copy_wait(sw *s, uint64_t *ticket) {
    if (*ticket) s->copier.wait(s->copier.user, *ticket);
    *ticket = 0;
}

/* A keep's pixels: with keeps.slot, its slot in `slots`, else memory of the keep's own size. */
static void keep_free(sw *s, shr__keep *e) {
    copy_wait(s, &s->copies[e - s->keeps.at].ticket);
    if (!s->keeps.slot) shr__free(&s->al, e->pixels, e->size, KEEP_ALIGN, SHR_ALLOC_PAYLOAD);
    *e = (shr__keep){0};
}

/* The id holds nothing; its slot stays. */
static void keep_empty(sw *s, shr__keep *e) {
    if (!s->keeps.slot) keep_free(s, e);
    *e = (shr__keep){.pixels = e->pixels};
}

/* Memory for a keep the checked batch stores. */
static shr_status keep_alloc(sw *s, shr__keep *e, shr_pixel_format format) {
    int32_t w = e->store_w, h = e->store_h;
    uint64_t size = (uint64_t)w * (uint64_t)h * shr__px_bytes(format);
#if SIZE_MAX < UINT64_MAX
    if (size > SIZE_MAX) return SHR_E_NO_MEMORY;
#endif
    size_t bytes = (size_t)size;
    if (!s->keeps.slot && e->size != bytes) keep_free(s, e);
    if (!e->pixels && !(e->pixels = shr__malloc(&s->al, bytes, KEEP_ALIGN, SHR_ALLOC_PAYLOAD))) return SHR_E_NO_MEMORY;
    e->size = bytes, e->width = w, e->height = h, e->format = format;
    return SHR_OK;
}

/* Before the destination rows of `r` are touched, the copies taking any of them finish. */
static void rows_wait(sw *s, shr_rect r) {
    if (r.y0 >= s->pend_y1 || r.y1 <= s->pend_y0) return;
    copy_wait(s, &s->pend_ticket);
    s->pend_y0 = s->pend_y1 = 0;
}

/* `rows` rows of `bytes` bytes between a keep (`k`; `writes`: into it) and the destination rows of `r`, with `trim` the
 * destination columns around them filled. */
static void copy(sw *s, void *dst, size_t dst_stride, const void *src, size_t src_stride, size_t bytes, shr_rect r,
                 kcopy *k, bool writes, const shr_software_trim *trim) {
    int32_t rows = r.y1 - r.y0;
    if (shr__rect_empty(r)) return;
    void *u = s->copier.user;
    uint64_t t = !s->copier.copy ? 0
                 : trim          ? s->copier.copy_trim(u, dst, dst_stride, src, src_stride, bytes, rows, trim)
                                 : s->copier.copy(u, dst, dst_stride, src, src_stride, bytes, rows);
    if (!t) {
        if (trim) shr_software_trim_fill(dst, dst_stride, bytes, rows, trim);
        for (int32_t y = 0; y < rows; y++)
            memcpy((uint8_t *)dst + (size_t)y * dst_stride, (const uint8_t *)src + (size_t)y * src_stride, bytes);
        return;
    }
    if (!s->pend_ticket) s->pend_y0 = r.y0, s->pend_y1 = r.y1;
    s->pend_y0 = r.y0 < s->pend_y0 ? r.y0 : s->pend_y0, s->pend_y1 = r.y1 > s->pend_y1 ? r.y1 : s->pend_y1;
    s->last = s->pend_ticket = k->ticket = t, k->writes = writes;
}

/* The pixel at `px` repeated to 32 bits, masked by `mask` (0 where the pixel may be undefined). */
static uint32_t pattern(const uint8_t *px, size_t bpp, uint32_t mask) {
    uint8_t b[4];
    uint32_t v;
    __builtin_memcpy(b, px, bpp);
    if (bpp == 2) __builtin_memcpy(b + 2, b, 2);
    __builtin_memcpy(&v, b, 4);
    return v & mask;
}

/* Records the columns at the edges of group `g` that hold one colour by how its commands draw it: from an opaque FILL
 * of its whole height reaching the edge, up to the nearest column a later command draws. `row` is its first row,
 * drawn. */
static void trim_cmds(kcopy *k, const shr_draw_cmd *c, size_t n, shr_rect g, const uint8_t *row, size_t bpp) {
    int32_t w = g.x1 - g.x0, lo = 0, x1 = w;
    if (w <= 0 || g.y1 <= g.y0) { /* no pixels: none to read */
        *k = (kcopy){.ticket = k->ticket, .writes = k->writes};
        return;
    }
    for (size_t i = 0; i < n; i++) { /* inside g (checked); an empty one only shortens the edges */
        shr_rect r = c[i].dst;
        bool cover = c[i].kind == SHR_CMD_FILL && !(c[i].flags & SHR_GLYPH_DIM) && r.y0 == g.y0 && r.y1 == g.y1;
        r.x0 -= g.x0, r.x1 -= g.x0;
        lo = cover && !r.x0 ? r.x1 : r.x0 < lo ? r.x0 : lo;
        x1 = cover && r.x1 == w ? r.x0 : r.x1 > x1 ? r.x1 : x1;
    }
    k->lo = lo, k->hi = w - (x1 > lo ? x1 : lo);
    k->left = pattern(row, bpp, UINT32_MAX * (k->lo > 0));
    k->right = pattern(row + (size_t)(w - 1) * bpp, bpp, UINT32_MAX * (k->hi > 0));
}

static void fill_bytes(uint8_t *p, size_t bytes, uint32_t v) {
    uint8_t *end = p + bytes;
    if (p < end && ((uintptr_t)p & 2)) __builtin_memcpy(p, &v, 2), p += 2;
    for (; end - p >= 16; p += 16) { /* unrolled: gcc -O2 does not */
        __builtin_memcpy(p, &v, 4), __builtin_memcpy(p + 4, &v, 4);
        __builtin_memcpy(p + 8, &v, 4), __builtin_memcpy(p + 12, &v, 4);
    }
    for (; end - p >= 4; p += 4) __builtin_memcpy(p, &v, 4);
    if (p < end) __builtin_memcpy(p, &v, 2);
}

void shr_software_trim_fill(void *dst, size_t stride, size_t bytes, int32_t rows, const shr_software_trim *t) {
    for (int32_t y = 0; y < rows; y++) {
        uint8_t *p = (uint8_t *)dst + (size_t)y * stride;
        fill_bytes(p - t->lo, t->lo, t->left);
        fill_bytes(p + bytes, t->hi, t->right);
    }
}

/* Renders the group at cmds[at] into its keep, or into the destination when the KEEP_DRAW right after it takes the
 * whole group, then copies it into the keep; returns the index of the last command it ran. */
static size_t keep_store(sw *s, const shr_surface *dst, const shr_draw_cmd *cmds, size_t count, size_t at) {
    uint32_t id = cmds[at].buffer;
    const shr__keep *e = &s->keeps.at[id - 1];
    kcopy *k = &s->copies[id - 1];
    shr_rect g = cmds[at].dst;
    size_t bpp = shr__px_bytes(e->format), stride = (size_t)e->width * bpp, end = at + 1;
    while (cmds[end].kind != SHR_CMD_KEEP_END) end++;
    copy_wait(s, &k->ticket);
    if (end + 1 < count && cmds[end + 1].kind == SHR_CMD_KEEP_DRAW && cmds[end + 1].buffer == id &&
        !memcmp(&cmds[end + 1].dst, &g, sizeof(g))) {
        const uint8_t *src = (const uint8_t *)dst->pixels + (size_t)g.y0 * dst->stride + (size_t)g.x0 * bpp;
        rows_wait(s, g);
        for (size_t i = at + 1; i < end; i++) draw(s, dst, &cmds[i], (shr_point){0, 0}, g);
        if (s->copier.copy_trim) trim_cmds(k, cmds + at + 1, end - at - 1, g, src, bpp);
        copy(s, e->pixels, stride, src, dst->stride, stride, g, k, true, NULL);
        return end + 1;
    }
    shr_surface ks = {.pixels = e->pixels, .width = e->width, .height = e->height, .stride = stride,
                      .byte_length = e->size, .format = e->format, .domain = SHR_MEMORY_CPU};
    for (size_t i = at + 1; i < end; i++)
        draw(s, &ks, &cmds[i], (shr_point){g.x0, g.y0}, (shr_rect){0, 0, e->width, e->height});
    if (s->copier.copy_trim) trim_cmds(k, cmds + at + 1, end - at - 1, g, e->pixels, bpp);
    return end;
}

/* With the keep's edge columns known, copies from and to multiples of the copier's align bytes of the destination (at
 * least one pixel) and has the copier fill the columns of the edge pixels around them. */
static void keep_draw(sw *s, const shr_surface *dst, const shr_draw_cmd *c) {
    const shr__keep *e = &s->keeps.at[c->buffer - 1];
    kcopy *k = &s->copies[c->buffer - 1];
    size_t bpp = shr__px_bytes(dst->format), stride = (size_t)e->width * bpp, w = (size_t)(c->dst.x1 - c->dst.x0);
    uint8_t *d = (uint8_t *)dst->pixels + (size_t)c->dst.y0 * dst->stride + (size_t)c->dst.x0 * bpp;
    const uint8_t *src = (const uint8_t *)e->pixels + (size_t)c->src_origin.y * stride + (size_t)c->src_origin.x * bpp;
    if (k->writes) copy_wait(s, &k->ticket);
    rows_wait(s, c->dst);
    size_t c0 = 0, c1 = w;
    if ((k->lo | k->hi) && !(((uintptr_t)d | dst->stride) % bpp)) { /* rows off the pixel grid: copied whole */
        int32_t l = k->lo - c->src_origin.x, r = e->width - k->hi - c->src_origin.x;
        size_t a = s->copier.align ? s->copier.align : 1;
        c0 = l <= 0 ? 0 : (size_t)l < w ? (size_t)l : w;
        c1 = r <= (int32_t)c0 ? c0 : (size_t)r < w ? (size_t)r : w;
        if (c0 == c1) c0 -= (c0 == w) & (w != 0), c1 = c0 + 1;
        uintptr_t b0 = (uintptr_t)d + c0 * bpp, b1 = (uintptr_t)d + c1 * bpp + a - 1;
        b0 -= b0 % a, b1 -= b1 % a;
        c0 = b0 > (uintptr_t)d ? (b0 - (uintptr_t)d) / bpp : 0;
        c1 = (b1 - (uintptr_t)d) / bpp < w ? (b1 - (uintptr_t)d) / bpp : w;
    }
    shr_software_trim t = {c0 * bpp, (w - c1) * bpp, k->left, k->right};
    copy(s, d + c0 * bpp, dst->stride, src + c0 * bpp, stride, (c1 - c0) * bpp, c->dst, k, false,
         t.lo | t.hi ? &t : NULL);
}

/* Buffer commands and KEEP_RELEASE take effect before the batch is checked: after an error the registrations and
 * keeps are unspecified. */
static shr_status sw_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t count,
                             shr_fence fence) {
    (void)fence;
    sw *s = user;
    size_t i = 0;
    for (; cmds && i < count && shr__buffer_cmd(cmds[i].kind); i++) {
        const shr_draw_cmd *c = &cmds[i];
        if (c->kind == SHR_CMD_KEEP_RELEASE) {
            if (c->buffer && c->buffer <= s->keeps.n) keep_empty(s, &s->keeps.at[c->buffer - 1]);
            continue;
        }
        shr_image m;
        shr_status st = shr__raster_buffer_check(c, s->buffers, s->nbuffers, NULL, NULL, &m);
        if (st != SHR_OK) return st;
        uint32_t *plane = c->buffer && c->buffer <= s->nbuffers ? s->preps[c->buffer - 1] : NULL;
        if (plane && c->kind == SHR_CMD_BUFFER_UPDATE)
            shr__raster_prep(plane, &s->buffers[c->buffer - 1], c->src_rect);
        else if (plane)
            prep_free(s, c->buffer);
        if (c->kind == SHR_CMD_BUFFER_REGISTER)
            s->buffers[c->buffer - 1] = m;
        else if (c->kind == SHR_CMD_BUFFER_RELEASE && c->buffer && c->buffer <= s->nbuffers)
            s->buffers[c->buffer - 1] = (shr_image){0};
        shr__synth_drop(&s->synth, c->buffer, c->kind == SHR_CMD_BUFFER_UPDATE ? &c->src_rect : NULL);
    }
    shr_status st = shr__raster_check(dst, cmds, count, s->buffers, s->nbuffers, &s->keeps, NULL, NULL);
    if (st != SHR_OK) return st;
    for (uint32_t k = 0; k < s->keeps.nstores; k++) /* out of memory, the stores allocated so far hold nothing */
        if (keep_alloc(s, &s->keeps.at[s->keeps.stores[k] - 1], dst->format) != SHR_OK) {
            while (k) keep_empty(s, &s->keeps.at[s->keeps.stores[--k] - 1]);
            return SHR_E_NO_MEMORY;
        }
    shr_rect all = {0, 0, dst->width, dst->height};
    s->pend_ticket = 0;
    for (; i < count; i++) {
        const shr_draw_cmd *c = &cmds[i];
        if (c->kind == SHR_CMD_KEEP_BEGIN) {
            i = keep_store(s, dst, cmds, count, i);
        } else if (c->kind == SHR_CMD_KEEP_DRAW) {
            keep_draw(s, dst, c);
        } else {
            rows_wait(s, c->kind == SHR_CMD_COPY && shr__raster_reads_dst(dst, &c->src) ? all : c->dst);
            draw(s, dst, c, (shr_point){0, 0}, all);
        }
    }
    return SHR_OK;
}

static shr_status sw_reset(void *user) {
    sw *s = user;
    copy_wait(s, &s->last);
    return SHR_OK;
}

static void sw_free(sw *s) {
    shr__alloc al = s->al;
    shr__synth_reset(&s->synth, &al, 0);
    for (uint32_t i = 0; s->keeps.at && s->copies && i < s->keeps.n; i++) keep_free(s, &s->keeps.at[i]);
    shr__free(&al, s->slots, (size_t)(s->keeps.n * s->keeps.slot), KEEP_ALIGN, SHR_ALLOC_PAYLOAD);
    preps_free(s);
    SHR_FREE_HOT_ARRAY(&al, s->preps, uint32_t *, s->nbuffers);
    SHR_FREE_HOT_ARRAY(&al, s->copies, kcopy, s->keeps.n);
    SHR_FREE_HOT_ARRAY(&al, s->keeps.at, shr__keep, s->keeps.n);
    SHR_FREE_HOT_ARRAY(&al, s->keeps.stores, uint32_t, s->keeps.n);
    SHR_FREE_HOT_ARRAY(&al, s->buffers, shr_image, s->nbuffers);
    SHR_DELETE(&al, s, sw);
}

shr_status shr_software_driver_create(const shr_allocator *allocator, uint64_t keep_bytes, uint32_t max_keeps,
                                      uint32_t max_buffers, shr_framebuffer_driver *out) {
    if (!out) return SHR_E_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    uint64_t slot = max_keeps ? keep_bytes / max_keeps / KEEP_ALIGN * KEEP_ALIGN : 0;
#if SIZE_MAX < UINT64_MAX
    if (max_keeps * slot > SIZE_MAX) return SHR_E_INVALID_ARG;
#endif
    shr__alloc al;
    if (!shr__alloc_init(&al, allocator) || (keep_bytes && max_keeps && !slot)) return SHR_E_INVALID_ARG;
    sw *s = SHR_NEW(&al, sw);
    if (!s) return SHR_E_NO_MEMORY;
    *s = (sw){.al = al, .nbuffers = max_buffers, .keeps = {.n = max_keeps, .slot = slot},
               .prep_cap = PREP_DEFAULT, .synth = {.cap = SHR__SYNTH_DEFAULT}};
    s->buffers = SHR_NEW_HOT_ARRAY(&al, shr_image, max_buffers);
    s->preps = SHR_NEW_HOT_ARRAY(&al, uint32_t *, max_buffers);
    s->keeps.at = SHR_NEW_HOT_ARRAY(&al, shr__keep, max_keeps);
    s->keeps.stores = SHR_NEW_HOT_ARRAY(&al, uint32_t, max_keeps);
    s->copies = SHR_NEW_HOT_ARRAY(&al, kcopy, max_keeps);
    s->slots = slot ? shr__malloc(&al, (size_t)(max_keeps * slot), KEEP_ALIGN, SHR_ALLOC_PAYLOAD) : NULL;
    if (!s->buffers || !s->preps || !s->keeps.at || !s->keeps.stores || !s->copies || (slot && !s->slots)) {
        sw_free(s);
        return SHR_E_NO_MEMORY;
    }
    for (uint32_t i = 0; slot && i < max_keeps; i++) s->keeps.at[i].pixels = s->slots + i * (size_t)slot;
    shr_framebuffer_driver_init(out);
    out->user = s;
    out->caps.domains = SHR_MEMORY_CPU | SHR_MEMORY_DMA;
    out->caps.max_buffers = max_buffers;
    out->caps.max_keeps = max_keeps;
    out->caps.keep_bytes = keep_bytes;
    out->caps.max_keep_bytes = slot;
    out->caps.flags = SHR_DRIVER_CHEAP_MOVE | SHR_DRIVER_SCALE;
    out->execute = sw_execute;
    out->reset = sw_reset;
    return SHR_OK;
}

shr_status shr_software_driver_synth_cache(shr_framebuffer_driver *driver, size_t bytes) {
    if (!driver || driver->execute != sw_execute || !driver->user) return SHR_E_INVALID_ARG;
    sw *s = driver->user;
    shr__synth_reset(&s->synth, &s->al, bytes);
    return SHR_OK;
}

shr_status shr_software_driver_image_planes(shr_framebuffer_driver *driver, size_t bytes) {
    if (!driver || driver->execute != sw_execute || !driver->user) return SHR_E_INVALID_ARG;
    sw *s = driver->user;
    preps_free(s);
    s->prep_cap = bytes;
    return SHR_OK;
}

shr_status shr_software_driver_destroy(shr_framebuffer_driver *driver) {
    if (!driver || driver->execute != sw_execute || !driver->user) return SHR_E_INVALID_ARG;
    sw_free(driver->user);
    memset(driver, 0, sizeof(*driver));
    return SHR_OK;
}

shr_status shr_software_driver_set_copier(shr_framebuffer_driver *driver, const shr_software_copier *copier) {
    if (!driver || driver->execute != sw_execute || !driver->user || (copier && (!copier->copy || !copier->wait)))
        return SHR_E_INVALID_ARG;
    sw *s = driver->user;
    copy_wait(s, &s->last);
    for (uint32_t i = 0; i < s->keeps.n; i++) s->copies[i] = (kcopy){0};
    s->copier = copier ? *copier : (shr_software_copier){0};
    driver->caps.flags = (driver->caps.flags & ~(uint32_t)SHR_DRIVER_CHEAP_STORE) | (copier ? SHR_DRIVER_CHEAP_STORE : 0);
    return SHR_OK;
}
