#include "harness.h"
#include "shr_compositor.h"

#define RED 0xFF0000u
#define GREEN 0x00FF00u
#define BLUE 0x0000FFu
#define PRESERVED (SHR_OUTPUT_PRESERVES_CONTENT | SHR_OUTPUT_RELEASE_ON_PRESENT)
#define FULL ((shr_rect){0, 0, HW, HH})

/* 2x2: opaque red, transparent, half green, opaque blue. */
static const uint8_t quad[16] = {255, 0, 0, 255, 0, 0, 0, 0, 0, 255, 0, 128, 0, 0, 255, 255};

static struct {
    int frames;
    uint64_t damaged;
} rec;

static void rec_trace(void *user, const shr_trace_event *ev) {
    (void)user;
    if (ev->kind == SHR_TRACE_RASTER_BEGIN) rec.frames++, rec.damaged = ev->value1;
}

static uint64_t budget_bytes;
static shr_driver_caps caps; /* applied when max_buffers is set */
static fail_alloc oom;
static shr_allocator oom_allocator;
static void tweak(shr_context_desc *d, shr_framebuffer_driver *drv) {
    if (caps.max_buffers) drv->caps = caps;
    memset(&rec, 0, sizeof(rec));
    d->trace = rec_trace;
    d->image_bytes = budget_bytes ? budget_bytes : d->image_bytes;
    oom = (fail_alloc){-1, 0};
    oom_allocator = fail_allocator(&oom);
    d->allocator = &oom_allocator;
}

static void frame(shr_context *ctx) {
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_pump(ctx), SHR_OK);
}

static shr_lyr *image_layer(shr_context *ctx, int32_t z, shr_pl_res_image *img, shr_rect src, shr_point at) {
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, z, FULL, &l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, img, src, at), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    return l;
}

static void set_commands(shr_lyr *l, size_t n, shr_pl_res_image *const *img) {
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    for (size_t i = 0; i < n; i++)
        ASSERT_EQ_LL(shr_lyr_cmd_image(l, img[i], (shr_rect){0, 0, 2, 2}, (shr_point){(int32_t)i * 4, 0}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
}

static int channel(uint32_t c, int shift) { return (int)(c >> shift & 255); }

TEST test_image_create_arguments(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak);
    shr_pl_res_image *img = (shr_pl_res_image *)&h;
    ASSERT_EQ_LL(shr_pl_res_image_create(NULL, 2, 2, quad, 8, &img), SHR_E_INVALID_ARG);
    ASSERT(img == NULL);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, NULL, 8, &img), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 0, 2, quad, 8, &img), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, -1, quad, 8, &img), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 7, &img), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, INT32_MAX, INT32_MAX, quad, SIZE_MAX, &img), SHR_E_LIMIT);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_E_STATE);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

TEST test_image_budget(void) {
    harness h;
    budget_bytes = 64;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    budget_bytes = 0;
    static const uint8_t px[64];
    shr_pl_res_image *a, *b, *c;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 5, 4, px, 20, &a), SHR_E_LIMIT); /* larger than the budget */
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 4, px, 8, &a), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 4, px, 8, &b), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 1, 1, px, 4, &c), SHR_E_LIMIT); /* the budget is used up */
    ASSERT_EQ_LL(shr_pl_res_image_release(a), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 1, 1, px, 4, &c), SHR_E_LIMIT); /* freed by the next pump */
    shr_pump(ctx);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 4, px, 8, &c), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(b), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(c), SHR_OK);
    long live = oom.live;
    shr_pump(ctx); /* the last image also frees the budget */
    ASSERT(oom.live < live - 4);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 4, 4, px, 16, &a), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(a), SHR_OK);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);

    /* The budget counts the driver's padded rows; images beyond its buffer size are not made. */
    budget_bytes = 64;
    caps = (shr_driver_caps){.domains = SHR_MEMORY_CPU, .stride_align = 32, .max_buffers = HBUFS,
                             .max_buffer_width = 8, .max_buffer_height = 8};
    ctx = harness_open(&h, 0, tweak);
    budget_bytes = 0;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 9, 1, px, 36, &b), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 1, 1, px, 4, &a), SHR_OK); /* 4 bytes, but a row of 32 */
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, px, 8, &b), SHR_E_LIMIT);
    ASSERT_EQ_LL(shr_pl_res_image_release(a), SHR_OK);
    harness_close(&h);
    caps = (shr_driver_caps){0};
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

