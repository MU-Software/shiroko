#define _POSIX_C_SOURCE 200809L
/* Host throughput of each part, per operation: the software driver's commands, the compositor's frame build
 * (on a driver that draws nothing), tilemap updates, font glyph lookups, image copies, and whole terminal frames.
 * Usage: shiroko_bench FONT_DIR [NAME_FILTER]; SHR_BENCH_SECONDS (default 0.2) per case, raise it to profile one. */
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "text_scenes.h"

#define W 1280
#define H 720
#define BPP (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? 2 : 4)
#define ROWS (H / SHR_CELL_HEIGHT)
#define COLS (W / SHR_CELL_WIDTH)
#define CW SHR_CELL_WIDTH
#define CH SHR_CELL_HEIGHT

static double seconds = 0.2;
static const char *filter, *font_dir;
static uint8_t pixels[W * H * 4], other[W * H * 4];

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void check(const char *what, shr_status st) {
    if (st == SHR_OK) return;
    fprintf(stderr, "%s: %s\n", what, shr_status_name(st));
    exit(1);
}

/* Runs `op` until `seconds` have passed; `units` per operation give a throughput in M`unit`/s. */
static void run(const char *name, void (*op)(void *), void *arg, double units, const char *unit) {
    if (filter && !strstr(name, filter)) return;
    op(arg);
    long n = 0;
    double t0 = now_s(), t;
    do op(arg), n++;
    while ((t = now_s() - t0) < seconds);
    double ns = t / (double)n * 1e9;
    if (units > 0)
        printf("%-36s %11.0f ns/op %9.1f M%s/s\n", name, ns, units / ns * 1e3, unit);
    else
        printf("%-36s %11.0f ns/op\n", name, ns);
}

static shr_surface screen_surface(uint8_t *px, int32_t w, int32_t h, shr_pixel_format f) {
    size_t bpp = f == SHR_FORMAT_RGB565 ? 2 : 4;
    return (shr_surface){px, w, h, (size_t)w * bpp, (size_t)w * (size_t)h * bpp, f, 0, SHR_MEMORY_CPU, 0};
}

/* ===== Software driver: one batch of commands per operation ===== */

typedef struct batch {
    shr_framebuffer_driver *drv; /* NULL: shr_software_execute */
    shr_surface dst;
    shr_draw_cmd *cmds;
    size_t n;
} batch;

static uint8_t a8[CW * CH], a4[(CW + 1) / 2 * CH], sparse[CW * CH], rgba[64 * 64 * 4];
/* Buffer ids of the sources, and the table shr_software_execute draws them from. */
enum { A8_SRC = 1, A4_SRC, SPARSE_SRC, RGBA_SRC, NSRC = RGBA_SRC };
static const shr_image srcs[NSRC] = {
    {a8, CW, CH, CW, sizeof(a8), SHR_FORMAT_A8, SHR_MEMORY_CPU},
    {a4, CW, CH, (CW + 1) / 2, sizeof(a4), SHR_FORMAT_A4, SHR_MEMORY_CPU},
    {sparse, CW, CH, CW, sizeof(sparse), SHR_FORMAT_A8, SHR_MEMORY_CPU},
    {rgba, 64, 64, 256, sizeof(rgba), SHR_FORMAT_RGBA8888, SHR_MEMORY_CPU},
};

static void op_batch(void *arg) {
    batch *b = arg;
    check("execute", b->drv ? b->drv->execute(b->drv->user, &b->dst, b->cmds, b->n, 1)
                            : shr_software_execute(&b->dst, b->cmds, b->n, srcs, NSRC));
}

/* `flags`: BOLD and ITALIC turning about the cell's middle, which the cell-sized source fits. */
static shr_draw_cmd *cell_cmds(shr_cmd_kind kind, uint32_t id, uint32_t flags, size_t *n) {
    shr_draw_cmd *c = calloc((size_t)ROWS * COLS, sizeof(*c));
    for (int r = 0, i = 0; r < ROWS; r++)
        for (int k = 0; k < COLS; k++, i++)
            c[i] = (shr_draw_cmd){.kind = (uint8_t)kind, .dst = {k * CW, r * CH, (k + 1) * CW, (r + 1) * CH},
                                  .color = SHR_RGB(200, 180 + r, k), .buffer = id, .src_rect = {0, 0, CW, CH},
                                  .flags = (uint16_t)flags, .slant_axis = CH};
    *n = (size_t)ROWS * COLS;
    return c;
}

