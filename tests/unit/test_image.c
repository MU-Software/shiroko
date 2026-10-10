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
static uint32_t caps_flags;  /* added to the driver's */
static fail_alloc oom;
static shr_allocator oom_allocator;
static void tweak(shr_context_desc *d, shr_framebuffer_driver *drv) {
    if (caps.max_buffers) drv->caps = caps;
    drv->caps.flags |= caps_flags;
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
    uint64_t used = 0, limit = 0;
    ASSERT_EQ_LL(shr_pl_res_image_budget(NULL, &used, &limit), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_budget(ctx, &used, &limit), SHR_OK);
    ASSERT(used == 64 && limit == 64);
    ASSERT_EQ_LL(shr_pl_res_image_release(a), SHR_OK); /* nothing used it: freed at once */
    ASSERT_EQ_LL(shr_pl_res_image_budget(ctx, &used, NULL), SHR_OK);
    ASSERT_EQ_LL(used, 32);
    limit = 0;
    ASSERT_EQ_LL(shr_pl_res_image_budget(ctx, NULL, &limit), SHR_OK);
    ASSERT_EQ_LL(limit, 64);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 4, px, 8, &c), SHR_OK);
    long live = oom.live;
    ASSERT_EQ_LL(shr_pl_res_image_release(b), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(c), SHR_OK); /* the last image also frees the budget */
    ASSERT(oom.live < live - 4);
    ASSERT_EQ_LL(shr_pl_res_image_budget(ctx, &used, &limit), SHR_OK);
    ASSERT(used == 0 && limit == 64);
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
    uint64_t used = 1;
    ASSERT_EQ_LL(shr_pl_res_image_budget(ctx, &used, NULL), SHR_OK);
    ASSERT_EQ_LL(used, 0);
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
    shr_lyr *hold = image_layer(ctx, 1, gone, (shr_rect){0, 0, 2, 2}, (shr_point){0, 0}); /* released, still used */
    ASSERT_EQ_LL(shr_pl_res_image_release(gone), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(gone), SHR_E_INVALID_ARG);
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
    ASSERT_EQ_LL(shr_lyr_destroy(hold), SHR_OK);
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
    shr_lyr *hold = image_layer(ctx, 1, gone, (shr_rect){0, 0, 2, 2}, (shr_point){0, 0}); /* released, still used */
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
    ASSERT_EQ_LL(shr_lyr_destroy(hold), SHR_OK);
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

#define KEPT_565 (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565)

TEST test_image_create_from_arguments(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak);
    shr_pl_res_image *img = (shr_pl_res_image *)&h;
    const shr_image_source ok = {2, 2, SHR_IMAGE_SRC_RGB888, quad, 6};
    shr_image_source bad[6] = {ok, ok, ok, ok, ok, ok};
    bad[0].pixels = NULL, bad[1].width = 0, bad[2].height = -1, bad[3].format = (shr_image_source_format)4;
    bad[4].stride = 5, bad[5].format = SHR_IMAGE_SRC_RGBA8888;
    ASSERT_EQ_LL(shr_pl_res_image_create_from(NULL, &ok, &img), SHR_E_INVALID_ARG);
    ASSERT(img == NULL);
    ASSERT_EQ_LL(shr_pl_res_image_create_from(ctx, NULL, &img), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_res_image_create_from(ctx, &ok, NULL), SHR_E_INVALID_ARG);
    for (int i = 0; i < 6; i++) ASSERT_EQ_LL(shr_pl_res_image_create_from(ctx, &bad[i], &img), SHR_E_INVALID_ARG);
    const shr_image_source gray = {1, 4, SHR_IMAGE_SRC_GRAY8, quad, 1};
    ASSERT_EQ_LL(shr_pl_res_image_create_from(ctx, &gray, &img), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_create_from(ctx, &ok, &img), SHR_E_STATE);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

/* Too large for what is left: refused before a pixel is read (the rows here are one byte long); a translucent image
 * that would fit only as RGB565 is refused after its alpha is read. */
