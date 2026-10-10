#include "compositor.h"
#define HW (8 * SHR_CELL_WIDTH) /* row commands span whole cells: 8 columns at any cell size */
#define HH 48
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
    shr_draw_cmd cmds[512]; /* the draws, after the buffer prologue */
    size_t n;
    shr_draw_cmd pro[64];
    size_t npro;
    int frames, logs, kinds[16];
    uint64_t damaged, commands, short_bufs; /* of the last RASTER_BEGIN, RASTER_END */
    uint64_t submit_id;         /* of the last SUBMIT */
    uint64_t converted;         /* of the last CONVERT */
    int syncs;
    const void *sync_addr[8];
    size_t sync_len[8];
    shr_fence fence; /* of the last execute() */
} rec;

static void rec_trace(void *user, const shr_trace_event *ev) {
    (void)user;
    rec.kinds[ev->kind]++;
    if (ev->kind == SHR_TRACE_RASTER_BEGIN) rec.frames++, rec.damaged = ev->value1;
    if (ev->kind == SHR_TRACE_RASTER_END) rec.commands = ev->value0, rec.short_bufs = ev->value1;
    if (ev->kind == SHR_TRACE_SUBMIT) rec.submit_id = ev->id;
    if (ev->kind == SHR_TRACE_CONVERT) rec.converted = ev->value0;
}

static void rec_log(void *user, shr_status st, const char *msg) {
    (void)user, (void)st, (void)msg;
    rec.logs++;
}

static shr_status rec_execute(void *user, const shr_surface *dst, const shr_draw_cmd *c, size_t n, shr_fence f) {
    size_t p = 0;
    while (p < n && c[p].kind >= SHR_CMD_BUFFER_REGISTER && c[p].kind <= SHR_CMD_KEEP_RELEASE) p++;
    rec.npro = p < 64 ? p : 64;
    memcpy(rec.pro, c, rec.npro * sizeof(*c));
    rec.n = n - p < 512 ? n - p : 512;
    memcpy(rec.cmds, c + p, rec.n * sizeof(*c));
    rec.fence = f;
    return md_execute(user, dst, c, n, f);
}

static void rec_sync(void *user, const void *addr, size_t bytes) {
    (void)user;
    if (rec.syncs < 8) rec.sync_addr[rec.syncs] = addr, rec.sync_len[rec.syncs] = bytes;
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
    d->blink = (shr_blink_profile){100 * MS, 0, true};
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
    shr__buf buf;
    shr__resolved px;
    const shr__resolved *spare; /* fallback(): these pixels, else fb_st */
    shr_status st, fb_st;
    bool changed, work;
    uint64_t deadline;
    int resolves, ends, frees, shutdowns, io_n;
    uint64_t io_tag;
    shr_status io_st;
} fake;

static shr_status fk_resolve(shr__res *r, uint64_t id, uint64_t frame, const shr__resolved **out) {
    fake *f = (fake *)r;
    (void)frame;
    f->resolves++;
    if (!id) return SHR_E_NOT_FOUND;
    if (f->st != SHR_OK) return f->st;
    *out = &f->px;
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
static void fk_free(shr__res *r) {
    ((fake *)r)->frees++;
    shr__buf_free(r->ctx, &((fake *)r)->buf);
}

static shr_status fk_fallback(shr__res *r, uint64_t id, uint64_t frame, const shr__resolved **out) {
    fake *f = (fake *)r;
    (void)id, (void)frame;
    if (f->fb_st != SHR_OK) return f->fb_st;
    *out = f->spare;
    return SHR_OK;
}

static const shr__res_ops fk_ops = {fk_resolve, NULL, fk_end, fk_pump, fk_work, fk_deadline, fk_io, fk_shutdown, fk_free};
static const shr__res_ops fb_ops = {fk_resolve, fk_fallback, fk_end, fk_pump, fk_work, fk_deadline, fk_io, fk_shutdown,
                                    fk_free};
static const shr__res_ops bare_ops = {.resolve = fk_resolve, .free = fk_free};

static void fake_wrap(fake *f, shr_pixel_format format, shr_memory_domain domain) {
    size_t stride = format == SHR_FORMAT_A8 ? 8 : 32;
    shr_image m = {f->cov, 8, 16, stride, stride * 16, format, domain};
    ASSERT_EQ_LL(shr__buf_wrap(f->res.ctx, &m, &f->buf), SHR_OK);
}

/* An 8x16 opaque glyph. */
static void fake_attach(fake *f, shr_context *ctx, const shr__res_ops *ops) {
    memset(f, 0, sizeof(*f));
    memset(f->cov, 255, sizeof(f->cov));
    ASSERT_EQ_LL(shr__res_attach(ctx, &f->res, ops), SHR_OK);
    fake_wrap(f, SHR_FORMAT_A8, SHR_MEMORY_CPU);
    f->px.buf = &f->buf;
    f->px.rect = (shr_rect){0, 0, 8, 16};
}

static shr__lcmd glyph(fake *f, int32_t x, int32_t y, shr_color color) {
    return (shr__lcmd){.kind = SHR__LCMD_GLYPH, .dst = {x, y, x + 8, y + 16}, .anchor = {x, y}, .color = color,
                       .res = &f->res, .id = 1};
}

static shr__lcmd fill(shr_rect r, shr_color color) {
    return (shr__lcmd){.kind = SHR__LCMD_FILL, .dst = r, .color = color};
}

static const uint64_t nokey[2];

/* `c` (x on cell edges) as a row command. */
static shr__rcmd row_of(shr__lcmd c) {
    bool glyph = c.kind == SHR__LCMD_GLYPH;
    return (shr__rcmd){(uint16_t)(c.dst.x0 / SHR_CELL_WIDTH), (uint16_t)(c.dst.x1 / SHR_CELL_WIDTH), (uint8_t)c.dst.y0,
                       (uint8_t)c.dst.y1, c.kind, (uint8_t)c.flags, c.color, glyph ? c.id : 0, glyph ? c.bg : 0};
}

/* Row group `id` at oy from n row commands copied. */
static shr_status rows_set(shr_lyr *l, uint32_t id, int32_t oy, shr__res *res, shr__rcmd *c, size_t n) {
    return shr__lyr_row_commit(l, id, oy, res, nokey, c, n);
}

/* ---- helpers ---- */

static bool rect_eq(shr_rect a, shr_rect b) { return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1; }

/* Prologue command i of the last batch. */
static bool pro_is(int i, shr_cmd_kind kind, uint32_t id) {
    return (size_t)i < rec.npro && rec.pro[i].kind == kind && rec.pro[i].buffer == id;
}

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
    ASSERT(d.io_retry_ns == 50 * MS && d.io_timeout_ns == 1000 * MS && !d.min_frame_interval_ns);
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
    const shr_allocator half[3] = {{&f, fa_alloc, NULL, 0}, {&f, NULL, fa_free, 0}, {&f, fa_alloc, fa_free, 1}};
    for (int i = 0; i < 21; i++) {
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
        case 11: o.timestamp = (shr_timestamp_kind)(SHR_TIMESTAMP_COMPOSITOR + 1); break;
        case 12: o.flags = 1u << 2; break;
        /* Without a clock no timer could ever expire. */
        case 13: dd.now_ns = NULL, dd.io_timeout_ns = 0; break;
        case 14: dd.now_ns = NULL, dd.io_retry_ns = 0; break;
        case 15: dd.now_ns = NULL, dd.io_retry_ns = dd.io_timeout_ns = 0, drv.caps.timeout_ns = 1; break;
        case 16: dd.now_ns = NULL, dd.io_retry_ns = dd.io_timeout_ns = 0, dd.min_frame_interval_ns = 1; break;
        case 17: dd.allocator = &half[0]; break;
        case 18: dd.allocator = &half[1]; break;
        case 19: dd.allocator = &half[2]; break; /* an unknown flag */
        default: dd.max_reads = 0x10000; break;
        }
        ctx = (shr_context *)&h;
        ASSERT_EQ_LL(shr_create(&dd, &ctx), i < 20 ? SHR_E_INVALID_ARG : SHR_E_LIMIT);
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

_Alignas(64) static uint8_t band_px[2][HW * 16 * 4 + 64];

static shr_surface band_surface(int i, shr_memory_domain dom) {
    return (shr_surface){band_px[i], HW, 16, HW * SCREEN_BPP, HW * 16 * SCREEN_BPP, SHR_PIXEL_FORMAT, 1, dom, 0};
}

static shr_status configure_bands(shr_context *ctx, shr_surface *b, uint32_t count, uint32_t align) {
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = HW, sd.height = HH, sd.bands = b, sd.band_count = count, sd.band_align = align;
    return shr_screen_configure(ctx, &sd);
}

TEST test_screen_configure_bands(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_rec);
    shr_surface good = {band_px[0], HW, 16, HW * SCREEN_BPP, sizeof(band_px[0]), SHR_PIXEL_FORMAT, 1, 0, 0};
    shr_surface b[2] = {good, good};
    b[1].pixels = band_px[1];
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = HW, sd.height = HH, sd.bands = b, sd.band_count = 3;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    sd.band_count = 1, sd.bands = NULL;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    sd.bands = b, sd.composition = &comp_good;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    sd.composition = NULL, sd.flags = SHR_SCREEN_COMPOSITION;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    sd.flags = 0, sd.band_align = 32; /* HH is not a multiple */
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    sd.band_align = 1u << 31;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    sd.band_align = 3; /* nor HW */
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    sd.band_align = 16, b[0].height = 8; /* nor the bands */
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    b[0] = good, b[0].pixels = NULL;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    b[0] = good;
    sd.band_align = 0, sd.band_count = 2, b[1].height = 8;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    b[1] = good, b[1].width = HW - 1;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    b[1] = good, b[1].format = OTHER_FORMAT, b[1].stride = HW * 4;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    b[1] = good, b[1].domain = SHR_MEMORY_DEVICE, b[1].resource_id = 1;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_UNSUPPORTED);
    b[1] = good, b[1].pixels = band_px[1];
    b[0].height = b[1].height = 0;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    b[0] = b[1] = good, b[1].pixels = band_px[1];
    sd.height = 15; /* bands taller than the screen */
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_E_INVALID_ARG);
    sd.height = HH, sd.band_align = 16, sd.rotation = SHR_ROTATE_180;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_OK);
    shr_lyr *l = solid(ctx, 0, (shr_rect){0, 0, HW, 20}, RED);
    frame(ctx);
    /* Three bands (one per 16 rows), each drawn into the bands in turn and rotated by its own batch. */
    ASSERT_EQ_LL(h.drv.calls, 6);
    ASSERT_EQ_LL(rec.cmds[0].kind, SHR_CMD_ROTATE);
    ASSERT_EQ_LL(px(h.out.shown, HW - 1, HH - 1), RED);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), 0u);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

static void tweak_align32(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps.address_align = 32;
}

/* ROTATE sources start in a band at multiples of band_align (or 1) columns and rows: address_align must divide both
 * steps in bytes. */
TEST test_band_sources_aligned(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_align32);
    shr_surface b[2] = {band_surface(0, 0), band_surface(1, 0)};
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 8), 8 * SCREEN_BPP % 32 ? SHR_E_UNSUPPORTED : SHR_OK);
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 16), SHR_OK);
    b[1].stride += 1, b[1].byte_length += 16; /* 16 rows of it are not a multiple of 32 bytes */
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 16), SHR_E_UNSUPPORTED);
    b[1] = band_surface(1, 0), b[1].pixels = band_px[1] + 2;
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 16), SHR_E_UNSUPPORTED);
    harness_close(&h);
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
static shr_allocator dma_allocator = {&dma_alloc.f, dma_count_alloc, fa_free, 0};

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

/* With bands, each destination is synced once per frame (only the driver writes it after that), each ROTATE source
 * before its batch; CONVERT is traced once for all bands. */
TEST test_band_sync_and_trace(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_dma_only);
    h.out.domain = SHR_MEMORY_DMA;
    shr_surface b[2] = {band_surface(0, SHR_MEMORY_DMA), band_surface(1, SHR_MEMORY_DMA)};
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_OK);
    shr_lyr *l = solid(ctx, 0, FULL, GREEN);
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(px(h.out.shown, HW - 1, HH - 1), GREEN);
    /* band 0, the output and the first source; band 1 and its source; band 0 again, only its source */
    ASSERT_EQ_LL(rec.syncs, 6);
    ASSERT(rec.sync_addr[0] == band_px[0] && rec.sync_addr[1] == h.out.bufs[h.out.last_buf] &&
           rec.sync_addr[2] == band_px[0] && rec.sync_addr[3] == band_px[1] && rec.sync_addr[5] == band_px[0]);
    ASSERT_EQ_LL(rec.sync_len[1], sizeof(h.out.bufs[0]));
    ASSERT_EQ_LL(rec.kinds[SHR_TRACE_CONVERT], 1);
    ASSERT_EQ_LL(rec.converted, 2 * HW * HH * SCREEN_BPP);
    destroy_layers(&l, 1);
    harness_close(&h);
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
    /* A command's image spans what its rows reach, nothing when it is empty or DEVICE. */
    shr_image_ref r = {buf, 3, 2, 8, SHR_FORMAT_RGB565, 0, 0};
    ASSERT_EQ_LL(shr_image_ref_get(&r, &m), SHR_OK);
    ASSERT(m.pixels == buf && m.width == 3 && m.height == 2 && m.stride == 8 && m.byte_length == 14 &&
           m.format == SHR_FORMAT_RGB565 && m.domain == 0);
    const shr_image_ref none[3] = {{buf, 0, 2, 8, SHR_FORMAT_A8, 0, 0}, {buf, 3, 0, 8, SHR_FORMAT_A8, 0, 0},
                                   {buf, 3, 2, 0, SHR_FORMAT_A8, SHR_MEMORY_DEVICE, 0}};
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_LL(shr_image_ref_get(&none[i], &m), SHR_OK);
        ASSERT_EQ_LL(m.byte_length, 0);
    }
    shr_image keep = m;
    r.stride = 4; /* rows overlap */
    ASSERT_EQ_LL(shr_image_ref_get(&r, &m), SHR_E_INVALID_ARG);
    r.stride = 8, r.format = 0;
    ASSERT_EQ_LL(shr_image_ref_get(&r, &m), SHR_E_INVALID_ARG);
    ASSERT(!memcmp(&keep, &m, sizeof(m))); /* left as it was */
    ASSERT_EQ_LL(shr_image_ref_get(NULL, &m), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_image_ref_get(&r, NULL), SHR_E_INVALID_ARG);
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
    const shr_image_source one = {1, 1, SHR_IMAGE_SRC_RGBA8888, &rgba, 4};
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = HW, sd.height = HH;
    shr_event ev;
    shr_deadline dl;
    shr_lyr *l;
    shr_pl_res_image *img;
    uint64_t used;
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
        shr__lyr_group_set(host.lyr, 1, &c, 1), shr__lyr_row_commit(host.lyr, 1, 0, NULL, nokey, shr__lyr_row_begin(host.lyr, 1), 1),
        shr__lyr_groups_clear(host.lyr, 0),
        shr__lyr_attach(host.lyr, &host, NULL, NULL, NULL), shr__res_attach(ctx, &f.res, &fk_ops),
        shr__ctx_read(ctx, &f.res, &src, 0, 0, NULL, 0), shr_pl_res_image_create(ctx, 1, 1, &rgba, 4, &img),
        shr_pl_res_image_update(host.img, r, &rgba, 4), shr_pl_res_image_update(host.img, none, NULL, 0),
        shr_lyr_cmd_image(host.lyr, host.img, none, (shr_point){0, 0}), shr_pl_res_image_budget(ctx, &used, NULL),
        shr_pl_res_image_create_scaled(ctx, &one, r, 1, 1, SHR_SCALE_BILINEAR, &img),
        shr_pl_res_image_view(host.img, r, 1, 1, 0, &img), shr_pl_res_image_release(host.img)};
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

/* Output submitted every 33 ms does not hold the phase: it changes every 100 ms of the clock. */
TEST test_blink_ignores_output(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_blink);
    shr_lyr *b = blink_layer(ctx, FULL), *s = solid(ctx, 1, (shr_rect){40, 0, 48, 16}, RED);
    for (uint64_t t = 0; t <= 400 * MS; t += 33 * MS) {
        fake_now = t;
        shr__lcmd c = fill((shr_rect){0, 0, 8, 16}, (t / (33 * MS)) % 2 ? RED : GREEN);
        paint(s, 1, &c);
        frame(ctx);
        ASSERT_EQ_LL(px(h.out.shown, 0, 0), (t / (100 * MS)) % 2 ? 0 : WHITE);
        ASSERT_EQ_LL(deadline_at(ctx), (t / (100 * MS) + 1) * 100 * MS);
    }
    shr_lyr *ls[2] = {b, s};
    destroy_layers(ls, 2);
    harness_close(&h);
    PASS();
}

static void tweak_blink_later(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->blink = (shr_blink_profile){100 * MS, 500 * MS, false};
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
    tweak_blink(d, drv);
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

static void tweak_cap(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->min_frame_interval_ns = 20 * MS;
}

static void recolor(shr_lyr *l, shr_color color) {
    shr__lcmd c = fill(FULL, color);
    paint(l, 1, &c);
    ASSERT_EQ_LL(shr_submit(l->ctx), SHR_OK);
}

/* Frames start at least min_frame_interval_ns apart; what changed meanwhile is drawn by the next one. */
TEST test_frame_cap(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_cap);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    fake_now = 5 * MS;
    recolor(l, GREEN);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 1);
    ASSERT_EQ_LL(deadline_at(ctx), 20 * MS);
    fake_now = 12 * MS;
    recolor(l, BLUE);
    shr_pump(ctx);
    ASSERT_EQ_LL(deadline_at(ctx), 20 * MS);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    r.deadline = 15 * MS; /* the earliest timer wins */
    ASSERT_EQ_LL(deadline_at(ctx), 15 * MS);
    r.deadline = 25 * MS;
    ASSERT_EQ_LL(deadline_at(ctx), 20 * MS);
    r.deadline = 0;
    fake_now = 20 * MS;
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(h.out.presents == 2 && rec.frames == 2 && px(h.out.shown, 0, 0) == BLUE);
    for (uint64_t id = 1; id <= 2; id++) {
        ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED).frame_id, id);
        ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_RELEASED).frame_id, id);
    }
    ASSERT_EQ_LL(shr_poll_event(ctx, &(shr_event){0}), SHR_E_NOT_FOUND);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);

    /* Neither a frame the output already shows nor one the output refused moves the next start. */
    fake_now = 45 * MS;
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    shr_pump(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED).frame_id, 3);
    fake_now = 46 * MS;
    h.out.acquire_result = SHR_E_WOULD_BLOCK;
    recolor(l, RED);
    shr_pump(ctx);
    h.out.acquire_result = SHR_OK;
    ASSERT_EQ_LL(shr_output_ready(ctx), SHR_OK);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(h.out.presents == 3 && px(h.out.shown, 0, 0) == RED);
    fake_now = 50 * MS;
    recolor(l, GREEN);
    ASSERT_EQ_LL(deadline_at(ctx), 66 * MS);
    fake_now = 66 * MS;
    settle(ctx);
    ASSERT(h.out.presents == 4 && px(h.out.shown, 0, 0) == GREEN);
    r.res.dead = true;
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

static void tweak_blink_cap(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_blink(d, drv);
    d->min_frame_interval_ns = 30 * MS;
}