TEST test_image_create_out_of_memory(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak);
    shr_pl_res_image *first = NULL;
    for (int round = 0; round < 2; round++) { /* the first image also allocates the budget */
        for (long budget = 0;; budget++) {
            long live = oom.live;
            shr_pl_res_image *img = (shr_pl_res_image *)&h;
            oom.budget = budget;
            shr_status st = shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img);
            oom.budget = -1;
            if (st == SHR_OK) {
                if (!first) first = img;
                else ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
                break;
            }
            ASSERT(st == SHR_E_NO_MEMORY && img == NULL && oom.live == live);
        }
    }
    ASSERT_EQ_LL(shr_pl_res_image_release(first), SHR_OK);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

TEST test_image_needs_plugin_slot(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak);
    static const char kinds[8];
    for (int i = 0; i < 8; i++) ASSERT(shr__ctx_plugin_slot(ctx, &kinds[i]) != NULL);
    shr_pl_res_image *img;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_E_LIMIT);
    harness_close(&h);
    PASS();
}

/* Straight alpha, source over what lies below; src picks a part of the image. */
TEST test_image_blends_over_layers_below(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    shr_lyr *below;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &below), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(below), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_fill(below, FULL, SHR_RGB(255, 255, 255)), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(below), SHR_OK);
    uint8_t padded[2][12] = {{0}}; /* rows with a stride beyond the image */
    memcpy(padded[0], quad, 8), memcpy(padded[1], quad + 8, 8);
    shr_pl_res_image *img;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, padded, 12, &img), SHR_OK);
    shr_lyr *above;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 1, FULL, &above), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(above), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(above, img, (shr_rect){0, 0, 2, 2}, (shr_point){4, 4}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(above, img, (shr_rect){1, 1, 2, 2}, (shr_point){10, 10}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(above, img, (shr_rect){0, 0, 2, 2}, (shr_point){-1, -1}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(above), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 4, 4), RED);
    ASSERT_EQ_LL(px(h.out.shown, 5, 4), 0xFFFFFFu); /* transparent */
    uint32_t half = px(h.out.shown, 4, 5);
    ASSERT(channel(half, 8) == 255 && abs(channel(half, 16) - 127) < 12 && abs(channel(half, 0) - 127) < 12);
    ASSERT_EQ_LL(px(h.out.shown, 5, 5), BLUE);
    ASSERT(px(h.out.shown, 10, 10) == BLUE && px(h.out.shown, 11, 10) == 0xFFFFFFu && px(h.out.shown, 10, 11) == 0xFFFFFFu);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), BLUE); /* clipped at the screen edge */
    ASSERT_EQ_LL(shr_lyr_destroy(above), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_destroy(below), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    harness_close(&h);
    PASS();
}

TEST test_cmd_image_validation(void) {
    harness h, other;
    shr_context *ctx = harness_open(&h, 0, tweak);
    shr_context *octx = harness_open(&other, 0, NULL);
    shr_pl_res_image *img, *foreign, *gone;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &gone), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_create(octx, 2, 2, quad, 8, &foreign), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(gone), SHR_OK);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr_rect all = {0, 0, 2, 2};
    const shr_point o = {0, 0};
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, img, all, o), SHR_E_STATE); /* outside begin/commit */
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(NULL, img, all, o), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, NULL, all, o), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, gone, all, o), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, foreign, all, o), SHR_E_INVALID_ARG);
    const shr_rect bad_src[6] = {{1, 0, 0, 2}, {-1, 0, 2, 2}, {0, -1, 2, 2}, {0, 0, 3, 2}, {0, 0, 2, 3}, {0, 1, 2, 0}};
    for (int i = 0; i < 6; i++) ASSERT_EQ_LL(shr_lyr_cmd_image(l, img, bad_src[i], o), SHR_E_INVALID_ARG);
    const shr_point bad_at[4] = {{INT32_MAX, 0}, {0, INT32_MAX}, {INT32_MIN, 0}, {0, INT32_MIN}};
    const shr_rect shifted = {1, 1, 2, 2};
    for (int i = 0; i < 4; i++) ASSERT_EQ_LL(shr_lyr_cmd_image(l, img, i < 2 ? all : shifted, bad_at[i]), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, img, (shr_rect){1, 1, 1, 2}, o), SHR_OK); /* empty: draws nothing */
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, img, (shr_rect){0, 0, 1, 1}, (shr_point){INT32_MAX - 1, INT32_MAX - 1}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, img, (shr_rect){1, 1, 1, 2}, o), SHR_E_STATE); /* outside begin/commit */
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(foreign), SHR_OK);
    shr_output_released(ctx, 1);
    harness_close(&h);
    harness_close(&other);
    PASS();
}

