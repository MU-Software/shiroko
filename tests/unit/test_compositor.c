#include "compositor.h"
#include "harness.h"

#define MS 1000000ull
#define WHITE 0xFFFFFFu
#define RED 0xFF0000u
#define GREEN 0x00FF00u
#define BLUE 0x0000FFu
#define PRESERVED (SHR_OUTPUT_PRESERVES_CONTENT | SHR_OUTPUT_RELEASE_ON_PRESENT)
#define OTHER_FORMAT (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? SHR_FORMAT_RGBX8888 : SHR_FORMAT_RGB565)
#define FULL ((shr_rect){0, 0, HW, HH})

/* ---- recording host callbacks ---- */

static struct {
    shr_draw_cmd cmds[512];
    size_t n;
    int frames, logs, kinds[16];
    uint64_t damaged, commands; /* of the last RASTER_BEGIN */
    uint64_t submit_id;         /* of the last SUBMIT */
    int syncs;
    const void *sync_addr[8];
    shr_fence fence; /* of the last execute() */
} rec;

static void rec_trace(void *user, const shr_trace_event *ev) {
    (void)user;
    rec.kinds[ev->kind]++;
    if (ev->kind == SHR_TRACE_RASTER_BEGIN) rec.frames++, rec.commands = ev->value0, rec.damaged = ev->value1;
    if (ev->kind == SHR_TRACE_SUBMIT) rec.submit_id = ev->id;
}

static void rec_log(void *user, shr_status st, const char *msg) {
    (void)user, (void)st, (void)msg;
    rec.logs++;
}

static shr_status rec_execute(void *user, const shr_surface *dst, const shr_draw_cmd *c, size_t n, shr_fence f) {
    rec.n = n < 512 ? n : 512;
    memcpy(rec.cmds, c, rec.n * sizeof(*c));
    rec.fence = f;
    return md_execute(user, dst, c, n, f);
}

static void rec_sync(void *user, const void *addr, size_t bytes) {
    (void)user, (void)bytes;
    if (rec.syncs < 8) rec.sync_addr[rec.syncs] = addr;
    rec.syncs++;
}

static void tweak_rec(shr_context_desc *d, shr_framebuffer_driver *drv) {
    memset(&rec, 0, sizeof(rec));
    d->trace = rec_trace, d->log = rec_log;
    drv->execute = rec_execute;
}

static fail_alloc oom;
static shr_allocator oom_allocator;
static void tweak_oom(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    oom = (fail_alloc){-1, 0};
    oom_allocator = fail_allocator(&oom);
    d->allocator = &oom_allocator;
}

static void tweak_blink(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->blink = (shr_blink_profile){100 * MS, 0, true, SHR_BLINK_RESTART_NONE};
}

static void tweak_timeout(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps.timeout_ns = 10 * MS;
}

static void tweak_one_frame(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->max_unreleased_frames = 1;
}

/* ---- a resource whose behaviour the test controls ---- */

typedef struct fake {
    shr__res res;
    uint8_t cov[8 * 16 * 4];
    shr__resolved px;
    shr_status st;
    bool changed, work;
    uint64_t deadline;
    int resolves, ends, frees, shutdowns, io_n;
    uint64_t io_tag;
    shr_status io_st;
} fake;

static shr_status fk_resolve(shr__res *r, uint64_t id, uint64_t frame, shr__resolved *out) {
    fake *f = (fake *)r;
    (void)frame;
    f->resolves++;
    if (!id) return SHR_E_NOT_FOUND;
    if (f->st != SHR_OK) return f->st;
    *out = f->px;
    return SHR_OK;
}
static void fk_end(shr__res *r, uint64_t frame) { (void)frame, ((fake *)r)->ends++; }
static bool fk_pump(shr__res *r) {
    fake *f = (fake *)r;
    bool changed = f->changed;
    f->changed = false;
    return changed;
}
static bool fk_work(const shr__res *r) { return ((const fake *)r)->work; }
static uint64_t fk_deadline(const shr__res *r) { return ((const fake *)r)->deadline; }
static void fk_io(shr__res *r, uint64_t tag, shr_status st) {
    fake *f = (fake *)r;
    f->io_n++, f->io_tag = tag, f->io_st = st;
}
static void fk_shutdown(shr__res *r) { ((fake *)r)->shutdowns++; }
static void fk_free(shr__res *r) { ((fake *)r)->frees++; }

static const shr__res_ops fk_ops = {fk_resolve, fk_end, fk_pump, fk_work, fk_deadline, fk_io, fk_shutdown, fk_free};
static const shr__res_ops bare_ops = {.resolve = fk_resolve, .free = fk_free};

/* An 8x16 opaque glyph. */
static void fake_attach(fake *f, shr_context *ctx, const shr__res_ops *ops) {
    memset(f, 0, sizeof(*f));
    memset(f->cov, 255, sizeof(f->cov));
    f->px.image = (shr_image){f->cov, 8, 16, 8, 8 * 16, SHR_FORMAT_A8, SHR_MEMORY_CPU};
    ASSERT_EQ_LL(shr__res_attach(ctx, &f->res, ops), SHR_OK);
}

static shr__lcmd glyph(fake *f, int32_t x, int32_t y, shr_color color) {
    return (shr__lcmd){.kind = SHR__LCMD_GLYPH, .dst = {x, y, x + 8, y + 16}, .anchor = {x, y}, .color = color,
                       .res = &f->res, .id = 1};
}

static shr__lcmd fill(shr_rect r, shr_color color) {
    return (shr__lcmd){.kind = SHR__LCMD_FILL, .dst = r, .color = color};
}

/* ---- helpers ---- */

static void paint(shr_lyr *l, size_t n, const shr__lcmd *cmds) {
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    for (size_t i = 0; i < n; i++) ASSERT_EQ_LL(shr__lyr_cmd_add(l, &cmds[i]), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
}

static shr_lyr *solid(shr_context *ctx, int32_t z, shr_rect rect, shr_color color) {
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, z, rect, &l), SHR_OK);
    shr__lcmd c = fill((shr_rect){0, 0, rect.x1 - rect.x0, rect.y1 - rect.y0}, color);
    paint(l, 1, &c);
    return l;
}

static void frame(shr_context *ctx) {
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_pump(ctx), SHR_OK);
}

static shr_deadline_kind deadline(const shr_context *ctx) {
    shr_deadline dl;
    ASSERT_EQ_LL(shr_next_deadline(ctx, &dl), SHR_OK);
    return dl.kind;
}

static uint64_t deadline_at(const shr_context *ctx) {
    shr_deadline dl;
    ASSERT_EQ_LL(shr_next_deadline(ctx, &dl), SHR_OK);
    ASSERT_EQ_LL(dl.kind, SHR_DEADLINE_AT);
    return dl.at_ns;
}

/* The next event, which must be of `kind`. */
static shr_event expect_event(shr_context *ctx, shr_event_kind kind) {
    shr_event ev = {0};
    ASSERT_EQ_LL(shr_poll_event(ctx, &ev), SHR_OK);
    ASSERT_EQ_LL(ev.kind, kind);
    return ev;
}

static void drain(shr_context *ctx) {
    shr_event ev;
    while (shr_poll_event(ctx, &ev) == SHR_OK) continue;
}

/* 0xRRGGBB of what the output shows, in the output's own size and format. */
static uint32_t opx(const harness *h, int x, int y) {
    size_t bpp = h->out.format == SHR_FORMAT_RGB565 ? 2 : 4;
    const uint8_t *p = h->out.shown + ((size_t)y * (size_t)h->out.w + (size_t)x) * bpp;
    if (bpp == 4) return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    uint16_t v;
    memcpy(&v, p, 2);
    return ((v >> 11) * 255u / 31) << 16 | ((v >> 5 & 63) * 255u / 63) << 8 | (v & 31) * 255u / 31;
}

static int count_color(const harness *h, uint32_t color) {
    int n = 0;
    for (int y = 0; y < h->out.h; y++)
        for (int x = 0; x < h->out.w; x++) n += opx(h, x, y) == color;
    return n;
}

static void destroy_layers(shr_lyr **l, size_t n) {
    for (size_t i = 0; i < n; i++) ASSERT_EQ_LL(shr_lyr_destroy(l[i]), SHR_OK);
}

/* ===== context ===== */

TEST test_init_functions(void) {
    shr_context_desc d;
    ASSERT_EQ_LL(shr_context_desc_init(&d), SHR_OK);
    ASSERT(d.blink.start_visible && !d.blink.interval_ns && !d.driver && !d.output && !d.now_ns && !d.allocator);
    ASSERT(d.event_capacity == 64 && d.max_unreleased_frames == 2 && d.max_commands == 16384 && d.max_reads == 4);
    ASSERT(d.page_cache_bytes == 3u << 20 && d.image_bytes == 4u << 20 && d.io_retry_limit == 3);
    ASSERT(d.io_retry_ns == 50 * MS && d.io_timeout_ns == 1000 * MS);
    shr_screen_desc sd;
    shr_output o;
    shr_asset_source s;
    shr_framebuffer_driver drv;
    memset(&sd, 1, sizeof(sd)), memset(&o, 1, sizeof(o)), memset(&s, 1, sizeof(s)), memset(&drv, 1, sizeof(drv));
    ASSERT_EQ_LL(shr_screen_desc_init(&sd), SHR_OK);
    ASSERT_EQ_LL(shr_output_init(&o), SHR_OK);
    ASSERT_EQ_LL(shr_asset_source_init(&s), SHR_OK);
    ASSERT_EQ_LL(shr_framebuffer_driver_init(&drv), SHR_OK);
    ASSERT(!sd.width && !sd.composition && !sd.output_format && !o.acquire && !o.flags && !s.read && !s.data);
    ASSERT(!drv.execute && drv.caps.domains == SHR_MEMORY_CPU && !drv.caps.timeout_ns);
    ASSERT_EQ_LL(shr_context_desc_init(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_screen_desc_init(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_output_init(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_asset_source_init(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_framebuffer_driver_init(NULL), SHR_E_INVALID_ARG);
    PASS();
}

TEST test_create_rejects_invalid_descriptors(void) {
    harness h;
    shr_output out;
    shr_context_desc d;
    harness_desc(&h, 0, &out, &d);
    shr_context *ctx = (shr_context *)&h;
    ASSERT_EQ_LL(shr_create(NULL, &ctx), SHR_E_INVALID_ARG);
    ASSERT(ctx == NULL);
    ASSERT_EQ_LL(shr_create(&d, NULL), SHR_E_INVALID_ARG);
    fail_alloc f = {-1, 0};
    const shr_allocator half[2] = {{&f, fa_alloc, NULL}, {&f, NULL, fa_free}};
    for (int i = 0; i < 20; i++) {
        shr_context_desc dd = d;
        shr_framebuffer_driver drv = h.driver;
        shr_output o = out;
        dd.driver = &drv, dd.output = &o;
        switch (i) {
        case 0: dd.driver = NULL; break;
        case 1: dd.output = NULL; break;
        case 2: drv.execute = NULL; break;
        case 3: o.acquire = NULL; break;
        case 4: o.present = NULL; break;
        case 5: o.discard = NULL; break;
        case 6: dd.max_commands = 0; break;
        case 7: dd.max_unreleased_frames = 0; break;
        case 8: dd.event_capacity = 1; break;
        case 9: dd.event_capacity = dd.max_unreleased_frames + 1; break;
        case 10: dd.max_reads = 0; break;
        case 11: dd.blink.restart = (shr_blink_restart)(SHR_BLINK_RESTART_ON_SUBMIT + 1); break;
        case 12: o.timestamp = (shr_timestamp_kind)(SHR_TIMESTAMP_COMPOSITOR + 1); break;
        case 13: o.flags = 1u << 2; break;
        /* Without a clock no timer could ever expire. */
        case 14: dd.now_ns = NULL, dd.io_timeout_ns = 0; break;
        case 15: dd.now_ns = NULL, dd.io_retry_ns = 0; break;
        case 16: dd.now_ns = NULL, dd.io_retry_ns = dd.io_timeout_ns = 0, drv.caps.timeout_ns = 1; break;
        case 17: dd.allocator = &half[0]; break;
        case 18: dd.allocator = &half[1]; break;
        default: dd.max_reads = 0x10000; break;
        }
        ctx = (shr_context *)&h;
        ASSERT_EQ_LL(shr_create(&dd, &ctx), i < 19 ? SHR_E_INVALID_ARG : SHR_E_LIMIT);
        ASSERT(ctx == NULL);
    }
    d.max_reads = 0xFFFF, d.event_capacity = d.max_unreleased_frames + 2;
    d.now_ns = NULL, d.io_retry_ns = d.io_timeout_ns = 0;
    ASSERT_EQ_LL(shr_create(&d, &ctx), SHR_OK);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);
    PASS();
}

TEST test_create_out_of_memory(void) {
    harness h;
    shr_output out;
    shr_context_desc d;
    harness_desc(&h, 0, &out, &d);
    for (long budget = 0;; budget++) {
        fail_alloc f = {budget, 0};
        shr_allocator a = fail_allocator(&f);
        d.allocator = &a;
        shr_context *ctx = (shr_context *)&f;
        shr_status st = shr_create(&d, &ctx);
        if (st == SHR_OK) {
            ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);
            ASSERT_EQ_LL(f.live, 0);
            break;
        }
        ASSERT_EQ_LL(st, SHR_E_NO_MEMORY);
        ASSERT(ctx == NULL);
        ASSERT_EQ_LL(f.live, 0);
    }
    PASS();
}

static void tweak_services(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->io_retry_limit = 0x10000;
    d->user = &rec;
}

TEST test_context_services(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak_services);
    const shr_context_desc *d = shr__ctx_desc(ctx);
    ASSERT(d->io_retry_limit == 0xFFFF && !d->driver && !d->output && !d->allocator && d->user == &rec);
    ASSERT(shr__ctx_alloc(ctx) != NULL);
    fake_now = 1234;
    ASSERT_EQ_LL(shr__ctx_now(ctx), 1234);
    ASSERT(!shr__ctx_refused(ctx));
    shr__ctx_log(ctx, SHR_E_IO, "x");
    shr__ctx_trace(ctx, SHR_TRACE_PAGE_READY, 1, 2, 3);
    ASSERT(rec.logs == 1 && rec.kinds[SHR_TRACE_PAGE_READY] == 1);
    harness_close(&h);

    shr_output out;
    shr_context_desc dd;
    harness_desc(&h, 0, &out, &dd);
    dd.now_ns = NULL, dd.io_retry_ns = dd.io_timeout_ns = 0;
    ASSERT_EQ_LL(shr_create(&dd, &ctx), SHR_OK);
    ASSERT_EQ_LL(shr__ctx_now(ctx), 0);
    shr__ctx_log(ctx, SHR_E_IO, "no log callback");
    shr__ctx_trace(ctx, SHR_TRACE_PAGE_READY, 0, 0, 0);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);
    PASS();
}

TEST test_plugin_slots(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    static const char kinds[SHR_PLUGIN_SLOTS + 1];
    ASSERT(shr__ctx_plugin_slot(ctx, NULL) == NULL);
    void **first = shr__ctx_plugin_slot(ctx, &kinds[0]);
    ASSERT(first && !*first);
    *first = (void *)&kinds;
    ASSERT(shr__ctx_plugin_slot(ctx, &kinds[0]) == first);
    for (int i = 1; i < SHR_PLUGIN_SLOTS; i++) {
        void **s = shr__ctx_plugin_slot(ctx, &kinds[i]);
        ASSERT(s && s != first && !*s);
    }
    ASSERT(shr__ctx_plugin_slot(ctx, &kinds[SHR_PLUGIN_SLOTS]) == NULL);
    ASSERT(*shr__ctx_plugin_slot(ctx, &kinds[0]) == (void *)&kinds);
    harness_close(&h);
    PASS();
}

static shr_status configure(shr_context *ctx, shr_rotation rot, shr_pixel_format out_format, uint32_t flags,
                            const shr_surface *comp) {
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = HW, sd.height = HH, sd.rotation = rot, sd.output_format = out_format, sd.flags = flags;
    sd.composition = comp;
    return shr_screen_configure(ctx, &sd);
}

static uint8_t comp_px[HW * HH * 4];
static const shr_surface comp_good = {comp_px, HW, HH, HW * SCREEN_BPP, sizeof(comp_px), SHR_PIXEL_FORMAT, 1, 0, 0};