/* A capped blink frame shows the phase of its start; phases that pass while it waits draw nothing. */
TEST test_frame_cap_blink(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_blink_cap);
    shr_lyr *l = blink_layer(ctx, FULL), *s = solid(ctx, 1, (shr_rect){40, 0, 48, 16}, RED);
    frame(ctx);
    ASSERT_EQ_LL(px(h.out.shown, 0, 0), WHITE);
    ASSERT_EQ_LL(deadline_at(ctx), 100 * MS); /* the blink phase comes after the next allowed start */
    fake_now = 90 * MS;
    ASSERT_EQ_LL(shr_lyr_set_visible(s, false), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(h.out.presents, 2);
    ASSERT_EQ_LL(deadline_at(ctx), 120 * MS); /* the next allowed start comes after the blink phase */
    fake_now = 110 * MS;
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 2);
    ASSERT_EQ_LL(deadline_at(ctx), 120 * MS);
    fake_now = 120 * MS;
    shr_pump(ctx);
    ASSERT(h.out.presents == 3 && px(h.out.shown, 0, 0) == 0 && px(h.out.shown, 20, 0) == WHITE);
    fake_now = 195 * MS;
    ASSERT_EQ_LL(shr_lyr_set_visible(s, true), SHR_OK);
    frame(ctx);
    ASSERT(h.out.presents == 4 && px(h.out.shown, 0, 0) == 0 && px(h.out.shown, 40, 0) == RED);
    ASSERT_EQ_LL(deadline_at(ctx), 225 * MS);
    fake_now = 301 * MS; /* hidden again */
    ASSERT_EQ_LL(deadline_at(ctx), 400 * MS);
    shr_pump(ctx);
    ASSERT_EQ_LL(h.out.presents, 4);
    fake_now = 400 * MS;
    shr_pump(ctx);
    ASSERT(h.out.presents == 5 && px(h.out.shown, 0, 0) == WHITE);
    destroy_layers(&l, 1);
    destroy_layers(&s, 1);
    harness_close(&h);
    PASS();
}

static uint32_t max_cmds;
static void tweak_max_cmds(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    d->max_commands = max_cmds;
}

/* A band frame fails cleanly wherever memory or the command limit runs out. */
TEST test_band_frame_failures(void) {
    for (int mode = 0; mode < 2; mode++) {
        bool done = false;
        for (uint32_t n = 1; !done; n++) {
            harness h;
            max_cmds = n;
            shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, mode ? tweak_max_cmds : tweak_oom);
            shr_surface b[2] = {band_surface(0, 0), band_surface(1, 0)};
            ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_OK);
            shr_lyr *l = solid(ctx, 0, (shr_rect){0, 8, HW, HH}, GREEN);
            if (!mode) oom.budget = n - 1;
            frame(ctx);
            oom.budget = -1;
            shr_event ev = {0};
            ASSERT_EQ_LL(shr_poll_event(ctx, &ev), SHR_OK);
            done = ev.kind == SHR_EVENT_PRESENT_ACCEPTED;
            if (!done) ASSERT(ev.kind == SHR_EVENT_PRESENT_FAILED && ev.status == (mode ? SHR_E_LIMIT : SHR_E_NO_MEMORY));
            if (done) ASSERT(px(h.out.shown, 0, 7) == 0 && px(h.out.shown, HW - 1, HH - 1) == GREEN);
            destroy_layers(&l, 1);
            harness_close(&h);
        }
    }
    PASS();
}

static struct {
    fail_alloc f;
    size_t size;
    shr_alloc_kind kind;
} kind_alloc;
static void *kind_count_alloc(void *user, size_t size, size_t align, shr_alloc_kind kind) {
    void *p = fa_alloc(user, size, align, kind);
    if (p) kind_alloc.size = size, kind_alloc.kind = kind;
    return p;
}
static shr_allocator kind_allocator = {&kind_alloc.f, kind_count_alloc, fa_free, 0};

static void tweak_kind_alloc(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    kind_alloc.f = (fail_alloc){-1, 0};
    d->allocator = &kind_allocator;
}

/* Configuring bands reserves one band's commands as descriptor memory; without it the configuration stays. Commands
 * carry strides in 32 bits: a wider one is out of the driver's reach. */
TEST test_band_command_list(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_kind_alloc);
    shr_surface b[2] = {band_surface(0, 0), band_surface(1, 0)};
    shr_lyr *l = solid(ctx, 0, FULL, GREEN);
    kind_alloc.f.budget = 0;
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_E_NO_MEMORY);
    kind_alloc.f.budget = -1;
    frame(ctx);
    ASSERT(h.drv.calls == 1 && px(h.out.shown, HW - 1, HH - 1) == GREEN); /* still without bands */
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_OK);
    size_t cells = (HW + SHR_CELL_WIDTH - 1) / SHR_CELL_WIDTH * ((16 + SHR_CELL_HEIGHT - 1) / SHR_CELL_HEIGHT);
    ASSERT(kind_alloc.kind == SHR_ALLOC_PAYLOAD && kind_alloc.size == (2 * cells + 32) * sizeof(shr_draw_cmd));
    frame(ctx);
    ASSERT(h.drv.calls == 7 && px(h.out.shown, HW - 1, HH - 1) == GREEN);
    shr_surface wide = band_surface(1, 0);
    wide.stride = (size_t)1 << 32, wide.byte_length = 15 * wide.stride + HW * SCREEN_BPP; /* never read */
    b[1] = wide;
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, NULL), SHR_OK); /* back to the payload list */
    frame(ctx);
    ASSERT(h.drv.calls == 8 && px(h.out.shown, HW - 1, HH - 1) == GREEN);
    destroy_layers(&l, 1);
    harness_close(&h);
    ASSERT_EQ_LL(kind_alloc.f.live, 0);
    PASS();
}

/* An allocator asking for hints gets the band command list as hot memory. */
TEST test_band_command_list_hot(void) {
    kind_allocator.flags = SHR_ALLOC_HOT;
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_kind_alloc);
    kind_allocator.flags = 0;
    shr_surface b[2] = {band_surface(0, 0), band_surface(1, 0)};
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_OK);
    ASSERT_EQ_LL(kind_alloc.kind, SHR_ALLOC_PAYLOAD | SHR_ALLOC_HOT);
    harness_close(&h);
    ASSERT_EQ_LL(kind_alloc.f.live, 0);
    PASS();
}

static void tweak_two_ids(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps.max_buffers = 2;
}

/* A band's commands are built once the band before ran, yet buffer ids stay held for the frame: no band evicts a
 * buffer an earlier band drew from, also one that took the id of a buffer it evicted; a buffer left without is not
 * drawn. */
TEST test_band_buffers_held_for_frame(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_two_ids);
    shr_surface b[2] = {band_surface(0, 0), band_surface(1, 0)};
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_OK);
    fake r[5]; /* X, Y, then A, B and C, which draws nothing */
    for (int i = 0; i < 5; i++) fake_attach(&r[i], ctx, &fk_ops);
    memset(r[4].cov, 0, sizeof(r[4].cov));
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr__lcmd old[2] = {glyph(&r[0], 0, 0, RED), glyph(&r[1], 8, 0, RED)};
    paint(l, 2, old);
    frame(ctx);
    drain(ctx);
    /* A takes the id of X in band 0, B that of Y in band 1; C finds none in band 2, where A draws again. */
    const shr__lcmd cur[4] = {glyph(&r[2], 0, 0, GREEN), glyph(&r[3], 0, 16, BLUE), glyph(&r[4], 0, 32, RED),
                              glyph(&r[2], 8, 32, GREEN)};
    paint(l, 4, cur);
    frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    expect_event(ctx, SHR_EVENT_FRAME_RELEASED);
    ASSERT(r[2].buf.id == 1 && r[3].buf.id == 2 && r[4].buf.id == 0 && rec.short_bufs == 1);
    ASSERT(px(h.out.shown, 0, 0) == GREEN && px(h.out.shown, 0, 16) == BLUE && px(h.out.shown, 8, 32) == GREEN);
    const shr__lcmd again[3] = {glyph(&r[2], 0, 0, GREEN), glyph(&r[3], 0, 16, BLUE), glyph(&r[2], 8, 32, GREEN)};
    paint(l, 3, again);
    frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(px(h.out.shown, 0, 0) == GREEN && px(h.out.shown, 0, 16) == BLUE && px(h.out.shown, 8, 32) == GREEN);
    destroy_layers(&l, 1);
    for (int i = 0; i < 5; i++) r[i].res.dead = true;
    harness_close(&h);
    PASS();
}

/* The limit applies while commands are emitted: clear, fill, glyph (a driver keeping nothing gets no keep group). */
TEST test_command_limit_fails_frame(void) {
    for (max_cmds = 1; max_cmds <= 3; max_cmds++) {
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
        if (max_cmds < 3)
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

/* Destinations outside the driver's limits fail the frame with SHR_E_UNSUPPORTED. */
TEST test_driver_caps_checked(void) {
    const struct {
        shr_driver_caps caps;
        bool ok;
    } cases[] = {
        {{.max_buffers = HBUFS}, true}, /* no domains: CPU */
        {{.domains = SHR_MEMORY_CPU, .address_align = 1, .stride_align = 1, .max_width = HW, .max_height = HH}, true},
        {{.domains = SHR_MEMORY_DMA}, false},
        {{.domains = SHR_MEMORY_CPU, .stride_align = 1000}, false},
        {{.domains = SHR_MEMORY_CPU, .max_width = 10}, false},
        {{.domains = SHR_MEMORY_CPU, .max_height = 10}, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        caps_case = cases[i].caps;
        harness h;
        shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_caps);
        shr_lyr *l = solid(ctx, 0, FULL, GREEN);
        frame(ctx);
        if (cases[i].ok)
            expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
        else
            ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_UNSUPPORTED);
        destroy_layers(&l, 1);
        harness_close(&h);
    }
    PASS();
}

/* Buffers are checked against the caps when made: wrapped memory the driver cannot reach is refused. */
TEST test_buffer_wrap(void) {
    caps_case = (shr_driver_caps){.domains = SHR_MEMORY_CPU, .address_align = 4, .stride_align = 4,
                                  .max_buffers = HBUFS, .max_buffer_width = 16, .max_buffer_height = 8};
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak_caps);
    static _Alignas(16) uint8_t px[16 * 9 * 4];
    shr__buf b = {0};
    const shr_image good = {px, 16, 8, 16, 16 * 8, SHR_FORMAT_A8, 0};
    struct {
        shr_image m;
        shr_status st;
    } cases[] = {
        {good, SHR_OK},
        {{px, 16, 8, 64, 16 * 8 * 4, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU}, SHR_OK},
        {{px, 16, 8, 8, 64, SHR_FORMAT_A4, SHR_MEMORY_CPU}, SHR_OK},
        {{px, 16, 8, 32, 32 * 8, SHR_FORMAT_RGB565, 0}, SHR_E_INVALID_ARG},
        {{px, 16, 8, 15, 16 * 8, SHR_FORMAT_A8, 0}, SHR_E_INVALID_ARG},
        {{px, 17, 8, 20, 20 * 8, SHR_FORMAT_A8, 0}, SHR_E_UNSUPPORTED},
        {{px, 16, 9, 16, 16 * 9, SHR_FORMAT_A8, 0}, SHR_E_UNSUPPORTED},
        {{px + 2, 8, 8, 16, 16 * 8, SHR_FORMAT_A8, 0}, SHR_E_UNSUPPORTED},
        {{px, 14, 8, 14, 14 * 8, SHR_FORMAT_A8, 0}, SHR_E_UNSUPPORTED},
        {{px, 16, 8, 16, 16 * 8, SHR_FORMAT_A8, SHR_MEMORY_DMA}, SHR_E_UNSUPPORTED},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(&b, 0x5A, sizeof(b));
        ASSERT_EQ_LL(shr__buf_wrap(ctx, &cases[i].m, &b), cases[i].st);
        if (cases[i].st == SHR_OK) ASSERT(!memcmp(&b.mem, &cases[i].m, sizeof(b.mem)) && !b.id && !b.owned);
    }
    ASSERT_EQ_LL(shr__buf_wrap(ctx, NULL, &b), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr__buf_wrap(ctx, &good, &b), SHR_OK);
    shr__buf_free(ctx, &b); /* not owned: the memory stays */
    ASSERT(b.mem.pixels == NULL && px[0] == 0);
    harness_close(&h);
    PASS();
}

static fail_alloc buf_oom;
static shr_allocator buf_oom_allocator;
static void tweak_buf_alloc(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_caps(d, drv);
    buf_oom = (fail_alloc){-1, 0};
    buf_oom_allocator = fail_allocator(&buf_oom);
    dma_alloc.f = (fail_alloc){-1, 0}, dma_alloc.dma = 0;
    d->allocator = caps_case.domains == SHR_MEMORY_DMA ? &dma_allocator : &buf_oom_allocator;
}

/* Allocated buffers follow the driver: alignment, stride, size limits and memory domain. */
TEST test_buffer_alloc(void) {
    caps_case = (shr_driver_caps){.domains = SHR_MEMORY_CPU, .address_align = 128, .stride_align = 12,
                                  .max_buffers = HBUFS, .max_buffer_width = 64, .max_buffer_height = 32};
    harness h;
    shr_context *ctx = harness_open(&h, 0, tweak_buf_alloc);
    shr__buf b = {0};
    ASSERT_EQ_LL(shr__buf_alloc(ctx, SHR_FORMAT_A4, 7, 3, &b), SHR_OK);
    ASSERT(b.owned && b.mem.stride == 12 && b.mem.byte_length == 36 && (uintptr_t)b.mem.pixels % 128 == 0);
    ASSERT(b.mem.domain == SHR_MEMORY_CPU && b.mem.format == SHR_FORMAT_A4 && b.mem.width == 7 && b.mem.height == 3);
    shr__buf_free(ctx, &b);
    ASSERT_EQ_LL(shr__buf_alloc(ctx, SHR_FORMAT_RGBA8888, 64, 32, &b), SHR_OK);
    ASSERT_EQ_LL(b.mem.stride, 264);
    long live = buf_oom.live;
    shr__buf_free(ctx, &b);
    ASSERT_EQ_LL(buf_oom.live, live - 1);
    const struct {
        shr_pixel_format f;
        int32_t w, h;
        shr_status st;
    } bad[] = {{SHR_FORMAT_RGB565, 1, 1, SHR_E_INVALID_ARG}, {SHR_FORMAT_RGBX8888, 1, 1, SHR_E_INVALID_ARG},
               {SHR_FORMAT_A8, 0, 1, SHR_E_INVALID_ARG},
               {SHR_FORMAT_A8, 1, 0, SHR_E_INVALID_ARG},     {SHR_FORMAT_A8, 65, 1, SHR_E_UNSUPPORTED},
               {SHR_FORMAT_A8, 1, 33, SHR_E_UNSUPPORTED}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        ASSERT_EQ_LL(shr__buf_alloc(ctx, bad[i].f, bad[i].w, bad[i].h, &b), bad[i].st);
    buf_oom.budget = 0;
    ASSERT_EQ_LL(shr__buf_alloc(ctx, SHR_FORMAT_A8, 4, 4, &b), SHR_E_NO_MEMORY);
    buf_oom.budget = -1;
    harness_close(&h);
    ASSERT_EQ_LL(buf_oom.live, 0);

    /* A row padded to 3 * 2863311533 = 2^33 + 7 bytes: INT32_MAX rows take more than 2^64. */
    caps_case = (shr_driver_caps){.stride_align = 2863311533u, .max_buffers = HBUFS};
    ctx = harness_open(&h, 0, tweak_caps);
    ASSERT_EQ_LL(shr__buf_alloc(ctx, SHR_FORMAT_RGBA8888, INT32_MAX, INT32_MAX, &b), SHR_E_NO_MEMORY);
    harness_close(&h);
    caps_case = (shr_driver_caps){.max_buffers = HBUFS}; /* no alignment asked: 64 bytes, rows unpadded */
    ctx = harness_open(&h, 0, tweak_caps);
    ASSERT_EQ_LL(shr__buf_alloc(ctx, SHR_FORMAT_A8, 3, 2, &b), SHR_OK);
    ASSERT(b.mem.stride == 3 && (uintptr_t)b.mem.pixels % 64 == 0);
    shr__buf_free(ctx, &b);
    harness_close(&h);

    caps_case = (shr_driver_caps){.domains = SHR_MEMORY_DMA, .max_buffers = HBUFS}; /* DMA memory from the app */
    ctx = harness_open(&h, 0, tweak_buf_alloc);
    ASSERT_EQ_LL(shr__buf_alloc(ctx, SHR_FORMAT_A8, 3, 2, &b), SHR_OK);
    ASSERT(b.mem.domain == SHR_MEMORY_DMA && dma_alloc.dma == 1);
    shr__buf_free(ctx, &b);
    harness_close(&h);
    ASSERT_EQ_LL(dma_alloc.f.live, 0);

    ctx = harness_open(&h, 0, tweak_caps); /* without it, nothing the driver reaches */
    ASSERT_EQ_LL(shr__buf_alloc(ctx, SHR_FORMAT_A8, 3, 2, &b), SHR_E_UNSUPPORTED);
    harness_close(&h);
    PASS();
}

static void tweak_sync(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps.domains = SHR_MEMORY_CPU | SHR_MEMORY_DMA;
    drv->sync = rec_sync;
    ((mock_output *)d->output->user)->domain = SHR_MEMORY_DMA;
}

/* sync() hands the device the destination, each REGISTERed DMA buffer whole and only the rows of an UPDATE. */
TEST test_dma_sync(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_sync);
    fake a, b;
    fake_attach(&a, ctx, &fk_ops);
    fake_attach(&b, ctx, &fk_ops);
    fake_wrap(&a, SHR_FORMAT_A8, SHR_MEMORY_DMA);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr__lcmd c[5] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, 16, 16}, .key = {1}}, glyph(&a, 0, 0, WHITE),
                            glyph(&a, 8, 0, WHITE), {.kind = SHR__LCMD_CACHE_END}, glyph(&b, 16, 0, WHITE)};
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, c, 5), SHR_OK);
    frame(ctx);
    ASSERT_EQ_LL(rec.npro, 2);
    ASSERT_EQ_LL(rec.syncs, 2);
    ASSERT(rec.sync_addr[0] == h.out.bufs[h.out.last_buf] && rec.sync_addr[1] == a.cov && rec.sync_len[1] == 128);
    h.out.domain = SHR_MEMORY_CPU;
    shr__buf_changed(&a.buf, (shr_rect){2, 3, 4, 5});
    shr__buf_changed(&b.buf, (shr_rect){0, 0, 8, 16}); /* CPU memory: no sync */
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT(rec.npro == 2 && pro_is(0, SHR_CMD_BUFFER_UPDATE, 1) && rect_eq(rec.pro[0].src_rect, (shr_rect){2, 3, 4, 5}));
    ASSERT(rec.syncs == 3 && rec.sync_addr[2] == a.cov + 3 * 8 && rec.sync_len[2] == 16);
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT(rec.npro == 0 && rec.syncs == 3);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_180, 0, 0, NULL), SHR_OK); /* CPU memory will do */
    frame(ctx);
    ASSERT(h.out.presents == 4 && rec.syncs == 3);
    destroy_layers(&l, 1);
    a.res.dead = b.res.dead = true;
    harness_close(&h);
    PASS();
}

/* ---- buffer registry ---- */

/* Paints one glyph per fake, 8 pixels apart. */
static void draw_fakes(shr_lyr *l, int n, fake *const *f) {
    shr__lcmd c[8];
    for (int i = 0; i < n; i++) c[i] = glyph(f[i], i * 8, 0, WHITE);
    paint(l, (size_t)n, c);
}