TEST test_image_update_arguments(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak);
    shr_pl_res_image *img, *gone;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &gone), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(gone), SHR_OK);
    const shr_rect all = {0, 0, 2, 2};
    ASSERT_EQ_LL(shr_pl_res_image_update(NULL, all, quad, 8), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_update(gone, all, quad, 8), SHR_E_INVALID_ARG);
    const shr_rect bad[5] = {{1, 0, 0, 2}, {-1, 0, 2, 2}, {0, -1, 2, 2}, {0, 0, 3, 2}, {0, 0, 2, 3}};
    for (int i = 0; i < 5; i++) ASSERT_EQ_LL(shr_pl_res_image_update(img, bad[i], quad, 8), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){1, 1, 1, 2}, NULL, 0), SHR_OK); /* empty */
    ASSERT_EQ_LL(shr_pl_res_image_update(img, all, NULL, 8), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, all, quad, 7), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){1, 0, 2, 2}, quad, 4), SHR_OK);
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, all, quad, 8), SHR_E_STATE);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){1, 1, 1, 2}, NULL, 0), SHR_E_STATE);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK); /* allowed during shutdown */
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

/* An update reaches the screen in every buffer, and only where the image is drawn. */
TEST test_image_update_damages_every_buffer(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak);
    shr_pl_res_image *img;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_OK);
    shr_lyr *l = image_layer(ctx, 0, img, (shr_rect){0, 0, 2, 2}, (shr_point){8, 8});
    frame(ctx);
    h.out.busy[0] = true;
    frame(ctx);
    h.out.busy[0] = false;
    static const uint8_t green[4] = {0, 255, 0, 255};
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){0, 0, 1, 1}, green, 4), SHR_OK);
    frame(ctx);
    ASSERT(h.out.last_buf == 0 && rec.damaged == 1 && px(h.out.shown, 8, 8) == GREEN);
    h.out.busy[0] = true;
    frame(ctx);
    ASSERT(h.out.last_buf == 1 && rec.damaged == 1 && px(h.out.shown, 8, 8) == GREEN);
    h.out.busy[0] = false;
    ASSERT_EQ_LL(px(h.out.shown, 9, 9), BLUE);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    harness_close(&h);
    PASS();
}

/* A frame reads the pixels it resolved until it ends, and release waits for it. An update meanwhile needs room
 * for a second buffer in the budget. */
TEST test_image_pinned_by_frames(void) {
    harness h;
    budget_bytes = 16;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    budget_bytes = 0;
    shr_pl_res_image *img, *next;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_OK);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr_pl_res_image *twice[2] = {img, img};
    set_commands(l, 2, twice);
    h.drv.async = true;
    frame(ctx);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){0, 0, 1, 1}, quad, 4), SHR_E_LIMIT);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK); /* no command refers to it any more */
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    shr_pump(ctx);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &next), SHR_E_LIMIT); /* still read */
    md_complete(&h.drv);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &next), SHR_OK);

    /* Released while a command still refers to it: freed once the command is gone. */
    h.drv.async = false;
    set_commands(l, 1, &next);
    frame(ctx);
    ASSERT_EQ_LL(shr_pl_res_image_update(next, (shr_rect){0, 0, 1, 1}, quad, 4), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(next), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_update(next, (shr_rect){0, 0, 1, 1}, quad, 4), SHR_E_INVALID_ARG);
    frame(ctx);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_E_LIMIT);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    shr_pump(ctx);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

/* The memory the pending batch registered for buffer `id`. */
static const uint8_t *pending_buffer(const harness *h, uint32_t id) {
    for (size_t i = 0; i < h->drv.pending_count; i++)
        if (h->drv.pending_cmds[i].kind == SHR_CMD_BUFFER_REGISTER && h->drv.pending_cmds[i].buffer == id)
            return h->drv.pending_cmds[i].src.pixels;
    return NULL;
}

/* With bands a frame resolves band after band, also across updates while an earlier band still runs: every band
 * draws the buffer the frame pinned first, which no update writes; the next frame draws the new pixels. */
