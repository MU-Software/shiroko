/* Scaled images: the filters against the reference (shr_scale_ref.h), scaled IMAGE draws of the software driver
 * (planes, parts), copies against driver-scaled views through the compositor, views' lifetime and updates. */
#include "harness.h"
#include "shr_compositor.h"
#include "shr_scale.h"
#include "shr_scale_ref.h"

static uint32_t seed = 12345;
static uint32_t rnd(void) { return seed = (uint32_t)((uint64_t)seed * 1103515245u + 12345u), seed >> 8; }

typedef struct kase {
    int32_t w, h, sx, sy, sw, sh, dw, dh;
} kase;

/* viu, icat in tmux, yazi pieces (top, middle, bottom row of a 472x295 image), then random ones. */
static const kase fixed[] = {{512, 512, 0, 0, 512, 512, 720, 704}, {600, 590, 0, 0, 600, 590, 480, 464},
                             {640, 400, 0, 0, 640, 400, 1152, 704}, {512, 512, 0, 0, 512, 512, 400, 400},
                             {600, 590, 0, 0, 600, 590, 600, 592}, {472, 295, 248, 0, 8, 15, 8, 16},
                             {472, 295, 272, 124, 8, 15, 8, 16}, {472, 295, 464, 279, 8, 16, 8, 16},
                             {472, 295, 0, 15, 8, 16, 8, 16}, {472, 295, 8, 280, 8, 15, 8, 16}};
#define NFIXED (int)(sizeof(fixed) / sizeof(fixed[0]))
#define NCASE (NFIXED + 30)

static kase case_at(int k) {
    if (k < NFIXED) return fixed[k];
    kase c;
    c.w = 1 + (int32_t)(rnd() % 70), c.h = 1 + (int32_t)(rnd() % 70);
    c.sw = 1 + (int32_t)(rnd() % (uint32_t)c.w), c.sh = 1 + (int32_t)(rnd() % (uint32_t)c.h);
    c.sx = (int32_t)(rnd() % (uint32_t)(c.w - c.sw + 1)), c.sy = (int32_t)(rnd() % (uint32_t)(c.h - c.sh + 1));
    c.dw = 1 + (int32_t)(rnd() % 90), c.dh = 1 + (int32_t)(rnd() % 90);
    return c;
}

static uint8_t *image(const kase *c, bool opaque) {
    uint8_t *p = malloc((size_t)c->w * (size_t)c->h * 4);
    for (size_t i = 0; i < (size_t)c->w * (size_t)c->h * 4; i++) p[i] = (uint8_t)rnd();
    for (size_t i = 0; opaque && i < (size_t)c->w * (size_t)c->h; i++) p[4 * i + 3] = 255;
    return p;
}

static shr_rect src_of(const kase *c) { return (shr_rect){c->sx, c->sy, c->sx + c->sw, c->sy + c->sh}; }

static shr__scale scale_of(const kase *c, const uint8_t *p, size_t stride, uint32_t format, uint32_t filter) {
    return (shr__scale){.pixels = p, .stride = stride, .w = c->w, .h = c->h, .format = format, .src = src_of(c),
                        .dw = c->dw, .dh = c->dh, .filter = filter};
}

TEST rows_match_reference(void) {
    for (int k = 0; k < NCASE; k++) {
        kase c = case_at(k);
        uint8_t *p = image(&c, k % 3 == 0);
        uint8_t *row = malloc((size_t)c.dw * 4);
        shr_scale_src m = {p, (size_t)c.w * 4, c.w, c.h, c.sx, c.sy, c.sw, c.sh, c.dw, c.dh};
        for (uint32_t f = SHR_SCALE_BILINEAR; f <= SHR_SCALE_BOX; f++) {
            shr__scale s = scale_of(&c, p, (size_t)c.w * 4, SHR_IMAGE_SRC_RGBA8888, f);
            for (int32_t y = 0; y < c.dh; y++) {
                int32_t x0 = (int32_t)(rnd() % (uint32_t)c.dw), n = 1 + (int32_t)(rnd() % (uint32_t)(c.dw - x0));
                shr__scale_row(&s, y, 0, c.dw, row);
                shr__scale_row(&s, y, x0, n, row + (size_t)x0 * 4); /* a part again, in place */
                for (int32_t x = 0; x < c.dw; x++) {
                    uint8_t want[4];
                    shr_scale_px(&m, f == SHR_SCALE_BOX ? SHR_FILTER_BOX : SHR_FILTER_BILINEAR, x, y, want);
                    if (memcmp(want, row + 4 * x, 4)) {
                        fprintf(stderr, "case %d filter %u (%d, %d)\n", k, f, x, y);
                        FAIL();
                    }
                }
            }
        }
        free(row), free(p);
    }
    PASS();
}