static void bench_software(void) {
    for (size_t i = 0; i < sizeof(a8); i++) a8[i] = (uint8_t)(i * 37 % 3 ? i * 53 : 0);
    for (size_t i = 0; i < sizeof(a4); i++) a4[i] = (uint8_t)(i * 29);
    for (int y = CH / 4; y < CH * 3 / 4; y++) /* a stem with soft edges and a bar: mostly zeros, like a glyph */
        for (int x = CW / 2 - 2; x <= CW / 2 + 1; x++)
            sparse[y * CW + x] = x == CW / 2 - 2 || x == CW / 2 + 1 ? 90 : 255;
    for (int x = 1; x < CW - 1; x++) sparse[CH / 2 * CW + x] = 255;
    for (size_t i = 0; i < sizeof(rgba); i++) rgba[i] = (uint8_t)(i % 4 == 3 ? (i * 7 % 3 ? 255 : 90) : i);
    const double screen_px = (double)W * H;
    shr_surface dst = screen_surface(pixels, W, H, SHR_PIXEL_FORMAT);
    shr_draw_cmd full = {.kind = SHR_CMD_FILL, .dst = {0, 0, W, H}, .color = SHR_RGB(10, 20, 30)};
    run("software/fill-screen", op_batch, &(batch){NULL, dst, &full, 1}, screen_px, "px");
    full.flags = SHR_GLYPH_DIM;
    run("software/fill-screen-dim", op_batch, &(batch){NULL, dst, &full, 1}, screen_px, "px");
    size_t n;
    shr_draw_cmd *cells = cell_cmds(SHR_CMD_FILL, 0, 0, &n);
    run("software/fill-cells", op_batch, &(batch){NULL, dst, cells, n}, screen_px, "px");
    free(cells);
    static const struct {
        const char *name;
        uint32_t src, flags;
    } glyphs[] = {
        {"software/glyph-a8-cells", A8_SRC, 0},
        {"software/glyph-a4-cells", A4_SRC, 0},
        {"software/glyph-a8-cells-bold", A8_SRC, SHR_GLYPH_BOLD},
        {"software/glyph-a8-cells-italic", A8_SRC, SHR_GLYPH_ITALIC},
        {"software/glyph-a8-cells-bold-italic", A8_SRC, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC},
        {"software/glyph-a4-cells-bold-italic", A4_SRC, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC},
        {"software/glyph-a8-sparse-bold-italic", SPARSE_SRC, SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC},
    };
    for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++) {
        cells = cell_cmds(SHR_CMD_GLYPH, glyphs[i].src, glyphs[i].flags, &n);
        run(glyphs[i].name, op_batch, &(batch){NULL, dst, cells, n}, screen_px, "px");
        free(cells);
    }

    shr_draw_cmd tiles[(W / 64) * (H / 64)];
    size_t nt = 0;
    for (int y = 0; y + 64 <= H; y += 64)
        for (int x = 0; x + 64 <= W; x += 64)
            tiles[nt++] = (shr_draw_cmd){.kind = SHR_CMD_IMAGE, .dst = {x, y, x + 64, y + 64}, .buffer = RGBA_SRC,
                                         .src_rect = {0, 0, 64, 64}};
    run("software/image-tiles-mixed-alpha", op_batch, &(batch){NULL, dst, tiles, nt}, (double)nt * 64 * 64, "px");

    shr_pixel_format of = SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? SHR_FORMAT_RGBX8888 : SHR_FORMAT_RGB565;
    shr_surface out = screen_surface(other, W, H, of), rot = screen_surface(other, H, W, SHR_PIXEL_FORMAT);
    shr_image_ref logical = {pixels, W, H, (uint32_t)dst.stride, SHR_PIXEL_FORMAT, SHR_MEMORY_CPU, 0};
    shr_draw_cmd copy = {.kind = SHR_CMD_COPY, .dst = {0, 0, W, H}, .src = logical};
    run("software/copy-convert-screen", op_batch, &(batch){NULL, out, &copy, 1}, screen_px, "px");
    shr_draw_cmd turn = {.kind = SHR_CMD_ROTATE, .dst = {0, 0, H, W}, .src = logical, .rotation = SHR_ROTATE_90_CW};
    run("software/rotate-90-screen", op_batch, &(batch){NULL, rot, &turn, 1}, screen_px, "px");

    /* One keep per row (background + glyph per cell after the glyph's REGISTER), stored by one batch and then drawn by
     * every operation. */
    shr_draw_cmd *rows = calloc((size_t)ROWS * (COLS * 2 + 2) + 1, sizeof(*rows)), draws[ROWS];
    size_t m = 0;
    const shr_image *m8 = &srcs[A8_SRC - 1];
    rows[m++] = (shr_draw_cmd){.kind = SHR_CMD_BUFFER_REGISTER, .buffer = A8_SRC,
                               .src = {m8->pixels, m8->width, m8->height, (uint32_t)m8->stride, SHR_FORMAT_A8, 0, 0}};
    for (int r = 0; r < ROWS; r++) {
        shr_rect row = {0, r * CH, W, (r + 1) * CH};
        rows[m++] = (shr_draw_cmd){.kind = SHR_CMD_KEEP_BEGIN, .dst = row, .buffer = (uint32_t)r + 1};
        for (int k = 0; k < COLS; k++) {
            shr_rect cell = {k * CW, r * CH, (k + 1) * CW, (r + 1) * CH};
            rows[m++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = cell, .color = SHR_RGB(20, 20, r)};
            rows[m++] = (shr_draw_cmd){.kind = SHR_CMD_GLYPH, .dst = cell, .color = SHR_RGB(220, 220, 220),
                                       .buffer = A8_SRC, .src_rect = {0, 0, CW, CH}};
        }
        rows[m++] = (shr_draw_cmd){.kind = SHR_CMD_KEEP_END};
        draws[r] = (shr_draw_cmd){.kind = SHR_CMD_KEEP_DRAW, .dst = row, .buffer = (uint32_t)r + 1};
    }
    shr_framebuffer_driver drv;
    check("driver", shr_software_driver_create(NULL, (uint64_t)ROWS * W * CH * BPP, ROWS, NSRC, &drv));
    check("store", drv.execute(drv.user, &dst, rows, m, 1));
    run("software/keep-rows-draw", op_batch, &(batch){&drv, dst, draws, ROWS}, screen_px, "px");
    shr_software_driver_destroy(&drv);
    free(rows);
}

