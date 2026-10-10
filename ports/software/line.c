#include "raster.h"

/* Every platform gets the same coverage: no fused multiply-adds. */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#define SS 16
#define STEPS 32
#define POINTS (3 * 2 * STEPS + 1)

typedef struct pt {
    float x, y;
} pt;

static float bezier(float a, float b, float c, float d, float t) {
    float u = 1 - t;
    return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
}

/* The curve of one period and its copies a period left and right. */
static void curly_points(pt *p, int32_t w, int32_t h) {
    float c = 0.5f * (float)w, k = 0.4f, top = 0.5f, bottom = (float)h - 0.5f;
    pt one[2 * STEPS + 1];
    for (int i = 0; i <= STEPS; i++) {
        float t = (float)i / STEPS;
        one[i] = (pt){bezier(0, c * k, c - c * k, c, t), bezier(bottom, bottom, top, top, t)};
        one[STEPS + i] = (pt){bezier(c, c + c * k, (float)w - c * k, (float)w, t), bezier(top, top, bottom, bottom, t)};
    }
    for (int r = 0, n = 0; r < 3; r++)
        for (int i = r ? 1 : 0; i <= 2 * STEPS; i++) p[n++] = (pt){one[i].x + (float)((r - 1) * w), one[i].y};
}

static bool near(float px, float py, const pt *p) {
    for (int i = 0; i + 1 < POINTS; i++) {
        pt a = p[i], b = p[i + 1];
        if (px < a.x - 1 || px > b.x + 1) continue; /* x grows along the curve */
        float dx = b.x - a.x, dy = b.y - a.y;
        float t = ((px - a.x) * dx + (py - a.y) * dy) / (dx * dx + dy * dy);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float ex = a.x + t * dx - px, ey = a.y + t * dy - py;
        if (ex * ex + ey * ey <= 0.25f) return true;
    }
    return false;
}

static int32_t floor_pos(float v) { return (int32_t)v; }
static int32_t ceil_pos(float v) { return (int32_t)v + ((float)(int32_t)v < v); }

/* Sixteen by sixteen samples of the pixel, quantized to the 16 levels of A4. */
static uint8_t sampled(uint32_t shape, int32_t x, int32_t y, const float *xs, int32_t m, float yc, const pt *p) {
    float r = 0.70710678f;
    uint32_t in = 0;
    for (int j = 0; j < SS; j++)
        for (int i = 0; i < SS; i++) {
            float sx = (float)x + ((float)i + 0.5f) / SS, sy = (float)y + ((float)j + 0.5f) / SS;
            bool hit = false;
            for (int32_t k = 0; shape == SHR_LINE_DOTTED && k < m && !hit; k++)
                hit = (sx - xs[k]) * (sx - xs[k]) + (sy - yc) * (sy - yc) <= r * r;
            in += shape == SHR_LINE_DOTTED ? hit : near(sx, sy, p);
        }
    uint32_t a = (in * 255 + 128) >> 8;
    return (uint8_t)(17 * ((a * 15 + 127) / 255));
}

void shr__raster_line_coverage(uint8_t *out, uint32_t shape, int32_t w, int32_t h) SHR_NONBLOCKING {
    float r = 0.70710678f, yc = 0.5f * (float)h, xs[SHR_LINE_MAX_PERIOD];
    int32_t m = 0;
    pt p[POINTS];
    if (shape == SHR_LINE_DOTTED) {
        int32_t a = ceil_pos((float)w / (4 * r)), b = floor_pos((float)w / (3 * r)), c = floor_pos((float)w / (2 * r + 1));
        m = a < b ? a : b;
        m = m < c ? m : c;
        m = m < 1 ? 1 : m;
        for (int32_t k = 0; k < m; k++) xs[k] = (float)w / (float)m * ((float)k + 0.5f);
    } else if (shape == SHR_LINE_CURLY) {
        curly_points(p, w, h);
    }
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            uint8_t v = 255;
            if (shape == SHR_LINE_DOUBLE) v = y == 0 || y == h - 1 ? 255 : 0;
            if (shape == SHR_LINE_DASHED) v = (x / (w / 3 + 1)) % 2 ? 0 : 255;
            if (shape == SHR_LINE_DOTTED || shape == SHR_LINE_CURLY) v = sampled(shape, x, y, xs, m, yc, p);
            out[y * w + x] = v;
        }
}

const uint8_t *shr__raster_line_cell(const shr_draw_cmd *c, shr__line_memo *memo, uint8_t *tmp) SHR_NONBLOCKING {
    uint32_t shape = c->flags >> SHR_LINE_SHAPE_SHIFT, key = (uint32_t)c->src_rect.x1 << 8 | (uint32_t)c->src_rect.y1;
    bool kept = memo && (shape == SHR_LINE_CURLY || shape == SHR_LINE_DOTTED);
    int k = shape == SHR_LINE_DOTTED;
    uint8_t *out = kept ? memo->cov[k] : tmp;
    if (kept && memo->key[k] == key) return out;
    shr__raster_line_coverage(out, shape, c->src_rect.x1, c->src_rect.y1);
    if (kept) memo->key[k] = key;
    return out;
}
