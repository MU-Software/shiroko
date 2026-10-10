/* Zero allocations after warm-up: each scene of the Tab5 example (tab5_scenes.h) and a terminal drawn with set_text
 * run on the Tab5's screen configuration (bands, keeps, image planes) with every allocation (context, plugins,
 * driver) from one allocator, which refuses all of them after ALLOCZERO_WARM loop frames (or SHR_ALLOCZERO_WARM in
 * the environment): the next ALLOCZERO_FRAMES (SHR_ALLOCZERO_FRAMES) must not ask for memory. Scenes on paths that still allocate by design say why and only report. */
#include "check.h"
#include "tab5_scenes.h"

#include <shiroko/port_software.h>

#ifndef ALLOCZERO_WARM /* 0: the scene's own frames, as the Tab5 example runs it */
#define ALLOCZERO_WARM 0
#endif
#ifndef ALLOCZERO_FRAMES
#define ALLOCZERO_FRAMES 20
#endif
#ifndef ALLOCZERO_W /* the landscape screen; the Tab5's is 1280 x 720 */
#define ALLOCZERO_W 1280
#endif
#ifndef ALLOCZERO_H
#define ALLOCZERO_H 720
#endif
#define BAND_H 16 /* as examples/tab5 */
#define BPP (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? 2 : 4)
#define KEEPS (4 * (ALLOCZERO_H / SHR_CELL_HEIGHT)) /* four screens of rows, as examples/tab5 */
#define VT LOADS                                     /* the set_text terminal */
#define VT_LONG (LOADS + 1)                          /* the same with a cluster of over 12 bytes in each line */

/* The context's (and its plugins') allocations and the driver's: refused at budget 0; asked: all requests; live: bytes
 * by kind, then by kind with SHR_ALLOC_HOT (frees must name the kind of their allocation); hot: the most hot bytes. */
typedef struct counted {
    fail_alloc f;
    long refused, asked;
    size_t size; /* of the first refused */
    shr_alloc_kind kind;
    long long live[6], hot;
} counted;

static counted mem[2];
static int warm = ALLOCZERO_WARM, frames = ALLOCZERO_FRAMES;

static long long *live(counted *c, shr_alloc_kind kind) { return &c->live[(kind & 3) + (kind & SHR_ALLOC_HOT ? 3 : 0)]; }

static long long hot(const counted *c) { return c->live[3] + c->live[4] + c->live[5]; }

static void *za_alloc(void *user, size_t size, size_t align, shr_alloc_kind kind) {
    counted *c = user;
    c->asked++;
    if (c->f.budget == 0 && !c->refused++) c->size = size, c->kind = kind;
    void *p = fa_alloc(&c->f, size, align, kind);
    if (p) *live(c, kind) += (long long)size, c->hot = hot(c) > c->hot ? hot(c) : c->hot;
    return p;
}

static void za_free(void *user, void *p, size_t size, size_t align, shr_alloc_kind kind) {
    *live(user, kind) -= (long long)size;
    fa_free(&((counted *)user)->f, p, size, align, kind);
}

static const shr_allocator za[2] = {{&mem[0], za_alloc, za_free, SHR_ALLOC_HOT}, {&mem[1], za_alloc, za_free, SHR_ALLOC_HOT}};

static shr_surface targets[3];

static shr_status out_acquire(void *user, shr_surface *s) {
    (void)user;
    *s = targets[0];
    return SHR_OK;
}

static shr_status out_present(void *user, const shr_surface *s, uint64_t id) {
    (void)user, (void)s, (void)id;
    return SHR_OK;
}

static void out_discard(void *user, const shr_surface *s) { (void)user, (void)s; }

static uint64_t no_clock(void *user) { return (void)user, 0; }

static shr_status open_package(void *user, const char *name, shr_asset_source *out) {
    (void)user;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", SHR_FONT_DIR, name);
    return shr_asset_source_file(path, out);
}

static shr_surface surface(int32_t w, int32_t h, uint32_t generation) {
    size_t stride = (size_t)w * BPP;
    void *px = aligned_alloc(128, stride * (size_t)h);
    if (!px) FAIL_WITH_LONGJMPm("surface");
    return (shr_surface){px, w, h, stride, stride * (size_t)h, SHR_PIXEL_FORMAT, generation, SHR_MEMORY_CPU, 0};
}

/* A line of prose with its words' styles as runs, the status line, every 8th frame a wrapped paragraph from the top,
 * the grid scrolled a row first, the sprites moved and one recorded anew, as a terminal draws with set_text. */
