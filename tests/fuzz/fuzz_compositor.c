/* Compositor under random layer, image, group shift (a move), submit, pump, output and fence events with faults (driver
 * failures and stalls, async fences, watchdog timeouts, output errors, screen changes with or without bands, sparse
 * event polling).
 * Invariants:
 *   - presented frame ids only grow; a frame ends once (accepted, failed or superseded); only accepted frames
 *     are released (exactly once) or displayed; once settled, every frame a submission reported has ended
 *   - the driver only writes buffers the compositor owns; acquired surfaces always come back; an accepted
 *     submission's commands, destination and the memory it reads (buffers it names, composition) stay unchanged
 *     until its fence resolves
 *   - buffer commands lead each batch with ids in 1..max_buffers, draws name registered ids (also after a failed
 *     batch, which forgets every id or first runs its buffer commands, or a reset, which forgets every id), and a
 *     driver keeping copies holds at most buffer_bytes at every REGISTER and draws what the images hold: every
 *     change reached it through an UPDATE or REGISTER; KEEP_RELEASE names ids in 1..max_keeps, and layers of fills
 *     and images make no keep group
 *   - no event is dropped, even with the smallest queue polled only now and then
 *   - once settled, the presented image equals a reference raster of the layer model (z, creation order,
 *     visibility, layer clipping, images with alpha, moved groups), also rotated or converted, composed whole or band
 *     by band, after moves the driver made as COPYs of the output onto itself
 *   - shutdown ends every submitted frame and drains once the device and the output release everything
 *   - with a frame-rate cap (every input also runs with one), frames start at least the cap apart and the settled
 *     output still shows the model */
#include "fuzz_common.h"

#include "shr_compositor.h"

#define NBUF 3
#define NLAYERS 4
#define NCMDS 6
#define NIMAGES 3
#define MAX_FRAMES 1024
#define NIDS 16
#define IMAGE_BYTES (8 * 8 * 4)
#define CAP_NS 250

enum { BUF_FREE, BUF_ACQUIRED, BUF_HELD };
enum { FRAME_NONE, FRAME_ACCEPTED, FRAME_RELEASED, FRAME_ENDED };

typedef struct mcmd {
    bool image;
    shr_rect rect;
    shr_color color;
    int img;
    shr_point at;
} mcmd;

typedef struct mlayer {
    shr_lyr *l;
    int32_t z;
    uint64_t seq;
    shr_rect rect;
    bool visible, building;
    mcmd cmds[NCMDS], pend[NCMDS];
    int n, np;
    int32_t oy; /* the committed commands moved down by oy */
} mlayer;

typedef struct mimage {
    shr_pl_res_image *img;
    bool released;
    int32_t w, h;
    uint8_t px[8 * 8 * 4];
} mimage;

typedef struct harness {
    uint8_t bufs[NBUF][FUZZ_W * FUZZ_H * 4];
    uint8_t band_px[2][FUZZ_W * FUZZ_H * 4];
    shr_surface bands[2];
    uint8_t buf_state[NBUF];
    uint32_t gen[NBUF];
    uint8_t shown[FUZZ_W * FUZZ_H * 4];
    int32_t ow, oh;
    shr_pixel_format ofmt;
    bool presented; /* since the last screen change */
    shr_status acquire_result, present_result;
    uint64_t held[NBUF];
    int held_count;
    bool release_on_present, reset_ok, async, fail_late;
    int driver_fail, driver_block;
    shr_context *ctx;
    shr_fence pending;
    const shr_surface *pending_dst; /* the compositor's own, unchanged until the fence resolves */
    const shr_draw_cmd *pending_cmds;
    size_t pending_count;
    shr_surface seen_dst; /* copies to check that */
    shr_draw_cmd seen_cmds[512];
    struct {
        const void *pixels;
        size_t length;
        uint64_t hash;
    } seen_src[16];
    int seen_srcs;
    shr_driver_caps caps;
    shr_image table[NIDS];          /* what each id names; with copies, the driver's copy */
    const uint8_t *origin[NIDS];    /* with copies: the memory a REGISTER named */
    uint8_t copies[NIDS][IMAGE_BYTES];
    uint8_t frames[MAX_FRAMES];
    bool submitted[MAX_FRAMES]; /* ids SHR_TRACE_SUBMIT reported */
    uint64_t last_accepted;
    mlayer layers[NLAYERS];
    mimage images[NIMAGES];
    uint64_t seq;
    uint64_t cap, next_start;
} harness;

static harness h;
static uint64_t now;

static uint64_t clock_fn(void *user) {
    (void)user;
    return now;
}

