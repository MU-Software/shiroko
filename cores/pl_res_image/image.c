#include "shr_compositor.h"
#include "shr_scale.h"

/* Frames resolve to buf[cur]. An update while the frame in flight reads it goes to the other buffer, made on
 * demand, which then becomes current; `behind` is where the other buffer lags the current one. */
struct shr_pl_res_image {
    shr__res res;
    shr__buf buf[2];
    int cur;
    shr_rect behind;
    shr__buf *pinned; /* by the frame in flight, one at a time; it resolves to it throughout, band after band */
    shr__resolved resolved;
    uint64_t last_frame;
    size_t bytes;
    shr_pl_res_image *base;  /* a view the driver scales: `src` of base drawn as w x h */
    shr_pl_res_image *views; /* the views of an image */
    shr_pl_res_image *next;  /* the next view of base */
    shr_rect src;
    int32_t w, h;
};

typedef struct budget {
    uint64_t used;
} budget;

static const char budget_kind;

static budget *img_budget(shr_context *ctx) { return *shr__ctx_plugin_slot(ctx, &budget_kind); }

/* No room for `bytes` more; also guards the size_t conversions. */
static bool over_budget(shr_context *ctx, uint64_t used, uint64_t bytes) {
    uint64_t cap = shr__ctx_desc(ctx)->image_bytes;
    return bytes > cap || used > cap - bytes;
}

static bool source_valid(const shr_image_source *s) {
    return s && s->pixels && s->width > 0 && s->height > 0 && (uint32_t)s->format <= SHR_IMAGE_SRC_GRAY_ALPHA88 &&
           s->stride >= (uint64_t)s->width * shr__src_bytes(s->format);
}

/* Every alpha of the `w` x `h` rows is 255. */
static bool rows_opaque(uint32_t f, const uint8_t *row, size_t stride, int32_t w, int32_t h) {
    if (f == SHR_IMAGE_SRC_RGB888 || f == SHR_IMAGE_SRC_GRAY8 || f == SHR__SRC_RGB565) return true;
    size_t bpp = shr__src_bytes(f), n = (size_t)w * bpp;
    for (int32_t y = 0; y < h; y++, row += stride) {
        if (f == SHR_IMAGE_SRC_RGBA8888) { /* the alpha bytes of the words ANDed */
            uint32_t all = ~0u, v;
            size_t x = 0;
            for (; x + 16 <= n; x += 16) {
                uint32_t v1, v2, v3;
                memcpy(&v, row + x, 4), memcpy(&v1, row + x + 4, 4), memcpy(&v2, row + x + 8, 4);
                memcpy(&v3, row + x + 12, 4);
                all &= v & v1 & v2 & v3;
            }
            for (; x < n; x += 4) memcpy(&v, row + x, 4), all &= v;
            if (((const uint8_t *)&all)[3] != 255) return false;
            continue;
        }
        for (size_t x = 1; x < n; x += 2)
            if (row[x] != 255) return false;
    }
    return true;
}

static uint32_t q(uint32_t c, uint32_t m) { return (c * m + 127) / 255; } /* as drivers quantize */

/* Copies `rect` of source rows `src` of format `f` into `b`, converted; the caller validated both. */
static void img_put(shr__buf *b, shr_rect rect, shr_image_source_format f, const uint8_t *src, size_t stride) {
    bool to565 = b->mem.format == SHR_FORMAT_RGB565;
    size_t sb = shr__src_bytes(f), db = to565 ? 2 : 4, w = (size_t)(rect.x1 - rect.x0);
    uint8_t *dst = (uint8_t *)b->mem.pixels + (size_t)rect.y0 * b->mem.stride + (size_t)rect.x0 * db;
    for (int32_t y = rect.y0; y < rect.y1; y++, dst += b->mem.stride, src += stride) {
        if (!to565 && f == SHR_IMAGE_SRC_RGBA8888) {
            memcpy(dst, src, w * 4);
            continue;
        }
        const uint8_t *p = src;
        uint8_t *o = dst;
        for (size_t x = 0; x < w; x++, p += sb, o += db) {
            uint32_t r = p[0], g = sb >= 3 ? p[1] : r, bl = sb >= 3 ? p[2] : r;
            if (to565) {
                uint16_t v = (uint16_t)(q(r, 31) << 11 | q(g, 63) << 5 | q(bl, 31));
                memcpy(o, &v, 2);
            } else {
                o[0] = (uint8_t)r, o[1] = (uint8_t)g, o[2] = (uint8_t)bl;
                o[3] = sb == 2 ? p[1] : 255;
            }
        }
    }
}

