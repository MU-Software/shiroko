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

/* Copies `rect` of `src` rows into `b`; the caller validated both. */
static void img_copy(shr__buf *b, shr_rect rect, const uint8_t *src, size_t stride) {
    size_t row = (size_t)(rect.x1 - rect.x0) * 4;
    uint8_t *dst = (uint8_t *)b->mem.pixels + (size_t)rect.y0 * b->mem.stride + (size_t)rect.x0 * 4;
    for (int32_t y = rect.y0; y < rect.y1; y++, dst += b->mem.stride, src += stride) memcpy(dst, src, row);
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

/* A width x height image within the budget, its pixels for the caller to write. */
static shr_status img_new(shr_context *ctx, int32_t width, int32_t height, shr_pl_res_image **out) {
    uint64_t row = (uint64_t)width * 4; /* below 2^64 with the height */
    void **slot = shr__ctx_plugin_slot(ctx, &budget_kind);
    if (!slot) return SHR_E_LIMIT;
    budget *b = *slot;
    uint64_t used = b ? b->used : 0;
    if (over_budget(ctx, used, row * (uint64_t)height)) return SHR_E_LIMIT;

    const shr__alloc *al = shr__ctx_alloc(ctx);
    bool new_budget = !b;
    if (new_budget && !(b = SHR_NEW(al, budget))) return SHR_E_NO_MEMORY;
    shr_pl_res_image *img = SHR_NEW(al, shr_pl_res_image);
    shr_status st = img ? shr__buf_alloc(ctx, SHR_FORMAT_RGBA8888, width, height, &img->buf[0]) : SHR_E_NO_MEMORY;
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

shr_status shr_pl_res_image_create(shr_context *ctx, int32_t width, int32_t height, const void *rgba, size_t stride,
                                   shr_pl_res_image **out) {
    if (out) *out = NULL;
    if (!ctx || !out || !rgba || width <= 0 || height <= 0 || stride < (uint64_t)width * 4) return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    shr_status st = img_new(ctx, width, height, out);
    if (st == SHR_OK) img_copy(&(*out)->buf[0], (shr_rect){0, 0, width, height}, rgba, stride);
    return st;
}

static void view_changed(shr_pl_res_image *v, shr_rect area);

/* Makes the other buffer current, brought level with the current one. */
static shr_status img_swap(shr_pl_res_image *img) {
    shr_context *ctx = img->res.ctx;
    shr__buf *from = &img->buf[img->cur], *to = &img->buf[!img->cur];
    if (!to->mem.pixels) {
        budget *b = img_budget(ctx);
        if (over_budget(ctx, b->used, from->mem.byte_length)) return SHR_E_LIMIT;
        shr_status st = shr__buf_alloc(ctx, SHR_FORMAT_RGBA8888, from->mem.width, from->mem.height, to);
        if (st != SHR_OK) return st;
        b->used += to->mem.byte_length, img->bytes += to->mem.byte_length;
        img->behind = (shr_rect){0, 0, from->mem.width, from->mem.height};
    }
    shr_rect r = img->behind;
    img_copy(to, r, (const uint8_t *)from->mem.pixels + (size_t)r.y0 * from->mem.stride + (size_t)r.x0 * 4,
             from->mem.stride);
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
    shr_status st = img->pinned == b ? img_swap(img) : SHR_OK;
    if (st != SHR_OK) return st;
    b = &img->buf[img->cur];
    img_copy(b, rect, rgba, stride);
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
    img->res.dead = true; /* the compositor frees it once no command or frame uses it */
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
    shr_status st = img_resolve(&v->base->res, id, frame, &b);
    if (st != SHR_OK) return st;
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

/* A new image of the scaled pixels of `s`. */
static shr_status scaled_new(shr_context *ctx, const shr__scale *s, shr_pl_res_image **out) {
    shr_status st = img_new(ctx, s->dw, s->dh, out);
    if (st != SHR_OK) return st;
    const shr_image *m = &(*out)->buf[0].mem;
    for (int32_t y = 0; y < s->dh; y++) shr__scale_row(s, y, 0, s->dw, (uint8_t *)m->pixels + (size_t)y * m->stride);
    return SHR_OK;
}

static bool source_valid(const shr_image_source *s) {
    return s && s->pixels && s->width > 0 && s->height > 0 && (uint32_t)s->format <= SHR_IMAGE_SRC_GRAY_ALPHA88 &&
           s->stride >= (uint64_t)s->width * shr__src_bytes(s->format);
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
                        .format = SHR_IMAGE_SRC_RGBA8888, .src = src, .dw = width, .dh = height, .filter = filter};
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