static void vt_step(t5_scene *s, bool long_cluster) {
    static const char family[] = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    char line[1024];
    shr_style_run runs[256];
    size_t len = 0, n = 0;
    s->tick++;
    t5_try(s, shr_pl_lyr_tilemap_scroll(s->grid, 0, s->rows - 1, 1, (shr_text_style){0}), "scroll");
    if (long_cluster) memcpy(line, family, sizeof(family) - 1), len = sizeof(family) - 1;
    for (int32_t c = long_cluster ? 2 : 0; c < s->cols;) {
        uint32_t span;
        shr_text_style st;
        char t[8];
        ts_prose_next(&s->prose, t, &span, &st);
        size_t k = strlen(t);
        if (n && runs[n - 1].byte_end == len && !memcmp(&runs[n - 1].style, &st, sizeof(st)))
            runs[n - 1].byte_end += k;
        else
            runs[n++] = (shr_style_run){len, len + k, st};
        memcpy(line + len, t, k), len += k, c += (int32_t)span;
    }
    t5_try(s, shr_pl_lyr_tilemap_set_text(s->grid, s->rows - 2, 0, line, len, (shr_text_style){0}, runs, n, 0, NULL),
           "set_text");
    const shr_text_style bar = {SHR_RGB(0xF8, 0xF8, 0xF2), SHR_RGB(0x44, 0x47, 0x5A), 0};
    len = (size_t)snprintf(line, sizeof(line), " frame %4d \xE2\x94\x82 \xED\x95\x9C\xEA\xB8\x80 \xF0\x9F\x98\x80\t%s",
                           (int)s->tick, s->tick & 1 ? "ok" : "--");
    const shr_style_run hl = {1, 11, {SHR_RGB(0x50, 0xFA, 0x7B), bar.bg, SHR_STYLE_BOLD}};
    t5_try(s, shr_pl_lyr_tilemap_set_text(s->grid, s->rows - 1, 0, line, len, bar, &hl, 1, 0, NULL), "set_text");
    if (s->tick % 8 == 0) {
        len = 0;
        for (int i = 0; i < 3 * s->cols / 2; i++) {
            char t[8];
            uint32_t span;
            shr_text_style st;
            ts_prose_next(&s->prose, t, &span, &st);
            size_t k = strlen(t);
            memcpy(line + len, t, k), len += k;
        }
        t5_try(s, shr_pl_lyr_tilemap_set_text(s->grid, 0, 4, line, len, (shr_text_style){SHR_RGB(0xF1, 0xFA, 0x8C), 0, 0},
                                              NULL, 0, SHR_TEXT_WRAP, NULL),
               "set_text");
    }
    move_sprites(s);
    shr_lyr *l = s->sprites[0].l; /* recorded anew, as an overlay redrawn every frame */
    int32_t bar_w = (int32_t)(s->tick % T5_SPRITE) + 1;
    t5_try(s, shr_lyr_cmd_begin(l), "cmd_begin");
    t5_try(s, shr_lyr_cmd_image(l, s->img[s->tick % 2], (shr_rect){0, 0, T5_SPRITE, T5_SPRITE}, (shr_point){0, 0}),
           "cmd_image");
    t5_try(s, shr_lyr_cmd_fill(l, (shr_rect){0, T5_SPRITE - 4, bar_w, T5_SPRITE}, SHR_RGB(0x50, 0xFA, 0x7B)), "cmd_fill");
    t5_try(s, shr_lyr_cmd_commit(l), "commit");
}

static bool step(t5_scene *s, int load) {
    if (load < LOADS) return load_step(s);
    vt_step(s, load == VT_LONG);
    return true;
}

typedef struct run_result {
    long refused[2]; /* context, driver: in the last `frames` frames */
    int last_warm;   /* the last warm-up loop frame that allocated, -1: none */
    long warm_asks;  /* allocations in the warm-up loop frames */
    long long hot[2], payload; /* hot bytes at the end and the most; other payload at the end */
} run_result;

