#include <stdbool.h>
#include <string.h>

#include "raster.h"
#include "shr_rect.h"

static inline uint32_t expand(uint32_t v, uint32_t m) { return (v * 255 + m / 2) / m; }
static inline uint32_t quantize(uint32_t c, uint32_t m) { return (c * m + 127) / 255; }
static inline uint32_t blend8(uint32_t fg, uint32_t bg, uint32_t a) {
    return (fg * a + bg * (255 - a) + 127) / 255;
}

typedef struct rgb {
    uint32_t r, g, b;
} rgb;

static inline rgb color_rgb(shr_color c) { return (rgb){(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF}; }
static inline rgb blend(rgb fg, rgb bg, uint32_t a) {
    return (rgb){blend8(fg.r, bg.r, a), blend8(fg.g, bg.g, a), blend8(fg.b, bg.b, a)};
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
        uint16_t v = (uint16_t)((quantize(c.r, 31) << 11) | (quantize(c.g, 63) << 5) | quantize(c.b, 31));
        memcpy(p, &v, 2);
    } else {
        p[0] = (uint8_t)c.r, p[1] = (uint8_t)c.g, p[2] = (uint8_t)c.b, p[3] = 255;
    }
}

static inline uint32_t coverage_at(const shr_image *m, int32_t x, int32_t y) {
    const uint8_t *row = (const uint8_t *)m->pixels + (size_t)y * m->stride;
    if (m->format == SHR_FORMAT_A8) return row[x];
    uint8_t b = row[x / 2];
    uint32_t n = (x % 2 == 0) ? (b >> 4) : (b & 0x0F);
    return 17 * n;
}

static bool screen_format(shr_pixel_format f) { return f == SHR_FORMAT_RGB565 || f == SHR_FORMAT_RGBX8888; }

static bool rect_inside(shr_rect r, shr_rect b) {
    return r.x0 >= b.x0 && r.y0 >= b.y0 && r.x0 <= r.x1 && r.y0 <= r.y1 && r.x1 <= b.x1 && r.y1 <= b.y1;
}

/* Bytes spanned by rows [y, y + h) and pixels [x, x + w); addresses compare as integers. */
typedef struct span {
    uintptr_t begin, end;
} span;

static span span_of(const void *pixels, size_t stride, shr_pixel_format f, int32_t x, int32_t y, int32_t w, int32_t h) {
    size_t bpp = shr__px_bytes(f);
    uintptr_t begin = (uintptr_t)pixels + (size_t)y * stride + (size_t)x * bpp;
    return (span){begin, begin + (size_t)(h - 1) * stride + (size_t)w * bpp};
}

static bool spans_overlap(span a, span b) { return a.begin < b.end && b.begin < a.end; }

static span copy_src_span(const shr_image *m, shr_point s, shr_rect r) {
    return span_of(m->pixels, m->stride, m->format, s.x, s.y, r.x1 - r.x0, r.y1 - r.y0);
}

static span dst_span(const shr_surface *dst, shr_rect r) {
    return span_of(dst->pixels, dst->stride, dst->format, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}

/* Cached groups render into a separate buffer, so a group must not read its destination. */
static bool reads_dst(const shr_surface *dst, const shr_image *m) {
    uintptr_t d = (uintptr_t)dst->pixels, p = (uintptr_t)m->pixels;
    return p < d + dst->byte_length && d < p + m->byte_length;
}

static shr_status src_check(const shr_draw_cmd *c, bool format_ok) {
    if (!format_ok) return SHR_E_UNSUPPORTED;
    const shr_image *m = &c->src;
    shr_status st = shr_image_validate(m);
    if (st != SHR_OK) return st;
    int64_t sx1 = (int64_t)c->src_origin.x + (c->dst.x1 - c->dst.x0);
    int64_t sy1 = (int64_t)c->src_origin.y + (c->dst.y1 - c->dst.y0);
    if (c->src_origin.x < 0 || c->src_origin.y < 0 || sx1 > m->width || sy1 > m->height) return SHR_E_INVALID_ARG;
    return m->domain == SHR_MEMORY_DEVICE ? SHR_E_UNSUPPORTED : SHR_OK;
}

static shr_status rotate_check(const shr_surface *dst, const shr_draw_cmd *c) {
    const shr_image *m = &c->src;
    shr_status st = shr_image_validate(m);
    if (st != SHR_OK) return st;
    if (!screen_format(m->format) || m->domain == SHR_MEMORY_DEVICE) return SHR_E_UNSUPPORTED;
    bool quarter = c->rotation == SHR_ROTATE_90_CW || c->rotation == SHR_ROTATE_90_CCW;
    if (c->rotation != SHR_ROTATE_180 && !quarter) return SHR_E_INVALID_ARG;
    if (dst->width != (quarter ? m->height : m->width) || dst->height != (quarter ? m->width : m->height))
        return SHR_E_INVALID_ARG;
    if (shr__rect_empty(c->dst)) return SHR_OK;
    span s = span_of(m->pixels, m->stride, m->format, 0, 0, m->width, m->height);
    return spans_overlap(s, dst_span(dst, c->dst)) ? SHR_E_UNSUPPORTED : SHR_OK;
}

shr_status shr__raster_check(const shr_surface *dst, const shr_draw_cmd *cmds, size_t count) SHR_NONBLOCKING {
    if (count && !cmds) return SHR_E_INVALID_ARG;
    shr_status st = shr_surface_validate(dst);
    if (st != SHR_OK) return st;
    if (dst->domain == SHR_MEMORY_DEVICE) return SHR_E_UNSUPPORTED;
    shr_rect all = {0, 0, dst->width, dst->height};
    const shr_draw_cmd *group = NULL;
    for (size_t i = 0; i < count; i++) {
        const shr_draw_cmd *c = &cmds[i];
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
        case SHR_CMD_GLYPH: st = src_check(c, c->src.format == SHR_FORMAT_A4 || c->src.format == SHR_FORMAT_A8); break;
        case SHR_CMD_IMAGE: st = src_check(c, c->src.format == SHR_FORMAT_RGBA8888); break;
        case SHR_CMD_COPY:
            st = src_check(c, screen_format(c->src.format));
            /* Row-wise memmove handles overlap only between identical layouts. */
            if (st == SHR_OK && (c->src.format != dst->format || c->src.stride != dst->stride) &&
                !shr__rect_empty(c->dst) && spans_overlap(copy_src_span(&c->src, c->src_origin, c->dst), dst_span(dst, c->dst)))
                st = SHR_E_UNSUPPORTED;
            break;
        case SHR_CMD_ROTATE: st = group ? SHR_E_INVALID_ARG : rotate_check(dst, c); break;
        default: return SHR_E_INVALID_ARG;
        }
        if (st != SHR_OK) return st;
        if (group && reads_dst(dst, &c->src)) return SHR_E_INVALID_ARG;
    }
    return group ? SHR_E_INVALID_ARG : SHR_OK;
}