/* RGB, gray and gray-alpha rows (padded) scale as the RGBA rows they widen to. */
TEST sources_scale_as_rgba(void) {
    for (int k = NFIXED; k < NCASE; k++) {
        kase c = case_at(k);
        uint8_t *rgba = image(&c, false), *row = malloc((size_t)c.dw * 4), *want = malloc((size_t)c.dw * 4);
        for (uint32_t fmt = SHR_IMAGE_SRC_RGB888; fmt <= SHR_IMAGE_SRC_GRAY_ALPHA88; fmt++) {
            size_t bpp = shr__src_bytes(fmt), stride = (size_t)c.w * bpp + 3;
            uint8_t *src = malloc(stride * (size_t)c.h), *wide = malloc((size_t)c.w * (size_t)c.h * 4);
            for (size_t i = 0; i < stride * (size_t)c.h; i++) src[i] = (uint8_t)rnd();
            for (int32_t y = 0; y < c.h; y++)
                for (int32_t x = 0; x < c.w; x++)
                    shr_src_widen(src + (size_t)y * stride + (size_t)x * bpp, bpp, wide + ((size_t)y * c.w + x) * 4);
            for (uint32_t f = SHR_SCALE_BILINEAR; f <= SHR_SCALE_BOX; f++) {
                shr__scale s = scale_of(&c, src, stride, fmt, f), r = scale_of(&c, wide, (size_t)c.w * 4, 0, f);
                for (int32_t y = 0; y < c.dh; y++) {
                    shr__scale_row(&s, y, 0, c.dw, row), shr__scale_row(&r, y, 0, c.dw, want);
                    ASSERT_MEM_EQ(want, row, (size_t)c.dw * 4);
                }
            }
            free(src), free(wide);
        }
        free(rgba), free(row), free(want);
    }
    const kase one = {1, 1, 0, 0, 1, 1, 1, 1}; /* 1 x 1 unscaled: the pixel itself */
    uint8_t g[2] = {7, 99}, out[4];
    shr__scale s = scale_of(&one, g, 2, SHR_IMAGE_SRC_GRAY_ALPHA88, SHR_SCALE_BILINEAR);
    shr__scale_row(&s, 0, 0, 1, out);
    ASSERT(out[0] == 7 && out[1] == 7 && out[2] == 7 && out[3] == 99);
    s.format = SHR_IMAGE_SRC_GRAY8;
    shr__scale_row(&s, 0, 0, 1, out);
    ASSERT(out[0] == 7 && out[2] == 7 && out[3] == 255);
    PASS();
}

/* Columns past 2^23 read their own pixels (taps count from the first source column of their span). */
TEST wide_sources_read_their_columns(void) {
    enum { FAR = 1 << 23 };
    const int32_t w = FAR + 100;
    uint8_t *p = calloc((size_t)w, 1), out[10 * 4];
    ASSERT(p != NULL);
    for (int32_t j = 0; j < 100; j++) p[FAR + j] = (uint8_t)(1 + 2 * j);
    shr__scale s = {.pixels = p, .stride = (size_t)w, .w = w, .h = 1, .format = SHR_IMAGE_SRC_GRAY8,
                    .src = {FAR + 10, 0, FAR + 20, 1}, .dw = 10, .dh = 1, .filter = SHR_SCALE_BILINEAR};
    shr__scale_row(&s, 0, 0, 10, out);
    for (int i = 0; i < 10; i++) ASSERT_EQ_LL(out[4 * i], 21 + 2 * i);
    memset(out, 0, sizeof(out));
    shr__scale_rows(&s, out, sizeof(out));
    for (int i = 0; i < 10; i++) ASSERT_EQ_LL(out[4 * i], 21 + 2 * i);
    free(p);
    PASS();
}

/* Whole copies, which keep each source row's horizontal blend while the rows below read it, in every source format. */
TEST copies_match_reference(void) {
    for (int k = 0; k < NCASE; k++) {
        kase c = case_at(k);
        uint8_t *rgba = image(&c, k % 3 == 0), *out = malloc((size_t)c.dw * (size_t)c.dh * 4);
        for (uint32_t fmt = SHR_IMAGE_SRC_RGBA8888; fmt <= SHR_IMAGE_SRC_GRAY_ALPHA88; fmt++) {
            size_t bpp = shr__src_bytes(fmt), stride = (size_t)c.w * bpp + 1;
            uint8_t *src = malloc(stride * (size_t)c.h), *wide = malloc((size_t)c.w * (size_t)c.h * 4);
            for (int32_t y = 0; y < c.h; y++) {
                memcpy(src + (size_t)y * stride, rgba + (size_t)y * c.w * bpp, (size_t)c.w * bpp);
                for (int32_t x = 0; x < c.w; x++)
                    shr_src_widen(src + (size_t)y * stride + (size_t)x * bpp, bpp, wide + ((size_t)y * c.w + x) * 4);
            }
            shr_scale_src m = {wide, (size_t)c.w * 4, c.w, c.h, c.sx, c.sy, c.sw, c.sh, c.dw, c.dh};
            for (uint32_t f = SHR_SCALE_BILINEAR; f <= SHR_SCALE_BOX; f++) {
                shr__scale s = scale_of(&c, src, stride, fmt, f);
                shr__scale_rows(&s, out, (size_t)c.dw * 4);
                for (int32_t y = 0; y < c.dh; y++)
                    for (int32_t x = 0; x < c.dw; x++) {
                        uint8_t want[4];
                        shr_scale_px(&m, f == SHR_SCALE_BOX ? SHR_FILTER_BOX : SHR_FILTER_BILINEAR, x, y, want);
                        if (memcmp(want, out + ((size_t)y * (size_t)c.dw + (size_t)x) * 4, 4)) {
                            fprintf(stderr, "case %d format %u filter %u (%d, %d)\n", k, fmt, f, x, y);
                            FAIL();
                        }
                    }
            }
            free(src), free(wide);
        }
        free(out), free(rgba);
    }
    PASS();
}

static shr_draw_cmd scaled_cmd(const kase *c, uint32_t id, shr_rect dst, shr_point at) {
    shr_draw_cmd d = {0};
    d.kind = SHR_CMD_IMAGE, d.flags = SHR_IMAGE_SCALED;
    d.buffer = id, d.dst = dst, d.src_origin = (shr_point){dst.x0 - at.x, dst.y0 - at.y};
    d.src_rect = src_of(c), d.scale_w = c->dw, d.scale_h = c->dh;
    return d;
}