/* ===== Contexts on a driver that draws nothing (frame build only) or on the software driver ===== */

static shr_status acq(void *u, shr_surface *s) {
    *s = screen_surface(pixels, W, H, SHR_PIXEL_FORMAT);
    return (void)u, SHR_OK;
}
static shr_status pres(void *u, const shr_surface *s, uint64_t id) { return (void)u, (void)s, (void)id, SHR_OK; }
static void disc(void *u, const shr_surface *s) { (void)u, (void)s; }
static shr_status draw_nothing(void *u, const shr_surface *d, const shr_draw_cmd *c, size_t n, shr_fence f) {
    return (void)u, (void)d, (void)c, (void)n, (void)f, SHR_OK;
}

typedef struct counting {
    shr_framebuffer_driver inner;
    unsigned long long calls, cmds;
} counting;

static shr_status count_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t n, shr_fence f) {
    counting *c = user;
    c->calls++, c->cmds += n;
    return c->inner.execute ? c->inner.execute(c->inner.user, dst, cmds, n, f) : SHR_OK;
}

typedef struct env {
    counting drv;
    shr_framebuffer_driver wrap;
    shr_output out;
    shr_context *ctx;
} env;

/* Buffer ids of the drivers below: font pages within page_cache_bytes, builtin pages and images. */
#define MAX_BUFFERS 1024

