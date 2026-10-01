/* Offscreen render comparison. Every scene of the catalog is drawn through a backend into an offscreen output.
 * software: XXH3-128 of each output per scene, cell size and screen format against golden.txt (SHR_UPDATE_GOLDEN=1
 *   rewrites this build's entries; exit 77 when the file has none for this cell size), and reftest pairs exactly.
 * GPU backends: every scene against the software image within the scene's fuzzy limit, and reftest pairs fuzzily.
 * Options (before greatest's own, e.g. -t <name>): --backend software|angle, --golden, --reftest, --export (PNG of
 * every scene into the output directory), --out DIR, --frames N (times the first frame, update included, then N
 * full redraws per scene; -t filters by substring). Mismatches write expected/actual/diff PNGs into <out>/failures. */
#define _POSIX_C_SOURCE 200809L
#include "check.h"
#include "render.h"
#include "shr_hash.h"

#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#ifdef SHR_RENDER_ANGLE
#include <GLES3/gl3.h>
#include <shiroko/port_angle.h>
#endif

shr_status png_write(const char *path, int32_t w, int32_t h, const uint8_t *rgb);

#define FMT_NAME(f) ((f) == SHR_FORMAT_RGB565 ? "rgb565" : "rgbx8888")
#define BPP(f) ((f) == SHR_FORMAT_RGB565 ? 2u : 4u)

/* ===== Backends ===== */

typedef struct backend {
    const char *name;
    shr_status (*driver_create)(shr_framebuffer_driver *out);
    shr_status (*driver_destroy)(shr_framebuffer_driver *d);
    shr_status (*surface_create)(shr_framebuffer_driver *d, int32_t w, int32_t h, shr_pixel_format f, shr_surface *out);
    shr_status (*surface_destroy)(shr_framebuffer_driver *d, shr_surface *s);
    /* CPU copy in the surface's format, rows `stride` bytes apart. */
    shr_status (*surface_read)(shr_framebuffer_driver *d, const shr_surface *s, void *pixels, size_t stride);
    void (*finish)(void); /* waits until the device finished everything submitted (frame timing) */
} backend;

static shr_status sw_driver_create(shr_framebuffer_driver *out) { return shr_software_driver_create(NULL, 1u << 20, out); }

static shr_status sw_surface_create(shr_framebuffer_driver *d, int32_t w, int32_t h, shr_pixel_format f, shr_surface *out) {
    (void)d;
    size_t row;
    shr_status st = shr_format_row_bytes(f, w, &row);
    void *px = st == SHR_OK ? calloc((size_t)h, row) : NULL;
    if (!px) return st == SHR_OK ? SHR_E_NO_MEMORY : st;
    *out = (shr_surface){px, w, h, row, row * (size_t)h, f, 1, SHR_MEMORY_CPU, 0};
    return SHR_OK;
}

static shr_status sw_surface_destroy(shr_framebuffer_driver *d, shr_surface *s) {
    (void)d;
    free(s->pixels);
    *s = (shr_surface){0};
    return SHR_OK;
}

static shr_status sw_surface_read(shr_framebuffer_driver *d, const shr_surface *s, void *pixels, size_t stride) {
    (void)d;
    size_t row = (size_t)s->width * BPP(s->format);
    for (int32_t y = 0; y < s->height; y++)
        memcpy((uint8_t *)pixels + (size_t)y * stride, (const uint8_t *)s->pixels + (size_t)y * s->stride, row);
    return SHR_OK;
}

static void no_finish(void) {}

static const backend software = {"software",         sw_driver_create, shr_software_driver_destroy, sw_surface_create,
                                 sw_surface_destroy, sw_surface_read,  no_finish};

#ifdef SHR_RENDER_ANGLE
static shr_status angle_driver_create(shr_framebuffer_driver *out) { return shr_angle_driver_create(NULL, 64u << 20, out); }
static void angle_finish(void) { glFinish(); }
static const backend angle = {"angle",
                              angle_driver_create,
                              shr_angle_driver_destroy,
                              shr_angle_surface_create,
                              shr_angle_surface_destroy,
                              shr_angle_surface_read,
                              angle_finish};