TEST test_image_pinned_across_bands(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    static uint8_t band_px[2][HW * 16 * 4];
    shr_surface bands[2];
    for (int k = 0; k < 2; k++)
        bands[k] = (shr_surface){band_px[k], HW, 16, HW * SCREEN_BPP, HW * 16 * SCREEN_BPP, SHR_PIXEL_FORMAT, 1, 0, 0};
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = HW, sd.height = HH, sd.bands = bands, sd.band_count = 2;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_OK);
    shr_pl_res_image *img;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_OK);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, img, (shr_rect){0, 0, 2, 2}, (shr_point){0, 0}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(l, img, (shr_rect){0, 0, 2, 2}, (shr_point){0, 20}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    h.drv.async = true;
    frame(ctx);
    const uint8_t *first = pending_buffer(&h, 1);
    static const uint8_t blue[16] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};
    ASSERT(first && h.drv.pending_dst->pixels == band_px[0]);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){0, 0, 2, 2}, blue, 8), SHR_OK);
    while (h.drv.pending && h.drv.pending_dst->pixels != band_px[1]) md_complete(&h.drv), shr_pump(ctx);
    ASSERT_EQ_LL(h.drv.pending_dst->pixels, band_px[1]);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){0, 0, 1, 1}, blue, 8), SHR_OK);
    while (h.drv.pending) md_complete(&h.drv), shr_pump(ctx);
    ASSERT(h.out.presents == 1 && px(h.out.shown, 0, 0) == RED && px(h.out.shown, 0, 20) == RED);
    frame(ctx);
    while (h.drv.pending) md_complete(&h.drv), shr_pump(ctx);
    ASSERT(h.out.presents == 2 && px(h.out.shown, 0, 0) == BLUE && px(h.out.shown, 0, 20) == BLUE);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}


/* An image released and unused while a frame is built frees its id, which stays taken until a plan releases it: the
 * image drawn in a later band of that frame gets another id, so the next frame registers nothing again. Released
 * before the frame (freed by the pump that started it) and after its first band. */
TEST test_image_id_freed_while_a_frame_is_built(void) {
    static uint8_t band_px[2][HW * 16 * 4], big[8 * 8 * 4];
    memset(big, 255, sizeof(big));
    for (int late = 0; late < 2; late++) {
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
        shr_surface bands[2];
        for (int k = 0; k < 2; k++)
            bands[k] = (shr_surface){band_px[k], HW, 16, HW * SCREEN_BPP, HW * 16 * SCREEN_BPP, SHR_PIXEL_FORMAT, 1, 0, 0};
        shr_screen_desc sd;
        shr_screen_desc_init(&sd);
        sd.width = HW, sd.height = HH, sd.bands = bands, sd.band_count = 2;
        ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_OK);
        shr_pl_res_image *a, *b;
        ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 8, 8, big, 32, &a), SHR_OK);
        shr_lyr *la = image_layer(ctx, 0, a, (shr_rect){0, 0, 8, 8}, (shr_point){0, 0});
        frame(ctx);
        ASSERT_EQ_LL(shr_lyr_cmd_begin(la), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_commit(la), SHR_OK);
        if (!late) ASSERT_EQ_LL(shr_pl_res_image_release(a), SHR_OK);
        ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &b), SHR_OK);
        shr_lyr *lb = image_layer(ctx, 1, b, (shr_rect){0, 0, 2, 2}, (shr_point){0, 40});
        h.drv.async = true;
        frame(ctx);
        ASSERT(h.drv.pending && h.drv.pending_dst->pixels == band_px[0]);
        if (late) ASSERT_EQ_LL(shr_pl_res_image_release(a), SHR_OK);
        while (h.drv.pending) md_complete(&h.drv), shr_pump(ctx);
        int registers = h.drv.registers;
        shr_request_redraw(ctx);
        frame(ctx);
        while (h.drv.pending) md_complete(&h.drv), shr_pump(ctx);
        ASSERT_EQ_LL(h.drv.registers, registers);
        ASSERT_EQ_LL(h.out.presents, 3);
        ASSERT_EQ_LL(shr_lyr_destroy(la), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_destroy(lb), SHR_OK);
        ASSERT_EQ_LL(shr_pl_res_image_release(b), SHR_OK);
        harness_close(&h);
        ASSERT_EQ_LL(oom.live, 0);
    }
    PASS();
}