static void trace_fn(void *user, const shr_trace_event *ev) {
    (void)user;
    if (ev->kind == SHR_TRACE_SUBMIT && ev->id < MAX_FRAMES) h.submitted[ev->id] = true;
    if (ev->kind == SHR_TRACE_RASTER_BEGIN) {
        FUZZ_CHECK(now >= h.next_start);
        h.next_start = now + h.cap;
    }
}

static int buf_index(const void *pixels) {
    for (int i = 0; i < NBUF; i++)
        if (pixels == h.bufs[i]) return i;
    return -1;
}

static shr_status h_acquire(void *user, shr_surface *s) {
    (void)user;
    if (h.acquire_result != SHR_OK) return h.acquire_result;
    size_t bpp = h.ofmt == SHR_FORMAT_RGB565 ? 2 : 4;
    for (int i = 0; i < NBUF; i++) {
        if (h.buf_state[i] != BUF_FREE) continue;
        h.buf_state[i] = BUF_ACQUIRED;
        *s = (shr_surface){h.bufs[i], h.ow, h.oh, (size_t)h.ow * bpp, (size_t)h.ow * (size_t)h.oh * bpp, h.ofmt,
                           h.gen[i], SHR_MEMORY_CPU, 0};
        return SHR_OK;
    }
    return SHR_E_WOULD_BLOCK;
}

static shr_status h_present(void *user, const shr_surface *s, uint64_t frame_id) {
    (void)user;
    int i = buf_index(s->pixels);
    FUZZ_CHECK(i >= 0 && h.buf_state[i] == BUF_ACQUIRED);
    if (h.present_result != SHR_OK) return h.present_result;
    memcpy(h.shown, s->pixels, s->byte_length);
    h.presented = true;
    if (h.release_on_present) {
        h.buf_state[i] = BUF_FREE;
    } else {
        h.buf_state[i] = BUF_HELD;
        h.held[h.held_count++] = frame_id << 8 | (uint64_t)i;
    }
    return SHR_OK;
}

static void h_discard(void *user, const shr_surface *s) {
    (void)user;
    int i = buf_index(s->pixels);
    FUZZ_CHECK(i >= 0 && h.buf_state[i] == BUF_ACQUIRED);
    h.buf_state[i] = BUF_FREE;
}

/* Polynomial modulo a prime below 2^32: any single changed byte changes it, and nothing wraps. */
static uint64_t hash_bytes(const void *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v * 257 + ((const uint8_t *)p)[i]) % 4294967291u;
    return v;
}

static bool copies(void) { return h.caps.buffer_flags & SHR_BUFFER_COPIES; }

static size_t prologue_len(const shr_draw_cmd *cmds, size_t n) {
    size_t i = 0;
    while (i < n && cmds[i].kind >= SHR_CMD_BUFFER_REGISTER && cmds[i].kind <= SHR_CMD_KEEP_RELEASE) i++;
    return i;
}

/* Applies a buffer command to `t`, as a driver keeping copies does when `copy`. */
static void apply(shr_image *t, const shr_draw_cmd *c, bool copy) {
    uint32_t k = c->buffer - 1;
    if (c->kind == SHR_CMD_KEEP_RELEASE) {
        FUZZ_CHECK(c->buffer >= 1 && c->buffer <= h.caps.max_keeps);
        return;
    }
    if (c->kind == SHR_CMD_BUFFER_RELEASE) {
        if (c->buffer && c->buffer <= NIDS) t[k] = (shr_image){0};
        return;
    }
    FUZZ_CHECK(c->buffer >= 1 && c->buffer <= h.caps.max_buffers);
    if (c->kind == SHR_CMD_BUFFER_REGISTER) {
        FUZZ_CHECK(shr_image_ref_get(&c->src, &t[k]) == SHR_OK && t[k].byte_length <= IMAGE_BYTES);
        if (!copy) return;
        h.origin[k] = c->src.pixels;
        memcpy(h.copies[k], c->src.pixels, t[k].byte_length);
        t[k].pixels = h.copies[k];
        uint64_t held = 0;
        for (int i = 0; i < NIDS; i++) held += t[i].byte_length;
        FUZZ_CHECK(!h.caps.buffer_bytes || held <= h.caps.buffer_bytes);
        return;
    }
    FUZZ_CHECK(t[k].format && c->src_rect.x0 >= 0 && c->src_rect.y0 >= 0 && c->src_rect.x1 <= t[k].width &&
               c->src_rect.y1 <= t[k].height && c->src_rect.x0 < c->src_rect.x1 && c->src_rect.y0 < c->src_rect.y1);
    for (int32_t y = c->src_rect.y0; copy && y < c->src_rect.y1; y++) {
        size_t at = (size_t)y * t[k].stride + (size_t)c->src_rect.x0 * 4;
        memcpy(h.copies[k] + at, h.origin[k] + at, (size_t)(c->src_rect.x1 - c->src_rect.x0) * 4);
    }
}

