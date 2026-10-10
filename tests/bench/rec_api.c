/* Call recordings: the rec_api.h wrappers record into rec_api_rec, rec_calls_play() replays. Records (payload u32
 * words unless noted; objects by id, 1.. in creation order; styles by index, defined once by a STYLE record):
 * CLOCK u64 ns; CREATE output flags; SCREEN; SUBMIT; PUMP; POLL; REDRAW; LYR_CREATE id z rect; LYR_RECT id rect;
 * LYR_Z id z; LYR_VISIBLE id v; LYR_DESTROY id; CMD_BEGIN id; CMD_FILL id rect color; CMD_IMAGE id image src x y;
 * CMD_COMMIT id; IMG_CREATE id w h + rows; IMG_UPDATE id rect + rows; IMG_RELEASE id; FONT_CREATE id locale[16];
 * FONT_DESTROY id; FONT_PRELOAD id pages name[32]; RESIZE layer font rows cols has-bg bg; STYLE index fg bg flags;
 * CELLS layer count + per cell row u16 col u16 style u16 span u8 length u8 UTF-8 (set_cell calls in a row);
 * TEXT layer row col style flags runs length + runs (start end style) + UTF-8; CLEAR layer row col rows cols style;
 * SCROLL layer top bottom n style; LINES layer row count + per line u16 col u16 cols u8 kind u8 shape u16 flags u32
 * colour; ROW layer row col cells styles scalars lines (UINT32_MAX: NULL) + u32 style per style, then per cell u32 text
 * u16 style (into the call's styles) u8 span u8 scalars, u32 code points and lines as in LINES; IMG_SCALED id format w h
 * src dst-w dst-h filter + rows (w x h: the source pixels around src a filter reads, src relative to them); IMG_VIEW id
 * image src w h flags. */
#define REC_API_IMPL
#include "rec_api.h"

#include "rec.h"

#include <stdlib.h>
#include <string.h>

enum { C_CLOCK = REC_CALL0, C_CREATE, C_SCREEN, C_SUBMIT, C_PUMP, C_POLL, C_REDRAW, C_LYR_CREATE, C_LYR_RECT, C_LYR_Z,
       C_LYR_VISIBLE, C_LYR_DESTROY, C_CMD_BEGIN, C_CMD_FILL, C_CMD_IMAGE, C_CMD_COMMIT, C_IMG_CREATE, C_IMG_UPDATE,
       C_IMG_RELEASE, C_FONT_CREATE, C_FONT_DESTROY, C_FONT_PRELOAD, C_RESIZE, C_STYLE, C_CELLS, C_TEXT, C_CLEAR,
       C_SCROLL, C_LINES, C_ROW, C_IMG_SCALED, C_IMG_VIEW, C_END };
enum { K_FRAME, K_API, K_SUBMIT, K_BUILD };
enum { A_ALL, A_CELLS, A_SCROLL, A_LAYER, A_IMAGE, A_OTHER };
/* The api column of each call decoded ahead (the rest: A_ALL). */
static const int8_t kinds[C_END - C_CLOCK] = {
    [C_CREATE - C_CLOCK] = A_OTHER,      [C_SCREEN - C_CLOCK] = A_OTHER,      [C_REDRAW - C_CLOCK] = A_OTHER,
    [C_LYR_CREATE - C_CLOCK] = A_LAYER,  [C_LYR_RECT - C_CLOCK] = A_LAYER,    [C_LYR_Z - C_CLOCK] = A_LAYER,
    [C_LYR_VISIBLE - C_CLOCK] = A_LAYER, [C_LYR_DESTROY - C_CLOCK] = A_LAYER, [C_CMD_BEGIN - C_CLOCK] = A_LAYER,
    [C_CMD_FILL - C_CLOCK] = A_LAYER,    [C_CMD_IMAGE - C_CLOCK] = A_LAYER,   [C_CMD_COMMIT - C_CLOCK] = A_LAYER,
    [C_IMG_CREATE - C_CLOCK] = A_IMAGE,  [C_IMG_UPDATE - C_CLOCK] = A_IMAGE,  [C_IMG_RELEASE - C_CLOCK] = A_IMAGE,
    [C_FONT_CREATE - C_CLOCK] = A_OTHER, [C_FONT_DESTROY - C_CLOCK] = A_OTHER, [C_FONT_PRELOAD - C_CLOCK] = A_OTHER,
    [C_RESIZE - C_CLOCK] = A_OTHER,      [C_CELLS - C_CLOCK] = A_CELLS,       [C_TEXT - C_CLOCK] = A_CELLS,
    [C_CLEAR - C_CLOCK] = A_CELLS,       [C_SCROLL - C_CLOCK] = A_SCROLL,     [C_LINES - C_CLOCK] = A_CELLS,
    [C_ROW - C_CLOCK] = A_CELLS,         [C_IMG_SCALED - C_CLOCK] = A_IMAGE,  [C_IMG_VIEW - C_CLOCK] = A_IMAGE,
};

const char *const rec_compositor_cols[REC_COLS] = {"frame", "api", "submit", "build"};
const char *const rec_api_cols[REC_COLS] = {"api", "cells", "scroll", "layer", "image", "other"};

rec_calls *rec_api_rec;

struct rec_style_slot {
    shr_text_style st;
    uint32_t index; /* + 1; 0: free */
};

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ===== Recording ===== */

static void fail(rec_calls *c, shr_status st) {
    if (c->w.st == SHR_OK) c->w.st = st;
    c->w.on = false;
}

/* The recorder while it records; first the clock the call runs at, when it changed. */
static rec_calls *recorder(void) {
    rec_calls *c = rec_api_rec;
    if (!c || !c->w.on) return NULL;
    uint64_t t = c->clock ? c->clock(c->clock_user) : c->now;
    uint8_t *p = t != c->now ? rec_put(&c->w, C_CLOCK, 8) : NULL;
    if (p) rec_put64(p, t);
    c->now = t;
    return c->w.on ? c : NULL;
}

static uint32_t obj_id(rec_calls *c, const void *o) {
    if (c->last && c->objs[c->last - 1] == o) return c->last;
    for (uint32_t i = c->nobjs; o && i--;)
        if (c->objs[i] == o) return c->last = i + 1;
    fail(c, SHR_E_STATE);
    return 0;
}

static uint32_t obj_add(rec_calls *c, const void *o) {
    if (c->nobjs == c->cap) {
        uint32_t cap = c->cap ? 2 * c->cap : 64;
        const void **n = realloc(c->objs, cap * sizeof(*n));
        if (!n) {
            fail(c, SHR_E_NO_MEMORY);
            return 0;
        }
        c->objs = n, c->cap = cap;
    }
    c->objs[c->nobjs++] = o;
    return c->nobjs;
}