/* The scaled image at `at` of a W x H surface, whole, then in random parts (bands, damage), by the stateless
 * software execute and by a driver with planes: each equal to the reference blend. */
TEST draws_match_reference_in_parts(void) {
    enum { W = 100, H = 100 };
    static uint8_t bg[W * H * 4], out[W * H * 4];
    for (int k = 0; k < NCASE; k++) {
        kase c = case_at(k);
        if (c.dw > 90 || c.dh > 90) continue;
        uint8_t *p = image(&c, k % 3 == 0);
        shr_image b = {p, c.w, c.h, (size_t)c.w * 4, (size_t)c.w * (size_t)c.h * 4, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU};
        shr_scale_src m = {p, (size_t)c.w * 4, c.w, c.h, c.sx, c.sy, c.sw, c.sh, c.dw, c.dh};
        shr_point at = {(int32_t)(rnd() % 20) - 10, (int32_t)(rnd() % 20) - 10};
        shr_rect whole = shr__rect_intersect((shr_rect){at.x, at.y, at.x + c.dw, at.y + c.dh}, (shr_rect){0, 0, W, H});
        if (shr__rect_empty(whole)) {
            free(p);
            continue;
        }
        for (int fi = 0; fi < 2; fi++) {
            shr_pixel_format fmt = fi ? SHR_FORMAT_RGBX8888 : SHR_FORMAT_RGB565;
            size_t bpp = fi ? 4 : 2;
            shr_surface s = {out, W, H, W * bpp, sizeof(out), fmt, 1, SHR_MEMORY_CPU, 0};
            for (size_t i = 0; i < sizeof(bg); i++) bg[i] = (uint8_t)rnd();
            for (size_t i = 0; fi && i < W * H; i++) bg[4 * i + 3] = 255;
            for (int way = 0; way < 3; way++) {
                memcpy(out, bg, sizeof(out));
                shr_draw_cmd cmds[17];
                size_t n = 0;
                if (way == 0) {
                    cmds[n++] = scaled_cmd(&c, 1, whole, at);
                } else { /* a grid of up to 4 x 4 random parts */
                    int32_t xs[5] = {whole.x0, 0, 0, 0, whole.x1}, ys[5] = {whole.y0, 0, 0, 0, whole.y1};
                    for (int i = 1; i < 4; i++) {
                        xs[i] = whole.x0 + (int32_t)(rnd() % (uint32_t)(whole.x1 - whole.x0 + 1));
                        ys[i] = whole.y0 + (int32_t)(rnd() % (uint32_t)(whole.y1 - whole.y0 + 1));
                    }
                    for (int i = 1; i < 4; i++) /* sorted */
                        for (int j = i; j > 1 && xs[j] < xs[j - 1]; j--) {
                            int32_t t = xs[j];
                            xs[j] = xs[j - 1], xs[j - 1] = t;
                        }
                    for (int i = 1; i < 4; i++)
                        for (int j = i; j > 1 && ys[j] < ys[j - 1]; j--) {
                            int32_t t = ys[j];
                            ys[j] = ys[j - 1], ys[j - 1] = t;
                        }
                    for (int i = 0; i < 4; i++)
                        for (int j = 0; j < 4; j++)
                            if (xs[i] < xs[i + 1] && ys[j] < ys[j + 1])
                                cmds[n++] = scaled_cmd(&c, 1, (shr_rect){xs[i], ys[j], xs[i + 1], ys[j + 1]}, at);
                }
                if (way < 2) {
                    shr_image bufs[1] = {b};
                    ASSERT_EQ_LL(shr_software_execute(&s, cmds, n, bufs, 1), SHR_OK);
                } else {
                    shr_framebuffer_driver drv;
                    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 0, 1, &drv), SHR_OK);
                    ASSERT(drv.caps.flags & SHR_DRIVER_SCALE);
                    shr_draw_cmd all[18] = {{0}};
                    all[0].kind = SHR_CMD_BUFFER_REGISTER, all[0].buffer = 1, all[0].src = img_ref(b);
                    memcpy(all + 1, cmds, n * sizeof(*cmds));
                    ASSERT_EQ_LL(drv.execute(drv.user, &s, all, n + 1, 1), SHR_OK);
                    ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
                }
                for (int32_t y = 0; y < H; y++)
                    for (int32_t x = 0; x < W; x++) {
                        const uint8_t *q = bg + ((size_t)y * W + (size_t)x) * bpp;
                        const uint8_t *got = out + ((size_t)y * W + (size_t)x) * bpp;
                        uint8_t d[4];
                        memcpy(d, q, 4);
                        if (x >= whole.x0 && x < whole.x1 && y >= whole.y0 && y < whole.y1) {
                            uint8_t f4[4];
                            shr_scale_px(&m, SHR_FILTER_BILINEAR, x - at.x, y - at.y, f4);
                            if (fi) {
                                shr_blend8888(d, f4);
                            } else {
                                uint16_t v;
                                memcpy(&v, q, 2);
                                v = shr_blend565(v, f4);
                                memcpy(d, &v, 2);
                            }
                        }
                        if (memcmp(d, got, bpp)) {
                            fprintf(stderr, "case %d fmt %d way %d (%d, %d)\n", k, fi, way, x, y);
                            FAIL();
                        }
                    }
            }
        }
        free(p);
    }
    PASS();
}

