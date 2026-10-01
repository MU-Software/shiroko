/* Completions from other threads, and independent contexts on separate threads; under ThreadSanitizer
 * this looks for unsynchronised shared state. */
#include <pthread.h>

#include "harness.h"
#include "shr_compositor.h"

static void *complete_driver(void *arg) {
    md_complete(arg);
    return NULL;
}

TEST test_fence_signalled_from_another_thread(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_fill(l, (shr_rect){0, 0, HW, HH}, SHR_RGB(255, 0, 0)), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    h.drv.async = true;
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
        shr_pump(ctx);
        shr_deadline dl;
        shr_next_deadline(ctx, &dl);
        ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_NONE); /* the completion wakes the application */
        pthread_t th;
        ASSERT_EQ_LL(pthread_create(&th, NULL, complete_driver, &h.drv), 0);
        while (h.out.presents == i) shr_pump(ctx); /* races with the signal */
        pthread_join(th, NULL);
        ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_PRESENT_ACCEPTED, NULL), 1);
    }
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), 0xFF0000u);
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    harness_close(&h);
    PASS();
}

static void *signal_ready(void *arg) {
    shr_driver_ready(arg);
    shr_asset_ready(arg);
    return NULL;
}

static void tweak_wait_ready(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)drv;
    d->io_retry_ns = 0;
}

/* The driver refuses a submission and says it is ready again from another thread. */
TEST test_driver_ready_from_another_thread(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_wait_ready);
    harness_font(&h);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &l), SHR_OK);
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_fill(l, (shr_rect){0, 0, HW, HH}, SHR_RGB(i, 0, 0)), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
        h.drv.block_next = 1;
        ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
        shr_pump(ctx);
        pthread_t th;
        ASSERT_EQ_LL(pthread_create(&th, NULL, signal_ready, ctx), 0);
        while (h.out.presents == i) { /* races with the signal */
            shr_deadline dl;
            shr_next_deadline(ctx, &dl);
            if (dl.kind == SHR_DEADLINE_NOW) shr_pump(ctx);
        }
        pthread_join(th, NULL);
        ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_PRESENT_ACCEPTED, NULL), 1);
    }
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    harness_close(&h);
    PASS();
}

/* An asynchronous source: a worker thread fills and completes queued reads. */
typedef struct q_item {
    uint64_t request, offset;
    uint8_t *dst;
    uint32_t length;
} q_item;

typedef struct queue {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    q_item item[8];
    int n;
    bool stop;
    shr_context *ctx;
} queue;

static shr_status q_read(void *user, uint64_t offset, uint32_t length, void *dst, uint64_t request) {
    queue *q = user;
    pthread_mutex_lock(&q->lock);
    shr_status st = q->n < 8 ? SHR_IN_PROGRESS : SHR_E_WOULD_BLOCK;
    if (q->n < 8) q->item[q->n++] = (q_item){request, offset, dst, length};
    pthread_cond_signal(&q->wake);
    pthread_mutex_unlock(&q->lock);
    return st;
}

/* Holds the lock while writing, so nothing is written once it returns. */
static void q_cancel(void *user, uint64_t request) {
    queue *q = user;
    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < q->n; i++)
        if (q->item[i].request == request) q->item[i--] = q->item[--q->n];
    pthread_mutex_unlock(&q->lock);
}

static void *q_worker(void *arg) {
    queue *q = arg;
    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (!q->n && !q->stop) pthread_cond_wait(&q->wake, &q->lock);
        bool stop = q->stop;
        for (int i = 0; i < q->n; i++) {
            memset(q->item[i].dst, (int)q->item[i].offset, q->item[i].length);
            shr_asset_complete(q->ctx, q->item[i].request, SHR_OK);
        }
        q->n = 0;
        pthread_mutex_unlock(&q->lock);
        if (stop) return NULL;
    }
}

typedef struct reader {
    shr__res res;
    uint8_t buf[64][16];
    int done, ok, wrong;
} reader;

static shr_status rd_resolve(shr__res *r, uint64_t id, uint64_t frame, shr__resolved *out) {
    (void)r, (void)id, (void)frame, (void)out;
    return SHR_E_NOT_FOUND;
}
static void rd_io_done(shr__res *r, uint64_t tag, shr_status st) {
    reader *rd = (reader *)r;
    rd->done++;
    if (st == SHR_OK) rd->ok++;
    if (st == SHR_OK ? rd->buf[tag % 64][15] != (uint8_t)tag : st != SHR_E_IO) rd->wrong++;
}
static void rd_free(shr__res *r) { (void)r; }
static const shr__res_ops rd_ops = {.resolve = rd_resolve, .io_done = rd_io_done, .free = rd_free};

