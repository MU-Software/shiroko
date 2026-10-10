#define _POSIX_C_SOURCE 200809L
/* Replay benchmark: records the Tab5 example's scenes (tab5_scenes.h) on the Tab5 screen configuration with the held
 * clock, then replays them into one part alone and times it (B and BS lines, as the Tab5 example prints with
 * TAB5_REPLAY): the recorded batches into a new software driver (driver-sw), and the recorded calls into new plugins
 * and a context over a driver drawing nothing (compositor, with the api line), whose commands must hash as recorded.
 * Usage: shiroko_replay FONT_DIR [SCENE[,SCENE...]|all|cost] [FRAMES]; FRAMES loop frames are recorded (default 60,
 * churn 30); cost: the scenes of the app cost table. SHR_REPLAY_PROFILE=tab5 (the only one so far),
 * SHR_REPLAY_PART=driver-sw or compositor (default both), SHR_REPLAY_REPS (default 5), SHR_REPLAY_FRAMES=1 adds BF lines
 * per frame, SHR_REPLAY_SAVE=DIR / SHR_REPLAY_LOAD=DIR write or read DIR/<scene>.shrr (commands) and
 * DIR/<scene>.calls.shrr instead of recording. SHR_REPLAY_KEEPS: the driver's keep slots in screens of rows, and
 * SHR_REPLAY_BANDS: the bands (1 or 2); 4 and 2 by default, the Tab5 example's defaults, so the hashes match the
 * device's (set them as its CONFIG_SHIROKO_TAB5_KEEP_SCREENS and BANDS); recordings loaded must have been made with
 * the same counts. */
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rec.h"
#include "rec_api.h"
#include "tab5_scenes.h"

/* The Tab5: a 720 x 1280 portrait panel, the screen drawn in landscape by bands of 1280 x 16 the PPA turns. */
#define OUT_W 720
#define OUT_H 1280
#define BAND_H 16
#define BAND_ALIGN 16

static struct {
    const char *fonts;
    uint32_t keeps;
    uint32_t ntargets; /* the frame buffer and the bands */
    shr_surface targets[3];
    rec_package packages[T5_N(t5_packages)];
    t5_scene s;
} g;

static void check(shr_status st, const char *what) {
    if (st == SHR_OK) return;
    fprintf(stderr, "%s: %s\n", what, shr_status_name(st));
    exit(1);
}

static uint64_t now_ns(void *user) {
    (void)user;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static shr_status out_acquire(void *user, shr_surface *s) {
    (void)user;
    *s = g.targets[0];
    return SHR_OK;
}

static shr_status out_present(void *user, const shr_surface *s, uint64_t id) {
    (void)user, (void)s, (void)id;
    return SHR_OK;
}

static void out_discard(void *user, const shr_surface *s) { (void)user, (void)s; }

static shr_status open_package(void *user, const char *name, shr_asset_source *out) {
    (void)user;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g.fonts, name);
    return shr_asset_source_file(path, out);
}

static uint32_t fb_checksum(void *user) {
    (void)user;
    return rec_hash(REC_HASH_INIT, g.targets[0].pixels, g.targets[0].byte_length);
}

static void *mem_alloc(void *user, size_t bytes) {
    (void)user;
    return aligned_alloc(128, (bytes + 127) / 128 * 128);
}

static void mem_free(void *user, void *p) { (void)user, free(p); }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long n = f && !fseek(f, 0, SEEK_END) ? ftell(f) : -1;
    if (n >= 0 && !fseek(f, 0, SEEK_SET) && (b = malloc((size_t)n + 1)) && fread(b, 1, (size_t)n, f) != (size_t)n)
        free(b), b = NULL;
    if (f) fclose(f);
    *len = (size_t)n;
    return b;
}

/* The record pass of `load` into the commands w and the calls c (NULL: none; buffers set): returns the screen checksum
 * at its end. */