/* An opaque image kept as RGB565: rows, whole copies and scaled draws (in two parts, both screen formats, stateless
 * and through a driver) equal the reference on its widened pixels. */
TEST rgb565_scales_widened(void) {
    enum { W = 100, H = 100 };
    static uint8_t bg[W * H * 4], out[2][W * H * 4];
    for (int k = 0; k < NCASE; k++) {
        kase c = case_at(k);
        uint16_t *p = malloc((size_t)c.w * (size_t)c.h * 2);
        for (size_t i = 0; i < (size_t)c.w * (size_t)c.h; i++) p[i] = (uint16_t)rnd();
        uint8_t *wide = malloc((size_t)c.w * (size_t)c.h * 4), *copy = malloc((size_t)c.dw * (size_t)c.dh * 4);
        uint8_t *row = malloc((size_t)c.dw * 4);
        shr_rgb565_widen(p, (size_t)c.w * 2, c.w, c.h, wide);
        shr_scale_src m = {wide, (size_t)c.w * 4, c.w, c.h, c.sx, c.sy, c.sw, c.sh, c.dw, c.dh};
        for (uint32_t f = SHR_SCALE_BILINEAR; f <= SHR_SCALE_BOX; f++) {
            shr__scale s = scale_of(&c, (const uint8_t *)p, (size_t)c.w * 2, SHR__SRC_RGB565, f);
            shr__scale_rows(&s, copy, (size_t)c.dw * 4);
            for (int32_t y = 0; y < c.dh; y++) {
                shr__scale_row(&s, y, 0, c.dw, row);
                for (int32_t x = 0; x < c.dw; x++) {
                    uint8_t want[4];
                    shr_scale_px(&m, f == SHR_SCALE_BOX ? SHR_FILTER_BOX : SHR_FILTER_BILINEAR, x, y, want);
                    if (memcmp(want, copy + ((size_t)y * (size_t)c.dw + (size_t)x) * 4, 4) || memcmp(want, row + 4 * x, 4)) {
                        fprintf(stderr, "case %d filter %u (%d, %d)\n", k, f, x, y);
                        FAIL();
                    }
                }
            }
        }
        shr_point at = {(int32_t)(rnd() % 20) - 10, (int32_t)(rnd() % 20) - 10};
        shr_rect whole = shr__rect_intersect((shr_rect){at.x, at.y, at.x + c.dw, at.y + c.dh}, (shr_rect){0, 0, W, H});
        shr_image b = {p, c.w, c.h, (size_t)c.w * 2, (size_t)c.w * (size_t)c.h * 2, SHR_FORMAT_RGB565, SHR_MEMORY_CPU};
        for (int fi = 0; fi < 2 && !shr__rect_empty(whole); fi++) {
            shr_pixel_format fmt = fi ? SHR_FORMAT_RGBX8888 : SHR_FORMAT_RGB565;
            size_t bpp = fi ? 4 : 2;
            for (size_t i = 0; i < sizeof(bg); i++) bg[i] = (uint8_t)rnd();
            int32_t mid = whole.y0 + (whole.y1 - whole.y0) / 2;
            shr_draw_cmd all[3] = {{0}};
            all[0].kind = SHR_CMD_BUFFER_REGISTER, all[0].buffer = 1, all[0].src = img_ref(b);
            all[1] = scaled_cmd(&c, 1, (shr_rect){whole.x0, whole.y0, whole.x1, mid}, at);
            all[2] = scaled_cmd(&c, 1, (shr_rect){whole.x0, mid, whole.x1, whole.y1}, at);
            memcpy(out[0], bg, sizeof(bg)), memcpy(out[1], bg, sizeof(bg));
            shr_surface s0 = {out[0], W, H, W * bpp, sizeof(out[0]), fmt, 1, SHR_MEMORY_CPU, 0}, s1 = s0;
            s1.pixels = out[1];
            shr_image bufs[1] = {b};
            ASSERT_EQ_LL(shr_software_execute(&s0, all + 1, 2, bufs, 1), SHR_OK);
            shr_framebuffer_driver drv;
            ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 0, 1, &drv), SHR_OK);
            ASSERT_EQ_LL(drv.execute(drv.user, &s1, all, 3, 1), SHR_OK);
            ASSERT_EQ_LL(shr_software_driver_destroy(&drv), SHR_OK);
            for (int32_t y = 0; y < H; y++)
                for (int32_t x = 0; x < W; x++) {
                    size_t o = ((size_t)y * W + (size_t)x) * bpp;
                    uint8_t d[4];
                    memcpy(d, bg + o, 4);
                    if (x >= whole.x0 && x < whole.x1 && y >= whole.y0 && y < whole.y1) {
                        uint8_t f4[4];
                        shr_scale_px(&m, SHR_FILTER_BILINEAR, x - at.x, y - at.y, f4);
                        if (fi) {
                            shr_blend8888(d, f4);
                        } else {
                            uint16_t v;
                            memcpy(&v, bg + o, 2);
                            v = shr_blend565(v, f4);
                            memcpy(d, &v, 2);
                        }
                    }
                    if (memcmp(d, out[0] + o, bpp) || memcmp(d, out[1] + o, bpp)) {
                        fprintf(stderr, "case %d fmt %d draw (%d, %d)\n", k, fi, x, y);
                        FAIL();
                    }
                }
        }
        free(row), free(copy), free(wide), free(p);
    }
    PASS();
}