#endif

/* ===== Options ===== */

static struct {
    const backend *backend;
    const char *out;
    const char *filter;
    long frames;
    bool golden, reftest, export_all;
} opt = {&software, SHR_RENDER_OUT, NULL, 0, false, false, false};

/* ===== Rendering one scene ===== */

typedef struct picture {
    uint8_t *px; /* rows packed */
    int32_t w, h;
    shr_pixel_format fmt;
} picture;

typedef struct run {
    const backend *b;
    const scene *sc;
    stage s;
    shr_framebuffer_driver drv;
    bool has_drv;
    shr_surface out, comp;
    int presents;
    uint32_t flags;
    char log[256];
} run;

static shr_status out_acquire(void *user, shr_surface *s) {
    *s = ((run *)user)->out;
    return SHR_OK;
}

static shr_status out_present(void *user, const shr_surface *s, uint64_t id) {
    (void)s, (void)id;
    ((run *)user)->presents++;
    return SHR_OK;
}

static void out_discard(void *user, const shr_surface *s) { (void)user, (void)s; }

static uint64_t scene_clock(void *user) { return ((run *)user)->s.now; }

static void on_log(void *user, shr_status st, const char *msg) {
    run *r = user;
    snprintf(r->log, sizeof(r->log), "%s (%s)", msg, shr_status_name(st));
}

static shr_status open_package(void *user, const char *name, shr_asset_source *out) {
    run *r = user;
    return stage_open_package(&r->s, r->flags, SHR_FONT_DIR, name, out);
}

/* Submits and pumps until nothing is due now; fails on failed frames and unexpected resource failures. */
static shr_status frame(run *r) {
    shr_context *ctx = r->s.ctx;
    if (!stage_ok(&r->s, shr_submit(ctx), "submit")) return r->s.st;
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    int i = 0;
    for (; i < 100000 && dl.kind == SHR_DEADLINE_NOW; i++) {
        shr_pump(ctx);
        shr_next_deadline(ctx, &dl);
    }
    if (dl.kind == SHR_DEADLINE_NOW) stage_ok(&r->s, SHR_E_TIMEOUT, "frame does not settle");
    shr_event ev;
    while (shr_poll_event(ctx, &ev) == SHR_OK) {
        bool resource = ev.kind == SHR_EVENT_RESOURCE_FAILED || ev.kind == SHR_EVENT_OVERFLOW;
        if (ev.kind == SHR_EVENT_PRESENT_FAILED) stage_ok(&r->s, ev.status ? ev.status : SHR_E_STATE, "frame failed");
        if (resource && !(r->flags & RESOURCE_FAILS)) stage_ok(&r->s, ev.status ? ev.status : SHR_E_STATE, "resource failed");
    }
    return r->s.st;
}

