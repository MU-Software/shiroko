#ifndef SHIROKO_FUZZ_COMMON_H
#define SHIROKO_FUZZ_COMMON_H

#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#define FUZZ_MSAN 1
#endif
#endif

/* A command whose fields stay unspecified until set: bytes of `v`, which MSan also takes as never written. */
static inline void fuzz_blank(shr_draw_cmd *c, uint8_t v) {
    memset(c, v, sizeof(*c));
#ifdef FUZZ_MSAN
    __msan_poison(c, sizeof(*c));
#endif
}

/* The bytes a command's image spans where a driver may read them: 0 when it is empty, DEVICE or invalid. */
static inline size_t fuzz_span(const shr_image_ref *m) {
    size_t row;
    if (m->width <= 0 || m->height <= 0 || m->domain == SHR_MEMORY_DEVICE ||
        shr_format_row_bytes((shr_pixel_format)m->format, m->width, &row) != SHR_OK || m->stride < row)
        return 0;
    return (size_t)(m->height - 1) * m->stride + row;
}

/* An invariant violation is a finding: abort so libFuzzer keeps the input. */
#define FUZZ_CHECK(cond)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "%s:%d: invariant failed: %s\n", __FILE__, __LINE__, #cond); \
            abort();                                                                     \
        }                                                                                \
    } while (0)

typedef struct fuzz_reader {
    const uint8_t *p;
    size_t n;
} fuzz_reader;

static inline uint8_t fr_u8(fuzz_reader *r) {
    if (r->n == 0) return 0;
    r->n--;
    return *r->p++;
}

static inline uint16_t fr_u16(fuzz_reader *r) {
    uint16_t lo = fr_u8(r);
    return (uint16_t)(lo | (uint16_t)(fr_u8(r) << 8));
}

static inline int32_t fr_i8(fuzz_reader *r) { return (int8_t)fr_u8(r); }
static inline int32_t fr_i16(fuzz_reader *r) { return (int16_t)fr_u16(r); }

/* Takes up to `max` bytes as a borrowed slice. */
static inline const char *fr_bytes(fuzz_reader *r, size_t max, size_t *len) {
    size_t k = max < r->n ? max : r->n;
    const char *s = (const char *)r->p;
    r->p += k;
    r->n -= k;
    *len = k;
    return s;
}

static inline shr_rect fr_rect(fuzz_reader *r) {
    int32_t x0 = fr_i8(r), y0 = fr_i8(r), x1 = fr_i8(r), y1 = fr_i8(r);
    return (shr_rect){x0, y0, x1, y1};
}

#define FUZZ_W 64
#define FUZZ_H 48
#define FUZZ_BPP (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? 2 : 4)

/* One screen-sized buffer in SHR_PIXEL_FORMAT, released on present. */
typedef struct fuzz_output {
    uint8_t pixels[FUZZ_W * FUZZ_H * 4];
    int presents;
} fuzz_output;

static inline shr_status fo_acquire(void *user, shr_surface *s) {
    fuzz_output *o = (fuzz_output *)user;
    *s = (shr_surface){o->pixels, FUZZ_W, FUZZ_H, FUZZ_W * FUZZ_BPP, FUZZ_W * FUZZ_H * FUZZ_BPP, SHR_PIXEL_FORMAT, 0,
                       SHR_MEMORY_CPU, 0};
    return SHR_OK;
}
static inline shr_status fo_present(void *user, const shr_surface *s, uint64_t id) {
    (void)s, (void)id;
    ((fuzz_output *)user)->presents++;
    return SHR_OK;
}
static inline void fo_discard(void *user, const shr_surface *s) { (void)user, (void)s; }

static inline void fuzz_output_init(fuzz_output *o, shr_output *out, uint32_t flags) {
    shr_output_init(out);
    out->user = o, out->flags = SHR_OUTPUT_RELEASE_ON_PRESENT | flags;
    out->acquire = fo_acquire, out->present = fo_present, out->discard = fo_discard;
}