/* Copies `rect` of `from` into `to`, buffers of one format. */
static void img_copy(shr__buf *to, const shr__buf *from, shr_rect rect) {
    size_t bpp = to->mem.format == SHR_FORMAT_RGB565 ? 2 : 4, row = (size_t)(rect.x1 - rect.x0) * bpp;
    size_t at = (size_t)rect.x0 * bpp;
    const uint8_t *src = (const uint8_t *)from->mem.pixels + (size_t)rect.y0 * from->mem.stride + at;
    uint8_t *dst = (uint8_t *)to->mem.pixels + (size_t)rect.y0 * to->mem.stride + at;
    for (int32_t y = rect.y0; y < rect.y1; y++, dst += to->mem.stride, src += from->mem.stride) memcpy(dst, src, row);
}

static shr_status img_resolve(shr__res *res, uint64_t id, uint64_t frame, const shr__resolved **out) {
    shr_pl_res_image *img = (shr_pl_res_image *)res;
    (void)id;
    if (frame != img->last_frame) img->last_frame = frame, img->pinned = &img->buf[img->cur];
    shr__buf *b = img->pinned;
    img->resolved = (shr__resolved){.buf = b, .rect = {0, 0, b->mem.width, b->mem.height}};
    *out = &img->resolved;
    return SHR_OK;
}

static void img_frame_end(shr__res *res, uint64_t frame) {
    (void)frame;
    ((shr_pl_res_image *)res)->pinned = NULL;
}

static void img_free(shr__res *res) {
    shr_pl_res_image *img = (shr_pl_res_image *)res;
    shr_context *ctx = res->ctx;
    const shr__alloc *al = shr__ctx_alloc(ctx);
    void **slot = shr__ctx_plugin_slot(ctx, &budget_kind);
    budget *b = *slot;
    if ((b->used -= img->bytes) == 0) {
        SHR_DELETE(al, b, budget);
        *slot = NULL;
    }
    shr__buf_free(ctx, &img->buf[0]);
    shr__buf_free(ctx, &img->buf[1]);
    SHR_DELETE(al, img, shr_pl_res_image);
}

static const shr__res_ops img_ops = {.resolve = img_resolve, .frame_end = img_frame_end, .free = img_free};

/* A width x height image of `f` within the budget, its pixels for the caller to write. */
static shr_status img_new(shr_context *ctx, shr_pixel_format f, int32_t width, int32_t height, shr_pl_res_image **out) {
    void **slot = shr__ctx_plugin_slot(ctx, &budget_kind);
    if (!slot) return SHR_E_LIMIT;
    budget *b = *slot;
    uint64_t used = b ? b->used : 0;
    if (over_budget(ctx, used, (uint64_t)width * (uint64_t)height * (f == SHR_FORMAT_RGB565 ? 2 : 4))) return SHR_E_LIMIT;

    const shr__alloc *al = shr__ctx_alloc(ctx);
    bool new_budget = !b;
    if (new_budget && !(b = SHR_NEW(al, budget))) return SHR_E_NO_MEMORY;
    shr_pl_res_image *img = SHR_NEW(al, shr_pl_res_image);
    shr_status st = img ? shr__buf_alloc(ctx, f, width, height, &img->buf[0]) : SHR_E_NO_MEMORY;
    if (st == SHR_OK && over_budget(ctx, used, img->buf[0].mem.byte_length)) { /* padded rows */
        shr__buf_free(ctx, &img->buf[0]);
        st = SHR_E_LIMIT;
    }
    if (st != SHR_OK) {
        SHR_DELETE(al, img, shr_pl_res_image);
        if (new_budget) SHR_DELETE(al, b, budget);
        return st;
    }
    img->bytes = img->buf[0].mem.byte_length;
    shr__res_attach(ctx, &img->res, &img_ops); /* cannot fail: the context accepted calls above */
    b->used = used + img->bytes;
    *slot = b;
    *out = img;
    return SHR_OK;
}