static shr_status run_open(run *r, const backend *b, const scene *sc, int page) {
    memset(r, 0, sizeof(*r));
    r->b = b, r->sc = sc, r->flags = sc->flags;
    r->s.page = page, r->s.now = sc->now_ns, r->s.hold = (sc->flags & FONTS_ASYNC) != 0;
    stage *s = &r->s;
    bool quarter = sc->rotation == SHR_ROTATE_90_CW || sc->rotation == SHR_ROTATE_90_CCW;
    shr_pixel_format of = sc->output_format ? sc->output_format : SHR_PIXEL_FORMAT;
    if (!stage_ok(s, b->driver_create(&r->drv), "driver_create")) return s->st;
    r->has_drv = true;
    if (!stage_ok(s, b->surface_create(&r->drv, quarter ? sc->h : sc->w, quarter ? sc->w : sc->h, of, &r->out), "output"))
        return s->st;
    bool composing = sc->rotation || of != SHR_PIXEL_FORMAT || (sc->screen_flags & SHR_SCREEN_COMPOSITION);
    if (composing && !stage_ok(s, b->surface_create(&r->drv, sc->w, sc->h, SHR_PIXEL_FORMAT, &r->comp), "composition"))
        return s->st;
    shr_output output;
    shr_output_init(&output);
    output.user = r, output.acquire = out_acquire, output.present = out_present, output.discard = out_discard;
    output.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | (sc->flags & PRESERVE_NONE ? 0 : SHR_OUTPUT_PRESERVES_CONTENT);
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.user = r, cd.now_ns = scene_clock, cd.log = on_log;
    cd.driver = &r->drv, cd.output = &output;
    cd.blink = (shr_blink_profile){BLINK_NS, 0, true, SHR_BLINK_RESTART_NONE};
    cd.max_commands = 1u << 20, cd.page_cache_bytes = 64u << 20, cd.image_bytes = 64u << 20;
    cd.io_retry_ns = cd.io_timeout_ns = 0;
    if (!stage_ok(s, shr_create(&cd, &s->ctx), "create")) return s->st;
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = sc->w, sd.height = sc->h, sd.rotation = sc->rotation, sd.output_format = sc->output_format;
    sd.flags = sc->screen_flags, sd.composition = composing ? &r->comp : NULL, sd.clear = SHR_RGB(0x1E, 0x1F, 0x29);
    if (!stage_ok(s, shr_screen_configure(s->ctx, &sd), "screen_configure")) return s->st;
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.user = r, fd.open = open_package, fd.locale = "ko";
    stage_ok(s, shr_pl_res_bitmap_font_create(s->ctx, &fd, &s->font), "font_create");
    return s->st;
}

static shr_status run_read(run *r, picture *pic) {
    shr_pixel_format f = r->out.format;
    *pic = (picture){malloc((size_t)r->out.width * (size_t)r->out.height * BPP(f)), r->out.width, r->out.height, f};
    if (!pic->px) return stage_ok(&r->s, SHR_E_NO_MEMORY, "picture"), r->s.st;
    if (!r->presents) stage_ok(&r->s, SHR_E_STATE, "nothing presented");
    stage_ok(&r->s, r->b->surface_read(&r->drv, &r->out, pic->px, (size_t)pic->w * BPP(f)), "surface_read");
    return r->s.st;
}

static void run_close(run *r) {
    stage *s = &r->s;
    shr_context *ctx = s->ctx;
    for (size_t i = 0; i < s->nlayers; i++) shr_lyr_destroy(s->layers[i]);
    for (size_t i = 0; i < s->nimages; i++) shr_pl_res_image_release(s->images[i]);
    if (ctx) {
        shr_begin_shutdown(ctx);
        bool done = false;
        for (int i = 0; i < 64 && !done; i++) {
            shr_pump(ctx);
            if (s->font && shr_pl_res_bitmap_font_destroy(s->font) == SHR_OK) s->font = NULL;
            done = !s->font && shr_destroy(ctx) == SHR_OK;
        }
        if (!done) stage_ok(s, SHR_E_STATE, "context did not shut down");
    }
    if (r->comp.byte_length || r->comp.resource_id) r->b->surface_destroy(&r->drv, &r->comp);
    if (r->out.byte_length || r->out.resource_id) r->b->surface_destroy(&r->drv, &r->out);
    if (r->has_drv) r->b->driver_destroy(&r->drv);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* Renders the scene; with `times` (frames + 1 entries) also times the first frame and `frames` full redraws. */
static shr_status render(const backend *b, const scene *sc, int page, picture *pic, double *times, long frames,
                         char *why, size_t why_len) {
    static run r;
    *pic = (picture){0};
    if (run_open(&r, b, sc, page) == SHR_OK && sc->build(&r.s) == SHR_OK) {
        double t0 = now_ms();
        if (frame(&r) == SHR_OK && sc->update && sc->update(&r.s) == SHR_OK) frame(&r);
        b->finish();
        if (times) times[0] = now_ms() - t0;
        for (long i = 0; times && i < frames && r.s.st == SHR_OK; i++) {
            int presents = r.presents;
            t0 = now_ms();
            if (stage_ok(&r.s, shr_request_redraw(r.s.ctx), "request_redraw")) frame(&r);
            b->finish();
            times[i + 1] = now_ms() - t0;
            if (r.presents == presents) stage_ok(&r.s, SHR_E_STATE, "redraw presented nothing");
        }
        if (r.s.st == SHR_OK) run_read(&r, pic);
    }
    run_close(&r);
    if (r.s.st != SHR_OK)
        snprintf(why, why_len, "%s%s%.200s", r.s.what, r.log[0] ? "; last log: " : "", r.log);
    return r.s.st;
}

/* ===== Pictures ===== */

static void rgb_of(const picture *p, size_t i, uint32_t c[3]) {
    if (p->fmt == SHR_FORMAT_RGB565) {
        uint16_t v;
        memcpy(&v, p->px + 2 * i, 2);
        c[0] = v >> 11 & 31, c[1] = v >> 5 & 63, c[2] = v & 31;
    } else {
        for (int k = 0; k < 3; k++) c[k] = p->px[4 * i + (size_t)k];
    }
}

static uint8_t *to_rgb8(const picture *p) {
    size_t n = (size_t)p->w * (size_t)p->h;
    uint8_t *rgb = malloc(n * 3 + 1);
    for (size_t i = 0; rgb && i < n; i++) {
        uint32_t c[3];
        rgb_of(p, i, c);
        if (p->fmt == SHR_FORMAT_RGB565) c[0] = (c[0] * 255 + 15) / 31, c[1] = (c[1] * 255 + 31) / 63, c[2] = (c[2] * 255 + 15) / 31;
        for (int k = 0; k < 3; k++) rgb[i * 3 + (size_t)k] = (uint8_t)c[k];
    }
    return rgb;
}

static void make_dirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof(p), "%s", path);
    for (char *q = p + 1; *q; q++)
        if (*q == '/') *q = 0, mkdir(p, 0777), *q = '/';
    mkdir(p, 0777);
}