/* Ids are given on first draw, dense from 1, and come back with a RELEASE once a buffer is freed. */
TEST test_buffer_ids(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    fake a, b, c, d;
    fake_attach(&a, ctx, &fk_ops), fake_attach(&b, ctx, &fk_ops), fake_attach(&c, ctx, &fk_ops);
    fake_attach(&d, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    draw_fakes(l, 3, (fake *[]){&a, &b, &a});
    frame(ctx);
    ASSERT(rec.npro == 2 && pro_is(0, SHR_CMD_BUFFER_REGISTER, 1) && pro_is(1, SHR_CMD_BUFFER_REGISTER, 2));
    ASSERT(rec.pro[0].src.pixels == a.cov && rec.pro[1].src.pixels == b.cov && a.buf.id == 1 && b.buf.id == 2);
    ASSERT(rec.cmds[1].buffer == 1 && rec.cmds[2].buffer == 2 && rec.cmds[3].buffer == 1);
    ASSERT(rect_eq(rec.cmds[1].src_rect, (shr_rect){0, 0, 8, 16}) && h.drv.buffers[1].pixels == b.cov);
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT_EQ_LL(rec.npro, 0); /* the driver has them */
    draw_fakes(l, 3, (fake *[]){&a, &c, &a});
    b.res.dead = true;
    frame(ctx); /* b is freed after this frame */
    ASSERT(pro_is(0, SHR_CMD_BUFFER_REGISTER, 3) && c.buf.id == 3 && b.frees == 1 && ctx->nreleased == 1);
    draw_fakes(l, 3, (fake *[]){&a, &d, &c});
    frame(ctx); /* the released id is taken again after its RELEASE */
    ASSERT(rec.npro == 2 && pro_is(0, SHR_CMD_BUFFER_RELEASE, 2) && pro_is(1, SHR_CMD_BUFFER_REGISTER, 2));
    ASSERT(d.buf.id == 2 && h.drv.buffers[1].pixels == d.cov && ctx->nreleased == 0);
    ASSERT_EQ_LL(px(h.out.shown, 9, 3), WHITE);
    draw_fakes(l, 2, (fake *[]){&a, &c}); /* d is freed while the frame waits: its RELEASE is for the next one */
    h.drv.block_next = 1;
    frame(ctx);
    d.res.dead = true;
    shr_pump(ctx);
    shr_driver_ready(ctx);
    shr_pump(ctx);
    ASSERT(ctx->nreleased == 1 && ctx->released[0] == 2 && h.out.presents == 5);
    draw_fakes(l, 1, (fake *[]){&a});
    frame(ctx);
    ASSERT(rec.npro == 1 && pro_is(0, SHR_CMD_BUFFER_RELEASE, 2) && ctx->nreleased == 0);
    destroy_layers(&l, 1);
    a.res.dead = c.res.dead = true;
    harness_close(&h);
    PASS();
}

/* A refused frame keeps its plan for the retry; a superseded one drops it: no id is lost, no change forgotten. */
TEST test_buffer_plan_dropped(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    fake a, b;
    fake_attach(&a, ctx, &fk_ops), fake_attach(&b, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    draw_fakes(l, 1, (fake *[]){&a});
    frame(ctx);
    shr__buf_changed(&a.buf, (shr_rect){0, 0, 2, 2});
    shr__res_changed(&a.res, (shr_rect){0, 0, 2, 2});
    draw_fakes(l, 2, (fake *[]){&a, &b});
    h.drv.block_next = 1;
    frame(ctx);
    ASSERT(b.buf.id == 2 && !ctx->slots[1].buf && rec.npro == 2); /* planned, not taken */
    draw_fakes(l, 2, (fake *[]){&b, &a});
    frame(ctx); /* supersedes the refused frame, the next one runs */
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_FRAME_SUPERSEDED, NULL), 1);
    ASSERT(rec.npro == 2 && pro_is(0, SHR_CMD_BUFFER_REGISTER, 2) && pro_is(1, SHR_CMD_BUFFER_UPDATE, 1));
    ASSERT_EQ_LL(ctx->nreleased, 0); /* a dropped plan the driver never saw releases nothing */
    ASSERT(rect_eq(rec.pro[1].src_rect, (shr_rect){0, 0, 2, 2}) && b.buf.id == 2 && shr__rect_empty(a.buf.dirty));

    shr__buf_changed(&b.buf, (shr_rect){0, 1, 8, 2});
    shr__buf_changed(&b.buf, (shr_rect){-4, 14, 3, 99}); /* clipped to the buffer */
    shr_request_redraw(ctx);
    h.drv.block_next = 1;
    int updates = h.drv.updates;
    frame(ctx);
    ASSERT_EQ_LL(h.drv.updates, updates);
    shr_driver_ready(ctx);
    shr_pump(ctx); /* the retry runs the same prologue once */
    ASSERT(h.drv.updates == updates + 1 && rec.npro == 1 && rect_eq(rec.pro[0].src_rect, (shr_rect){0, 1, 8, 16}));
    ASSERT(shr__rect_empty(b.buf.dirty) && ctx->slots[1].buf == &b.buf);

    shr__buf_changed(&a.buf, (shr_rect){1, 1, 2, 2}); /* a configure drops a planned frame too */
    shr_request_redraw(ctx);
    h.drv.block_next = 1;
    frame(ctx);
    ASSERT_EQ_LL(configure(ctx, SHR_ROTATE_NONE, 0, 0, NULL), SHR_OK);
    ASSERT(rect_eq(a.buf.dirty, (shr_rect){1, 1, 2, 2}) && a.buf.id == 1 && b.buf.id == 2);
    destroy_layers(&l, 1);
    a.res.dead = b.res.dead = true;
    harness_close(&h);
    PASS();
}

/* After a failed batch or a timeout the driver's registrations are unknown: buffers register again. */
TEST test_buffer_registered_again(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_timeout);
    h.drv.reset_ok = true;
    fake a;
    fake_attach(&a, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    draw_fakes(l, 1, (fake *[]){&a});
    frame(ctx);
    h.drv.fail_next = 1; /* the mock forgets every id */
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_PRESENT_FAILED, NULL), 1);
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT(pro_is(0, SHR_CMD_BUFFER_REGISTER, 1) && rec.npro == 1 && px(h.out.shown, 1, 1) == WHITE);

    h.drv.async = true;
    shr_request_redraw(ctx);
    frame(ctx);
    fake_now += 20 * MS;
    shr_pump(ctx); /* the watchdog resets the driver */
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_DRIVER_TIMEOUT, NULL), 1);
    h.drv.async = false;
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT(pro_is(0, SHR_CMD_BUFFER_REGISTER, 1) && rec.npro == 1);
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT_EQ_LL(rec.npro, 0);
    destroy_layers(&l, 1);
    a.res.dead = true;
    harness_close(&h);
    PASS();
}

static uint32_t limit_ids, limit_flags;
static uint64_t limit_bytes;
static void tweak_limits(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps.max_buffers = limit_ids, drv->caps.buffer_flags = limit_flags, drv->caps.buffer_bytes = limit_bytes;
}

/* A driver keeping copies holds at most buffer_bytes: least recently used buffers the frame does not draw from
 * are released first, also one the frame draws later (it registers again). In-place drivers ignore the bytes. */
TEST test_buffer_eviction_by_bytes(void) {
    for (int copies = 0; copies < 2; copies++) {
        limit_ids = HBUFS, limit_flags = copies ? SHR_BUFFER_COPIES : 0, limit_bytes = 256; /* two fakes */
        harness h;
        shr_context *ctx = harness_open(&h, PRESERVED, tweak_limits);
        fake a, b, c;
        fake_attach(&a, ctx, &fk_ops), fake_attach(&b, ctx, &fk_ops), fake_attach(&c, ctx, &fk_ops);
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        draw_fakes(l, 2, (fake *[]){&a, &b});
        frame(ctx);
        ASSERT_EQ_LL(ctx->resident, 256);
        draw_fakes(l, 2, (fake *[]){&c, &a});
        frame(ctx);
        if (copies) {
            ASSERT(rec.npro == 4 && pro_is(0, SHR_CMD_BUFFER_RELEASE, 1) && pro_is(1, SHR_CMD_BUFFER_REGISTER, 3));
            ASSERT(pro_is(2, SHR_CMD_BUFFER_RELEASE, 2) && pro_is(3, SHR_CMD_BUFFER_REGISTER, 1));
            ASSERT(c.buf.id == 3 && a.buf.id == 1 && b.buf.id == 0 && ctx->resident == 256 && !ctx->slots[1].buf);
        } else {
            ASSERT(rec.npro == 1 && pro_is(0, SHR_CMD_BUFFER_REGISTER, 3) && ctx->resident == 384 && b.buf.id == 2);
        }
        ASSERT(px(h.out.shown, 1, 1) == WHITE && px(h.out.shown, 9, 1) == WHITE && h.drv.completed == SHR_OK);
        limit_bytes = 100; /* one fake is too large */
        destroy_layers(&l, 1);
        a.res.dead = b.res.dead = c.res.dead = true;
        harness_close(&h);
    }
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_limits);
    fake a;
    fake_attach(&a, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    draw_fakes(l, 1, (fake *[]){&a});
    frame(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_LIMIT);
    destroy_layers(&l, 1);
    a.res.dead = true;
    harness_close(&h);
    PASS();
}

static void tweak_copies_timeout(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps.buffer_flags = SHR_BUFFER_COPIES, drv->caps.buffer_bytes = 256, drv->caps.timeout_ns = 10 * MS;
}

/* A failed or timed-out batch may have run its buffer commands: the ids it registered or released are released
 * first in the next prologue, so a driver keeping copies within buffer_bytes takes the next REGISTERs. */
TEST test_buffer_lost_batch_released(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_copies_timeout);
    h.drv.budget = 256, h.drv.fail_late = true, h.drv.reset_ok = true;
    fake a, b, c;
    fake_attach(&a, ctx, &fk_ops), fake_attach(&b, ctx, &fk_ops), fake_attach(&c, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    draw_fakes(l, 2, (fake *[]){&a, &b});
    frame(ctx);
    shr__buf_changed(&b.buf, (shr_rect){0, 0, 1, 1});
    draw_fakes(l, 2, (fake *[]){&b, &c});
    h.drv.fail_next = 1; /* runs UPDATE 2, RELEASE 1, REGISTER 3 (c), then fails */
    frame(ctx);
    ASSERT(rec.npro == 3 && pro_is(1, SHR_CMD_BUFFER_RELEASE, 1) && pro_is(2, SHR_CMD_BUFFER_REGISTER, 3));
    ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_PRESENT_FAILED, NULL), 1);
    ASSERT(a.buf.id == 0 && c.buf.id == 0 && b.buf.id == 2 && ctx->nreleased == 2);
    draw_fakes(l, 2, (fake *[]){&a, &b});
    frame(ctx); /* without the RELEASE of 3 the driver would hold a, b and c */
    ASSERT(rec.npro == 4 && pro_is(0, SHR_CMD_BUFFER_RELEASE, 1) && pro_is(1, SHR_CMD_BUFFER_RELEASE, 3));
    ASSERT(pro_is(2, SHR_CMD_BUFFER_REGISTER, 1) && pro_is(3, SHR_CMD_BUFFER_REGISTER, 2));
    ASSERT(h.out.presents == 2 && md_held(&h.drv) == 256 && ctx->resident == 256 && ctx->nreleased == 0);

    draw_fakes(l, 1, (fake *[]){&a});
    b.res.dead = true;
    frame(ctx); /* b is freed: RELEASE 2 opens the next prologue */
    h.drv.async = true;
    draw_fakes(l, 2, (fake *[]){&a, &c});
    frame(ctx); /* accepted with RELEASE 2, REGISTER 2 (c), then times out */
    ASSERT(rec.npro == 2 && pro_is(0, SHR_CMD_BUFFER_RELEASE, 2) && pro_is(1, SHR_CMD_BUFFER_REGISTER, 2));
    fake_now += 20 * MS;
    shr_pump(ctx);
    ASSERT(count_events(ctx, SHR_EVENT_DRIVER_TIMEOUT, NULL) == 1 && c.buf.id == 0 && ctx->nreleased == 1);
    h.drv.async = false;
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT(rec.npro == 3 && pro_is(0, SHR_CMD_BUFFER_RELEASE, 2) && pro_is(1, SHR_CMD_BUFFER_REGISTER, 1));
    ASSERT(pro_is(2, SHR_CMD_BUFFER_REGISTER, 2) && c.buf.id == 2 && md_held(&h.drv) == ctx->resident);
    ASSERT_EQ_LL(px(h.out.shown, 9, 1), WHITE);
    destroy_layers(&l, 1);
    a.res.dead = c.res.dead = true;
    harness_close(&h);
    PASS();
}

/* Every driver has max_buffers ids; a frame drawing from more buffers leaves out what gets none. */
TEST test_buffer_eviction_by_ids(void) {
    limit_ids = 2, limit_flags = SHR_BUFFER_COPIES, limit_bytes = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_limits);
    fake a, b, c;
    fake_attach(&a, ctx, &fk_ops), fake_attach(&b, ctx, &fk_ops), fake_attach(&c, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    draw_fakes(l, 2, (fake *[]){&a, &b});
    frame(ctx);
    draw_fakes(l, 2, (fake *[]){&b, &c});
    frame(ctx);
    ASSERT(rec.npro == 2 && pro_is(0, SHR_CMD_BUFFER_RELEASE, 1) && pro_is(1, SHR_CMD_BUFFER_REGISTER, 1));
    ASSERT(a.buf.id == 0 && b.buf.id == 2 && c.buf.id == 1);
    draw_fakes(l, 1, (fake *[]){&a}); /* frees an id while a frame waits: the freed buffer was its victim */
    h.drv.block_next = 1;
    frame(ctx);
    ASSERT(pro_is(0, SHR_CMD_BUFFER_RELEASE, 2) && pro_is(1, SHR_CMD_BUFFER_REGISTER, 2) && rec.npro == 2);
    destroy_layers(&l, 1);
    b.res.dead = true;
    shr_pump(ctx);
    ASSERT(b.frees == 1 && ctx->nreleased == 1);
    shr_driver_ready(ctx);
    shr_pump(ctx);
    ASSERT(a.buf.id == 2 && ctx->slots[1].buf == &a.buf && ctx->nreleased == 0 && h.out.presents == 3);
    ASSERT_EQ_LL(ctx->slots[1].releasing, false);
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    draw_fakes(l, 1, (fake *[]){&a});
    frame(ctx);
    ASSERT_EQ_LL(rec.npro, 0); /* no stale RELEASE of the id a holds */
    /* With every id the frame's: the buffer left without one is not drawn, then drawn by the next frame by itself. */
    fake d;
    fake_attach(&d, ctx, &fk_ops);
    draw_fakes(l, 3, (fake *[]){&a, &c, &d});
    int logs = rec.logs;
    shr_request_redraw(ctx);
    frame(ctx);
    ASSERT(count_events(ctx, SHR_EVENT_PRESENT_FAILED, NULL) == 0 && h.out.presents == 5 && rec.logs == logs + 1);
    ASSERT(rec.short_bufs == 1 && d.buf.id == 0 && px(h.out.shown, 9, 1) == WHITE && px(h.out.shown, 17, 1) != WHITE);
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
    shr_pump(ctx);
    ASSERT(h.out.presents == 6 && rec.short_bufs == 0 && d.buf.id != 0 && rec.damaged == 8 * 16);
    ASSERT(px(h.out.shown, 1, 1) == WHITE && px(h.out.shown, 9, 1) == WHITE && px(h.out.shown, 17, 1) == WHITE);
    ASSERT_EQ_LL(rec.logs, logs + 1);
    destroy_layers(&l, 1);
    a.res.dead = c.res.dead = d.res.dead = true;
    harness_close(&h);
    PASS();
}

/* A buffer that gets no id draws its resource's fallback in its place: the fallback's pixels, nothing when there are
 * none (SHR_E_NOT_FOUND), or the frame fails with the fallback's error. */
TEST test_fallback_when_ids_run_short(void) {
    const shr_status fb[3] = {SHR_OK, SHR_E_NOT_FOUND, SHR_E_LIMIT};
    limit_ids = 2, limit_flags = SHR_BUFFER_COPIES, limit_bytes = 0;
    for (int i = 0; i < 3; i++) {
        harness h;
        shr_context *ctx = harness_open(&h, PRESERVED, tweak_limits);
        fake a, b, c;
        fake_attach(&a, ctx, &fk_ops), fake_attach(&b, ctx, &fk_ops), fake_attach(&c, ctx, &fb_ops);
        shr__resolved spare = a.px; /* a's pixels, provisional as fallbacks are */
        spare.provisional = true;
        c.spare = &spare, c.fb_st = fb[i];
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        draw_fakes(l, 3, (fake *[]){&a, &b, &c});
        frame(ctx);
        shr_event ev;
        ASSERT_EQ_LL(count_events(ctx, SHR_EVENT_PRESENT_FAILED, &ev), i == 2);
        if (i == 2) {
            ASSERT_EQ_LL(ev.status, SHR_E_LIMIT);
        } else {
            ASSERT(h.out.presents == 1 && rec.short_bufs == 1 && c.buf.id == 0 && px(h.out.shown, 9, 1) == WHITE);
            ASSERT_EQ_LL(px(h.out.shown, 17, 1) == WHITE, i == 0);
        }
        destroy_layers(&l, 1);
        a.res.dead = b.res.dead = c.res.dead = true;
        harness_close(&h);
    }
    PASS();
}

/* What got no id is drawn again at once while each frame leaves fewer buffers out: 3, then 1, then none. */
TEST test_short_ids_redrawn_while_fewer_go_without(void) {
    limit_ids = 2, limit_flags = SHR_BUFFER_COPIES, limit_bytes = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_limits);
    fake f[5];
    for (int i = 0; i < 5; i++) fake_attach(&f[i], ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    draw_fakes(l, 5, (fake *[]){&f[0], &f[1], &f[2], &f[3], &f[4]});
    frame(ctx);
    ASSERT(rec.short_bufs == 3 && px(h.out.shown, 9, 1) == WHITE && px(h.out.shown, 17, 1) != WHITE);
    const uint64_t left[2] = {1, 0};
    for (int k = 0; k < 2; k++) {
        ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NOW);
        shr_pump(ctx);
        ASSERT_EQ_LL(rec.short_bufs, left[k]);
    }
    ASSERT_EQ_LL(deadline(ctx), SHR_DEADLINE_NONE);
    ASSERT_EQ_LL(h.out.presents, 3);
    for (int i = 0; i < 5; i++) ASSERT_EQ_LL(px(h.out.shown, 8 * i + 1, 1), WHITE);
    destroy_layers(&l, 1);
    for (int i = 0; i < 5; i++) f[i].res.dead = true;
    harness_close(&h);
    PASS();
}

static void limited(long *budget, shr_status (*call)(shr_context *), shr_context *ctx) {
    oom.budget = *budget;
    call(ctx);
    *budget = oom.budget;
    oom.budget = -1;
}

static void tweak_limits_oom(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_oom(d, drv);
    drv->caps.max_buffers = limit_ids;
}

/* Every allocation of the buffer plan may fail (also while the plan grows past its earlier size, evicting or
 * releasing): the frame fails and the registry stays consistent. */