/* Sizes and reads the driver checks. */
TEST scaled_draws_checked(void) {
    enum { W = 8, H = 8 };
    static uint8_t px[4 * 4 * 4], out[W * H * 4];
    shr_image b = {px, 4, 4, 16, sizeof(px), SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU};
    shr_surface s = {out, W, H, W * 4, sizeof(out), SHR_FORMAT_RGBX8888, 1, SHR_MEMORY_CPU, 0};
    kase c = {4, 4, 0, 0, 4, 4, 8, 8};
    struct {
        int32_t dw, dh, ox;
        shr_rect src;
        shr_status want;
    } t[] = {{8, 8, 0, {0, 0, 4, 4}, SHR_OK},           {8, 8, 1, {0, 0, 4, 4}, SHR_E_INVALID_ARG},
             {0, 8, 0, {0, 0, 4, 4}, SHR_E_INVALID_ARG}, {32768, 8, 0, {0, 0, 4, 4}, SHR_E_INVALID_ARG},
             {32767, 8, 0, {0, 0, 4, 4}, SHR_OK},       {8, 8, 0, {1, 1, 1, 4}, SHR_E_INVALID_ARG},
             {8, 8, 0, {0, 0, 5, 4}, SHR_E_INVALID_ARG}};
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        shr_draw_cmd d = scaled_cmd(&c, 1, (shr_rect){0, 0, W, H}, (shr_point){-t[i].ox, 0});
        d.scale_w = t[i].dw, d.scale_h = t[i].dh, d.src_rect = t[i].src;
        shr_image bufs[1] = {b};
        ASSERT_EQ_LL(shr_software_execute(&s, &d, 1, bufs, 1), t[i].want);
    }
    PASS();
}

static bool scale_driver;
static uint64_t budget;
static void tweak(shr_context_desc *d, shr_framebuffer_driver *drv) {
    if (budget) d->image_bytes = budget;
    if (scale_driver) drv->caps.flags |= SHR_DRIVER_SCALE | SHR_DRIVER_IMAGE_565;
}

static void frame(shr_context *ctx) {
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_pump(ctx), SHR_OK);
}

enum { MODE_COPY, MODE_VIEW_COPY, MODE_VIEW_DRIVER };

/* One frame of 4 placements of a random 37 x 23 image, partly off screen, over a fill, with two bands or none. */
static void composite(int mode, uint32_t filter, bool banded, uint8_t *shown) {
    static const kase pl[] = {{37, 23, 3, 2, 30, 19, 50, 41}, {37, 23, 0, 0, 37, 23, 20, 11},
                              {37, 23, 8, 0, 8, 15, 8, 16}, {37, 23, 0, 0, 37, 23, 37, 23}};
    harness h;
    scale_driver = mode == MODE_VIEW_DRIVER, budget = 0;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    static uint8_t band_px[2][HW * 24 * 4];
    shr_surface bands[2];
    if (banded) {
        for (int k = 0; k < 2; k++)
            bands[k] = (shr_surface){band_px[k], HW, 24, HW * SCREEN_BPP, HW * 24 * SCREEN_BPP, SHR_PIXEL_FORMAT, 1, 0, 0};
        shr_screen_desc sd;
        shr_screen_desc_init(&sd);
        sd.width = HW, sd.height = HH, sd.bands = bands, sd.band_count = 2;
        ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_OK);
    }
    seed = 777;
    kase c = pl[0];
    uint8_t *p = image(&c, false);
    shr_image_source source = {c.w, c.h, SHR_IMAGE_SRC_RGBA8888, p, (size_t)c.w * 4};
    shr_pl_res_image *base = NULL, *img[4];
    if (mode != MODE_COPY) ASSERT_EQ_LL(shr_pl_res_image_create(ctx, c.w, c.h, p, (size_t)c.w * 4, &base), SHR_OK);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_fill(l, (shr_rect){0, 0, HW, HH}, SHR_RGB(30, 90, 200)), SHR_OK);
    static const shr_point at[] = {{-7, 5}, {40, -3}, {50, 30}, {33, 40}};
    for (int i = 0; i < 4; i++) {
        if (mode == MODE_COPY)
            ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &source, src_of(&pl[i]), pl[i].dw, pl[i].dh, filter, &img[i]),
                         SHR_OK);
        else
            ASSERT_EQ_LL(shr_pl_res_image_view(base, src_of(&pl[i]), pl[i].dw, pl[i].dh,
                                               filter | (mode == MODE_VIEW_COPY ? SHR_SCALE_COPY : SHR_SCALE_DRIVER), &img[i]),
                         SHR_OK);
        /* cropped off the top or left in scaled pixels */
        shr_rect crop = {at[i].x < 0 ? -at[i].x : 0, at[i].y < 0 ? -at[i].y : 0, pl[i].dw, pl[i].dh};
        shr_point to = {at[i].x < 0 ? 0 : at[i].x, at[i].y < 0 ? 0 : at[i].y};
        ASSERT_EQ_LL(shr_lyr_cmd_image(l, img[i], crop, to), SHR_OK);
    }
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    if (base) ASSERT_EQ_LL(shr_pl_res_image_release(base), SHR_OK); /* views keep it */
    frame(ctx);
    memcpy(shown, h.out.shown, sizeof(h.out.shown));
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    for (int i = 0; i < 4; i++) ASSERT_EQ_LL(shr_pl_res_image_release(img[i]), SHR_OK);
    free(p);
    harness_close(&h);
}