static void obj_drop(rec_calls *c, uint32_t id) {
    if (id) c->objs[id - 1] = NULL, c->last = 0;
}

static uint32_t style_hash(shr_text_style st) {
    uint32_t h = st.fg * 0x9E3779B1u ^ st.bg * 0x85EBCA77u ^ st.flags * 0xC2B2AE3Du;
    return h ^ h >> 15;
}

/* The style's index, defined by a STYLE record when new. */
static uint32_t style_id(rec_calls *c, shr_text_style st) {
    if (2 * (c->nstyles + 1) > c->slots) {
        uint32_t slots = c->slots ? 2 * c->slots : 1024;
        rec_style_slot *n = slots <= 1u << 17 ? calloc(slots, sizeof(*n)) : NULL;
        if (!n) {
            fail(c, SHR_E_NO_MEMORY);
            return 0;
        }
        for (uint32_t i = 0; i < c->slots; i++) {
            uint32_t j = style_hash(c->styles[i].st) & (slots - 1);
            while (c->styles[i].index && n[j].index) j = (j + 1) & (slots - 1);
            if (c->styles[i].index) n[j] = c->styles[i];
        }
        free(c->styles);
        c->styles = n, c->slots = slots;
    }
    uint32_t j = style_hash(st) & (c->slots - 1);
    for (; c->styles[j].index; j = (j + 1) & (c->slots - 1))
        if (!memcmp(&c->styles[j].st, &st, sizeof(st))) return c->styles[j].index - 1;
    c->styles[j] = (rec_style_slot){st, ++c->nstyles};
    uint8_t *p = rec_put(&c->w, C_STYLE, 16);
    if (p) rec_put32(p, c->nstyles - 1), rec_put32(p + 4, st.fg), rec_put32(p + 8, st.bg), rec_put32(p + 12, st.flags);
    return c->nstyles - 1;
}

/* A record of `type` with `words` u32 words from `v` and room for `extra` bytes after them (returned). */
static uint8_t *call(rec_calls *c, unsigned type, const uint32_t *v, size_t words, size_t extra) {
    uint8_t *p = rec_put(&c->w, type, 4 * words + extra);
    for (size_t i = 0; p && i < words; i++) rec_put32(p + 4 * i, v[i]);
    c->w.cmds += p != NULL;
    return p ? p + 4 * words : NULL;
}

shr_status rec_calls_begin(rec_calls *c, const rec_profile *p, const shr_driver_caps *caps, const shr_surface *targets,
                           uint32_t ntargets) {
    rec_writer w = c->w;
    *c = (rec_calls){.w = w};
    shr_status st = rec_start(&c->w, p, caps, REC_KIND_CALLS, targets, ntargets);
    rec_api_rec = st == SHR_OK ? c : NULL;
    return st;
}

shr_status rec_calls_end(rec_calls *c) {
    rec_api_rec = NULL;
    c->w.mems = c->nobjs, c->w.max_batch = c->nstyles;
    return rec_end(&c->w);
}

void rec_calls_free(rec_calls *c) {
    if (rec_api_rec == c) rec_api_rec = NULL;
    rec_free(&c->w);
    free(c->objs), free(c->styles);
    c->objs = NULL, c->styles = NULL, c->nobjs = c->cap = c->nstyles = c->slots = 0;
}

/* ===== Wrappers ===== */

shr_status rec_shr_create(const shr_context_desc *desc, shr_context **out_ctx) {
    rec_calls *c = rec_api_rec && rec_api_rec->w.on ? rec_api_rec : NULL;
    if (c && desc && desc->now_ns) c->clock = desc->now_ns, c->clock_user = desc->user, c->now = ~c->now;
    c = recorder();
    shr_status st = shr_create(desc, out_ctx);
    if (c && st == SHR_OK) call(c, C_CREATE, (const uint32_t[]){desc->output ? desc->output->flags : 0}, 1, 0);
    return st;
}

shr_status rec_shr_screen_configure(shr_context *ctx, const shr_screen_desc *desc) {
    rec_calls *c = recorder();
    shr_status st = shr_screen_configure(ctx, desc);
    if (c) call(c, C_SCREEN, NULL, 0, 0);
    return st;
}

shr_status rec_shr_submit(shr_context *ctx) {
    rec_calls *c = recorder();
    shr_status st = shr_submit(ctx);
    if (c) call(c, C_SUBMIT, NULL, 0, 0);
    return st;
}

shr_status rec_shr_pump(shr_context *ctx) {
    rec_calls *c = recorder();
    shr_status st = shr_pump(ctx);
    if (c) call(c, C_PUMP, NULL, 0, 0);
    return st;
}

shr_status rec_shr_poll_event(shr_context *ctx, shr_event *out) {
    rec_calls *c = recorder();
    shr_status st = shr_poll_event(ctx, out);
    if (c) call(c, C_POLL, NULL, 0, 0);
    return st;
}

shr_status rec_shr_request_redraw(shr_context *ctx) {
    rec_calls *c = recorder();
    shr_status st = shr_request_redraw(ctx);
    if (c) call(c, C_REDRAW, NULL, 0, 0);
    return st;
}

shr_status rec_shr_lyr_create(shr_context *ctx, int32_t z, shr_rect rect, shr_lyr **out) {
    rec_calls *c = recorder();
    shr_status st = shr_lyr_create(ctx, z, rect, out);
    if (c && st == SHR_OK) {
        uint8_t *p = call(c, C_LYR_CREATE, (const uint32_t[]){obj_add(c, *out), (uint32_t)z}, 2, 16);
        if (p) rec_put_rect(p, rect);
    }
    return st;
}

static void layer_rect(rec_calls *c, unsigned type, const void *o, shr_rect r) {
    uint8_t *p = call(c, type, (const uint32_t[]){obj_id(c, o)}, 1, 16);
    if (p) rec_put_rect(p, r);
}

shr_status rec_shr_lyr_set_rect(shr_lyr *layer, shr_rect rect) {
    rec_calls *c = recorder();
    shr_status st = shr_lyr_set_rect(layer, rect);
    if (c) layer_rect(c, C_LYR_RECT, layer, rect);
    return st;
}

shr_status rec_shr_lyr_set_z(shr_lyr *layer, int32_t z) {
    rec_calls *c = recorder();
    shr_status st = shr_lyr_set_z(layer, z);
    if (c) call(c, C_LYR_Z, (const uint32_t[]){obj_id(c, layer), (uint32_t)z}, 2, 0);
    return st;
}

shr_status rec_shr_lyr_set_visible(shr_lyr *layer, bool visible) {
    rec_calls *c = recorder();
    shr_status st = shr_lyr_set_visible(layer, visible);
    if (c) call(c, C_LYR_VISIBLE, (const uint32_t[]){obj_id(c, layer), visible}, 2, 0);
    return st;
}

