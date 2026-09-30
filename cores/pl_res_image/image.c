#include "shr_compositor.h"

struct shr_pl_res_image {
    shr__res res;
    uint8_t *pixels;
    int32_t width, height;
    size_t bytes;
    uint64_t last_frame;
    uint32_t pins; /* frames reading the pixels; a frame resolves all its commands at once */
};

typedef struct budget {
    uint64_t used;
} budget;

static const char budget_kind;

static const shr__alloc *img_alloc(const shr_pl_res_image *img) { return shr__ctx_alloc(img->res.ctx); }

/* Copies `rect` of `src` rows into the image; the caller validated both. */
static void img_copy(shr_pl_res_image *img, shr_rect rect, const uint8_t *src, size_t stride) {
    size_t row = (size_t)(rect.x1 - rect.x0) * 4, pitch = (size_t)img->width * 4;
    uint8_t *dst = img->pixels + (size_t)rect.y0 * pitch + (size_t)rect.x0 * 4;
    for (int32_t y = rect.y0; y < rect.y1; y++, dst += pitch, src += stride) memcpy(dst, src, row);
}

static shr_status img_resolve(shr__res *res, uint64_t id, uint64_t frame, shr__resolved *out) {
    shr_pl_res_image *img = (shr_pl_res_image *)res;
    (void)id;
    if (frame != img->last_frame) img->last_frame = frame, img->pins++;
    *out = (shr__resolved){{img->pixels, img->width, img->height, (size_t)img->width * 4, img->bytes,
                            SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU},
                           {0, 0},
                           false};
    return SHR_OK;
}

static void img_frame_end(shr__res *res, uint64_t frame) {
    (void)frame;
    ((shr_pl_res_image *)res)->pins--;
}

static void img_free(shr__res *res) {
    shr_pl_res_image *img = (shr_pl_res_image *)res;
    const shr__alloc *al = img_alloc(img);
    void **slot = shr__ctx_plugin_slot(res->ctx, &budget_kind);
    budget *b = *slot;
    if ((b->used -= img->bytes) == 0) {
        SHR_DELETE(al, b, budget);
        *slot = NULL;
    }
    shr__free(al, img->pixels, img->bytes, 8, SHR_ALLOC_PAYLOAD);
    SHR_DELETE(al, img, shr_pl_res_image);
}

static const shr__res_ops img_ops = {.resolve = img_resolve, .frame_end = img_frame_end, .free = img_free};

shr_status shr_pl_res_image_create(shr_context *ctx, int32_t width, int32_t height, const void *rgba, size_t stride,
                                   shr_pl_res_image **out) {
    if (out) *out = NULL;
    if (!ctx || !out || !rgba || width <= 0 || height <= 0) return SHR_E_INVALID_ARG;
    uint64_t row = (uint64_t)width * 4, bytes = row * (uint64_t)height; /* below 2^64 */
    if (stride < row) return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    void **slot = shr__ctx_plugin_slot(ctx, &budget_kind);
    if (!slot) return SHR_E_LIMIT;
    budget *b = *slot;
    uint64_t used = b ? b->used : 0;
    if (bytes > shr__ctx_desc(ctx)->image_bytes || used > shr__ctx_desc(ctx)->image_bytes - bytes) return SHR_E_LIMIT;
#if SIZE_MAX < UINT64_MAX
    if (bytes > SIZE_MAX) return SHR_E_LIMIT;
#endif

    const shr__alloc *al = shr__ctx_alloc(ctx);
    bool new_budget = !b;
    if (new_budget && !(b = SHR_NEW(al, budget))) return SHR_E_NO_MEMORY;
    shr_pl_res_image *img = SHR_NEW(al, shr_pl_res_image);
    uint8_t *pixels = img ? shr__malloc(al, (size_t)bytes, 8, SHR_ALLOC_PAYLOAD) : NULL;
    if (!pixels) {
        SHR_DELETE(al, img, shr_pl_res_image);
        if (new_budget) SHR_DELETE(al, b, budget);
        return SHR_E_NO_MEMORY;
    }
    *img = (shr_pl_res_image){.pixels = pixels, .width = width, .height = height, .bytes = (size_t)bytes};
    img_copy(img, (shr_rect){0, 0, width, height}, rgba, stride);
    shr__res_attach(ctx, &img->res, &img_ops); /* cannot fail: the context accepted calls above */
    b->used = used + bytes;
    *slot = b;
    *out = img;
    return SHR_OK;
}

shr_status shr_pl_res_image_update(shr_pl_res_image *img, shr_rect rect, const void *rgba, size_t stride) {
    if (!img || img->res.dead || !shr__rect_valid(rect) || rect.x0 < 0 || rect.y0 < 0 || rect.x1 > img->width ||
        rect.y1 > img->height)
        return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(img->res.ctx)) return SHR_E_STATE;
    if (shr__rect_empty(rect)) return SHR_OK;
    if (!rgba || stride < (size_t)(rect.x1 - rect.x0) * 4) return SHR_E_INVALID_ARG;
    if (img->pins) return SHR_E_WOULD_BLOCK;
    img_copy(img, rect, rgba, stride);
    shr__res_changed(&img->res, rect);
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
    if (!layer || !img || img->res.dead || shr__lyr_ctx(layer) != img->res.ctx || !shr__rect_valid(src) ||
        src.x0 < 0 || src.y0 < 0 || src.x1 > img->width || src.y1 > img->height)
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