/* keep_bytes < 0: a driver that draws nothing; 0: one that keeps nothing. */
static void env_open(env *e, long keep_bytes) {
    memset(e, 0, sizeof(*e));
    if (keep_bytes >= 0)
        check("driver", shr_software_driver_create(NULL, (uint64_t)keep_bytes, keep_bytes ? 4 * ROWS : 0, MAX_BUFFERS,
                                                   &e->drv.inner));
    else
        e->drv.inner.execute = draw_nothing, e->drv.inner.caps.max_buffers = MAX_BUFFERS;
    e->wrap = e->drv.inner;
    e->wrap.user = &e->drv, e->wrap.execute = count_execute;
    shr_output_init(&e->out);
    e->out.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT;
    e->out.acquire = acq, e->out.present = pres, e->out.discard = disc;
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.driver = &e->wrap, cd.output = &e->out, cd.page_cache_bytes = 64u << 20, cd.max_commands = 1u << 20;
    cd.io_retry_ns = cd.io_timeout_ns = 0; /* synchronous files, no clock */
    check("create", shr_create(&cd, &e->ctx));
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = W, sd.height = H;
    check("configure", shr_screen_configure(e->ctx, &sd));
}

static void settle(shr_context *ctx) {
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    for (int i = 0; i < 4096 && dl.kind == SHR_DEADLINE_NOW; i++) {
        shr_pump(ctx);
        shr_next_deadline(ctx, &dl);
    }
    shr_event ev;
    while (shr_poll_event(ctx, &ev) == SHR_OK)
        if (ev.kind == SHR_EVENT_PRESENT_FAILED) check("frame", ev.status);
}

static void env_close(env *e, shr_pl_res_bitmap_font *font) {
    shr_begin_shutdown(e->ctx);
    while (font && shr_pl_res_bitmap_font_destroy(font) == SHR_E_STATE) shr_pump(e->ctx);
    while (shr_destroy(e->ctx) == SHR_E_WOULD_BLOCK) shr_pump(e->ctx);
    if (e->drv.inner.user) shr_software_driver_destroy(&e->drv.inner);
}

static double cmds_per_frame(env *e, void (*op)(void *), void *arg) {
    unsigned long long c0 = e->drv.cmds;
    for (int i = 0; i < 8; i++) op(arg);
    return (double)(e->drv.cmds - c0) / 8;
}

/* ===== Compositor: application layers ===== */

typedef struct layers {
    env *e;
    shr_lyr *l[32];
    int count, cells_per_side, frame;
    bool scattered;
} layers;

/* Rewrites every layer (only colours change) and draws: full or (scattered) 64 small changed cells. */
static void op_layers(void *arg) {
    layers *s = arg;
    s->frame++;
    for (int i = 0; i < s->count; i++) {
        check("begin", shr_lyr_cmd_begin(s->l[i]));
        int side = s->cells_per_side, cw = W / side, ch = H / side;
        for (int y = 0; y < side; y++)
            for (int x = 0; x < side; x++) {
                int k = y * side + x;
                bool change = s->scattered ? k % (side * side / 64) == 0 : true;
                check("fill", shr_lyr_cmd_fill(s->l[i], (shr_rect){x * cw, y * ch, (x + 1) * cw, (y + 1) * ch},
                                               SHR_RGB(k, change ? s->frame : 0, i)));
            }
        check("commit", shr_lyr_cmd_commit(s->l[i]));
    }
    shr_submit(s->e->ctx);
    settle(s->e->ctx);
}

static void bench_compositor(void) {
    static const struct {
        const char *name;
        int count, side;
        bool scattered;
    } cases[] = {
        {"compositor/one-layer-7200-fills", 1, 90, false},
        {"compositor/one-layer-64-changes", 1, 90, true},
        {"compositor/32-layers-64-fills", 32, 8, false},
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        if (filter && !strstr(cases[k].name, filter)) continue;
        env e;
        env_open(&e, -1);
        layers s = {&e, {0}, cases[k].count, cases[k].side, 0, cases[k].scattered};
        for (int i = 0; i < s.count; i++) check("layer", shr_lyr_create(e.ctx, i, (shr_rect){0, 0, W, H}, &s.l[i]));
        double cmds = cmds_per_frame(&e, op_layers, &s);
        char name[96];
        snprintf(name, sizeof(name), "%s (%.0f cmds)", cases[k].name, cmds);
        run(name, op_layers, &s, cmds, "cmd");
        for (int i = 0; i < s.count; i++) shr_lyr_destroy(s.l[i]);
        env_close(&e, NULL);
    }
}

/* ===== Tilemap and font ===== */