TEST test_screen_configure_validation(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_oom);
    const struct {
        int32_t w, h;
        shr_pixel_format output_format;
        shr_rotation rotation;
        uint32_t flags;
    } bad[] = {
        {0, HH, 0, SHR_ROTATE_NONE, 0},
        {HW, 0, 0, SHR_ROTATE_NONE, 0},
        {-1, HH, 0, SHR_ROTATE_NONE, 0},
        {16385, HH, 0, SHR_ROTATE_NONE, 0},
        {HW, 16385, 0, SHR_ROTATE_NONE, 0},
        {HW, HH, SHR_FORMAT_A8, SHR_ROTATE_NONE, 0},
        {HW, HH, SHR_FORMAT_RGBA8888, SHR_ROTATE_NONE, 0},
        {HW, HH, 0, (shr_rotation)(SHR_ROTATE_90_CCW + 1), 0},
        {HW, HH, 0, SHR_ROTATE_NONE, 2},
    };
    shr_screen_desc sd;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        shr_screen_desc_init(&sd);
        sd.width = bad[i].w, sd.height = bad[i].h, sd.output_format = bad[i].output_format;
        sd.rotation = bad[i].rotation, sd.flags = bad[i].flags;
        ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    }
    ASSERT_EQ_LL(shr_screen_configure(NULL, &sd), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_screen_configure(ctx, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_90_CW, OTHER_FORMAT, 0, NULL), SHR_E_UNSUPPORTED);
    shr_surface comp[5] = {comp_good, comp_good, comp_good, comp_good, comp_good};
    comp[0].stride = 4, comp[1].width = HW - 1, comp[2].height = HH - 1;
    comp[3].format = OTHER_FORMAT, comp[3].stride = HW * 4;
    comp[4].domain = (shr_memory_domain)8;
    for (int i = 0; i < 5; i++) ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, &comp[i]), SHR_E_INVALID_ARG);
    oom.budget = 0;
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_E_NO_MEMORY);
    oom.budget = -1;

    shr_lyr *l = solid(ctx, 0, FULL, RED);
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, HW - 1, HH - 1), RED);
    ASSERT_EQ_LL(h.drv.calls, 1); /* the previous configuration (no composition) is still active */
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, &comp_good), SHR_OK);
    memset(comp_px, 0, sizeof(comp_px));
    frame(ctx);
    ASSERT_EQ_LL(h.drv.calls, 3);
    ASSERT_EQ_LL(rec.cmds[0].kind, SHR_CMD_COPY);
    ASSERT_EQ_LL(px(comp_px, 0, 0), RED);
    ASSERT_EQ_LL(px(h.out.shown, HW - 1, HH - 1), RED);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK); /* replaces its own */
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, SHR_PIXEL_FORMAT, 0, NULL), SHR_OK);
    destroy_layers(&l, 1);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

static struct {
    fail_alloc f;
    int dma;
} dma_alloc;
static void *dma_count_alloc(void *user, size_t size, size_t align, shr_alloc_kind kind) {
    dma_alloc.dma += kind == SHR_ALLOC_DMA;
    return fa_alloc(user, size, align, kind);
}
static shr_allocator dma_allocator = {&dma_alloc.f, dma_count_alloc, fa_free};

static void tweak_dma_only(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps.domains = SHR_MEMORY_DMA;
    drv->sync = rec_sync;
}

static void tweak_dma_alloc(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_dma_only(d, drv);
    dma_alloc.f = (fail_alloc){-1, 0}, dma_alloc.dma = 0;
    d->allocator = &dma_allocator;
}

/* DMA-only drivers need DMA memory: from the application's allocator or its own surface. */
TEST test_dma_only_composition(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_dma_only);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_E_UNSUPPORTED);
    shr_surface comp = comp_good;
    comp.domain = SHR_MEMORY_DMA;
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, &comp), SHR_OK);
    harness_close(&h);

    shr_output out;
    shr_context_desc d;
    harness_desc(&h, 0, &out, &d);
    const shr_allocator none = {0}; /* the default allocator promises no DMA memory */
    d.allocator = &none;
    h.driver.caps.domains = SHR_MEMORY_DMA;
    ASSERT_EQ_LL(shr_create(&d, &ctx), SHR_OK);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);

    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_dma_alloc);
    h.out.domain = SHR_MEMORY_DMA;
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
    ASSERT_EQ_LL(dma_alloc.dma, 1);
    shr_lyr *l = solid(ctx, 0, FULL, GREEN);
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 3, 3), GREEN);
    ASSERT_EQ_LL(rec.syncs, 3); /* composition before raster; output and composition before the copy */
    ASSERT(rec.sync_addr[0] == rec.sync_addr[2] && rec.sync_addr[1] == h.out.bufs[h.out.last_buf]);
    destroy_layers(&l, 1);
    harness_close(&h);
    ASSERT_EQ_LL(dma_alloc.f.live, 0);
    PASS();
}

TEST test_rotation_and_conversion(void) {
    const shr_rotation rots[3] = {SHR_ROTATE_90_CW, SHR_ROTATE_180, SHR_ROTATE_90_CCW};
    for (int i = 0; i < 4; i++) {
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
        shr_rotation rot = i < 3 ? rots[i] : SHR_ROTATE_NONE;
        bool quarter = rot == SHR_ROTATE_90_CW || rot == SHR_ROTATE_90_CCW;
        h.out.w = quarter ? HH : HW, h.out.h = quarter ? HW : HH;
        if (i == 3) h.out.format = OTHER_FORMAT;
        ASSERT_EQ_LL(configure(ctx, rot, i == 3 ? OTHER_FORMAT : 0, 0, NULL), SHR_OK);
        shr_lyr *l = solid(ctx, 0, (shr_rect){0, 0, 4, 2}, RED);
        frame(ctx);
        ASSERT_EQ_LL(h.out.presents, 1);
        ASSERT_EQ_LL(rec.cmds[0].kind, i < 3 ? SHR_CMD_ROTATE : SHR_CMD_COPY);
        ASSERT_EQ_LL(rec.kinds[SHR_TRACE_CONVERT], 1);
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 4; x++) {
                shr_point p;
                ASSERT_EQ_LL(shr_rotation_map_point(rot, HW, HH, (shr_point){x, y}, false, &p), SHR_OK);
                ASSERT_EQ_LL(opx(&h, p.x, p.y), RED);
            }
        ASSERT_EQ_LL(count_color(&h, RED), 8);
        destroy_layers(&l, 1);
        harness_close(&h);
    }
    PASS();
}

TEST test_rotation_map_point(void) {
    const shr_rotation rots[4] = {SHR_ROTATE_NONE, SHR_ROTATE_90_CW, SHR_ROTATE_180, SHR_ROTATE_90_CCW};
    const shr_point want[4] = {{1, 0}, {2, 1}, {2, 2}, {0, 2}};
    for (int i = 0; i < 4; i++) {
        shr_point p, back;
        ASSERT_EQ_LL(shr_rotation_map_point(rots[i], 4, 3, (shr_point){1, 0}, false, &p), SHR_OK);
        ASSERT(p.x == want[i].x && p.y == want[i].y);
        ASSERT_EQ_LL(shr_rotation_map_point(rots[i], 4, 3, p, true, &back), SHR_OK);
        ASSERT(back.x == 1 && back.y == 0);
    }
    shr_point p;
    ASSERT_EQ_LL(shr_rotation_map_point(SHR_ROTATE_NONE, 4, 3, (shr_point){0, 0}, false, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_rotation_map_point((shr_rotation)4, 4, 3, (shr_point){0, 0}, false, &p), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_rotation_map_point(SHR_ROTATE_NONE, 0, 3, (shr_point){0, 0}, false, &p), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_rotation_map_point(SHR_ROTATE_NONE, 4, 0, (shr_point){0, 0}, false, &p), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_rotation_map_point(SHR_ROTATE_180, 4, 3, (shr_point){INT32_MIN, 0}, false, &p), SHR_E_OVERFLOW);
    ASSERT_EQ_LL(shr_rotation_map_point(SHR_ROTATE_180, 4, 3, (shr_point){0, INT32_MIN}, false, &p), SHR_E_OVERFLOW);
    PASS();
}

TEST test_buffer_validation(void) {
    static uint8_t buf[64];
    size_t row;
    ASSERT_EQ_LL(shr_format_row_bytes(SHR_FORMAT_A4, 3, &row), SHR_OK);
    ASSERT_EQ_LL(row, 2);
    ASSERT_EQ_LL(shr_format_row_bytes(SHR_FORMAT_A8, 3, &row), SHR_OK);
    ASSERT_EQ_LL(row, 3);
    ASSERT_EQ_LL(shr_format_row_bytes(SHR_FORMAT_RGB565, 3, &row), SHR_OK);
    ASSERT_EQ_LL(row, 6);
    ASSERT_EQ_LL(shr_format_row_bytes(SHR_FORMAT_RGBX8888, 3, &row), SHR_OK);
    ASSERT_EQ_LL(row, 12);
    ASSERT_EQ_LL(shr_format_row_bytes(SHR_FORMAT_RGBA8888, 3, &row), SHR_OK);
    ASSERT_EQ_LL(row, 12);
    ASSERT_EQ_LL(shr_format_row_bytes(SHR_FORMAT_A8, 3, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_format_row_bytes((shr_pixel_format)0, 3, &row), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_format_row_bytes((shr_pixel_format)99, 3, &row), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_format_row_bytes(SHR_FORMAT_A8, -1, &row), SHR_E_INVALID_ARG);

    const shr_surface ok = {buf, 4, 4, 16, 64, SHR_FORMAT_RGBX8888, 0, 0, 0};
    const struct {
        int32_t w, h;
        size_t stride, len;
        void *pixels;
        shr_pixel_format f;
        shr_memory_domain dom;
        shr_status want;
    } cases[] = {
        {4, 4, 16, 64, buf, SHR_FORMAT_RGBX8888, 0, SHR_OK},
        {4, 4, 8, 64, buf, SHR_FORMAT_RGB565, SHR_MEMORY_DMA, SHR_OK},
        {4, 4, 16, 63, buf, SHR_FORMAT_RGBX8888, 0, SHR_E_INVALID_ARG},
        {4, 4, 15, 64, buf, SHR_FORMAT_RGBX8888, 0, SHR_E_INVALID_ARG},
        {4, 4, 16, 64, NULL, SHR_FORMAT_RGBX8888, 0, SHR_E_INVALID_ARG},
        {0, 4, 0, 0, NULL, SHR_FORMAT_RGBX8888, 0, SHR_OK},
        {4, 0, 0, 0, NULL, SHR_FORMAT_RGBX8888, 0, SHR_OK},
        {-1, 4, 16, 64, buf, SHR_FORMAT_RGBX8888, 0, SHR_E_INVALID_ARG},
        {4, -1, 16, 64, buf, SHR_FORMAT_RGBX8888, 0, SHR_E_INVALID_ARG},
        {4, 4, 16, 64, NULL, SHR_FORMAT_RGBX8888, SHR_MEMORY_DEVICE, SHR_OK},
        {0, 4, 16, 64, NULL, SHR_FORMAT_RGBX8888, SHR_MEMORY_DEVICE, SHR_E_INVALID_ARG},
        {4, 0, 16, 64, NULL, SHR_FORMAT_RGBX8888, SHR_MEMORY_DEVICE, SHR_E_INVALID_ARG},
        {4, 4, 16, 64, buf, SHR_FORMAT_RGBX8888, (shr_memory_domain)3, SHR_E_INVALID_ARG},
        {4, 3, SIZE_MAX / 2 + 1, SIZE_MAX, buf, SHR_FORMAT_RGBX8888, 0, SHR_E_OVERFLOW},
        {4, 2, SIZE_MAX - 4, SIZE_MAX, buf, SHR_FORMAT_RGBX8888, 0, SHR_E_OVERFLOW},
        {4, 4, 16, 64, buf, SHR_FORMAT_A8, 0, SHR_E_INVALID_ARG},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        shr_surface s = {cases[i].pixels, cases[i].w, cases[i].h, cases[i].stride, cases[i].len, cases[i].f, 0,
                         cases[i].dom, 0};
        ASSERT_EQ_LL(shr_surface_validate(&s), cases[i].want);
    }
    ASSERT_EQ_LL(shr_surface_validate(&ok), SHR_OK);
    ASSERT_EQ_LL(shr_surface_validate(NULL), SHR_E_INVALID_ARG);
    shr_image m = {buf, 3, 2, 2, 4, SHR_FORMAT_A4, 0};
    ASSERT_EQ_LL(shr_image_validate(&m), SHR_OK);
    m.format = (shr_pixel_format)0;
    ASSERT_EQ_LL(shr_image_validate(&m), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_image_validate(NULL), SHR_E_INVALID_ARG);
    PASS();
}

/* ---- an asset source the test controls ---- */

typedef struct src_log {
    shr_status ret;
    int reads, cancels;
    uint64_t req, cancelled;
} src_log;

static shr_status sl_read(void *user, uint64_t off, uint32_t len, void *dst, uint64_t req) {
    src_log *s = user;
    s->reads++, s->req = req;
    if (len) memset(dst, (int)off, len);
    return s->ret;
}

static void sl_cancel(void *user, uint64_t req) {
    src_log *s = user;
    s->cancels++, s->cancelled = req;
}

static shr_asset_source source(src_log *s, bool cancellable) {
    shr_asset_source a;
    shr_asset_source_init(&a);
    a.user = s, a.read = sl_read, a.cancel = cancellable ? sl_cancel : NULL;
    return a;
}

TEST test_shutdown_refuses_changes(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    ASSERT_EQ_LL(shr_begin_shutdown(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_destroy(NULL), SHR_E_INVALID_ARG);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    fake a, b, c;
    fake_attach(&a, ctx, &fk_ops);
    fake_attach(&b, ctx, &bare_ops);
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(a.shutdowns, 1);
    shr_lyr *nl = (shr_lyr *)&h;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &nl), SHR_E_STATE);
    ASSERT(nl == NULL);
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = HW, sd.height = HH;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_STATE);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_E_STATE);
    ASSERT_EQ_LL(shr_lyr_set_rect(l, FULL), SHR_E_STATE);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_E_STATE);
    ASSERT_EQ_LL(shr__res_attach(ctx, &c.res, &fk_ops), SHR_E_STATE);
    ASSERT_EQ_LL(shr_pump(ctx), SHR_OK);
    ASSERT_EQ_LL(h.out.presents, 0);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_E_WOULD_BLOCK); /* a layer is alive */
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_E_WOULD_BLOCK); /* resources are alive */
    a.res.dead = b.res.dead = true;
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);
    ASSERT(a.frees == 1 && b.frees == 1);
    PASS();
}

TEST test_destroy_waits_for_frames(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    frame(ctx);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_E_WOULD_BLOCK); /* presented, not released */
    ASSERT_EQ_LL(shr_output_released(ctx, h.out.last_frame), SHR_OK);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);

    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    h.drv.async = true;
    frame(ctx);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_E_WOULD_BLOCK); /* the device still draws */
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    md_complete(&h.drv);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, 1);
    ASSERT(h.out.presents == 0 && h.out.discards == 1);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);

    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    h.out.present_result = SHR_E_WOULD_BLOCK;
    frame(ctx);
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW); /* the waiting frame is superseded */
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);       /* ...or dropped with its buffer */
    ASSERT_EQ_LL(h.out.discards, 1);
    PASS();
}

/* Every context call made from a host callback is refused without effect. */
static struct {
    shr_context *ctx;
    shr_lyr *lyr;
    shr_pl_res_image *img;
    unsigned kinds;
    int probes, wrong;
} host;

enum { P_ACQUIRE, P_PRESENT, P_DISCARD, P_EXECUTE, P_RESET, P_CANCEL, P_SYNC, P_LOG, P_TRACE, P_READ, P_ACANCEL, P_KINDS };

static void probe(unsigned kind) {
    shr_context *ctx = host.ctx;
    if (!ctx) return;
    host.kinds |= 1u << kind;
    host.probes++;
    static const uint32_t rgba = 0xFFFFFFFFu;
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = HW, sd.height = HH;
    shr_event ev;
    shr_deadline dl;
    shr_lyr *l;
    shr_pl_res_image *img;
    shr_rect r = {0, 0, 1, 1}, none = {0, 0, 0, 0};
    shr__lcmd c = fill(r, 0);
    fake f;
    src_log sl = {0};
    shr_asset_source src = source(&sl, true);
    const shr_status st[] = {
        shr_pump(ctx), shr_submit(ctx), shr_poll_event(ctx, &ev), shr_next_deadline(ctx, &dl), shr_output_released(ctx, 1),
        shr_output_displayed(ctx, 1, 0), shr_output_ready(ctx), shr_output_error(ctx, SHR_E_DEVICE),
        shr_output_recover(ctx), shr_request_redraw(ctx), shr_begin_shutdown(ctx), shr_destroy(ctx),
        shr_screen_configure(ctx, &sd), shr_lyr_create(ctx, 0, r, &l), shr_lyr_set_rect(host.lyr, r),
        shr_lyr_set_z(host.lyr, 5), shr_lyr_set_visible(host.lyr, false), shr_lyr_destroy(host.lyr),
        shr_lyr_cmd_begin(host.lyr), shr_lyr_cmd_fill(host.lyr, r, 0), shr_lyr_cmd_commit(host.lyr),
        shr__lyr_group_set(host.lyr, 1, &c, 1), shr__lyr_groups_clear(host.lyr),
        shr__lyr_attach(host.lyr, &host, NULL, NULL, NULL), shr__res_attach(ctx, &f.res, &fk_ops),
        shr__ctx_read(ctx, &f.res, &src, 0, 0, NULL, 0), shr_pl_res_image_create(ctx, 1, 1, &rgba, 4, &img),
        shr_pl_res_image_update(host.img, r, &rgba, 4), shr_pl_res_image_update(host.img, none, NULL, 0),
        shr_lyr_cmd_image(host.lyr, host.img, none, (shr_point){0, 0}), shr_pl_res_image_release(host.img)};
    for (size_t i = 0; i < sizeof(st) / sizeof(st[0]); i++) host.wrong += st[i] != SHR_E_STATE;
    host.wrong += shr_fence_signal(ctx, 1u << 30, SHR_FENCE_SUCCEEDED) != SHR_E_NOT_FOUND;
    host.wrong += shr_asset_complete(ctx, 0, SHR_OK) != SHR_E_NOT_FOUND;
    host.wrong += shr_driver_ready(ctx) != SHR_OK;
    host.wrong += shr_asset_ready(ctx) != SHR_OK;
}