TEST test_buffer_plan_out_of_memory(void) {
    static fake fk[25];
    limit_ids = 20;
    for (long budget = 0;; budget++) {
        harness h;
        shr_context *ctx = harness_open(&h, PRESERVED, tweak_limits_oom);
        for (int i = 0; i < 25; i++) fake_attach(&fk[i], ctx, &fk_ops);
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        long left = budget;
        /* 20 ids taken 4 at a time, 5 more evict as many, then all are freed and released at once */
        for (int step = 0; step < 8; step++) {
            shr__lcmd c[5] = {fill((shr_rect){0, 0, 4, 4}, (shr_color)step)};
            int n = step < 6 ? 4 + (step == 5) : 0;
            for (int i = 0; i < n; i++) c[i] = glyph(&fk[step * 4 + i], i * 8, 0, WHITE);
            ASSERT_EQ_LL(shr__lyr_group_set(l, 0, c, n ? (size_t)n : 1), SHR_OK);
            for (int i = 0; step == 6 && i < 25; i++) fk[i].res.dead = true;
            limited(&left, shr_submit, ctx);
            limited(&left, shr_pump, ctx);
        }
        uint32_t releasing = 0;
        uint64_t bytes = 0;
        for (uint32_t k = 0; k < limit_ids; k++) {
            const shr__slot *s = &ctx->slots[k];
            releasing += s->releasing, bytes += s->bytes;
            ASSERT(!s->buf || s->buf->id == k + 1);
        }
        ASSERT(releasing == ctx->nreleased && bytes == ctx->resident);
        destroy_layers(&l, 1);
        harness_close(&h);
        ASSERT_EQ_LL(oom.live, 0);
        if (left) {
            ASSERT_EQ_LL(h.drv.releases, 25);
            break;
        }
    }
    PASS();
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

/* A row group's LINE commands may find no memory to be listed: the frame fails with SHR_E_NO_MEMORY and nothing
 * leaks; with the memory there the lines are drawn. */
TEST test_lines_out_of_memory(void) {
    for (long budget = 0;; budget++) {
        harness h;
        shr_context *ctx = harness_open(&h, PRESERVED, tweak_oom);
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        shr__rcmd c[16]; /* more than the first command list holds */
        for (int i = 0; i < 16; i++)
            c[i] = (shr__rcmd){(uint16_t)(i % 8), (uint16_t)(i % 8 + 1), (uint8_t)(i / 8 * 4), (uint8_t)(i / 8 * 4 + 1),
                               SHR__LCMD_LINE, 0, RED, SHR_LINE_SINGLE, 0};
        ASSERT_EQ_LL(rows_set(l, 0, 0, NULL, c, 16), SHR_OK);
        long left = budget;
        limited(&left, shr_submit, ctx);
        limited(&left, shr_pump, ctx);
        int presents = h.out.presents;
        bool drawn = px(h.out.shown, 0, 0) == RED && px(h.out.shown, HW - 1, 4) == RED && px(h.out.shown, 0, 1) == 0;
        shr_event ev;
        while (shr_poll_event(ctx, &ev) == SHR_OK)
            if (ev.kind == SHR_EVENT_PRESENT_FAILED) ASSERT_EQ_LL(ev.status, SHR_E_NO_MEMORY);
        destroy_layers(&l, 1);
        harness_close(&h);
        ASSERT_EQ_LL(oom.live, 0);
        if (left) {
            ASSERT(presents == 1 && drawn);
            break;
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
    c[0] = fill((shr_rect){0, 0, HW - 4, HH}, BLUE); /* most of the screen: redrawn whole */
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
    d->blink = (shr_blink_profile){1ull << 63, 0, true};
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
    const shr_driver_caps caps[2] = {{.domains = SHR_MEMORY_CPU, .stride_align = 1000},
                                     {.domains = SHR_MEMORY_CPU, .max_width = HW - 1}};
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
    ASSERT_EQ_LL(shr__lyr_groups_clear(NULL, 0), SHR_E_INVALID_ARG);
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

#define GROUP_BYTES(n) ((n) * sizeof(shr__rcmd) + ((n) + SHR__BLOCK - 1) / SHR__BLOCK * sizeof(shr_rect))

/* Group memory comes in three classes up to the most commands asked for: idle memory of the group's class first, else
 * new memory while the layer holds less than its peak + 1 largest groups, past that idle memory of a larger class, else
 * new memory after freeing idle smaller memory. More commands than the largest class grow the classes by a quarter at
 * least; memory of a size no class has any more is freed when it comes back. */
TEST test_group_memory_classes(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_oom);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__rcmd c[130];
    for (int i = 0; i < 130; i++)
        c[i] = (shr__rcmd){(uint16_t)(i % 8), (uint16_t)(i % 8 + 1), (uint8_t)(i / 8), (uint8_t)(i / 8 + 1),
                           SHR__LCMD_FILL, 0, (shr_color)(i + 1), 0, 0};
    shr__rcmd *a = shr__lyr_row_begin(l, 0); /* at least one command: classes of 16 */
    ASSERT(a && l->class_cap[0] == 16 && l->class_cap[2] == 16);
    ASSERT_EQ_LL(shr__lyr_groups_clear(l, 10), SHR_OK); /* fewer than 16: all classes alike */
    ASSERT(l->class_cap[0] == 10 && l->class_cap[1] == 10 && l->class_cap[2] == 10 && l->held == 0);
    ASSERT_EQ_LL(shr__lyr_groups_clear(l, 64), SHR_OK);
    ASSERT(l->class_cap[0] == 16 && l->class_cap[1] == 32 && l->class_cap[2] == 64);

    /* No group yet: room for one largest. */
    a = shr__lyr_row_begin(l, 10);
    oom.budget = 0;
    ASSERT(shr__lyr_row_begin(l, 16) == a && l->spare_cap == 16); /* the lent memory again */
    oom.budget = 1;
    shr__rcmd *b = shr__lyr_row_begin(l, 20); /* a goes idle */
    ASSERT(b && l->spare_cap == 32 && l->spares[0] == a);
    ASSERT_EQ_LL(oom.budget, 0);
    oom.budget = 1;
    shr__rcmd *d = shr__lyr_row_begin(l, 50); /* over the limit with b idle too: both freed */
    ASSERT(d && l->spare_cap == 64 && !l->spares[0] && !l->spares[1] && l->held == GROUP_BYTES(64));
    ASSERT_EQ_LL(oom.budget, 0);
    ASSERT(shr__lyr_row_begin(l, 10) == d && l->spare_cap == 64); /* borrowed: no room for new memory */
    oom.budget = -1;
    ASSERT_EQ_LL(rows_set(l, 0, 0, NULL, c, 10), SHR_OK); /* only the group list allocates */
    ASSERT(SHR_VEC_AT(&l->groups, shr__group, 0)->rows == d && l->held == GROUP_BYTES(64));

    /* Peak 1, then 2: new memory of each class; then idle memory of the class. */
    ASSERT_EQ_LL(rows_set(l, 1, 0, NULL, c, 20), SHR_OK);
    ASSERT_EQ_LL(rows_set(l, 0, 0, NULL, c + 1, 10), SHR_OK);
    ASSERT(SHR_VEC_AT(&l->groups, shr__group, 0)->cap == 16 && SHR_VEC_AT(&l->groups, shr__group, 1)->cap == 32);
    ASSERT(l->spares[2] == d && l->held == GROUP_BYTES(16) + GROUP_BYTES(32) + GROUP_BYTES(64));
    oom.budget = 0;
    ASSERT_EQ_LL(rows_set(l, 2, 0, NULL, c, 50), SHR_OK);
    ASSERT_EQ_LL(SHR_VEC_AT(&l->groups, shr__group, 2)->rows, d);
    oom.budget = -1;

    /* Rebuilt groups that change class allocate nothing once each class had its most. */
    static const size_t sizes[3] = {10, 20, 50};
    for (int pass = 0; pass < 9; pass++) {
        if (pass == 3) oom.budget = 0;
        for (uint32_t g = 0; g < 3; g++) ASSERT_EQ_LL(rows_set(l, g, 0, NULL, c + pass % 2, sizes[(g + pass) % 3]), SHR_OK);
    }
    oom.budget = -1;
    ASSERT(l->held <= 4 * GROUP_BYTES(64));

    /* Growth: by a quarter, then to what was asked; memory of old sizes is freed when it comes back. */
    ASSERT_EQ_LL(rows_set(l, 0, 0, NULL, c, 10), SHR_OK);
    ASSERT_EQ_LL(rows_set(l, 1, 0, NULL, c, 20), SHR_OK);
    ASSERT_EQ_LL(rows_set(l, 2, 0, NULL, c, 50), SHR_OK);
    ASSERT_EQ_LL(rows_set(l, 3, 0, NULL, c, 70), SHR_OK);
    ASSERT(l->class_cap[0] == 16 && l->class_cap[1] == 48 && l->class_cap[2] == 80);
    ASSERT(!l->spares[0] && !l->spares[1] && !l->spares[2]);
    long live = oom.live;
    ASSERT_EQ_LL(rows_set(l, 0, 0, NULL, c + 1, 10), SHR_OK); /* 16 still a class: kept */
    ASSERT_EQ_LL(oom.live, live + 1);
    ASSERT_EQ_LL(rows_set(l, 1, 0, NULL, c + 1, 20), SHR_OK); /* 32 is not: freed */
    ASSERT_EQ_LL(rows_set(l, 2, 0, NULL, c + 1, 50), SHR_OK); /* nor 64 */
    ASSERT_EQ_LL(oom.live, live + 1);
    ASSERT_EQ_LL(rows_set(l, 3, 0, NULL, c, 120), SHR_OK);
    ASSERT_EQ_LL(l->class_cap[2], 128);
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
    shr__lcmd two[2] = {c[0], c[2]}; /* equal ends match: only the removed fill changed */
    paint(l, 2, two);
    frame(ctx);
    ASSERT(rec.damaged == 64 && px(h.out.shown, 0, 0) == BLUE && px(h.out.shown, 16, 16) == GREEN);
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

/* Changes of one group in place are damaged in GROUP_RUNS (8) separate rects, then the rest of the middle as one. */
TEST test_group_damage_runs(void) {
    harness h;
    shr_context *ctx = harness_open(&h, 0, NULL);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd c[20];
    for (int i = 0; i < 20; i++) c[i] = fill((shr_rect){i * 3, 0, i * 3 + 1, 1}, RED);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, c, 20), SHR_OK);
    ctx->staged.rects.len = 0;
    for (int i = 0; i < 20; i++) c[i].color = GREEN;
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, c, 20), SHR_OK);
    ASSERT_EQ_LL(ctx->staged.rects.len, 9);
    for (int i = 0; i < 8; i++) ASSERT_EQ_LL(SHR_VEC_AT(&ctx->staged.rects, shr_rect, i)->x0, i * 3);
    const shr_rect *rest = SHR_VEC_AT(&ctx->staged.rects, shr_rect, 8);
    ASSERT(rest->x0 == 24 && rest->x1 == 58 && rest->y0 == 0 && rest->y1 == 1);
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
    v[5].color = RED, v[6].res = &r2.res, v[7].id = 2, v[8].bg = RED, v[9].dst.y0 = 1;
    for (int i = 0; i < 11; i++) {
        ASSERT_EQ_LL(shr__lyr_group_set(l, 0, &base, 1), SHR_OK);
        ctx->staged.rects.len = 0;
        ASSERT_EQ_LL(shr__lyr_group_set(l, 0, &v[i], 1), SHR_OK);
        ASSERT_EQ_LL(ctx->staged.rects.len > 0, i < 10);
    }
    /* Other kinds compare only what they draw with. */
    const shr__lcmd fb = fill(FULL, WHITE), begin = {.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, 8, 16}, .key = {1, 2}},
                    end = {.kind = SHR__LCMD_CACHE_END};
    shr__lcmd f1 = fb, f2 = fb, k1[3] = {begin, fb, end}, k2[3] = {begin, fb, end}, k3[3] = {begin, fb, end};
    f1.dst.x0 = 1, f2.id = 9, k1[0].key[1] = 3, k2[0].key[0] = 3, k3[2].id = 9;
    const struct {
        const shr__lcmd *a, *b;
        size_t n;
        bool replaced;
    } kinds[] = {{&fb, &f1, 1, true}, {&fb, &f2, 1, false}, {(shr__lcmd[]){begin, fb, end}, k1, 3, true},
                 {(shr__lcmd[]){begin, fb, end}, k2, 3, true}, {(shr__lcmd[]){begin, fb, end}, k3, 3, false}};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        ASSERT_EQ_LL(shr__lyr_group_set(l, 0, kinds[i].a, kinds[i].n), SHR_OK);
        const shr__lcmd *was = SHR_VEC_AT(&l->groups, shr__group, 0)->cmds;
        ASSERT_EQ_LL(shr__lyr_group_set(l, 0, kinds[i].b, kinds[i].n), SHR_OK);
        ASSERT_EQ_LL(SHR_VEC_AT(&l->groups, shr__group, 0)->cmds != was, kinds[i].replaced);
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

TEST test_group_resource_counts(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    fake r, r2;
    fake_attach(&r, ctx, &fk_ops);
    fake_attach(&r2, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr__lcmd one[3] = {glyph(&r, 0, 0, WHITE), fill((shr_rect){0, 0, 4, 4}, RED), glyph(&r, 8, 0, WHITE)};
    const shr__lcmd mixed[3] = {glyph(&r, 0, 0, WHITE), glyph(&r2, 8, 0, WHITE), glyph(&r, 16, 0, WHITE)};
    const shr__lcmd plain = fill((shr_rect){40, 40, 44, 44}, RED);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, one, 3), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 1, mixed, 3), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 2, &plain, 1), SHR_OK);
    ASSERT(r.res.users == 4 && r2.res.users == 1);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, mixed, 3), SHR_OK);
    ASSERT(r.res.users == 4 && r2.res.users == 2);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 1, one, 3), SHR_OK);
    ASSERT(r.res.users == 4 && r2.res.users == 1);
    frame(ctx);
    shr__res_changed(&r2.res, (shr_rect){0, 0, 8, 16}); /* only group 0 refers to it */
    frame(ctx);
    ASSERT_EQ_LL(rec.damaged, 8 * 16);
    shr__rcmd *c = shr__lyr_row_begin(l, 1);
    ASSERT(c != NULL);
    *c = row_of(glyph(&r2, 24, 0, WHITE));
    ASSERT_EQ_LL(shr__lyr_row_commit(l, 3, 0, &r2.res, nokey, c, 1), SHR_OK);
    ASSERT_EQ_LL(r2.res.users, 2);
    *(c = shr__lyr_row_begin(l, 1)) = row_of(glyph(&r2, 24, 0, WHITE));
    ASSERT_EQ_LL(shr__lyr_row_commit(l, 3, 0, &r2.res, nokey, c, 1), SHR_OK); /* unchanged: freed */
    *(c = shr__lyr_row_begin(l, 1)) = row_of(glyph(&r2, 24, 0, WHITE));
    ASSERT_EQ_LL(shr__lyr_row_commit(l, 3, 0, NULL, nokey, c, 1), SHR_E_INVALID_ARG); /* a glyph without resource */
    ASSERT_EQ_LL(shr__lyr_row_commit(l, 3, 0, NULL, nokey, shr__lyr_row_begin(l, 0), 0), SHR_OK);
    ASSERT(r.res.users == 4 && r2.res.users == 1);
    ASSERT_EQ_LL(shr__lyr_groups_clear(l, 0), SHR_OK);
    ASSERT(r.res.users == 0 && r2.res.users == 0);
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
    ASSERT_EQ_LL(shr__lyr_groups_clear(l, 0), SHR_OK);
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
    flagged.flags = 1u << 4, no_img.kind = SHR__LCMD_IMAGE;
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
    flagged.flags = SHR__LCMD_DIM | SHR__LCMD_BOLD | SHR__LCMD_ITALIC | SHR__LCMD_ON_FILL | SHR__LCMD_BLINK;
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
    fake r; /* glyphs placed past INT32_MAX, which 32 bits would wrap onto the screen, stay away */
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *g;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 1, FULL, &g), SHR_OK);
    shr__lcmd far_glyphs[2] = {glyph(&r, 0, 0, RED), glyph(&r, 0, 0, RED)};
    far_glyphs[0].anchor.x = INT32_MAX, far_glyphs[1].anchor.y = INT32_MIN;
    r.px.offset = (shr_point){0, -1};
    ASSERT_EQ_LL(shr_lyr_cmd_begin(g), SHR_OK);
    for (int i = 0; i < 2; i++) ASSERT_EQ_LL(shr__lyr_cmd_add(g, &far_glyphs[i]), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(g), SHR_OK);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    settle(ctx);
    ASSERT(r.resolves >= 2 && px(h.out.shown, 0, 0) == 0);
    destroy_layers(&g, 1);
    destroy_layers(l, 2);
    r.res.dead = true;
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
    ASSERT_EQ_LL(shr__lyr_groups_clear(l, 0), SHR_OK);
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
        uint16_t flags;
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
    ASSERT_EQ_LL(shr__lyr_groups_clear(top, 0), SHR_OK);
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
    const int32_t bx = SHR__BLOCK % (HW / 4) * 4, by = SHR__BLOCK / (HW / 4) * 4;
    cells[SHR__BLOCK].color = GREEN; /* the first command of the second block (x 0, y 16 at 8x16) */
    paint(l, n, cells);
    frame(ctx);
    ASSERT(rec.n == 2 && rec.cmds[1].color == GREEN && px(h.out.shown, bx, by) == GREEN); /* clear, cell */
    ASSERT(px(h.out.shown, 0, 0) == BLUE && px(h.out.shown, 4, 0) == RED);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* ---- keeps: the mock runs batches on a software driver keeping up to keep_max ids, each in a slot of keep_slot bytes
 * (0: of its own size), and advertises keep_cap bytes and keep_slot as the largest keep ---- */

static struct {
    shr_draw_cmd cmds[1024]; /* every command of the batches since klog_reset(), prologues included */
    size_t n;
    int calls;
    int block_at; /* refuse the call with this number (1-based), 0 = none */
} klog;

static uint32_t keep_max;
static uint64_t keep_cap, keep_slot;
static fail_alloc keep_mem; /* the keep driver's allocations */
static shr_allocator keep_al;

static shr_status klog_execute(void *user, const shr_surface *dst, const shr_draw_cmd *c, size_t n, shr_fence f) {
    if (++klog.calls == klog.block_at) return SHR_E_WOULD_BLOCK;
    for (size_t i = 0; i < n && klog.n < 1024; i++) klog.cmds[klog.n++] = c[i];
    return rec_execute(user, dst, c, n, f);
}

static void keeps_on(shr_framebuffer_driver *drv) {
    mock_driver *m = drv->user;
    keep_mem = (fail_alloc){-1, 0};
    keep_al = fail_allocator(&keep_mem);
    ASSERT_EQ_LL(shr_software_driver_create(&keep_al, keep_slot * keep_max, keep_max, HBUFS, &m->sw), SHR_OK);
    drv->caps.max_keeps = keep_max, drv->caps.keep_bytes = keep_cap, drv->caps.max_keep_bytes = keep_slot;
    drv->execute = klog_execute;
}

static void tweak_keep(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    keeps_on(drv);
}

static void tweak_keep_blink(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_blink(d, drv);
    keeps_on(drv);
}

static void tweak_keep_cmds(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_keep(d, drv);
    d->max_commands = max_cmds;
}

static void tweak_keep_oom(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_oom(d, drv);
    keeps_on(drv);
}

static void klog_reset(void) { memset(&klog, 0, sizeof(klog)); }

static int klog_count(shr_cmd_kind kind) {
    int n = 0;
    for (size_t i = 0; i < klog.n; i++) n += klog.cmds[i].kind == kind;
    return n;
}

/* The nth command of `kind` (0-based). */
static const shr_draw_cmd *klog_nth(shr_cmd_kind kind, int nth) {
    for (size_t i = 0; i < klog.n; i++)
        if (klog.cmds[i].kind == kind && !nth--) return &klog.cmds[i];
    FAIL_WITH_LONGJMPm("command not sent");
    return NULL;
}

/* A frame, after the events so far. */
static void keep_frame(shr_context *ctx) {
    drain(ctx);
    klog_reset();
    frame(ctx);
}

/* The whole screen again: what showed is seen a second time. */
static void keep_again(shr_context *ctx) {
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    keep_frame(ctx);
}

/* A frame redrawing `r` as a layer change would. */
static void keep_damage(shr_context *ctx, shr_rect r) {
    shr__damage_add(ctx, &ctx->staged, r);
    keep_frame(ctx);
}

static shr__keep_count kc(const shr_context *ctx) { return ctx->keep_count; }

#define ROW_BYTES ((uint64_t)HW * 16 * SCREEN_BPP)