/* An update while a frame reads the image goes to a second buffer: the frame in flight keeps the old pixels, the
 * next frame draws the new ones. The two buffers then take turns, each brought level before it is written. */
TEST test_image_double_buffer(void) {
    harness h;
    budget_bytes = 35;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    budget_bytes = 0;
    shr_pl_res_image *img, *more;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_OK);
    shr_lyr *l = image_layer(ctx, 0, img, (shr_rect){0, 0, 2, 2}, (shr_point){8, 8});
    static const uint8_t green[4] = {0, 255, 0, 255}, blue[8] = {0, 0, 255, 255, 0, 0, 255, 255};
    h.drv.async = true;
    frame(ctx);
    const uint8_t *first = pending_buffer(&h, 1);
    uint8_t before[16];
    memcpy(before, first, 16);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){0, 0, 1, 1}, green, 4), SHR_OK);
    ASSERT(memcmp(before, first, 16) == 0); /* the frame in flight still reads these */
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 1, 1, green, 4, &more), SHR_E_LIMIT); /* the budget counts both */
    md_complete(&h.drv);
    shr_pump(ctx);
    ASSERT(h.out.presents == 1 && px(h.out.shown, 8, 8) == RED);
    frame(ctx);
    const uint8_t *second = pending_buffer(&h, 2);
    ASSERT(second && second != first);
    md_complete(&h.drv);
    shr_pump(ctx);
    ASSERT(h.out.presents == 2 && px(h.out.shown, 8, 8) == GREEN && px(h.out.shown, 9, 9) == BLUE);

    frame(ctx); /* now the second buffer is read */
    ASSERT_EQ_LL(h.drv.pending_count, 2);
    memcpy(before, second, 16);
    int updates = h.drv.updates;
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){0, 1, 2, 2}, blue, 8), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){1, 1, 2, 2}, green, 4), SHR_OK); /* in place */
    ASSERT(memcmp(before, second, 16) == 0);
    md_complete(&h.drv);
    shr_pump(ctx);
    frame(ctx);
    ASSERT(h.drv.pending_count == 3 && h.drv.pending_cmds[0].kind == SHR_CMD_BUFFER_UPDATE);
    shr_rect up = h.drv.pending_cmds[0].src_rect; /* the green pixel it missed and the new row */
    ASSERT(h.drv.pending_cmds[0].buffer == 1 && up.x0 == 0 && up.y0 == 0 && up.x1 == 2 && up.y1 == 2);
    md_complete(&h.drv);
    shr_pump(ctx);
    ASSERT(h.drv.updates == updates + 1 && px(h.out.shown, 8, 8) == GREEN && px(h.out.shown, 8, 9) == BLUE);
    ASSERT_EQ_LL(px(h.out.shown, 9, 9), GREEN);

    h.drv.async = false; /* not read by a frame: written in place */
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){1, 0, 2, 1}, green, 4), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 9, 8), GREEN);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

/* The second buffer may not be had: the update fails and changes nothing. */
TEST test_image_second_buffer_out_of_memory(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    shr_pl_res_image *img;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &img), SHR_OK);
    shr_lyr *l = image_layer(ctx, 0, img, (shr_rect){0, 0, 2, 2}, (shr_point){0, 0});
    h.drv.async = true;
    frame(ctx);
    static const uint8_t green[4] = {0, 255, 0, 255};
    oom.budget = 0;
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){0, 0, 1, 1}, green, 4), SHR_E_NO_MEMORY);
    oom.budget = -1;
    md_complete(&h.drv);
    shr_pump(ctx);
    shr_request_redraw(ctx);
    h.drv.async = false;
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), RED);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(test_image_create_arguments);
    RUN_TEST(test_image_budget);
    RUN_TEST(test_image_create_out_of_memory);
    RUN_TEST(test_image_needs_plugin_slot);
    RUN_TEST(test_image_blends_over_layers_below);
    RUN_TEST(test_cmd_image_validation);
    RUN_TEST(test_image_update_arguments);
    RUN_TEST(test_image_update_damages_every_buffer);
    RUN_TEST(test_image_pinned_by_frames);
    RUN_TEST(test_image_double_buffer);
    RUN_TEST(test_image_pinned_across_bands);
    RUN_TEST(test_image_id_freed_while_a_frame_is_built);
    RUN_TEST(test_image_second_buffer_out_of_memory);
    GREATEST_MAIN_END();
}