static shr_status open_read(void *user, const char *name, shr_asset_source *out) {
    char path[1024];
    if (!user) return SHR_E_NOT_FOUND;
    snprintf(path, sizeof(path), "%s/%s", (const char *)user, name);
    return shr_asset_source_file(path, out);
}

static void close_mapped(void *user) { free(user); }

/* The whole package in memory: glyphs are read in place. */
static shr_status open_mapped(void *user, const char *name, shr_asset_source *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", (const char *)user, name);
    FILE *f = fopen(path, "rb");
    if (!f) return SHR_E_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = malloc((size_t)size);
    bool ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) return free(data), SHR_E_IO;
    shr_asset_source_init(out);
    out->user = data, out->data = data, out->size = (uint64_t)size, out->close = close_mapped;
    return SHR_OK;
}

enum { FONT_BUILTIN, FONT_READ, FONT_MAPPED };

static shr_pl_res_bitmap_font *font_open(env *e, int kind) {
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.user = kind == FONT_BUILTIN ? NULL : (void *)font_dir;
    fd.open = kind == FONT_MAPPED ? open_mapped : open_read;
    shr_pl_res_bitmap_font *font;
    check("font", shr_pl_res_bitmap_font_create(e->ctx, &fd, &font));
    return font;
}

typedef struct term {
    env *e;
    shr_lyr *l;
    const char *const *sample;
    int nsample, shift;
} term;

static const char *const mixed[] = {"a", "b", "\xEA\xB0\x80", "#", "\xED\x95\x9C", "0", "x", "\xE2\x94\x80"};
static const char *const ascii[] = {"a", "b", "c", "#", "0", "x", "-", "|"};
static const char *const hangul[] = {"\xEA\xB0\x80", "\xED\x95\x9C", "\xEA\xB8\x80", "\xEB\x82\x98"};
static const char *const emoji[] = {"\xF0\x9F\x98\x80", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x91\x8D"};
static const char *const combining[] = {"e\xCC\x81", "a\xCC\x8A", "o\xCC\x88"};
static const char *const blank[] = {" "};

/* A terminal screen as a VT engine writes it, cell by cell; `shift` scrolls it by lines. */
static void fill_screen(term *t, int shift) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS;) {
            const char *s = t->sample[(unsigned)(r + shift + c * 7) % (unsigned)t->nsample];
            uint32_t span = (unsigned char)s[0] >= 0xEA ? 2 : 1;
            if (c + (int)span > COLS) span = 1, s = " ";
            shr_text_style st = {SHR_RGB(220, 220, 220), SHR_RGB(20, 20, (r + shift) * 3 % 64), 0};
            check("set_cell", shr_pl_lyr_tilemap_set_cell(t->l, r, c, s, strlen(s), span, st));
            c += (int)span;
        }
}

/* fill_screen() through set_row, a row per call, from the samples' code points decoded once. */
typedef struct row_term {
    term t;
    uint32_t cps[8][4];
    uint8_t n[8];
} row_term;

static void row_term_init(row_term *rt) {
    for (int k = 0; k < rt->t.nsample; k++) {
        const uint8_t *u = (const uint8_t *)rt->t.sample[k];
        int n = 0;
        while (*u) {
            int len = *u < 0x80 ? 1 : *u < 0xE0 ? 2 : *u < 0xF0 ? 3 : 4;
            uint32_t cp = len == 1 ? *u : (uint32_t)(*u & (0x7F >> len));
            for (int i = 1; i < len; i++) cp = cp << 6 | (u[i] & 0x3F);
            rt->cps[k][n++] = cp, u += len;
        }
        rt->n[k] = (uint8_t)n;
    }
}

