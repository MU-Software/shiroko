#define _POSIX_C_SOURCE 200809L
/* Host throughput of text measurement and of tilemap frames (unchanged, scrolled), with the built-in
 * font or the packages in the given directory, on the software driver with and without its row cache.
 * A wrapping driver counts the submissions and commands. */
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 1280
#define H 720
#define BPP (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? 2 : 4)
#define ROWS (H / SHR_CELL_HEIGHT)
#define COLS (W / SHR_CELL_WIDTH)

static uint8_t pixels[W * H * 4];

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static shr_status acq(void *u, shr_surface *s) {
    (void)u;
    *s = (shr_surface){pixels, W, H, W * BPP, (size_t)W * H * BPP, SHR_PIXEL_FORMAT, 0, SHR_MEMORY_CPU, 0};
    return SHR_OK;
}
static shr_status pres(void *u, const shr_surface *s, uint64_t id) { return (void)u, (void)s, (void)id, SHR_OK; }
static void disc(void *u, const shr_surface *s) { (void)u, (void)s; }

typedef struct counting {
    shr_framebuffer_driver inner;
    unsigned long long calls, cmds, groups;
} counting;

static shr_status count_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t n, shr_fence f) {
    counting *c = user;
    c->calls++, c->cmds += n;
    for (size_t i = 0; i < n; i++) c->groups += cmds[i].kind == SHR_CMD_CACHE_BEGIN;
    return c->inner.execute(c->inner.user, dst, cmds, n, f);
}

static shr_status open_package(void *user, const char *name, shr_asset_source *out) {
    char path[1024];
    if (!user) return SHR_E_NOT_FOUND;
    snprintf(path, sizeof(path), "%s/%s", (const char *)user, name);
    return shr_asset_source_file(path, out);
}

/* A terminal screen as a VT engine writes it, cell by cell; `shift` scrolls it by lines. */
static void screen(shr_lyr *l, int shift) {
    static const char *sample[] = {"a", "b", "\xEA\xB0\x80", "#", "\xED\x95\x9C", "0", "x", "\xE2\x94\x80"};
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS;) {
            const char *s = sample[(unsigned)(r + shift + c * 7) % 8];
            uint32_t span = (unsigned char)s[0] >= 0xEA ? 2 : 1;
            if (c + (int)span > COLS) span = 1, s = " ";
            shr_text_style st = {SHR_RGB(220, 220, 220), SHR_RGB(20, 20, (r + shift) * 3 % 64), SHR_STYLE_BG};
            if (shr_pl_lyr_tilemap_set_cell(l, r, c, s, strlen(s), span, st) != SHR_OK) exit(1);
            c += (int)span;
        }
}

static void settle(shr_context *ctx) {
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    for (int i = 0; i < 4096 && dl.kind == SHR_DEADLINE_NOW; i++) {
        shr_pump(ctx);
        shr_next_deadline(ctx, &dl);
    }
    shr_event ev;
    while (shr_poll_event(ctx, &ev) == SHR_OK)
        if (ev.kind == SHR_EVENT_PRESENT_FAILED) {
            fprintf(stderr, "frame failed: %s\n", shr_status_name(ev.status));
            exit(1);
        }
}

typedef struct result {
    double update_ms, frame_ms, calls, cmds, groups;
} result;