static shr_status probe_acquire(void *user, shr_surface *out) { return probe(P_ACQUIRE), mo_acquire(user, out); }
static shr_status probe_present(void *user, const shr_surface *s, uint64_t id) {
    return probe(P_PRESENT), mo_present(user, s, id);
}
static void probe_discard(void *user, const shr_surface *s) { probe(P_DISCARD), mo_discard(user, s); }
static shr_status probe_execute(void *user, const shr_surface *dst, const shr_draw_cmd *c, size_t n, shr_fence f) {
    return probe(P_EXECUTE), md_execute(user, dst, c, n, f);
}
static shr_status probe_reset(void *user) { return probe(P_RESET), md_reset(user); }
static void probe_cancel(void *user, shr_fence f) { (void)user, (void)f, probe(P_CANCEL); }
static void probe_sync(void *user, const void *p, size_t n) { (void)user, (void)p, (void)n, probe(P_SYNC); }
static void probe_log(void *user, shr_status st, const char *msg) { (void)user, (void)st, (void)msg, probe(P_LOG); }
static void probe_trace(void *user, const shr_trace_event *ev) { (void)user, (void)ev, probe(P_TRACE); }
static shr_status probe_read(void *user, uint64_t off, uint32_t len, void *dst, uint64_t req) {
    (void)user, (void)off, (void)len, (void)dst, (void)req, probe(P_READ);
    return SHR_IN_PROGRESS;
}
static void probe_acancel(void *user, uint64_t req) { (void)user, (void)req, probe(P_ACANCEL); }

static void tweak_probes(shr_context_desc *d, shr_framebuffer_driver *drv) {
    shr_output *o = (shr_output *)d->output;
    o->acquire = probe_acquire, o->present = probe_present, o->discard = probe_discard;
    ((mock_output *)o->user)->domain = SHR_MEMORY_DMA;
    drv->execute = probe_execute, drv->reset = probe_reset, drv->cancel = probe_cancel, drv->sync = probe_sync;
    drv->caps.domains = SHR_MEMORY_CPU | SHR_MEMORY_DMA;
    drv->caps.timeout_ns = 10 * MS;
    d->log = probe_log, d->trace = probe_trace;
}

TEST test_host_callback_reentry(void) {
    harness h;
    memset(&host, 0, sizeof(host));
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_probes);
    static const uint32_t rgba = 0xFFFFFFFFu;
    host.lyr = solid(ctx, 0, FULL, RED);
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 1, 1, &rgba, 4, &host.img), SHR_OK);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_asset_source src;
    shr_asset_source_init(&src);
    src.read = probe_read, src.cancel = probe_acancel;
    host.ctx = ctx;
    frame(ctx); /* acquire, execute, sync, present, trace */
    h.drv.reset_ok = true;
    h.out.present_result = SHR_E_DEVICE; /* discard, log */
    frame(ctx);
    h.out.present_result = SHR_OK;
    h.drv.async = true; /* timeout: cancel, reset */
    frame(ctx);
    fake_now += 20 * MS;
    shr_pump(ctx);
    h.drv.async = false;
    uint8_t buf[4];
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 4, buf, 7), SHR_OK);
    ASSERT(!shr__ctx_read_cancel(ctx, &src));
    shr_pump(ctx);
    ASSERT_EQ_LL(r.io_st, SHR_E_IO);
    ASSERT_EQ_LL(host.kinds, (1u << P_KINDS) - 1);
    ASSERT(host.probes > P_KINDS);
    ASSERT_EQ_LL(host.wrong, 0);
    host.ctx = NULL;
    int presents = h.out.presents;
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, presents + 1); /* the refused output_error() did not isolate it */
    ASSERT_EQ_LL(shr_pl_res_image_release(host.img), SHR_OK);
    destroy_layers(&host.lyr, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

static void tweak_four_events(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->event_capacity = 4; /* the minimum for two unreleased frames */
}

/* Only RESOURCE_FAILED may be dropped; the rest waits for room in the queue. */
TEST test_event_queue_reserves_room(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak_four_events);
    shr_event ev;
    ASSERT_EQ_LL(shr_poll_event(ctx, &ev), SHR_E_NOT_FOUND);
    ASSERT_EQ_LL(shr_poll_event(NULL, &ev), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_poll_event(ctx, NULL), SHR_E_INVALID_ARG);
    for (int i = 0; i < 5; i++) shr__ctx_resource_failed(ctx, SHR_E_CHECKSUM); /* one slot stays reserved */
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, 0); /* no room for the frame's events */
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_RESOURCE_FAILED).status, SHR_E_CHECKSUM);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    for (int i = 0; i < 2; i++) ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_RESOURCE_FAILED).status, SHR_E_CHECKSUM);
    expect_event(ctx, SHR_EVENT_OVERFLOW); /* once, where the fourth was dropped */
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    uint64_t id = h.out.last_frame;
    shr__ctx_resource_failed(ctx, SHR_E_IO);
    shr__ctx_resource_failed(ctx, SHR_E_IO); /* dropped: the release of the frame keeps its slot */
    ASSERT_EQ_LL(shr_output_displayed(ctx, id, 5), SHR_E_WOULD_BLOCK);
    ASSERT_EQ_LL(shr_output_error(ctx, SHR_E_DEVICE), SHR_OK);
    ASSERT_EQ_LL(shr_output_recover(ctx), SHR_E_WOULD_BLOCK);
    ASSERT_EQ_LL(shr_output_released(ctx, id), SHR_OK);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED).frame_id, id);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_RESOURCE_FAILED).status, SHR_E_IO);
    expect_event(ctx, SHR_EVENT_OVERFLOW);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_OUTPUT_ISOLATED).status, SHR_E_DEVICE);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_RELEASED).frame_id, id);
    ASSERT_EQ_LL(shr_poll_event(ctx, &ev), SHR_E_NOT_FOUND);
    ASSERT_EQ_LL(shr_output_recover(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_output_displayed(ctx, id, 5), SHR_OK);
    ev = expect_event(ctx, SHR_EVENT_FRAME_DISPLAYED);
    ASSERT(ev.frame_id == id && ev.timestamp_ns == 5);
    harness_close(&h);
    PASS();
}

/* Every frame ends with one event, also when shutdown or shr_destroy() stops it. */
TEST test_shutdown_ends_every_frame(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    h.drv.block_next = 100;
    frame(ctx);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_E_WOULD_BLOCK); /* a layer is alive */
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, 1);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, rec.submit_id);
    destroy_layers(&l, 1);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);

    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec); /* submitted, never started */
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, rec.submit_id);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    ASSERT_EQ_LL(h.drv.calls, 0);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);

    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_four_events); /* ...once the queue has room */
    for (int i = 0; i < 4; i++) shr__ctx_resource_failed(ctx, SHR_E_IO);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    shr_pump(ctx);
    expect_event(ctx, SHR_EVENT_RESOURCE_FAILED);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    for (int i = 0; i < 2; i++) expect_event(ctx, SHR_EVENT_RESOURCE_FAILED);
    expect_event(ctx, SHR_EVENT_OVERFLOW);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, rec.submit_id);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);
    PASS();
}

/* A composed frame is not converted once shutdown began, unless the conversion already runs. */
TEST test_shutdown_stops_conversion(void) {
    for (int step = 0; step < 3; step++) {
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
        ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
        h.drv.async = true;
        frame(ctx);
        if (step) { /* the conversion is refused, or runs */
            h.drv.async = step == 2, h.drv.block_next = step == 1;
            md_complete(&h.drv);
            shr_pump(ctx);
        }
        ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
        ASSERT_EQ_LL(deadline(ctx), step == 1 ? SHR_DEADLINE_NOW : SHR_DEADLINE_NONE);
        md_complete(&h.drv);
        shr_pump(ctx);
        ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, 1);
        ASSERT_EQ_LL(h.drv.calls, step ? 2 : 1);
        ASSERT(h.out.presents == 0 && h.out.discards == (step ? 1 : 0));
        ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);
    }
    PASS();
}

static void tweak_deadlines(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_timeout(d, drv);
    d->io_timeout_ns = 5 * MS;
}

TEST test_deadline_is_earliest_timer(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_deadlines);
    shr_deadline dl;
    ASSERT_EQ_LL(shr_next_deadline(NULL, &dl), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_next_deadline(ctx, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    r.deadline = 30 * MS;
    ASSERT_EQ_LL(deadline_at(ctx), 30 * MS);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    h.drv.async = true;
    shr_pump(ctx);
    ASSERT_EQ_LL(deadline_at(ctx), 10 * MS); /* the driver watchdog */
    src_log sl = {SHR_IN_PROGRESS, 0, 0, 0, 0};
    shr_asset_source src = source(&sl, true);
    uint8_t buf[4];
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 4, buf, 1), SHR_OK);
    ASSERT_EQ_LL(deadline_at(ctx), 5 * MS); /* the read watchdog */
    r.deadline = 3 * MS;
    ASSERT_EQ_LL(deadline_at(ctx), 3 * MS);
    r.deadline = 30 * MS;
    fake_now = 6 * MS;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(r.io_n == 1 && r.io_st == SHR_E_TIMEOUT && sl.cancels == 1);
    ASSERT_EQ_LL(deadline_at(ctx), 10 * MS);
    md_complete(&h.drv);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(deadline_at(ctx), 30 * MS);
    r.work = true;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    r.work = false;
    fake_now = 30 * MS;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW); /* already due */
    r.deadline = 0;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

TEST test_fence_signal_rules(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    ASSERT_EQ_LL(shr_fence_signal(NULL, 1, SHR_FENCE_SUCCEEDED), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_fence_signal(ctx, 1, SHR_FENCE_PENDING), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_fence_signal(ctx, 1, (shr_fence_state)(SHR_FENCE_CANCELLED + 1)), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_fence_signal(ctx, 1, SHR_FENCE_SUCCEEDED), SHR_E_NOT_FOUND); /* nothing submitted */
    ASSERT_EQ_LL(shr_fence_signal(ctx, 0, SHR_FENCE_SUCCEEDED), SHR_E_INVALID_ARG);  /* never issued */
    frame(ctx);
    ASSERT_EQ_LL(shr_fence_signal(ctx, rec.fence, SHR_FENCE_SUCCEEDED), SHR_E_NOT_FOUND); /* finished at once */
    h.drv.fail_next = 1;
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT_EQ_LL(shr_fence_signal(ctx, rec.fence, SHR_FENCE_SUCCEEDED), SHR_E_NOT_FOUND); /* refused */
    drain(ctx);
    shr_request_redraw(ctx);
    h.drv.async = true;
    const shr_fence_state fails[2] = {SHR_FENCE_FAILED, SHR_FENCE_CANCELLED};
    for (int i = 0; i < 2; i++) {
        frame(ctx);
        shr_fence f = h.drv.pending;
        h.drv.pending = 0;
        ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE); /* no watchdog: completion wakes the application */
        ASSERT_EQ_LL(shr_fence_signal(ctx, f + 1, SHR_FENCE_SUCCEEDED), SHR_E_NOT_FOUND);
        ASSERT_EQ_LL(shr_fence_signal(ctx, f | 1ull << 62, SHR_FENCE_SUCCEEDED), SHR_E_INVALID_ARG); /* no alias */
        ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, NULL), SHR_E_WOULD_BLOCK); /* a frame is drawn */
        if (i) shr_request_redraw(ctx); /* the target's history is gone: nothing to hand back */
        ASSERT_EQ_LL(shr_fence_signal(ctx, f, fails[i]), SHR_OK);
        ASSERT_EQ_LL(shr_fence_signal(ctx, f, SHR_FENCE_SUCCEEDED), SHR_E_NOT_FOUND); /* once */
        ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
        shr_pump(ctx);
        ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_DEVICE);
        ASSERT_EQ_LL(h.out.discards, i + 2);
    }
    frame(ctx);
    md_complete(&h.drv);
    shr_pump(ctx);
    ASSERT(h.out.presents == 2 && rec.damaged == HW * HH);
    ASSERT_EQ_LL(rec.kinds[SHR_TRACE_FENCE_WAIT], 3);
    h.drv.async = false;
    h.drv.fail_next = 1; /* the driver refuses the submission */
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_PRESENT_FAILED, NULL), 1);
    harness_close(&h);
    PASS();
}

static int driver_cancels;
static void count_cancel(void *user, shr_fence f) { (void)user, (void)f, driver_cancels++; }

static void tweak_timeout_cancel(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_timeout(d, drv);
    drv->cancel = count_cancel;
    driver_cancels = 0;
}

TEST test_driver_timeout_with_reset(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_timeout_cancel);
    h.drv.async = true, h.drv.reset_ok = true;
    frame(ctx);
    ASSERT_EQ_LL(deadline_at(ctx), 10 * MS);
    fake_now = 20 * MS;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    shr_event ev = expect_event(ctx, SHR_EVENT_DRIVER_TIMEOUT);
    ASSERT(ev.frame_id == 1 && ev.status == SHR_E_TIMEOUT);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_TIMEOUT);
    ASSERT(h.drv.resets == 1 && driver_cancels == 1 && h.out.discards == 1);
    ASSERT_EQ_LL(shr_fence_signal(ctx, rec.fence, SHR_FENCE_SUCCEEDED), SHR_E_NOT_FOUND); /* reset */
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE); /* a failed frame is not retried by itself */
    h.drv.async = false;
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    harness_close(&h);
    PASS();
}

/* Without a working reset the device may still touch the buffers: they stay held until the fence. */
TEST test_driver_timeout_isolates_buffers(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_timeout);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd g = glyph(&r, 0, 0, WHITE);
    paint(l, 1, &g);
    h.drv.async = true;
    frame(ctx);
    fake_now = 20 * MS;
    shr_pump(ctx);
    expect_event(ctx, SHR_EVENT_DRIVER_TIMEOUT);
    expect_event(ctx, SHR_EVENT_PRESENT_FAILED);
    ASSERT(h.drv.resets == 1 && h.out.discards == 0 && h.out.busy[0] && r.ends == 0);
    ASSERT_EQ_LL(shr_screen_configure(ctx, &(shr_screen_desc){.width = HW, .height = HH}), SHR_E_WOULD_BLOCK);
    destroy_layers(&l, 1);
    r.res.dead = true;
    l = solid(ctx, 0, FULL, GREEN);
    h.drv.async = false;
    frame(ctx);
    ASSERT(h.out.presents == 0 && r.frees == 0); /* no new frame, the glyph pixels stay pinned */
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    md_complete(&h.drv);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(h.out.discards == 1 && r.ends == 1 && r.frees == 1 && h.out.presents == 1);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), GREEN);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

static void tweak_timeout_no_reset(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_timeout(d, drv);
    drv->reset = NULL;
}

TEST test_composed_timeouts(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_timeout_no_reset);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
    h.drv.async = true;
    frame(ctx);
    fake_now = 20 * MS;
    shr_pump(ctx); /* the raster into the composition timed out: no output is held */
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_DRIVER_TIMEOUT).frame_id, 1);
    md_complete(&h.drv);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.discards, 0);

    frame(ctx);
    md_complete(&h.drv);
    shr_pump(ctx); /* the conversion now runs on the device */
    fake_now = 40 * MS;
    shr_pump(ctx);
    drain(ctx);
    ASSERT_EQ_LL(h.drv.resets, 0);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_E_WOULD_BLOCK);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_E_WOULD_BLOCK); /* the device may still write the output */
    ASSERT_EQ_LL(h.out.discards, 0);
    harness_close(&h);
    ASSERT_EQ_LL(h.out.discards, 1);
    PASS();
}

TEST test_output_error_and_recover(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    frame(ctx);
    ASSERT_EQ_LL(shr_output_error(NULL, SHR_E_DEVICE), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_output_recover(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_output_ready(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_output_recover(ctx), SHR_OK); /* not isolated */
    drain(ctx);
    ASSERT_EQ_LL(shr_output_error(ctx, SHR_E_DEVICE), SHR_OK);
    ASSERT_EQ_LL(shr_output_error(ctx, SHR_E_IO), SHR_OK);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_OUTPUT_ISOLATED).status, SHR_E_DEVICE);
    ASSERT_EQ_LL(shr_poll_event(ctx, &(shr_event){0}), SHR_E_NOT_FOUND);
    shr__lcmd g = fill((shr_rect){0, 0, 4, 4}, GREEN);
    paint(l, 1, &g);
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    ASSERT_EQ_LL(shr_output_recover(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 2);
    ASSERT_EQ_LL(rec.damaged, HW * HH); /* the output's contents are no longer trusted */

    /* Isolated while a frame is drawn: it waits for recovery. */
    h.drv.async = true;
    g = fill((shr_rect){0, 0, 4, 4}, BLUE);
    paint(l, 1, &g);
    frame(ctx);
    shr_output_error(ctx, SHR_E_DEVICE);
    md_complete(&h.drv);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 2);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    shr_output_recover(ctx);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 3);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), BLUE);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

TEST test_acquire_failures(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    h.out.acquire_result = SHR_E_WOULD_BLOCK;
    frame(ctx);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE); /* waits for shr_output_ready() */
    h.out.acquire_result = SHR_OK;
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 0);
    ASSERT_EQ_LL(shr_output_ready(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED).frame_id, 1);
    drain(ctx);

    h.out.acquire_result = SHR_E_DEVICE;
    frame(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_DEVICE);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    h.out.acquire_result = SHR_OK;
    for (int i = 0; i < 4; i++) { /* the output no longer matches the screen */
        h.out.w = i == 0 ? 32 : HW, h.out.h = i == 1 ? HH - 1 : HH;
        h.out.format = i == 2 ? OTHER_FORMAT : SHR_PIXEL_FORMAT;
        h.out.domain = i == 3 ? (shr_memory_domain)8 : 0;
        int logs = rec.logs;
        frame(ctx);
        ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_INVALID_ARG);
        ASSERT(h.out.discards == i + 1 && rec.logs == logs + 2);
    }
    h.out.domain = 0;
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, 2);

    /* The composed frame acquires its output only for the conversion. */
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
    h.out.acquire_result = SHR_E_WOULD_BLOCK;
    frame(ctx);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    h.out.acquire_result = SHR_OK;
    shr_output_ready(ctx);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 3);
    drain(ctx);
    h.out.acquire_result = SHR_E_DEVICE;
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_DEVICE);
    h.out.acquire_result = SHR_OK;
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