shr_status rec_shr_lyr_destroy(shr_lyr *layer) {
    rec_calls *c = recorder();
    uint32_t id = c ? obj_id(c, layer) : 0;
    shr_status st = shr_lyr_destroy(layer);
    if (c) call(c, C_LYR_DESTROY, &id, 1, 0), obj_drop(c, id);
    return st;
}

shr_status rec_shr_lyr_cmd_begin(shr_lyr *layer) {
    rec_calls *c = recorder();
    shr_status st = shr_lyr_cmd_begin(layer);
    if (c) call(c, C_CMD_BEGIN, (const uint32_t[]){obj_id(c, layer)}, 1, 0);
    return st;
}

shr_status rec_shr_lyr_cmd_fill(shr_lyr *layer, shr_rect rect, shr_color color) {
    rec_calls *c = recorder();
    shr_status st = shr_lyr_cmd_fill(layer, rect, color);
    uint8_t *p = c ? call(c, C_CMD_FILL, (const uint32_t[]){obj_id(c, layer)}, 1, 20) : NULL;
    if (p) rec_put_rect(p, rect), rec_put32(p + 16, color);
    return st;
}

shr_status rec_shr_lyr_cmd_image(shr_lyr *layer, shr_pl_res_image *image, shr_rect src, shr_point at) {
    rec_calls *c = recorder();
    shr_status st = shr_lyr_cmd_image(layer, image, src, at);
    uint8_t *p = c ? call(c, C_CMD_IMAGE, (const uint32_t[]){obj_id(c, layer), obj_id(c, image)}, 2, 24) : NULL;
    if (p) rec_put_rect(p, src), rec_put32(p + 16, (uint32_t)at.x), rec_put32(p + 20, (uint32_t)at.y);
    return st;
}

shr_status rec_shr_lyr_cmd_commit(shr_lyr *layer) {
    rec_calls *c = recorder();
    shr_status st = shr_lyr_cmd_commit(layer);
    if (c) call(c, C_CMD_COMMIT, (const uint32_t[]){obj_id(c, layer)}, 1, 0);
    return st;
}

static size_t src_bytes(uint32_t format) {
    return format == SHR_IMAGE_SRC_RGBA8888 ? 4 : format == SHR_IMAGE_SRC_RGB888 ? 3 : format - 1u;
}

/* Rows of `row` bytes from rgba, stride apart, packed after a record's words. */
static void rows_put(uint8_t *p, const void *rgba, size_t stride, size_t row, int32_t rows) {
    for (int32_t y = 0; p && y < rows; y++) memcpy(p + (size_t)y * row, (const uint8_t *)rgba + (size_t)y * stride, row);
}

shr_status rec_shr_pl_res_image_create(shr_context *ctx, int32_t width, int32_t height, const void *rgba, size_t stride,
                                       shr_pl_res_image **out) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_res_image_create(ctx, width, height, rgba, stride, out);
    if (c && st == SHR_OK) {
        uint32_t id = obj_add(c, *out);
        size_t row = (size_t)width * 4;
        rows_put(call(c, C_IMG_CREATE, (const uint32_t[]){id, (uint32_t)width, (uint32_t)height}, 3, row * (size_t)height),
                 rgba, stride, row, height);
    }
    return st;
}

shr_status rec_shr_pl_res_image_update(shr_pl_res_image *image, shr_rect rect, const void *rgba, size_t stride) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_res_image_update(image, rect, rgba, stride);
    if (c && st == SHR_OK) {
        size_t row = (size_t)(rect.x1 - rect.x0) * 4;
        uint8_t *p = call(c, C_IMG_UPDATE, (const uint32_t[]){obj_id(c, image)}, 1, 16 + row * (size_t)(rect.y1 - rect.y0));
        if (p) rec_put_rect(p, rect), rows_put(p + 16, rgba, stride, row, rect.y1 - rect.y0);
    }
    return st;
}

shr_status rec_shr_pl_res_image_release(shr_pl_res_image *image) {
    rec_calls *c = recorder();
    uint32_t id = c ? obj_id(c, image) : 0;
    shr_status st = shr_pl_res_image_release(image);
    if (c) call(c, C_IMG_RELEASE, &id, 1, 0), obj_drop(c, id);
    return st;
}

shr_status rec_shr_pl_res_image_create_scaled(shr_context *ctx, const shr_image_source *source, shr_rect src,
                                              int32_t width, int32_t height, uint32_t filter, shr_pl_res_image **out) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_res_image_create_scaled(ctx, source, src, width, height, filter, out);
    if (c && st == SHR_OK) { /* bilinear taps reach one pixel beyond src */
        shr_rect k = {src.x0 > 0 ? src.x0 - 1 : 0, src.y0 > 0 ? src.y0 - 1 : 0,
                      src.x1 < source->width ? src.x1 + 1 : source->width,
                      src.y1 < source->height ? src.y1 + 1 : source->height};
        size_t bpp = src_bytes(source->format), row = (size_t)(k.x1 - k.x0) * bpp;
        const uint32_t v[11] = {obj_add(c, *out), source->format, (uint32_t)(k.x1 - k.x0), (uint32_t)(k.y1 - k.y0),
                                (uint32_t)(src.x0 - k.x0), (uint32_t)(src.y0 - k.y0), (uint32_t)(src.x1 - k.x0),
                                (uint32_t)(src.y1 - k.y0), (uint32_t)width, (uint32_t)height, filter};
        rows_put(call(c, C_IMG_SCALED, v, 11, row * (size_t)(k.y1 - k.y0)),
                 (const uint8_t *)source->pixels + (size_t)k.y0 * source->stride + (size_t)k.x0 * bpp, source->stride, row,
                 k.y1 - k.y0);
    }
    return st;
}

shr_status rec_shr_pl_res_image_view(shr_pl_res_image *image, shr_rect src, int32_t width, int32_t height,
                                     uint32_t flags, shr_pl_res_image **out) {
    rec_calls *c = recorder();
    uint32_t base = c ? obj_id(c, image) : 0;
    shr_status st = shr_pl_res_image_view(image, src, width, height, flags, out);
    if (c && st == SHR_OK)
        call(c, C_IMG_VIEW,
             (const uint32_t[]){obj_add(c, *out), base, (uint32_t)src.x0, (uint32_t)src.y0, (uint32_t)src.x1,
                                (uint32_t)src.y1, (uint32_t)width, (uint32_t)height, flags},
             9, 0);
    return st;
}

shr_status rec_shr_pl_res_bitmap_font_create(shr_context *ctx, const shr_pl_res_bitmap_font_desc *desc,
                                             shr_pl_res_bitmap_font **out) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_res_bitmap_font_create(ctx, desc, out);
    if (c && st == SHR_OK) {
        uint8_t *p = call(c, C_FONT_CREATE, (const uint32_t[]){obj_add(c, *out)}, 1, 16);
        if (p && desc->locale) strncpy((char *)p, desc->locale, 15);
    }
    return st;
}