static void do_fill(const shr_surface *dst, shr_rect r, shr_color color, bool dim) {
    size_t bpp = shr__px_bytes(dst->format);
    rgb fg = color_rgb(color);
    uint8_t px[4];
    write_px(dst->format, px, fg);
    for (int32_t y = r.y0; y < r.y1; y++) {
        uint8_t *p = pixel_at(dst, r.x0, y);
        for (int32_t x = r.x0; x < r.x1; x++, p += bpp) {
            if (dim)
                write_px(dst->format, p, blend(fg, read_px(dst->format, p), 128));
            else
                memcpy(p, px, bpp);
        }
    }
}

static void do_glyph(const shr_surface *dst, const shr_draw_cmd *c, shr_rect r, shr_point s) {
    rgb fg = color_rgb(c->color);
    size_t bpp = shr__px_bytes(dst->format);
    for (int32_t y = r.y0; y < r.y1; y++) {
        int32_t sy = s.y + (y - r.y0);
        uint8_t *p = pixel_at(dst, r.x0, y);
        for (int32_t x = r.x0; x < r.x1; x++, p += bpp) {
            uint32_t a = coverage_at(&c->src, s.x + (x - r.x0), sy);
            if (c->flags & SHR_GLYPH_DIM) a = (a + 1) / 2;
            if (a == 0) continue;
            write_px(dst->format, p, blend(fg, read_px(dst->format, p), a));
        }
    }
}

static void do_image(const shr_surface *dst, const shr_draw_cmd *c, shr_rect r, shr_point s) {
    size_t bpp = shr__px_bytes(dst->format);
    for (int32_t y = r.y0; y < r.y1; y++) {
        const uint8_t *q = src_at(&c->src, s.x, s.y + (y - r.y0));
        uint8_t *p = pixel_at(dst, r.x0, y);
        for (int32_t x = r.x0; x < r.x1; x++, p += bpp, q += 4) {
            rgb fg = {q[0], q[1], q[2]};
            if (q[3] == 255) write_px(dst->format, p, fg);
            else if (q[3]) write_px(dst->format, p, blend(fg, read_px(dst->format, p), q[3]));
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

void shr__raster_draw(const shr_surface *dst, const shr_draw_cmd *c, shr_point origin, shr_rect clip) SHR_NONBLOCKING {
    shr_rect d = {c->dst.x0 - origin.x, c->dst.y0 - origin.y, c->dst.x1 - origin.x, c->dst.y1 - origin.y};
    shr_rect r = shr__rect_intersect(d, clip);
    if (shr__rect_empty(r)) return;
    shr_point s = {c->src_origin.x + (r.x0 - d.x0), c->src_origin.y + (r.y0 - d.y0)};
    switch (c->kind) {
    case SHR_CMD_FILL: do_fill(dst, r, c->color, (c->flags & SHR_GLYPH_DIM) != 0); break;
    case SHR_CMD_GLYPH: do_glyph(dst, c, r, s); break;
    case SHR_CMD_IMAGE: do_image(dst, c, r, s); break;
    case SHR_CMD_COPY: do_copy(dst, &c->src, r, s); break;
    default: do_rotate(dst, c, r); break;
    }
}