static uint32_t record(int load, int32_t frames, shr_framebuffer_driver *sw, rec_writer *w, rec_calls *c) {
    t5_scene *s = &g.s;
    t5_scene_reset(s, load, OUT_H, OUT_W);
    s->now_ns = now_ns, s->sleep = NULL, s->frozen = T5_REC_T0_NS;
    shr_output output;
    shr_output_init(&output);
    output.acquire = out_acquire, output.present = out_present, output.discard = out_discard;
    output.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT;
    shr_context_desc cd;
    t5_context_desc(s, &cd);
    cd.output = &output;
    shr_screen_desc sd;
    t5_screen_desc(s, &sd);
    sd.bands = g.targets + 1, sd.band_count = g.ntargets - 1, sd.band_align = BAND_ALIGN;
    rec_profile p = {load_names[load], &cd, &sd, T5_REC_STEP_NS, frames, g.packages, T5_N(g.packages)};
    check(rec_begin(w, &p, sw, g.targets, g.ntargets), "rec_begin");
    rec_frame(w, REC_OPEN, s->frozen);
    if (c) {
        check(rec_calls_begin(c, &p, &sw->caps, g.targets, g.ntargets), "rec_calls_begin");
        rec_frame(&c->w, REC_OPEN, s->frozen);
    }
    cd.driver = &w->drv;
    check(shr_create(&cd, &s->ctx), "create");
    check(shr_screen_configure(s->ctx, &sd), "screen_configure");
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = open_package, fd.locale = "ko";
    check(shr_pl_res_bitmap_font_create(s->ctx, &fd, &s->font), "font_create");
    t5_preload(s);
    t5_frame(s, false);
    t5_scene_fill(s);
    t5_frame(s, true);
    check(s->st, s->what);
    uint32_t sum = t5_record_loop(s, w, c ? &c->w : NULL, fb_checksum, NULL, NULL);
    check(s->st, s->what);
    if (c) check(c->w.st, "call recording"), check(rec_calls_end(c), "rec_calls_end");
    t5_scene_close(s);
    check(s->st, s->what);
    check(w->st, "recording");
    if (!w->hash_only) check(rec_end(w), "rec_end");
    return sum;
}

static shr_framebuffer_driver new_driver(void) {
    shr_framebuffer_driver d;
    check(shr_software_driver_create(NULL, g.keeps * (OUT_H * SHR_CELL_HEIGHT * 2ull), g.keeps, 256, &d), "driver");
    check(shr_software_driver_image_planes(&d, 2u << 20), "planes"); /* as examples/tab5 */
    d.caps.flags |= SHR_DRIVER_CHEAP_STORE; /* as the Tab5's, whose keeps copy by DMA2D */
    return d;
}

static void save_file(const char *dir, const char *scene, const char *ext, const uint8_t *buf, size_t len) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s%s", dir, scene, ext);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, len, f) != len || fclose(f)) check(SHR_E_IO, path);
}

static uint8_t *load_file(const char *dir, const char *scene, const char *ext, size_t *len) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s%s", dir, scene, ext);
    uint8_t *b = read_file(path, len);
    if (!b) check(SHR_E_IO, path);
    return b;
}

/* The command recording of `load` (recorded, or loaded from load_dir), replayed into the software driver when
 * `drive`; its hash after each recorded frame goes to *ref (*nref words, to free). */
static void replay_driver(int load, int32_t frames, bool drive, int reps, bool per_frame, const char *save,
                          const char *load_dir, uint32_t **ref, uint32_t *nref) {
    size_t len = 0;
    uint8_t *buf = NULL;
    uint32_t pass = 0;
    rec_writer w = {0};
    if (load_dir) {
        buf = load_file(load_dir, load_names[load], ".shrr", &len);
    } else {
        w.cap = 512u << 20, w.buf = buf = malloc(w.cap);
        if (!buf) check(SHR_E_NO_MEMORY, "recording");
        shr_framebuffer_driver sw = new_driver();
        pass = record(load, frames, &sw, &w, NULL);
        check(shr_software_driver_destroy(&sw), "driver");
        len = w.len;
    }
    rec_info in;
    check(rec_scan(buf, len, &in), "rec_scan");
    if (!load_dir) rec_print_pass(&w, &in, pass), rec_free(&w);
    if (save) save_file(save, load_names[load], ".shrr", buf, len);
    if (!(*ref = calloc(in.frames + 1u, sizeof(**ref)))) check(SHR_E_NO_MEMORY, "hashes");
    *nref = in.frames;
    for (uint32_t k = 0; k < in.frames; k++) (*ref)[k] = rec_frame_hash(&in, k);
    uint32_t record_sum = rec_sum(&in, in.frames - 1);
    rec_times *t = calloc(in.frames, sizeof(*t));
    shr_draw_cmd *cmds = calloc(in.max_batch + 1u, sizeof(*cmds));
    rec_stats *st = calloc((size_t)reps, sizeof(*st));
    if (!t || !cmds || !st) check(SHR_E_NO_MEMORY, "replay");
    bool same = true;
    int32_t bad = -2;
    for (int rep = -1; drive && rep < reps; rep++) {
        shr_framebuffer_driver d = new_driver();
        for (uint32_t k = 0; k < g.ntargets; k++) memset(g.targets[k].pixels, 0x5A, g.targets[k].byte_length);
        rec_host h = {&d, g.targets, g.ntargets, cmds, mem_alloc, mem_free, now_ns, NULL, NULL, NULL, fb_checksum, NULL};
        uint32_t sum = 0;
        int32_t b = -2;
        check(rec_play(buf, len, &h, rep < 0, t, &sum, &b), "rec_play");
        check(shr_software_driver_destroy(&d), "driver");
        same &= sum == record_sum && b == -2;
        if (rep < 0) bad = b, rec_print_diff(&in, "driver-sw", t);
        if (rep >= 0)
            st[rep] = rec_print(rep, in.scene, "driver-sw", rec_driver_cols, REC_COLS, t, in.frames - 1, sum,
                                sum == record_sum, in.hash, per_frame);
    }
    if (drive) {
        rec_print_summary(in.scene, "driver-sw", st, reps);
        printf("REPLAY CHECKSUMS rec=%s record %08lx pass %08lx rec-hash %08lx first-bad %d -> %s\n", in.scene,
               (unsigned long)record_sum, (unsigned long)pass, (unsigned long)in.hash, (int)bad, same ? "EQUAL" : "DIFFER");
    }
    free(st), free(cmds), free(t), free(buf);
}