TEST copies_equal_driver_views(void) {
    static uint8_t a[HW * HH * 4], b[HW * HH * 4];
    for (uint32_t f = SHR_SCALE_BILINEAR; f <= SHR_SCALE_BOX; f++)
        for (int banded = 0; banded < 2; banded++) {
            composite(MODE_COPY, f, banded, a);
            for (int mode = MODE_VIEW_COPY; mode <= (f == SHR_SCALE_BOX ? MODE_VIEW_COPY : MODE_VIEW_DRIVER); mode++) {
                composite(mode, f, banded, b);
                if (memcmp(a, b, sizeof(a))) {
                    fprintf(stderr, "filter %u banded %d mode %d\n", f, banded, mode);
                    FAIL();
                }
            }
        }
    PASS();
}

/* A driver view shows the base's updates: the frame after an update equals a fresh copy of the updated pixels. */
TEST views_follow_updates(void) {
    static const kase c = {29, 17, 2, 1, 25, 15, 61, 45};
    static uint8_t a[HW * HH * 4];
    uint8_t *p = image(&c, false), patch[3 * 2 * 4];
    for (size_t i = 0; i < sizeof(patch); i++) patch[i] = (uint8_t)rnd();
    static const shr_rect parts[] = {{0, 0, 3, 2}, {13, 7, 16, 9}, {26, 15, 29, 17}, {27, 0, 29, 1}};
    for (size_t k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
        shr_rect u = parts[k];
        for (int pass = 0; pass < 2; pass++) {
            harness h;
            scale_driver = pass == 0, budget = 0;
            shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT, tweak);
            shr_pl_res_image *base, *v;
            ASSERT_EQ_LL(shr_pl_res_image_create(ctx, c.w, c.h, p, (size_t)c.w * 4, &base), SHR_OK);
            if (pass) /* the updated pixels, copied */
                ASSERT_EQ_LL(shr_pl_res_image_update(base, u, patch, 3 * 4), SHR_OK);
            ASSERT_EQ_LL(shr_pl_res_image_view(base, src_of(&c), c.dw, c.dh, pass ? SHR_SCALE_COPY : SHR_SCALE_DRIVER, &v),
                         SHR_OK);
            shr_lyr *l;
            ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &l), SHR_OK);
            ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
            ASSERT_EQ_LL(shr_lyr_cmd_fill(l, (shr_rect){0, 0, HW, HH}, SHR_RGB(200, 10, 60)), SHR_OK);
            ASSERT_EQ_LL(shr_lyr_cmd_image(l, v, (shr_rect){0, 0, c.dw, c.dh}, (shr_point){1, 2}), SHR_OK);
            ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
            frame(ctx);
            if (!pass) {
                ASSERT_EQ_LL(shr_pl_res_image_update(v, u, patch, 3 * 4), SHR_E_INVALID_ARG);
                ASSERT_EQ_LL(shr_pl_res_image_update(base, u, patch, 3 * 4), SHR_OK);
                frame(ctx);
                memcpy(a, h.out.shown, sizeof(a));
            } else if (memcmp(a, h.out.shown, sizeof(a))) {
                fprintf(stderr, "update %zu\n", k);
                FAIL();
            }
            ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
            ASSERT_EQ_LL(shr_pl_res_image_release(v), SHR_OK);
            ASSERT_EQ_LL(shr_pl_res_image_release(base), SHR_OK);
            harness_close(&h);
        }
    }
    free(p);
    PASS();
}

/* An opaque image kept as RGB565 (half the budget bytes) is scaled by the driver, and copied (also shrunk by BOX), from
 * its widened pixels: the frame equals the reference on them, onto RGB565 or RGBX8888 screens. */
TEST opaque_images_scale_widened(void) {
    static const kase c = {37, 23, 3, 2, 30, 19, 50, 41};
    static const struct {
        uint32_t flags;
        int32_t dw, dh;
    } modes[] = {{SHR_SCALE_DRIVER, 50, 41}, {SHR_SCALE_COPY, 50, 41}, {SHR_SCALE_COPY | SHR_SCALE_BOX, 20, 11}};
    bool kept = SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565;
    uint8_t *p = image(&c, true), *wide = malloc((size_t)c.w * (size_t)c.h * 4);
    uint16_t *q = malloc((size_t)c.w * (size_t)c.h * 2);
    for (size_t i = 0; i < (size_t)c.w * (size_t)c.h; i++) {
        const uint8_t *o = p + 4 * i;
        q[i] = (uint16_t)((o[0] * 31 + 127) / 255 << 11 | (o[1] * 63 + 127) / 255 << 5 | (o[2] * 31 + 127) / 255);
    }
    if (kept) shr_rgb565_widen(q, (size_t)c.w * 2, c.w, c.h, wide);
    else memcpy(wide, p, (size_t)c.w * (size_t)c.h * 4);
    for (int k = 0; k < 3; k++) {
        int32_t dw = modes[k].dw, dh = modes[k].dh;
        shr_scale_src m = {wide, (size_t)c.w * 4, c.w, c.h, c.sx, c.sy, c.sw, c.sh, dw, dh};
        harness h;
        scale_driver = true, budget = 0;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
        shr_pl_res_image *base, *v;
        ASSERT_EQ_LL(shr_pl_res_image_create(ctx, c.w, c.h, p, (size_t)c.w * 4, &base), SHR_OK);
        uint64_t used;
        ASSERT_EQ_LL(shr_pl_res_image_budget(ctx, &used, NULL), SHR_OK);
        ASSERT_EQ_LL(used, (uint64_t)c.w * (uint64_t)c.h * (kept ? 2 : 4));
        ASSERT_EQ_LL(shr_pl_res_image_view(base, src_of(&c), dw, dh, modes[k].flags, &v), SHR_OK);
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &l), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_image(l, v, (shr_rect){0, 0, dw, dh}, (shr_point){5, 3}), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
        frame(ctx);
        for (int32_t y = 0; y < dh; y++)
            for (int32_t x = 0; x < dw; x++) {
                uint8_t want[4];
                shr_scale_px(&m, modes[k].flags & SHR_SCALE_BOX ? SHR_FILTER_BOX : SHR_FILTER_BILINEAR, x, y, want);
                const uint8_t *got = h.out.shown + ((size_t)(y + 3) * HW + (size_t)(x + 5)) * SCREEN_BPP;
                uint16_t g16;
                memcpy(&g16, got, 2);
                bool ok = SCREEN_BPP == 2 ? g16 == shr_blend565(0, want) : !memcmp(got, want, 3);
                if (!ok) {
                    fprintf(stderr, "mode %d (%d, %d)\n", k, x, y);
                    FAIL();
                }
            }
        ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
        ASSERT_EQ_LL(shr_pl_res_image_release(v), SHR_OK);
        ASSERT_EQ_LL(shr_pl_res_image_release(base), SHR_OK);
        harness_close(&h);
    }
    free(p), free(wide), free(q);
    PASS();
}