static void picture_path(char *path, size_t len, const char *dir, const char *name, const char *suffix) {
    snprintf(path, len, "%s/%s-%s-%dx%d-%s%s.png", dir, name, opt.backend->name, CW, CH, FMT_NAME(SHR_PIXEL_FORMAT), suffix);
}

static void save(const picture *p, const char *dir, const char *name, const char *suffix) {
    char path[1200];
    make_dirs(dir);
    picture_path(path, sizeof(path), dir, name, suffix);
    uint8_t *rgb = to_rgb8(p);
    if (!rgb || png_write(path, p->w, p->h, rgb) != SHR_OK) fprintf(stderr, "could not write %s\n", path);
    free(rgb);
}

typedef struct diffstat {
    uint32_t max_delta;
    uint64_t pixels;
} diffstat;

/* Native channel depths, X ignored. Sizes or formats that differ count as every pixel at the largest delta. */
static diffstat compare(const picture *a, const picture *b) {
    diffstat d = {0, 0};
    if (a->w != b->w || a->h != b->h || a->fmt != b->fmt) return (diffstat){255, UINT64_MAX};
    for (size_t i = 0; i < (size_t)a->w * (size_t)a->h; i++) {
        uint32_t x[3], y[3], m = 0;
        rgb_of(a, i, x), rgb_of(b, i, y);
        for (int k = 0; k < 3; k++) {
            uint32_t dk = x[k] > y[k] ? x[k] - y[k] : y[k] - x[k];
            m = dk > m ? dk : m;
        }
        d.pixels += m > 0, d.max_delta = m > d.max_delta ? m : d.max_delta;
    }
    return d;
}