/* A keep row of the layer at y: background `bg` with a glyph of `f` at x, keyed by both. */
static void keep_row(shr_lyr *l, uint32_t group, fake *f, int32_t y, shr_color bg, int32_t x, uint32_t flags) {
    uint64_t key = ((uint64_t)x << 32 | bg) + 1 + flags;
    shr__lcmd c[4] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, y, HW, y + 16}, .key = {key, ~key}},
                      fill((shr_rect){0, y, HW, y + 16}, bg), glyph(f, x, y, WHITE), {.kind = SHR__LCMD_CACHE_END}};
    c[2].flags = (uint16_t)flags;
    ASSERT_EQ_LL(shr__lyr_group_set(l, group, c, 4), SHR_OK);
}

/* What the output shows of a row drawn by keep_row(). */
static bool row_shown(const harness *h, int32_t y, shr_color bg, int32_t x, bool glyph_shown) {
    return px(h->out.shown, x + 3, y + 5) == (glyph_shown ? WHITE : bg) && px(h->out.shown, (x + 20) % HW, y + 9) == bg;
}

/* Rows are stored the second time they are drawn, then drawn from their keeps wherever their content shows, also in
 * part and several times in a frame; content is resolved only when it is stored or drawn plainly. */
TEST test_keep_rows(void) {
    keep_max = 8, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr_color bg[5] = {RED, GREEN, BLUE, 0xFF00FFu, 0x00FFFFu};
    for (int i = 0; i < 3; i++) keep_row(l, (uint32_t)i, &r, 16 * i, bg[i], 8 * i, 0);
    keep_frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT_EQ_LL(rec.npro, 9); /* the first frame releases every id; the glyph's buffer */
    for (int i = 0; i < 8; i++) ASSERT(pro_is(i, SHR_CMD_KEEP_RELEASE, (uint32_t)i + 1));
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_FILL) == 3 && kc(ctx).direct == 3);
    for (int i = 0; i < 3; i++) ASSERT(row_shown(&h, 16 * i, bg[i], 8 * i, true));
    keep_again(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 3 && klog_count(SHR_CMD_KEEP_DRAW) == 3 && klog_count(SHR_CMD_FILL) == 3);
    ASSERT(kc(ctx).stores == 3 && kc(ctx).direct == 0 && kc(ctx).hits == 0);
    for (int i = 0; i < 3; i++) {
        const shr_draw_cmd *b = klog_nth(SHR_CMD_KEEP_BEGIN, i), *d = klog_nth(SHR_CMD_KEEP_DRAW, i);
        ASSERT(b->buffer == (uint32_t)i + 1 && rect_eq(b->dst, (shr_rect){0, 16 * i, HW, 16 * i + 16}));
        ASSERT(d->buffer == b->buffer && rect_eq(d->dst, b->dst) && d->src_origin.x == 0 && d->src_origin.y == 0);
        ASSERT(row_shown(&h, 16 * i, bg[i], 8 * i, true));
    }
    /* Scrolled up a row: two rows draw the keeps of the rows below, the new one draws plainly; shown again, it is
     * stored. */
    int resolves = r.resolves;
    for (int i = 0; i < 3; i++) keep_row(l, (uint32_t)i, &r, 16 * i, bg[i + 1], 8 * (i + 1), 0);
    keep_frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(rec.npro == 0 && klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_KEEP_DRAW) == 2);
    ASSERT(klog_nth(SHR_CMD_KEEP_DRAW, 0)->buffer == 2 && klog_nth(SHR_CMD_KEEP_DRAW, 1)->buffer == 3);
    ASSERT(r.resolves == resolves + 1 && kc(ctx).hits == 2 && kc(ctx).direct == 1);
    for (int i = 0; i < 3; i++) ASSERT(row_shown(&h, 16 * i, bg[i + 1], 8 * (i + 1), true));
    keep_again(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 1 && klog_nth(SHR_CMD_KEEP_BEGIN, 0)->buffer == 4);
    /* Rows of equal content, the second time: stored once, drawn twice. */
    keep_row(l, 0, &r, 0, bg[4], 40, 0);
    keep_row(l, 1, &r, 16, bg[4], 40, 0);
    keep_frame(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && kc(ctx).direct == 2);
    ASSERT(row_shown(&h, 0, bg[4], 40, true) && row_shown(&h, 16, bg[4], 40, true));
    keep_again(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 1 && klog_count(SHR_CMD_KEEP_DRAW) == 3);
    uint32_t id = klog_nth(SHR_CMD_KEEP_BEGIN, 0)->buffer;
    ASSERT(klog_nth(SHR_CMD_KEEP_DRAW, 0)->buffer == id && klog_nth(SHR_CMD_KEEP_DRAW, 1)->buffer == id);
    /* Damage inside a row, twice: each part is drawn from the keep, nothing under the row. */
    shr_lyr *m[2];
    const shr_rect spots[2] = {{10, 2, 12, 4}, {50, 2, 52, 4}};
    for (int k = 0; k < 2; k++) {
        ASSERT_EQ_LL(shr_lyr_create(ctx, 1, spots[k], &m[k]), SHR_OK);
        shr__lcmd dim = fill((shr_rect){0, 0, 2, 2}, BLUE);
        dim.flags = SHR__LCMD_DIM;
        paint(m[k], 1, &dim);
    }
    keep_frame(ctx);
    for (int k = 0; k < 2; k++) {
        shr__lcmd dim = fill((shr_rect){0, 0, 2, 2}, k ? RED : GREEN);
        dim.flags = SHR__LCMD_DIM;
        paint(m[k], 1, &dim);
    }
    resolves = r.resolves;
    keep_frame(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_DRAW) == 2 && klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_FILL) == 2);
    for (int k = 0; k < 2; k++) {
        const shr_draw_cmd *d = klog_nth(SHR_CMD_KEEP_DRAW, k);
        ASSERT(d->buffer == id && rect_eq(d->dst, spots[k]) && d->src_origin.x == spots[k].x0 && d->src_origin.y == 2);
    }
    ASSERT(r.resolves == resolves && row_shown(&h, 0, bg[4], 40, true));
    destroy_layers(m, 2);
    keep_frame(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_DRAW) == 2 && klog_count(SHR_CMD_FILL) == 0);
    /* Fallback pixels are not kept: the row draws plainly, then, once final and seen before, it is stored though only
     * its glyph is redrawn. */
    r.px.provisional = true;
    keep_row(l, 2, &r, 32, bg[0], 16, 0);
    keep_frame(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_KEEP_DRAW) == 0 && klog_count(SHR_CMD_GLYPH) == 1);
    ASSERT(row_shown(&h, 32, bg[0], 16, true));
    keep_again(ctx); /* seen, but provisional */
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_GLYPH) == 1 && kc(ctx).direct == 1);
    r.px.provisional = false, r.changed = true;
    klog_reset();
    shr_pump(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 1 && klog_count(SHR_CMD_GLYPH) == 1 && klog_count(SHR_CMD_KEEP_DRAW) == 1);
    ASSERT(rect_eq(klog_nth(SHR_CMD_KEEP_DRAW, 0)->dst, (shr_rect){16, 32, 24, 48}) && row_shown(&h, 32, bg[0], 16, true));
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    klog_reset();
    shr_pump(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_KEEP_DRAW) == 3);
    /* Rows outside their layer or the screen draw plainly. */
    for (int k = 0; k < 2; k++) {
        ASSERT_EQ_LL(shr_lyr_set_rect(l, k ? (shr_rect){8, 0, 72, 48} : (shr_rect){0, 0, 56, 48}), SHR_OK);
        keep_frame(ctx);
        ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_KEEP_DRAW) == 0 && klog_count(SHR_CMD_FILL) >= 3);
    }
    /* A keep group outside the damage is left out with its end. */
    ASSERT_EQ_LL(shr_lyr_set_rect(l, FULL), SHR_OK);
    shr__lcmd far[4] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, 4, 4}, .key = {9, 9}}, fill((shr_rect){0, 0, 4, 4}, BLUE),
                        {.kind = SHR__LCMD_CACHE_END}, fill((shr_rect){10, 10, 14, 14}, RED)};
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, far, 4), SHR_OK);
    keep_frame(ctx);
    far[3].color = GREEN;
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, far, 4), SHR_OK);
    keep_frame(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_KEEP_DRAW) == 0 && klog_count(SHR_CMD_FILL) == 2);
    ASSERT_EQ_LL(px(h.out.shown, 12, 12), GREEN);
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

/* A keep group drawn from its keep is passed over to its end, in a group of two keep groups with commands before,
 * between and after them: those still draw, in order. */
TEST test_keep_hit_skips_to_its_end(void) {
    keep_max = 8, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr_rect a = {0, 0, HW, 16}, b = {0, 16, HW, 32};
    const shr__lcmd c[11] = {fill((shr_rect){0, 32, 8, 40}, BLUE),
                             {.kind = SHR__LCMD_CACHE_BEGIN, .dst = a, .key = {3, 4}},
                             fill(a, RED),
                             fill((shr_rect){0, 0, 8, 8}, GREEN),
                             fill((shr_rect){8, 0, 16, 8}, BLUE),
                             {.kind = SHR__LCMD_CACHE_END},
                             fill((shr_rect){0, 40, 8, 48}, GREEN),
                             {.kind = SHR__LCMD_CACHE_BEGIN, .dst = b, .key = {5, 6}},
                             fill(b, GREEN),
                             {.kind = SHR__LCMD_CACHE_END},
                             fill((shr_rect){4, 20, 12, 28}, RED)};
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, c, 11), SHR_OK);
    keep_frame(ctx);
    keep_again(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 2);
    keep_again(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_KEEP_DRAW) == 2 && klog_count(SHR_CMD_FILL) == 4);
    ASSERT(px(h.out.shown, 2, 2) == GREEN && px(h.out.shown, 10, 2) == BLUE && px(h.out.shown, 20, 2) == RED);
    ASSERT(px(h.out.shown, 2, 34) == BLUE && px(h.out.shown, 2, 42) == GREEN);
    ASSERT(px(h.out.shown, 6, 22) == RED && px(h.out.shown, 20, 22) == GREEN);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* Out of ids or bytes, a row replaces the least recently used keep the frame does not draw; when every keep is drawn
 * by the frame (a refused store), or one row alone exceeds the bytes or the driver's largest keep, the row draws plainly.
 * A keep replaced before a later frame drew it is a dead store. */
TEST test_keep_eviction(void) {
    static const struct {
        uint32_t max;
        uint64_t cap, slot;
    } modes[] = {{2, 0, 0}, {4, 2 * ROW_BYTES, 0}, {4, ROW_BYTES - 1, 0}, {4, 0, ROW_BYTES - 128}, {2, 0, ROW_BYTES}};
    for (int mode = 0; mode < 5; mode++) {
        keep_max = modes[mode].max, keep_cap = modes[mode].cap, keep_slot = modes[mode].slot;
        bool ids = keep_max == 2, none = mode == 2 || mode == 3; /* out of ids; no row fits */
        harness h;
        shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep);
        fake r;
        fake_attach(&r, ctx, &fk_ops);
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        for (int i = 0; i < 3; i++) keep_row(l, (uint32_t)i, &r, 16 * i, i ? GREEN : RED, 8 * i, 0);
        keep_frame(ctx);
        expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
        keep_again(ctx);
        expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
        int stored = none ? 0 : 2;
        ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == stored && klog_count(SHR_CMD_KEEP_DRAW) == stored);
        ASSERT(kc(ctx).stores == (uint32_t)stored && kc(ctx).refused == (mode < 2 || mode == 4) && kc(ctx).direct == 3u - stored);
        keep_again(ctx); /* every keep drawn: nothing to evict */
        ASSERT(rec.npro == 0 && klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_KEEP_DRAW) == stored);
        keep_row(l, ids ? 0 : 2, &r, ids ? 0 : 32, BLUE, 24, 0);
        keep_frame(ctx);
        ASSERT_EQ_LL(klog_count(SHR_CMD_KEEP_BEGIN), 0);
        keep_damage(ctx, (shr_rect){0, ids ? 0 : 32, 8, ids ? 16 : 48}); /* seen: stored, though damaged in part */
        expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
        if (!none) { /* the oldest keep, row 0's, is replaced */
            ASSERT(rec.npro == 0 && klog_nth(SHR_CMD_KEEP_BEGIN, 0)->buffer == 1);
        } else {
            ASSERT(rec.npro == 0 && klog_count(SHR_CMD_KEEP_BEGIN) == 0);
        }
        ASSERT(row_shown(&h, ids ? 0 : 32, BLUE, 24, true));
        if (ids) { /* content evicted earlier in the frame is not drawn from its keep: stored again */
            keep_row(l, 0, &r, 0, 0xFFFF00u, 48, 0);
            keep_frame(ctx);
            keep_again(ctx);
            expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
            ASSERT(rec.npro == 0 && klog_count(SHR_CMD_KEEP_BEGIN) == 2 && kc(ctx).dead == 1);
            ASSERT(klog_nth(SHR_CMD_KEEP_BEGIN, 0)->buffer == 2 && klog_nth(SHR_CMD_KEEP_BEGIN, 1)->buffer == 1);
            ASSERT(row_shown(&h, 0, 0xFFFF00u, 48, true) && row_shown(&h, 16, GREEN, 8, true));
        }
        destroy_layers(&l, 1);
        r.res.dead = true;
        harness_close(&h);
    }
    PASS();
}

/* A row the bytes of the keeps the frame does not draw cannot take, or whose glyphs turn out provisional, leaves the
 * keep it would replace held. */
TEST test_keep_replacement(void) {
    keep_max = 4, keep_cap = 2 * ROW_BYTES, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    keep_row(l, 0, &r, 0, RED, 0, 0);
    keep_row(l, 1, &r, 16, GREEN, 8, 0);
    keep_frame(ctx);
    keep_again(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    /* Row 0 is drawn from its keep; two rows high, the new row, once seen, would need row 1's keep and row 0's. */
    const shr_rect bars[2] = {{0, 16, HW, 32}, {0, 16, HW, 48}};
    const shr__lcmd plain = fill(bars[0], BLUE),
                    tall[3] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = bars[1], .key = {7, 7}}, fill(bars[1], RED),
                               {.kind = SHR__LCMD_CACHE_END}};
    ASSERT_EQ_LL(shr__lyr_group_set(l, 1, &plain, 1), SHR_OK);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 2, tall, 3), SHR_OK);
    keep_frame(ctx);
    keep_again(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(rec.npro == 0 && klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_KEEP_DRAW) == 1);
    ASSERT_EQ_LL(kc(ctx).refused, 1);
    ASSERT(px(h.out.shown, 4, 20) == RED && row_shown(&h, 0, RED, 0, true));
    ASSERT_EQ_LL(shr__lyr_group_set(l, 2, NULL, 0), SHR_OK);
    keep_row(l, 1, &r, 16, GREEN, 8, 0);
    keep_frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_nth(SHR_CMD_KEEP_DRAW, 0)->buffer == 2);
    ASSERT(row_shown(&h, 16, GREEN, 8, true) && ctx->keep_resident == 2 * ROW_BYTES);
    /* Row 0's new glyph is provisional: its keep stays. */
    r.px.provisional = true;
    keep_row(l, 0, &r, 0, BLUE, 24, 0);
    keep_frame(ctx);
    keep_damage(ctx, (shr_rect){0, 0, HW, 16});
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_GLYPH) == 1 && row_shown(&h, 0, BLUE, 24, true));
    ASSERT(kc(ctx).direct == 1 && kc(ctx).stores == 0);
    r.px.provisional = false;
    keep_row(l, 0, &r, 0, RED, 0, 0);
    keep_frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_nth(SHR_CMD_KEEP_DRAW, 0)->buffer == 1);
    ASSERT(row_shown(&h, 0, RED, 0, true) && ctx->keep_resident == 2 * ROW_BYTES);
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

/* Content the blink phase hides is left out of a row's keep: each phase keeps its own. */
TEST test_keep_blink_phases(void) {
    keep_max = 4, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_blink);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    /* Blinking glyphs at both ends: a phase change damages the whole row. */
    shr__lcmd row[6] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, HW, 16}, .key = {5, 6}}, fill((shr_rect){0, 0, HW, 16}, RED),
                        glyph(&r, 0, 0, WHITE), glyph(&r, 24, 0, WHITE), glyph(&r, HW - 8, 0, WHITE),
                        {.kind = SHR__LCMD_CACHE_END}};
    row[2].flags = row[4].flags = SHR__LCMD_BLINK;
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, row, 6), SHR_OK);
    keep_frame(ctx);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_GLYPH) == 3 && row_shown(&h, 0, RED, 0, true));
    for (int k = 1; k <= 5; k++) { /* each phase is stored when it shows again */
        fake_now = (uint64_t)(100 * k + 50) * MS;
        klog_reset();
        shr_pump(ctx);
        bool shown = !(k & 1), stored = k == 2 || k == 3;
        ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == stored && klog_count(SHR_CMD_KEEP_DRAW) == (k >= 2));
        ASSERT_EQ_LL(klog_count(SHR_CMD_GLYPH), k < 2 || stored ? (shown ? 3 : 1) : 0);
        ASSERT(row_shown(&h, 0, RED, 0, shown) && row_shown(&h, 0, RED, 24, true));
        if (k >= 2) ASSERT_EQ_LL(klog_nth(SHR_CMD_KEEP_DRAW, 0)->buffer, shown ? 1u : 2u);
    }
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

/* After a failed batch the keeps are released before any is drawn again; out of memory for the keeps a batch stores,
 * keeps take no more bytes than before it. */
TEST test_keep_lost_batches(void) {
    keep_max = 8, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    for (int i = 0; i < 3; i++) keep_row(l, (uint32_t)i, &r, 16 * i, i ? GREEN : RED, 8 * i, 0);
    keep_frame(ctx);
    keep_again(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    keep_row(l, 0, &r, 0, BLUE, 40, 0); /* seen before the failures */
    keep_frame(ctx);
    keep_row(l, 0, &r, 0, RED, 0, 0);
    keep_frame(ctx);
    /* Twice failing (the second frame stores again into an id it releases), then all four ids are released. */
    keep_row(l, 0, &r, 0, BLUE, 40, 0);
    h.drv.fail_next = 2;
    keep_frame(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_DEVICE);
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    keep_frame(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_DEVICE);
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    keep_frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT_EQ_LL(klog_count(SHR_CMD_KEEP_RELEASE), 4);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 3 && klog_count(SHR_CMD_KEEP_DRAW) == 3 && row_shown(&h, 0, BLUE, 40, true));
    /* Out of memory, a batch storing nothing leaves the bytes alone; one storing keeps lowers them to what was held. */
    h.drv.fail_status = SHR_E_NO_MEMORY, h.drv.fail_next = 1;
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    keep_frame(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_NO_MEMORY);
    ASSERT_EQ_LL(ctx->keep_budget, UINT64_MAX);
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    keep_frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT_EQ_LL(klog_count(SHR_CMD_KEEP_BEGIN), 3);
    keep_row(l, 0, &r, 0, RED, 48, 0);
    keep_frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    keep_mem.budget = 0;
    keep_damage(ctx, (shr_rect){0, 0, 8, 16});
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_NO_MEMORY);
    ASSERT_EQ_LL(ctx->keep_budget, 3 * ROW_BYTES);
    keep_mem.budget = -1;
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    keep_frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    keep_row(l, 0, &r, 0, BLUE, 48, 0);
    keep_frame(ctx);
    keep_damage(ctx, (shr_rect){0, 0, 8, 16});
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(rec.npro == 0 && klog_count(SHR_CMD_KEEP_BEGIN) == 1 && klog_nth(SHR_CMD_KEEP_BEGIN, 0)->buffer <= 3);
    ASSERT_EQ_LL(ctx->keep_resident, 3 * ROW_BYTES);
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

/* Bands 12 rows high cut the rows: a band stores the row it covers to the end of the band, later bands draw the rest
 * of it. A row is held once the batch storing it was accepted: a frame dropped before that stores it again. */