/* RGB565 for `px` opaque pixels where the screen and driver take it, the budget checked before `opaque` reads any. */
static shr_status img_format(shr_context *ctx, uint64_t px, bool (*opaque)(const void *), const void *arg,
                             shr_pixel_format *f) {
    void **slot = shr__ctx_plugin_slot(ctx, &budget_kind);
    budget *b = slot ? *slot : NULL;
    bool may565 = SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 && (shr__ctx_driver_flags(ctx) & SHR_DRIVER_IMAGE_565);
    if (over_budget(ctx, b ? b->used : 0, px * (may565 ? 2 : 4))) return SHR_E_LIMIT;
    *f = may565 && opaque(arg) ? SHR_FORMAT_RGB565 : SHR_FORMAT_RGBA8888;
    return SHR_OK;
}

static bool source_opaque(const void *arg) {
    const shr_image_source *s = arg;
    return rows_opaque(s->format, s->pixels, s->stride, s->width, s->height);
}

shr_status shr_pl_res_image_create_from(shr_context *ctx, const shr_image_source *source, shr_pl_res_image **out) {
    if (out) *out = NULL;
    if (!ctx || !out || !source_valid(source)) return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    shr_pixel_format f;
    shr_status st = img_format(ctx, (uint64_t)source->width * (uint64_t)source->height, source_opaque, source, &f);
    if (st == SHR_OK) st = img_new(ctx, f, source->width, source->height, out);
    if (st == SHR_OK)
        img_put(&(*out)->buf[0], (shr_rect){0, 0, source->width, source->height}, source->format, source->pixels,
                source->stride);
    return st;
}

shr_status shr_pl_res_image_create(shr_context *ctx, int32_t width, int32_t height, const void *rgba, size_t stride,
                                   shr_pl_res_image **out) {
    const shr_image_source s = {width, height, SHR_IMAGE_SRC_RGBA8888, rgba, stride};
    return shr_pl_res_image_create_from(ctx, &s, out);
}

shr_status shr_pl_res_image_budget(shr_context *ctx, uint64_t *used, uint64_t *limit) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if (shr__ctx_in_callback(ctx)) return SHR_E_STATE;
    void **slot = shr__ctx_plugin_slot(ctx, &budget_kind);
    budget *b = slot ? *slot : NULL;
    if (used) *used = b ? b->used : 0;
    if (limit) *limit = shr__ctx_desc(ctx)->image_bytes;
    return SHR_OK;
}

static void view_changed(shr_pl_res_image *v, shr_rect area);

/* Makes the other buffer current, brought level with the current one. */
static shr_status img_swap(shr_pl_res_image *img) {
    shr_context *ctx = img->res.ctx;
    shr__buf *from = &img->buf[img->cur], *to = &img->buf[!img->cur];
    if (!to->mem.pixels) {
        budget *b = img_budget(ctx);
        if (over_budget(ctx, b->used, from->mem.byte_length)) return SHR_E_LIMIT;
        shr_status st = shr__buf_alloc(ctx, from->mem.format, from->mem.width, from->mem.height, to);
        if (st != SHR_OK) return st;
        b->used += to->mem.byte_length, img->bytes += to->mem.byte_length;
        img->behind = (shr_rect){0, 0, from->mem.width, from->mem.height};
    }
    shr_rect r = img->behind;
    img_copy(to, from, r);
    shr__buf_changed(to, r);
    img->behind = (shr_rect){0, 0, 0, 0};
    img->cur = !img->cur;
    return SHR_OK;
}