static void op_set_rows(void *arg) {
    static const uint32_t space[] = {' '};
    row_term *rt = arg;
    term *t = &rt->t;
    int shift = ++t->shift;
    for (int r = 0; r < ROWS; r++) {
        shr_row_cell cells[COLS];
        uint32_t scalars[4 * COLS];
        size_t n = 0, ns = 0;
        for (int c = 0; c < COLS;) {
            int k = (int)((unsigned)(r + shift + c * 7) % (unsigned)t->nsample);
            uint32_t span = (unsigned char)t->sample[k][0] >= 0xEA ? 2 : 1;
            const uint32_t *cp = rt->cps[k];
            uint8_t m = rt->n[k];
            if (c + (int)span > COLS) span = 1, cp = space, m = 1;
            cells[n++] = (shr_row_cell){m == 1 ? cp[0] : (uint32_t)ns, 0, (uint8_t)span, m};
            if (m > 1) memcpy(scalars + ns, cp, m * sizeof(*cp)), ns += m;
            c += (int)span;
        }
        shr_text_style st = {SHR_RGB(220, 220, 220), SHR_RGB(20, 20, (r + shift) * 3 % 64), 0};
        shr_row row = {cells, n, &st, 1, scalars, ns, NULL, 0};
        check("set_row", shr_pl_lyr_tilemap_set_row(t->l, r, 0, &row, NULL));
    }
}

static void op_set_cells(void *arg) {
    term *t = arg;
    fill_screen(t, ++t->shift);
}

static void op_redraw(void *arg) { /* the frame build of an unchanged screen: glyph lookups and commands */
    term *t = arg;
    shr_request_redraw(t->e->ctx);
    settle(t->e->ctx);
}

static void op_scroll(void *arg) {
    term *t = arg;
    fill_screen(t, ++t->shift);
    shr_submit(t->e->ctx);
    settle(t->e->ctx);
}

/* The same screen scrolled with shr_pl_lyr_tilemap_scroll(): one row up, the new bottom row written. */
static void op_scroll_api(void *arg) {
    term *t = arg;
    check("scroll", shr_pl_lyr_tilemap_scroll(t->l, 0, ROWS, 1, (shr_text_style){0}));
    ++t->shift;
    for (int c = 0; c < COLS;) {
        const char *s = t->sample[(unsigned)(ROWS - 1 + t->shift + c * 7) % (unsigned)t->nsample];
        uint32_t span = (unsigned char)s[0] >= 0xEA ? 2 : 1;
        if (c + (int)span > COLS) span = 1, s = " ";
        shr_text_style st = {SHR_RGB(220, 220, 220), SHR_RGB(20, 20, (ROWS - 1 + t->shift) * 3 % 64), 0};
        check("set_cell", shr_pl_lyr_tilemap_set_cell(t->l, ROWS - 1, c, s, strlen(s), span, st));
        c += (int)span;
    }
    shr_submit(t->e->ctx);
    settle(t->e->ctx);
}

static void term_open(term *t, env *e, shr_pl_res_bitmap_font *font, const char *const *sample, int n,
                      const shr_color *background) {
    *t = (term){e, NULL, sample, n, 0};
    check("layer", shr_lyr_create(e->ctx, 1, (shr_rect){0, 0, W, H}, &t->l));
    check("resize", shr_pl_lyr_tilemap_resize(t->l, font, ROWS, COLS, background));
    fill_screen(t, 0);
    shr_submit(e->ctx);
    settle(e->ctx);
}