TEST test_asset_completed_from_another_thread(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    static reader rd;
    memset(&rd, 0, sizeof(rd));
    ASSERT_EQ_LL(shr__res_attach(ctx, &rd.res, &rd_ops), SHR_OK);
    queue q = {.ctx = ctx};
    pthread_mutex_init(&q.lock, NULL);
    pthread_cond_init(&q.wake, NULL);
    shr_asset_source src;
    shr_asset_source_init(&src);
    src.user = &q, src.read = q_read, src.cancel = q_cancel;
    pthread_t th;
    ASSERT_EQ_LL(pthread_create(&th, NULL, q_worker, &q), 0);
    const int reads = 2000;
    for (int issued = 0; issued < reads || rd.done < reads;) {
        if (issued < reads &&
            shr__ctx_read(ctx, &rd.res, &src, (uint8_t)issued, 16, rd.buf[issued % 64], (uint64_t)issued) == SHR_OK)
            issued++;
        if (issued % 3 == 0) shr__ctx_read_cancel(ctx, &src); /* races with the completions */
        shr_pump(ctx);
    }
    pthread_mutex_lock(&q.lock);
    q.stop = true;
    pthread_cond_signal(&q.wake);
    pthread_mutex_unlock(&q.lock);
    pthread_join(th, NULL);
    pthread_cond_destroy(&q.wake);
    pthread_mutex_destroy(&q.lock);
    ASSERT(rd.done == reads && rd.ok > 0 && rd.wrong == 0);
    rd.res.dead = true;
    harness_close(&h);
    PASS();
}

#define THREADS 4
#define ROUNDS 20

typedef struct worker {
    uint8_t pixels[HW * HH * 4];
    int presents, failures;
} worker;

static shr_status w_acquire(void *user, shr_surface *s) {
    worker *w = user;
    *s = (shr_surface){w->pixels, HW, HH, HW * SCREEN_BPP, sizeof(w->pixels), SHR_PIXEL_FORMAT, 0, SHR_MEMORY_CPU, 0};
    return SHR_OK;
}
static shr_status w_present(void *user, const shr_surface *s, uint64_t id) {
    (void)s, (void)id;
    ((worker *)user)->presents++;
    return SHR_OK;
}
static void w_discard(void *user, const shr_surface *s) { (void)user, (void)s; }

static void *run_context(void *arg) {
    worker *w = arg;
    shr_framebuffer_driver drv;
    if (shr_software_driver_create(NULL, 64 << 10, 16, &drv) != SHR_OK) return w->failures++, NULL;
    shr_output out;
    shr_output_init(&out);
    out.user = w, out.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT;
    out.acquire = w_acquire, out.present = w_present, out.discard = w_discard;
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.driver = &drv, cd.output = &out;
    cd.io_retry_ns = cd.io_timeout_ns = 0; /* no clock */
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = HW, sd.height = HH, sd.clear = SHR_RGB(0, 0, 80);
    shr_context *ctx;
    shr_lyr *bg, *fg;
    shr_pl_res_image *img;
    static const uint8_t rgba[16] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64, 255, 255, 255, 255};
    if (shr_create(&cd, &ctx) != SHR_OK || shr_screen_configure(ctx, &sd) != SHR_OK ||
        shr_lyr_create(ctx, 0, (shr_rect){0, 0, HW, HH}, &bg) != SHR_OK ||
        shr_lyr_create(ctx, 1, (shr_rect){8, 8, 56, 40}, &fg) != SHR_OK ||
        shr_pl_res_image_create(ctx, 2, 2, rgba, 8, &img) != SHR_OK)
        return w->failures++, NULL;
    for (int r = 0; r < ROUNDS; r++) {
        bool bad = shr_lyr_cmd_begin(bg) != SHR_OK;
        for (int i = 0; i < 8; i++)
            bad |= shr_lyr_cmd_fill(bg, (shr_rect){(r + i * 7) % HW, i * 6, (r + i * 7) % HW + 5, i * 6 + 4},
                                    SHR_RGB(r * 12, i * 30, 200)) != SHR_OK;
        bad |= shr_lyr_cmd_commit(bg) != SHR_OK || shr_lyr_cmd_begin(fg) != SHR_OK;
        bad |= shr_lyr_cmd_image(fg, img, (shr_rect){0, 0, 2, 2}, (shr_point){r, r % 7}) != SHR_OK;
        bad |= shr_lyr_cmd_commit(fg) != SHR_OK || shr_submit(ctx) != SHR_OK || shr_pump(ctx) != SHR_OK;
        shr_event ev;
        while (shr_poll_event(ctx, &ev) == SHR_OK) bad |= ev.kind == SHR_EVENT_PRESENT_FAILED;
        w->failures += bad;
    }
    if (shr_lyr_destroy(bg) != SHR_OK || shr_lyr_destroy(fg) != SHR_OK || shr_pl_res_image_release(img) != SHR_OK)
        w->failures++;
    shr_begin_shutdown(ctx);
    shr_pump(ctx);
    if (shr_destroy(ctx) != SHR_OK) w->failures++;
    shr_software_driver_destroy(&drv);
    return NULL;
}

TEST test_independent_contexts(void) {
    static worker workers[THREADS];
    memset(workers, 0, sizeof(workers));
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++) ASSERT_EQ_LL(pthread_create(&th[i], NULL, run_context, &workers[i]), 0);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < THREADS; i++) {
        ASSERT_EQ_LL(workers[i].failures, 0);
        ASSERT_EQ_LL(workers[i].presents, ROUNDS);
        /* Same input, same output on every thread (bit-exact CPU raster). */
        ASSERT(memcmp(workers[i].pixels, workers[0].pixels, sizeof(workers[0].pixels)) == 0);
    }
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(test_fence_signalled_from_another_thread);
    RUN_TEST(test_asset_completed_from_another_thread);
    RUN_TEST(test_driver_ready_from_another_thread);
    RUN_TEST(test_independent_contexts);
    GREATEST_MAIN_END();
}