shr_status rec_shr_pl_res_bitmap_font_destroy(shr_pl_res_bitmap_font *font) {
    rec_calls *c = recorder();
    uint32_t id = c ? obj_id(c, font) : 0;
    shr_status st = shr_pl_res_bitmap_font_destroy(font);
    if (c) call(c, C_FONT_DESTROY, &id, 1, 0);
    if (c && st == SHR_OK) obj_drop(c, id);
    return st;
}

shr_status rec_shr_pl_res_bitmap_font_preload(shr_pl_res_bitmap_font *font, const char *package, uint32_t pages) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_res_bitmap_font_preload(font, package, pages);
    uint8_t *p = c ? call(c, C_FONT_PRELOAD, (const uint32_t[]){obj_id(c, font), pages}, 2, 32) : NULL;
    if (p && package) strncpy((char *)p, package, 31);
    return st;
}

shr_status rec_shr_pl_lyr_tilemap_resize(shr_lyr *layer, shr_pl_res_bitmap_font *font, int32_t rows, int32_t cols,
                                         const shr_color *background) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_lyr_tilemap_resize(layer, font, rows, cols, background);
    if (c)
        call(c, C_RESIZE,
             (const uint32_t[]){obj_id(c, layer), font ? obj_id(c, font) : 0, (uint32_t)rows, (uint32_t)cols,
                                background != NULL, background ? *background : 0},
             6, 0);
    return st;
}

shr_status rec_shr_pl_lyr_tilemap_set_cell(shr_lyr *layer, int32_t row, int32_t col, const char *utf8, size_t length,
                                           uint32_t span, shr_text_style style) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_lyr_tilemap_set_cell(layer, row, col, utf8, length, span, style);
    if (!c) return st;
    uint32_t id = obj_id(c, layer), s = style_id(c, style);
    if ((uint32_t)row > 0xFFFF || (uint32_t)col > 0xFFFF || length > 255 || span > 255 || (length && !utf8)) {
        fail(c, SHR_E_UNSUPPORTED);
        return st;
    }
    uint8_t *p = id == c->cells_layer ? rec_grow(&c->w, c->cells_at, 8 + length) : NULL;
    if (p)
        rec_put32(c->w.buf + c->cells_at + 8, rec_get32(c->w.buf + c->cells_at + 8) + 1), c->w.cmds++;
    else if ((p = call(c, C_CELLS, (const uint32_t[]){id, 1}, 2, 8 + length)))
        c->cells_at = (size_t)(p - 12 - c->w.buf), c->cells_layer = id;
    if (!p) return st;
    p[0] = (uint8_t)row, p[1] = (uint8_t)(row >> 8), p[2] = (uint8_t)col, p[3] = (uint8_t)(col >> 8);
    p[4] = (uint8_t)s, p[5] = (uint8_t)(s >> 8), p[6] = (uint8_t)span, p[7] = (uint8_t)length;
    if (length) memcpy(p + 8, utf8, length);
    return st;
}

shr_status rec_shr_pl_lyr_tilemap_set_text(shr_lyr *layer, int32_t row, int32_t col, const char *utf8, size_t length,
                                           shr_text_style style, const shr_style_run *runs, size_t run_count,
                                           uint32_t flags, shr_error_info *err) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_lyr_tilemap_set_text(layer, row, col, utf8, length, style, runs, run_count, flags, err);
    if (!c) return st;
    uint32_t id = obj_id(c, layer), s = style_id(c, style), n = runs ? (uint32_t)run_count : 0;
    uint32_t *rs = n ? malloc(3 * sizeof(uint32_t) * n) : NULL;
    for (uint32_t i = 0; rs && i < n; i++)
        rs[3 * i] = (uint32_t)runs[i].byte_start, rs[3 * i + 1] = (uint32_t)runs[i].byte_end,
        rs[3 * i + 2] = style_id(c, runs[i].style);
    if (n && !rs) fail(c, SHR_E_NO_MEMORY);
    uint8_t *p = call(c, C_TEXT, (const uint32_t[]){id, (uint32_t)row, (uint32_t)col, s, flags, n, (uint32_t)length}, 7,
                      12 * (size_t)n + length);
    for (uint32_t i = 0; p && i < 3 * n; i++) rec_put32(p + 4 * i, rs[i]);
    if (p && length) memcpy(p + 12 * (size_t)n, utf8, length);
    free(rs);
    return st;
}

shr_status rec_shr_pl_lyr_tilemap_clear(shr_lyr *layer, int32_t row, int32_t col, int32_t rows, int32_t cols,
                                        shr_text_style style) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_lyr_tilemap_clear(layer, row, col, rows, cols, style);
    if (c)
        call(c, C_CLEAR,
             (const uint32_t[]){obj_id(c, layer), (uint32_t)row, (uint32_t)col, (uint32_t)rows, (uint32_t)cols,
                                style_id(c, style)},
             6, 0);
    return st;
}

shr_status rec_shr_pl_lyr_tilemap_scroll(shr_lyr *layer, int32_t top, int32_t bottom, int32_t n, shr_text_style style) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_lyr_tilemap_scroll(layer, top, bottom, n, style);
    if (c)
        call(c, C_SCROLL,
             (const uint32_t[]){obj_id(c, layer), (uint32_t)top, (uint32_t)bottom, (uint32_t)n, style_id(c, style)}, 5, 0);
    return st;
}

static void put_lines(uint8_t *p, const shr_text_line *lines, size_t count) {
    for (size_t i = 0; i < count; i++, p += 12) {
        const shr_text_line *e = &lines[i];
        p[0] = (uint8_t)e->col, p[1] = (uint8_t)(e->col >> 8), p[2] = (uint8_t)e->cols, p[3] = (uint8_t)(e->cols >> 8);
        p[4] = e->kind, p[5] = e->shape, p[6] = (uint8_t)e->flags, p[7] = (uint8_t)(e->flags >> 8);
        rec_put32(p + 8, e->color);
    }
}

shr_status rec_shr_pl_lyr_tilemap_set_lines(shr_lyr *layer, int32_t row, const shr_text_line *lines, size_t count,
                                            shr_error_info *err) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_lyr_tilemap_set_lines(layer, row, lines, count, err);
    uint8_t *p = c && (count == 0 || lines)
                     ? call(c, C_LINES, (const uint32_t[]){obj_id(c, layer), (uint32_t)row, (uint32_t)count}, 3, 12 * count)
                     : NULL;
    if (p) put_lines(p, lines, count);
    return st;
}