/* Runs a batch: its buffer prologue, then its draws from the table. */
static void run_batch(const shr_surface *dst, const shr_draw_cmd *cmds, size_t n) {
    size_t p = prologue_len(cmds, n);
    for (size_t i = 0; i < p; i++) apply(h.table, &cmds[i], copies());
    FUZZ_CHECK(shr_software_execute(dst, cmds + p, n - p, h.table, NIDS) == SHR_OK);
}

static void see(const void *pixels, size_t length) {
    for (int k = 0; k < h.seen_srcs; k++)
        if (h.seen_src[k].pixels == pixels) return;
    FUZZ_CHECK(h.seen_srcs < 16);
    h.seen_src[h.seen_srcs].pixels = pixels, h.seen_src[h.seen_srcs].length = length;
    h.seen_src[h.seen_srcs++].hash = hash_bytes(pixels, length);
}

/* Remembers what the batch reads: COPY and ROTATE sources, and buffer memory (with copies only for REGISTER and
 * UPDATE). */
static void see_batch(const shr_draw_cmd *cmds, size_t n) {
    shr_image after[NIDS];
    memcpy(after, h.table, sizeof(after));
    size_t p = prologue_len(cmds, n);
    h.seen_srcs = 0;
    for (size_t i = 0; i < p; i++) {
        if (cmds[i].kind == SHR_CMD_BUFFER_UPDATE && copies()) {
            const shr_image *m = &h.table[cmds[i].buffer - 1];
            see(h.origin[cmds[i].buffer - 1], m->byte_length);
        }
        apply(after, &cmds[i], false);
        if (cmds[i].kind == SHR_CMD_BUFFER_REGISTER) see(cmds[i].src.pixels, fuzz_span(&cmds[i].src));
    }
    for (size_t i = p; i < n; i++) {
        const shr_draw_cmd *c = &cmds[i];
        if (c->kind == SHR_CMD_COPY || c->kind == SHR_CMD_ROTATE) see(c->src.pixels, fuzz_span(&c->src));
        if (c->kind != SHR_CMD_GLYPH && c->kind != SHR_CMD_IMAGE) continue;
        FUZZ_CHECK(c->buffer >= 1 && c->buffer <= h.caps.max_buffers);
        if (!copies()) see(after[c->buffer - 1].pixels, after[c->buffer - 1].byte_length);
    }
}

static shr_status d_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t n, shr_fence fence) {
    (void)user;
    int i = buf_index(dst->pixels);
    FUZZ_CHECK(i < 0 || h.buf_state[i] == BUF_ACQUIRED); /* only buffers the compositor owns */
    FUZZ_CHECK(!h.pending);                             /* one submission at a time */
    if (h.driver_fail > 0) {
        h.driver_fail--;
        for (size_t c = 0, p = prologue_len(cmds, n); h.fail_late && c < p; c++) apply(h.table, &cmds[c], copies());
        if (!h.fail_late) memset(h.table, 0, sizeof(h.table));
        return SHR_E_DEVICE;
    }
    if (h.driver_block > 0) {
        h.driver_block--;
        return SHR_E_WOULD_BLOCK;
    }
    if (!h.async || n > 512) {
        run_batch(dst, cmds, n);
        return SHR_OK;
    }
    h.pending = fence;
    h.pending_dst = dst, h.pending_cmds = cmds, h.pending_count = n;
    h.seen_dst = *dst;
    memcpy(h.seen_cmds, cmds, n * sizeof(*cmds));
#ifdef FUZZ_MSAN
    __msan_unpoison(h.seen_cmds, n * sizeof(*cmds)); /* fields a command leaves unspecified are compared as they are */
#endif
    see_batch(cmds, n);
    return SHR_IN_PROGRESS;
}