static void bench_tilemap(void) {
    static const struct {
        const char *name;
        const char *const *sample;
        int n;
    } sets[] = {{"ascii", ascii, 8}, {"hangul", hangul, 4}, {"emoji", emoji, 3}, {"combining", combining, 3}};
    for (size_t k = 0; k < sizeof(sets) / sizeof(sets[0]); k++) {
        char name[96];
        snprintf(name, sizeof(name), "tilemap/set_cell-%s", sets[k].name);
        if (filter && !strstr(name, filter)) continue;
        env e;
        env_open(&e, -1);
        shr_pl_res_bitmap_font *font = font_open(&e, FONT_BUILTIN);
        term t;
        term_open(&t, &e, font, sets[k].sample, sets[k].n, NULL);
        run(name, op_set_cells, &t, (double)ROWS * COLS, "cell");
        shr_lyr_destroy(t.l);
        env_close(&e, font);
    }
    for (size_t k = 0; k < sizeof(sets) / sizeof(sets[0]); k++) {
        char name[96];
        snprintf(name, sizeof(name), "tilemap/set_row-%s", sets[k].name);
        if (filter && !strstr(name, filter)) continue;
        env e;
        env_open(&e, -1);
        shr_pl_res_bitmap_font *font = font_open(&e, FONT_BUILTIN);
        row_term rt;
        term_open(&rt.t, &e, font, sets[k].sample, sets[k].n, NULL);
        row_term_init(&rt);
        run(name, op_set_rows, &rt, (double)ROWS * COLS, "cell");
        shr_lyr_destroy(rt.t.l);
        env_close(&e, font);
    }
    if (!filter || strstr("tilemap/set+flush+frame-all-rows", filter)) {
        env e;
        env_open(&e, -1);
        shr_pl_res_bitmap_font *font = font_open(&e, FONT_BUILTIN);
        term t;
        term_open(&t, &e, font, mixed, 8, NULL);
        run("tilemap/set+flush+frame-all-rows", op_scroll, &t, (double)ROWS * COLS, "cell");
        shr_lyr_destroy(t.l);
        env_close(&e, font);
    }
    static char text[1 << 16];
    for (size_t i = 0; i + 3 < sizeof(text); i += 3) memcpy(text + i, i % 9 ? "abc" : "\xEA\xB0\x80", 3);
    size_t len = strlen(text);
    static shr_text_cluster clusters[sizeof(text)];
    shr_text_extent extent;
    if (!filter || strstr("tilemap/measure-64KiB", filter)) {
        double t0 = now_s();
        long n = 0;
        do shr_pl_lyr_tilemap_measure(text, len, COLS, SHR_TEXT_WRAP, clusters, sizeof(text), &extent, NULL), n++;
        while (now_s() - t0 < seconds);
        double ns = (now_s() - t0) / (double)n * 1e9;
        printf("%-36s %11.0f ns/op %9.1f MB/s\n", "tilemap/measure-64KiB", ns, (double)len / ns * 1e3);
    }
}

static void bench_font(void) {
    static const char *const kinds[] = {"builtin", "read", "mapped"};
    for (int kind = FONT_BUILTIN; kind <= FONT_MAPPED; kind++) {
        char name[96];
        snprintf(name, sizeof(name), "font/redraw-glyphs-%s", kinds[kind]);
        if (filter && !strstr(name, filter) && !(kind == FONT_BUILTIN && strstr("font/redraw-blank", filter))) continue;
        env e;
        env_open(&e, -1);
        shr_pl_res_bitmap_font *font = font_open(&e, kind);
        term t;
        term_open(&t, &e, font, kind == FONT_BUILTIN ? ascii : mixed, 8, NULL);
        run(name, op_redraw, &t, (double)ROWS * COLS, "cell");
        if (kind == FONT_BUILTIN) { /* the same frame without glyphs: what the lookups add */
            fill_screen(&(term){&e, t.l, blank, 1, 0}, 0);
            shr_submit(e.ctx);
            settle(e.ctx);
            run("font/redraw-blank (no glyphs)", op_redraw, &t, (double)ROWS * COLS, "cell");
        }
        shr_lyr_destroy(t.l);
        env_close(&e, font);
    }
}

/* ===== Image ===== */

typedef struct image_arg {
    env *e;
    shr_pl_res_image *img;
    uint8_t *px;
} image_arg;

static void op_image_create(void *arg) {
    image_arg *a = arg;
    shr_pl_res_image *img;
    check("image", shr_pl_res_image_create(a->e->ctx, 512, 512, a->px, 512 * 4, &img));
    shr_pl_res_image_release(img);
    shr_pump(a->e->ctx);
}

static void op_image_update(void *arg) {
    image_arg *a = arg;
    check("update", shr_pl_res_image_update(a->img, (shr_rect){0, 0, 512, 512}, a->px, 512 * 4));
}

static void bench_image(void) {
    if (filter && !strstr("image/", filter)) return;
    env e;
    env_open(&e, -1);
    uint8_t *px = malloc(512 * 512 * 4);
    for (size_t i = 0; i < 512 * 512 * 4; i++) px[i] = (uint8_t)i;
    image_arg a = {&e, NULL, px};
    run("image/create+release-512", op_image_create, &a, 512.0 * 512, "px");
    check("image", shr_pl_res_image_create(e.ctx, 512, 512, px, 512 * 4, &a.img));
    run("image/update-512", op_image_update, &a, 512.0 * 512, "px");
    shr_pl_res_image_release(a.img);
    free(px);
    env_close(&e, NULL);
}