static run_result run(int load) {
    t5_scene s = {0};
    t5_scene_reset(&s, load < LOADS ? load : SCROLL_API, ALLOCZERO_W, ALLOCZERO_H);
    if (load >= LOADS) s.nsprites = 2, s.prose.mix = true;
    s.now_ns = no_clock, s.frozen = T5_REC_T0_NS;
    mem[0] = mem[1] = (counted){{-1, 0}, 0, 0, 0, 0, {0}, 0};
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_software_driver_create(&za[1], KEEPS * ((uint64_t)ALLOCZERO_W * BAND_H * BPP), KEEPS, 256, &d), SHR_OK);
    ASSERT_EQ_LL(shr_software_driver_image_planes(&d, 2u << 20), SHR_OK);
    d.caps.flags |= SHR_DRIVER_CHEAP_STORE;
    shr_output output;
    shr_output_init(&output);
    output.acquire = out_acquire, output.present = out_present, output.discard = out_discard;
    output.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT;
    shr_context_desc cd;
    t5_context_desc(&s, &cd);
    cd.output = &output, cd.driver = &d, cd.allocator = &za[0];
    shr_screen_desc sd;
    t5_screen_desc(&s, &sd);
    sd.bands = targets + 1, sd.band_count = 2, sd.band_align = 16;
    ASSERT_EQ_LL(shr_create(&cd, &s.ctx), SHR_OK);
    ASSERT_EQ_LL(shr_screen_configure(s.ctx, &sd), SHR_OK);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = open_package, fd.locale = "ko";
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(s.ctx, &fd, &s.font), SHR_OK);
    t5_preload(&s);
    t5_frame(&s, false);
    t5_scene_fill(&s);
    t5_frame(&s, true);
    run_result r = {{0, 0}, -1, 0, {0, 0}, 0};
    int n = warm ? warm : s.frames;
    for (int f = 0; f < n + frames; f++) {
        if (f == n) mem[0].f.budget = mem[1].f.budget = 0;
        long was = mem[0].asked + mem[1].asked;
        s.frozen += T5_REC_STEP_NS;
        t5_frame(&s, step(&s, load));
        long now = mem[0].asked + mem[1].asked;
        if (f < n && now > was) r.last_warm = f, r.warm_asks += now - was;
    }
    r.refused[0] = mem[0].refused, r.refused[1] = mem[1].refused;
    r.hot[0] = hot(&mem[0]) + hot(&mem[1]), r.hot[1] = mem[0].hot + mem[1].hot;
    r.payload = mem[0].live[SHR_ALLOC_PAYLOAD] + mem[1].live[SHR_ALLOC_PAYLOAD];
    mem[0].f.budget = mem[1].f.budget = -1;
    t5_hold_last_phase(&s, 0);
    bool clean = !r.refused[0] && !r.refused[1];
    if (clean) {
        ASSERT_EQ_LL(s.st, SHR_OK);
        ASSERT_EQ_LL(s.resource_failed, 0);
    }
    t5_scene_close(&s);
    ASSERT_EQ_LL(s.st == SHR_OK || !clean, 1);
    ASSERT_EQ_LL(shr_software_driver_destroy(&d), SHR_OK);
    ASSERT_EQ_LL(mem[0].f.live + mem[1].f.live, 0);
    for (int k = 0; k < 6; k++) ASSERT_EQ_LL(mem[0].live[k] + mem[1].live[k], 0);
    return r;
}

/* Paths that allocate by design, by scene (NULL: none, the scene must not allocate). */
static const char *known(int load) {
    switch (load) {
    case VT_LONG: return "cells keep text over 12 bytes on the heap: each new one allocates, set_text copies it anew";
    default: return NULL;
    }
}

TEST scenes_allocate_nothing_after_warm_up(void) {
    targets[0] = surface(ALLOCZERO_H, ALLOCZERO_W, 0);
    targets[1] = surface(ALLOCZERO_W, BAND_H, 1), targets[2] = surface(ALLOCZERO_W, BAND_H, 1);
    int bad = 0;
    for (int load = 0; load <= VT_LONG; load++) {
        run_result r = run(load);
        const char *why = known(load);
        printf("alloczero %-18s last warm-up frame allocating %3d (%ld allocations), last %d frames: ctx %ld drv %ld",
               load < LOADS ? load_names[load] : load == VT ? "vt" : "vt-long", r.last_warm, r.warm_asks, frames,
               r.refused[0], r.refused[1]);
        for (int k = 0; k < 2; k++)
            if (r.refused[k]) printf(" (%s first %zu B kind %d)", k ? "drv" : "ctx", mem[k].size, (int)mem[k].kind);
        printf(", hot %lld KiB (most %lld), other payload %lld KiB", r.hot[0] >> 10, r.hot[1] >> 10, r.payload >> 10);
        printf("%s%s\n", why ? " known: " : "", why ? why : "");
        bad += !why && (r.refused[0] || r.refused[1]);
    }
    for (int k = 0; k < 3; k++) free(targets[k].pixels);
    ASSERT_EQ_LL(bad, 0);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    const char *w = getenv("SHR_ALLOCZERO_WARM"), *k = getenv("SHR_ALLOCZERO_FRAMES");
    warm = w ? atoi(w) : warm, frames = k ? atoi(k) : frames;
    GREATEST_MAIN_BEGIN();
    RUN_TEST(scenes_allocate_nothing_after_warm_up);
    GREATEST_MAIN_END();
}