TEST test_keep_bands(void) {
    keep_max = 8, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep);
    shr_surface b[2] = {band_surface(0, SHR_MEMORY_CPU), band_surface(1, SHR_MEMORY_CPU)};
    b[0].height = b[1].height = 12;
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_OK);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const shr_color bg[3] = {RED, GREEN, BLUE};
    for (int i = 0; i < 3; i++) keep_row(l, (uint32_t)i, &r, 16 * i, bg[i], 8 * i, 0);
    keep_frame(ctx);
    ASSERT_EQ_LL(klog_count(SHR_CMD_KEEP_BEGIN), 0); /* a row seen in a band is not seen again in the next one */
    ASSERT(kc(ctx).direct == 6 && kc(ctx).hits == 0 && kc(ctx).stores == 0);
    keep_again(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 3 && klog_count(SHR_CMD_KEEP_DRAW) == 6 && klog.calls == 8);
    ASSERT(kc(ctx).stores == 3 && kc(ctx).hits == 3 && kc(ctx).direct == 0); /* counted over all bands */
    const shr_draw_cmd *b1 = klog_nth(SHR_CMD_KEEP_BEGIN, 1), *d1 = klog_nth(SHR_CMD_KEEP_DRAW, 1);
    ASSERT(rect_eq(b1->dst, (shr_rect){0, 4, HW, 20}) && b1->buffer == 2); /* row 1 in band 1, reaching past it */
    ASSERT(d1->buffer == 1 && rect_eq(d1->dst, (shr_rect){0, 0, HW, 4}) && d1->src_origin.y == 12);
    for (int i = 0; i < 3; i++) ASSERT(row_shown(&h, 16 * i, bg[i], 8 * i, true));
    /* Refused at band 1 while a newer state waits: row 1's store never ran, so it is stored again. */
    for (int i = 0; i < 3; i++) keep_row(l, (uint32_t)i, &r, 16 * i, bg[(i + 1) % 3], 8 * i + 4, 0);
    keep_frame(ctx);
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    drain(ctx);
    klog_reset();
    klog.block_at = 3;
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    shr_pump(ctx);
    ASSERT(klog.calls == 3 && klog_nth(SHR_CMD_KEEP_BEGIN, 0)->buffer == 4);
    keep_row(l, 2, &r, 32, RED, 44, 0);
    klog_reset();
    frame(ctx);
    expect_event(ctx, SHR_EVENT_FRAME_SUPERSEDED);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 1 && klog_nth(SHR_CMD_KEEP_DRAW, 0)->buffer == 4); /* row 0 was held */
    ASSERT(rect_eq(klog_nth(SHR_CMD_KEEP_BEGIN, 0)->dst, (shr_rect){0, 4, HW, 20}));
    ASSERT(row_shown(&h, 0, GREEN, 4, true) && row_shown(&h, 16, BLUE, 12, true) && row_shown(&h, 32, RED, 44, true));
    /* Out of memory in band 0, which stores nothing (band 2 would): the bytes stay. */
    keep_row(l, 2, &r, 32, GREEN, 36, 0);
    keep_frame(ctx);
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    h.drv.fail_status = SHR_E_NO_MEMORY, h.drv.fail_next = 1;
    keep_frame(ctx);
    ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_NO_MEMORY);
    ASSERT_EQ_LL(ctx->keep_budget, UINT64_MAX);
    ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
    keep_frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 3 && row_shown(&h, 32, GREEN, 36, true));
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

/* A frame stores eight groups, more while their bytes fit in keep_store_bytes (here less than one group, then twelve);
 * those past it draw plainly and are stored when seen again. Keys sharing their first place in the seen table take
 * their second. */
TEST test_keep_store_budget(void) {
    keep_max = 16, keep_cap = 0, keep_slot = 0;
    size_t row;
    shr_format_row_bytes(SHR_PIXEL_FORMAT, 8, &row);
    static const uint32_t hits[2][4] = {{0, 0, 8, 16}, {0, 0, 12, 16}}, stores[2][4] = {{0, 8, 8, 0}, {0, 12, 4, 0}};
    for (int mode = 0; mode < 2; mode++) {
        harness h;
        shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep);
        ctx->keep_store_bytes = mode ? 12 * row * 16 : 1;
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        for (uint32_t i = 0; i < 16; i++) {
            shr_rect r = {8 * (int32_t)(i % 8), 16 * (int32_t)(i / 8), 8 * (int32_t)(i % 8) + 8, 16 * (int32_t)(i / 8) + 16};
            shr__lcmd c[3] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = r, .key = {(uint64_t)i << 32 | 5, 0x9E3779B9ull * (i + 1)}},
                              fill(r, i & 1 ? RED : GREEN), {.kind = SHR__LCMD_CACHE_END}};
            ASSERT_EQ_LL(shr__lyr_group_set(l, i, c, 3), SHR_OK);
        }
        for (int k = 0; k < 4; k++) {
            keep_again(ctx);
            expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
            shr__keep_count n = kc(ctx);
            ASSERT(n.hits == hits[mode][k] && n.stores == stores[mode][k] && n.direct == 16 - hits[mode][k] - stores[mode][k]);
            ASSERT(n.dead == 0 && n.refused == 0 && klog_count(SHR_CMD_KEEP_BEGIN) == (int)stores[mode][k]);
            ASSERT(px(h.out.shown, 4, 4) == GREEN && px(h.out.shown, 12, 20) == RED);
        }
        destroy_layers(&l, 1);
        harness_close(&h);
    }
    PASS();
}

/* ---- stores on credit: drivers with SHR_DRIVER_CHEAP_STORE ---- */

static void tweak_keep_cheap(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_keep(d, drv);
    drv->caps.flags |= SHR_DRIVER_CHEAP_STORE;
}

static const shr_color cell_colors[6] = {RED, GREEN, BLUE, 0xFF00FFu, 0x00FFFFu, 0xFFFF00u};

/* Keep group `group`: cell `cell` of 8 x 16 (8 a row) filled with a colour of `key`. */
static void keep_cell(shr_lyr *l, uint32_t group, uint32_t cell, uint64_t key) {
    int32_t x = 8 * (int32_t)(cell % 8), y = 16 * (int32_t)(cell / 8);
    shr_rect r = {x, y, x + 8, y + 16};
    shr__lcmd c[3] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = r, .key = {key << 32 | 5, 0x9E3779B9ull * (key + 1)}},
                      fill(r, cell_colors[key % 6]), {.kind = SHR__LCMD_CACHE_END}};
    ASSERT_EQ_LL(shr__lyr_group_set(l, group, c, 3), SHR_OK);
}

static bool cell_shown(const harness *h, uint32_t cell, uint64_t key) {
    return px(h->out.shown, 8 * (int32_t)(cell % 8) + 4, 16 * (int32_t)(cell / 8) + 8) == cell_colors[key % 6];
}

static bool keep_is(const shr_context *ctx, uint32_t hits, uint32_t stores, uint32_t direct, uint32_t credit) {
    shr__keep_count n = kc(ctx);
    return n.hits == hits && n.stores == stores && n.direct == direct && ctx->keep_credit == credit;
}

/* Past the stores of the store policy (here 8 a frame), and at first sight, a frame stores on credit: from 2/3 of the
 * keep ids, one each, one back at every frame and two when a later frame first draws such a keep, up to the ids.
 * Without the flag nothing changes. */
TEST test_keep_credit(void) {
    keep_max = 24, keep_cap = 0, keep_slot = 0;
    static const uint32_t off[4][3] = {{0, 0, 12}, {0, 8, 4}, {8, 4, 0}, {12, 0, 0}}, /* hits, stores, direct */
        on[4][4] = {{0, 1, 11, 0}, {1, 11, 0, 0}, {12, 0, 0, 7}, {12, 0, 0, 8}};      /* and the credit after */
    for (int mode = 0; mode < 2; mode++) {
        harness h;
        shr_context *ctx = harness_open(&h, PRESERVED, mode ? tweak_keep_cheap : tweak_keep);
        ASSERT_EQ_LL(ctx->keep_credit, 16u);
        ctx->keep_store_bytes = 1;
        ctx->keep_credit = 0;
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        for (uint32_t i = 0; i < 12; i++) keep_cell(l, i, i, i + 1);
        for (int k = 0; k < 4; k++) {
            keep_again(ctx);
            expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
            ASSERT(mode ? keep_is(ctx, on[k][0], on[k][1], on[k][2], on[k][3])
                        : keep_is(ctx, off[k][0], off[k][1], off[k][2], ctx->keep_credit));
            ASSERT(kc(ctx).dead == 0 && kc(ctx).refused == 0 && cell_shown(&h, 0, 1) && cell_shown(&h, 11, 12));
        }
        if (mode) {
            for (int k = 0; k < 20; k++) keep_again(ctx); /* 16 frames up to the ids */
            ASSERT_EQ_LL(ctx->keep_credit, 24u);
            keep_cell(l, 12, 12, 13);
            keep_frame(ctx);
            ASSERT(keep_is(ctx, 0, 1, 0, 23));
            keep_again(ctx); /* the refund stops at the ids too */
            ASSERT(keep_is(ctx, 13, 0, 0, 24));
            ASSERT(cell_shown(&h, 12, 13));
        }
        destroy_layers(&l, 1);
        harness_close(&h);
    }
    PASS();
}

/* A store on credit takes a free id or one of a keep stored on credit that no later frame drew, least recently used
 * first; never a keep drawn since, nor one the store policy made. Out of credit a group draws plainly. */
TEST test_keep_credit_evicts(void) {
    keep_max = 6, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_cheap);
    ASSERT_EQ_LL(ctx->keep_credit, 4u);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    keep_cell(l, 0, 7, 1);
    keep_cell(l, 1, 6, 2);
    ctx->keep_credit = 0;
    keep_frame(ctx); /* 1 on credit, the other plainly */
    ASSERT(keep_is(ctx, 0, 1, 1, 0));
    keep_again(ctx); /* drawn from its keep: 2 back; the other one stored as the policy says */
    ASSERT(keep_is(ctx, 1, 1, 0, 3));
    keep_again(ctx); /* all from keeps: stores on credit again */
    ASSERT(keep_is(ctx, 2, 0, 0, 4));
    for (uint32_t i = 0; i < 4; i++) keep_cell(l, 2 + i, i, 10 + i);
    keep_again(ctx);
    ASSERT(keep_is(ctx, 2, 4, 0, 1));
    for (uint32_t i = 0; i < 5; i++) keep_cell(l, 2 + i, i, 20 + i);
    ctx->keep_credit = 6;
    keep_frame(ctx); /* the four undrawn keeps give way, the older two drawn ones not; the fifth store finds none */
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(keep_is(ctx, 0, 4, 1, 2));
    ASSERT(kc(ctx).dead == 4 && kc(ctx).refused == 1 && klog_count(SHR_CMD_KEEP_RELEASE) == 0);
    for (uint32_t i = 0; i < 5; i++) ASSERT(cell_shown(&h, i, 20 + i));
    ASSERT(cell_shown(&h, 6, 2) && cell_shown(&h, 7, 1));
    keep_again(ctx);
    ASSERT(keep_is(ctx, 6, 0, 1, 6));
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* Stores on credit only in the first two of frames in a row that draw a keep group without its keep: a group that
 * changes every frame is not stored past them. A frame drawing all from keeps starts over. */
TEST test_keep_credit_run(void) {
    keep_max = 24, keep_cap = 0, keep_slot = 0;
    static const uint32_t want[6][4] = {{0, 2, 0, 9}, {0, 2, 0, 8}, {0, 0, 2, 9}, {0, 2, 0, 10}, {2, 0, 0, 11},
                                        {0, 2, 0, 10}}; /* hits, stores, direct, credit after */
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_cheap);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    keep_again(ctx);
    keep_again(ctx);
    ctx->keep_credit = 10;
    for (int k = 0; k < 6; k++) {
        if (k < 3 || k == 5)
            for (uint32_t i = 0; i < 2; i++) keep_cell(l, i, i, 10 * (uint64_t)k + i);
        keep_again(ctx);
        drain(ctx);
        ASSERT(keep_is(ctx, want[k][0], want[k][1], want[k][2], want[k][3]));
        ASSERT(cell_shown(&h, 0, 10 * (uint64_t)(k < 3 ? k : k == 5 ? 5 : 2)));
    }
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* A group with fallback pixels is not stored: its store on credit gives the credit back. */
TEST test_keep_credit_provisional(void) {
    keep_max = 4, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_cheap);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    r.px.provisional = true;
    keep_row(l, 0, &r, 0, RED, 8, 0);
    keep_frame(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(keep_is(ctx, 0, 0, 1, 3));
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 0 && klog_count(SHR_CMD_GLYPH) == 1 && row_shown(&h, 0, RED, 8, true));
    r.px.provisional = false, r.changed = true;
    klog_reset();
    shr_pump(ctx);
    ASSERT(keep_is(ctx, 0, 1, 0, 4)); /* seen before: stored as the policy says */
    ASSERT(klog_count(SHR_CMD_KEEP_BEGIN) == 1 && row_shown(&h, 0, RED, 8, true));
    keep_again(ctx);
    ASSERT(keep_is(ctx, 1, 0, 0, 4));
    keep_row(l, 1, &r, 16, GREEN, 16, 0);
    keep_frame(ctx);
    ASSERT(keep_is(ctx, 0, 1, 0, 3));
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

/* A frame drawing more groups from its keeps than it draws or stores otherwise starts the run over: the one group that
 * changes in each frame of a kept screen is stored on credit every time. */
TEST test_keep_credit_mostly_kept(void) {
    keep_max = 24, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_cheap);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    for (uint32_t i = 0; i < 4; i++) keep_cell(l, i, i, i + 1);
    keep_again(ctx);
    ASSERT(keep_is(ctx, 0, 4, 0, 13));
    for (uint32_t k = 0; k < 4; k++) {
        keep_cell(l, 0, 0, 10 + k);
        keep_again(ctx);
        ASSERT(keep_is(ctx, 3, 1, 0, 19) && cell_shown(&h, 0, 10 + k));
    }
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* A pump finding nothing to do once a frame showed (no frame running, nothing submitted or due, no resource with work
 * or a read in flight) makes the next frame store nothing on credit; stores at second sight stay. */
TEST test_keep_credit_still(void) {
    keep_max = 24, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_cheap);
    fake r, bare;
    fake_attach(&r, ctx, &fk_ops);
    fake_attach(&bare, ctx, &bare_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr_pump(ctx);
    ASSERT(!ctx->keep_still); /* nothing shown yet */
    for (uint32_t i = 0; i < 2; i++) keep_cell(l, i, i, i + 1);
    keep_frame(ctx);
    ASSERT(keep_is(ctx, 0, 2, 0, 15));
    r.work = true;
    shr_pump(ctx);
    ASSERT(!ctx->keep_still);
    r.work = false;
    src_log sl = {SHR_IN_PROGRESS, 0, 0, 0, 0};
    shr_asset_source src = source(&sl, true);
    uint8_t buf[4];
    ASSERT_EQ_LL(shr__ctx_read(ctx, &r.res, &src, 0, 4, buf, 1), SHR_OK);
    shr_pump(ctx);
    ASSERT(!ctx->keep_still); /* a read in flight */
    ASSERT_EQ_LL(shr_asset_complete(ctx, sl.req, SHR_OK), SHR_OK);
    shr_pump(ctx);
    ASSERT(!ctx->keep_still && r.io_n == 1); /* a read to deliver */
    shr_pump(ctx);
    ASSERT(ctx->keep_still);
    for (uint32_t i = 0; i < 2; i++) keep_cell(l, i, i, i + 10);
    keep_frame(ctx); /* plainly, credit left or not */
    ASSERT(!ctx->keep_still && keep_is(ctx, 0, 0, 2, 16));
    keep_again(ctx); /* seen again: stored as the policy says */
    ASSERT(keep_is(ctx, 0, 2, 0, 17));
    keep_again(ctx);
    ASSERT(keep_is(ctx, 2, 0, 0, 18));
    keep_cell(l, 0, 0, 20);
    keep_again(ctx); /* on credit again */
    ASSERT(keep_is(ctx, 1, 1, 0, 18) && cell_shown(&h, 0, 20) && cell_shown(&h, 1, 11));

    h.drv.async = true; /* not while a frame runs, nor at the pump that ends it */
    keep_again(ctx);
    shr_pump(ctx);
    ASSERT(!ctx->keep_still);
    md_complete(&h.drv);
    shr_pump(ctx);
    ASSERT(!ctx->keep_still && h.out.presents == 6);
    h.drv.async = false;
    r.px.provisional = true; /* nor when a resource change makes a frame due */
    keep_row(l, 2, &r, 32, RED, 8, 0);
    keep_frame(ctx);
    ASSERT(keep_is(ctx, 0, 0, 1, 22));
    r.px.provisional = false, r.changed = true;
    shr_pump(ctx);
    ASSERT(!ctx->keep_still && ctx->keep_run == 1 && h.out.presents == 8);
    destroy_layers(&l, 1);
    r.res.dead = bare.res.dead = true;
    harness_close(&h);

    ctx = harness_open(&h, 0, tweak_one_frame); /* nor while a submitted frame waits for the output */
    frame(ctx);
    frame(ctx);
    ASSERT(!ctx->keep_still && h.out.presents == 1);
    ASSERT_EQ_LL(shr_output_released(ctx, h.out.last_frame), SHR_OK);
    shr_pump(ctx);
    ASSERT(!ctx->keep_still && h.out.presents == 2);
    ASSERT_EQ_LL(shr_output_released(ctx, h.out.last_frame), SHR_OK);
    shr_pump(ctx);
    ASSERT(ctx->keep_still);
    harness_close(&h);
    PASS();
}

/* The pause takes the first frame submitted after it only: the next frame of new rows stores on credit again, the one
 * after that (the third in a row drawing more than it takes from keeps) does not. */
TEST test_keep_credit_still_once(void) {
    keep_max = 24, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_cheap);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    for (uint32_t i = 0; i < 2; i++) keep_cell(l, i, i, i + 1);
    keep_frame(ctx);
    keep_again(ctx); /* from keeps: a new run of frames may start */
    shr_pump(ctx);
    ASSERT(ctx->keep_still && keep_is(ctx, 2, 0, 0, 20));
    uint32_t stores[3];
    for (uint32_t f = 0; f < 3; f++) {
        for (uint32_t i = 0; i < 2; i++) keep_cell(l, i, i, 10 * (f + 1) + i);
        keep_frame(ctx);
        stores[f] = kc(ctx).stores;
    }
    ASSERT(stores[0] == 0 && stores[1] == 2 && stores[2] == 0);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

static void tweak_keep_blink_cheap(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_keep_blink(d, drv);
    drv->caps.flags |= SHR_DRIVER_CHEAP_STORE;
}

/* Blink phases alone do not end the pause: a blink frame after it stores on credit, the next submitted frame not. */
TEST test_keep_credit_still_blink(void) {
    keep_max = 4, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_blink_cheap);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    keep_row(l, 0, &r, 0, RED, 8, SHR__LCMD_BLINK);
    keep_frame(ctx);
    ASSERT(keep_is(ctx, 0, 1, 0, 2) && row_shown(&h, 0, RED, 8, true));
    fake_now = 50 * MS;
    shr_pump(ctx);
    ASSERT(ctx->keep_still);
    fake_now = 150 * MS;
    klog_reset();
    shr_pump(ctx);
    ASSERT(ctx->keep_still && keep_is(ctx, 0, 1, 0, 2) && klog_count(SHR_CMD_KEEP_BEGIN) == 1);
    ASSERT(row_shown(&h, 0, RED, 8, false));
    keep_row(l, 1, &r, 16, GREEN, 16, 0);
    keep_frame(ctx);
    ASSERT(!ctx->keep_still && keep_is(ctx, 0, 0, 1, 3) && row_shown(&h, 16, GREEN, 16, true));
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