TEST test_present_failures(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    frame(ctx);
    drain(ctx);
    shr__lcmd g = fill((shr_rect){0, 0, 4, 4}, GREEN);
    h.out.present_result = SHR_E_DEVICE;
    paint(l, 1, &g);
    frame(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_DEVICE);
    ASSERT_EQ_LL(h.out.discards, 1);
    h.out.present_result = SHR_OK;
    frame(ctx);
    ASSERT_EQ_LL(rec.damaged, HW * HH); /* the failed buffer has no trusted history */
    drain(ctx);

    h.out.present_result = SHR_E_WOULD_BLOCK;
    g = fill((shr_rect){0, 0, 4, 4}, BLUE);
    paint(l, 1, &g);
    frame(ctx);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 2);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    h.out.present_result = SHR_OK;
    shr_output_ready(ctx);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 3);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), BLUE);

    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
    h.out.present_result = SHR_E_IO;
    frame(ctx);
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_PRESENT_FAILED, NULL), 1);
    h.out.present_result = SHR_OK;
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* A finished frame is presented first; only one the output still refuses yields to a newer state. */
TEST test_waiting_frame_superseded(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    h.out.present_result = SHR_E_WOULD_BLOCK;
    frame(ctx);
    shr__lcmd g = fill(FULL, GREEN);
    paint(l, 1, &g);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    h.out.present_result = SHR_OK;
    shr_output_ready(ctx);
    shr_pump(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED).frame_id, 1);
    expect_event(ctx, SHR_EVENT_FRAME_RELEASED);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED).frame_id, 2);
    ASSERT(h.out.presents == 2 && h.out.discards == 0);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), GREEN);
    drain(ctx);

    h.out.present_result = SHR_E_WOULD_BLOCK;
    g.color = BLUE;
    paint(l, 1, &g);
    frame(ctx);
    g.color = RED;
    paint(l, 1, &g);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    shr_output_ready(ctx);
    shr_pump(ctx); /* tried again, still refused */
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, 3);
    ASSERT(h.out.presents == 2 && h.out.discards == 1);
    h.out.present_result = SHR_OK;
    shr_output_ready(ctx);
    shr_pump(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED).frame_id, 4);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), RED);

    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
    shr_pump(ctx);
    drain(ctx);
    h.out.acquire_result = SHR_E_WOULD_BLOCK;
    g.color = GREEN;
    paint(l, 1, &g);
    frame(ctx); /* composed; the conversion waits for the output */
    g.color = BLUE;
    paint(l, 1, &g);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    h.out.acquire_result = SHR_OK;
    shr_output_ready(ctx);
    shr_pump(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED).frame_id, 6);
    expect_event(ctx, SHR_EVENT_FRAME_RELEASED);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED).frame_id, 7);
    ASSERT(h.out.presents == 6 && h.out.discards == 1 && px(h.out.shown, 0, 0) == BLUE);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* A new configuration supersedes a frame that waits for the output or the driver. */
TEST test_configure_supersedes_waiting_frame(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    h.out.acquire_result = SHR_E_WOULD_BLOCK;
    frame(ctx); /* composed; the conversion waits for the output */
    shr_surface bad = comp_good;
    bad.width = HW - 1;
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, &bad), SHR_E_INVALID_ARG);
    shr_event ev;
    ASSERT_EQ_LL(shr_poll_event(ctx, &ev), SHR_E_NOT_FOUND); /* a failed call changes nothing */
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, NULL), SHR_OK);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, 1);
    h.out.acquire_result = SHR_OK;
    shr_output_ready(ctx);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED).frame_id, 2);
    drain(ctx);
    ASSERT(h.out.presents == 1 && px(h.out.shown, 0, 0) == RED);

    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_180, 0, 0, NULL), SHR_OK);
    shr__lcmd c = fill(FULL, GREEN);
    paint(l, 1, &c);
    h.drv.async = true;
    frame(ctx);
    h.drv.async = false;
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, NULL), SHR_E_WOULD_BLOCK); /* the raster runs */
    h.drv.block_next = 100;
    md_complete(&h.drv);
    shr_pump(ctx); /* the conversion is refused, holding its output */
    ASSERT_EQ_LL(h.out.discards, 0);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, NULL), SHR_OK);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, 3);
    ASSERT_EQ_LL(h.out.discards, 1);
    h.drv.block_next = 0;
    shr_pump(ctx);
    ASSERT(h.out.presents == 2 && px(h.out.shown, 0, 0) == GREEN && px(h.out.shown, HW - 1, HH - 1) == GREEN);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

TEST test_release_and_display(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak_rec);
    frame(ctx);
    frame(ctx);
    frame(ctx); /* at most two presented frames are unreleased */
    ASSERT_EQ_LL(h.out.presents, 2);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    ASSERT_EQ_LL(shr_output_released(NULL, 1), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_output_released(ctx, 99), SHR_E_NOT_FOUND);
    ASSERT_EQ_LL(shr_output_released(ctx, 1), SHR_OK);
    ASSERT_EQ_LL(shr_output_released(ctx, 1), SHR_E_NOT_FOUND);
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_FRAME_RELEASED, NULL), 1);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.last_frame, 3);
    ASSERT_EQ_LL(shr_output_displayed(NULL, 3, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_output_displayed(ctx, 0, 0), SHR_E_NOT_FOUND);
    ASSERT_EQ_LL(shr_output_displayed(ctx, 4, 0), SHR_E_NOT_FOUND); /* never presented */
    ASSERT_EQ_LL(shr_output_displayed(ctx, 3, 77), SHR_OK);
    ASSERT_EQ_LL(shr_output_displayed(ctx, 3, 78), SHR_E_NOT_FOUND); /* once */
    shr_event ev;
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_FRAME_DISPLAYED, &ev), 1);
    ASSERT(ev.frame_id == 3 && ev.timestamp_ns == 77);
    ASSERT_EQ_LL(rec.kinds[SHR_TRACE_DISPLAYED], 1);
    shr_output_released(ctx, 2);
    shr_output_released(ctx, 3);
    for (int i = 0; i < SHR_DISPLAY_HISTORY + 1; i++) {
        frame(ctx);
        ASSERT_EQ_LL(shr_output_released(ctx, h.out.last_frame), SHR_OK);
        drain(ctx);
    }
    ASSERT_EQ_LL(shr_output_displayed(ctx, h.out.last_frame - SHR_DISPLAY_HISTORY, 1), SHR_E_NOT_FOUND); /* too old */
    ASSERT_EQ_LL(shr_output_displayed(ctx, h.out.last_frame - SHR_DISPLAY_HISTORY + 1, 1), SHR_OK);
    harness_close(&h);
    PASS();
}

static void tweak_no_timestamps(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)drv;
    ((shr_output *)d->output)->timestamp = SHR_TIMESTAMP_NONE;
}

TEST test_output_without_timestamps(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_no_timestamps);
    frame(ctx);
    ASSERT_EQ_LL(shr_output_displayed(ctx, h.out.last_frame, 1), SHR_E_UNSUPPORTED);
    harness_close(&h);
    PASS();
}

/* Each buffer of a preserving output redraws what changed since it was last drawn. */
TEST test_preserved_damage_history(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd c[2] = {fill((shr_rect){0, 0, 4, 4}, RED), fill((shr_rect){40, 32, 44, 36}, RED)};
    paint(l, 2, c);
    frame(ctx);
    ASSERT(h.out.last_buf == 0 && rec.damaged == HW * HH);
    c[0].color = GREEN;
    paint(l, 2, c);
    h.out.busy[0] = true;
    frame(ctx);
    ASSERT(h.out.last_buf == 1 && rec.damaged == HW * HH); /* a new buffer has no history */
    c[1].color = GREEN;
    paint(l, 2, c);
    h.out.busy[0] = false, h.out.busy[1] = true;
    memset(h.out.bufs[0] + (20 * HW + 20) * SCREEN_BPP, 0x5A, SCREEN_BPP); /* canary outside any change */
    uint32_t canary = px(h.out.bufs[0], 20, 20);
    frame(ctx);
    ASSERT(h.out.last_buf == 0 && rec.damaged == 32); /* both changes since buffer 0 was drawn */
    ASSERT_EQ_LL(px(h.out.shown, 20, 20), canary);
    ASSERT(px(h.out.shown, 0, 0) == GREEN && px(h.out.shown, 40, 32) == GREEN);
    h.out.busy[0] = h.out.busy[1] = true;
    frame(ctx);
    ASSERT(h.out.last_buf == 2 && rec.damaged == HW * HH);
    c[0].color = BLUE;
    paint(l, 2, c);
    h.out.busy[1] = false, h.out.busy[2] = true;
    frame(ctx);
    ASSERT(h.out.last_buf == 1 && rec.damaged == 32); /* triple buffering: the last two changes */
    ASSERT(px(h.out.shown, 0, 0) == BLUE && px(h.out.shown, 40, 32) == GREEN);
    h.out.busy[0] = h.out.busy[2] = false;
    frame(ctx);
    ASSERT(h.out.last_buf == 0 && rec.damaged == 16);
    int frames = rec.frames, presents = h.out.presents;
    drain(ctx);
    frame(ctx); /* the buffer already shows this state */
    ASSERT(rec.frames == frames && h.out.presents == presents && h.out.discards == 1);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, h.out.last_frame + 1);
    h.out.gen[0]++;
    frame(ctx);
    ASSERT_EQ_LL(rec.damaged, HW * HH); /* its contents were lost */
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* Device surfaces without pixels are preserved per resource_id, four at a time; the least recently used
 * is forgotten. */
static struct {
    uint64_t id;
    uint32_t gen;
} dev;
static shr_status dev_acquire(void *user, shr_surface *out) {
    (void)user;
    *out = (shr_surface){NULL, HW, HH, 0, 0, SHR_PIXEL_FORMAT, dev.gen, SHR_MEMORY_DEVICE, dev.id};
    return SHR_OK;
}
static shr_status dev_present(void *user, const shr_surface *s, uint64_t frame_id) {
    (void)s, (void)frame_id;
    ((mock_output *)user)->presents++;
    return SHR_OK;
}
static void dev_discard(void *user, const shr_surface *s) { (void)s, ((mock_output *)user)->discards++; }

static void tweak_device(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    shr_output *o = (shr_output *)d->output;
    o->acquire = dev_acquire, o->present = dev_present, o->discard = dev_discard;
    drv->caps.domains |= SHR_MEMORY_DEVICE;
}

TEST test_device_surface_identity(void) {
    harness h;
    memset(&dev, 0, sizeof(dev));
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_device);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const struct {
        uint64_t id;
        uint64_t damage;
    } steps[] = {{0, HW * HH}, {1, HW * HH}, {2, HW * HH}, {3, HW * HH}, {4, HW * HH}, {2, 4},
                 {1, 4},       {5, HW * HH}, {3, HW * HH}, {2, 4},       {4, HW * HH}};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        dev.id = steps[i].id;
        shr__lcmd c = fill((shr_rect){0, 0, 2, 2}, i % 2 ? RED : GREEN);
        paint(l, 1, &c);
        frame(ctx);
        ASSERT_EQ_LL(h.out.presents, (int)i + 1);
        ASSERT_EQ_LL(rec.damaged, steps[i].damage);
    }
    dev.gen++;
    frame(ctx);
    ASSERT_EQ_LL(rec.damaged, HW * HH);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

TEST test_request_redraw(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    ASSERT_EQ_LL(shr_request_redraw(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_submit(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pump(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE); /* nothing was ever submitted */
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    frame(ctx);
    shr_pump(ctx);
    ASSERT(h.out.presents == 1 && rec.frames == 1);
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(h.out.presents == 2 && rec.damaged == HW * HH);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

static shr_lyr *blink_layer(shr_context *ctx, shr_rect rect) {
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, rect, &l), SHR_OK);
    shr__lcmd c[2] = {fill((shr_rect){0, 0, 8, 16}, WHITE), fill((shr_rect){20, 0, 28, 16}, WHITE)};
    c[0].flags = SHR__LCMD_BLINK;
    paint(l, 2, c);
    return l;
}

TEST test_blink_phase(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_blink);
    shr_lyr *l = blink_layer(ctx, FULL), *hidden = blink_layer(ctx, (shr_rect){0, 20, 30, 36});
    ASSERT_EQ_LL(shr_lyr_set_visible(hidden, false), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), WHITE);
    ASSERT_EQ_LL(deadline_at(ctx), 100 * MS);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    r.deadline = 50 * MS; /* the earliest timer wins */
    ASSERT_EQ_LL(deadline_at(ctx), 50 * MS);
    r.deadline = 150 * MS;
    ASSERT_EQ_LL(deadline_at(ctx), 100 * MS);
    r.deadline = 0;
    fake_now = 150 * MS;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(h.out.presents == 2 && px(h.out.shown, 0, 0) == 0 && px(h.out.shown, 20, 0) == WHITE);
    ASSERT_EQ_LL(rec.damaged, 8 * 16); /* only the blinking command */
    ASSERT_EQ_LL(deadline_at(ctx), 200 * MS);
    fake_now = 350 * MS; /* late: already hidden again */
    ASSERT_EQ_LL(deadline_at(ctx), 400 * MS);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 2);
    fake_now = 420 * MS;
    shr_pump(ctx);
    ASSERT(h.out.presents == 3 && px(h.out.shown, 0, 0) == WHITE);
    ASSERT_EQ_LL(shr_lyr_set_visible(l, false), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE); /* hidden layers do not blink */
    destroy_layers(&l, 1);
    destroy_layers(&hidden, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

static void tweak_blink_restart(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_blink(d, drv);
    d->blink.restart = SHR_BLINK_RESTART_ON_SUBMIT;
}

TEST test_blink_restarts_on_submit(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_blink_restart);
    shr_lyr *l = blink_layer(ctx, FULL);
    fake_now = 50 * MS;
    frame(ctx);
    ASSERT_EQ_LL(deadline_at(ctx), 150 * MS);
    fake_now = 160 * MS;
    shr_pump(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), 0);
    fake_now = 170 * MS;
    frame(ctx); /* visible again at once */
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), WHITE);
    ASSERT_EQ_LL(deadline_at(ctx), 270 * MS);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

static void tweak_blink_later(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->blink = (shr_blink_profile){100 * MS, 500 * MS, false, SHR_BLINK_RESTART_NONE};
}

TEST test_blink_epoch_in_future(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_blink_later);
    shr_lyr *l = blink_layer(ctx, FULL);
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), 0);
    ASSERT_EQ_LL(deadline_at(ctx), 500 * MS);
    fake_now = 650 * MS;
    shr_pump(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), WHITE);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

static void tweak_blink_no_clock(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_blink_restart(d, drv);
    d->now_ns = NULL, d->io_retry_ns = d->io_timeout_ns = 0;
}

TEST test_blink_needs_clock(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_blink_no_clock);
    shr_lyr *l = blink_layer(ctx, FULL);
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), WHITE);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    r.deadline = 5;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE); /* never AT without a clock */
    r.res.dead = true;
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

TEST test_blink_outside_screen_draws_nothing(void) {
    for (int composing = 0; composing < 2; composing++) {
        harness h;
        shr_context *ctx = harness_open(&h, PRESERVED, tweak_blink);
        if (composing) ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
        shr_lyr *l = blink_layer(ctx, (shr_rect){100, 100, 130, 116});
        frame(ctx);
        fake_now = 150 * MS;
        ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
        shr_pump(ctx); /* nothing visible changed: no raster, no present */
        ASSERT(h.out.presents == 1 && rec.frames == 1 && h.out.discards == !composing);
        ASSERT_EQ_LL(deadline_at(ctx), 200 * MS);
        destroy_layers(&l, 1);
        harness_close(&h);
    }
    PASS();
}

static uint32_t max_cmds;
static void tweak_max_cmds(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->max_commands = max_cmds;
}

/* The limit applies while commands are emitted: clear, cache begin, fill, cache end, glyph. */
TEST test_command_limit_fails_frame(void) {
    for (max_cmds = 1; max_cmds <= 5; max_cmds++) {
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_max_cmds);
        fake r;
        fake_attach(&r, ctx, &fk_ops);
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        const shr__lcmd cached[3] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, 8, 16}, .key = {1}},
                                     fill((shr_rect){0, 0, 8, 16}, RED),
                                     {.kind = SHR__LCMD_CACHE_END}};
        shr__lcmd g = glyph(&r, 16, 0, WHITE);
        ASSERT_EQ_LL(shr__lyr_group_set(l, 0, cached, 3), SHR_OK);
        ASSERT_EQ_LL(shr__lyr_group_set(l, 1, &g, 1), SHR_OK);
        frame(ctx);
        if (max_cmds < 5)
            ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_LIMIT);
        else
            expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
        destroy_layers(&l, 1);
        r.res.dead = true;
        harness_close(&h);
    }
    PASS();
}