static void complete_pending(void) {
    if (!h.pending) return;
    static shr_draw_cmd cmds[512];
    memcpy(cmds, h.pending_cmds, h.pending_count * sizeof(*cmds));
#ifdef FUZZ_MSAN
    __msan_unpoison(cmds, h.pending_count * sizeof(*cmds));
#endif
    FUZZ_CHECK(!memcmp(&h.seen_dst, h.pending_dst, sizeof(h.seen_dst)) &&
               !memcmp(h.seen_cmds, cmds, h.pending_count * sizeof(*cmds)));
    for (int k = 0; k < h.seen_srcs; k++)
        FUZZ_CHECK(hash_bytes(h.seen_src[k].pixels, h.seen_src[k].length) == h.seen_src[k].hash);
    run_batch(h.pending_dst, h.pending_cmds, h.pending_count);
    shr_fence f = h.pending;
    h.pending = 0;
    shr_fence_signal(h.ctx, f, SHR_FENCE_SUCCEEDED);
}

static shr_status d_reset(void *user) {
    (void)user;
    if (!h.reset_ok) return SHR_E_DEVICE;
    h.pending = 0;
    memset(h.table, 0, sizeof(h.table));
    return SHR_OK;
}

static void d_cancel(void *user, shr_fence fence) { (void)user, (void)fence; }

static void drain_events(void) {
    shr_event ev;
    while (shr_poll_event(h.ctx, &ev) == SHR_OK) {
        FUZZ_CHECK(ev.kind != SHR_EVENT_OVERFLOW && ev.kind != SHR_EVENT_RESOURCE_FAILED);
        uint8_t *f = ev.frame_id < MAX_FRAMES ? &h.frames[ev.frame_id] : NULL;
        switch (ev.kind) {
        case SHR_EVENT_PRESENT_ACCEPTED:
            FUZZ_CHECK(ev.frame_id > h.last_accepted);
            h.last_accepted = ev.frame_id;
            if (f) {
                FUZZ_CHECK(*f == FRAME_NONE);
                *f = FRAME_ACCEPTED;
            }
            break;
        case SHR_EVENT_PRESENT_FAILED:
        case SHR_EVENT_FRAME_SUPERSEDED:
            FUZZ_CHECK(ev.kind == SHR_EVENT_FRAME_SUPERSEDED || ev.status != SHR_OK);
            if (f) {
                FUZZ_CHECK(*f == FRAME_NONE);
                *f = FRAME_ENDED;
            }
            break;
        case SHR_EVENT_FRAME_RELEASED:
            if (f) {
                FUZZ_CHECK(*f == FRAME_ACCEPTED);
                *f = FRAME_RELEASED;
            }
            break;
        case SHR_EVENT_FRAME_DISPLAYED:
            if (f) FUZZ_CHECK(*f == FRAME_ACCEPTED || *f == FRAME_RELEASED);
            break;
        default:
            break;
        }
    }
}

static void release_one(bool lose_contents, bool displayed) {
    if (!h.held_count) return;
    uint64_t v = h.held[0];
    memmove(h.held, h.held + 1, (size_t)(--h.held_count) * sizeof(h.held[0]));
    h.buf_state[v & 0xFF] = BUF_FREE;
    h.gen[v & 0xFF] += lose_contents;
    if (displayed) {
        shr_status st = shr_output_displayed(h.ctx, v >> 8, now);
        FUZZ_CHECK(st == SHR_OK || st == SHR_E_NOT_FOUND || st == SHR_E_WOULD_BLOCK);
    }
    FUZZ_CHECK(shr_output_released(h.ctx, v >> 8) == SHR_OK);
}