static void save_failure(const char *name, const picture *expected, const picture *actual) {
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s/failures", opt.out);
    if (expected) save(expected, dir, name, "-expected");
    save(actual, dir, name, "-actual");
    if (!expected || expected->w != actual->w || expected->h != actual->h || expected->fmt != actual->fmt) return;
    picture diff = *actual;
    diff.px = malloc((size_t)diff.w * (size_t)diff.h * BPP(diff.fmt));
    if (!diff.px) return;
    for (size_t i = 0; i < (size_t)diff.w * (size_t)diff.h; i++) {
        uint32_t x[3], y[3];
        rgb_of(expected, i, x), rgb_of(actual, i, y);
        bool same = x[0] == y[0] && x[1] == y[1] && x[2] == y[2];
        if (diff.fmt == SHR_FORMAT_RGB565) {
            uint16_t v = same ? (uint16_t)((x[0] / 4) << 11 | (x[1] / 4) << 5 | x[2] / 4) : (uint16_t)0xF800;
            memcpy(diff.px + 2 * i, &v, 2);
        } else {
            uint8_t v[4] = {same ? (uint8_t)(x[0] / 4) : 255, same ? (uint8_t)(x[1] / 4) : 0, same ? (uint8_t)(x[2] / 4) : 0, 0};
            memcpy(diff.px + 4 * i, v, 4);
        }
    }
    save(&diff, dir, name, "-diff");
    free(diff.px);
}

/* ===== Scene instances ===== */

typedef struct item {
    const scene *sc;
    int page;
    char name[64];
    picture pic;     /* this backend */
    picture sw;      /* software, for GPU comparison */
    bool rendered, failed;
} item;

static item *items;
static size_t item_count;

static void items_build(void) {
    size_t cap = 0;
    for (size_t i = 0; i < scene_count; i++) {
        int pages = scenes[i].pages ? scenes[i].pages() : 1;
        for (int p = 1; p <= pages; p++) {
            if (item_count == cap) {
                item *grown = realloc(items, (cap = cap ? cap * 2 : 128) * sizeof(item));
                if (!grown) abort();
                items = grown;
            }
            item *it = &items[item_count++];
            *it = (item){.sc = &scenes[i], .page = p};
            if (scenes[i].pages)
                snprintf(it->name, sizeof(it->name), "%s-%d", scenes[i].name, p);
            else
                snprintf(it->name, sizeof(it->name), "%s", scenes[i].name);
        }
    }
}

static item *item_find(const char *name) {
    for (size_t i = 0; i < item_count; i++)
        if (!strcmp(items[i].name, name)) return &items[i];
    return NULL;
}

/* Renders once per process and backend; failures are reported by every test that needs the picture. */
static shr_status item_render(item *it, char *why, size_t why_len) {
    if (!it->rendered) {
        it->rendered = true;
        it->failed = render(opt.backend, it->sc, it->page, &it->pic, NULL, 0, why, why_len) != SHR_OK;
        if (!it->failed && opt.export_all) save(&it->pic, opt.out, it->name, "");
        if (!it->failed && opt.backend != &software)
            it->failed = render(&software, it->sc, it->page, &it->sw, NULL, 0, why, why_len) != SHR_OK;
    } else if (it->failed) {
        snprintf(why, why_len, "rendering failed earlier");
    }
    return it->failed ? SHR_E_STATE : SHR_OK;
}

/* ===== Golden list ===== */

static char (*golden)[160], (*results)[160];
static size_t golden_count, result_count;
static bool golden_this_cell, golden_skipped;

static void golden_load(void) {
    golden = calloc(4096, sizeof(*golden)), results = calloc(4096, sizeof(*results));
    if (!golden || !results) abort();
    FILE *f = fopen(SHR_GOLDEN_DIR "/golden.txt", "r");
    char cell[16];
    snprintf(cell, sizeof(cell), "-%dx%d-", CW, CH);
    while (f && golden_count < 4096 && fgets(golden[golden_count], 160, f)) {
        char *line = golden[golden_count];
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        golden_this_cell |= strstr(line, cell) && (size_t)(strstr(line, cell) - line) < strcspn(line, " ");
        golden_count++;
    }
    if (f) fclose(f);
}

static void golden_update(void) {
    const char *path = SHR_GOLDEN_DIR "/golden.txt";
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    for (size_t i = 0; i < golden_count; i++) {
        size_t n = strcspn(golden[i], " ");
        bool ours = false;
        for (size_t k = 0; k < result_count && !ours; k++) ours = !strncmp(golden[i], results[k], n) && results[k][n] == ' ';
        if (!ours) fprintf(f, "%s\n", golden[i]);
    }
    for (size_t k = 0; k < result_count; k++) fprintf(f, "%s\n", results[k]);
    fclose(f);
    printf("updated %s\n", path);
}