TEST test_driver_would_block(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    h.drv.block_next = 1;
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, 0);
    ASSERT_EQ_LL(deadline_at(ctx), 50 * MS); /* retried after io_retry_ns */
    shr_pump(ctx);
    ASSERT_EQ_LL(h.drv.calls, 1); /* not before */
    ASSERT_EQ_LL(h.out.presents, 0);
    fake_now = 50 * MS;
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);

    /* A frame the driver never accepted yields to a newer state. */
    shr__lcmd c = fill(FULL, GREEN);
    h.drv.block_next = 100;
    paint(l, 1, &c);
    frame(ctx);
    fake_now = 60 * MS;
    ASSERT_EQ_LL(deadline_at(ctx), 100 * MS);
    fake_now = 100 * MS;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(deadline_at(ctx), 150 * MS);
    uint64_t refused = h.out.last_frame + 1;
    c.color = BLUE;
    paint(l, 1, &c);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    h.drv.block_next = 0;
    drain(ctx);
    shr_pump(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, refused);
    ASSERT(h.out.presents == 2 && h.out.discards == 1 && px(h.out.shown, 0, 0) == BLUE);

    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
    for (int replace = 0; replace < 2; replace++) {
        c.color = replace ? GREEN : BLUE;
        paint(l, 1, &c);
        h.drv.async = true;
        frame(ctx);
        h.drv.async = false;
        h.drv.block_next = replace ? 100 : 1;
        md_complete(&h.drv);
        shr_pump(ctx); /* the conversion is blocked, holding its output */
        ASSERT_EQ_LL(deadline_at(ctx), fake_now + 50 * MS);
        if (replace) {
            c.color = RED;
            paint(l, 1, &c);
            ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
            ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
            h.drv.block_next = 0;
        } else {
            fake_now += 50 * MS;
        }
        shr_pump(ctx);
        ASSERT_EQ_LL(h.out.presents, 3 + replace);
    }
    ASSERT(h.out.discards == 2 && px(h.out.shown, 0, 0) == RED);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

static void tweak_no_retry_delay(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->io_retry_ns = 0;
}

/* A refused submission is retried once the driver says it is ready; without a retry delay only then. */
TEST test_driver_ready_retries(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_no_retry_delay);
    h.drv.block_next = 100;
    frame(ctx);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE); /* no busy loop */
    shr_pump(ctx);
    ASSERT_EQ_LL(h.drv.calls, 1);
    h.drv.block_next = 0;
    ASSERT_EQ_LL(shr_driver_ready(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(shr_driver_ready(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_asset_ready(NULL), SHR_E_INVALID_ARG);
    harness_close(&h);

    ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec); /* before io_retry_ns has passed */
    h.drv.block_next = 1;
    frame(ctx);
    ASSERT_EQ_LL(deadline_at(ctx), 50 * MS);
    shr_driver_ready(ctx);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    harness_close(&h);
    PASS();
}

static shr_driver_caps caps_case;
static void tweak_caps(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps = caps_case;
}

/* Destinations and sources outside the driver's limits fail the frame with SHR_E_UNSUPPORTED. */
TEST test_driver_caps_checked(void) {
    const struct {
        shr_driver_caps caps;
        bool odd_src, dma_src, ok;
    } cases[] = {
        {{0, 0, 0, 0, 0, 0}, false, false, true}, /* no domains: CPU */
        {{SHR_MEMORY_CPU, 1, 1, HW, HH, 0}, false, false, true},
        {{SHR_MEMORY_DMA, 0, 0, 0, 0, 0}, false, false, false},
        {{SHR_MEMORY_CPU, 0, 1000, 0, 0, 0}, false, false, false},
        {{SHR_MEMORY_CPU, 0, 0, 10, 0, 0}, false, false, false},
        {{SHR_MEMORY_CPU, 0, 0, 0, 10, 0}, false, false, false},
        {{SHR_MEMORY_CPU, 2, 0, 0, 0, 0}, true, false, false},
        {{SHR_MEMORY_CPU, 0, 0, 0, 0, 0}, false, true, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        caps_case = cases[i].caps;
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_caps);
        fake r;
        fake_attach(&r, ctx, &fk_ops);
        if (cases[i].odd_src) r.px.image.pixels = r.cov + 1;
        if (cases[i].dma_src) r.px.image.domain = SHR_MEMORY_DMA;
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        shr__lcmd g = glyph(&r, 0, 0, WHITE);
        paint(l, 1, &g);
        frame(ctx);
        if (cases[i].ok)
            expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
        else
            ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_UNSUPPORTED);
        destroy_layers(&l, 1);
        r.res.dead = true;
        harness_close(&h);
    }
    PASS();
}

static void tweak_sync(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps.domains = SHR_MEMORY_CPU | SHR_MEMORY_DMA;
    drv->sync = rec_sync;
    ((mock_output *)d->output->user)->domain = SHR_MEMORY_DMA;
}

/* sync() hands the destination and each DMA source to the device before execute(). */
TEST test_dma_sync(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_sync);
    fake a, b;
    fake_attach(&a, ctx, &fk_ops);
    fake_attach(&b, ctx, &fk_ops);
    a.px.image.domain = SHR_MEMORY_DMA;
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr__lcmd c[5] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, 16, 16}, .key = {1}}, glyph(&a, 0, 0, WHITE),
                            glyph(&a, 8, 0, WHITE), {.kind = SHR__LCMD_CACHE_END}, glyph(&b, 16, 0, WHITE)};
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, c, 5), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(rec.syncs, 2);
    ASSERT(rec.sync_addr[0] == h.out.bufs[h.out.last_buf] && rec.sync_addr[1] == a.cov);
    h.out.domain = SHR_MEMORY_CPU;
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT(rec.syncs == 3 && rec.sync_addr[2] == a.cov);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK); /* CPU memory will do */
    destroy_layers(&l, 1);
    a.res.dead = b.res.dead = true;
    harness_close(&h);
    PASS();
}

static void limited(long *budget, shr_status (*call)(shr_context *), shr_context *ctx) {
    oom.budget = *budget;
    call(ctx);
    *budget = oom.budget;
    oom.budget = -1;
}

/* Every allocation a frame makes may fail: the frame then fails with SHR_E_NO_MEMORY and nothing leaks. */
TEST test_frame_out_of_memory(void) {
    for (int mode = 0; mode < 3; mode++) {
        for (long budget = 0;; budget++) {
            harness h;
            shr_context *ctx = harness_open(&h, PRESERVED, tweak_oom);
            if (mode == 1) ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
            fake r, q; /* q has no frame_end(); in mode 1 it is resolved first */
            fake_attach(&r, ctx, &fk_ops);
            fake_attach(&q, ctx, &bare_ops);
            r.px.provisional = mode == 2;
            shr_lyr *l;
            ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
            const shr__lcmd cached[4] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, 32, 16}, .key = {1}},
                                         fill((shr_rect){0, 0, 32, 16}, BLUE), glyph(mode == 1 ? &q : &r, 8, 0, WHITE),
                                         {.kind = SHR__LCMD_CACHE_END}};
            ASSERT_EQ_LL(shr__lyr_group_set(l, 0, cached, 4), SHR_OK);
            shr__lcmd dots[12];
            for (int i = 0; i < 12; i++) dots[i] = glyph(&r, i % 6 * 10, 16 + i / 6 * 16, RED);
            ASSERT_EQ_LL(shr__lyr_group_set(l, 1, dots, 12), SHR_OK);
            long left = budget;
            limited(&left, shr_submit, ctx);
            limited(&left, shr_pump, ctx);
            for (int i = 0; i < 12; i++) dots[i].color = GREEN, dots[i].dst.x1 -= 4;
            ASSERT_EQ_LL(shr__lyr_group_set(l, 1, dots, 12), SHR_OK);
            limited(&left, shr_submit, ctx);
            limited(&left, shr_pump, ctx);
            r.changed = true;
            limited(&left, shr_pump, ctx);
            int presents = h.out.presents;
            shr_event ev;
            while (shr_poll_event(ctx, &ev) == SHR_OK)
                if (ev.kind == SHR_EVENT_PRESENT_FAILED) ASSERT_EQ_LL(ev.status, SHR_E_NO_MEMORY);
            destroy_layers(&l, 1);
            r.res.dead = q.res.dead = true;
            harness_close(&h);
            ASSERT_EQ_LL(oom.live, 0);
            if (left) {
                ASSERT_EQ_LL(presents, mode == 2 ? 3 : 2);
                break;
            }
        }
    }
    PASS();
}

TEST test_scattered_damage(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd c[70]; /* more than the damage list holds */
    for (int i = 0; i < 70; i++) c[i] = fill((shr_rect){i % 16 * 4, i / 16 * 4, i % 16 * 4 + 1, i / 16 * 4 + 1}, RED);
    paint(l, 70, c);
    frame(ctx);
    for (int i = 0; i < 70; i++) c[i].color = GREEN;
    paint(l, 70, c);
    frame(ctx);
    ASSERT(rec.damaged >= 70 && rec.damaged < HW * HH);
    ASSERT(count_color(&h, GREEN) == 70 && count_color(&h, RED) == 0);
    c[0] = fill((shr_rect){0, 0, 60, 48}, BLUE); /* most of the screen: redrawn whole */
    paint(l, 1, c);
    frame(ctx);
    ASSERT_EQ_LL(rec.damaged, HW * HH);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* A damage list that cannot grow falls back to a full redraw. */
TEST test_damage_out_of_memory_redraws_all(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_oom);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd c[24];
    for (int i = 0; i < 24; i++) c[i] = glyph(&r, i % 8 * 8, i / 8 * 16, WHITE);
    paint(l, 24, c);
    frame(ctx);
    oom.budget = 0;
    shr__res_changed(&r.res, (shr_rect){0, 0, 1, 1});
    oom.budget = -1;
    shr_pump(ctx);
    ASSERT_EQ_LL(rec.damaged, HW * HH);
    oom.budget = 0; /* a layer change that cannot be listed: still not drawn before shr_submit() */
    for (int i = 0; i < SHR_MAX_DAMAGE && !ctx->staged.full; i++)
        shr__damage_add(ctx, &ctx->staged, (shr_rect){i % 16 * 4, i / 16 * 4, i % 16 * 4 + 1, i / 16 * 4 + 1});
    oom.budget = -1;
    ASSERT(ctx->staged.full);
    shr_request_redraw(ctx);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    int presents = h.out.presents;
    frame(ctx);
    ASSERT(h.out.presents == presents + 1 && rec.damaged == HW * HH);
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

/* A finished frame is presented even when the next submission already waits. */
TEST test_async_frames_keep_presenting(void) {
    for (int composing = 0; composing < 2; composing++) {
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
        if (composing) ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
        shr_lyr *l = solid(ctx, 0, FULL, RED);
        h.drv.async = true;
        for (int i = 0; i < 20; i++) { /* a new state each time the device finishes */
            shr__lcmd c = fill(FULL, i & 1 ? GREEN : BLUE);
            paint(l, 1, &c);
            ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
            shr_pump(ctx);
            md_complete(&h.drv);
        }
        ASSERT_EQ_LL(h.out.presents, composing ? 9 : 19);
        ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_FRAME_SUPERSEDED, NULL), 0);
        for (int i = 0; i < 4; i++) shr_pump(ctx), md_complete(&h.drv);
        ASSERT_EQ_LL(count_color(&h, GREEN), HW * HH);
        destroy_layers(&l, 1);
        harness_close(&h);
    }
    PASS();
}

/* A damage list that cannot be allocated fails the frame; its target is then redrawn whole. */
TEST test_damage_list_out_of_memory_forgets_target(void) {
    for (int composing = 0; composing < 3; composing++) { /* 2: an output that preserves nothing */
        harness h;
        shr_context *ctx = harness_open(&h, composing == 2 ? SHR_OUTPUT_RELEASE_ON_PRESENT : PRESERVED, tweak_oom);
        if (composing == 1) ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
        memset(h.out.bufs, 0x55, sizeof(h.out.bufs));
        if (composing == 1) memset(ctx->composition.pixels, 0x55, ctx->composition.byte_length);
        shr_lyr *l[2] = {solid(ctx, 0, FULL, RED), NULL};
        ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
        oom.budget = 0;
        shr_pump(ctx);
        oom.budget = -1;
        ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_NO_MEMORY);
        l[1] = solid(ctx, 1, (shr_rect){0, 0, 1, 1}, GREEN);
        frame(ctx);
        ASSERT(count_color(&h, RED) == HW * HH - 1 && count_color(&h, GREEN) == 1);
        destroy_layers(l, 2);
        harness_close(&h);
        ASSERT_EQ_LL(oom.live, 0);
    }
    PASS();
}

/* Blink, redraw and resolved-fallback frames never show layer changes that were not submitted. */
TEST test_automatic_frames_wait_for_submit(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_blink);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l = solid(ctx, 0, FULL, RED), *b = blink_layer(ctx, FULL);
    frame(ctx);
    shr__lcmd g = fill(FULL, GREEN);
    paint(l, 1, &g);
    fake_now = 150 * MS;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    shr_request_redraw(ctx);
    r.changed = true;
    shr_pump(ctx);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    ASSERT(h.out.presents == 1 && px(h.out.shown, 40, 40) == RED);
    frame(ctx);
    ASSERT(h.out.presents == 2 && px(h.out.shown, 40, 40) == GREEN && px(h.out.shown, 0, 0) == GREEN);
    ASSERT_EQ_LL(deadline_at(ctx), 200 * MS);
    shr_lyr *ls[2] = {l, b};
    destroy_layers(ls, 2);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

static void tweak_huge_timers(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->blink = (shr_blink_profile){1ull << 63, 0, true, SHR_BLINK_RESTART_NONE};
    d->io_timeout_ns = UINT64_MAX;
    drv->caps.timeout_ns = UINT64_MAX;
}

/* Timer sums past 2^64 saturate instead of wrapping into the past. */
TEST test_timers_saturate(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_huge_timers);
    fake_now = (1ull << 63) + 5;
    shr_lyr *l = blink_layer(ctx, FULL);
    frame(ctx);
    ASSERT_EQ_LL(deadline_at(ctx), UINT64_MAX); /* the next blink phase */
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    src_log sl = {SHR_IN_PROGRESS, 0, 0, 0, 0};
    shr_asset_source src = source(&sl, true);
    uint8_t buf[4];
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 4, buf, 1), SHR_OK);
    h.drv.async = true;
    shr_request_redraw(ctx);
    shr_pump(ctx);
    shr_pump(ctx);
    ASSERT(r.io_n == 0 && sl.cancels == 0);
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_DRIVER_TIMEOUT, NULL), 0);
    ASSERT_EQ_LL(deadline_at(ctx), UINT64_MAX);
    ASSERT_EQ_LL(shr_asset_complete(ctx, sl.req, SHR_OK), SHR_OK);
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

/* A composition the driver cannot draw into or read from is refused; the old configuration stays. */
TEST test_configure_rejects_unreachable_composition(void) {
    const shr_driver_caps caps[2] = {{SHR_MEMORY_CPU, 0, 1000, 0, 0, 0}, {SHR_MEMORY_CPU, 0, 0, HW - 1, 0, 0}};
    for (int i = 0; i < 2; i++) {
        caps_case = caps[i];
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_caps);
        ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_90_CW, 0, 0, NULL), SHR_E_UNSUPPORTED);
        shr_surface comp = comp_good;
        comp.domain = SHR_MEMORY_DMA;
        ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, &comp), SHR_E_UNSUPPORTED);
        ASSERT(!ctx->composing && ctx->screen.rotation == SHR_ROTATE_NONE);
        harness_close(&h);
    }
    PASS();
}

/* ===== layers ===== */