/* A copy is kept as RGB565 only when the pixels its taps read around `src` are opaque too: an opaque `src` next to a
 * transparent column stays RGBA8888 (4 budget bytes a pixel) and its edge pixel blends. */
TEST copies_keep_the_alpha_their_taps_read(void) {
    static const uint8_t px[12] = {255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 0};
    const shr_image_source source = {3, 1, SHR_IMAGE_SRC_RGBA8888, px, 12};
    const shr_scale_src m = {px, 12, 3, 1, 0, 0, 2, 1, 4, 1};
    for (int k = 0; k < 2; k++) { /* create_scaled, a copy view */
        harness h;
        scale_driver = true, budget = 0;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
        shr_pl_res_image *base = NULL, *img;
        uint64_t before, after;
        if (k) ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 3, 1, px, 12, &base), SHR_OK);
        ASSERT_EQ_LL(shr_pl_res_image_budget(ctx, &before, NULL), SHR_OK);
        if (k)
            ASSERT_EQ_LL(shr_pl_res_image_view(base, (shr_rect){0, 0, 2, 1}, 4, 1, SHR_SCALE_COPY, &img), SHR_OK);
        else
            ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &source, (shr_rect){0, 0, 2, 1}, 4, 1, SHR_SCALE_BILINEAR,
                                                        &img),
                         SHR_OK);
        ASSERT_EQ_LL(shr_pl_res_image_budget(ctx, &after, NULL), SHR_OK);
        ASSERT_EQ_LL(after - before, 16);
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &l), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_fill(l, (shr_rect){0, 0, HW, HH}, SHR_RGB(30, 90, 200)), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_image(l, img, (shr_rect){0, 0, 4, 1}, (shr_point){5, 3}), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
        frame(ctx);
        uint8_t fill[4];
        uint16_t fill16;
        memcpy(fill, h.out.shown, 4), memcpy(&fill16, h.out.shown, 2);
        for (int32_t x = 0; x < 4; x++) {
            uint8_t want[4], d[4];
            shr_scale_px(&m, SHR_FILTER_BILINEAR, x, 0, want);
            const uint8_t *got = h.out.shown + ((size_t)3 * HW + (size_t)(x + 5)) * SCREEN_BPP;
            uint16_t g16;
            memcpy(&g16, got, 2), memcpy(d, fill, 4);
            shr_blend8888(d, want);
            ASSERT(x < 3 || want[3] < 255); /* the edge pixel reads the transparent column */
            if (SCREEN_BPP == 2 ? g16 != shr_blend565(fill16, want) : memcmp(got, d, 3) != 0) {
                fprintf(stderr, "case %d pixel %d\n", k, x);
                FAIL();
            }
        }
        ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
        ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
        if (base) ASSERT_EQ_LL(shr_pl_res_image_release(base), SHR_OK);
        harness_close(&h);
    }
    PASS();
}

/* A driver view holds no budget bytes and keeps its released base (and the base's bytes) until it goes. */
TEST views_keep_their_base(void) {
    harness h;
    scale_driver = true, budget = 64;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    uint8_t px[64] = {0};
    shr_pl_res_image *base, *v[2], *other;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 4, 4, px, 16, &base), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_view(base, (shr_rect){0, 0, 4, 4}, 9, 9, 0, &v[0]), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_view(base, (shr_rect){1, 1, 3, 3}, 9, 9, SHR_SCALE_DRIVER, &v[1]), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_view(base, (shr_rect){0, 0, 4, 4}, 9, 9, SHR_SCALE_COPY, &other), SHR_E_LIMIT);
    ASSERT_EQ_LL(shr_pl_res_image_release(base), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_view(base, (shr_rect){0, 0, 4, 4}, 9, 9, 0, &other), SHR_E_INVALID_ARG);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, v[1], (shr_rect){0, 0, 9, 9}, (shr_point){0, 0}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(v[0]), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 4, 4, px, 16, &other), SHR_E_LIMIT); /* v[1] keeps the base */
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(v[1]), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 4, 4, px, 16, &other), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(other), SHR_OK);
    harness_close(&h);
    PASS();
}