shr_status rec_shr_pl_lyr_tilemap_set_row(shr_lyr *layer, int32_t row, int32_t col, const shr_row *in,
                                          shr_error_info *err) {
    rec_calls *c = recorder();
    shr_status st = shr_pl_lyr_tilemap_set_row(layer, row, col, in, err);
    if (!c || !in || (in->cell_count && !in->cells) || (in->style_count && !in->styles) ||
        (in->scalar_count && !in->scalars))
        return st;
    uint32_t id = obj_id(c, layer), nl = in->lines ? (uint32_t)in->line_count : UINT32_MAX;
    uint32_t *ids = in->style_count ? malloc(in->style_count * sizeof(uint32_t)) : NULL;
    if (in->style_count && !ids) fail(c, SHR_E_NO_MEMORY);
    for (size_t i = 0; ids && i < in->style_count; i++) ids[i] = style_id(c, in->styles[i]);
    uint8_t *p = call(c, C_ROW,
                      (const uint32_t[]){id, (uint32_t)row, (uint32_t)col, (uint32_t)in->cell_count,
                                         (uint32_t)in->style_count, (uint32_t)in->scalar_count, nl},
                      7, 4 * (in->style_count + in->scalar_count) + 8 * in->cell_count + 12 * (size_t)(in->lines ? nl : 0));
    for (size_t i = 0; p && i < in->style_count; i++, p += 4) rec_put32(p, ids ? ids[i] : 0);
    for (size_t i = 0; p && i < in->cell_count; i++, p += 8) {
        const shr_row_cell *e = &in->cells[i];
        rec_put32(p, e->text);
        p[4] = (uint8_t)e->style, p[5] = (uint8_t)(e->style >> 8), p[6] = e->span, p[7] = e->scalars;
    }
    for (size_t i = 0; p && i < in->scalar_count; i++, p += 4) rec_put32(p, in->scalars[i]);
    if (p && in->lines) put_lines(p, in->lines, nl);
    free(ids);
    return st;
}

/* ===== Replay ===== */

/* A call decoded ahead of its timed run; objects by id, as a call may use one created earlier in its run. */
typedef struct pcall {
    uint32_t type, n, a, b; /* n: bytes at data */
    int32_t i[6];
    shr_text_style st;
    const uint8_t *data;
} pcall;

typedef struct player {
    const rec_calls_host *h;
    shr_framebuffer_driver drv; /* draws nothing */
    shr_output output;
    shr_context *ctx;
    shr_screen_desc sd;
    shr_context_desc cd;
    uint64_t clock;
    void **objs;
    uint8_t *types; /* by id: the record that created it */
    shr_text_style *styles;
    uint32_t nobjs, nstyles, cmds;
    shr_style_run *runs;
    shr_text_line *lines;
    shr_row_cell *cells;
    shr_text_style *row_styles;
    uint32_t *scalars;
    pcall *run;
    uint32_t nrun, cap;
    int col;
} player;

static uint64_t play_clock(void *user) { return ((player *)user)->clock; }

static shr_status null_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t n, shr_fence f) {
    (void)dst, (void)cmds, (void)f;
    ((player *)user)->cmds += (uint32_t)n;
    return SHR_OK;
}

static void null_cancel(void *user, shr_fence f) {
    const shr_framebuffer_driver *d = ((player *)user)->h->drv;
    d->cancel(d->user, f);
}

static shr_status null_reset(void *user) {
    const shr_framebuffer_driver *d = ((player *)user)->h->drv;
    return d->reset(d->user);
}

static void null_sync(void *user, const void *addr, size_t bytes) {
    const shr_framebuffer_driver *d = ((player *)user)->h->drv;
    d->sync(d->user, addr, bytes);
}

static shr_status out_acquire(void *user, shr_surface *s) {
    *s = ((player *)user)->h->targets[0];
    return SHR_OK;
}

static shr_status out_present(void *user, const shr_surface *s, uint64_t id) {
    (void)user, (void)s, (void)id;
    return SHR_OK;
}

static void out_discard(void *user, const shr_surface *s) { (void)user, (void)s; }

static bool caps_equal(const shr_driver_caps *a, const shr_driver_caps *b) {
    return a->domains == b->domains && a->address_align == b->address_align && a->stride_align == b->stride_align &&
           a->max_width == b->max_width && a->max_height == b->max_height && a->timeout_ns == b->timeout_ns &&
           a->max_buffers == b->max_buffers && a->max_buffer_width == b->max_buffer_width &&
           a->max_buffer_height == b->max_buffer_height && a->buffer_bytes == b->buffer_bytes &&
           a->buffer_flags == b->buffer_flags && a->max_keeps == b->max_keeps && a->keep_bytes == b->keep_bytes &&
           a->max_keep_bytes == b->max_keep_bytes && a->flags == b->flags;
}

static void *obj(const player *p, uint32_t id) { return id && id <= p->nobjs ? p->objs[id - 1] : NULL; }

static shr_text_style style(const player *p, uint32_t s) { return s < p->nstyles ? p->styles[s] : (shr_text_style){0}; }