static void draw_model(const shr_surface *screen, shr_color clear) {
    shr_rect all = {0, 0, screen->width, screen->height};
    shr_draw_cmd c = {.kind = SHR_CMD_FILL, .dst = all, .color = clear};
    FUZZ_CHECK(shr_software_execute(screen, &c, 1, NULL, 0) == SHR_OK);
    bool done[NLAYERS] = {0};
    for (;;) {
        int k = -1;
        for (int i = 0; i < NLAYERS; i++) {
            const mlayer *l = &h.layers[i];
            if (!l->l || done[i]) continue;
            if (k < 0 || l->z < h.layers[k].z || (l->z == h.layers[k].z && l->seq < h.layers[k].seq)) k = i;
        }
        if (k < 0) break;
        done[k] = true;
        const mlayer *l = &h.layers[k];
        for (int i = 0; l->visible && i < l->n; i++) {
            const mcmd *m = &l->cmds[i];
            shr_rect clip = {l->rect.x0 > 0 ? l->rect.x0 : 0, l->rect.y0 > 0 ? l->rect.y0 : 0,
                             l->rect.x1 < all.x1 ? l->rect.x1 : all.x1, l->rect.y1 < all.y1 ? l->rect.y1 : all.y1};
            int64_t x0 = (int64_t)l->rect.x0 + m->rect.x0, y0 = (int64_t)l->rect.y0 + l->oy + m->rect.y0;
            int64_t x1 = (int64_t)l->rect.x0 + m->rect.x1, y1 = (int64_t)l->rect.y0 + l->oy + m->rect.y1;
            shr_rect d = {(int32_t)(x0 > clip.x0 ? x0 : clip.x0), (int32_t)(y0 > clip.y0 ? y0 : clip.y0),
                          (int32_t)(x1 < clip.x1 ? x1 : clip.x1), (int32_t)(y1 < clip.y1 ? y1 : clip.y1)};
            if (d.x0 >= d.x1 || d.y0 >= d.y1) continue;
            c = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = d, .color = m->color};
            shr_image table = {0};
            if (m->image) {
                const mimage *im = &h.images[m->img];
                /* Image pixel (0, 0) sits at the command's anchor. */
                int64_t ax = (int64_t)l->rect.x0 + m->at.x, ay = (int64_t)l->rect.y0 + l->oy + m->at.y;
                table = (shr_image){im->px, im->w, im->h, (size_t)im->w * 4, sizeof(im->px), SHR_FORMAT_RGBA8888,
                                    SHR_MEMORY_CPU};
                c.kind = SHR_CMD_IMAGE;
                c.buffer = 1;
                c.src_rect = (shr_rect){0, 0, im->w, im->h};
                c.src_origin = (shr_point){(int32_t)(d.x0 - ax), (int32_t)(d.y0 - ay)};
            }
            FUZZ_CHECK(shr_software_execute(screen, &c, 1, &table, 1) == SHR_OK);
        }
    }
}

/* Removes every fault, asks for the current state and lets everything finish; the output then shows the
 * model exactly. */
static void check_model(const shr_screen_desc *sd) {
    h.acquire_result = h.present_result = SHR_OK;
    h.driver_fail = h.driver_block = 0;
    h.async = false;
    FUZZ_CHECK(shr_submit(h.ctx) == SHR_OK);
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    for (int i = 0; i < 64 && (dl.kind == SHR_DEADLINE_NOW || (h.cap && dl.kind == SHR_DEADLINE_AT) || h.pending ||
                                h.held_count);
         i++) {
        if (h.cap && dl.kind == SHR_DEADLINE_AT) now = dl.at_ns;
        complete_pending();
        while (h.held_count) release_one(false, false);
        drain_events();
        shr_output_ready(h.ctx);
        shr_output_recover(h.ctx); /* SHR_E_WOULD_BLOCK until the queue has room */
        shr_driver_ready(h.ctx);
        FUZZ_CHECK(shr_pump(h.ctx) == SHR_OK);
        drain_events();
        FUZZ_CHECK(shr_next_deadline(h.ctx, &dl) == SHR_OK);
    }
    FUZZ_CHECK(dl.kind != SHR_DEADLINE_NOW && h.presented);
    for (int f = 0; f < MAX_FRAMES; f++) FUZZ_CHECK(!h.submitted[f] || h.frames[f] != FRAME_NONE);
    static uint8_t logical[FUZZ_W * FUZZ_H * 4], expect[FUZZ_W * FUZZ_H * 4];
    size_t row;
    shr_format_row_bytes(SHR_PIXEL_FORMAT, sd->width, &row);
    shr_surface screen = {logical, sd->width, sd->height, row, row * (size_t)sd->height, SHR_PIXEL_FORMAT, 0,
                          SHR_MEMORY_CPU, 0};
    draw_model(&screen, sd->clear);
    shr_format_row_bytes(h.ofmt, h.ow, &row);
    shr_surface o = {expect, h.ow, h.oh, row, row * (size_t)h.oh, h.ofmt, 0, SHR_MEMORY_CPU, 0};
    shr_draw_cmd c = {.kind = sd->rotation ? SHR_CMD_ROTATE : SHR_CMD_COPY,
                      .rotation = (uint8_t)sd->rotation,
                      .dst = {0, 0, h.ow, h.oh},
                      .src = {logical, screen.width, screen.height, (uint32_t)screen.stride, SHR_PIXEL_FORMAT,
                              SHR_MEMORY_CPU, 0}};
    FUZZ_CHECK(shr_software_execute(&o, &c, 1, NULL, 0) == SHR_OK);
    FUZZ_CHECK(memcmp(expect, h.shown, o.byte_length) == 0);
}

static bool image_used(int k) {
    for (int i = 0; i < NLAYERS; i++)
        for (int j = 0; h.layers[i].l && j < NCMDS; j++)
            if ((j < h.layers[i].n && h.layers[i].cmds[j].image && h.layers[i].cmds[j].img == k) ||
                (j < h.layers[i].np && h.layers[i].pend[j].image && h.layers[i].pend[j].img == k))
                return true;
    return false;
}