TEST test_layer_arguments(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    const shr_rect bad = {1, 0, 0, 0};
    shr_lyr *l = (shr_lyr *)&h;
    ASSERT_EQ_LL(shr_lyr_create(NULL, 0, FULL, &l), SHR_E_INVALID_ARG);
    ASSERT(l == NULL);
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, bad, &l), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 1, 0, 0}, &l), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){3, 4, 3, 4}, &l), SHR_OK); /* empty is fine */
    ASSERT_EQ_LL(shr_lyr_set_rect(NULL, FULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_set_rect(l, bad), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_set_z(NULL, 1), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_set_visible(NULL, true), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_destroy(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_fill(NULL, FULL, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_E_STATE);     /* no begin */
    ASSERT_EQ_LL(shr_lyr_cmd_fill(l, FULL, 0), SHR_E_STATE);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_fill(l, bad, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__lyr_cmd_add(l, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__lyr_cmd_add(NULL, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_group_set(NULL, 0, NULL, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, NULL, 1), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__lyr_groups_clear(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__lyr_attach(NULL, &h, NULL, NULL, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__lyr_attach(l, NULL, NULL, NULL, NULL), SHR_E_INVALID_ARG);
    ASSERT(!shr__lyr_state(NULL, &h) && !shr__lyr_state(l, NULL) && !shr__lyr_state(l, &h));
    ASSERT(shr__lyr_ctx(l) == ctx && shr__lyr_rect(l).x0 == 3 && shr__lyr_rect(l).y1 == 4);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

TEST test_layer_out_of_memory(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_oom);
    shr_lyr *l = (shr_lyr *)&h;
    oom.budget = 0;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_E_NO_MEMORY);
    ASSERT(l == NULL);
    oom.budget = -1;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    oom.budget = 0;
    ASSERT_EQ_LL(shr_lyr_cmd_fill(l, FULL, RED), SHR_E_NO_MEMORY);
    oom.budget = -1;
    ASSERT_EQ_LL(shr_lyr_cmd_fill(l, FULL, RED), SHR_OK);
    for (long budget = 0; budget < 2; budget++) { /* the command copy, then the group list */
        oom.budget = budget;
        ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_E_NO_MEMORY);
        oom.budget = -1;
    }
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK); /* still building after the failures */
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), RED);
    destroy_layers(&l, 1);
    harness_close(&h);
    ASSERT_EQ_LL(oom.live, 0);
    PASS();
}

TEST test_layer_order(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    shr_lyr *l[3] = {solid(ctx, 1, FULL, RED), solid(ctx, 0, FULL, GREEN), solid(ctx, 1, (shr_rect){0, 0, 8, 8}, BLUE)};
    frame(ctx); /* z, then creation order */
    ASSERT(px(h.out.shown, 0, 0) == BLUE && px(h.out.shown, 20, 20) == RED);
    int frames = rec.frames;
    ASSERT_EQ_LL(shr_lyr_set_z(l[0], 1), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(rec.frames, frames); /* unchanged */
    ASSERT_EQ_LL(shr_lyr_set_z(l[0], 5), SHR_OK);
    frame(ctx);
    ASSERT(px(h.out.shown, 0, 0) == RED && px(h.out.shown, 20, 20) == RED);
    ASSERT_EQ_LL(shr_lyr_set_z(l[0], 0), SHR_OK); /* before the later-created layer of z 0 */
    frame(ctx);
    ASSERT(px(h.out.shown, 0, 0) == BLUE && px(h.out.shown, 20, 20) == GREEN);
    destroy_layers(l, 3);
    frame(ctx);
    ASSERT_EQ_LL(count_color(&h, 0), HW * HH);
    harness_close(&h);
    PASS();
}

TEST test_layer_rect_clips_and_moves(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){10, 10, 20, 20}, &l), SHR_OK);
    shr__lcmd c = fill((shr_rect){0, 0, 100, 100}, RED);
    paint(l, 1, &c);
    frame(ctx);
    ASSERT(count_color(&h, RED) == 100 && px(h.out.shown, 10, 10) == RED && px(h.out.shown, 20, 20) == 0);
    ASSERT_EQ_LL(shr_lyr_set_rect(l, (shr_rect){30, 30, 40, 40}), SHR_OK);
    frame(ctx);
    ASSERT(rec.damaged == 200 && px(h.out.shown, 10, 10) == 0 && px(h.out.shown, 30, 30) == RED);
    ASSERT_EQ_LL(shr_lyr_set_visible(l, false), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(count_color(&h, RED), 0);
    int frames = rec.frames;
    ASSERT_EQ_LL(shr_lyr_set_visible(l, false), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_set_rect(l, (shr_rect){0, 0, 10, 10}), SHR_OK); /* hidden: nothing to redraw */
    c.color = GREEN;
    paint(l, 1, &c);
    frame(ctx);
    ASSERT_EQ_LL(rec.frames, frames);
    ASSERT_EQ_LL(shr_lyr_set_visible(l, true), SHR_OK);
    frame(ctx);
    ASSERT(count_color(&h, GREEN) == 100 && px(h.out.shown, 0, 0) == GREEN);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* Application layers are rewritten whole; only what differs is redrawn. */
TEST test_app_layer_diff(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd c[3] = {fill(FULL, BLUE), fill((shr_rect){0, 0, 8, 8}, RED), fill((shr_rect){16, 16, 24, 24}, GREEN)};
    paint(l, 3, c);
    frame(ctx);
    int frames = rec.frames;
    paint(l, 3, c);
    frame(ctx);
    ASSERT(rec.frames == frames && h.out.discards == 1);
    memset(h.out.bufs[0] + (40 * HW + 40) * SCREEN_BPP, 0x5A, SCREEN_BPP);
    uint32_t canary = px(h.out.bufs[0], 40, 40);
    c[1].color = GREEN;
    paint(l, 3, c);
    frame(ctx);
    ASSERT(rec.damaged == 64 && rec.commands == 2 && rec.n == 2); /* background, the changed fill: no clear below */
    ASSERT(rec.cmds[1].dst.x0 == 0 && rec.cmds[1].dst.x1 == 8 && rec.cmds[1].color == GREEN);
    ASSERT(px(h.out.shown, 40, 40) == canary && px(h.out.shown, 0, 0) == GREEN);
    shr__lcmd two[2] = {c[0], c[2]}; /* compared by position: both later fills changed */
    paint(l, 2, two);
    frame(ctx);
    ASSERT(rec.damaged == 128 && px(h.out.shown, 0, 0) == BLUE && px(h.out.shown, 16, 16) == GREEN);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_fill(l, FULL, RED), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK); /* starts over */
    ASSERT_EQ_LL(shr_lyr_cmd_fill(l, (shr_rect){0, 0, 1, 1}, GREEN), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    frame(ctx);
    ASSERT(count_color(&h, GREEN) == 1 && px(h.out.shown, 0, 0) == GREEN);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

TEST test_command_diff_compares_every_field(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    fake r, r2;
    fake_attach(&r, ctx, &fk_ops);
    fake_attach(&r2, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr__lcmd base = glyph(&r, 0, 0, WHITE);
    shr__lcmd v[11];
    for (int i = 0; i < 11; i++) v[i] = base;
    v[0].kind = SHR__LCMD_IMAGE, v[1].flags = SHR__LCMD_DIM, v[2].dst.x1 = 7, v[3].anchor.x = 1, v[4].anchor.y = 1;
    v[5].color = RED, v[6].res = &r2.res, v[7].id = 2, v[8].key[0] = 1, v[9].key[1] = 1;
    for (int i = 0; i < 11; i++) {
        ASSERT_EQ_LL(shr__lyr_group_set(l, 0, &base, 1), SHR_OK);
        ctx->staged.rects.len = 0;
        ASSERT_EQ_LL(shr__lyr_group_set(l, 0, &v[i], 1), SHR_OK);
        ASSERT_EQ_LL(ctx->staged.rects.len > 0, i < 10);
    }
    ASSERT_EQ_LL(shr_lyr_set_visible(l, false), SHR_OK);
    ctx->staged.rects.len = 0;
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, &v[5], 1), SHR_OK);
    ASSERT_EQ_LL(ctx->staged.rects.len, 0); /* hidden layers record no damage */
    destroy_layers(&l, 1);
    r.res.dead = r2.res.dead = true;
    harness_close(&h);
    PASS();
}

TEST test_groups_draw_in_id_order(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr__lcmd red = fill(FULL, RED), green = fill(FULL, GREEN), blue = fill((shr_rect){0, 0, 4, 4}, BLUE);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 5, &red, 1), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 2, &green, 1), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 9, NULL, 0), SHR_OK); /* removing a missing group */
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), RED);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 3, &blue, 1), SHR_OK); /* between 2 and 5 */
    ASSERT_EQ_LL(shr__lyr_group_set(l, 5, NULL, 0), SHR_OK);
    frame(ctx);
    ASSERT(px(h.out.shown, 0, 0) == BLUE && px(h.out.shown, 10, 10) == GREEN);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 3, NULL, 0), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), GREEN);
    ASSERT_EQ_LL(shr__lyr_groups_clear(l), SHR_OK);
    frame(ctx);
    ASSERT(rec.damaged == HW * HH && count_color(&h, 0) == HW * HH);
    for (uint32_t id = 40; id > 10; id -= 3) ASSERT_EQ_LL(shr__lyr_group_set(l, id, &blue, 1), SHR_OK);
    for (uint32_t id = 11; id < 60; id += 2) ASSERT_EQ_LL(shr__lyr_group_set(l, id, &blue, 1), SHR_OK);
    ASSERT_EQ_LL(l->groups.len, 30);
    for (size_t i = 1; i < l->groups.len; i++)
        ASSERT(SHR_VEC_AT(&l->groups, shr__group, i - 1)->id < SHR_VEC_AT(&l->groups, shr__group, i)->id);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

TEST test_command_validation(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr__lcmd begin = {.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, 4, 4}}, end = {.kind = SHR__LCMD_CACHE_END};
    shr__lcmd flagged = fill(FULL, 0), no_res = {.kind = SHR__LCMD_GLYPH, .dst = {0, 0, 1, 1}};
    shr__lcmd no_img = no_res;
    flagged.flags = 1u << 3, no_img.kind = SHR__LCMD_IMAGE;
    const struct {
        shr__lcmd c[3];
        size_t n;
    } bad[] = {
        {{fill((shr_rect){1, 0, 0, 0}, 0)}, 1},
        {{flagged}, 1},
        {{no_res}, 1},
        {{no_img}, 1},
        {{end}, 1},
        {{begin}, 1},
        {{begin, begin, end}, 3},
        {{begin, end, end}, 3},
        {{{.kind = 0}}, 1},
        {{{.kind = SHR__LCMD_CACHE_END + 1}}, 1},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        ASSERT_EQ_LL(shr__lyr_group_set(l, 0, bad[i].c, bad[i].n), SHR_E_INVALID_ARG);
    const shr__lcmd good[4] = {begin, fill((shr_rect){0, 0, 4, 4}, RED), end, fill(FULL, 0)};
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, good, 4), SHR_OK);
    flagged.flags = SHR__LCMD_DIM | SHR__LCMD_BOLD | SHR__LCMD_ITALIC | SHR__LCMD_BLINK;
    ASSERT_EQ_LL(shr__lyr_group_set(l, 1, &flagged, 1), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_cmd_add(l, &begin), SHR_E_INVALID_ARG); /* one command is never a cache pair */
    ASSERT_EQ_LL(shr__lyr_cmd_add(l, &no_res), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

static int plugin_destroyed;
static void plugin_destroy(void *state) { *(int *)state += 1; }

TEST test_plugin_owned_layers(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    static const char kind_a = 0, kind_b = 0;
    shr_lyr *l[3];
    for (int i = 0; i < 3; i++) ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l[i]), SHR_OK);
    plugin_destroyed = 0;
    ASSERT_EQ_LL(shr__lyr_attach(l[0], &kind_a, &plugin_destroyed, plugin_destroy, NULL), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_attach(l[0], &kind_b, NULL, NULL, NULL), SHR_E_STATE); /* one plugin per layer */
    ASSERT(shr__lyr_state(l[0], &kind_a) == &plugin_destroyed && !shr__lyr_state(l[0], &kind_b));
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l[0]), SHR_E_STATE);
    shr__lcmd c = fill(FULL, RED);
    ASSERT_EQ_LL(shr__lyr_group_set(l[0], 0, &c, 1), SHR_OK); /* the plugin's own commands */
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l[1]), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_attach(l[1], &kind_a, NULL, NULL, NULL), SHR_E_STATE); /* building */
    ASSERT_EQ_LL(shr_lyr_cmd_fill(l[1], FULL, RED), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l[1]), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_attach(l[1], &kind_a, NULL, NULL, NULL), SHR_E_STATE); /* has commands */
    ASSERT_EQ_LL(shr__lyr_attach(l[2], &kind_b, NULL, NULL, NULL), SHR_OK);
    destroy_layers(l, 3);
    ASSERT_EQ_LL(plugin_destroyed, 1);
    harness_close(&h);
    PASS();
}

static int flush_calls, flush_fail;
static shr_status plugin_flush(void *state) {
    flush_calls++;
    if (flush_fail-- > 0) return flush_fail ? SHR_E_LIMIT : SHR_E_NO_MEMORY;
    shr__lcmd c = fill(FULL, GREEN);
    return state ? shr__lyr_group_set(state, 0, &c, 1) : SHR_OK;
}

/* Every layer is flushed and what was flushed is submitted; the first error is returned. */
TEST test_submit_flushes_plugin_layers(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    static const char kind = 0;
    shr_lyr *l[2];
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l[0]), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_create(ctx, 1, (shr_rect){0, 0, 8, 8}, &l[1]), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_attach(l[0], &kind, NULL, NULL, plugin_flush), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_attach(l[1], &kind, l[1], NULL, plugin_flush), SHR_OK);
    flush_calls = 0, flush_fail = 1;
    ASSERT_EQ_LL(shr_submit(ctx), SHR_E_NO_MEMORY);
    ASSERT_EQ_LL(flush_calls, 2);
    shr_pump(ctx);
    ASSERT(h.out.presents == 1 && px(h.out.shown, 0, 0) == GREEN && px(h.out.shown, 8, 8) == 0);
    flush_fail = 2;
    ASSERT_EQ_LL(shr_submit(ctx), SHR_E_LIMIT); /* the first of two errors */
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(flush_calls, 6);
    destroy_layers(l, 2);
    harness_close(&h);
    PASS();
}

TEST test_extreme_coordinates_clamp(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    shr_lyr *l[2];
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){INT32_MAX - 8, 0, INT32_MAX, HH}, &l[0]), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){INT32_MIN, 0, INT32_MIN + 8, HH}, &l[1]), SHR_OK);
    const shr_rect far[2] = {{0, 0, INT32_MAX, HH}, {INT32_MIN, 0, 8, HH}};
    for (int i = 0; i < 2; i++) {
        ASSERT_EQ_LL(shr_lyr_cmd_begin(l[i]), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_fill(l[i], far[i], RED), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_cmd_commit(l[i]), SHR_OK);
    }
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    settle(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), 0); /* both layers lie off screen */
    destroy_layers(l, 2);
    harness_close(&h);
    PASS();
}

TEST test_resource_users_counted(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd g = glyph(&r, 0, 0, WHITE), img = g, f = fill(FULL, 0);
    img.kind = SHR__LCMD_IMAGE;
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_cmd_add(l, &g), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_cmd_add(l, &img), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_cmd_add(l, &f), SHR_OK);
    ASSERT_EQ_LL(r.res.users, 2);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK);
    ASSERT_EQ_LL(r.res.users, 2);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_cmd_add(l, &g), SHR_OK);
    ASSERT_EQ_LL(r.res.users, 3);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK); /* drops the pending command */
    ASSERT_EQ_LL(r.res.users, 2);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(l), SHR_OK); /* an empty list */
    ASSERT_EQ_LL(r.res.users, 0);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 1, &g, 1), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 1, &g, 1), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 2, &img, 1), SHR_OK);
    ASSERT_EQ_LL(r.res.users, 2);
    ASSERT_EQ_LL(shr__lyr_groups_clear(l), SHR_OK);
    ASSERT_EQ_LL(r.res.users, 0);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 1, &g, 1), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_cmd_add(l, &g), SHR_OK);
    ASSERT_EQ_LL(r.res.users, 2);
    destroy_layers(&l, 1); /* pending and retained */
    ASSERT_EQ_LL(r.res.users, 0);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

static bool cache_hint_sent(void) {
    for (size_t i = 0; i < rec.n; i++)
        if (rec.cmds[i].kind == SHR_CMD_CACHE_BEGIN) return true;
    return false;
}

static bool rect_eq(shr_rect a, shr_rect b) { return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1; }

static bool fill_sent(shr_color color, int32_t x, int32_t y) {
    for (size_t i = 0; i < rec.n; i++) {
        const shr_draw_cmd *c = &rec.cmds[i];
        if (c->kind == SHR_CMD_FILL && c->color == color && x >= c->dst.x0 && x < c->dst.x1 && y >= c->dst.y0 &&
            y < c->dst.y1)
            return true;
    }
    return false;
}

/* A damaged area an upper layer surely hides is drawn from that layer up: no clear, nothing below. Fills that
 * are dim or blink, invisible layers and gaps between groups hide nothing; opaque parts end at the layer. */
TEST test_hidden_layers_skipped(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    const shr_color black = SHR_RGB(0, 0, 0);
    shr_lyr *below = solid(ctx, 0, FULL, RED), *top;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 1, (shr_rect){0, 0, 32, 32}, &top), SHR_OK);
    shr__lcmd rows[2] = {fill((shr_rect){0, 0, 32, 16}, BLUE), fill((shr_rect){0, 16, 32, 32}, GREEN)};
    ASSERT_EQ_LL(shr__lyr_group_set(top, 0, &rows[0], 1), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_group_set(top, 1, &rows[1], 1), SHR_OK);
    frame(ctx);
    ASSERT(!fill_sent(black, 40, 40)); /* the full fill below hides the clear */
    rows[1].color = RED;
    ASSERT_EQ_LL(shr__lyr_group_set(top, 1, &rows[1], 1), SHR_OK);
    frame(ctx); /* inside the two rows: drawn from the top layer only */
    ASSERT(rec.n == 1 && rec.cmds[0].color == RED && px(h.out.shown, 4, 20) == RED);
    shr__lcmd under = fill(FULL, WHITE);
    paint(below, 1, &under);
    frame(ctx); /* one damaged rect over the whole screen: the top layer hides only part of it */
    ASSERT(px(h.out.shown, 4, 4) == BLUE && px(h.out.shown, 40, 40) == WHITE);

    static const struct {
        uint32_t flags;
        bool visible, gap;
    } cases[] = {{SHR__LCMD_DIM, true, false}, {SHR__LCMD_BLINK, true, false}, {0, false, false}, {0, true, true}};
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        shr__lcmd a = fill((shr_rect){0, 0, 32, 16}, BLUE), b = fill((shr_rect){0, cases[k].gap ? 20 : 16, 32, 32}, GREEN);
        a.flags = b.flags = cases[k].flags;
        ASSERT_EQ_LL(shr__lyr_group_set(top, 0, &a, 1), SHR_OK);
        ASSERT_EQ_LL(shr__lyr_group_set(top, 1, &b, 1), SHR_OK);
        ASSERT_EQ_LL(shr_lyr_set_visible(top, cases[k].visible), SHR_OK);
        frame(ctx);
        shr__lcmd inside[2] = {under, fill((shr_rect){2, 2, 30, 30}, k % 2 ? RED : WHITE)};
        paint(below, 2, inside);
        frame(ctx);
        inside[1].color = k % 2 ? WHITE : RED; /* damage within the top layer's rows */
        paint(below, 2, inside);
        frame(ctx);
        ASSERT(fill_sent(inside[1].color, 4, 17));
        if (!cases[k].visible || cases[k].gap) ASSERT_EQ_LL(px(h.out.shown, 4, 17), inside[1].color);
    }
    ASSERT_EQ_LL(shr_lyr_set_visible(top, true), SHR_OK);

    shr__lcmd wide = fill((shr_rect){-8, 0, 64, 32}, BLUE); /* opaque only inside the layer */
    ASSERT_EQ_LL(shr__lyr_groups_clear(top), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_group_set(top, 0, &wide, 1), SHR_OK);
    shr__lcmd spot[2] = {fill(FULL, RED), fill((shr_rect){40, 4, 48, 12}, RED)};
    paint(below, 2, spot);
    frame(ctx);
    spot[1].color = GREEN; /* beside the layer, inside its fill */
    paint(below, 2, spot);
    frame(ctx);
    ASSERT(fill_sent(GREEN, 44, 8) && px(h.out.shown, 44, 8) == GREEN && px(h.out.shown, 4, 4) == BLUE);
    destroy_layers(&top, 1);
    destroy_layers(&below, 1);
    harness_close(&h);
    PASS();
}