TEST test_image_budget_before_reading(void) {
    harness h;
    budget_bytes = 4096;
    caps_flags = SHR_DRIVER_IMAGE_565;
    shr_context *ctx = harness_open(&h, 0, tweak);
    budget_bytes = 0, caps_flags = 0;
    static const uint8_t one[1] = {255};
    const shr_image_source big = {64, 33, SHR_IMAGE_SRC_GRAY8, one, 0}, wide = {1 << 20, 1, SHR_IMAGE_SRC_RGB888, one, 0};
    shr_image_source s[2] = {big, wide};
    s[0].stride = 64, s[1].stride = 3u << 20;
    shr_pl_res_image *img;
    for (int i = 0; i < 2; i++) ASSERT_EQ_LL(shr_pl_res_image_create_from(ctx, &s[i], &img), SHR_E_LIMIT);
    static uint8_t px[40 * 40 * 4]; /* 3200 bytes as RGB565, 6400 as RGBA8888 */
    memset(px, 255, sizeof(px));
    px[3] = 254;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 40, 40, px, 40 * 4, &img), SHR_E_LIMIT);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

/* 3 x 2 rows of each layout with padded rows, drawn over white: the same screen as the RGBA8888 rows the layout means,
 * kept as RGB565 (half the bytes) exactly when the screen is RGB565, the driver can and every alpha is 255. */
TEST test_image_sources_and_storage(void) {
    static const uint8_t g[2][8] = {{0, 77, 128, 0}, {200, 255, 9, 0}};
    static const uint8_t ga[2][8] = {{0, 255, 77, 255, 128, 40}, {200, 255, 255, 0, 9, 255}};
    static const uint8_t gao[2][8] = {{0, 255, 77, 255, 128, 255}, {200, 255, 255, 255, 9, 255}};
    static const uint8_t rgb[2][12] = {{255, 0, 0, 1, 2, 3, 40, 80, 120}, {9, 99, 199, 0, 255, 0, 250, 251, 252}};
    uint8_t opaque[2][16], alpha[2][16];
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 3; x++) {
            memcpy(opaque[y] + 4 * x, rgb[y] + 3 * x, 3), opaque[y][4 * x + 3] = 255;
            memcpy(alpha[y] + 4 * x, rgb[y] + 3 * x, 3), alpha[y][4 * x + 3] = (uint8_t)(255 - 60 * x * y);
        }
    const struct {
        shr_image_source src;
        int opaque;
    } cases[] = {
        {{3, 2, SHR_IMAGE_SRC_RGBA8888, opaque, 16}, 1}, {{3, 2, SHR_IMAGE_SRC_RGBA8888, alpha, 16}, 0},
        {{3, 2, SHR_IMAGE_SRC_RGB888, rgb, 12}, 1},      {{3, 2, SHR_IMAGE_SRC_GRAY8, g, 8}, 1},
        {{3, 2, SHR_IMAGE_SRC_GRAY_ALPHA88, ga, 8}, 0},  {{3, 2, SHR_IMAGE_SRC_GRAY_ALPHA88, gao, 8}, 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        for (int able = 0; able < 2; able++) {
            const shr_image_source *src = &cases[i].src;
            uint8_t want[2][12];
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 3; x++) {
                    const uint8_t *p = (const uint8_t *)src->pixels + (size_t)y * src->stride;
                    uint8_t *o = want[y] + 4 * x;
                    switch (src->format) {
                    case SHR_IMAGE_SRC_RGBA8888: memcpy(o, p + 4 * x, 4); break;
                    case SHR_IMAGE_SRC_RGB888: memcpy(o, p + 3 * x, 3), o[3] = 255; break;
                    case SHR_IMAGE_SRC_GRAY8: o[0] = o[1] = o[2] = p[x], o[3] = 255; break;
                    default: o[0] = o[1] = o[2] = p[2 * x], o[3] = p[2 * x + 1]; break;
                    }
                }
            uint8_t screen[2][HW * HH * 4];
            for (int k = 0; k < 2; k++) {
                harness h;
                caps_flags = able && !k ? SHR_DRIVER_IMAGE_565 : 0;
                shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
                caps_flags = 0;
                shr_lyr *below;
                ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &below), SHR_OK);
                ASSERT_EQ_LL(shr_lyr_cmd_begin(below), SHR_OK);
                ASSERT_EQ_LL(shr_lyr_cmd_fill(below, FULL, SHR_RGB(255, 255, 255)), SHR_OK);
                ASSERT_EQ_LL(shr_lyr_cmd_commit(below), SHR_OK);
                shr_pl_res_image *img;
                if (k) ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 3, 2, want, 12, &img), SHR_OK);
                else ASSERT_EQ_LL(shr_pl_res_image_create_from(ctx, src, &img), SHR_OK);
                uint64_t used;
                ASSERT_EQ_LL(shr_pl_res_image_budget(ctx, &used, NULL), SHR_OK);
                ASSERT_EQ_LL(used, !k && able && KEPT_565 && cases[i].opaque ? 12 : 24);
                shr_lyr *l = image_layer(ctx, 1, img, (shr_rect){0, 0, 3, 2}, (shr_point){5, 7});
                frame(ctx);
                memcpy(screen[k], h.out.shown, sizeof(screen[k]));
                ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
                ASSERT_EQ_LL(shr_lyr_destroy(below), SHR_OK);
                ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
                harness_close(&h);
            }
            ASSERT_MEM_EQ(screen[1], screen[0], sizeof(screen[0]));
        }
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