TEST golden_test(void *arg) {
    item *it = arg;
    static char why[512];
    if (item_render(it, why, sizeof(why)) != SHR_OK) FAILm(why);
    picture *p = &it->pic;
    XXH128_hash_t h = XXH3_128bits(p->px, (size_t)p->w * (size_t)p->h * BPP(p->fmt));
    char *line = results[result_count++];
    snprintf(line, 160, "%s-%dx%d-%s %016llx%016llx", it->name, CW, CH, FMT_NAME(SHR_PIXEL_FORMAT),
             (unsigned long long)h.high64, (unsigned long long)h.low64);
    if (getenv("SHR_UPDATE_GOLDEN")) PASS();
    if (!golden_this_cell) {
        golden_skipped = true;
        SKIPm("no golden entries for this cell size");
    }
    size_t n = strcspn(line, " ");
    for (size_t i = 0; i < golden_count; i++) {
        if (strncmp(golden[i], line, n) || golden[i][n] != ' ') continue;
        if (!strcmp(golden[i], line)) PASS();
        save_failure(it->name, NULL, p);
        fprintf(stderr, "golden mismatch: %s\n  expected %s\n", line, golden[i]);
        FAILm("golden mismatch (actual image in failures/)");
    }
    fprintf(stderr, "no golden entry: %s (SHR_UPDATE_GOLDEN=1 records it)\n", line);
    FAILm("no golden entry");
}

/* ===== Reftests and GPU comparison ===== */

static fuzz fuzz_of(const item *it) {
    if (opt.backend == &software) return (fuzz){0, 0};
    fuzz f = it->sc->gpu;
    if (f.max_delta || f.max_pixels || SHR_PIXEL_FORMAT == SHR_FORMAT_RGBX8888) return f;
    /* RGB565 blends: the GPU may blend 16-bit targets at reduced precision, one step off on under 1 % of the
     * pixels (Metal); an RGB565 screen converted to RGBX8888 scales the step to 9. */
    bool widened = it->sc->output_format == SHR_FORMAT_RGBX8888;
    uint64_t pixels = (uint64_t)it->sc->w * (uint64_t)it->sc->h;
    return (fuzz){widened ? 9u : 1u, pixels / 50};
}

static bool within(diffstat d, fuzz f) { return d.max_delta <= f.max_delta && (d.pixels <= f.max_pixels || !d.max_delta); }

TEST reftest_test(void *arg) {
    const reftest *rt = arg;
    item *a = item_find(rt->test), *b = item_find(rt->ref);
    static char why[512];
    if (!a || !b) FAILm("reftest names an unknown scene");
    if (item_render(a, why, sizeof(why)) != SHR_OK || item_render(b, why, sizeof(why)) != SHR_OK) FAILm(why);
    diffstat d = compare(&b->pic, &a->pic);
    fuzz f = fuzz_of(a);
    if (within(d, f)) PASS();
    save_failure(a->name, &b->pic, &a->pic);
    fprintf(stderr, "reftest %s == %s: %llu pixels differ, max delta %u (allowed %u on %llu)\n", rt->test, rt->ref,
            (unsigned long long)d.pixels, d.max_delta, f.max_delta, (unsigned long long)f.max_pixels);
    FAILm("reftest mismatch (images in failures/)");
}

TEST gpu_test(void *arg) {
    item *it = arg;
    static char why[512];
    if (item_render(it, why, sizeof(why)) != SHR_OK) FAILm(why);
    diffstat d = compare(&it->sw, &it->pic);
    fuzz f = fuzz_of(it);
    printf("%s: %llu pixels differ from software, max delta %u\n", it->name, (unsigned long long)d.pixels, d.max_delta);
    if (within(d, f)) PASS();
    save_failure(it->name, &it->sw, &it->pic);
    FAILm("differs from the software image beyond the fuzzy limit (images in failures/)");
}

