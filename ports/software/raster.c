#include <stdbool.h>
#include <string.h>

#include "raster.h"
#include "shr_glyph.h"
#include "shr_rect.h"

static inline uint32_t expand(uint32_t v, uint32_t m) { return (v * 255 + m / 2) / m; }
static inline uint32_t blend8(uint32_t fg, uint32_t bg, uint32_t a) {
    return (fg * a + bg * (255 - a) + 127) / 255;
}

typedef struct rgb {
    uint32_t r, g, b;
} rgb;

static inline rgb color_rgb(shr_color c) { return (rgb){(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF}; }

/* RGB565 blends round once from the 5/6-bit destination, as a GPU blending unorm values does:
 * round((m * fg * a + 255 * d * (255 - a)) / 255^2), with w = 255 * (255 - a). */
static inline uint32_t blend565(uint32_t fg, uint32_t d, uint32_t a, uint32_t w, uint32_t m) {
    return (m * fg * a + d * w + 65025 / 2) / 65025;
}

static inline uint8_t *pixel_at(const shr_surface *s, int32_t x, int32_t y) {
    return (uint8_t *)s->pixels + (size_t)y * s->stride + (size_t)x * shr__px_bytes(s->format);
}

static inline const uint8_t *src_at(const shr_image *m, int32_t x, int32_t y) {
    return (const uint8_t *)m->pixels + (size_t)y * m->stride + (size_t)x * shr__px_bytes(m->format);
}

static inline rgb read_px(shr_pixel_format f, const uint8_t *p) {
    if (f == SHR_FORMAT_RGB565) {
        uint16_t v;
        memcpy(&v, p, 2);
        return (rgb){expand((v >> 11) & 31, 31), expand((v >> 5) & 63, 63), expand(v & 31, 31)};
    }
    return (rgb){p[0], p[1], p[2]};
}

static inline void write_px(shr_pixel_format f, uint8_t *p, rgb c) {
    if (f == SHR_FORMAT_RGB565) {
        uint16_t v = (uint16_t)((shr__quantize(c.r, 31) << 11) | (shr__quantize(c.g, 63) << 5) | shr__quantize(c.b, 31));
        memcpy(p, &v, 2);
    } else {
        p[0] = (uint8_t)c.r, p[1] = (uint8_t)c.g, p[2] = (uint8_t)c.b, p[3] = 255;
    }
}

/* Zero coverage leaves the pixel as it was. The builtins: a fortified memcpy keeps loops scalar. */
static inline void blend_px(shr_pixel_format f, uint8_t *p, rgb fg, uint32_t a) {
    if (f != SHR_FORMAT_RGB565) {
        p[0] = (uint8_t)blend8(fg.r, p[0], a), p[1] = (uint8_t)blend8(fg.g, p[1], a), p[2] = (uint8_t)blend8(fg.b, p[2], a);
        p[3] |= (uint8_t)((a != 0) * 255u);
        return;
    }
    uint16_t v, o;
    __builtin_memcpy(&v, p, 2);
    uint32_t w = 255 * (255 - a);
    o = (uint16_t)(blend565(fg.r, v >> 11, a, w, 31) << 11 | blend565(fg.g, (v >> 5) & 63, a, w, 63) << 5 |
                   blend565(fg.b, v & 31, a, w, 31));
    __builtin_memcpy(p, &o, 2);
}

/* Row y of `r`, the region of buffer `b` a GLYPH or IMAGE draws from. */
static inline const uint8_t *region_row(const shr_image *b, shr_rect r, int32_t y) {
    return (const uint8_t *)b->pixels + (size_t)(r.y0 + y) * b->stride;
}

static bool screen_format(shr_pixel_format f) { return f == SHR_FORMAT_RGB565 || f == SHR_FORMAT_RGBX8888; }

static bool rect_inside(shr_rect r, shr_rect b) {
    return r.x0 >= b.x0 && r.y0 >= b.y0 && r.x0 <= r.x1 && r.y0 <= r.y1 && r.x1 <= b.x1 && r.y1 <= b.y1;
}

/* Bytes a buffer region spans, addresses compared as integers; a DEVICE buffer is its handle alone. */
typedef struct span {
    uintptr_t begin, end;
    bool device;
} span;

static span whole(const void *pixels, size_t bytes, shr_memory_domain dom) {
    bool device = dom == SHR_MEMORY_DEVICE;
    return (span){(uintptr_t)pixels, (uintptr_t)pixels + (device ? 1 : bytes), device};
}

/* Rows [y, y + h) and pixels [x, x + w). */
static span span_of(const void *pixels, shr_memory_domain dom, size_t stride, shr_pixel_format f, int32_t x, int32_t y,
                    int32_t w, int32_t h) {
    if (dom == SHR_MEMORY_DEVICE) return whole(pixels, 0, dom);
    size_t bpp = shr__px_bytes(f);
    uintptr_t begin = (uintptr_t)pixels + (size_t)y * stride + (size_t)x * bpp;
    return (span){begin, begin + (size_t)(h - 1) * stride + (size_t)w * bpp, false};
}

static bool spans_overlap(span a, span b) { return a.device == b.device && a.begin < b.end && b.begin < a.end; }

static span copy_src_span(const shr_image *m, shr_point s, shr_rect r) {
    return span_of(m->pixels, m->domain, m->stride, m->format, s.x, s.y, r.x1 - r.x0, r.y1 - r.y0);
}

static span dst_span(const shr_surface *dst, shr_rect r) {
    return span_of(dst->pixels, dst->domain, dst->stride, dst->format, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}

bool shr__raster_reads_dst(const shr_surface *dst, const shr_image *m) SHR_NONBLOCKING {
    return spans_overlap(whole(dst->pixels, dst->byte_length, dst->domain), whole(m->pixels, m->byte_length, m->domain));
}

static shr_status reach(shr__reach_fn fn, const void *user, const void *pixels, int32_t w, int32_t h, shr_pixel_format f,
                        shr_memory_domain dom) SHR_NONBLOCKING {
    if (fn) return fn(user, pixels, w, h, f, dom);
    return dom == SHR_MEMORY_DEVICE ? SHR_E_UNSUPPORTED : SHR_OK;
}

/* Source columns [x0, x1) and rows [0, h) a command with src_origin may read from a w x h source; `syn` holds
 * the BOLD and ITALIC flags of a GLYPH, whose columns may reach into its footprint. */
static shr_status origin_check(const shr_draw_cmd *c, int32_t w, int32_t h, uint32_t syn) {
    int32_t x0 = 0, x1 = w;
    if (syn) {
        if (w > SHR_GLYPH_SYNTH_MAX || h > SHR_GLYPH_SYNTH_MAX ||
            ((syn & SHR_GLYPH_ITALIC) &&
             (c->slant_axis > 4 * SHR_GLYPH_SYNTH_MAX || c->slant_axis < -4 * SHR_GLYPH_SYNTH_MAX)))
            return SHR_E_INVALID_ARG;
        shr__glyph_footprint(w, h, syn, c->slant_axis, &x0, &x1);
    }
    int64_t sx1 = (int64_t)c->src_origin.x + (c->dst.x1 - c->dst.x0);
    int64_t sy1 = (int64_t)c->src_origin.y + (c->dst.y1 - c->dst.y0);
    return c->src_origin.x < x0 || c->src_origin.y < 0 || sx1 > x1 || sy1 > h ? SHR_E_INVALID_ARG : SHR_OK;
}

static shr_status region_check(const shr_draw_cmd *c, const shr_image *buffers, uint32_t n, bool image) {
    if (!c->buffer || c->buffer > n || !buffers[c->buffer - 1].format) return SHR_E_INVALID_ARG;
    const shr_image *b = &buffers[c->buffer - 1];
    bool a4 = b->format == SHR_FORMAT_A4;
    if (image ? b->format != SHR_FORMAT_RGBA8888 : !a4 && b->format != SHR_FORMAT_A8) return SHR_E_UNSUPPORTED;
    shr_rect r = c->src_rect;
    if (!rect_inside(r, (shr_rect){0, 0, b->width, b->height}) || (a4 && (r.x0 & 1))) return SHR_E_INVALID_ARG;
    return origin_check(c, r.x1 - r.x0, r.y1 - r.y0, image ? 0 : c->flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC));
}

static shr_status copy_check(const shr_draw_cmd *c, shr__reach_fn fn, const void *user) {
    const shr_image *m = &c->src;
    if (!screen_format(m->format)) return SHR_E_UNSUPPORTED;
    shr_status st = shr_image_validate(m);
    if (st == SHR_OK) st = origin_check(c, m->width, m->height, 0);
    return st == SHR_OK ? reach(fn, user, m->pixels, m->width, m->height, m->format, m->domain) : st;
}

shr_status shr__raster_buffer_check(const shr_draw_cmd *c, const shr_image *buffers, uint32_t n, shr__reach_fn fn,
                                    const void *user) SHR_NONBLOCKING {
    if (c->kind == SHR_CMD_BUFFER_RELEASE) return SHR_OK;
    if (!c->buffer || c->buffer > n) return SHR_E_INVALID_ARG;
    if (c->kind == SHR_CMD_BUFFER_UPDATE) {
        const shr_image *b = &buffers[c->buffer - 1];
        return b->format && rect_inside(c->src_rect, (shr_rect){0, 0, b->width, b->height}) ? SHR_OK
                                                                                             : SHR_E_INVALID_ARG;
    }
    const shr_image *m = &c->src;
    shr_status st = shr_image_validate(m);
    if (st != SHR_OK) return st;
    if (m->format != SHR_FORMAT_A4 && m->format != SHR_FORMAT_A8 && m->format != SHR_FORMAT_RGBA8888)
        return SHR_E_UNSUPPORTED;
    return reach(fn, user, m->pixels, m->width, m->height, m->format, m->domain);
}

static shr_status rotate_check(const shr_surface *dst, const shr_draw_cmd *c, shr__reach_fn fn, const void *user) {
    const shr_image *m = &c->src;
    shr_status st = shr_image_validate(m);
    if (st != SHR_OK) return st;
    if (!screen_format(m->format)) return SHR_E_UNSUPPORTED;
    if ((st = reach(fn, user, m->pixels, m->width, m->height, m->format, m->domain)) != SHR_OK) return st;
    bool quarter = c->rotation == SHR_ROTATE_90_CW || c->rotation == SHR_ROTATE_90_CCW;
    if (c->rotation != SHR_ROTATE_180 && !quarter) return SHR_E_INVALID_ARG;
    if (dst->width != (quarter ? m->height : m->width) || dst->height != (quarter ? m->width : m->height))
        return SHR_E_INVALID_ARG;
    if (shr__rect_empty(c->dst)) return SHR_OK;
    span s = span_of(m->pixels, m->domain, m->stride, m->format, 0, 0, m->width, m->height);
    return spans_overlap(s, dst_span(dst, c->dst)) ? SHR_E_UNSUPPORTED : SHR_OK;
}

shr_status shr__raster_check(const shr_surface *dst, const shr_draw_cmd *cmds, size_t count, const shr_image *buffers,
                             uint32_t n, shr__reach_fn fn, const void *user) SHR_NONBLOCKING {
    if (count && !cmds) return SHR_E_INVALID_ARG;
    shr_status st = shr_surface_validate(dst);
    if (st != SHR_OK) return st;
    if ((st = reach(fn, user, dst->pixels, dst->width, dst->height, dst->format, dst->domain)) != SHR_OK) return st;
    shr_rect all = {0, 0, dst->width, dst->height};
    const shr_draw_cmd *group = NULL;
    bool drawn = false;
    for (size_t i = 0; i < count; i++) {
        const shr_draw_cmd *c = &cmds[i];
        if (shr__buffer_cmd(c->kind)) {
            if (drawn) return SHR_E_INVALID_ARG;
            continue;
        }
        drawn = true;
        if (c->kind == SHR_CMD_CACHE_BEGIN) {
            if (group || !rect_inside(c->dst, all) || !rect_inside(c->cache_clip, c->dst)) return SHR_E_INVALID_ARG;
            group = c;
            continue;
        }
        if (c->kind == SHR_CMD_CACHE_END) {
            if (!group) return SHR_E_INVALID_ARG;
            group = NULL;
            continue;
        }
        if (!rect_inside(c->dst, group ? group->dst : all)) return SHR_E_INVALID_ARG;
        switch (c->kind) {
        case SHR_CMD_FILL: break;
        case SHR_CMD_GLYPH: st = region_check(c, buffers, n, false); break;
        case SHR_CMD_IMAGE: st = region_check(c, buffers, n, true); break;
        case SHR_CMD_COPY:
            st = copy_check(c, fn, user);
            /* Row-wise memmove handles overlap only between identical layouts. */
            if (st == SHR_OK && (c->src.format != dst->format || c->src.stride != dst->stride) &&
                !shr__rect_empty(c->dst) && spans_overlap(copy_src_span(&c->src, c->src_origin, c->dst), dst_span(dst, c->dst)))
                st = SHR_E_UNSUPPORTED;
            if (st == SHR_OK && group && shr__raster_reads_dst(dst, &c->src)) st = SHR_E_INVALID_ARG;
            break;
        case SHR_CMD_ROTATE: st = group ? SHR_E_INVALID_ARG : rotate_check(dst, c, fn, user); break;
        default: return SHR_E_INVALID_ARG;
        }
        if (st != SHR_OK) return st;
    }
    return group ? SHR_E_INVALID_ARG : SHR_OK;
}

/* Formats are passed as constants so each format compiles to its own loop. */
static inline void fill_rows(const shr_surface *dst, shr_rect r, rgb fg, bool dim, shr_pixel_format f) {
    size_t bpp = shr__px_bytes(f);
    uint8_t px[4];
    write_px(f, px, fg);
    for (int32_t y = r.y0; y < r.y1; y++) {
        uint8_t *p = pixel_at(dst, r.x0, y);
        if (dim)
            for (int32_t x = r.x0; x < r.x1; x++, p += bpp) blend_px(f, p, fg, 128);
        else
            for (int32_t x = 0; x < r.x1 - r.x0; x++) __builtin_memcpy(p + (size_t)x * bpp, px, bpp);
    }
}

static void do_fill(const shr_surface *dst, shr_rect r, shr_color color, bool dim) {
    if (dst->format == SHR_FORMAT_RGB565)
        fill_rows(dst, r, color_rgb(color), dim, SHR_FORMAT_RGB565);
    else
        fill_rows(dst, r, color_rgb(color), dim, SHR_FORMAT_RGBX8888);
}

static inline uint32_t coverage_at(const uint8_t *row, int32_t x, shr_pixel_format g) {
    return g == SHR_FORMAT_A8 ? row[x] : 17u * ((row[x >> 1] >> (~x & 1) * 4) & 15u);
}

#define SPAN 64

/* Coverage of buffer columns [x, x + n) of `row` into cov; returns their OR. */
static inline __attribute__((always_inline)) uint32_t load_coverage(uint8_t *restrict cov, const uint8_t *restrict row,
                                                                    int32_t x, int32_t n, shr_pixel_format g) {
    uint32_t any = 0;
    int32_t k = 0;
    if (g == SHR_FORMAT_A4 && !(x & 1))
        for (const uint8_t *q = row + x / 2; k + 2 <= n; k += 2) {
            uint8_t v = q[k / 2];
            cov[k] = (uint8_t)(17 * (v >> 4)), cov[k + 1] = (uint8_t)(17 * (v & 15)), any |= v;
        }
    for (; k < n; k++) cov[k] = (uint8_t)coverage_at(row, x + k, g), any |= cov[k];
    return any;
}

/* Blends cov[0, n) onto the n pixels at p in blocks of 8 from the right, which vectorize. The leftmost block may start
 * up to 7 pixels left of p when the row has `room` pixels there: cov[-8, 0) is zero, so they stay as they were. */
static inline __attribute__((always_inline)) void blend_span(uint8_t *restrict p, const uint8_t *restrict cov,
                                                             int32_t n, int32_t room, rgb fg, uint32_t dim,
                                                             shr_pixel_format f) {
    size_t bpp = shr__px_bytes(f);
    int32_t k = n;
    for (; k >= 8; k -= 8)
        for (int32_t i = 0; i < 8; i++) blend_px(f, p + (size_t)(k - 8 + i) * bpp, fg, (cov[k - 8 + i] + dim) >> dim);
    if (k > 0 && room >= 8 - k)
        for (int32_t i = 0; i < 8; i++) blend_px(f, p + (k - 8 + i) * (ptrdiff_t)bpp, fg, (cov[k - 8 + i] + dim) >> dim);
    else
        for (int32_t i = 0; i < k; i++) blend_px(f, p + (size_t)i * bpp, fg, (cov[i] + dim) >> dim);
}

/* Inlined by force: clang otherwise keeps one copy and the formats stop being constants. Rows go in spans of SPAN
 * from the right, so only the leftmost span has a partial block. */
static inline __attribute__((always_inline)) void glyph_rows(const shr_surface *dst, const shr_draw_cmd *c,
                                                             const shr_image *b, shr_rect r, shr_point s,
                                                             shr_pixel_format f, shr_pixel_format g) {
    rgb fg = color_rgb(c->color);
    size_t bpp = shr__px_bytes(f);
    uint32_t dim = (c->flags & SHR_GLYPH_DIM) != 0;
    uint8_t buf[8 + SPAN] = {0}, *cov = buf + 8;
    for (int32_t y = r.y0; y < r.y1; y++) {
        const uint8_t *row = region_row(b, c->src_rect, s.y + (y - r.y0));
        uint8_t *p = pixel_at(dst, r.x0, y);
        for (int32_t hi = r.x1 - r.x0; hi > 0; hi -= SPAN) {
            int32_t lo = hi > SPAN ? hi - SPAN : 0;
            if (load_coverage(cov, row, c->src_rect.x0 + s.x + lo, hi - lo, g))
                blend_span(p + (size_t)lo * bpp, cov, hi - lo, r.x0 + lo, fg, dim, f);
        }
    }
}

/* BOLD of the middle sample m between neighbours l and r. */
static inline uint32_t embolden(uint32_t l, uint32_t m, uint32_t r, bool bold) {
    return (!bold || r > m) ? m : m > l ? m : l;
}

/* The shiroko_driver.h coverage formula over a window in(q - 2 .. q + 1), q = sx - k, moved one column per pixel;
 * every term derives from absolute src coordinates, so clipping never changes a pixel. in(i) is rect column i,
 * zero outside the rect: win[j] holds in(c0 + j), loaded only where the rect has pixels. */
static inline __attribute__((always_inline)) void glyph_synth_rows(const shr_surface *dst, const shr_draw_cmd *c,
                                                                   const shr_image *b, shr_rect r, shr_point s,
                                                                   shr_pixel_format f, shr_pixel_format g) {
    rgb fg = color_rgb(c->color);
    size_t bpp = shr__px_bytes(f);
    uint32_t dim = (c->flags & SHR_GLYPH_DIM) != 0;
    bool bold = c->flags & SHR_GLYPH_BOLD, italic = c->flags & SHR_GLYPH_ITALIC;
    int32_t w = c->src_rect.x1 - c->src_rect.x0;
    uint8_t win[SPAN + 3], buf[8 + SPAN] = {0}, *cov = buf + 8;
    for (int32_t y = r.y0; y < r.y1; y++) {
        int32_t sy = s.y + (y - r.y0), k = 0, fr = 0;
        if (italic) shr__slant(c->slant_axis, sy, &k, &fr);
        const uint8_t *row = region_row(b, c->src_rect, sy);
        uint8_t *p = pixel_at(dst, r.x0, y);
        for (int32_t hi = r.x1 - r.x0; hi > 0; hi -= SPAN) {
            int32_t x = hi > SPAN ? hi - SPAN : 0, n = hi - x, c0 = s.x - k + x - 2;
            int32_t lo = c0 < 0 ? -c0 : 0, top = w - c0 < n + 3 ? w - c0 : n + 3;
            if (lo >= top) continue;
            memset(win, 0, sizeof(win));
            if (!load_coverage(win + lo, row, c->src_rect.x0 + c0 + lo, top - lo, g)) continue;
            for (int32_t j = 0; j < n; j += 8) /* whole blocks vectorize; cov past n is unused */
                for (int32_t i = j; i < j + 8; i++) {
                    uint32_t b1 = embolden(win[i], win[i + 1], win[i + 2], bold);
                    uint32_t b2 = embolden(win[i + 1], win[i + 2], win[i + 3], bold);
                    cov[i] = (uint8_t)((b2 * (uint32_t)(256 - fr) + b1 * (uint32_t)fr + 128) >> 8);
                }
            blend_span(p + (size_t)x * bpp, cov, n, r.x0 + x, fg, dim, f);
        }
    }
}

static void do_glyph(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *b, shr_rect r, shr_point s) {
    bool rgb565 = dst->format == SHR_FORMAT_RGB565, a8 = b->format == SHR_FORMAT_A8;
    if (c->flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC)) {
        if (c->src_rect.x0 == c->src_rect.x1) return; /* no coverage, and `pixels` may be NULL */
        if (rgb565 && a8)
            glyph_synth_rows(dst, c, b, r, s, SHR_FORMAT_RGB565, SHR_FORMAT_A8);
        else if (rgb565)
            glyph_synth_rows(dst, c, b, r, s, SHR_FORMAT_RGB565, SHR_FORMAT_A4);
        else if (a8)
            glyph_synth_rows(dst, c, b, r, s, SHR_FORMAT_RGBX8888, SHR_FORMAT_A8);
        else
            glyph_synth_rows(dst, c, b, r, s, SHR_FORMAT_RGBX8888, SHR_FORMAT_A4);
        return;
    }
    if (rgb565 && a8)
        glyph_rows(dst, c, b, r, s, SHR_FORMAT_RGB565, SHR_FORMAT_A8);
    else if (rgb565)
        glyph_rows(dst, c, b, r, s, SHR_FORMAT_RGB565, SHR_FORMAT_A4);
    else if (a8)
        glyph_rows(dst, c, b, r, s, SHR_FORMAT_RGBX8888, SHR_FORMAT_A8);
    else
        glyph_rows(dst, c, b, r, s, SHR_FORMAT_RGBX8888, SHR_FORMAT_A4);
}