/* The call recording of `load` (recorded with the commands hashed alongside, or loaded), replayed into the compositor
 * over a driver drawing nothing; its commands must hash as `ref` (the command recording's, nref frames). */
static void replay_calls(int load, int32_t frames, int reps, bool per_frame, const char *save, const char *load_dir,
                         const uint32_t *ref, uint32_t nref) {
    size_t len = 0;
    uint8_t *buf = NULL;
    rec_writer v = {.cap = 64u << 20, .hash_only = true};
    if (!(v.buf = malloc(v.cap))) check(SHR_E_NO_MEMORY, "hashes");
    uint32_t pass2 = REC_HASH_INIT;
    if (load_dir) {
        buf = load_file(load_dir, load_names[load], ".calls.shrr", &len);
    } else {
        rec_calls c = {.w = {.cap = 512u << 20}};
        if (!(c.w.buf = buf = malloc(c.w.cap))) check(SHR_E_NO_MEMORY, "recording");
        shr_framebuffer_driver sw = new_driver();
        uint32_t pass = record(load, frames, &sw, &v, &c);
        check(shr_software_driver_destroy(&sw), "driver");
        rec_info in;
        check(rec_scan(buf, c.w.len, &in), "rec_scan");
        pass2 = v.hash, len = c.w.len;
        printf("  RC rec=%s frames=%u/%u pass %08lx bytes %.1f MiB objects %u styles %u calls %llu rec %08lx cmd-hash"
               " %08lx%s\n", in.scene, (unsigned)in.frames - 1, (unsigned)c.w.limit, (unsigned long)pass,
               (double)len / 1048576, (unsigned)in.mems, (unsigned)in.max_batch, (unsigned long long)in.cmds,
               (unsigned long)in.hash, (unsigned long)pass2, c.w.full ? " FULL" : "");
        rec_calls_free(&c), rec_free(&v);
    }
    if (save) save_file(save, load_names[load], ".calls.shrr", buf, len);
    rec_info in;
    check(rec_scan(buf, len, &in), "rec_scan");
    rec_times *t = calloc(2 * (size_t)in.frames, sizeof(*t));
    rec_stats *st = calloc(2 * (size_t)reps, sizeof(*st));
    size_t scratch_bytes = 16u << 10;
    void *scratch = malloc(scratch_bytes);
    if (!t || !st || !scratch) check(SHR_E_NO_MEMORY, "replay");
    shr_framebuffer_driver sw = new_driver();
    rec_calls_host h = {&sw, g.targets, g.ntargets, NULL, open_package, now_ns, NULL, NULL, scratch, scratch_bytes};
    v = (rec_writer){.buf = v.buf, .cap = v.cap, .hash_only = true};
    check(rec_calls_play(buf, len, &h, &v, t, t + in.frames), "rec_calls_play");
    int32_t bad = -2;
    for (uint32_t k = 0; k < v.done && bad == -2; k++)
        if (k >= nref || v.hashes[k] != ref[k]) bad = (int32_t)k - 1;
    if ((v.done != in.frames || in.frames != nref) && bad == -2) bad = (int32_t)(v.done < nref ? v.done : nref) - 1;
    uint32_t hash = v.hash, last = nref ? ref[nref - 1] : 0;
    size_t peak = v.peak;
    bool same = hash == last && bad == -2 && !v.full && v.st == SHR_OK;
    rec_free(&v), free(v.buf);
    for (int rep = 0; rep < reps; rep++) {
        check(rec_calls_play(buf, len, &h, NULL, t, t + in.frames), "rec_calls_play");
        st[rep] = rec_print(rep, in.scene, "compositor", rec_compositor_cols, REC_COMPOSITOR_COLS, t, in.frames - 1, hash,
                            same, in.hash, per_frame);
        st[reps + rep] = rec_print(rep, in.scene, "api", rec_api_cols, REC_API_COLS, t + in.frames, in.frames - 1, hash,
                                   same, in.hash, per_frame);
    }
    rec_print_summary(in.scene, "compositor", st, reps);
    rec_print_summary(in.scene, "api", st + reps, reps);
    printf("REPLAY CALLS rec=%s calls-hash %08lx cmd-hash record %08lx", in.scene, (unsigned long)in.hash,
           (unsigned long)last);
    if (!load_dir) printf(" pass %08lx", (unsigned long)pass2);
    printf(" replay %08lx first-bad %d peak %zu KiB -> %s\n", (unsigned long)hash, (int)bad, peak >> 10,
           same && (load_dir || pass2 == hash) ? "EQUAL" : "DIFFER");
    check(shr_software_driver_destroy(&sw), "driver");
    free(scratch), free(st), free(t), free(buf);
}