shr_status shr_pl_res_image_update(shr_pl_res_image *img, shr_rect rect, const void *rgba, size_t stride) {
    shr__buf *b = img ? &img->buf[img->cur] : NULL;
    if (!img || img->res.dead || img->base || !shr__rect_valid(rect) || rect.x0 < 0 || rect.y0 < 0 ||
        rect.x1 > b->mem.width || rect.y1 > b->mem.height)
        return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(img->res.ctx)) return SHR_E_STATE;
    if (shr__rect_empty(rect)) return SHR_OK;
    if (!rgba || stride < (size_t)(rect.x1 - rect.x0) * 4) return SHR_E_INVALID_ARG;
    if (b->mem.format == SHR_FORMAT_RGB565 &&
        !rows_opaque(SHR_IMAGE_SRC_RGBA8888, rgba, stride, rect.x1 - rect.x0, rect.y1 - rect.y0))
        return SHR_E_UNSUPPORTED;
    shr_status st = img->pinned == b ? img_swap(img) : SHR_OK;
    if (st != SHR_OK) return st;
    b = &img->buf[img->cur];
    img_put(b, rect, SHR_IMAGE_SRC_RGBA8888, rgba, stride);
    img->behind = shr__rect_union(img->behind, rect);
    shr__buf_changed(b, rect);
    shr__res_changed(&img->res, rect);
    for (shr_pl_res_image *v = img->views; v; v = v->next) view_changed(v, rect);
    return SHR_OK;
}

shr_status shr_pl_res_image_release(shr_pl_res_image *img) {
    if (!img || img->res.dead) return SHR_E_INVALID_ARG;
    /* Allowed during shutdown, which waits for the resource to go. */
    if (shr__ctx_in_callback(img->res.ctx)) return SHR_E_STATE;
    img->res.dead = true;
    if (!img->res.users) shr__res_collect(img->res.ctx); /* at once, unless a frame still reads it */
    return SHR_OK;
}

shr_status shr_lyr_cmd_image(shr_lyr *layer, shr_pl_res_image *img, shr_rect src, shr_point at) {
    const shr_image *m = img ? &img->buf[img->cur].mem : NULL;
    int32_t w = !img ? 0 : img->base ? img->w : m->width, h = !img ? 0 : img->base ? img->h : m->height;
    if (!layer || !img || img->res.dead || shr__lyr_ctx(layer) != img->res.ctx || !shr__rect_valid(src) ||
        src.x0 < 0 || src.y0 < 0 || src.x1 > w || src.y1 > h)
        return SHR_E_INVALID_ARG;
    int64_t x1 = (int64_t)at.x + (src.x1 - src.x0), y1 = (int64_t)at.y + (src.y1 - src.y0);
    int64_t ax = (int64_t)at.x - src.x0, ay = (int64_t)at.y - src.y0;
    if (!shr__fits_i32(x1) || !shr__fits_i32(y1) || !shr__fits_i32(ax) || !shr__fits_i32(ay))
        return SHR_E_INVALID_ARG;
    shr__lcmd cmd = {.kind = SHR__LCMD_IMAGE,
                     .dst = {at.x, at.y, (int32_t)x1, (int32_t)y1},
                     .anchor = {(int32_t)ax, (int32_t)ay},
                     .res = &img->res};
    return shr__lyr_cmd_add(layer, &cmd);
}

/* ===== Scaling ===== */

