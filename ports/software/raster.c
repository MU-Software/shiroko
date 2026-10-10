#include <stdbool.h>
#include <string.h>

#include "raster.h"
#include "shr_glyph.h"
#include "shr_rect.h"
#include "shr_scale.h"

/* 1 on cores without SIMD, where the blend loops stay scalar: they skip zero coverage and store full coverage without
 * DIM as the colour itself, the same bytes the blend gives. A build may define it either way. */
#ifndef SHR_SCALAR_BLEND
#if defined(__SSE2__) || defined(__ARM_NEON) || defined(__ARM_FEATURE_MVE) || defined(__riscv_vector) || \
    defined(__wasm_simd128__)
#define SHR_SCALAR_BLEND 0
#else
#define SHR_SCALAR_BLEND 1
#endif
#endif

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

static inline const uint8_t *src_at(const shr_image_ref *m, int32_t x, int32_t y) {
    return (const uint8_t *)m->pixels + (size_t)y * m->stride + (size_t)x * shr__px_bytes((shr_pixel_format)m->format);
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

static span copy_src_span(const shr_image_ref *m, shr_point s, shr_rect r) {
    return span_of(m->pixels, (shr_memory_domain)m->domain, m->stride, (shr_pixel_format)m->format, s.x, s.y, r.x1 - r.x0,
                   r.y1 - r.y0);
}

static span dst_span(const shr_surface *dst, shr_rect r) {
    return span_of(dst->pixels, dst->domain, dst->stride, dst->format, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}

bool shr__raster_reads_dst(const shr_surface *dst, const shr_image_ref *m) SHR_NONBLOCKING {
    span src = m->width > 0 && m->height > 0 ? copy_src_span(m, (shr_point){0, 0}, (shr_rect){0, 0, m->width, m->height})
                                             : whole(m->pixels, 0, (shr_memory_domain)m->domain);
    return spans_overlap(whole(dst->pixels, dst->byte_length, dst->domain), src);
}

static shr_status reach(shr__reach_fn fn, const void *user, const void *pixels, int32_t w, int32_t h, shr_pixel_format f,
                        shr_memory_domain dom) SHR_NONBLOCKING {
    if (fn) return fn(user, pixels, w, h, f, dom);
    return dom == SHR_MEMORY_DEVICE ? SHR_E_UNSUPPORTED : SHR_OK;
}

/* Source columns [x0, x1) and rows [0, h) a command with src_origin may read from a w x h source; `syn` holds
 * the BOLD and ITALIC flags of a GLYPH, whose columns may reach into its footprint. */
static shr_status origin_check(const shr_draw_cmd *c, int32_t w, int32_t h, uint32_t syn) {
    int32_t x0 = 0, x1 = w, axis = syn & SHR_GLYPH_ITALIC ? c->slant_axis : 0;
    if (syn) {
        if (w > SHR_GLYPH_SYNTH_MAX || h > SHR_GLYPH_SYNTH_MAX || axis > 4 * SHR_GLYPH_SYNTH_MAX ||
            axis < -4 * SHR_GLYPH_SYNTH_MAX)
            return SHR_E_INVALID_ARG;
        shr__glyph_footprint(w, h, syn, axis, &x0, &x1);
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
    if (image && (c->flags & SHR_IMAGE_SCALED)) {
        if (shr__rect_empty(r) || r.x1 - r.x0 > SHR__SCALE_MAX || r.y1 - r.y0 > SHR__SCALE_MAX || c->scale_w <= 0 ||
            c->scale_h <= 0 || c->scale_w > SHR__SCALE_MAX || c->scale_h > SHR__SCALE_MAX)
            return SHR_E_INVALID_ARG;
        return origin_check(c, c->scale_w, c->scale_h, 0);
    }
    return origin_check(c, r.x1 - r.x0, r.y1 - r.y0, image ? 0 : c->flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC));
}

static shr_status line_check(const shr_draw_cmd *c) {
    int32_t w = c->src_rect.x1, h = c->src_rect.y1;
    int64_t sy1 = (int64_t)c->src_origin.y + (c->dst.y1 - c->dst.y0);
    bool bad = (c->flags & ~(SHR_GLYPH_DIM | 7u << SHR_LINE_SHAPE_SHIFT)) ||
               c->flags >> SHR_LINE_SHAPE_SHIFT > SHR_LINE_DASHED || c->src_rect.x0 || c->src_rect.y0 || w < 1 ||
               w > SHR_LINE_MAX_PERIOD || h < 1 || h > SHR_LINE_MAX_BAND || c->src_origin.x < 0 ||
               c->src_origin.x >= w || c->src_origin.y < 0 || sy1 > h;
    return bad ? SHR_E_INVALID_ARG : SHR_OK;
}

static shr_status copy_check(const shr_draw_cmd *c, shr__reach_fn fn, const void *user) {
    shr_image m;
    if (!screen_format((shr_pixel_format)c->src.format)) return SHR_E_UNSUPPORTED;
    shr_status st = shr_image_ref_get(&c->src, &m);
    if (st == SHR_OK) st = origin_check(c, m.width, m.height, 0);
    return st == SHR_OK ? reach(fn, user, m.pixels, m.width, m.height, m.format, m.domain) : st;
}

shr_status shr__raster_buffer_check(const shr_draw_cmd *c, const shr_image *buffers, uint32_t n, shr__reach_fn fn,
                                    const void *user, shr_image *mem) SHR_NONBLOCKING {
    if (c->kind == SHR_CMD_BUFFER_RELEASE) return SHR_OK;
    if (!c->buffer || c->buffer > n) return SHR_E_INVALID_ARG;
    if (c->kind == SHR_CMD_BUFFER_UPDATE) {
        const shr_image *b = &buffers[c->buffer - 1];
        return b->format && rect_inside(c->src_rect, (shr_rect){0, 0, b->width, b->height}) ? SHR_OK
                                                                                             : SHR_E_INVALID_ARG;
    }
    shr_status st = shr_image_ref_get(&c->src, mem);
    if (st != SHR_OK) return st;
    if (mem->format != SHR_FORMAT_A4 && mem->format != SHR_FORMAT_A8 && mem->format != SHR_FORMAT_RGBA8888)
        return SHR_E_UNSUPPORTED;
    return reach(fn, user, mem->pixels, mem->width, mem->height, mem->format, mem->domain);
}

static shr_status rotate_check(const shr_surface *dst, const shr_draw_cmd *c, shr__reach_fn fn, const void *user) {
    shr_image m;
    shr_status st = shr_image_ref_get(&c->src, &m);
    if (st != SHR_OK) return st;
    if (!screen_format(m.format)) return SHR_E_UNSUPPORTED;
    if ((st = reach(fn, user, m.pixels, m.width, m.height, m.format, m.domain)) != SHR_OK) return st;
    bool quarter = c->rotation == SHR_ROTATE_90_CW || c->rotation == SHR_ROTATE_90_CCW;
    if (c->rotation != SHR_ROTATE_180 && !quarter) return SHR_E_INVALID_ARG;
    if (c->dst.x1 - c->dst.x0 != (quarter ? m.height : m.width) || c->dst.y1 - c->dst.y0 != (quarter ? m.width : m.height))
        return SHR_E_INVALID_ARG;
    if (shr__rect_empty(c->dst)) return SHR_OK;
    span s = span_of(m.pixels, m.domain, m.stride, m.format, 0, 0, m.width, m.height);
    return spans_overlap(s, dst_span(dst, c->dst)) ? SHR_E_UNSUPPORTED : SHR_OK;
}

#define GROUP_REACH (1 << 24)

static shr__keep *keep_at(const shr__keeps *k, uint32_t id) { return k && id && id <= k->n ? &k->at[id - 1] : NULL; }

/* What a KEEP_DRAW reads: the keep as this batch stored it, else as it is held. */
static shr_status keep_draw_check(shr__keeps *k, const shr_surface *dst, const shr_draw_cmd *c) {
    shr__keep *e = keep_at(k, c->buffer);
    if (!e) return SHR_E_INVALID_ARG;
    bool now = e->mark == k->batch && e->stored;
    shr_pixel_format f = now ? dst->format : e->format;
    if (f != dst->format) return SHR_E_INVALID_ARG;
    if (e->mark != k->batch) e->mark = k->batch, e->stored = false;
    return origin_check(c, now ? e->store_w : e->width, now ? e->store_h : e->height, 0);
}

shr_status shr__raster_check(const shr_surface *dst, const shr_draw_cmd *cmds, size_t count, const shr_image *buffers,
                             uint32_t n, shr__keeps *keeps, shr__reach_fn fn, const void *user) SHR_NONBLOCKING {
    if (count && !cmds) return SHR_E_INVALID_ARG;
    shr_status st = shr_surface_validate(dst);
    if (st != SHR_OK) return st;
    if ((st = reach(fn, user, dst->pixels, dst->width, dst->height, dst->format, dst->domain)) != SHR_OK) return st;
    shr_rect all = {0, 0, dst->width, dst->height};
    const shr_draw_cmd *group = NULL;
    bool drawn = false;
    if (keeps) keeps->batch++, keeps->nstores = 0;
    for (size_t i = 0; i < count; i++) {
        const shr_draw_cmd *c = &cmds[i];
        if (shr__buffer_cmd(c->kind)) {
            if (drawn) return SHR_E_INVALID_ARG;
            continue;
        }
        drawn = true;
        if (c->kind == SHR_CMD_KEEP_BEGIN) {
            shr__keep *e = keep_at(keeps, c->buffer);
            if (group || !e || e->mark == keeps->batch ||
                !rect_inside(c->dst, (shr_rect){-GROUP_REACH, -GROUP_REACH, GROUP_REACH, GROUP_REACH}) ||
                (keeps->slot && (uint64_t)(c->dst.x1 - c->dst.x0) * (uint64_t)(c->dst.y1 - c->dst.y0) *
                                        shr__px_bytes(dst->format) > keeps->slot))
                return SHR_E_INVALID_ARG;
            e->mark = keeps->batch, e->stored = true;
            e->store_w = c->dst.x1 - c->dst.x0, e->store_h = c->dst.y1 - c->dst.y0;
            keeps->stores[keeps->nstores++] = c->buffer;
            group = c;
            continue;
        }
        if (c->kind == SHR_CMD_KEEP_END) {
            if (!group) return SHR_E_INVALID_ARG;
            group = NULL;
            continue;
        }
        if (!rect_inside(c->dst, group ? group->dst : all)) return SHR_E_INVALID_ARG;
        switch (c->kind) {
        case SHR_CMD_FILL: break;
        case SHR_CMD_GLYPH: st = region_check(c, buffers, n, false); break;
        case SHR_CMD_IMAGE: st = region_check(c, buffers, n, true); break;
        case SHR_CMD_LINE: st = line_check(c); break;
        case SHR_CMD_COPY:
            st = copy_check(c, fn, user);
            /* Row-wise memmove handles overlap only between identical layouts. */
            if (st == SHR_OK && (c->src.format != dst->format || c->src.stride != dst->stride)) {
                shr_rect in = shr__rect_intersect(c->dst, all); /* what a group's command may write */
                if (!shr__rect_empty(in) &&
                    spans_overlap(copy_src_span(&c->src, c->src_origin, c->dst), dst_span(dst, in)))
                    st = SHR_E_UNSUPPORTED;
            }
            if (st == SHR_OK && group && shr__raster_reads_dst(dst, &c->src)) st = SHR_E_INVALID_ARG;
            break;
        case SHR_CMD_ROTATE: st = group ? SHR_E_INVALID_ARG : rotate_check(dst, c, fn, user); break;
        case SHR_CMD_KEEP_DRAW: st = group ? SHR_E_INVALID_ARG : keep_draw_check(keeps, dst, c); break;
        default: return SHR_E_INVALID_ARG;
        }
        if (st != SHR_OK) return st;
    }
    return group ? SHR_E_INVALID_ARG : SHR_OK;
}

/* Formats are passed as constants so each format compiles to its own loop. SHR_SCALAR_BLEND stores `word`, the
 * colour (RGB565: twice), 32 bits at a time from a 4-byte boundary. */
static inline __attribute__((always_inline)) void fill_rows(const shr_surface *dst, shr_rect r, rgb fg, bool dim,
                                                            shr_pixel_format f) {
    size_t bpp = shr__px_bytes(f);
    uint8_t px[4];
    uint32_t word;
    write_px(f, px, fg);
    if (f == SHR_FORMAT_RGB565) __builtin_memcpy(px + 2, px, 2);
    __builtin_memcpy(&word, px, 4);
    uint8_t *row = pixel_at(dst, r.x0, r.y0);
    size_t ds = dst->stride, n_bytes = (size_t)(r.x1 - r.x0) * bpp; /* the stores below may alias the stride */
    for (int32_t y = r.y0; y < r.y1; y++, row += ds) {
        uint8_t *p = row, *end = row + n_bytes;
        int32_t n = r.x1 - r.x0;
        if (dim) {
            for (int32_t x = 0; x < n; x++, p += bpp) blend_px(f, p, fg, 128);
        } else if (SHR_SCALAR_BLEND) {
            if (f == SHR_FORMAT_RGB565 && ((uintptr_t)p & 2)) __builtin_memcpy(p, &word, 2), p += 2;
            for (; end - p >= 16; p += 16) { /* unrolled: gcc -O2 does not */
                __builtin_memcpy(p, &word, 4), __builtin_memcpy(p + 4, &word, 4);
                __builtin_memcpy(p + 8, &word, 4), __builtin_memcpy(p + 12, &word, 4);
            }
            for (; end - p >= 4; p += 4) __builtin_memcpy(p, &word, 4);
            if (f == SHR_FORMAT_RGB565 && p < end) __builtin_memcpy(p, &word, 2);
        } else {
            for (int32_t x = 0; x < n; x++) __builtin_memcpy(p + (size_t)x * bpp, px, bpp);
        }
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

/* One SHR_SCALAR_BLEND pixel of coverage c: 0 leaves it, `full` (255 without DIM) stores the colour `px`. */
static inline __attribute__((always_inline)) void blend_cov(shr_pixel_format f, uint8_t *p, const uint8_t *px, rgb fg,
                                                            uint32_t c, uint32_t full, uint32_t dim) {
    if (c == full)
        __builtin_memcpy(p, px, shr__px_bytes(f));
    else if (c)
        blend_px(f, p, fg, (c + dim) >> dim);
}

/* Blends cov[0, n) onto the n pixels at p in blocks of 8 from the right, which vectorize. The leftmost block may start
 * up to 7 pixels left of p when the row has `room` pixels there: cov[-8, 0) is zero, so they stay as they were.
 * SHR_SCALAR_BLEND goes from the left in groups of 4 and skips a group of all 0 at once; testing a group for all 255
 * the same way measured no faster than blend_cov storing them one by one. */
static inline __attribute__((always_inline)) void blend_span(uint8_t *restrict p, const uint8_t *restrict cov,
                                                             int32_t n, int32_t room, rgb fg, uint32_t dim,
                                                             shr_pixel_format f) {
    size_t bpp = shr__px_bytes(f);
    int32_t k = n;
    if (SHR_SCALAR_BLEND) {
        uint32_t full = 255 + dim; /* unreachable under DIM */
        uint8_t px[4];
        write_px(f, px, fg);
        int32_t i = 0;
        for (; i + 4 <= n; i += 4) {
            uint32_t v;
            __builtin_memcpy(&v, cov + i, 4);
            if (!v) continue;
            /* unrolled: the compilers keep a loop here, which costs more than the groups save */
            blend_cov(f, p + (size_t)i * bpp, px, fg, cov[i], full, dim);
            blend_cov(f, p + (size_t)(i + 1) * bpp, px, fg, cov[i + 1], full, dim);
            blend_cov(f, p + (size_t)(i + 2) * bpp, px, fg, cov[i + 2], full, dim);
            blend_cov(f, p + (size_t)(i + 3) * bpp, px, fg, cov[i + 3], full, dim);
        }
        for (; i < n; i++) blend_cov(f, p + (size_t)i * bpp, px, fg, cov[i], full, dim);
        return;
    }
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

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "pixels and coverage are read as words: the first byte low");

static inline __attribute__((always_inline)) void lut_px(uint8_t *lut, rgb fg, shr_color bg, uint32_t dim,
                                                         shr_pixel_format f) {
    size_t bpp = shr__px_bytes(f);
    uint8_t px[4];
    write_px(f, px, color_rgb(bg));
    for (uint32_t a = 0; a < 256; a++, lut += bpp) {
        __builtin_memcpy(lut, px, bpp);
        blend_px(f, lut, fg, (a + dim) >> dim);
    }
}

void shr__raster_lut(uint8_t *lut, shr_pixel_format f, shr_color color, shr_color bg, uint32_t dim) SHR_NONBLOCKING {
    if (f == SHR_FORMAT_RGB565)
        lut_px(lut, color_rgb(color), bg, dim, SHR_FORMAT_RGB565);
    else
        lut_px(lut, color_rgb(color), bg, dim, SHR_FORMAT_RGBX8888);
}

/* cov[0, n) onto the n pixels at p through `lut`, in groups of 4 from p; a group of all zero coverage is left as the
 * FILL, lut[0]. */
static inline __attribute__((always_inline)) void lut_span(uint8_t *restrict p, const uint8_t *restrict cov, int32_t n,
                                                           const uint8_t *lut, shr_pixel_format f) {
    size_t bpp = shr__px_bytes(f);
    int32_t k = 0;
    for (; k + 4 <= n; k += 4) {
        uint32_t v;
        uint8_t four[16];
        __builtin_memcpy(&v, cov + k, 4);
        if (!v) continue;
        __builtin_memcpy(four, lut + (v & 255) * bpp, bpp);
        __builtin_memcpy(four + bpp, lut + (v >> 8 & 255) * bpp, bpp);
        __builtin_memcpy(four + 2 * bpp, lut + (v >> 16 & 255) * bpp, bpp);
        __builtin_memcpy(four + 3 * bpp, lut + (v >> 24) * bpp, bpp);
        __builtin_memcpy(p + (size_t)k * bpp, four, 4 * bpp);
    }
    for (; k < n; k++) __builtin_memcpy(p + (size_t)k * bpp, lut + cov[k] * bpp, bpp);
}

static inline __attribute__((always_inline)) void glyph_lut_rows(const shr_surface *dst, shr_rect r, const uint8_t *src,
                                                                 size_t stride, const uint8_t *lut, shr_pixel_format f) {
    uint8_t *p = pixel_at(dst, r.x0, r.y0);
    for (int32_t y = r.y0; y < r.y1; y++, src += stride, p += dst->stride) lut_span(p, src, r.x1 - r.x0, lut, f);
}

void shr__raster_glyph_lut(const shr_surface *dst, shr_rect r, const uint8_t *src, size_t stride,
                           const uint8_t *lut) SHR_NONBLOCKING {
    if (dst->format == SHR_FORMAT_RGB565)
        glyph_lut_rows(dst, r, src, stride, lut, SHR_FORMAT_RGB565);
    else
        glyph_lut_rows(dst, r, src, stride, lut, SHR_FORMAT_RGBX8888);
}

/* BOLD of the middle sample m between neighbours l and r. */
static inline uint32_t embolden(uint32_t l, uint32_t m, uint32_t r, bool bold) {
    return (!bold || r > m) ? m : m > l ? m : l;
}

/* The shiroko_driver.h coverage formula for rect columns [x, x + n), n <= SPAN, of a row sheared by k + fr / 256, into
 * cov (in whole blocks of 8 under !SHR_SCALAR_BLEND); 0 when all of it is 0. It runs a window in(q - 2 .. q + 1),
 * q = x - k, one column per pixel; every term derives from absolute rect coordinates, so clipping never changes a
 * pixel. in(i) is column i of the w-wide rect at buffer column rx, zero outside: win[j] holds in(c0 + j), loaded only
 * where the rect has pixels. */
static inline __attribute__((always_inline)) bool synth_span(uint8_t *restrict cov, const uint8_t *restrict row,
                                                             int32_t rx, int32_t w, int32_t x, int32_t n, int32_t k,
                                                             int32_t fr, bool bold, shr_pixel_format g) {
    uint8_t win[SPAN + 3];
    int32_t c0 = x - k - 2, lo = c0 < 0 ? -c0 : 0, top = w - c0 < n + 3 ? w - c0 : n + 3;
    if (lo >= top) return false;
    memset(win, 0, sizeof(win));
    if (!load_coverage(win + lo, row, rx + c0 + lo, top - lo, g)) return false;
    for (int32_t j = 0; j < n; j += 8) /* whole blocks vectorize; cov past n is unused */
        for (int32_t i = j; i < j + 8 && (!SHR_SCALAR_BLEND || i < n); i++) {
            uint32_t b1 = embolden(win[i], win[i + 1], win[i + 2], bold);
            uint32_t b2 = embolden(win[i + 1], win[i + 2], win[i + 3], bold);
            cov[i] = (uint8_t)((b2 * (uint32_t)(256 - fr) + b1 * (uint32_t)fr + 128) >> 8);
        }
    return true;
}

/* With `lut` (ON_FILL) the coverage goes through it, in spans from the left so that its groups start where
 * shr__raster_glyph_lut's do; else it blends. */
static inline __attribute__((always_inline)) void glyph_synth_rows(const shr_surface *dst, const shr_draw_cmd *c,
                                                                   const shr_image *b, shr_rect r, shr_point s,
                                                                   const uint8_t *lut, shr_pixel_format f,
                                                                   shr_pixel_format g) {
    rgb fg = color_rgb(c->color);
    size_t bpp = shr__px_bytes(f);
    uint32_t dim = (c->flags & SHR_GLYPH_DIM) != 0;
    bool bold = c->flags & SHR_GLYPH_BOLD, italic = c->flags & SHR_GLYPH_ITALIC;
    int32_t w = c->src_rect.x1 - c->src_rect.x0;
    uint8_t buf[8 + SPAN] = {0}, *cov = buf + 8;
    for (int32_t y = r.y0; y < r.y1; y++) {
        int32_t sy = s.y + (y - r.y0), k = 0, fr = 0;
        if (italic) shr__slant(c->slant_axis, sy, &k, &fr);
        const uint8_t *row = region_row(b, c->src_rect, sy);
        uint8_t *p = pixel_at(dst, r.x0, y);
        for (int32_t x = 0; lut && x < r.x1 - r.x0; x += SPAN) {
            int32_t n = r.x1 - r.x0 - x < SPAN ? r.x1 - r.x0 - x : SPAN;
            if (!synth_span(cov, row, c->src_rect.x0, w, s.x + x, n, k, fr, bold, g)) memset(cov, 0, (size_t)n);
            lut_span(p + (size_t)x * bpp, cov, n, lut, f);
        }
        for (int32_t hi = lut ? 0 : r.x1 - r.x0; hi > 0; hi -= SPAN) {
            int32_t x = hi > SPAN ? hi - SPAN : 0;
            if (synth_span(cov, row, c->src_rect.x0, w, s.x + x, hi - x, k, fr, bold, g))
                blend_span(p + (size_t)x * bpp, cov, hi - x, r.x0 + x, fg, dim, f);
        }
    }
}

static inline __attribute__((always_inline)) void synth_rows(uint8_t *out, const shr_image *b, shr_rect rect,
                                                             uint32_t flags, int32_t axis, int32_t x0, int32_t w,
                                                             shr_pixel_format g) {
    uint8_t cov[SPAN];
    for (int32_t y = 0; y < rect.y1 - rect.y0; y++, out += w) {
        int32_t k = 0, fr = 0;
        if (flags & SHR_GLYPH_ITALIC) shr__slant(axis, y, &k, &fr);
        for (int32_t x = 0; x < w; x += SPAN) {
            int32_t n = w - x < SPAN ? w - x : SPAN;
            if (synth_span(cov, region_row(b, rect, y), rect.x0, rect.x1 - rect.x0, x0 + x, n, k, fr,
                           flags & SHR_GLYPH_BOLD, g))
                memcpy(out + x, cov, (size_t)n);
            else
                memset(out + x, 0, (size_t)n);
        }
    }
}

void shr__raster_synth(uint8_t *out, const shr_image *b, shr_rect rect, uint32_t flags, int32_t axis, int32_t x0,
                       int32_t w) SHR_NONBLOCKING {
    if (b->format == SHR_FORMAT_A8)
        synth_rows(out, b, rect, flags, axis, x0, w, SHR_FORMAT_A8);
    else
        synth_rows(out, b, rect, flags, axis, x0, w, SHR_FORMAT_A4);
}

static void do_glyph(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *b, shr_rect r, shr_point s) {
    bool rgb565 = dst->format == SHR_FORMAT_RGB565, a8 = b->format == SHR_FORMAT_A8;
    if (c->flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC)) {
        if (c->src_rect.x0 == c->src_rect.x1) return; /* no coverage, and `pixels` may be NULL */
        uint8_t table[256 * 4];
        const uint8_t *lut = c->flags & SHR_GLYPH_ON_FILL ? table : NULL; /* as the driver's cache draws it */
        if (lut) shr__raster_lut(table, dst->format, c->color, c->bg, c->flags & SHR_GLYPH_DIM);
        if (rgb565 && a8)
            glyph_synth_rows(dst, c, b, r, s, lut, SHR_FORMAT_RGB565, SHR_FORMAT_A8);
        else if (rgb565)
            glyph_synth_rows(dst, c, b, r, s, lut, SHR_FORMAT_RGB565, SHR_FORMAT_A4);
        else if (a8)
            glyph_synth_rows(dst, c, b, r, s, lut, SHR_FORMAT_RGBX8888, SHR_FORMAT_A8);
        else
            glyph_synth_rows(dst, c, b, r, s, lut, SHR_FORMAT_RGBX8888, SHR_FORMAT_A4);
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

#define LINE_RUN (4 * SPAN)

/* Cell rows of full coverage draw as FILLs, rows of none not at all, the others as A8 GLYPHs from the row repeated in
 * a strip, in runs that each start at their phase. */
static void do_line(const shr_surface *dst, const shr_draw_cmd *c, const uint8_t *cell, shr_rect r, shr_point s) {
    int32_t w = c->src_rect.x1, n = r.x1 - r.x0, len = (n < LINE_RUN ? n : LINE_RUN) + w;
    bool dim = c->flags & SHR_GLYPH_DIM;
    uint8_t strip[LINE_RUN + SHR_LINE_MAX_PERIOD];
    shr_image b = {strip, len, 1, sizeof(strip), sizeof(strip), SHR_FORMAT_A8, SHR_MEMORY_CPU};
    shr_draw_cmd g = {.kind = SHR_CMD_GLYPH, .flags = dim, .color = c->color, .src_rect = {0, 0, len, 1}};
    for (int32_t y = r.y0; y < r.y1; y++) {
        const uint8_t *row = cell + (size_t)(s.y + y - r.y0) * (size_t)w;
        uint32_t all = 255, any = 0;
        for (int32_t x = 0; x < w; x++) all &= row[x], any |= row[x];
        if (all == 255) {
            do_fill(dst, (shr_rect){r.x0, y, r.x1, y + 1}, c->color, dim);
            continue;
        }
        if (!any) continue;
        memcpy(strip, row, (size_t)w);
        for (int32_t have = w, k; have < len; have += k) { /* doubling: no division per byte */
            k = have < len - have ? have : len - have;
            memcpy(strip + have, strip, (size_t)k);
        }
        for (int32_t x = 0; x < n; x += LINE_RUN) {
            int32_t k = n - x < LINE_RUN ? n - x : LINE_RUN;
            do_glyph(dst, &g, &b, (shr_rect){r.x0 + x, y, r.x0 + x + k, y + 1}, (shr_point){(s.x + x) % w, 0});
        }
    }
}

void shr__raster_line(const shr_surface *dst, const shr_draw_cmd *c, shr_point origin, shr_rect clip,
                      shr__line_memo *memo) SHR_NONBLOCKING {
    shr_rect d = {c->dst.x0 - origin.x, c->dst.y0 - origin.y, c->dst.x1 - origin.x, c->dst.y1 - origin.y};
    shr_rect r = shr__rect_intersect(d, clip);
    if (shr__rect_empty(r)) return;
    uint8_t tmp[SHR_LINE_MAX_PERIOD * SHR_LINE_MAX_BAND];
    shr_point s = {c->src_origin.x + (r.x0 - d.x0), c->src_origin.y + (r.y0 - d.y0)};
    do_line(dst, c, shr__raster_line_cell(c, memo, tmp), r, s);
}

shr__pal_set shr__pals[SHR__PAL_SETS];

/* Bits 16.. pick the set of shr__pals, the top 4 bits the memo entry. */
_Static_assert(SHR__MEMO_PALS == 16, "memo entries");
static uint32_t pal_hash(uint32_t k0, uint32_t k1) {
    uint32_t x = k0 ^ (uint32_t)((uint64_t)k1 * 0x9E3779B1u);
    x ^= x >> 15, x = (uint32_t)((uint64_t)x * 0x2C1B3C6Du);
    return x ^ x >> 12;
}

static bool pal_holds(shr__pal *e, uint32_t k0, uint32_t k1) {
    return !((atomic_load_explicit(&e->key[0], memory_order_relaxed) ^ k0) |
             (atomic_load_explicit(&e->key[1], memory_order_relaxed) ^ k1));
}

/* Copies the palette of key {k0, k1} into pal; false when the set lacks it or it is being written. */
static bool pal_get(shr__pal_set *set, uint32_t k0, uint32_t k1, uint32_t *pal, size_t words) {
    for (int w = 0; w < SHR__PAL_WAYS; w++) {
        shr__pal *e = &set->way[w];
        if (!pal_holds(e, k0, k1)) continue;
        uint32_t s = atomic_load_explicit(&e->seq, memory_order_acquire);
        if ((s & 1) | !pal_holds(e, k0, k1)) return false;
        for (size_t i = 0; i < words; i++) pal[i] = atomic_load_explicit(&e->pal[i], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        return atomic_load_explicit(&e->seq, memory_order_relaxed) == s;
    }
    return false;
}

/* Puts the palette in the set's next way, unless another caller is writing that entry. */
static void pal_put(shr__pal_set *set, uint32_t k0, uint32_t k1, const uint32_t *pal, size_t words) {
    shr__pal *e = &set->way[atomic_fetch_add_explicit(&set->next, 1, memory_order_relaxed) % SHR__PAL_WAYS];
    uint32_t s = atomic_load_explicit(&e->seq, memory_order_relaxed) & ~1u;
    if (!atomic_compare_exchange_strong_explicit(&e->seq, &s, s + 1, memory_order_relaxed, memory_order_relaxed)) return;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&e->key[0], k0, memory_order_relaxed);
    atomic_store_explicit(&e->key[1], k1, memory_order_relaxed);
    for (size_t i = 0; i < words; i++) atomic_store_explicit(&e->pal[i], pal[i], memory_order_relaxed);
    atomic_store_explicit(&e->seq, s + 2, memory_order_release);
}

/* The two pixels of coverage byte v at p; SHR_SCALAR_BLEND skips v = 0, whose pixels hold pal[0], the FILL. */
static inline __attribute__((always_inline)) void pal_pair(uint8_t *p, const uint8_t *pal, uint32_t v, size_t bpp) {
    if (SHR_SCALAR_BLEND && !v) return;
    __builtin_memcpy(p, pal + (v >> 4) * bpp, bpp);
    __builtin_memcpy(p + bpp, pal + (v & 15) * bpp, bpp);
}

/* A plain A4 GLYPH over pixels that all hold colour `bg`: 16 results, looked up per pixel, a coverage byte at a time. */
static inline __attribute__((always_inline)) void glyph_on_rows(const shr_surface *dst, const shr_draw_cmd *c,
                                                                const shr_image *b, shr_rect r, shr_point s,
                                                                shr_color bg, shr_pixel_format f,
                                                                shr__pal_memo *memo) {
    size_t bpp = shr__px_bytes(f), words = 16 * bpp / 4;
    uint32_t dim = (c->flags & SHR_GLYPH_DIM) != 0, local[16];
    uint32_t k0 = (c->color & 0xFFFFFFu) | dim << 24 | (uint32_t)(f == SHR_FORMAT_RGB565) << 25 | 1u << 26;
    uint32_t k1 = bg & 0xFFFFFFu, x = pal_hash(k0, k1), m = x >> 28;
    uint32_t *pal32 = memo ? memo->pal[m] : local;
    uint8_t *pal = (uint8_t *)pal32;
    shr__pal_set *set = &shr__pals[(x >> 16) % SHR__PAL_SETS];
    bool held = memo && memo->key[m][0] == k0 && memo->key[m][1] == k1;
    /* shr__pals kept for SHR_SCALAR_BLEND: the SIMD builds compute a palette faster than they look one up */
    if (!held && (!SHR_SCALAR_BLEND || !pal_get(set, k0, k1, pal32, words))) {
        rgb fg = color_rgb(c->color);
        uint8_t px[4];
        write_px(f, px, color_rgb(bg));
        uint32_t ends = SHR_SCALAR_BLEND; /* 0 and 15 without DIM: `bg` and the colour, stored below */
        if (f == SHR_FORMAT_RGB565) {
            uint16_t v, out[16];
            __builtin_memcpy(&v, px, 2);
            for (uint32_t n = ends; n < 16 - (ends & !dim); n++) {
                uint32_t a = (17 * n + dim) >> dim, wa = 255 * (255 - a);
                out[n] = (uint16_t)(blend565(fg.r, v >> 11, a, wa, 31) << 11 |
                                    blend565(fg.g, (v >> 5) & 63, a, wa, 63) << 5 | blend565(fg.b, v & 31, a, wa, 31));
            }
            __builtin_memcpy(pal, out, sizeof(out));
        } else {
            for (uint32_t n = ends; n < 16 - (ends & !dim); n++) {
                uint32_t a = (17 * n + dim) >> dim;
                pal[4 * n] = (uint8_t)blend8(fg.r, px[0], a), pal[4 * n + 1] = (uint8_t)blend8(fg.g, px[1], a);
                pal[4 * n + 2] = (uint8_t)blend8(fg.b, px[2], a), pal[4 * n + 3] = px[3];
            }
        }
        if (ends) {
            __builtin_memcpy(pal, px, bpp);
            if (!dim) write_px(f, pal + 15 * bpp, fg);
        }
        if (SHR_SCALAR_BLEND) pal_put(set, k0, k1, pal32, words);
    }
    if (memo) memo->key[m][0] = k0, memo->key[m][1] = k1;
    int32_t x0 = c->src_rect.x0 + s.x, w = r.x1 - r.x0;
    size_t bs = b->stride, ds = dst->stride; /* the stores below may alias them */
    const uint8_t *row = region_row(b, c->src_rect, s.y) + x0 / 2;
    uint8_t *line = pixel_at(dst, r.x0, r.y0);
    for (int32_t y = r.y0; y < r.y1; y++, row += bs, line += ds) {
        const uint8_t *q = row;
        uint8_t *p = line;
        int32_t k = w;
        if (x0 & 1) __builtin_memcpy(p, pal + (*q++ & 15) * bpp, bpp), p += bpp, k--;
        const uint8_t *end = q + k / 2; /* q and p alone step: gcc otherwise recomputes them after the loop */
        for (; end - q >= 4; q += 4, p += 8 * bpp) { /* unrolled: gcc -O2 does not */
            pal_pair(p, pal, q[0], bpp), pal_pair(p + 2 * bpp, pal, q[1], bpp);
            pal_pair(p + 4 * bpp, pal, q[2], bpp), pal_pair(p + 6 * bpp, pal, q[3], bpp);
        }
        if (end - q >= 2) pal_pair(p, pal, q[0], bpp), pal_pair(p + 2 * bpp, pal, q[1], bpp), q += 2, p += 4 * bpp;
        if (q < end) pal_pair(p, pal, *q++, bpp), p += 2 * bpp;
        if (k & 1) __builtin_memcpy(p, pal + (*q >> 4) * bpp, bpp);
    }
}

static void do_glyph_on(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *b, shr_rect r, shr_point s,
                        shr_color bg, shr__pal_memo *memo) {
    if (dst->format == SHR_FORMAT_RGB565)
        glyph_on_rows(dst, c, b, r, s, bg, SHR_FORMAT_RGB565, memo);
    else
        glyph_on_rows(dst, c, b, r, s, bg, SHR_FORMAT_RGBX8888, memo);
}

/* The colour of RGBA pixel v at full alpha. */
static inline __attribute__((always_inline)) void opaque_px(shr_pixel_format f, uint8_t *p, uint32_t v) {
    v |= 0xFF000000u;
    if (f == SHR_FORMAT_RGB565)
        write_px(f, p, (rgb){v & 255, v >> 8 & 255, v >> 16 & 255});
    else
        __builtin_memcpy(p, &v, 4);
}

static inline __attribute__((always_inline)) void image_px(shr_pixel_format f, uint8_t *p, uint32_t v) {
    if (v >= 0xFF000000u)
        opaque_px(f, p, v);
    else if (v >> 24)
        blend_px(f, p, (rgb){v & 255, v >> 8 & 255, v >> 16 & 255}, v >> 24);
}

/* In groups of 4 pixels: a group of all alpha 0 is skipped and one of all 255 stored as is. */
static inline __attribute__((always_inline)) void image_rows(const shr_surface *dst, const shr_draw_cmd *c,
                                                             const shr_image *b, shr_rect r, shr_point s,
                                                             shr_pixel_format f) {
    size_t bpp = shr__px_bytes(f);
    int32_t n = r.x1 - r.x0;
    const uint8_t *row = region_row(b, c->src_rect, s.y) + (size_t)(c->src_rect.x0 + s.x) * 4;
    uint8_t *line = pixel_at(dst, r.x0, r.y0);
    for (int32_t y = r.y0; y < r.y1; y++, row += b->stride, line += dst->stride) {
        const uint8_t *q = row;
        uint8_t *p = line;
        int32_t x = 0;
        for (; x + 4 <= n; x += 4, p += 4 * bpp, q += 16) {
            uint32_t v0, v1, v2, v3;
            __builtin_memcpy(&v0, q, 4), __builtin_memcpy(&v1, q + 4, 4);
            __builtin_memcpy(&v2, q + 8, 4), __builtin_memcpy(&v3, q + 12, 4);
            if ((v0 | v1 | v2 | v3) < 0x01000000u) continue;
            if ((v0 & v1 & v2 & v3) >= 0xFF000000u) {
                opaque_px(f, p, v0), opaque_px(f, p + bpp, v1);
                opaque_px(f, p + 2 * bpp, v2), opaque_px(f, p + 3 * bpp, v3);
            } else {
                image_px(f, p, v0), image_px(f, p + bpp, v1);
                image_px(f, p + 2 * bpp, v2), image_px(f, p + 3 * bpp, v3);
            }
        }
        for (; x < n; x++, p += bpp, q += 4) {
            uint32_t v;
            __builtin_memcpy(&v, q, 4);
            image_px(f, p, v);
        }
    }
}

static void do_image(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *b, shr_rect r, shr_point s) {
    if (dst->format == SHR_FORMAT_RGB565)
        image_rows(dst, c, b, r, s, SHR_FORMAT_RGB565);
    else
        image_rows(dst, c, b, r, s, SHR_FORMAT_RGBX8888);
}

/* A SCALED IMAGE: the scaled pixels a SPAN at a time, blended as image_px() blends. */
static inline __attribute__((always_inline)) void scaled_rows(const shr_surface *dst, const shr_draw_cmd *c,
                                                              const shr_image *b, shr_rect r, shr_point s,
                                                              shr_pixel_format f) {
    shr__scale sc = {.pixels = b->pixels, .stride = b->stride, .w = b->width, .h = b->height,
                     .format = SHR_IMAGE_SRC_RGBA8888, .src = c->src_rect, .dw = c->scale_w, .dh = c->scale_h,
                     .filter = SHR_SCALE_BILINEAR};
    size_t bpp = shr__px_bytes(f);
    uint8_t px[SPAN * 4];
    for (int32_t y = r.y0; y < r.y1; y++) {
        uint8_t *line = pixel_at(dst, r.x0, y);
        for (int32_t x = 0; x < r.x1 - r.x0; x += SPAN) {
            int32_t n = r.x1 - r.x0 - x < SPAN ? r.x1 - r.x0 - x : SPAN;
            shr__scale_row(&sc, s.y + (y - r.y0), s.x + x, n, px);
            for (int32_t k = 0; k < n; k++) {
                uint32_t v;
                __builtin_memcpy(&v, px + 4 * k, 4);
                image_px(f, line + (size_t)(x + k) * bpp, v);
            }
        }
    }
}

static void do_image_scaled(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *b, shr_rect r,
                            shr_point s) {
    if (dst->format == SHR_FORMAT_RGB565)
        scaled_rows(dst, c, b, r, s, SHR_FORMAT_RGB565);
    else
        scaled_rows(dst, c, b, r, s, SHR_FORMAT_RGBX8888);
}

void shr__raster_prep(uint32_t *plane, const shr_image *b, shr_rect r) SHR_NONBLOCKING {
    int32_t *lims = (int32_t *)(plane + (size_t)b->width * (size_t)b->height * 2);
    for (int32_t y = r.y0; y < r.y1; y++) {
        const uint8_t *q = (const uint8_t *)b->pixels + (size_t)y * b->stride;
        uint32_t *o = plane + (size_t)y * (size_t)b->width * 2;
        for (int32_t x = r.x0; x < r.x1; x++) {
            uint32_t a = q[4 * x + 3];
            o[2 * x] = ((31 * q[4 * x] * a + 32512) / 255 + 1) << 16 | ((31 * q[4 * x + 2] * a + 32512) / 255 + 1);
            o[2 * x + 1] = ((63 * q[4 * x + 1] * a + 32512) / 255 + 1) << 8 | (255 - a);
        }
        int32_t x0 = 0, x1 = b->width;
        while (x0 < x1 && (o[2 * x0 + 1] & 255) == 255) x0++;
        while (x1 > x0 && (o[2 * x1 - 1] & 255) == 255) x1--;
        lims[2 * y] = x0, lims[2 * y + 1] = x1;
    }
}

/* Red and blue go side by side in one word; each (n - 1) / 255 as (n + (n >> 8)) >> 8, exact for 0 < n < 65536. */
void shr__raster_image(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *b, const uint32_t *plane,
                       shr_point origin, shr_rect clip) SHR_NONBLOCKING {
    shr_rect d = {c->dst.x0 - origin.x, c->dst.y0 - origin.y, c->dst.x1 - origin.x, c->dst.y1 - origin.y};
    shr_rect r = shr__rect_intersect(d, clip);
    if (shr__rect_empty(r)) return;
    int32_t bx = c->src_rect.x0 + c->src_origin.x + (r.x0 - d.x0), w = r.x1 - r.x0;
    int32_t by = c->src_rect.y0 + c->src_origin.y + (r.y0 - d.y0);
    const int32_t *lims = (const int32_t *)(plane + (size_t)b->width * (size_t)b->height * 2);
    for (int32_t y = 0; y < r.y1 - r.y0; y++) {
        int32_t row = by + y, x0 = lims[2 * row] > bx ? lims[2 * row] : bx;
        int32_t x1 = lims[2 * row + 1] < bx + w ? lims[2 * row + 1] : bx + w;
        if (x0 >= x1) continue;
        const uint32_t *q = plane + ((size_t)row * (size_t)b->width + (size_t)x0) * 2;
        uint8_t *p = pixel_at(dst, r.x0 + (x0 - bx), r.y0 + y), *end = p + (size_t)(x1 - x0) * 2;
        for (; p < end; q += 2, p += 2) {
            uint16_t v;
            __builtin_memcpy(&v, p, 2);
            uint32_t inv = q[1] & 255, rb = q[0] + ((v | (uint32_t)v << 5) & 0x001F001Fu) * inv;
            uint32_t g = (q[1] >> 8) + (v >> 5 & 63u) * inv;
            rb = (rb + (rb >> 8 & 0x00FF00FFu)) >> 8 & 0x001F001Fu;
            v = (uint16_t)(rb | rb >> 5 | ((g + (g >> 8)) >> 3 & 0x7E0u));
            __builtin_memcpy(p, &v, 2);
        }
    }
}

static void do_copy(const shr_surface *dst, const shr_image_ref *m, shr_rect r, shr_point s) {
    int32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    shr_pixel_format sf = (shr_pixel_format)m->format;
    size_t sbpp = shr__px_bytes(sf), dbpp = shr__px_bytes(dst->format);
    /* Overlapping copies share format and stride. A source below the destination is copied in runs of the rows
     * between them, from the last run on, each run forward: no row read is one a run wrote. */
    uintptr_t from = copy_src_span(m, s, r).begin, to = dst_span(dst, r).begin;
    size_t apart = to > from ? (to - from) / dst->stride : (size_t)h;
    int32_t run = apart < 1 ? 1 : apart < (size_t)h ? (int32_t)apart : h;
    for (int32_t end = h; end > 0; end -= run)
        for (int32_t row = end > run ? end - run : 0; row < end; row++) {
            const uint8_t *q = src_at(m, s.x, s.y + row);
            uint8_t *d = pixel_at(dst, r.x0, r.y0 + row);
            if (sf == dst->format) {
                memmove(d, q, (size_t)w * dbpp);
            } else {
                for (int32_t x = 0; x < w; x++)
                    write_px(dst->format, d + (size_t)x * dbpp, read_px(sf, q + (size_t)x * sbpp));
            }
        }
}

#define TILE 32

/* Output pixel (x, y) of `dst` takes the source pixel that maps onto it; in tiles, so source rows stay in cache. */
static void do_rotate(const shr_surface *dst, const shr_draw_cmd *c, shr_rect r) {
    const shr_image_ref *m = &c->src;
    shr_pixel_format sf = (shr_pixel_format)m->format;
    size_t bpp = shr__px_bytes(dst->format), sbpp = shr__px_bytes(sf);
    ptrdiff_t step = c->rotation == SHR_ROTATE_90_CW    ? -(ptrdiff_t)m->stride
                     : c->rotation == SHR_ROTATE_90_CCW ? (ptrdiff_t)m->stride
                                                        : -(ptrdiff_t)sbpp;
    for (int32_t ty = r.y0; ty < r.y1; ty += TILE)
        for (int32_t tx = r.x0; tx < r.x1; tx += TILE)
            for (int32_t y = ty; y < r.y1 && y < ty + TILE; y++) {
                int32_t u = tx - c->dst.x0, v = y - c->dst.y0, sx, sy;
                switch (c->rotation) {
                case SHR_ROTATE_90_CW: sx = v, sy = m->height - 1 - u; break;
                case SHR_ROTATE_90_CCW: sx = m->width - 1 - v, sy = u; break;
                default: sx = m->width - 1 - u, sy = m->height - 1 - v; break;
                }
                const uint8_t *q = src_at(m, sx, sy);
                uint8_t *p = pixel_at(dst, tx, y);
                for (int32_t k = 0; k < r.x1 - tx && k < TILE; k++, p += bpp) {
                    if (sf == dst->format) memcpy(p, q + k * step, bpp);
                    else write_px(dst->format, p, read_px(sf, q + k * step));
                }
            }
}

void shr__raster_draw(const shr_surface *dst, const shr_draw_cmd *c, const shr_image *buffers, shr_point origin,
                      shr_rect clip, shr__pal_memo *memo) SHR_NONBLOCKING {
    shr_rect d = {c->dst.x0 - origin.x, c->dst.y0 - origin.y, c->dst.x1 - origin.x, c->dst.y1 - origin.y};
    shr_rect r = shr__rect_intersect(d, clip);
    if (shr__rect_empty(r)) return;
    if (c->kind == SHR_CMD_FILL) {
        do_fill(dst, r, c->color, (c->flags & SHR_GLYPH_DIM) != 0);
        return;
    }
    if (c->kind == SHR_CMD_ROTATE) {
        do_rotate(dst, c, r);
        return;
    }
    if (c->kind == SHR_CMD_LINE) {
        shr__raster_line(dst, c, origin, clip, NULL);
        return;
    }
    shr_point s = {c->src_origin.x + (r.x0 - d.x0), c->src_origin.y + (r.y0 - d.y0)};
    switch (c->kind) {
    case SHR_CMD_GLYPH: /* ON_FILL only for plain A4: blending the others onto `bg` is no faster than reading dst */
        if ((c->flags & (SHR_GLYPH_ON_FILL | SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC)) == SHR_GLYPH_ON_FILL &&
            buffers[c->buffer - 1].format == SHR_FORMAT_A4)
            do_glyph_on(dst, c, &buffers[c->buffer - 1], r, s, c->bg, memo);
        else
            do_glyph(dst, c, &buffers[c->buffer - 1], r, s);
        break;
    case SHR_CMD_IMAGE:
        if (c->flags & SHR_IMAGE_SCALED)
            do_image_scaled(dst, c, &buffers[c->buffer - 1], r, s);
        else
            do_image(dst, c, &buffers[c->buffer - 1], r, s);
        break;
    default: do_copy(dst, &c->src, r, s); break;
    }
}