static void exec(player *p, const pcall *c) {
    switch (c->type) {
    case C_CREATE:
        p->output.flags = (uint32_t)c->i[0];
        shr_create(&p->cd, &p->ctx);
        break;
    case C_SCREEN: shr_screen_configure(p->ctx, &p->sd); break;
    case C_REDRAW: shr_request_redraw(p->ctx); break;
    case C_LYR_CREATE:
        shr_lyr_create(p->ctx, c->i[4], (shr_rect){c->i[0], c->i[1], c->i[2], c->i[3]}, (shr_lyr **)&p->objs[c->a - 1]);
        break;
    case C_LYR_RECT: shr_lyr_set_rect(obj(p, c->a), (shr_rect){c->i[0], c->i[1], c->i[2], c->i[3]}); break;
    case C_LYR_Z: shr_lyr_set_z(obj(p, c->a), c->i[0]); break;
    case C_LYR_VISIBLE: shr_lyr_set_visible(obj(p, c->a), c->i[0] != 0); break;
    case C_LYR_DESTROY: shr_lyr_destroy(obj(p, c->a)), p->objs[c->a - 1] = NULL; break;
    case C_CMD_BEGIN: shr_lyr_cmd_begin(obj(p, c->a)); break;
    case C_CMD_FILL: shr_lyr_cmd_fill(obj(p, c->a), (shr_rect){c->i[0], c->i[1], c->i[2], c->i[3]}, (shr_color)c->i[4]); break;
    case C_CMD_IMAGE:
        shr_lyr_cmd_image(obj(p, c->a), obj(p, c->b), (shr_rect){c->i[0], c->i[1], c->i[2], c->i[3]}, (shr_point){c->i[4], c->i[5]});
        break;
    case C_CMD_COMMIT: shr_lyr_cmd_commit(obj(p, c->a)); break;
    case C_IMG_CREATE:
        shr_pl_res_image_create(p->ctx, c->i[0], c->i[1], c->data, (size_t)c->i[0] * 4,
                                (shr_pl_res_image **)&p->objs[c->a - 1]);
        break;
    case C_IMG_UPDATE:
        shr_pl_res_image_update(obj(p, c->a), (shr_rect){c->i[0], c->i[1], c->i[2], c->i[3]}, c->data,
                                (size_t)(c->i[2] - c->i[0]) * 4);
        break;
    case C_IMG_RELEASE: shr_pl_res_image_release(obj(p, c->a)), p->objs[c->a - 1] = NULL; break;
    case C_IMG_SCALED: {
        uint32_t v[11];
        for (int i = 0; i < 11; i++) v[i] = rec_get32(c->data + 4 * i);
        shr_image_source src = {(int32_t)v[2], (int32_t)v[3], (shr_image_source_format)v[1], c->data + 44,
                                (size_t)v[2] * src_bytes(v[1])};
        shr_pl_res_image_create_scaled(p->ctx, &src, (shr_rect){(int32_t)v[4], (int32_t)v[5], (int32_t)v[6], (int32_t)v[7]},
                                       (int32_t)v[8], (int32_t)v[9], v[10], (shr_pl_res_image **)&p->objs[c->a - 1]);
        break;
    }
    case C_IMG_VIEW:
        shr_pl_res_image_view(obj(p, c->b), (shr_rect){c->i[0], c->i[1], c->i[2], c->i[3]}, c->i[4], c->i[5],
                              (uint32_t)c->n, (shr_pl_res_image **)&p->objs[c->a - 1]);
        break;
    case C_FONT_CREATE: {
        shr_pl_res_bitmap_font_desc fd;
        shr_pl_res_bitmap_font_desc_init(&fd);
        fd.open = p->h->open, fd.user = p->h->user, fd.locale = (const char *)c->data;
        shr_pl_res_bitmap_font_create(p->ctx, &fd, (shr_pl_res_bitmap_font **)&p->objs[c->a - 1]);
        break;
    }
    case C_FONT_DESTROY:
        if (shr_pl_res_bitmap_font_destroy(obj(p, c->a)) == SHR_OK) p->objs[c->a - 1] = NULL;
        break;
    case C_FONT_PRELOAD: shr_pl_res_bitmap_font_preload(obj(p, c->a), (const char *)c->data, (uint32_t)c->i[0]); break;
    case C_RESIZE:
        shr_pl_lyr_tilemap_resize(obj(p, c->a), obj(p, c->b), c->i[0], c->i[1], c->i[2] ? &(shr_color){(shr_color)c->i[3]} : NULL);
        break;
    case C_CELLS:
        shr_pl_lyr_tilemap_set_cell(obj(p, c->a), c->i[0], c->i[1], (const char *)c->data, c->n, (uint32_t)c->i[2], c->st);
        break;
    case C_TEXT:
        shr_pl_lyr_tilemap_set_text(obj(p, c->a), c->i[0], c->i[1], (const char *)c->data, c->n, c->st, c->i[3] ? p->runs : NULL,
                                    (size_t)c->i[3], (uint32_t)c->i[2], NULL);
        break;
    case C_CLEAR: shr_pl_lyr_tilemap_clear(obj(p, c->a), c->i[0], c->i[1], c->i[2], c->i[3], c->st); break;
    case C_SCROLL: shr_pl_lyr_tilemap_scroll(obj(p, c->a), c->i[0], c->i[1], c->i[2], c->st); break;
    case C_LINES: shr_pl_lyr_tilemap_set_lines(obj(p, c->a), c->i[0], p->lines, c->n, NULL); break;
    case C_ROW: {
        const shr_text_line *ls = c->i[5] < 0 ? NULL : p->lines ? p->lines : &(const shr_text_line){0}; /* none: empty */
        shr_row in = {p->cells, (size_t)c->i[2], p->row_styles, (size_t)c->i[3], p->scalars, (size_t)c->i[4], ls,
                      ls ? (size_t)c->i[5] : 0};
        shr_pl_lyr_tilemap_set_row(obj(p, c->a), c->i[0], c->i[1], &in, NULL);
        break;
    }
    }
}


/* Runs the decoded calls, timed together. */
static void flush(player *p, rec_times *comp, rec_times *api) {
    if (!p->nrun) return;
    uint64_t t0 = p->h->now_ns(p->h->user);
    for (uint32_t i = 0; i < p->nrun; i++) exec(p, &p->run[i]);
    float dt = rec_ms(p->h->now_ns(p->h->user) - t0);
    comp->v[K_API] += dt, comp->v[K_FRAME] += dt, api->v[A_ALL] += dt, api->v[p->col] += dt;
    api->cmds += p->nrun;
    p->nrun = 0;
}

static pcall *slot(player *p, int col, rec_times *comp, rec_times *api) {
    if (p->nrun == p->cap || (p->nrun && col != p->col)) flush(p, comp, api);
    p->col = col;
    pcall *c = &p->run[p->nrun++];
    memset(c, 0, sizeof(*c));
    return c;
}

