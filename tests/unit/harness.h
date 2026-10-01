#ifndef SHIROKO_TEST_HARNESS_H
#define SHIROKO_TEST_HARNESS_H

#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#define HW 64
#define HH 48
#define NBUF 3
#define HBUFS 64 /* buffer ids of the mock driver */
#define SCREEN_BPP (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? 2 : 4)

typedef struct mock_output {
    uint8_t bufs[NBUF][HW * HH * 4];
    bool busy[NBUF];
    uint32_t gen[NBUF];
    int32_t w, h;
    shr_pixel_format format;
    shr_memory_domain domain;
    shr_status present_result, acquire_result;
    int presents, discards;
    uint8_t shown[HW * HH * 4];
    uint64_t last_frame;
    int last_buf;
} mock_output;

static inline shr_status mo_acquire(void *user, shr_surface *out) {
    mock_output *m = user;
    if (m->acquire_result != SHR_OK) return m->acquire_result;
    for (int i = 0; i < NBUF; i++) {
        if (m->busy[i]) continue;
        m->busy[i] = true;
        size_t bpp = m->format == SHR_FORMAT_RGB565 ? 2 : 4;
        *out = (shr_surface){m->bufs[i], m->w, m->h, (size_t)m->w * bpp, sizeof(m->bufs[i]), m->format, m->gen[i],
                             m->domain, 0};
        return SHR_OK;
    }
    return SHR_E_WOULD_BLOCK;
}

static inline int mo_index(mock_output *m, const shr_surface *s) {
    for (int i = 0; i < NBUF; i++)
        if (s->pixels == m->bufs[i]) return i;
    abort();
}

static inline shr_status mo_present(void *user, const shr_surface *s, uint64_t frame_id) {
    mock_output *m = user;
    if (m->present_result != SHR_OK) return m->present_result;
    int i = mo_index(m, s);
    memcpy(m->shown, s->pixels, sizeof(m->shown));
    m->busy[i] = false;
    m->presents++;
    m->last_frame = frame_id;
    m->last_buf = i;
    return SHR_OK;
}

static inline void mo_discard(void *user, const shr_surface *s) {
    mock_output *m = user;
    m->busy[mo_index(m, s)] = false;
    m->discards++;
}

static uint64_t fake_now;
static inline uint64_t fake_clock(void *user) {
    (void)user;
    return fake_now;
}

/* Runs commands on the software port, or, when async, keeps the compositor's own commands (no copy)
 * and runs them at md_complete(): the contract says they stay unchanged until the fence resolves.
 * The buffer prologue updates `buffers` (id k at [k - 1]) as the batch runs. A failed batch forgets them all, or
 * with `fail_late` first applies its prologue, as a driver checking draws only after it may do. `budget` (0 = none)
 * bounds the registered bytes like a driver keeping copies: a REGISTER beyond it fails the batch. */
typedef struct mock_driver {
    int fail_next;
    int block_next;
    int calls;
    bool async;
    bool reset_ok;
    int resets;
    shr_fence pending;
    const shr_surface *pending_dst;
    const shr_draw_cmd *pending_cmds;
    size_t pending_count;
    size_t last_count;
    shr_context *ctx;
    shr_image buffers[HBUFS];
    bool fail_late;
    uint64_t budget;
    int registers, updates, releases; /* buffer commands run */
    shr_status completed;             /* of the last md_complete() */
} mock_driver;

static inline uint64_t md_held(const mock_driver *d) {
    uint64_t n = 0;
    for (int k = 0; k < HBUFS; k++) n += d->buffers[k].byte_length;
    return n;
}

/* Runs the prologue, then (draw) the draws against the table. */
static inline shr_status md_batch(mock_driver *d, const shr_surface *dst, const shr_draw_cmd *cmds, size_t n, bool draw) {
    size_t i = 0;
    for (; i < n && cmds[i].kind >= SHR_CMD_BUFFER_REGISTER; i++) {
        const shr_draw_cmd *c = &cmds[i];
        if (c->kind != SHR_CMD_BUFFER_RELEASE && (!c->buffer || c->buffer > HBUFS)) return SHR_E_INVALID_ARG;
        if (c->kind == SHR_CMD_BUFFER_REGISTER && d->budget &&
            md_held(d) - d->buffers[c->buffer - 1].byte_length + c->src.byte_length > d->budget)
            return SHR_E_UNSUPPORTED;
        if (c->kind == SHR_CMD_BUFFER_REGISTER) d->buffers[c->buffer - 1] = c->src, d->registers++;
        if (c->kind == SHR_CMD_BUFFER_UPDATE && !d->buffers[c->buffer - 1].format) return SHR_E_INVALID_ARG;
        d->updates += c->kind == SHR_CMD_BUFFER_UPDATE;
        if (c->kind == SHR_CMD_BUFFER_RELEASE && c->buffer && c->buffer <= HBUFS) d->buffers[c->buffer - 1] = (shr_image){0};
        d->releases += c->kind == SHR_CMD_BUFFER_RELEASE;
    }
    return draw ? shr_software_execute(dst, cmds + i, n - i, d->buffers, HBUFS) : SHR_OK;
}

static inline shr_status md_run(mock_driver *d, const shr_surface *dst, const shr_draw_cmd *cmds, size_t n) {
    return md_batch(d, dst, cmds, n, true);
}