static shr_status view_resolve(shr__res *res, uint64_t id, uint64_t frame, const shr__resolved **out) {
    shr_pl_res_image *v = (shr_pl_res_image *)res;
    const shr__resolved *b;
    img_resolve(&v->base->res, id, frame, &b); /* cannot fail */
    v->resolved = (shr__resolved){.buf = b->buf, .rect = v->src, .scale_w = v->w, .scale_h = v->h};
    *out = &v->resolved;
    return SHR_OK;
}

static void view_frame_end(shr__res *res, uint64_t frame) {
    img_frame_end(&((shr_pl_res_image *)res)->base->res, frame);
}

static void view_free(shr__res *res) {
    shr_pl_res_image *v = (shr_pl_res_image *)res, **pp = &v->base->views;
    while (*pp != v) pp = &(*pp)->next;
    *pp = v->next;
    v->base->res.users--;
    SHR_DELETE(shr__ctx_alloc(res->ctx), v, shr_pl_res_image);
}

static const shr__res_ops view_ops = {.resolve = view_resolve, .frame_end = view_frame_end, .free = view_free};

static int64_t floor_div(int64_t a, int64_t b) { return a / b - (a % b < 0); }

/* Scaled indices [*i0, *i1) whose bilinear taps may read source columns [a0, a1) of `s` columns from s0 over d. */
static void map_axis(int32_t a0, int32_t a1, int32_t s0, int32_t s, int32_t d, int32_t *i0, int32_t *i1) {
    int64_t lo = floor_div(2 * (int64_t)d * (a0 - s0) - d - s, 2 * (int64_t)s);
    int64_t hi = floor_div(2 * (int64_t)d * (a1 - s0 + 1) - d - s, 2 * (int64_t)s) + 1;
    *i0 = (int32_t)(lo < 0 ? 0 : lo > d ? d : lo), *i1 = (int32_t)(hi < 0 ? 0 : hi > d ? d : hi);
}

/* `area` of the base changed: the view pixels reading it. */
static void view_changed(shr_pl_res_image *v, shr_rect area) {
    shr_rect r;
    map_axis(area.x0, area.x1, v->src.x0, v->src.x1 - v->src.x0, v->w, &r.x0, &r.x1);
    map_axis(area.y0, area.y1, v->src.y0, v->src.y1 - v->src.y0, v->h, &r.y0, &r.y1);
    if (!shr__rect_empty(r)) shr__res_changed(&v->res, r);
}

static bool scale_args(int32_t w, int32_t h, shr_rect src, int32_t width, int32_t height) {
    return shr__rect_valid(src) && !shr__rect_empty(src) && src.x0 >= 0 && src.y0 >= 0 && src.x1 <= w && src.y1 <= h &&
           src.x1 - src.x0 <= SHR__SCALE_MAX && src.y1 - src.y0 <= SHR__SCALE_MAX && width > 0 && height > 0 &&
           width <= SHR__SCALE_MAX && height <= SHR__SCALE_MAX;
}

/* The source pixels any filter reads for `s` (its `src` grown by a pixel, within the source) are opaque. */
static bool scale_opaque(const void *arg) {
    const shr__scale *s = arg;
    int32_t x0 = s->src.x0 > 0 ? s->src.x0 - 1 : 0, y0 = s->src.y0 > 0 ? s->src.y0 - 1 : 0;
    int32_t x1 = s->src.x1 < s->w ? s->src.x1 + 1 : s->w, y1 = s->src.y1 < s->h ? s->src.y1 + 1 : s->h;
    return rows_opaque(s->format, s->pixels + (size_t)y0 * s->stride + (size_t)x0 * shr__src_bytes(s->format),
                       s->stride, x1 - x0, y1 - y0);
}

