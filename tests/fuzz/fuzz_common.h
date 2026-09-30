#ifndef SHIROKO_FUZZ_COMMON_H
#define SHIROKO_FUZZ_COMMON_H

#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#endif