/* ===== Frame timing ===== */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int frames_mode(void) {
    double *t = malloc(((size_t)opt.frames + 1) * sizeof(double));
    if (!t) return EXIT_FAILURE;
    int failed = 0;
    printf("%-28s %10s %10s %10s %10s %10s  (ms, %s, %ld frames)\n", "scene", "first", "p50", "p95", "p99", "max",
           opt.backend->name, opt.frames);
    for (size_t i = 0; i < item_count; i++) {
        item *it = &items[i];
        if (opt.filter && !strstr(it->name, opt.filter)) continue;
        char why[512] = "";
        picture pic;
        if (render(opt.backend, it->sc, it->page, &pic, t, opt.frames, why, sizeof(why)) != SHR_OK) {
            printf("%-28s failed: %s\n", it->name, why);
            failed = 1;
            continue;
        }
        free(pic.px);
        qsort(t + 1, (size_t)opt.frames, sizeof(double), cmp_double);
        double q[3] = {0.5, 0.95, 0.99}, v[3];
        for (int k = 0; k < 3; k++) {
            size_t idx = (size_t)ceil(q[k] * (double)opt.frames);
            v[k] = t[1 + (idx ? idx - 1 : 0)];
        }
        printf("%-28s %10.3f %10.3f %10.3f %10.3f %10.3f\n", it->name, t[0], v[0], v[1], v[2], t[opt.frames]);
    }
    free(t);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* ===== Main ===== */

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    int kept = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--backend") && more) {
            const char *name = argv[++i];
            opt.backend = !strcmp(name, "software") ? &software : NULL;
#ifdef SHR_RENDER_ANGLE
            if (!strcmp(name, "angle")) opt.backend = &angle;
#endif
            if (!opt.backend) {
                fprintf(stderr, "unknown backend %s\n", name);
                return EXIT_FAILURE;
            }
        } else if (!strcmp(a, "--out") && more) {
            opt.out = argv[++i];
        } else if (!strcmp(a, "--frames") && more) {
            opt.frames = strtol(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--golden")) {
            opt.golden = true;
        } else if (!strcmp(a, "--reftest")) {
            opt.reftest = true;
        } else if (!strcmp(a, "--export")) {
            opt.export_all = true;
        } else {
            if (!strcmp(a, "-t") && more) opt.filter = argv[i + 1];
            argv[kept++] = argv[i];
        }
    }
    argc = kept;
    if (!opt.golden && !opt.reftest) opt.golden = opt.reftest = true;
#ifdef SHR_RENDER_ANGLE
    shr_angle_offscreen *gl = NULL;
    if (opt.backend == &angle) {
        if (shr_angle_offscreen_create(16, 16, &gl) != SHR_OK) {
            fprintf(stderr, "no ANGLE offscreen context\n");
            return EXIT_FAILURE;
        }
        printf("ANGLE backend: %s\n", shr_angle_offscreen_backend(gl));
    }
#endif
    items_build();
    int result;
    if (opt.frames > 0) {
        result = frames_mode();
    } else {
        GREATEST_MAIN_BEGIN();
        if (opt.golden) golden_load();
        /* volatile: failing tests longjmp back into this frame. */
        for (volatile size_t i = 0; i < item_count; i++) {
            greatest_set_test_suffix(items[i].name);
            if (opt.backend != &software)
                RUN_TEST1(gpu_test, &items[i]);
            else if (opt.golden)
                RUN_TEST1(golden_test, &items[i]);
        }
        for (volatile size_t i = 0; opt.reftest && i < reftest_count; i++) {
            greatest_set_test_suffix(reftests[i].test);
            RUN_TEST1(reftest_test, (void *)&reftests[i]);
        }
        if (opt.golden && opt.backend == &software && getenv("SHR_UPDATE_GOLDEN")) golden_update();
        GREATEST_PRINT_REPORT();
        result = !greatest_all_passed() ? EXIT_FAILURE : golden_skipped ? 77 : EXIT_SUCCESS;
    }
    for (size_t i = 0; i < item_count; i++) free(items[i].pic.px), free(items[i].sw.px);
    free(items), free(golden), free(results);
#ifdef SHR_RENDER_ANGLE
    if (gl) shr_angle_offscreen_destroy(gl);
#endif
    return result;
}