/* Whether `name` is one of the comma-separated `scenes`. */
static bool listed(const char *scenes, const char *name) {
    size_t n = strlen(name);
    for (const char *at = scenes;; at++) {
        if (!strncmp(at, name, n) && (at[n] == ',' || !at[n])) return true;
        if (!(at = strchr(at, ','))) return false;
    }
}

static shr_surface surface(int32_t w, int32_t h, uint32_t generation) {
    size_t stride = (size_t)w * 2;
    void *px = aligned_alloc(128, stride * (size_t)h);
    if (!px) check(SHR_E_NO_MEMORY, "surface");
    return (shr_surface){px, w, h, stride, stride * (size_t)h, SHR_FORMAT_RGB565, generation, SHR_MEMORY_CPU, 0};
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FONT_DIR [SCENE[,SCENE...]|all] [FRAMES]\n", argv[0]);
        return 2;
    }
    const char *profile = getenv("SHR_REPLAY_PROFILE"), *part = getenv("SHR_REPLAY_PART");
    const char *reps_env = getenv("SHR_REPLAY_REPS"), *frames_env = getenv("SHR_REPLAY_FRAMES");
    const char *keeps_env = getenv("SHR_REPLAY_KEEPS"), *bands_env = getenv("SHR_REPLAY_BANDS");
    int bands = bands_env ? atoi(bands_env) : 2;
    if ((profile && strcmp(profile, "tab5")) || (part && strcmp(part, "driver-sw") && strcmp(part, "compositor"))) {
        fprintf(stderr, "only SHR_REPLAY_PROFILE=tab5 and SHR_REPLAY_PART=driver-sw or compositor so far\n");
        return 2;
    }
    if (bands < 1 || bands > 2) {
        fprintf(stderr, "SHR_REPLAY_BANDS must be 1 or 2\n");
        return 2;
    }
    if (SHR_PIXEL_FORMAT != SHR_FORMAT_RGB565) {
        fprintf(stderr, "the tab5 profile needs an RGB565 build\n");
        return 77;
    }
    g.fonts = argv[1];
    g.keeps = (keeps_env ? (uint32_t)atoi(keeps_env) : 4) * (OUT_W / SHR_CELL_HEIGHT);
    g.ntargets = 1 + (uint32_t)bands;
    g.targets[0] = surface(OUT_W, OUT_H, 0);
    for (uint32_t k = 1; k < g.ntargets; k++) g.targets[k] = surface(OUT_H, BAND_H, 1);
    for (size_t i = 0; i < T5_N(t5_packages); i++) {
        char path[1024];
        size_t n;
        snprintf(path, sizeof(path), "%s/%s", g.fonts, t5_packages[i]);
        uint8_t *b = read_file(path, &n);
        g.packages[i] = (rec_package){t5_packages[i], b ? n : 0, b ? rec_hash(REC_HASH_INIT, b, n) : 0};
        free(b);
    }
    const char *scenes = argc > 2 ? argv[2] : "all", *save = getenv("SHR_REPLAY_SAVE"), *load_dir = getenv("SHR_REPLAY_LOAD");
    int reps = reps_env && atoi(reps_env) > 0 ? atoi(reps_env) : 5;
    bool per_frame = frames_env && atoi(frames_env);
    for (int load = 0; load < LOADS; load++) {
        bool all = !strcmp(scenes, "all"), cost = !strcmp(scenes, "cost");
        if (!(all ? load < COST_CELL : cost ? T5_COST_LOADS >> load & 1 : listed(scenes, load_names[load]))) continue;
        int32_t frames = argc > 3 ? atoi(argv[3]) : t5_rec_frames(load);
        uint32_t *ref = NULL, nref = 0;
        replay_driver(load, frames, !part || !strcmp(part, "driver-sw"), reps, per_frame, save, load_dir, &ref, &nref);
        if (!part || !strcmp(part, "compositor")) replay_calls(load, frames, reps, per_frame, save, load_dir, ref, nref);
        free(ref);
    }
    for (uint32_t k = 0; k < g.ntargets; k++) free(g.targets[k].pixels);
    return 0;
}