/* An image kept as RGB565 takes opaque updates, also into a second buffer while a frame reads it, and refuses others. */
TEST test_image_565_updates(void) {
    harness h;
    caps_flags = SHR_DRIVER_IMAGE_565;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    caps_flags = 0;
    static const uint8_t red[16] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    const uint8_t green[4] = {0, 255, 0, 255}, clear[4] = {0, 255, 0, 254};
    shr_pl_res_image *img;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, red, 8, &img), SHR_OK);
    shr_lyr *l = image_layer(ctx, 0, img, (shr_rect){0, 0, 2, 2}, (shr_point){0, 0});
    h.drv.async = true;
    frame(ctx);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){1, 1, 2, 2}, clear, 4),
                 KEPT_565 ? SHR_E_UNSUPPORTED : SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_update(img, (shr_rect){1, 1, 2, 2}, green, 4), SHR_OK); /* the second buffer */
    md_complete(&h.drv);
    shr_pump(ctx);
    h.drv.async = false;
    frame(ctx);
    ASSERT(px(h.out.shown, 0, 0) == RED && px(h.out.shown, 1, 0) == RED && px(h.out.shown, 1, 1) == GREEN);
    uint64_t used;
    ASSERT_EQ_LL(shr_pl_res_image_budget(ctx, &used, NULL), SHR_OK);
    ASSERT_EQ_LL(used, KEPT_565 ? 16 : 32);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    shr_output_released(ctx, 2);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
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

/* An image that gets no buffer id is left out of the frame and, on a preserved output, drawn by the frames after it
 * while each leaves fewer out; one that a whole frame would leave out again is left at that. */