/* Decodes a call record into the run (CELLS into one call per cell); false when malformed. */
static bool decode(player *p, unsigned type, const uint8_t *r, size_t n, rec_times *comp, rec_times *api) {
    const uint8_t *end = r + n;
    if (type == C_CELLS) {
        if (n < 8) return false;
        uint32_t layer = rec_get32(r);
        uint32_t count = rec_get32(r + 4);
        for (r += 8; count--; r += 8 + r[7]) {
            if (end - r < 8 || end - r < 8 + r[7]) return false;
            pcall *c = slot(p, A_CELLS, comp, api);
            c->type = type, c->a = layer, c->i[0] = get16(r), c->i[1] = get16(r + 2), c->st = style(p, get16(r + 4));
            c->i[2] = r[6], c->n = r[7], c->data = r + 8;
        }
        return true;
    }
    static const uint8_t words[C_END - C_CLOCK] = {
        [C_CREATE - C_CLOCK] = 1,      [C_LYR_CREATE - C_CLOCK] = 6, [C_LYR_RECT - C_CLOCK] = 5,
        [C_LYR_Z - C_CLOCK] = 2,       [C_LYR_VISIBLE - C_CLOCK] = 2, [C_LYR_DESTROY - C_CLOCK] = 1,
        [C_CMD_BEGIN - C_CLOCK] = 1,   [C_CMD_FILL - C_CLOCK] = 6,   [C_CMD_IMAGE - C_CLOCK] = 8,
        [C_CMD_COMMIT - C_CLOCK] = 1,  [C_IMG_CREATE - C_CLOCK] = 3, [C_IMG_UPDATE - C_CLOCK] = 5,
        [C_IMG_RELEASE - C_CLOCK] = 1, [C_FONT_CREATE - C_CLOCK] = 5, [C_FONT_DESTROY - C_CLOCK] = 1,
        [C_FONT_PRELOAD - C_CLOCK] = 10, [C_RESIZE - C_CLOCK] = 6,   [C_TEXT - C_CLOCK] = 7,
        [C_CLEAR - C_CLOCK] = 6,       [C_SCROLL - C_CLOCK] = 5,     [C_LINES - C_CLOCK] = 3,
        [C_ROW - C_CLOCK] = 7,         [C_IMG_SCALED - C_CLOCK] = 11, [C_IMG_VIEW - C_CLOCK] = 9,
    };
    uint32_t v[11] = {0}, w = words[type - C_CLOCK];
    if (n < 4 * (size_t)w) return false;
    for (uint32_t i = 0; i < w; i++) v[i] = rec_get32(r + 4 * i);
    bool makes = type == C_LYR_CREATE || type == C_IMG_CREATE || type == C_FONT_CREATE || type == C_LYR_DESTROY ||
                 type == C_IMG_RELEASE || type == C_FONT_DESTROY || type == C_IMG_SCALED || type == C_IMG_VIEW;
    if (makes && (!v[0] || v[0] > p->nobjs)) return false;
    pcall *c = slot(p, kinds[type - C_CLOCK], comp, api);
    c->type = type, c->a = v[0];
    for (int i = 0; i < 6; i++) c->i[i] = (int32_t)v[1 + i];
    switch (type) {
    case C_CREATE: c->i[0] = (int32_t)v[0]; break;
    case C_LYR_CREATE:
        for (int i = 0; i < 4; i++) c->i[i] = (int32_t)v[2 + i];
        c->i[4] = (int32_t)v[1];
        break;
    case C_CMD_IMAGE:
        c->b = v[1];
        for (int i = 0; i < 6; i++) c->i[i] = (int32_t)v[2 + i];
        break;
    case C_IMG_CREATE:
        if ((uint64_t)v[1] * 4 * v[2] > (uint64_t)(n - 12)) return false;
        c->data = r + 12;
        break;
    case C_IMG_UPDATE:
        if ((uint64_t)(uint32_t)(c->i[2] - c->i[0]) * 4 * (uint32_t)(c->i[3] - c->i[1]) > (uint64_t)(n - 20)) return false;
        c->data = r + 20;
        break;
    case C_FONT_CREATE: c->data = r + 4; break;
    case C_IMG_SCALED:
        if (v[1] > SHR_IMAGE_SRC_GRAY_ALPHA88 || (uint64_t)v[2] * src_bytes(v[1]) * v[3] > (uint64_t)(n - 44))
            return false;
        c->data = r;
        break;
    case C_IMG_VIEW:
        c->b = v[1], c->n = v[8];
        for (int i = 0; i < 6; i++) c->i[i] = (int32_t)v[2 + i];
        break;
    case C_FONT_PRELOAD: c->i[0] = (int32_t)v[1], c->data = r + 8; break;
    case C_RESIZE:
        c->b = v[1];
        for (int i = 0; i < 4; i++) c->i[i] = (int32_t)v[2 + i];
        break;
    case C_TEXT: {
        uint32_t nr = v[5], len = v[6];
        if (n < 28 + 12 * (size_t)nr + len) return false;
        shr_style_run *rs = nr ? realloc(p->runs, nr * sizeof(*rs)) : p->runs;
        if (nr && !rs) return false;
        p->runs = rs;
        for (uint32_t i = 0; i < nr; i++)
            rs[i] = (shr_style_run){rec_get32(r + 28 + 12 * i), rec_get32(r + 32 + 12 * i), style(p, rec_get32(r + 36 + 12 * i))};
        c->i[0] = (int32_t)v[1], c->i[1] = (int32_t)v[2], c->st = style(p, v[3]), c->i[2] = (int32_t)v[4];
        c->i[3] = (int32_t)nr, c->n = len, c->data = r + 28 + 12 * (size_t)nr;
        flush(p, comp, api); /* the runs are shared */
        break;
    }
    case C_CLEAR: c->st = style(p, v[5]); break;
    case C_SCROLL: c->st = style(p, v[4]); break;
    case C_LINES: {
        uint32_t nl = v[2];
        if (n < 12 + 12 * (size_t)nl) return false;
        shr_text_line *ls = nl ? realloc(p->lines, nl * sizeof(*ls)) : p->lines;
        if (nl && !ls) return false;
        p->lines = ls;
        for (uint32_t i = 0; i < nl; i++) {
            const uint8_t *q = r + 12 + 12 * i;
            ls[i] = (shr_text_line){get16(q), get16(q + 2), q[4], q[5], get16(q + 6), rec_get32(q + 8)};
        }
        c->i[0] = (int32_t)v[1], c->n = nl;
        flush(p, comp, api); /* the lines are shared */
        break;
    }
    case C_ROW: {
        uint32_t nc = v[3], nt = v[4], ns = v[5], nl = v[6] == UINT32_MAX ? 0 : v[6];
        if (n < 28 + 4 * (size_t)nt + 8 * (size_t)nc + 4 * (size_t)ns + 12 * (size_t)nl) return false;
        shr_row_cell *cs = nc ? realloc(p->cells, nc * sizeof(*cs)) : p->cells;
        shr_text_style *ts = nt ? realloc(p->row_styles, nt * sizeof(*ts)) : p->row_styles;
        uint32_t *sc = ns ? realloc(p->scalars, ns * sizeof(*sc)) : p->scalars;
        shr_text_line *ls = nl ? realloc(p->lines, nl * sizeof(*ls)) : p->lines;
        if ((nc && !cs) || (nt && !ts) || (ns && !sc) || (nl && !ls)) return false;
        p->cells = cs, p->row_styles = ts, p->scalars = sc, p->lines = ls;
        const uint8_t *q = r + 28;
        for (uint32_t i = 0; i < nt; i++, q += 4) ts[i] = style(p, rec_get32(q));
        for (uint32_t i = 0; i < nc; i++, q += 8) cs[i] = (shr_row_cell){rec_get32(q), get16(q + 4), q[6], q[7]};
        for (uint32_t i = 0; i < ns; i++, q += 4) sc[i] = rec_get32(q);
        for (uint32_t i = 0; i < nl; i++, q += 12)
            ls[i] = (shr_text_line){get16(q), get16(q + 2), q[4], q[5], get16(q + 6), rec_get32(q + 8)};
        c->i[2] = (int32_t)nc, c->i[3] = (int32_t)nt, c->i[4] = (int32_t)ns, c->i[5] = v[6] == UINT32_MAX ? -1 : (int32_t)nl;
        flush(p, comp, api); /* the cells, styles, code points and lines are shared */
        break;
    }
    }
    return true;
}