static void do_image(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *b, shr_rect r, shr_point s) {
    size_t bpp = shr__px_bytes(dst->format);
    for (int32_t y = r.y0; y < r.y1; y++) {
        const uint8_t *q = region_row(b, c->src_rect, s.y + (y - r.y0)) + (size_t)(c->src_rect.x0 + s.x) * 4;
        uint8_t *p = pixel_at(dst, r.x0, y);
        for (int32_t x = r.x0; x < r.x1; x++, p += bpp, q += 4) {
            rgb fg = {q[0], q[1], q[2]};
            if (q[3] == 255) write_px(dst->format, p, fg);
            else if (q[3]) blend_px(dst->format, p, fg, q[3]);
        }
    }
}

static void do_copy(const shr_surface *dst, const shr_image *m, shr_rect r, shr_point s) {
    int32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    size_t sbpp = shr__px_bytes(m->format), dbpp = shr__px_bytes(dst->format);
    /* Overlapping copies share format and stride: walk rows away from the destination. */
    bool up = copy_src_span(m, s, r).begin < dst_span(dst, r).begin;
    for (int32_t k = 0; k < h; k++) {
        int32_t row = up ? h - 1 - k : k;
        const uint8_t *q = src_at(m, s.x, s.y + row);
        uint8_t *d = pixel_at(dst, r.x0, r.y0 + row);
        if (m->format == dst->format) {
            memmove(d, q, (size_t)w * dbpp);
        } else {
            for (int32_t x = 0; x < w; x++) write_px(dst->format, d + (size_t)x * dbpp, read_px(m->format, q + (size_t)x * sbpp));
        }
    }
}