TEST test_image_left_out_without_id(void) {
    caps = (shr_driver_caps){.max_buffers = 2};
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak);
    caps = (shr_driver_caps){0};
    static const uint8_t rgba[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    static uint8_t px4[3][16], wide[HW * HH * 4];
    shr_pl_res_image *img[3], *top;
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 4; k++) memcpy(px4[i] + 4 * k, rgba[i], 4);
        ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, px4[i], 8, &img[i]), SHR_OK);
    }
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    set_commands(l, 3, img);
    frame(ctx);
    ASSERT(h.out.presents == 1 && px(h.out.shown, 0, 0) == RED && px(h.out.shown, 4, 0) == GREEN);
    ASSERT(px(h.out.shown, 8, 0) != BLUE);
    shr_deadline dl;
    ASSERT(shr_next_deadline(ctx, &dl) == SHR_OK && dl.kind == SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(h.out.presents == 2 && px(h.out.shown, 8, 0) == BLUE && rec.damaged == 4);

    for (size_t i = 0; i < sizeof(wide); i += 4) wide[i + 2] = 255, wide[i + 3] = 128;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, HW, HH, wide, HW * 4, &top), SHR_OK);
    shr_lyr *over = image_layer(ctx, 1, top, FULL, (shr_point){0, 0});
    set_commands(l, 2, img);
    frame(ctx);
    ASSERT(h.out.presents == 3 && px(h.out.shown, 0, 0) == RED && px(h.out.shown, 20, 20) == 0);
    shr_pump(ctx); /* the whole screen again, which leaves it out again */
    shr_pump(ctx);
    ASSERT(h.out.presents == 4 && rec.frames == 4 && rec.damaged == HW * HH && px(h.out.shown, 20, 20) == 0);
    ASSERT(shr_next_deadline(ctx, &dl) == SHR_OK && dl.kind != SHR_DEADLINE_NOW);
    ASSERT_EQ_LL(shr_lyr_destroy(over), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    for (int i = 0; i < 3; i++) ASSERT_EQ_LL(shr_pl_res_image_release(img[i]), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(top), SHR_OK);
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

/* A driver view of base columns 3..4, rows 1..2 scaled 2 -> 6 reads base columns 2..5 and rows 0..3: updates beside
 * those damage nothing, one in them redraws the view. */
TEST test_view_damage_follows_its_source(void) {
    harness h;
    caps_flags = SHR_DRIVER_SCALE;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak);
    caps_flags = 0;
    uint8_t blue[8 * 4 * 4];
    for (size_t i = 0; i < sizeof(blue); i += 4) blue[i] = 0, blue[i + 1] = 0, blue[i + 2] = 255, blue[i + 3] = 255;
    static const uint8_t green[4] = {0, 255, 0, 255};
    shr_pl_res_image *base, *v;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 8, 4, blue, 32, &base), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_view(base, (shr_rect){3, 1, 5, 3}, 6, 6, SHR_SCALE_DRIVER, &v), SHR_OK);
    shr_lyr *l = image_layer(ctx, 0, v, (shr_rect){0, 0, 6, 6}, (shr_point){0, 0});
    frame(ctx);
    static const shr_rect beside[] = {{0, 0, 1, 1}, {7, 3, 8, 4}};
    for (size_t i = 0; i < sizeof(beside) / sizeof(beside[0]); i++) {
        int frames = rec.frames;
        ASSERT_EQ_LL(shr_pl_res_image_update(base, beside[i], green, 4), SHR_OK);
        frame(ctx);
        ASSERT_EQ_LL(rec.frames, frames);
    }
    ASSERT_EQ_LL(px(h.out.shown, 2, 2), BLUE);
    ASSERT_EQ_LL(shr_pl_res_image_update(base, (shr_rect){3, 1, 4, 2}, green, 4), SHR_OK);
    frame(ctx);
    ASSERT(rec.damaged > 0 && px(h.out.shown, 2, 2) != BLUE);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(v), SHR_OK);
    ASSERT_EQ_LL(shr_pl_res_image_release(base), SHR_OK);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

/* A driver view the allocator cannot make holds nothing: its base is freed at once on release. */
TEST test_view_out_of_memory(void) {
    harness h;
    caps_flags = SHR_DRIVER_SCALE;
    shr_context *ctx = harness_open(&h, 0, tweak);
    caps_flags = 0;
    shr_pl_res_image *base, *v = (shr_pl_res_image *)&h;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 2, 2, quad, 8, &base), SHR_OK);
    long live = oom.live;
    oom.budget = 0;
    ASSERT_EQ_LL(shr_pl_res_image_view(base, (shr_rect){0, 0, 2, 2}, 4, 4, SHR_SCALE_DRIVER, &v), SHR_E_NO_MEMORY);
    oom.budget = -1;
    ASSERT(v == NULL && oom.live == live);
    ASSERT_EQ_LL(shr_pl_res_image_release(base), SHR_OK);
    ASSERT(oom.live < live);
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
    RUN_TEST(test_image_create_from_arguments);
    RUN_TEST(test_image_budget_before_reading);
    RUN_TEST(test_image_sources_and_storage);
    RUN_TEST(test_image_565_updates);
    RUN_TEST(test_image_needs_plugin_slot);
    RUN_TEST(test_image_blends_over_layers_below);
    RUN_TEST(test_cmd_image_validation);
    RUN_TEST(test_image_update_arguments);
    RUN_TEST(test_image_update_damages_every_buffer);
    RUN_TEST(test_image_pinned_by_frames);
    RUN_TEST(test_image_left_out_without_id);
    RUN_TEST(test_image_double_buffer);
    RUN_TEST(test_image_pinned_across_bands);
    RUN_TEST(test_image_id_freed_while_a_frame_is_built);
    RUN_TEST(test_image_second_buffer_out_of_memory);
    RUN_TEST(test_view_damage_follows_its_source);
    RUN_TEST(test_view_out_of_memory);
    GREATEST_MAIN_END();
}