TEST scale_arguments(void) {
    harness h;
    scale_driver = false, budget = 0;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    uint8_t px[16] = {0};
    shr_pl_res_image *img, *v;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, px, 8, &img), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 0, 2, 2}, 4, 4, SHR_SCALE_DRIVER, &v), SHR_E_UNSUPPORTED);
    ASSERT(!v);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 0, 3, 2}, 4, 4, 0, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){1, 1, 1, 2}, 4, 4, 0, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){-1, 0, 2, 2}, 4, 4, 0, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 0, 2, 2}, 0, 4, 0, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 0, 2, 2}, 32768, 4, 0, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 0, 2, 2}, 4, 4, 2, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 0, 2, 2}, 4, 4, SHR_SCALE_COPY | SHR_SCALE_DRIVER, &v),
                 SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_view(NULL, (shr_rect){0, 0, 2, 2}, 4, 4, 0, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 0, 2, 2}, 4, 4, 0, NULL), SHR_E_INVALID_ARG);
    shr_image_source s = {2, 2, SHR_IMAGE_SRC_RGBA8888, px, 4}; /* stride below a row */
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &s, (shr_rect){0, 0, 2, 2}, 4, 4, 0, &v), SHR_E_INVALID_ARG);
    s.stride = 8, s.format = (shr_image_source_format)4;
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &s, (shr_rect){0, 0, 2, 2}, 4, 4, 0, &v), SHR_E_INVALID_ARG);
    s.format = SHR_IMAGE_SRC_GRAY_ALPHA88, s.stride = 4;
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &s, (shr_rect){0, 0, 2, 2}, 4, 4, 2, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, NULL, (shr_rect){0, 0, 2, 2}, 4, 4, 0, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(NULL, &s, (shr_rect){0, 0, 2, 2}, 4, 4, 0, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &s, (shr_rect){0, 0, 2, 2}, 4, 4, 0, NULL), SHR_E_INVALID_ARG);
    static const shr_rect bad[] = {{2, 0, 1, 2}, {0, -1, 2, 2}, {0, 0, 2, 3}, {0, 1, 2, 1}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &s, bad[i], 4, 4, 0, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &s, (shr_rect){0, 0, 2, 2}, 4, 0, 0, &v), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &s, (shr_rect){0, 0, 2, 2}, 4, 32768, 0, &v), SHR_E_INVALID_ARG);
    ASSERT(!v);
    uint8_t *line = calloc(32768, 1); /* src sides up to 32767 */
    const shr_image_source wide = {32768, 1, SHR_IMAGE_SRC_GRAY8, line, 32768};
    const shr_image_source tall = {1, 32768, SHR_IMAGE_SRC_GRAY8, line, 1};
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &wide, (shr_rect){0, 0, 32768, 1}, 4, 1, 0, &v),
                 SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &tall, (shr_rect){0, 0, 1, 32768}, 1, 4, 0, &v),
                 SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &wide, (shr_rect){1, 0, 32768, 1}, 4, 1, 0, &v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &tall, (shr_rect){0, 1, 1, 32768}, 1, 4, 0, &v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(v), SHR_OK);
    free(line);
    ASSERT_EQ_LL(shr_pl_res_image_create_scaled(ctx, &s, (shr_rect){0, 0, 2, 2}, 4, 4, SHR_SCALE_BOX, &v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 1, 2, 2}, 5, 3, 0, &v), SHR_OK); /* no SCALE: a copy */
    ASSERT_EQ_LL(shr_pl_res_image_update(v, (shr_rect){0, 0, 1, 1}, px, 4), SHR_OK);
    shr_pl_res_image *vv;
    ASSERT_EQ_LL(shr_pl_res_image_view(v, (shr_rect){0, 0, 5, 3}, 2, 2, SHR_SCALE_BOX, &vv), SHR_OK); /* of a copy */
    ASSERT_EQ_LL(shr_pl_res_image_release(vv), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    harness_close(&h);
    PASS();
}

/* Views of views are refused; BOX views are copies even with a scaling driver. */
TEST driver_view_arguments(void) {
    harness h;
    scale_driver = true, budget = 0;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    uint8_t px[16] = {0};
    shr_pl_res_image *img, *v, *w;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, px, 8, &img), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 0, 2, 2}, 1, 1, SHR_SCALE_BOX | SHR_SCALE_DRIVER, &v),
                 SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 0, 2, 2}, 1, 1, SHR_SCALE_BOX, &v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_update(v, (shr_rect){0, 0, 1, 1}, px, 4), SHR_OK); /* a copy */
    ASSERT_EQ_LL(shr_pl_res_image_release(v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_view(img, (shr_rect){0, 0, 2, 2}, 7, 7, 0, &v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_view(v, (shr_rect){0, 0, 7, 7}, 3, 3, SHR_SCALE_COPY, &w), SHR_E_INVALID_ARG);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, v, (shr_rect){0, 0, 8, 7}, (shr_point){0, 0}), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, v, (shr_rect){2, 3, 7, 7}, (shr_point){0, 0}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    harness_close(&h);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(rows_match_reference);
    RUN_TEST(sources_scale_as_rgba);
    RUN_TEST(wide_sources_read_their_columns);
    RUN_TEST(copies_match_reference);
    RUN_TEST(draws_match_reference_in_parts);
    RUN_TEST(rgb565_scales_widened);
    RUN_TEST(scaled_draws_checked);
    RUN_TEST(copies_equal_driver_views);
    RUN_TEST(views_follow_updates);
    RUN_TEST(opaque_images_scale_widened);
    RUN_TEST(copies_keep_the_alpha_their_taps_read);
    RUN_TEST(views_keep_their_base);
    RUN_TEST(scale_arguments);
    RUN_TEST(driver_view_arguments);
    GREATEST_MAIN_END();
}