/* The command limit applies to keep commands too: clear and fill at first sight; with a glyph beside, clear, glyph,
 * KEEP_BEGIN, fill, KEEP_END, KEEP_DRAW at the second; then the KEEP_DRAW of a kept row. */
TEST test_keep_command_limit(void) {
    keep_max = 4, keep_cap = 0, keep_slot = 0;
    for (max_cmds = 1; max_cmds <= 6; max_cmds++) {
        harness h;
        shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_cmds);
        fake r;
        fake_attach(&r, ctx, &fk_ops);
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        shr__lcmd row[3] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, 8, 16}, .key = {1}}, fill((shr_rect){0, 0, 8, 16}, RED),
                            {.kind = SHR__LCMD_CACHE_END}};
        shr__lcmd g[5];
        for (int i = 0; i < 5; i++) g[i] = glyph(&r, 16 + 8 * i, 0, WHITE);
        ASSERT_EQ_LL(shr__lyr_group_set(l, 2, row, 3), SHR_OK);
        frame(ctx);
        if (max_cmds >= 2) {
            expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
            ASSERT_EQ_LL(shr__lyr_group_set(l, 1, g, 1), SHR_OK);
            keep_again(ctx);
        }
        if (max_cmds < 6) {
            ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_LIMIT);
        } else {
            expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
            ASSERT_EQ_LL(shr__lyr_group_set(l, 1, g, 5), SHR_OK);
            keep_again(ctx);
            ASSERT_EQ_LL(expect_event(ctx, SHR_EVENT_PRESENT_FAILED).status, SHR_E_LIMIT);
        }
        destroy_layers(&l, 1);
        r.res.dead = true;
        harness_close(&h);
    }
    PASS();
}

/* Every allocation of the keep plan may fail (the first frame's releases, the stores, an eviction growing the plan
 * past its earlier size): the frame fails and the keeps stay consistent. */
TEST test_keep_plan_out_of_memory(void) {
    static fake fk[9];
    keep_max = 2, keep_cap = 2 * ROW_BYTES, keep_slot = 0;
    for (long budget = 0;; budget++) {
        harness h;
        shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_oom);
        for (int i = 0; i < 9; i++) fake_attach(&fk[i], ctx, &fk_ops);
        shr_lyr *l;
        ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
        long left = budget;
        keep_row(l, 1, &fk[8], 0, RED, 0, 0);
        keep_row(l, 2, &fk[8], 16, GREEN, 8, 0);
        for (int k = 0; k < 2; k++) { /* seen, then stored */
            ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
            limited(&left, shr_submit, ctx);
            limited(&left, shr_pump, ctx);
        }
        /* Seen before, a row two rows high replaces row 1's keep and releases row 2's, growing the plan, whose first
         * prologue block eight new buffers fill. */
        const shr__lcmd tall[3] = {{.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, HW, 32}, .key = {7, 7}},
                                   fill((shr_rect){0, 0, HW, 32}, BLUE), {.kind = SHR__LCMD_CACHE_END}};
        ASSERT_EQ_LL(shr__lyr_group_set(l, 1, tall, 3), SHR_OK);
        limited(&left, shr_submit, ctx);
        limited(&left, shr_pump, ctx);
        shr__lcmd g[8];
        for (int i = 0; i < 8; i++) g[i] = glyph(&fk[i], 8 * i, 32, WHITE);
        ASSERT_EQ_LL(shr__lyr_group_set(l, 0, g, 8), SHR_OK);
        ASSERT_EQ_LL(shr_request_redraw(ctx), SHR_OK);
        klog_reset();
        limited(&left, shr_submit, ctx);
        limited(&left, shr_pump, ctx);
        uint64_t bytes = 0;
        uint32_t releasing = 0;
        for (uint32_t k = 0; k < keep_max; k++) bytes += ctx->keeps[k].bytes, releasing += ctx->keeps[k].releasing;
        ASSERT(bytes == ctx->keep_resident && releasing == ctx->nkeep_released);
        destroy_layers(&l, 1);
        for (int i = 0; i < 9; i++) fk[i].res.dead = true;
        harness_close(&h);
        ASSERT_EQ_LL(oom.live, 0);
        if (left) {
            ASSERT(rec.npro == 9 && pro_is(8, SHR_CMD_KEEP_RELEASE, 2) && klog_nth(SHR_CMD_KEEP_BEGIN, 0)->buffer == 1);
            break;
        }
    }
    PASS();
}

TEST test_resource_resolution(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    fake r, im, off;
    fake_attach(&r, ctx, &fk_ops);
    fake_attach(&im, ctx, &fk_ops);
    fake_attach(&off, ctx, &bare_ops);
    fake_wrap(&im, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU);
    r.px.offset = (shr_point){-2, -1};
    off.px.offset = (shr_point){100, 0}; /* its pixels miss its cell */
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){4, 4, 40, 40}, &l), SHR_OK);
    shr__lcmd c[5] = {glyph(&r, 2, 3, RED), glyph(&r, 20, 3, RED), glyph(&im, 0, 20, 0), glyph(&r, 12, 3, RED),
                      glyph(&off, 24, 20, RED)};
    c[0].flags = SHR__LCMD_DIM;
    c[1].id = 0; /* not found: draws nothing */
    c[2].kind = SHR__LCMD_IMAGE, c[2].flags = SHR__LCMD_DIM;
    c[3].flags = SHR__LCMD_ON_FILL, c[3].bg = 0xFF000000; /* on the clear colour */
    paint(l, 5, c);
    frame(ctx);
    ASSERT_EQ_LL(rec.n, 4);
    const shr_draw_cmd *g = &rec.cmds[1];
    ASSERT(g->kind == SHR_CMD_GLYPH && g->flags == SHR_GLYPH_DIM && g->color == RED);
    ASSERT(rect_eq(g->dst, (shr_rect){6, 7, 12, 22}) && g->src_origin.x == 2 && g->src_origin.y == 1);
    ASSERT(rec.cmds[2].kind == SHR_CMD_IMAGE && rec.cmds[2].flags == 0);
    ASSERT(rec.cmds[3].kind == SHR_CMD_GLYPH && rec.cmds[3].flags == SHR_GLYPH_ON_FILL &&
           rec.cmds[3].bg == 0xFF000000);
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
    fake_wrap(&im, SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU);
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
    const shr__resolved *out = NULL;
    ASSERT_EQ_LL(ir->ops->resolve(ir, 0, 1, &out), SHR_OK);
    ASSERT(out->synth == 0 && out->slant_axis == 0);
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

/* Fallback pixels are recorded as rectangles: a cell next to the last one widens it. */
TEST test_provisional_runs(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    r.px.provisional = true;
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__lcmd c[4] = {glyph(&r, 0, 0, WHITE), glyph(&r, 8, 0, WHITE), glyph(&r, 32, 0, WHITE), glyph(&r, 0, 16, WHITE)};
    paint(l, 4, c);
    frame(ctx);
    const shr__vec *v = NULL;
    for (int i = 0; i < SHR_TARGETS; i++)
        if (ctx->targets[i].provisional.len) v = &ctx->targets[i].provisional;
    ASSERT(v && v->len == 3 && rect_eq(*SHR_VEC_AT(v, shr_rect, 0), (shr_rect){0, 0, 16, 16}));
    r.changed = true, r.px.provisional = false;
    shr_pump(ctx);
    ASSERT(h.out.presents == 2 && rec.damaged == 4 * 8 * 16);
    destroy_layers(&l, 1);
    r.res.dead = true;
    harness_close(&h);
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

/* ---- moves: a layer of six opaque rows of 8 pixels, group r in row coordinates at y 8r, scrolled like a tilemap ---- */

#define MROWS 6
static shr_color mshown[MROWS]; /* what each screen row of the layer should show */
static uint32_t mnext = 1;      /* colours of new rows: mcolor(1), mcolor(2), ..., exact in either format */

static shr_color mcolor(uint32_t k) {
    uint32_t r = (k * 7 % 32 * 255 + 15) / 31, g = (k * 13 % 64 * 255 + 31) / 63, b = (k * 3 % 32 * 255 + 15) / 31;
    return r << 16 | g << 8 | b;
}

static void mrow(shr_lyr *l, uint32_t r, uint16_t flags) {
    shr__rcmd *c = shr__lyr_row_begin(l, 1);
    c[0] = row_of(fill((shr_rect){0, 0, HW, 8}, mcolor(mnext)));
    c[0].flags = (uint8_t)flags;
    ASSERT_EQ_LL(shr__lyr_row_commit(l, r, (int32_t)r * 8, NULL, nokey, c, 1), SHR_OK);
    mshown[r] = mcolor(mnext++);
}

static shr_lyr *mlayer(shr_context *ctx, shr_rect rect) {
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, rect, &l), SHR_OK);
    mnext = 1;
    for (uint32_t r = 0; r < MROWS; r++) mrow(l, r, 0);
    return l;
}

/* Rows [top, bottom) up by n (down for n < 0); the uncovered rows get new colours. */
static void mscroll(shr_lyr *l, uint32_t top, uint32_t bottom, int32_t n) {
    shr__lyr_groups_shift(l, top, bottom, -n, (shr_rect){0, (int32_t)top * 8, HW, (int32_t)bottom * 8}, -n * 8);
    shr_color was[MROWS];
    memcpy(was, mshown, sizeof(was));
    for (uint32_t r = top; r < bottom; r++) {
        int64_t from = (int64_t)r + n;
        if (from >= top && from < bottom) mshown[r] = was[from];
        else mrow(l, r, 0);
    }
}

/* Every row shows its colour at a few columns of the layer rect `in`. */
static bool mrows_shown(const harness *h, shr_rect in) {
    bool ok = true;
    for (int32_t r = 0; r < MROWS; r++)
        for (int32_t x = in.x0; x < in.x1; x += 9) ok &= px(h->out.shown, x, in.y0 + 8 * r + 3) == mshown[r];
    return ok;
}

static int mcopies(void) {
    int n = 0;
    for (size_t i = 0; i < rec.n; i++) n += rec.cmds[i].kind == SHR_CMD_COPY;
    return n;
}

static void tweak_moves(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->caps.flags = SHR_DRIVER_CHEAP_MOVE;
}

/* A scrolled layer's pixels move with one COPY of the output onto itself ahead of the draws of the rows it uncovers;
 * a full-screen move leaves the rest of the output to the driver, a region or a narrower layer does not. */
TEST test_moves_copy_the_output_onto_itself(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_moves);
    shr_lyr *l = mlayer(ctx, FULL);
    frame(ctx);
    mscroll(l, 0, MROWS, 1);
    frame(ctx);
    const shr_draw_cmd *c = &rec.cmds[0];
    ASSERT(rec.n == 2 && c->kind == SHR_CMD_COPY && c->flags == SHR_COPY_REST_UNDEFINED);
    ASSERT(rect_eq(c->dst, (shr_rect){0, 0, HW, 40}) && c->src_origin.x == 0 && c->src_origin.y == 8);
    ASSERT(c->src.pixels == h.out.bufs[h.out.last_buf] && c->src.stride == (size_t)HW * SCREEN_BPP);
    ASSERT(rec.cmds[1].kind == SHR_CMD_FILL && rect_eq(rec.cmds[1].dst, (shr_rect){0, 40, HW, 48}));
    ASSERT(rec.damaged == HW * 8 && mrows_shown(&h, FULL));
    mscroll(l, 0, MROWS, -2);
    frame(ctx);
    ASSERT(mcopies() == 1 && rec.cmds[0].src_origin.y == 0 && rect_eq(rec.cmds[0].dst, (shr_rect){0, 16, HW, 48}));
    ASSERT(mrows_shown(&h, FULL));
    mscroll(l, 1, 5, 1); /* a region: rows 0 and 5 stay */
    frame(ctx);
    ASSERT(mcopies() == 1 && !rec.cmds[0].flags && rect_eq(rec.cmds[0].dst, (shr_rect){0, 8, HW, 32}));
    ASSERT(mrows_shown(&h, FULL));
    destroy_layers(&l, 1);
    frame(ctx);
    const shr_rect narrow = {8, 0, 56, HH};
    l = mlayer(ctx, narrow);
    frame(ctx);
    mscroll(l, 0, MROWS, 1);
    frame(ctx);
    ASSERT(mcopies() == 1 && !rec.cmds[0].flags && rect_eq(rec.cmds[0].dst, (shr_rect){8, 0, 56, 40}));
    ASSERT(mrows_shown(&h, narrow));
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* Without the driver's cheap moves, beyond half the area, over rows that are not opaque, on a hidden layer, an output
 * that keeps nothing, or with damage beyond 3/4 of the screen, the pixels are drawn again. */
TEST test_moves_fall_back_to_drawing(void) {
    for (int mode = 0; mode < 6; mode++) {
        harness h;
        shr_context *ctx = harness_open(&h, mode == 4 ? SHR_OUTPUT_RELEASE_ON_PRESENT : PRESERVED,
                                        mode ? tweak_moves : tweak_rec);
        shr_lyr *l = mlayer(ctx, FULL);
        if (mode == 2) mrow(l, 3, SHR__LCMD_DIM);
        frame(ctx);
        if (mode == 3) ASSERT_EQ_LL(shr_lyr_set_visible(l, false), SHR_OK);
        shr__damage staged = ctx->staged;
        mscroll(l, 0, MROWS, mode == 1 ? 3 : 1);
        if (mode == 3) ASSERT(ctx->staged.rects.len == staged.rects.len && ctx->staged.nmoves == 0);
        if (mode == 3) ASSERT_EQ_LL(shr_lyr_set_visible(l, true), SHR_OK);
        for (uint32_t r = 0; mode == 5 && r < 4; r++) mrow(l, r, 0);
        frame(ctx);
        ASSERT_EQ_LL(mcopies(), 0);
        if (mode != 2) ASSERT(mrows_shown(&h, FULL));
        destroy_layers(&l, 1);
        harness_close(&h);
    }
    PASS();
}

/* Layers above keep their place: where they are and where their pixels moved to is drawn again; hidden ones and
 * layers below need nothing. */
TEST test_moves_under_other_layers(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_moves);
    shr_lyr *below = solid(ctx, -1, FULL, RED), *l = mlayer(ctx, FULL);
    shr_lyr *cursor = solid(ctx, 1, (shr_rect){16, 32, 24, 40}, WHITE), *hidden = solid(ctx, 2, (shr_rect){0, 0, 8, 8}, RED);
    ASSERT_EQ_LL(shr_lyr_set_visible(hidden, false), SHR_OK);
    frame(ctx);
    mscroll(l, 0, MROWS, 1);
    frame(ctx);
    ASSERT(mcopies() == 1 && px(h.out.shown, 18, 34) == WHITE && px(h.out.shown, 18, 26) == mshown[3]);
    ASSERT(px(h.out.shown, 2, 2) == mshown[0]);
    ASSERT_EQ_LL(rec.damaged, HW * 8 + 2 * 64);
    shr_lyr *big = solid(ctx, 3, (shr_rect){0, 8, 24, 24}, GREEN); /* covers too much: drawn again instead */
    frame(ctx);
    mscroll(l, 0, MROWS, 1);
    frame(ctx);
    ASSERT(mcopies() == 0 && px(h.out.shown, 2, 10) == GREEN && px(h.out.shown, 40, 10) == mshown[1]);
    shr_lyr *all[5] = {below, l, cursor, hidden, big};
    destroy_layers(all, 5);
    harness_close(&h);
    PASS();
}

/* Moves between frames: one way in one area add up; others follow one another, also onto buffers several frames
 * behind; past SHR_MAX_MOVES the moved areas are drawn again. */
TEST test_moves_accumulate(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_moves);
    shr_lyr *l = mlayer(ctx, FULL);
    frame(ctx);
    for (int down = 0; down < 2; down++) {
        mscroll(l, 0, MROWS, down ? -1 : 1);
        mscroll(l, 0, MROWS, down ? -1 : 1);
        frame(ctx);
        ASSERT(mcopies() == 1 && rec.cmds[0].src_origin.y == (down ? 0 : 16) && mrows_shown(&h, FULL));
    }
    mscroll(l, 0, MROWS, 1);
    mscroll(l, 0, MROWS, -1); /* the other way */
    mscroll(l, 0, 4, 1);      /* another area */
    mscroll(l, 0, MROWS, 2);
    mscroll(l, 0, MROWS, 2); /* too far to add up */
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    ASSERT_EQ_LL(ctx->targets[0].damage.nmoves, 5);
    ASSERT_EQ_LL(shr_pump(ctx), SHR_OK);
    ASSERT(mrows_shown(&h, FULL));
    h.out.busy[0] = true; /* the next frames go to buffer 1, then buffer 0 again, three moves behind */
    mscroll(l, 0, MROWS, -1);
    frame(ctx);
    ASSERT(h.out.last_buf == 1 && mcopies() == 0 && mrows_shown(&h, FULL));
    mscroll(l, 0, MROWS, 1);
    frame(ctx);
    ASSERT(h.out.last_buf == 1 && mcopies() == 1 && mrows_shown(&h, FULL));
    h.out.busy[0] = false, h.out.busy[1] = true;
    mscroll(l, 1, MROWS, 1);
    frame(ctx);
    ASSERT(h.out.last_buf == 0 && mcopies() == 3 && mrows_shown(&h, FULL));
    h.out.busy[1] = false;
    for (int i = 0; i < SHR_MAX_MOVES + 1; i++) mscroll(l, 0, MROWS, i % 2 ? 1 : -1);
    ASSERT_EQ_LL(ctx->staged.nmoves, 1); /* the first eight became damage */
    frame(ctx);
    ASSERT(mcopies() == 0 && mrows_shown(&h, FULL));
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* A move the frame being built cannot carry (made after the submission, or after the frame took its damage while it
 * still runs, fails, or drew fallback pixels) becomes damage of the moved area. */
TEST test_moves_racing_frames(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_moves);
    fake r;
    fake_attach(&r, ctx, &fk_ops);
    shr_lyr *l = mlayer(ctx, FULL), *top;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 1, (shr_rect){40, 0, 48, 8}, &top), SHR_OK); /* small enough to move under */
    frame(ctx);
    mrow(l, 2, 0);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK); /* submitted, then moved: the frame draws the moved rows */
    mscroll(l, 0, MROWS, 1);
    ASSERT_EQ_LL(shr_pump(ctx), SHR_OK);
    ASSERT_EQ_LL(mcopies(), 0);
    frame(ctx);
    ASSERT(mcopies() == 0 && mrows_shown(&h, FULL));
    mscroll(l, 0, MROWS, 1); /* a failed frame gives its damage and moves back */
    h.drv.fail_next = 1;
    frame(ctx);
    ASSERT_EQ_LL(mcopies(), 1);
    frame(ctx);
    ASSERT(mcopies() == 0 && mrows_shown(&h, FULL));
    mscroll(l, 0, MROWS, 1); /* more moves than commands allowed */
    mscroll(l, 0, MROWS, -1);
    ctx->desc.max_commands = 1;
    drain(ctx);
    frame(ctx);
    shr_event ev;
    ASSERT(count_events(ctx, SHR_EVENT_PRESENT_FAILED, &ev) == 1 && ev.status == SHR_E_LIMIT);
    ctx->desc.max_commands = 16384;
    frame(ctx);
    ASSERT(mrows_shown(&h, FULL));
    for (int prov = 0; prov < 2; prov++) { /* moved while the frame runs */
        r.px.provisional = prov;
        shr__lcmd g = glyph(&r, 0, 0, WHITE);
        paint(top, 1, &g);
        h.drv.async = true;
        mscroll(l, 0, MROWS, 1);
        frame(ctx);
        mscroll(l, 0, MROWS, 1);
        ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
        h.drv.async = false;
        md_complete(&h.drv);
        settle(ctx);
        ASSERT_EQ_LL(mcopies(), prov ? 0 : 1);
        ASSERT(px(h.out.shown, 2, 3) == mshown[0] && px(h.out.shown, 2, 43) == mshown[5]);
    }
    mscroll(l, 0, MROWS, -1); /* fallback pixels move along, then resolve */
    frame(ctx);
    ASSERT_EQ_LL(mcopies(), 1);
    r.px.provisional = false, r.changed = true;
    settle(ctx);
    ASSERT(rec.damaged == 8 * 8 && px(h.out.shown, 43, 3) == WHITE && mrows_shown(&h, (shr_rect){0, 0, 32, HH}));
    shr_lyr *all[2] = {l, top};
    destroy_layers(all, 2);
    r.res.dead = true;
    harness_close(&h);
    PASS();
}