/* Times one pump, poll or submit. */
static void timed(player *p, unsigned type, rec_times *comp) {
    shr_event ev;
    uint64_t t0 = p->h->now_ns(p->h->user);
    if (type == C_PUMP) shr_pump(p->ctx);
    if (type == C_POLL) shr_poll_event(p->ctx, &ev);
    if (type == C_SUBMIT) shr_submit(p->ctx);
    float dt = rec_ms(p->h->now_ns(p->h->user) - t0);
    comp->v[type == C_SUBMIT ? K_SUBMIT : K_BUILD] += dt, comp->v[K_FRAME] += dt;
}

/* As t5_scene_close: layers, images, then the fonts and the context. */
static void teardown(player *p) {
    if (!p->ctx) return;
    for (uint32_t i = 0; i < p->nobjs; i++)
        if (p->objs[i] && p->types[i] == C_LYR_CREATE) shr_lyr_destroy(p->objs[i]), p->objs[i] = NULL;
    for (uint32_t i = 0; i < p->nobjs; i++)
        if (p->objs[i] && p->types[i] == C_IMG_CREATE) shr_pl_res_image_release(p->objs[i]), p->objs[i] = NULL;
    shr_begin_shutdown(p->ctx);
    bool done = false;
    for (int k = 0; k < 64 && !done; k++) {
        shr_pump(p->ctx);
        done = true;
        for (uint32_t i = 0; i < p->nobjs; i++)
            if (p->objs[i] && p->types[i] == C_FONT_CREATE && shr_pl_res_bitmap_font_destroy(p->objs[i]) == SHR_OK)
                p->objs[i] = NULL;
        for (uint32_t i = 0; i < p->nobjs; i++) done &= !p->objs[i];
        done = done && shr_destroy(p->ctx) == SHR_OK;
    }
}

shr_status rec_calls_play(const uint8_t *rec, size_t len, const rec_calls_host *h, rec_writer *verify, rec_times *comp,
                          rec_times *api) {
    rec_header hd;
    rec_info in;
    shr_status st = rec_header_read(rec, len, &hd);
    if (st == SHR_OK) st = rec_scan(rec, len, &in);
    if (st != SHR_OK) return st;
    if (hd.kind != REC_KIND_CALLS || !h->drv || !caps_equal(&hd.caps, &h->drv->caps) || h->ntargets < 1 + hd.screen.band_count ||
        h->scratch_bytes < 16 * sizeof(pcall))
        return SHR_E_PROFILE_MISMATCH;
    player p = {.h = h, .nobjs = in.mems, .nstyles = in.max_batch, .run = h->scratch,
                .cap = (uint32_t)(h->scratch_bytes / sizeof(pcall))};
    p.objs = calloc(p.nobjs + 1u, sizeof(*p.objs)), p.types = calloc(p.nobjs + 1u, 1);
    p.styles = calloc(p.nstyles + 1u, sizeof(*p.styles));
    if (!p.objs || !p.types || !p.styles) st = SHR_E_NO_MEMORY;
    p.drv = *h->drv, p.drv.user = &p, p.drv.execute = null_execute;
    p.drv.cancel = h->drv->cancel ? null_cancel : NULL, p.drv.reset = h->drv->reset ? null_reset : NULL;
    p.drv.sync = h->drv->sync ? null_sync : NULL;
    shr_output_init(&p.output);
    p.output.user = &p, p.output.acquire = out_acquire, p.output.present = out_present, p.output.discard = out_discard;
    p.cd = hd.context, p.cd.allocator = h->allocator, p.cd.user = &p, p.cd.now_ns = play_clock;
    p.cd.driver = &p.drv, p.cd.output = &p.output;
    p.sd = hd.screen, p.sd.bands = hd.screen.band_count ? h->targets + 1 : NULL;
    size_t at = 0, n;
    unsigned type;
    const uint8_t *r;
    while (st == SHR_OK && rec_next(rec, len, &at, &type, &r, &n)) {
        uint32_t t = type == REC_TARGET ? rec_get32(r) : 0;
        if (type == REC_TARGET && (t >= h->ntargets || h->targets[t].width != (int32_t)rec_get32(r + 4) ||
                                   h->targets[t].height != (int32_t)rec_get32(r + 8) ||
                                   h->targets[t].stride != rec_get32(r + 12) || h->targets[t].format != rec_get32(r + 20)))
            st = SHR_E_PROFILE_MISMATCH;
        if ((type == C_LYR_CREATE || type == C_IMG_CREATE || type == C_FONT_CREATE || type == C_IMG_SCALED ||
             type == C_IMG_VIEW) && n >= 4 && rec_get32(r) - 1 < p.nobjs)
            p.types[rec_get32(r) - 1] = (uint8_t)(type == C_LYR_CREATE || type == C_FONT_CREATE ? type : C_IMG_CREATE);
    }
    if (st == SHR_OK && verify) {
        rec_profile prof = {hd.scene, &p.cd, &p.sd, hd.step_ns, hd.frames, hd.packages, hd.npackages};
        st = rec_begin(verify, &prof, &p.drv, h->targets, h->ntargets);
        p.cd.driver = &verify->drv;
    }
    int32_t k = -1;
    for (at = 0; st == SHR_OK && rec_next(rec, len, &at, &type, &r, &n);) {
        rec_times *c = k >= 0 ? &comp[k] : NULL, *a = k >= 0 ? &api[k] : NULL;
        if (type >= C_END) {
            st = SHR_E_FORMAT;
            break;
        }
        if (type >= C_CLOCK && type < C_END && kinds[type - C_CLOCK] != A_ALL) {
            if (!c || !decode(&p, type, r, n, c, a)) st = SHR_E_FORMAT;
            continue;
        }
        if (c && type != C_STYLE) flush(&p, c, a);
        if (type == C_STYLE && n >= 16 && rec_get32(r) < p.nstyles)
            p.styles[rec_get32(r)] = (shr_text_style){rec_get32(r + 4), rec_get32(r + 8), rec_get32(r + 12)};
        if (type == C_CLOCK && n >= 8) p.clock = rec_get64(r);
        if ((type == C_PUMP || type == C_POLL || type == C_SUBMIT) && c) {
            uint32_t before = p.cmds;
            timed(&p, type, c);
            c->cmds += p.cmds - before;
        }
        if (type != REC_FRAME && type != REC_END) continue;
        if (k >= 0 && verify) rec_frame_done(verify, 0);
        if (k >= 0 && h->idle) h->idle(h->user);
        if (type == REC_END || (uint32_t)++k >= in.frames) break;
        comp[k] = (rec_times){0}, api[k] = (rec_times){0};
        p.clock = rec_get64(r + 4);
        if (verify) rec_frame(verify, (int32_t)rec_get32(r), p.clock);
    }
    if (verify) rec_frame(verify, hd.frames, p.clock);
    teardown(&p);
    free(p.objs), free(p.types), free(p.styles), free(p.runs), free(p.lines), free(p.cells), free(p.row_styles),
        free(p.scalars);
    return st;
}