/* A frame skips the blocks of a large group that its damage does not reach; what it draws is unchanged. */
TEST test_large_group_blocks_skipped(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd cells[HW / 4 * (HH / 4)]; /* 4x4 cells, row by row: several blocks */
    size_t n = 0;
    for (int32_t y = 0; y < HH; y += 4)
        for (int32_t x = 0; x < HW; x += 4) cells[n++] = fill((shr_rect){x, y, x + 4, y + 4}, (x / 4 + y / 4) % 2 ? RED : BLUE);
    paint(l, n, cells);
    frame(ctx);
    cells[64].color = GREEN; /* the first command of the second block (x 0, y 16) */
    paint(l, n, cells);
    frame(ctx);
    ASSERT(rec.n == 2 && rec.cmds[1].color == GREEN && px(h.out.shown, 0, 16) == GREEN); /* clear, cell */
    ASSERT(px(h.out.shown, 0, 0) == BLUE && px(h.out.shown, 4, 0) == RED);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* A hint covers a whole group drawn with final pixels; cache_clip says which part is written. */
TEST test_cache_hints(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_blink);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){8, 0, 40, 16}, &l), SHR_OK);
    shr__lcmd cached[4] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, 32, 16}, .key = {7}},
                           fill((shr_rect){0, 0, 32, 16}, BLUE), glyph(&r, 8, 0, WHITE), {.kind = SHR__LCMD_CACHE_END}};
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, cached, 4), SHR_OK);
    frame(ctx);
    const shr_rect group = {8, 0, 40, 16};
    ASSERT_EQ_LL(rec.n, 5);
    ASSERT(rec.cmds[1].kind == SHR_CMD_CACHE_BEGIN && rec.cmds[1].key[0] == 7 && rect_eq(rec.cmds[1].dst, group));
    ASSERT(rect_eq(rec.cmds[1].cache_clip, group) && rec.cmds[4].kind == SHR_CMD_CACHE_END);
    ASSERT(rect_eq(rec.cmds[3].dst, (shr_rect){16, 0, 24, 16}));

    memset(h.out.bufs[0] + (5 * HW + 30) * SCREEN_BPP, 0x5A, SCREEN_BPP);
    uint32_t canary = px(h.out.bufs[0], 30, 5);
    shr_lyr *m;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 1, (shr_rect){10, 0, 12, 2}, &m), SHR_OK);
    shr__lcmd half = fill((shr_rect){0, 0, 2, 2}, RED);
    half.flags = SHR__LCMD_DIM; /* not opaque: the group below still draws */
    paint(m, 1, &half);
    frame(ctx); /* the group is drawn whole, only the damaged part is written */
    ASSERT_EQ_LL(rec.n, 5); /* no clear: the cached group hides the damage */
    ASSERT(rec.cmds[0].kind == SHR_CMD_CACHE_BEGIN && rect_eq(rec.cmds[0].cache_clip, (shr_rect){10, 0, 12, 2}));
    ASSERT(rect_eq(rec.cmds[1].dst, group) && rec.cmds[3].kind == SHR_CMD_CACHE_END);
    uint32_t mixed = px(h.out.shown, 10, 0);
    ASSERT(px(h.out.shown, 30, 5) == canary && mixed != RED && mixed != BLUE && px(h.out.shown, 9, 0) == BLUE);

    r.px.provisional = true; /* fallback pixels are not cached */
    shr_request_redraw(ctx);
    shr_pump(ctx);
    ASSERT(!cache_hint_sent() && rec.n == 4); /* clear, fill, glyph, the other layer */
    r.px.provisional = false;
    shr_request_redraw(ctx);
    shr_pump(ctx);
    ASSERT(cache_hint_sent());

    cached[2].flags = SHR__LCMD_BLINK; /* nor is content the blink phase hides */
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, cached, 4), SHR_OK);
    frame(ctx);
    ASSERT(cache_hint_sent());
    fake_now = 150 * MS;
    shr_pump(ctx);
    ASSERT(!cache_hint_sent() && rec.n == 1 && px(h.out.shown, 16, 8) == BLUE); /* the layer hides the clear */

    const shr_rect moved[2] = {{40, 0, 72, 16}, {8, 0, 24, 16}}; /* partly off the screen, or out of its layer */
    for (int i = 0; i < 2; i++) {
        int frames = rec.frames;
        ASSERT_EQ_LL(shr_lyr_set_rect(l, moved[i]), SHR_OK);
        frame(ctx);
        ASSERT(rec.frames == frames + 1 && !cache_hint_sent());
    }

    /* A hint outside the damage is left out with its end. */
    ASSERT_EQ_LL(shr_lyr_set_rect(l, (shr_rect){40, 20, 64, 48}), SHR_OK);
    shr__lcmd far[4] = {cached[0], fill((shr_rect){0, 0, 4, 4}, BLUE), cached[3], fill((shr_rect){10, 10, 14, 14}, RED)};
    far[0].dst = (shr_rect){0, 0, 4, 4};
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, far, 4), SHR_OK);
    frame(ctx);
    far[3].color = GREEN;
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, far, 4), SHR_OK);
    frame(ctx);
    ASSERT(rec.n == 2 && rec.cmds[1].kind == SHR_CMD_FILL && rec.cmds[1].color == GREEN);
    destroy_layers(&l, 1);
    destroy_layers(&m, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

TEST test_resource_resolution(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    fake r, im, off;
    fake_attach(&r, ctx, &fk_ops);
    fake_attach(&im, ctx, &fk_ops);
    fake_attach(&off, ctx, &bare_ops);
    im.px.image = (shr_image){im.cov, 8, 16, 32, sizeof(im.cov), SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU};
    r.px.offset = (shr_point){-2, -1};
    off.px.offset = (shr_point){100, 0}; /* its pixels miss its cell */
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){4, 4, 40, 40}, &l), SHR_OK);
    shr__lcmd c[5] = {glyph(&r, 2, 3, RED), glyph(&r, 20, 3, RED), glyph(&im, 0, 20, 0), glyph(&r, 12, 3, RED),
                      glyph(&off, 24, 20, RED)};
    c[0].flags = SHR__LCMD_DIM;
    c[1].id = 0; /* not found: draws nothing */
    c[2].kind = SHR__LCMD_IMAGE, c[2].flags = SHR__LCMD_DIM;
    paint(l, 5, c);
    frame(ctx);
    ASSERT_EQ_LL(rec.n, 4);
    const shr_draw_cmd *g = &rec.cmds[1];
    ASSERT(g->kind == SHR_CMD_GLYPH && g->flags == SHR_GLYPH_DIM && g->color == RED);
    ASSERT(rect_eq(g->dst, (shr_rect){6, 7, 12, 22}) && g->src_origin.x == 2 && g->src_origin.y == 1);
    ASSERT(rec.cmds[2].kind == SHR_CMD_IMAGE && rec.cmds[2].flags == 0);
    ASSERT(rec.cmds[3].kind == SHR_CMD_GLYPH && rec.cmds[3].flags == 0);
    ASSERT(r.resolves == 3 && r.ends == 1 && im.ends == 1 && off.resolves == 1); /* frame_end once per frame */
    ASSERT_EQ_LL(px(h.out.shown, 4, 24), WHITE);
    im.st = SHR_E_IO;
    shr_request_redraw(ctx);
    shr_pump(ctx);
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_PRESENT_FAILED, NULL), 1);
    ASSERT_EQ_LL(im.ends, 1); /* no pin was taken */
    destroy_layers(&l, 1);
    r.res.dead = im.res.dead = off.res.dead = true;
    harness_close(&h);
    PASS();
}

/* BOLD and ITALIC reach the driver only where the resource allows them; a styled glyph covers its footprint,
 * still cut at its cell. */
TEST test_glyph_styles(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    fake r, plain, im, up;
    fake_attach(&r, ctx, &fk_ops);
    fake_attach(&plain, ctx, &fk_ops);
    fake_attach(&im, ctx, &fk_ops);
    fake_attach(&up, ctx, &fk_ops);
    r.px.offset = (shr_point){2, 0};
    up.px.offset = (shr_point){2, -3}; /* sticks out above the cell */
    r.px.synth = im.px.synth = up.px.synth = SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC;
    r.px.slant_axis = up.px.slant_axis = 16; /* rows 0 and 15 shift by 1 + 149/256 and -2 + 107/256: columns -2 to 11 */
    plain.px.slant_axis = 7;
    im.px.image = (shr_image){im.cov, 8, 16, 32, sizeof(im.cov), SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU};
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){4, 4, 60, 40}, &l), SHR_OK);
    const uint32_t bi = SHR__LCMD_BOLD | SHR__LCMD_ITALIC;
    shr__lcmd c[7] = {glyph(&r, 0, 0, RED),  glyph(&r, 16, 0, RED),   glyph(&r, 40, 0, RED),  glyph(&plain, 0, 18, RED),
                      glyph(&im, 16, 18, 0), glyph(&up, 40, 18, RED), glyph(&up, 48, 18, RED)};
    c[0].flags = bi | SHR__LCMD_DIM;
    c[1].flags = bi, c[1].dst.x1 += 16; /* two cells: the whole footprint */
    c[2].flags = SHR__LCMD_BOLD, c[2].dst.x1 += 8;
    c[3].flags = bi | SHR__LCMD_DIM; /* the resource draws no styles */
    c[4].kind = SHR__LCMD_IMAGE, c[4].flags = bi;
    c[5].flags = bi, c[6].flags = SHR__LCMD_BOLD;
    paint(l, 7, c);
    frame(ctx);
    ASSERT_EQ_LL(rec.n, 8);
    /* The axis only with ITALIC, so equal draws are equal commands. */
    const struct {
        uint32_t flags;
        shr_rect dst;
        int32_t sx, sy, axis;
    } want[7] = {{SHR_GLYPH_DIM | SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC, {4, 4, 12, 20}, -2, 0, 16},
                 {SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC, {20, 4, 33, 20}, -2, 0, 16},
                 {SHR_GLYPH_BOLD, {46, 4, 55, 20}, 0, 0, 0},
                 {SHR_GLYPH_DIM, {4, 22, 12, 38}, 0, 0, 0},
                 {0, {20, 22, 28, 38}, 0, 0, 0},
                 {SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC, {44, 22, 52, 35}, -2, 3, 16},
                 {SHR_GLYPH_BOLD, {54, 22, 60, 35}, 0, 3, 0}};
    for (int i = 0; i < 7; i++) {
        const shr_draw_cmd *g = &rec.cmds[i + 1];
        ASSERT_EQ_LL(g->kind, i == 4 ? SHR_CMD_IMAGE : SHR_CMD_GLYPH);
        ASSERT_EQ_LL(g->flags, want[i].flags);
        ASSERT(rect_eq(g->dst, want[i].dst));
        ASSERT(g->src_origin.x == want[i].sx && g->src_origin.y == want[i].sy);
        ASSERT_EQ_LL(g->slant_axis, want[i].axis);
    }
    shr_pl_res_image *img; /* an image resource draws no styles */
    static const uint32_t rgba = 0xFFFFFFFFu;
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 1, 1, &rgba, 4, &img), SHR_OK);
    shr__res *ir = (shr__res *)img;
    shr__resolved out;
    memset(&out, 0xFF, sizeof(out));
    ASSERT_EQ_LL(ir->ops->resolve(ir, 0, 1, &out), SHR_OK);
    ASSERT(out.synth == 0 && out.slant_axis == 0);
    ir->ops->frame_end(ir, 1);
    ASSERT_EQ_LL(shr_pl_res_image_release(img), SHR_OK);
    destroy_layers(&l, 1);
    r.res.dead = plain.res.dead = im.res.dead = up.res.dead = true;
    harness_close(&h);
    PASS();
}

TEST test_provisional_pixels_redrawn(void) {
    for (int mode = 0; mode < 3; mode++) {
        harness h;
        shr_context *ctx = harness_open(&h, mode ? PRESERVED : SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
        if (mode == 2) ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
        fake r;
        fake_attach(&r, ctx, &fk_ops);
        r.px.provisional = true;
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        shr__lcmd g = glyph(&r, 0, 0, WHITE);
        paint(l, 1, &g);
        frame(ctx);
        shr_pump(ctx);
        ASSERT_EQ_LL(h.out.presents, 1);
        r.changed = true, r.px.provisional = false;
        shr_pump(ctx);
        ASSERT_EQ_LL(h.out.presents, 2);
        if (mode) ASSERT_EQ_LL(rec.damaged, 8 * 16);
        r.changed = true; /* nothing provisional any more */
        shr_pump(ctx);
        ASSERT_EQ_LL(h.out.presents, 2);
        destroy_layers(&l, 1);
        r.res.dead = true;
        harness_close(&h);
    }
    PASS();
}

TEST test_provisional_change_during_raster(void) {
    for (int preserved = 0; preserved < 2; preserved++) {
        harness h;
        shr_context *ctx = harness_open(&h, preserved ? PRESERVED : SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
        fake r;
        fake_attach(&r, ctx, &fk_ops);
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        shr__lcmd g = glyph(&r, 0, 0, WHITE);
        paint(l, 1, &g);
        h.drv.async = true;
        frame(ctx);
        r.changed = true; /* nothing in flight is provisional */
        shr_pump(ctx);
        md_complete(&h.drv);
        shr_pump(ctx);
        shr_pump(ctx);
        ASSERT_EQ_LL(h.out.presents, 1);

        r.px.provisional = true;
        shr_request_redraw(ctx);
        shr_pump(ctx);
        r.changed = true, r.px.provisional = false;
        shr_pump(ctx); /* the frame in flight drew the old fallback */
        h.drv.async = false;
        md_complete(&h.drv);
        shr_pump(ctx);
        ASSERT_EQ_LL(h.out.presents, 3);
        ASSERT_EQ_LL(rec.damaged, preserved ? 8 * 16 : HW * HH);
        shr_pump(ctx);
        ASSERT_EQ_LL(h.out.presents, 3);

        /* The target was invalidated while the frame was drawn: it is redrawn whole anyway. */
        r.px.provisional = true;
        h.drv.async = true;
        shr_request_redraw(ctx);
        shr_pump(ctx);
        shr_request_redraw(ctx);
        h.drv.async = false;
        md_complete(&h.drv);
        shr_pump(ctx);
        ASSERT_EQ_LL(h.out.presents, 5);
        destroy_layers(&l, 1);
        r.res.dead = true;
        harness_close(&h);
    }
    PASS();
}

/* Fallback areas are redrawn with the next change; an adjacent change merges with them. */
TEST test_fallback_areas_redrawn_with_changes(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    r.px.provisional = true;
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd c[2] = {glyph(&r, 0, 0, WHITE), fill((shr_rect){8, 0, 16, 16}, RED)};
    paint(l, 1, c);
    frame(ctx);
    paint(l, 2, c);
    frame(ctx);
    ASSERT(rec.damaged == 256 && rec.commands == 3); /* one rectangle: clear, glyph, fill */
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

TEST test_composed_frame_waits_for_output(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak_one_frame);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, SHR_SCREEN_COMPOSITION, NULL), SHR_OK);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    frame(ctx);
    shr__lcmd c = fill(FULL, GREEN);
    paint(l, 1, &c);
    frame(ctx); /* composed, but the one output frame allowed is not released */
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    drain(ctx);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK); /* a newer state replaces the waiting frame */
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, h.out.last_frame + 1);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    ASSERT_EQ_LL(shr_output_released(ctx, h.out.last_frame), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(h.out.presents == 2 && px(h.out.shown, 0, 0) == GREEN);
    shr_output_released(ctx, h.out.last_frame);

    h.drv.async = true;
    c.color = BLUE;
    paint(l, 1, &c);
    frame(ctx);
    shr_output_error(ctx, SHR_E_DEVICE);
    md_complete(&h.drv);
    shr_pump(ctx); /* composed; the conversion waits for recovery */
    ASSERT_EQ_LL(h.out.presents, 2);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    h.drv.async = false;
    shr_output_recover(ctx);
    shr_pump(ctx);
    ASSERT(h.out.presents == 3 && px(h.out.shown, 0, 0) == BLUE);
    shr_output_released(ctx, h.out.last_frame);
    drain(ctx);

    h.out.present_result = SHR_E_WOULD_BLOCK;
    c.color = RED;
    paint(l, 1, &c);
    frame(ctx);
    c.color = GREEN;
    paint(l, 1, &c);
    frame(ctx); /* supersedes the waiting frame; the next conversion waits for the output */
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, h.out.last_frame + 1);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    h.out.present_result = SHR_OK;
    shr_output_ready(ctx);
    shr_pump(ctx);
    ASSERT(h.out.presents == 4 && px(h.out.shown, 0, 0) == GREEN);
    shr_output_released(ctx, h.out.last_frame);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