/* Rewrites every cell, then draws; `scroll` moves the content one line up per frame. */
static result frames(shr_context *ctx, shr_lyr *l, counting *c, int count, bool scroll) {
    static int shift;
    unsigned long long c0 = c->calls, m0 = c->cmds, g0 = c->groups;
    double update = 0, frame = 0;
    for (int i = 0; i < count; i++) {
        double t0 = now_s();
        screen(l, scroll ? ++shift : shift);
        double t1 = now_s();
        shr_submit(ctx);
        settle(ctx);
        update += t1 - t0, frame += now_s() - t1;
    }
    return (result){update / count * 1e3, frame / count * 1e3, (double)(c->calls - c0) / count,
                    (double)(c->cmds - m0) / count, (double)(c->groups - g0) / count};
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/host/fonts";
    static char text[4096];
    for (size_t i = 0; i + 3 < sizeof(text); i += 3) memcpy(text + i, "\xEA\xB0\x80", 3);
    size_t len = strlen(text);
    double t0 = now_s();
    static shr_text_cluster clusters[sizeof(text)];
    shr_text_extent extent;
    for (int i = 0; i < 200; i++)
        shr_pl_lyr_tilemap_measure(text, len, COLS, SHR_TEXT_WRAP, clusters, sizeof(text), &extent, NULL);
    printf("measure: %.1f MB/s\n", 200.0 * (double)len / (now_s() - t0) / 1e6);
    printf("screen %dx%d, cell %dx%d, %d x %d cells\n", W, H, SHR_CELL_WIDTH, SHR_CELL_HEIGHT, COLS, ROWS);

    char probe[1024];
    snprintf(probe, sizeof(probe), "%s/shiroko-latin.shrf", dir);
    FILE *f = fopen(probe, "rb");
    if (f) fclose(f);
    else printf("fonts: packages not found in %s\n", dir);
    for (int use_fonts = 0; use_fonts < (f ? 2 : 1); use_fonts++)
        for (int cache = 0; cache < 2; cache++) {
            counting drv = {0};
            if (shr_software_driver_create(NULL, cache ? 8u << 20 : 0, &drv.inner) != SHR_OK) return 1;
            shr_framebuffer_driver wrap = drv.inner;
            wrap.user = &drv, wrap.execute = count_execute;
            shr_output o;
            shr_output_init(&o);
            o.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT;
            o.acquire = acq, o.present = pres, o.discard = disc;
            shr_context_desc cd;
            shr_context_desc_init(&cd);
            cd.driver = &wrap, cd.output = &o, cd.page_cache_bytes = 64u << 20;
            cd.io_retry_ns = cd.io_timeout_ns = 0; /* synchronous files, no clock */
            shr_context *ctx;
            if (shr_create(&cd, &ctx) != SHR_OK) return 1;
            shr_screen_desc sd;
            shr_screen_desc_init(&sd);
            sd.width = W, sd.height = H;
            shr_screen_configure(ctx, &sd);
            shr_pl_res_bitmap_font_desc fd;
            shr_pl_res_bitmap_font_desc_init(&fd);
            fd.user = use_fonts ? (void *)dir : NULL, fd.open = open_package;
            shr_pl_res_bitmap_font *font;
            shr_lyr *l;
            if (shr_pl_res_bitmap_font_create(ctx, &fd, &font) != SHR_OK ||
                shr_lyr_create(ctx, 0, (shr_rect){0, 0, W, H}, &l) != SHR_OK ||
                shr_pl_lyr_tilemap_resize(l, font, ROWS, COLS, NULL) != SHR_OK)
                return 1;
            frames(ctx, l, &drv, 2, false);
            for (int scroll = 0; scroll < 2; scroll++) {
                result r = frames(ctx, l, &drv, 20, scroll);
                printf("%s, cache %s, %s: update %6.2f ms, frame %6.2f ms (%.1f submissions, %5.0f commands, "
                       "%2.0f row groups)\n",
                       use_fonts ? "fonts   " : "built-in", cache ? "on " : "off", scroll ? "scroll   " : "unchanged",
                       r.update_ms, r.frame_ms, r.calls, r.cmds, r.groups);
            }
            shr_lyr_destroy(l);
            shr_begin_shutdown(ctx);
            while (shr_pl_res_bitmap_font_destroy(font) == SHR_E_STATE) shr_pump(ctx);
            while (shr_destroy(ctx) == SHR_E_WOULD_BLOCK) shr_pump(ctx);
            shr_software_driver_destroy(&drv.inner);
        }
    return 0;
}