/* Each output pixel reads the logical source pixel that maps onto it. */
static void do_rotate(const shr_surface *dst, const shr_draw_cmd *c, shr_rect r) {
    const shr_image *m = &c->src;
    size_t bpp = shr__px_bytes(dst->format);
    for (int32_t y = r.y0; y < r.y1; y++) {
        uint8_t *p = pixel_at(dst, r.x0, y);
        for (int32_t x = r.x0; x < r.x1; x++, p += bpp) {
            int32_t sx, sy;
            switch (c->rotation) {
            case SHR_ROTATE_90_CW: sx = y, sy = m->height - 1 - x; break;
            case SHR_ROTATE_90_CCW: sx = m->width - 1 - y, sy = x; break;
            default: sx = m->width - 1 - x, sy = m->height - 1 - y; break;
            }
            const uint8_t *q = src_at(m, sx, sy);
            if (m->format == dst->format) memcpy(p, q, bpp);
            else write_px(dst->format, p, read_px(m->format, q));
        }
    }
}

void shr__raster_draw(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *buffers, shr_point origin,
                      shr_rect clip) SHR_NONBLOCKING {
    shr_rect d = {c->dst.x0 - origin.x, c->dst.y0 - origin.y, c->dst.x1 - origin.x, c->dst.y1 - origin.y};
    shr_rect r = shr__rect_intersect(d, clip);
    if (shr__rect_empty(r)) return;
    shr_point s = {c->src_origin.x + (r.x0 - d.x0), c->src_origin.y + (r.y0 - d.y0)};
    switch (c->kind) {
    case SHR_CMD_FILL: do_fill(dst, r, c->color, (c->flags & SHR_GLYPH_DIM) != 0); break;
    case SHR_CMD_GLYPH: do_glyph(dst, c, &buffers[c->buffer - 1], r, s); break;
    case SHR_CMD_IMAGE: do_image(dst, c, &buffers[c->buffer - 1], r, s); break;
    case SHR_CMD_COPY: do_copy(dst, &c->src, r, s); break;
    default: do_rotate(dst, c, r); break;
    }
}