static void image_pixels(uint8_t *px, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) px[i] = (uint8_t)(seed + i * 37u);
    for (size_t i = 3; i < n; i += 4) px[i] = seed & 1 ? 255 : (uint8_t)(i * 11u); /* opaque or mixed alpha */
}

static int run_input(const uint8_t *data, size_t size, uint64_t cap) {
    memset(&h, 0, sizeof(h));
    h.cap = cap;
    now = 0;
    fuzz_reader r = {data, size};
    uint8_t setup = fr_u8(&r);
    h.release_on_present = setup & 1;
    h.reset_ok = setup & 2;

    shr_framebuffer_driver drv;
    shr_framebuffer_driver_init(&drv);
    drv.execute = d_execute, drv.reset = d_reset, drv.cancel = d_cancel;
    drv.caps.timeout_ns = (setup & 4) ? 1000 : 0;
    drv.caps.max_buffers = (setup & 64) ? NIMAGES : NIDS; /* each image draws from one of its two buffers */
    drv.caps.buffer_flags = (setup & 128) ? SHR_BUFFER_COPIES : 0;
    drv.caps.buffer_bytes = (setup & 128) ? NIMAGES * IMAGE_BYTES : 0;
    drv.caps.max_keeps = (setup & 64) ? 2 : 0;
    drv.caps.flags = SHR_DRIVER_CHEAP_MOVE | ((setup & 65) == 65 ? SHR_DRIVER_CHEAP_STORE : 0); /* keeps, half the time */
    h.caps = drv.caps;
    shr_output out;
    shr_output_init(&out);
    out.flags = (h.release_on_present ? SHR_OUTPUT_RELEASE_ON_PRESENT : 0u) |
                ((setup & 8) ? SHR_OUTPUT_PRESERVES_CONTENT : 0u);
    out.timestamp = SHR_TIMESTAMP_VSYNC;
    out.acquire = h_acquire, out.present = h_present, out.discard = h_discard;
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.driver = &drv, cd.output = &out, cd.now_ns = clock_fn, cd.trace = trace_fn;
    cd.event_capacity = (setup & 16) ? 4 : 256;
    cd.max_unreleased_frames = 2;
    cd.image_bytes = 3 * IMAGE_BYTES + 64; /* a second buffer for some images */
    cd.min_frame_interval_ns = cap;
    FUZZ_CHECK(shr_create(&cd, &h.ctx) == SHR_OK);
    shr_context *ctx = h.ctx;
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = FUZZ_W, sd.height = FUZZ_H, sd.clear = SHR_RGB(1, 2, 3);
    h.ow = FUZZ_W, h.oh = FUZZ_H, h.ofmt = SHR_PIXEL_FORMAT;
    FUZZ_CHECK(shr_screen_configure(ctx, &sd) == SHR_OK);

    for (int steps = 0; r.n > 0 && steps < 256; steps++) {
        uint8_t op = fr_u8(&r), arg = fr_u8(&r);
        mlayer *l = &h.layers[arg % NLAYERS];
        mimage *im = &h.images[arg % NIMAGES];
        shr_status st;
        switch (op % 26) {
        case 0:
            if (l->l) break;
            *l = (mlayer){.z = fr_i8(&r) % 4, .rect = fr_rect(&r), .visible = true};
            st = shr_lyr_create(ctx, l->z, l->rect, &l->l);
            if (st == SHR_OK) l->seq = h.seq++;
            break;
        case 1:
            if (!l->l) break;
            FUZZ_CHECK(shr_lyr_destroy(l->l) == SHR_OK);
            l->l = NULL;
            break;
        case 2: {
            shr_rect rect = fr_rect(&r);
            if (l->l && shr_lyr_set_rect(l->l, rect) == SHR_OK) l->rect = rect;
            break;
        }
        case 3: {
            int32_t z = fr_i8(&r) % 4;
            if (l->l && shr_lyr_set_z(l->l, z) == SHR_OK) l->z = z;
            break;
        }
        case 4:
            if (l->l && shr_lyr_set_visible(l->l, arg & 0x80) == SHR_OK) l->visible = (arg & 0x80) != 0;
            break;
        case 5:
            if (l->l && shr_lyr_cmd_begin(l->l) == SHR_OK) l->building = true, l->np = 0;
            break;
        case 6: {
            shr_rect rect = fr_rect(&r);
            shr_color color = SHR_RGB(fr_u8(&r), arg, 77);
            if (!l->l || l->np == NCMDS) break;
            st = shr_lyr_cmd_fill(l->l, rect, color);
            FUZZ_CHECK(l->building || st != SHR_OK);
            if (st == SHR_OK) l->pend[l->np++] = (mcmd){false, rect, color, 0, {0, 0}};
            break;
        }
        case 7: {
            int k = fr_u8(&r) % NIMAGES;
            shr_rect src = fr_rect(&r);
            shr_point at = {fr_i8(&r), fr_i8(&r)};
            if (!l->l || l->np == NCMDS || !h.images[k].img || h.images[k].released) break; /* may be freed */
            st = shr_lyr_cmd_image(l->l, h.images[k].img, src, at);
            bool empty = src.x0 == src.x1 || src.y0 == src.y1;
            if (st == SHR_OK && !empty) {
                FUZZ_CHECK(l->building);
                shr_rect area = {at.x, at.y, at.x + src.x1 - src.x0, at.y + src.y1 - src.y0};
                l->pend[l->np++] = (mcmd){true, area, 0, k, {at.x - src.x0, at.y - src.y0}};
            }
            break;
        }
        case 8:
            if (l->l && shr_lyr_cmd_commit(l->l) == SHR_OK) {
                FUZZ_CHECK(l->building);
                memcpy(l->cmds, l->pend, sizeof(l->cmds));
                l->n = l->np, l->np = 0, l->building = false, l->oy = 0;
            }
            break;
        case 9: {
            int32_t w = 1 + fr_u8(&r) % 8, hh = 1 + fr_u8(&r) % 8;
            if (im->img && (!im->released || image_used((int)(im - h.images)))) break;
            uint8_t px[8 * 8 * 4];
            image_pixels(px, (size_t)w * hh * 4, arg);
            shr_pl_res_image *img;
            if (shr_pl_res_image_create(ctx, w, hh, px, (size_t)w * 4, &img) == SHR_OK) {
                *im = (mimage){img, false, w, hh, {0}};
                memcpy(im->px, px, (size_t)w * hh * 4);
            }
            break;
        }
        case 10: {
            shr_rect rect = fr_rect(&r);
            uint8_t px[8 * 8 * 4];
            image_pixels(px, sizeof(px), arg);
            if (!im->img || im->released) break;
            st = shr_pl_res_image_update(im->img, rect, px, 8 * 4);
            if (st != SHR_OK) break;
            for (int32_t y = rect.y0; y < rect.y1; y++)
                memcpy(im->px + ((size_t)y * im->w + rect.x0) * 4, px + (size_t)(y - rect.y0) * 32,
                       (size_t)(rect.x1 - rect.x0) * 4);
            break;
        }
        case 11:
            if (!im->img || im->released) break;
            FUZZ_CHECK(shr_pl_res_image_release(im->img) == SHR_OK);
            im->released = true;
            break;
        case 12:
            FUZZ_CHECK(shr_submit(ctx) == SHR_OK);
            break;
        case 13:
            FUZZ_CHECK(shr_pump(ctx) == SHR_OK);
            break;
        case 14: { /* the layer's group moves down by dy with the pixels of the whole layer, or leaves */
            int32_t dy = fr_i8(&r) % 24;
            if (!l->l) break;
            shr__lyr_groups_shift(l->l, 0, 1, (arg & 0x80) != 0, (shr_rect){0, 0, l->rect.x1 - l->rect.x0,
                                  l->rect.y1 - l->rect.y0}, dy);
            l->oy += dy;
            if (arg & 0x80) l->n = 0;
            break;
        }
        case 15: {
            static const shr_status res[] = {SHR_OK, SHR_OK, SHR_E_WOULD_BLOCK, SHR_E_DEVICE};
            h.present_result = res[arg % 4];
            break;
        }
        case 16:
            h.acquire_result = arg % 4 == 0 ? SHR_E_WOULD_BLOCK : arg % 4 == 1 ? SHR_E_DEVICE : SHR_OK;
            break;
        case 17:
            h.driver_fail = arg % 3, h.driver_block = (arg >> 2) % 3, h.fail_late = arg & 0x40;
            break;
        case 18:
            shr_output_ready(ctx);
            if (arg & 1) shr_driver_ready(ctx);
            if (arg & 2) shr_asset_ready(ctx);
            break;
        case 19:
            now += arg * 10u;
            break;
        case 20:
            FUZZ_CHECK(shr_request_redraw(ctx) == SHR_OK);
            break;
        case 21:
            release_one(arg & 1, arg & 2);
            break;
        case 22: {
            shr_deadline dl;
            FUZZ_CHECK(shr_next_deadline(ctx, &dl) == SHR_OK);
            if (dl.kind == SHR_DEADLINE_AT) FUZZ_CHECK(dl.at_ns > now);
            break;
        }
        case 23:
            if (arg & 1)
                complete_pending();
            else
                h.async = arg & 2;
            break;
        case 24:
            if (arg & 1)
                FUZZ_CHECK(shr_output_error(ctx, SHR_E_DEVICE) == SHR_OK);
            else
                shr_output_recover(ctx);
            break;
        case 25: {
            shr_screen_desc next = sd;
            next.rotation = (shr_rotation)(arg & 3);
            next.flags = arg & 4 ? SHR_SCREEN_COMPOSITION : 0;
            next.output_format = arg & 8 ? (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? SHR_FORMAT_RGBX8888
                                                                                  : SHR_FORMAT_RGB565)
                                         : 0;
            next.clear = SHR_RGB(arg, 2, 3);
            next.bands = NULL, next.band_count = 0, next.band_align = 0;
            if (arg & 0x10) {
                check_model(&sd);
                break;
            }
            if (arg & 0x20) { /* 1 or 2 bands of a height that is a multiple of the alignment */
                static const uint32_t aligns[6] = {0, 1, 2, 4, 8, 16};
                uint8_t hb = fr_u8(&r), ab = fr_u8(&r);
                int32_t a = aligns[ab % 6] ? (int32_t)aligns[ab % 6] : 1, bh = a * (1 + hb % (FUZZ_H / a));
                size_t row;
                shr_format_row_bytes(SHR_PIXEL_FORMAT, FUZZ_W, &row);
                for (int k = 0; k < 2; k++)
                    h.bands[k] = (shr_surface){h.band_px[k], FUZZ_W, bh, row, row * (size_t)bh, SHR_PIXEL_FORMAT, 1,
                                               SHR_MEMORY_CPU, 0};
                next.bands = h.bands, next.band_count = 1 + (arg >> 6 & 1), next.band_align = aligns[ab % 6];
                next.flags = 0;
            }
            if (shr_screen_configure(ctx, &next) != SHR_OK) break;
            sd = next;
            bool quarter = sd.rotation == SHR_ROTATE_90_CW || sd.rotation == SHR_ROTATE_90_CCW;
            h.ow = quarter ? sd.height : sd.width, h.oh = quarter ? sd.width : sd.height;
            h.ofmt = sd.output_format ? sd.output_format : SHR_PIXEL_FORMAT;
            h.presented = false;
            break;
        }
        }
        if (!(op & 0x80)) drain_events();
    }
    if (setup & 32) check_model(&sd);

    for (int i = 0; i < NLAYERS; i++)
        if (h.layers[i].l) FUZZ_CHECK(shr_lyr_destroy(h.layers[i].l) == SHR_OK);
    for (int i = 0; i < NIMAGES; i++)
        if (h.images[i].img && !h.images[i].released) FUZZ_CHECK(shr_pl_res_image_release(h.images[i].img) == SHR_OK);
    FUZZ_CHECK(shr_begin_shutdown(ctx) == SHR_OK);
    h.present_result = h.acquire_result = SHR_OK;
    h.driver_fail = h.driver_block = 0;
    h.async = false;
    shr_status st = SHR_E_WOULD_BLOCK;
    for (int i = 0; i < 16 && st == SHR_E_WOULD_BLOCK; i++) {
        complete_pending();
        shr_output_ready(ctx);
        shr_output_recover(ctx);
        shr_driver_ready(ctx);
        shr_pump(ctx);
        while (h.held_count) release_one(false, false);
        drain_events();
        shr_deadline dl;
        FUZZ_CHECK(shr_next_deadline(ctx, &dl) == SHR_OK);
        if (dl.kind == SHR_DEADLINE_NOW) continue;
        for (int f = 0; f < MAX_FRAMES; f++) FUZZ_CHECK(!h.submitted[f] || h.frames[f] != FRAME_NONE);
        st = shr_destroy(ctx);
    }
    FUZZ_CHECK(st == SHR_OK);
    for (int i = 0; i < NBUF; i++) FUZZ_CHECK(h.buf_state[i] == BUF_FREE);
    for (uint64_t f = 1; f <= h.last_accepted && f < MAX_FRAMES; f++) FUZZ_CHECK(h.frames[f] != FRAME_ACCEPTED);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    run_input(data, size, 0);
    return run_input(data, size, CAP_NS);
}