/* Pumps until nothing is due now; a frame that fails is a finding. */
static inline void fuzz_settle(shr_context *ctx) {
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    for (int i = 0; i < 256 && dl.kind == SHR_DEADLINE_NOW; i++) {
        FUZZ_CHECK(shr_pump(ctx) == SHR_OK);
        FUZZ_CHECK(shr_next_deadline(ctx, &dl) == SHR_OK);
    }
    FUZZ_CHECK(dl.kind != SHR_DEADLINE_NOW);
    shr_event ev;
    while (shr_poll_event(ctx, &ev) == SHR_OK) FUZZ_CHECK(ev.kind != SHR_EVENT_PRESENT_FAILED);
}

static inline shr_status fuzz_no_packages(void *user, const char *name, shr_asset_source *src) {
    (void)user, (void)name, (void)src;
    return SHR_E_NOT_FOUND;
}

/* A keep as the driver should hold it. */
typedef struct fuzz_keep {
    bool held;
    int32_t w, h;
    uint8_t px[256 * 256 * 4];
} fuzz_keep;

/* Draws the draws of a batch the driver accepted by the stateless path from `bufs`: groups into keeps[id - 1], each
 * KEEP_DRAW from what the harness drew there, COPYs reading `self` from `dst`. false: a KEEP_DRAW of a keep the harness
 * does not hold or reading past it. */
static inline bool fuzz_keep_draw(const shr_surface *dst, const void *self, const shr_draw_cmd *cmds, size_t n,
                                  const shr_image *bufs, uint32_t nbufs, fuzz_keep *keeps, uint32_t nkeeps) {
    size_t bpp = dst->format == SHR_FORMAT_RGB565 ? 2 : 4;
    for (size_t i = 0; i < n; i++) {
        shr_draw_cmd moved_src = cmds[i];
        if (moved_src.kind == SHR_CMD_COPY && moved_src.src.pixels == self) moved_src.src.pixels = dst->pixels;
        const shr_draw_cmd *c = &moved_src;
        if (c->kind >= SHR_CMD_BUFFER_REGISTER) continue;
        if (c->kind == SHR_CMD_KEEP_DRAW) {
            const fuzz_keep *k = c->buffer >= 1 && c->buffer <= nkeeps ? &keeps[c->buffer - 1] : NULL;
            if (!k || !k->held || c->src_origin.x < 0 || c->src_origin.y < 0 ||
                c->src_origin.x + c->dst.x1 - c->dst.x0 > k->w || c->src_origin.y + c->dst.y1 - c->dst.y0 > k->h)
                return false;
            for (int32_t y = c->dst.y0; y < c->dst.y1; y++)
                memcpy((uint8_t *)dst->pixels + (size_t)y * dst->stride + (size_t)c->dst.x0 * bpp,
                       k->px + ((size_t)(c->src_origin.y + y - c->dst.y0) * (size_t)k->w + (size_t)c->src_origin.x) * bpp,
                       (size_t)(c->dst.x1 - c->dst.x0) * bpp);
            continue;
        }
        if (c->kind != SHR_CMD_KEEP_BEGIN) {
            FUZZ_CHECK(shr_software_execute(dst, c, 1, bufs, nbufs) == SHR_OK);
            continue;
        }
        FUZZ_CHECK(c->buffer >= 1 && c->buffer <= nkeeps);
        fuzz_keep *k = &keeps[c->buffer - 1];
        k->held = true, k->w = c->dst.x1 - c->dst.x0, k->h = c->dst.y1 - c->dst.y0;
        FUZZ_CHECK((size_t)k->w * (size_t)k->h * bpp <= sizeof(k->px));
        shr_surface ks = {k->px, k->w, k->h, (size_t)k->w * bpp, (size_t)k->w * bpp * (size_t)k->h, dst->format, 0,
                          SHR_MEMORY_CPU, 0};
        for (i++; cmds[i].kind != SHR_CMD_KEEP_END; i++) {
            shr_draw_cmd moved = cmds[i];
            moved.dst = (shr_rect){moved.dst.x0 - c->dst.x0, moved.dst.y0 - c->dst.y0, moved.dst.x1 - c->dst.x0,
                                   moved.dst.y1 - c->dst.y0};
            FUZZ_CHECK(shr_software_execute(&ks, &moved, 1, bufs, nbufs) == SHR_OK);
        }
    }
    return true;
}

#endif