static inline shr_status md_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t n, shr_fence fence) {
    mock_driver *d = user;
    d->calls++;
    d->last_count = n;
    if (d->fail_next > 0) {
        d->fail_next--;
        if (d->fail_late) md_batch(d, dst, cmds, n, false);
        else memset(d->buffers, 0, sizeof(d->buffers));
        return SHR_E_DEVICE;
    }
    if (d->block_next > 0) {
        d->block_next--;
        return SHR_E_WOULD_BLOCK;
    }
    if (dst->domain == SHR_MEMORY_DEVICE) return SHR_OK;
    if (!d->async) return md_run(d, dst, cmds, n);
    d->pending = fence;
    d->pending_dst = dst;
    d->pending_cmds = cmds;
    d->pending_count = n;
    return SHR_IN_PROGRESS;
}

static inline void md_complete(mock_driver *d) {
    if (!d->pending) return;
    d->completed = md_run(d, d->pending_dst, d->pending_cmds, d->pending_count);
    shr_fence f = d->pending;
    d->pending = 0;
    shr_fence_signal(d->ctx, f, SHR_FENCE_SUCCEEDED);
}

static inline shr_status md_reset(void *user) {
    mock_driver *d = user;
    d->resets++;
    if (!d->reset_ok) return SHR_E_DEVICE;
    d->pending = 0;
    memset(d->buffers, 0, sizeof(d->buffers));
    return SHR_OK;
}

typedef struct harness {
    mock_output out;
    mock_driver drv;
    shr_framebuffer_driver driver;
    shr_context *ctx;
    shr_pl_res_bitmap_font *font;
} harness;

/* The descriptor points into h and *out. */
static inline void harness_desc(harness *h, uint32_t output_flags, shr_output *out, shr_context_desc *d) {
    memset(h, 0, sizeof(*h));
    h->out.w = HW, h->out.h = HH, h->out.format = SHR_PIXEL_FORMAT;
    shr_framebuffer_driver_init(&h->driver);
    h->driver.user = &h->drv;
    h->driver.caps.max_buffers = HBUFS;
    h->driver.execute = md_execute;
    h->driver.reset = md_reset;
    shr_output_init(out);
    out->user = &h->out;
    out->flags = output_flags;
    out->timestamp = SHR_TIMESTAMP_VSYNC;
    out->acquire = mo_acquire;
    out->present = mo_present;
    out->discard = mo_discard;
    shr_context_desc_init(d);
    d->driver = &h->driver;
    d->output = out;
    d->now_ns = fake_clock;
    fake_now = 0;
}

static inline shr_context *harness_open(harness *h, uint32_t output_flags,
                                 void (*tweak)(shr_context_desc *, shr_framebuffer_driver *)) {
    shr_output out;
    shr_context_desc d;
    harness_desc(h, output_flags, &out, &d);
    if (tweak) tweak(&d, &h->driver);
    ASSERT_EQ_LL(shr_create(&d, &h->ctx), SHR_OK);
    h->drv.ctx = h->ctx;
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = HW, sd.height = HH, sd.clear = SHR_RGB(0, 0, 0);
    ASSERT_EQ_LL(shr_screen_configure(h->ctx, &sd), SHR_OK);
    return h->ctx;
}

#ifdef SHR_FONT_DIR
static inline shr_status font_dir_open(void *user, const char *name, shr_asset_source *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", user ? (const char *)user : SHR_FONT_DIR, name);
    return shr_asset_source_file(path, out); /* SHR_E_NOT_FOUND when the package is not there */
}

/* The baked packages of this build, opened on demand from SHR_FONT_DIR. */
static inline shr_pl_res_bitmap_font *harness_font(harness *h) {
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = font_dir_open;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(h->ctx, &fd, &h->font), SHR_OK);
    return h->font;
}
#endif

/* Pumps until nothing is due now (bounded). */
static inline void settle(shr_context *ctx) {
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    for (int i = 0; i < 256 && dl.kind == SHR_DEADLINE_NOW; i++) {
        shr_pump(ctx);
        shr_next_deadline(ctx, &dl);
    }
}

static inline void harness_close(harness *h) {
    shr_begin_shutdown(h->ctx);
    for (int i = 0; i < 16; i++) {
        md_complete(&h->drv);
        shr_pump(h->ctx);
        if (h->font && shr_pl_res_bitmap_font_destroy(h->font) == SHR_OK) h->font = NULL;
        if (!h->font && shr_destroy(h->ctx) == SHR_OK) return;
    }
    FAIL_WITH_LONGJMPm("context did not shut down");
}

/* 0x00RRGGBB of a pixel of a screen-format buffer. */
static inline uint32_t px(const uint8_t *buf, int x, int y) {
    const uint8_t *p = buf + ((size_t)y * HW + (size_t)x) * SCREEN_BPP;
    if (SCREEN_BPP == 2) {
        uint16_t v;
        memcpy(&v, p, 2);
        uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        return ((r * 255 + 15) / 31) << 16 | ((g * 255 + 31) / 63) << 8 | ((b * 255 + 15) / 31);
    }
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static inline int count_events(shr_context *ctx, shr_event_kind kind, shr_event *last) {
    int n = 0;
    shr_event ev = {0};
    while (shr_poll_event(ctx, &ev) == SHR_OK)
        if (ev.kind == kind) {
            n++;
            if (last) *last = ev;
        }
    return n;
}

#endif