static void tweak_moves_klog(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_moves(d, drv);
    drv->execute = klog_execute;
}

static void tweak_klog_moves(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_rec(d, drv);
    drv->execute = klog_execute;
    drv->caps.flags = SHR_DRIVER_CHEAP_MOVE;
}

/* A refused conversion runs again by itself; the band after it is built only then. */
TEST test_band_conversion_refused(void) {
    harness h;
    shr_context *ctx = harness_open(&h, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak_klog_moves);
    shr_surface b[2] = {band_surface(0, 0), band_surface(1, 0)};
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_OK);
    shr_lyr *l = solid(ctx, 0, FULL, GREEN);
    klog_reset();
    klog.block_at = 2;
    frame(ctx);
    ASSERT(klog.calls == 2 && klog.n == 1 && klog.cmds[0].kind == SHR_CMD_FILL && h.out.presents == 0);
    ASSERT_EQ_LL(shr_driver_ready(ctx), SHR_OK);
    ASSERT_EQ_LL(shr_pump(ctx), SHR_OK);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(klog.calls == 7 && klog.n == 6 && klog.cmds[1].kind == SHR_CMD_COPY && rect_eq(klog.cmds[2].dst, (shr_rect){0, 0, HW, 16}));
    ASSERT_EQ_LL(px(h.out.shown, HW - 1, HH - 1), GREEN);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* With a driver that runs batches on its own, a layer may move while a band runs: the bands built after that draw it
 * moved, so the move turns into damage and the next frame draws the area again instead of moving the pixels twice. */
TEST test_band_move_while_running(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_moves);
    shr_surface b[2] = {band_surface(0, 0), band_surface(1, 0)};
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 0), SHR_OK);
    shr_lyr *l = mlayer(ctx, FULL);
    frame(ctx);
    drain(ctx);
    h.drv.async = true;
    mscroll(l, 0, MROWS, 1);
    frame(ctx);
    ASSERT(h.drv.pending && rec.n == 1 && rec.cmds[0].kind == SHR_CMD_COPY);
    mscroll(l, 0, MROWS, 1);
    for (int i = 0; i < 8 && h.drv.pending; i++) md_complete(&h.drv), shr_pump(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    drain(ctx);
    frame(ctx);
    for (int i = 0; i < 8 && h.drv.pending; i++) md_complete(&h.drv), shr_pump(ctx);
    expect_event(ctx, SHR_EVENT_PRESENT_ACCEPTED);
    ASSERT(rec.damaged == HW * HH && mrows_shown(&h, FULL));
    mscroll(l, 0, MROWS, 1); /* the same, submitted while the move runs */
    frame(ctx);
    ASSERT(h.drv.pending && rec.n == 1 && rec.cmds[0].kind == SHR_CMD_COPY);
    mscroll(l, 0, MROWS, 1);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    for (int i = 0; i < 8 && h.drv.pending; i++) md_complete(&h.drv), shr_pump(ctx);
    for (int i = 0; i < 8 && h.drv.pending; i++) md_complete(&h.drv), shr_pump(ctx);
    drain(ctx);
    ASSERT(rec.damaged == HW * HH && mrows_shown(&h, FULL));
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* With bands the moves run first, in a batch of their own into the output, in output coordinates. */
TEST test_moves_with_bands(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_moves_klog);
    h.out.w = HH, h.out.h = HW;
    shr_surface b[2] = {band_surface(0, 0), band_surface(1, 0)};
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = HW, sd.height = HH, sd.rotation = SHR_ROTATE_90_CW, sd.bands = b, sd.band_count = 2, sd.band_align = 8;
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_OK);
    shr_lyr *l = mlayer(ctx, FULL);
    frame(ctx);
    klog_reset();
    mscroll(l, 0, MROWS, 1);
    frame(ctx);
    ASSERT(klog.calls == 3 && klog.cmds[0].kind == SHR_CMD_COPY && klog.cmds[0].flags == SHR_COPY_REST_UNDEFINED);
    ASSERT(rect_eq(klog.cmds[0].dst, (shr_rect){8, 0, HH, HW}) && klog.cmds[0].src_origin.x == 0);
    ASSERT(klog.cmds[0].src.pixels == h.out.bufs[h.out.last_buf] && klog.cmds[0].src.width == HH);
    for (int32_t r = 0; r < MROWS; r++)
        for (int32_t x = 1; x < HW; x += 13) {
            shr_point p;
            ASSERT_EQ_LL(shr_rotation_map_point(SHR_ROTATE_90_CW, HW, HH, (shr_point){x, 8 * r + 3}, false, &p), SHR_OK);
            uint32_t c = mshown[r], q = c;
            if (SCREEN_BPP == 2) /* opx() widens without rounding */
                q = ((c >> 16) * 31 + 127) / 255 * 255 / 31 << 16 | ((c >> 8 & 255) * 63 + 127) / 255 * 255 / 63 << 8 |
                    ((c & 255) * 31 + 127) / 255 * 255 / 31;
            ASSERT_EQ_LL(opx(&h, p.x, p.y), q);
        }
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* Damage widened to band_align merges where it overlaps or shares an edge: one COPY for both, two for a corner. */
TEST test_band_regions_merge(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_klog_moves);
    shr_surface b[2] = {band_surface(0, 0), band_surface(1, 0)};
    ASSERT_EQ_LL(configure_bands(ctx, b, 2, 8), SHR_OK);
    shr_lyr *l = solid(ctx, 0, FULL, RED);
    frame(ctx);
    const shr_rect d[4] = {{0, 0, 6, 8}, {10, 0, 14, 8}, {0, 16, 6, 24}, {10, 24, 14, 32}};
    for (int i = 0; i < 4; i++) shr__damage_add(ctx, &ctx->staged, d[i]);
    klog_reset();
    frame(ctx);
    ASSERT_EQ_LL(klog_count(SHR_CMD_COPY), 3);
    ASSERT(rect_eq(klog_nth(SHR_CMD_COPY, 0)->dst, (shr_rect){0, 0, 16, 8}));
    const shr_rect pairs[2][2] = {{{0, 0, 6, 6}, {0, 10, 6, 16}}, {{0, 0, 6, 6}, {4, 4, 22, 12}}}; /* stacked, overlapping */
    const shr_rect merged[2] = {{0, 0, 8, 16}, {0, 0, 24, 16}};
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 2; i++) shr__damage_add(ctx, &ctx->staged, pairs[k][i]);
        klog_reset();
        frame(ctx);
        ASSERT_EQ_LL(klog_count(SHR_CMD_COPY), 1);
        ASSERT(rect_eq(klog_nth(SHR_CMD_COPY, 0)->dst, merged[k]));
    }
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* The keeps of moved rows stay recent; rows that left, rows without a keep group and the blink phase never stored
 * are passed over. */
static void tweak_keep_moves(shr_context_desc *d, shr_framebuffer_driver *drv) {
    tweak_keep(d, drv);
    drv->caps.flags = SHR_DRIVER_CHEAP_MOVE;
}

TEST test_moves_keep_rows_recent(void) {
    keep_max = 8, keep_cap = 0, keep_slot = 0;
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_keep_moves);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    for (uint32_t r = 0; r < MROWS; r++) { /* keys in one chain */
        if (r == 4) continue;
        shr__rcmd *c = shr__lyr_row_begin(l, 3);
        c[0] = row_of((shr__lcmd){.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, HW, 8}});
        c[1] = row_of(fill((shr_rect){0, 0, HW, 8}, mcolor(r + 1)));
        c[1].flags = r == 3 ? SHR__LCMD_BLINK : 0;
        c[2] = row_of((shr__lcmd){.kind = SHR__LCMD_CACHE_END});
        const uint64_t key[2] = {8 * r + 1, r};
        ASSERT_EQ_LL(shr__lyr_row_commit(l, r, (int32_t)r * 8, NULL, key, c, 3), SHR_OK);
    }
    shr__lcmd plain_row = fill((shr_rect){0, 32, HW, 40}, BLUE); /* row 4 without a keep group */
    ASSERT_EQ_LL(shr__lyr_group_set(l, 4, &plain_row, 1), SHR_OK);
    keep_frame(ctx);
    keep_again(ctx); /* stored, rows 0 .. 5 in order */
    ASSERT_EQ_LL(klog_count(SHR_CMD_KEEP_BEGIN), 5);
    keep_damage(ctx, (shr_rect){0, 0, HW, 8}); /* row 0 drawn: the oldest is now row 1 */
    shr__lyr_groups_shift(l, 1, MROWS, 1, (shr_rect){0, 8, HW, 48}, 8);
    const shr__keep *oldest = SHR_CONTAINER(shr__lru_oldest(ctx->keep_lru), shr__keep, lru);
    ASSERT_EQ_LL(oldest->key.hash[0], 8 * 5 + 1); /* rows 1 .. 3 moved down and were touched; row 5 left */
    shr__lcmd fresh = fill((shr_rect){0, 8, HW, 16}, GREEN);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 1, &fresh, 1), SHR_OK);
    keep_frame(ctx);
    ASSERT(klog_nth(SHR_CMD_COPY, 0)->src_origin.y == 8 && px(h.out.shown, 3, 3 * 8 + 3) == mcolor(3));
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* A group committed at another y is a change of all of it, at both places. */
TEST test_group_placed_elsewhere(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    shr__rcmd *c = shr__lyr_row_begin(l, 1);
    c[0] = row_of(fill((shr_rect){0, 0, SHR_CELL_WIDTH, 8}, RED));
    ASSERT_EQ_LL(shr__lyr_row_commit(l, 0, 8, NULL, nokey, c, 1), SHR_OK);
    frame(ctx);
    ASSERT(px(h.out.shown, 2, 10) == RED && px(h.out.shown, 2, 2) == 0);
    c = shr__lyr_row_begin(l, 1);
    c[0] = row_of(fill((shr_rect){0, 0, SHR_CELL_WIDTH, 8}, RED));
    ASSERT_EQ_LL(shr__lyr_row_commit(l, 0, 24, NULL, nokey, c, 1), SHR_OK);
    frame(ctx);
    ASSERT(rec.damaged == 3 * 8 * SHR_CELL_WIDTH && px(h.out.shown, 2, 26) == RED && px(h.out.shown, 2, 10) == 0);
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, NULL, 0), SHR_OK); /* removed from its place */
    frame(ctx);
    ASSERT(rec.damaged == 8 * SHR_CELL_WIDTH && px(h.out.shown, 2, 26) == 0);
    destroy_layers(&l, 1);
    harness_close(&h);
    PASS();
}

/* Row groups draw like the same list in full form. A group that changes form or a row that changes resource changed
 * all of it, a row whose key alone changed nothing; a row's cache pair encloses it. */
TEST test_row_groups(void) {
    harness h;
    shr_context *ctx = harness_open(&h, PRESERVED, tweak_rec);
    fake r, r2;
    fake_attach(&r, ctx, &fk_ops);
    fake_attach(&r2, ctx, &fk_ops);
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, FULL, &l), SHR_OK);
    const int32_t cw = SHR_CELL_WIDTH, area = 2 * cw * 16; /* two cells of 16 rows */
    shr__lcmd full[2] = {fill((shr_rect){0, 0, 2 * cw, 16}, RED), glyph(&r, cw, 0, WHITE)};
    full[1].dst.x1 = 2 * cw;
    shr__rcmd rows[4] = {row_of((shr__lcmd){.kind = SHR__LCMD_CACHE_BEGIN, .dst = {0, 0, 2 * cw, 16}}), row_of(full[0]),
                         row_of(full[1]), row_of((shr__lcmd){.kind = SHR__LCMD_CACHE_END})};
    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, full, 2), SHR_OK);
    frame(ctx);
    ASSERT(px(h.out.shown, 2, 2) == RED && px(h.out.shown, cw + 2, 2) == WHITE);
    ASSERT_EQ_LL(rows_set(l, 0, 0, &r.res, rows + 1, 2), SHR_OK);
    ASSERT_EQ_LL(r.res.users, 1);
    frame(ctx);
    ASSERT(rec.damaged == area && px(h.out.shown, 2, 2) == RED && px(h.out.shown, cw + 2, 2) == WHITE);
    ASSERT_EQ_LL(rows_set(l, 0, 0, &r2.res, rows + 1, 2), SHR_OK);
    ASSERT(r.res.users == 0 && r2.res.users == 1);
    frame(ctx);
    ASSERT_EQ_LL(rec.damaged, area);

    const uint64_t keys[3][2] = {{1, 2}, {1, 3}, {4, 3}};
    for (int i = 0; i < 3; i++) {
        rec.damaged = 0;
        ASSERT_EQ_LL(shr__lyr_row_commit(l, 0, 0, &r2.res, keys[i], rows, 4), SHR_OK);
        frame(ctx);
        ASSERT_EQ_LL(rec.damaged, i ? 0 : area); /* the cache pair added, then only its key */
        ASSERT_EQ_LL(SHR_VEC_AT(&l->groups, shr__group, 0)->key[0], keys[i][0]);
        ASSERT_EQ_LL(SHR_VEC_AT(&l->groups, shr__group, 0)->key[1], keys[i][1]);
    }
    shr__rcmd inner[3] = {rows[1], rows[0], rows[3]}, open[3] = {rows[0], rows[3], rows[1]};
    ASSERT_EQ_LL(rows_set(l, 0, 0, &r2.res, inner, 3), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(rows_set(l, 0, 0, &r2.res, open, 3), SHR_E_INVALID_ARG);
    rows[1].flags = 1u << 6;
    ASSERT_EQ_LL(rows_set(l, 0, 0, &r2.res, rows, 4), SHR_E_INVALID_ARG); /* unknown flag */

    ASSERT_EQ_LL(shr__lyr_group_set(l, 0, full, 2), SHR_OK);
    ASSERT(r.res.users == 1 && r2.res.users == 0);
    frame(ctx);
    ASSERT(rec.damaged == area && px(h.out.shown, 2, 2) == RED && px(h.out.shown, cw + 2, 2) == WHITE);
    destroy_layers(&l, 1);
    r.res.dead = r2.res.dead = true;
    harness_close(&h);
    PASS();
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(test_init_functions);
    RUN_TEST(test_create_rejects_invalid_descriptors);
    RUN_TEST(test_create_out_of_memory);
    RUN_TEST(test_context_services);
    RUN_TEST(test_plugin_slots);
    RUN_TEST(test_screen_configure_validation);
    RUN_TEST(test_screen_configure_bands);
    RUN_TEST(test_band_sources_aligned);
    RUN_TEST(test_dma_only_composition);
    RUN_TEST(test_band_sync_and_trace);
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
    RUN_TEST(test_blink_ignores_output);
    RUN_TEST(test_blink_epoch_in_future);
    RUN_TEST(test_blink_needs_clock);
    RUN_TEST(test_blink_outside_screen_draws_nothing);
    RUN_TEST(test_frame_cap);
    RUN_TEST(test_frame_cap_blink);
    RUN_TEST(test_command_limit_fails_frame);
    RUN_TEST(test_band_frame_failures);
    RUN_TEST(test_band_command_list);
    RUN_TEST(test_band_command_list_hot);
    RUN_TEST(test_band_buffers_held_for_frame);
    RUN_TEST(test_driver_would_block);
    RUN_TEST(test_driver_ready_retries);
    RUN_TEST(test_driver_caps_checked);
    RUN_TEST(test_dma_sync);
    RUN_TEST(test_buffer_wrap);
    RUN_TEST(test_buffer_alloc);
    RUN_TEST(test_buffer_ids);
    RUN_TEST(test_buffer_plan_dropped);
    RUN_TEST(test_buffer_registered_again);
    RUN_TEST(test_buffer_eviction_by_bytes);
    RUN_TEST(test_buffer_eviction_by_ids);
    RUN_TEST(test_fallback_when_ids_run_short);
    RUN_TEST(test_short_ids_redrawn_while_fewer_go_without);
    RUN_TEST(test_buffer_lost_batch_released);
    RUN_TEST(test_buffer_plan_out_of_memory);
    RUN_TEST(test_frame_out_of_memory);
    RUN_TEST(test_lines_out_of_memory);
    RUN_TEST(test_scattered_damage);
    RUN_TEST(test_damage_out_of_memory_redraws_all);
    RUN_TEST(test_async_frames_keep_presenting);
    RUN_TEST(test_damage_list_out_of_memory_forgets_target);
    RUN_TEST(test_automatic_frames_wait_for_submit);
    RUN_TEST(test_timers_saturate);
    RUN_TEST(test_configure_rejects_unreachable_composition);
    RUN_TEST(test_layer_arguments);
    RUN_TEST(test_layer_out_of_memory);
    RUN_TEST(test_group_memory_classes);
    RUN_TEST(test_layer_order);
    RUN_TEST(test_layer_rect_clips_and_moves);
    RUN_TEST(test_app_layer_diff);
    RUN_TEST(test_group_damage_runs);
    RUN_TEST(test_command_diff_compares_every_field);
    RUN_TEST(test_group_resource_counts);
    RUN_TEST(test_groups_draw_in_id_order);
    RUN_TEST(test_command_validation);
    RUN_TEST(test_plugin_owned_layers);
    RUN_TEST(test_resource_users_counted);
    RUN_TEST(test_hidden_layers_skipped);
    RUN_TEST(test_large_group_blocks_skipped);
    RUN_TEST(test_keep_rows);
    RUN_TEST(test_keep_hit_skips_to_its_end);
    RUN_TEST(test_keep_eviction);
    RUN_TEST(test_keep_replacement);
    RUN_TEST(test_keep_blink_phases);
    RUN_TEST(test_keep_lost_batches);
    RUN_TEST(test_keep_bands);
    RUN_TEST(test_keep_store_budget);
    RUN_TEST(test_keep_credit);
    RUN_TEST(test_keep_credit_evicts);
    RUN_TEST(test_keep_credit_provisional);
    RUN_TEST(test_keep_credit_run);
    RUN_TEST(test_keep_credit_mostly_kept);
    RUN_TEST(test_keep_credit_still);
    RUN_TEST(test_keep_credit_still_once);
    RUN_TEST(test_keep_credit_still_blink);
    RUN_TEST(test_keep_command_limit);
    RUN_TEST(test_keep_plan_out_of_memory);
    RUN_TEST(test_resource_resolution);
    RUN_TEST(test_glyph_styles);
    RUN_TEST(test_provisional_pixels_redrawn);
    RUN_TEST(test_provisional_runs);
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
    RUN_TEST(test_moves_copy_the_output_onto_itself);
    RUN_TEST(test_moves_fall_back_to_drawing);
    RUN_TEST(test_moves_under_other_layers);
    RUN_TEST(test_moves_accumulate);
    RUN_TEST(test_moves_racing_frames);
    RUN_TEST(test_moves_with_bands);
    RUN_TEST(test_band_regions_merge);
    RUN_TEST(test_band_conversion_refused);
    RUN_TEST(test_band_move_while_running);
    RUN_TEST(test_moves_keep_rows_recent);
    RUN_TEST(test_group_placed_elsewhere);
    RUN_TEST(test_row_groups);
    GREATEST_MAIN_END();
}