TEST test_frames_wait_for_screen(void) {
    harness h;
    shr_output out;
    shr_context_desc d;
    harness_desc(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, &out, &d);
    shr_context *ctx;
    ASSERT_EQ_LL(shr_create(&d, &ctx), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, 0);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, NULL), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE); /* no frames once shutting down */
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);
    PASS();
}

static int chain_left;
static src_log chain_log;
static shr_asset_source chain_src;
static void chain_io_done(shr__res *r, uint64_t tag, shr_status st) {
    fk_io(r, tag, st);
    if (chain_left-- > 0) shr__ctx_read(r->ctx, r, &chain_src, 0, 0, NULL, tag + 1);
}

/* Completions that start further reads are delivered for a bounded number of rounds per pump. */
TEST test_pump_bounds_completion_rounds(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    shr__res_ops ops = fk_ops;
    ops.io_done = chain_io_done;
    fake r;
    fake_attach(&r, ctx, &ops);
    chain_log = (src_log){SHR_OK, 0, 0, 0, 0};
    chain_src = source(&chain_log, false);
    chain_left = 100;
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &chain_src, 0, 0, NULL, 0), SHR_OK);
    shr_pump(ctx);
    ASSERT_EQ_LL(r.io_n, 64);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(r.io_n == 101 && r.io_tag == 100);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

TEST test_resource_change_damages_every_buffer(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    fake r, other;
    fake_attach(&r, ctx, &fk_ops);
    fake_attach(&other, ctx, &fk_ops);
    shr_lyr *l[3];
    for (int i = 0; i < 3; i++) ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l[i]), SHR_OK);
    shr__lcmd c[3] = {glyph(&r, 0, 0, WHITE), glyph(&other, 20, 0, WHITE), fill((shr_rect){40, 0, 48, 8}, RED)};
    paint(l[0], 3, c);
    c[0] = glyph(&r, 20, 20, WHITE);
    paint(l[1], 1, c);
    ASSERT_EQ_LL(shr_lyr_set_visible(l[1], false), SHR_OK);
    frame(ctx);
    h.out.busy[0] = true;
    frame(ctx);
    h.out.busy[0] = false;
    shr__res_changed(&r.res, (shr_rect){0, 0, 2, 2});
    frame(ctx);
    ASSERT(h.out.last_buf == 0 && rec.damaged == 4);
    h.out.busy[0] = true;
    frame(ctx);
    ASSERT(h.out.last_buf == 1 && rec.damaged == 4);
    h.out.busy[0] = false;
    int frames = rec.frames;
    shr__res_changed(&r.res, (shr_rect){100, 100, 110, 110}); /* outside every command */
    frame(ctx);
    ASSERT_EQ_LL(rec.frames, frames);
    destroy_layers(l, 3);
    r.res.dead = other.res.dead = true;
    harness_close(&h);
    PASS();
}

TEST test_dead_resources_freed_when_unused(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd g = glyph(&r, 0, 0, WHITE);
    paint(l, 1, &g);
    r.res.dead = true;
    shr_pump(ctx);
    ASSERT_EQ_LL(r.frees, 0); /* a command refers to it */
    h.drv.async = true;
    frame(ctx);
    destroy_layers(&l, 1);
    fake q;
    fake_attach(&q, ctx, &fk_ops);
    q.res.dead = true;
    shr_pump(ctx);
    ASSERT(r.frees == 0 && q.frees == 1); /* the frame in flight reads only r */
    md_complete(&h.drv);
    shr_pump(ctx);
    ASSERT(r.frees == 1 && r.ends == 1);
    harness_close(&h);
    PASS();
}

TEST test_resource_attach_arguments(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    fake r;
    const shr__res_ops no_resolve = {.free = fk_free}, no_free = {.resolve = fk_resolve};
    ASSERT_EQ_LL(shr__res_attach(NULL, &r.res, &fk_ops), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__res_attach(ctx, NULL, &fk_ops), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__res_attach(ctx, &r.res, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__res_attach(ctx, &r.res, &no_resolve), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__res_attach(ctx, &r.res, &no_free), SHR_E_INVALID_ARG);
    fake_attach(&r, ctx, &bare_ops);
    ASSERT(r.res.ctx == ctx && !r.res.users && !r.res.dead);
    r.res.dead = true;
    harness_close(&h);
    ASSERT_EQ_LL(r.frees, 1);
    PASS();
}

/* ===== asset reads ===== */

TEST test_asset_read_arguments(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak_rec);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    src_log sl = {SHR_IN_PROGRESS, 0, 0, 0, 0};
    shr_asset_source src = source(&sl, true), no_read = src, mapped = src;
    no_read.read = NULL, mapped.data = &sl;
    uint8_t buf[8];
    ASSERT_EQ_LL(shr__ctx_read(NULL, &r.res, &src, 0, 4, buf, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__ctx_read(ctx, NULL, &src, 0, 4, buf, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, NULL, 0, 4, buf, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &no_read, 0, 4, buf, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &mapped, 0, 4, buf, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 4, NULL, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(sl.reads, 0);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 0, NULL, 9), SHR_OK);
    ASSERT_EQ_LL(shr_asset_complete(NULL, sl.req, SHR_OK), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_asset_complete(ctx, 0xFFFF, SHR_OK), SHR_E_NOT_FOUND);         /* no such slot */
    ASSERT_EQ_LL(shr_asset_complete(ctx, sl.req + (1u << 16), SHR_OK), SHR_E_NOT_FOUND); /* other generation */
    ASSERT_EQ_LL(shr_asset_complete(ctx, sl.req, SHR_OK), SHR_OK);
    ASSERT_EQ_LL(shr_asset_complete(ctx, sl.req, SHR_OK), SHR_E_NOT_FOUND); /* once */
    ASSERT_EQ_LL(r.io_n, 0);                                                 /* delivered by pump() */
    shr_pump(ctx);
    ASSERT(r.io_n == 1 && r.io_tag == 9 && r.io_st == SHR_OK);
    ASSERT(rec.kinds[SHR_TRACE_IO_BEGIN] == 1 && rec.kinds[SHR_TRACE_IO_END] == 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

static void tweak_two_reads(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->max_reads = 2;
    d->io_timeout_ns = 0;
}

TEST test_asset_read_results(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak_two_reads);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    src_log sl = {0};
    shr_asset_source src = source(&sl, true);
    uint8_t buf[4] = {0};
    const shr_status sync[3][2] = {{SHR_OK, SHR_OK}, {SHR_E_CHECKSUM, SHR_E_CHECKSUM}, {(shr_status)99, SHR_E_IO}};
    for (int i = 0; i < 3; i++) {
        sl.ret = sync[i][0];
        ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 7, 4, buf, (uint64_t)i), SHR_OK);
        shr_pump(ctx);
        ASSERT(r.io_n == i + 1 && r.io_tag == (uint64_t)i && r.io_st == sync[i][1] && buf[3] == 7);
    }
    sl.ret = SHR_E_WOULD_BLOCK; /* the source's queue is full: nothing is outstanding */
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 4, buf, 0), SHR_E_WOULD_BLOCK);
    shr_pump(ctx);
    ASSERT_EQ_LL(r.io_n, 3);
    sl.ret = SHR_IN_PROGRESS;
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 4, buf, 0), SHR_OK);
    uint64_t first = sl.req;
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 4, buf, 0), SHR_OK);
    uint64_t second = sl.req;
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 4, buf, 0), SHR_E_LIMIT);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE); /* no watchdog configured */
    ASSERT_EQ_LL(shr_asset_complete(ctx, first, SHR_IN_PROGRESS), SHR_OK); /* not a final result */
    ASSERT_EQ_LL(shr_asset_complete(ctx, second, SHR_E_WOULD_BLOCK), SHR_OK);
    shr_pump(ctx);
    ASSERT(r.io_n == 5 && r.io_st == SHR_E_IO);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 4, buf, 0), SHR_OK);
    ASSERT(sl.req != first && sl.req != second);
    ASSERT_EQ_LL(shr_asset_complete(ctx, first, SHR_OK), SHR_E_NOT_FOUND); /* a stale id of a reused slot */
    ASSERT_EQ_LL(shr_asset_complete(ctx, sl.req, SHR_OK), SHR_OK);
    shr_pump(ctx);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

TEST test_asset_read_cancel(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    fake r, bare;
    fake_attach(&r, ctx, &fk_ops);
    fake_attach(&bare, ctx, &bare_ops);
    src_log la = {SHR_IN_PROGRESS, 0, 0, 0, 0}, lb = la, lc = la;
    shr_asset_source a = source(&la, true), b = source(&lb, false), c = source(&lc, true);
    uint8_t buf[12];
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &a, 0, 4, buf, 1), SHR_OK);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &b, 0, 4, buf + 4, 2), SHR_OK);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &bare.res, &c, 0, 4, buf + 8, 3), SHR_OK);
    ASSERT(!shr__ctx_read_cancel(ctx, &a));
    ASSERT(la.cancels == 1 && la.cancelled == la.req && lb.cancels == 0);
    ASSERT_EQ_LL(shr_asset_complete(ctx, la.req, SHR_OK), SHR_E_NOT_FOUND); /* the cancellation won */
    ASSERT(shr__ctx_read_cancel(ctx, &b)); /* no cancel(): it may still write */
    shr_pump(ctx);
    ASSERT(r.io_n == 1 && r.io_tag == 1 && r.io_st == SHR_E_IO);
    ASSERT_EQ_LL(shr_asset_complete(ctx, lb.req, SHR_E_CHECKSUM), SHR_OK);
    shr_pump(ctx);
    ASSERT(r.io_n == 2 && r.io_tag == 2 && r.io_st == SHR_E_CHECKSUM);
    ASSERT_EQ_LL(shr_asset_complete(ctx, lc.req, SHR_OK), SHR_OK);
    ASSERT(!shr__ctx_read_cancel(ctx, &c)); /* the completion won */
    ASSERT_EQ_LL(lc.cancels, 0);
    ASSERT(!shr__ctx_read_cancel(ctx, &a)); /* nothing outstanding */
    shr_pump(ctx);                           /* a resource without io_done() */
    r.res.dead = bare.res.dead = true;
    harness_close(&h);
    PASS();
}

static void tweak_read_timeout(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->io_timeout_ns = 5 * MS;
}

TEST test_asset_read_timeout(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak_read_timeout);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    src_log la = {SHR_IN_PROGRESS, 0, 0, 0, 0}, lb = la;
    shr_asset_source a = source(&la, true), b = source(&lb, false);
    uint8_t buf[8];
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &b, 0, 4, buf + 4, 2), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE); /* only cancellable reads have a watchdog */
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &a, 0, 4, buf, 9), SHR_OK);
    uint64_t early = la.req;
    fake_now = 1 * MS;
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &a, 0, 4, buf, 1), SHR_OK);
    ASSERT_EQ_LL(shr_asset_complete(ctx, early, SHR_OK), SHR_OK);
    shr_pump(ctx); /* frees the slot before the pending one */
    fake_now = 2 * MS;
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &a, 0, 4, buf, 7), SHR_OK);
    uint64_t late = la.req;
    fake_now = 3 * MS;
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &a, 0, 4, buf, 8), SHR_OK);
    ASSERT_EQ_LL(deadline_at(ctx), 6 * MS); /* the earliest of 7, 6 and 8 ms */
    ASSERT_EQ_LL(shr_asset_complete(ctx, late, SHR_OK), SHR_OK);
    ASSERT_EQ_LL(shr_asset_complete(ctx, la.req, SHR_OK), SHR_OK);
    shr_pump(ctx);
    r.io_n = 0;
    fake_now = 6 * MS;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(r.io_n == 1 && r.io_tag == 1 && r.io_st == SHR_E_TIMEOUT && la.cancels == 1);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    ASSERT_EQ_LL(shr_asset_complete(ctx, lb.req, SHR_OK), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(r.io_n == 2 && r.io_st == SHR_OK);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

TEST test_shutdown_cancels_reads(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    fake r, q;
    fake_attach(&r, ctx, &fk_ops);
    fake_attach(&q, ctx, &fk_ops);
    src_log la = {SHR_IN_PROGRESS, 0, 0, 0, 0}, lb = la, lc = la;
    shr_asset_source a = source(&la, true), b = source(&lb, false), c = source(&lc, false);
    uint8_t buf[12];
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &a, 0, 4, buf, 1), SHR_OK);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &b, 0, 4, buf + 4, 2), SHR_OK);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &q.res, &c, 0, 4, buf + 8, 3), SHR_OK);
    ASSERT_EQ_LL(shr_begin_shutdown(ctx), SHR_OK);
    ASSERT_EQ_LL(la.cancels, 1);
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &a, 0, 4, buf, 1), SHR_E_STATE);
    r.res.dead = q.res.dead = true;
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_E_WOULD_BLOCK);
    shr_pump(ctx);
    ASSERT(r.io_n == 1 && r.frees == 0); /* a read of it is still outstanding */
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_E_WOULD_BLOCK);
    ASSERT_EQ_LL(shr_asset_complete(ctx, lb.req, SHR_OK), SHR_OK);
    shr_pump(ctx);
    ASSERT(r.io_n == 2 && r.frees == 1 && q.frees == 0); /* the other resource still waits for its read */
    ASSERT_EQ_LL(shr_asset_complete(ctx, lc.req, SHR_OK), SHR_OK);
    shr_pump(ctx);
    ASSERT_EQ_LL(q.frees, 1);
    ASSERT_EQ_LL(shr_destroy(ctx), SHR_OK);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(test_init_functions);
    RUN_TEST(test_create_rejects_invalid_descriptors);
    RUN_TEST(test_create_out_of_memory);
    RUN_TEST(test_context_services);
    RUN_TEST(test_plugin_slots);
    RUN_TEST(test_screen_configure_validation);
    RUN_TEST(test_dma_only_composition);
    RUN_TEST(test_rotation_and_conversion);
    RUN_TEST(test_rotation_map_point);
    RUN_TEST(test_buffer_validation);
    RUN_TEST(test_shutdown_refuses_changes);
    RUN_TEST(test_destroy_waits_for_frames);
    RUN_TEST(test_host_callback_reentry);
    RUN_TEST(test_event_queue_reserves_room);
    RUN_TEST(test_shutdown_ends_every_frame);
    RUN_TEST(test_shutdown_stops_conversion);
    RUN_TEST(test_deadline_is_earliest_timer);
    RUN_TEST(test_fence_signal_rules);
    RUN_TEST(test_driver_timeout_with_reset);
    RUN_TEST(test_driver_timeout_isolates_buffers);
    RUN_TEST(test_composed_timeouts);
    RUN_TEST(test_output_error_and_recover);
    RUN_TEST(test_acquire_failures);
    RUN_TEST(test_present_failures);
    RUN_TEST(test_waiting_frame_superseded);
    RUN_TEST(test_configure_supersedes_waiting_frame);
    RUN_TEST(test_release_and_display);
    RUN_TEST(test_output_without_timestamps);
    RUN_TEST(test_preserved_damage_history);
    RUN_TEST(test_device_surface_identity);
    RUN_TEST(test_request_redraw);
    RUN_TEST(test_blink_phase);
    RUN_TEST(test_blink_restarts_on_submit);
    RUN_TEST(test_blink_epoch_in_future);
    RUN_TEST(test_blink_needs_clock);
    RUN_TEST(test_blink_outside_screen_draws_nothing);
    RUN_TEST(test_command_limit_fails_frame);
    RUN_TEST(test_driver_would_block);
    RUN_TEST(test_driver_ready_retries);
    RUN_TEST(test_driver_caps_checked);
    RUN_TEST(test_dma_sync);
    RUN_TEST(test_frame_out_of_memory);
    RUN_TEST(test_scattered_damage);
    RUN_TEST(test_damage_out_of_memory_redraws_all);
    RUN_TEST(test_async_frames_keep_presenting);
    RUN_TEST(test_damage_list_out_of_memory_forgets_target);
    RUN_TEST(test_automatic_frames_wait_for_submit);
    RUN_TEST(test_timers_saturate);
    RUN_TEST(test_configure_rejects_unreachable_composition);
    RUN_TEST(test_layer_arguments);
    RUN_TEST(test_layer_out_of_memory);
    RUN_TEST(test_layer_order);
    RUN_TEST(test_layer_rect_clips_and_moves);
    RUN_TEST(test_app_layer_diff);
    RUN_TEST(test_command_diff_compares_every_field);
    RUN_TEST(test_groups_draw_in_id_order);
    RUN_TEST(test_command_validation);
    RUN_TEST(test_plugin_owned_layers);
    RUN_TEST(test_resource_users_counted);
    RUN_TEST(test_hidden_layers_skipped);
    RUN_TEST(test_large_group_blocks_skipped);
    RUN_TEST(test_cache_hints);
    RUN_TEST(test_resource_resolution);
    RUN_TEST(test_glyph_styles);
    RUN_TEST(test_provisional_pixels_redrawn);
    RUN_TEST(test_provisional_change_during_raster);
    RUN_TEST(test_fallback_areas_redrawn_with_changes);
    RUN_TEST(test_composed_frame_waits_for_output);
    RUN_TEST(test_frames_wait_for_screen);
    RUN_TEST(test_pump_bounds_completion_rounds);
    RUN_TEST(test_resource_change_damages_every_buffer);
    RUN_TEST(test_dead_resources_freed_when_unused);
    RUN_TEST(test_resource_attach_arguments);
    RUN_TEST(test_asset_read_arguments);
    RUN_TEST(test_asset_read_results);
    RUN_TEST(test_asset_read_cancel);
    RUN_TEST(test_asset_read_timeout);
    RUN_TEST(test_shutdown_cancels_reads);
    RUN_TEST(test_submit_flushes_plugin_layers);
    RUN_TEST(test_extreme_coordinates_clamp);
    GREATEST_MAIN_END();
}