/* A new image of the scaled pixels of `s`; opaque ones go into RGB565 a span of columns at a time. */
static shr_status scaled_new(shr_context *ctx, const shr__scale *s, shr_pl_res_image **out) {
    shr_pixel_format f;
    shr_status st = img_format(ctx, (uint64_t)s->dw * (uint64_t)s->dh, scale_opaque, s, &f);
    if (st == SHR_OK) st = img_new(ctx, f, s->dw, s->dh, out);
    if (st != SHR_OK) return st;
    shr__buf *b = &(*out)->buf[0];
    if (f == SHR_FORMAT_RGBA8888) {
        shr__scale_rows(s, (uint8_t *)b->mem.pixels, b->mem.stride);
        return SHR_OK;
    }
    uint32_t px[SHR__SCALE_SPAN];
    shr__bilin bl;
    for (int32_t x0 = 0; x0 < s->dw; x0 += SHR__SCALE_SPAN) {
        int32_t n = s->dw - x0 < SHR__SCALE_SPAN ? s->dw - x0 : SHR__SCALE_SPAN;
        if (s->filter == SHR_SCALE_BILINEAR) shr__bilin_start(&bl, s, x0, n, 0);
        for (int32_t y = 0; y < s->dh; y++) {
            if (s->filter == SHR_SCALE_BILINEAR) shr__bilin_next(&bl, px);
            else shr__scale_row(s, y, x0, n, (uint8_t *)px);
            img_put(b, (shr_rect){x0, y, x0 + n, y + 1}, SHR_IMAGE_SRC_RGBA8888, (const uint8_t *)px, 0);
        }
    }
    return SHR_OK;
}

shr_status shr_pl_res_image_create_scaled(shr_context *ctx, const shr_image_source *source, shr_rect src,
                                          int32_t width, int32_t height, uint32_t filter, shr_pl_res_image **out) {
    if (out) *out = NULL;
    if (!ctx || !out || !source_valid(source) || filter > SHR_SCALE_BOX ||
        !scale_args(source->width, source->height, src, width, height))
        return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    shr__scale s = {.pixels = source->pixels, .stride = source->stride, .w = source->width, .h = source->height,
                    .format = source->format, .src = src, .dw = width, .dh = height, .filter = filter};
    return scaled_new(ctx, &s, out);
}

shr_status shr_pl_res_image_view(shr_pl_res_image *image, shr_rect src, int32_t width, int32_t height, uint32_t flags,
                                 shr_pl_res_image **out) {
    if (out) *out = NULL;
    uint32_t filter = flags & ~(uint32_t)(SHR_SCALE_COPY | SHR_SCALE_DRIVER);
    if (!image || !out || image->res.dead || image->base || filter > SHR_SCALE_BOX ||
        ((flags & SHR_SCALE_COPY) && (flags & SHR_SCALE_DRIVER)))
        return SHR_E_INVALID_ARG;
    const shr_image *m = &image->buf[image->cur].mem;
    if (!scale_args(m->width, m->height, src, width, height)) return SHR_E_INVALID_ARG;
    shr_context *ctx = image->res.ctx;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    bool driver = (shr__ctx_driver_flags(ctx) & SHR_DRIVER_SCALE) && filter == SHR_SCALE_BILINEAR;
    if ((flags & SHR_SCALE_DRIVER) && !driver) return SHR_E_UNSUPPORTED;
    if ((flags & SHR_SCALE_COPY) || !driver) {
        shr__scale s = {.pixels = m->pixels, .stride = m->stride, .w = m->width, .h = m->height,
                        .format = shr__src_of(m->format), .src = src, .dw = width, .dh = height, .filter = filter};
        return scaled_new(ctx, &s, out);
    }
    shr_pl_res_image *v = SHR_NEW(shr__ctx_alloc(ctx), shr_pl_res_image);
    if (!v) return SHR_E_NO_MEMORY;
    v->base = image, v->src = src, v->w = width, v->h = height, v->next = image->views;
    shr__res_attach(ctx, &v->res, &view_ops); /* cannot fail: the context accepted calls above */
    image->views = v, image->res.users++;
    *out = v;
    return SHR_OK;
}