/* ===== Whole terminal frames on the software driver ===== */

static void bench_terminal(void) {
    for (int api = 0; api < 2; api++)
        for (int fonts = 0; fonts < 2; fonts++)
            for (int keep = 0; keep < 2; keep++) {
                char name[96];
                snprintf(name, sizeof(name), "terminal/scroll%s-%s-keeps-%s", api ? "-api" : "",
                         fonts ? "fonts" : "builtin", keep ? "on" : "off");
                if (filter && !strstr(name, filter)) continue;
                env e;
                env_open(&e, keep ? 4l * ROWS * W * CH * BPP : 0); /* a row per slot */
                shr_pl_res_bitmap_font *font = font_open(&e, fonts ? FONT_READ : FONT_BUILTIN);
                term t;
                term_open(&t, &e, font, mixed, 8, NULL);
                run(name, api ? op_scroll_api : op_scroll, &t, (double)W * H, "px");
                shr_lyr_destroy(t.l);
                env_close(&e, font);
            }
}

/* ===== Glyph-heavy terminal frames (text_scenes.h) on the software driver ===== */

typedef struct text_term {
    env *e;
    shr_lyr *l;
    ts_prose prose;
    uint32_t top;
    bool code;
} text_term;

static void set_text(void *u, int32_t row, int32_t col, const char *t, uint32_t span, shr_text_style st) {
    check("set_cell", shr_pl_lyr_tilemap_set_cell(((text_term *)u)->l, row, col, t, strlen(t), span, st));
}

static void op_text(void *arg) {
    text_term *t = arg;
    if (t->code)
        ts_code_screen(++t->top, ROWS, COLS, set_text, t);
    else
        ts_prose_churn(&t->prose, ROWS, COLS, 1000, set_text, t);
    shr_submit(t->e->ctx);
    settle(t->e->ctx);
}

static void bench_text(void) {
    static const char *const names[] = {"terminal/churn-ko", "terminal/code", "terminal/cjk-mix"};
    for (int k = 0; k < 3; k++) {
        if (filter && !strstr(names[k], filter)) continue;
        env e;
        env_open(&e, 4l * ROWS * W * CH * BPP); /* a row per slot */
        shr_pl_res_bitmap_font_desc fd;
        shr_pl_res_bitmap_font_desc_init(&fd);
        fd.user = (void *)font_dir, fd.open = open_read, fd.locale = "ko";
        shr_pl_res_bitmap_font *font;
        check("font", shr_pl_res_bitmap_font_create(e.ctx, &fd, &font));
        text_term t = {&e, NULL, {.rng = 0x2545F491u, .mix = k == 2, .left = -1}, 0, k == 1};
        check("layer", shr_lyr_create(e.ctx, 1, (shr_rect){0, 0, W, H}, &t.l));
        check("resize", shr_pl_lyr_tilemap_resize(t.l, font, ROWS, COLS, NULL));
        if (t.code)
            ts_code_screen(0, ROWS, COLS, set_text, &t);
        else
            ts_prose_screen(&t.prose, ROWS, COLS, set_text, &t);
        shr_submit(e.ctx);
        settle(e.ctx);
        run(names[k], op_text, &t, (double)W * H, "px");
        shr_lyr_destroy(t.l);
        env_close(&e, font);
    }
}

int main(int argc, char **argv) {
    font_dir = argc > 1 ? argv[1] : "build/host/fonts";
    filter = argc > 2 ? argv[2] : NULL;
    const char *s = getenv("SHR_BENCH_SECONDS");
    if (s) seconds = atof(s);
    char probe[1024];
    snprintf(probe, sizeof(probe), "%s/shiroko-latin.shrf", font_dir);
    FILE *f = fopen(probe, "rb");
    if (!f) {
        fprintf(stderr, "font packages not found in %s\n", font_dir);
        return 1;
    }
    fclose(f);
    printf("screen %dx%d, cell %dx%d (%d x %d cells), %s\n", W, H, CW, CH, COLS, ROWS,
           SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? "RGB565" : "RGBX8888");
    bench_software();
    bench_compositor();
    bench_tilemap();
    bench_font();
    bench_image();
    bench_terminal();
    bench_text();
    return 0;
}
