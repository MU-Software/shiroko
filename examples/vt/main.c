/* External VT consumer: libghostty-vt owns the terminal, this app copies the
 * dirty rows of its RenderState into a Shiroko tilemap and draws the cursor
 * on its own layer. Usage: shiroko_vt OUT_PREFIX [FONT_DIR] [FIXTURE] writes
 * OUT_PREFIX-{1,2}.ppm, before and after a resize, and reports heap peaks. */
#include <ghostty/vt.h>
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 400
#define PAD 8
#define BPP (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? 2 : 4)

typedef struct meter {
    size_t live, peak;
} meter;

static void meter_add(meter *m, size_t add, size_t sub) {
    m->live += add - sub;
    if (m->live > m->peak) m->peak = m->live;
}

static void *g_alloc(void *ctx, size_t len, uint8_t align, uintptr_t ra) {
    (void)align, (void)ra; /* malloc covers Ghostty's alignment limit of 16 */
    void *p = malloc(len);
    if (p) meter_add(ctx, len, 0);
    return p;
}
static bool g_resize(void *ctx, void *mem, size_t len, uint8_t align, size_t new_len, uintptr_t ra) {
    (void)mem, (void)align, (void)ra;
    if (new_len > len) return false;
    meter_add(ctx, new_len, len);
    return true;
}
static void *g_remap(void *ctx, void *mem, size_t len, uint8_t align, size_t new_len, uintptr_t ra) {
    (void)ctx, (void)mem, (void)len, (void)align, (void)new_len, (void)ra;
    return NULL;
}
static void g_free(void *ctx, void *mem, size_t len, uint8_t align, uintptr_t ra) {
    (void)align, (void)ra;
    free(mem);
    meter_add(ctx, 0, len);
}
static const GhosttyAllocatorVtable G_VTABLE = {g_alloc, g_resize, g_remap, g_free};

static void *s_alloc(void *user, size_t size, size_t align, shr_alloc_kind kind) {
    (void)kind;
    void *p = align <= 16 ? malloc(size) : aligned_alloc(align, (size + align - 1) / align * align);
    if (p) meter_add(user, size, 0);
    return p;
}
static void s_free(void *user, void *p, size_t size, size_t align, shr_alloc_kind kind) {
    (void)align, (void)kind;
    free(p);
    meter_add(user, 0, size);
}

typedef struct app {
    uint8_t *pixels;
    int presented;
    meter term_mem, rs_mem, shr_mem;
    GhosttyAllocator term_alloc, rs_alloc;
    GhosttyTerminal term;
    GhosttyRenderState rs;
    GhosttyRenderStateRowIterator rows;
    GhosttyRenderStateRowCells cells;
    shr_context *ctx;
    shr_screen_desc screen;
    shr_lyr *grid, *cursor;
    shr_pl_res_bitmap_font *font;
    int32_t rows_n, cols_n;
    uint8_t *buf;
    size_t buf_len;
} app;

static shr_status out_acquire(void *user, shr_surface *s) {
    *s = (shr_surface){((app *)user)->pixels, W, H, W * BPP, (size_t)W * H * BPP, SHR_PIXEL_FORMAT, 0, SHR_MEMORY_CPU, 0};
    return SHR_OK;
}
static shr_status out_present(void *user, const shr_surface *s, uint64_t frame_id) {
    (void)s, (void)frame_id;
    ((app *)user)->presented++;
    return SHR_OK;
}
static void out_discard(void *user, const shr_surface *s) { (void)user, (void)s; }

static void on_log(void *user, shr_status st, const char *msg) {
    (void)user;
    fprintf(stderr, "shiroko: %s (%s)\n", msg, shr_status_name(st));
}

static shr_status open_package(void *user, const char *name, shr_asset_source *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", (const char *)user, name);
    return shr_asset_source_file(path, out);
}

static void check(const char *what, long long st) {
    if (st == 0) return;
    fprintf(stderr, "%s failed (%lld)\n", what, st);
    exit(1);
}

/* The terminal fills the layer; rows and columns follow from its size. Cells without their own background
 * show the terminal's default one, which also fills the padding around the grid. */
static void resize(app *a, int32_t width, int32_t height) {
    int32_t cols = width / SHR_CELL_WIDTH, rows = height / SHR_CELL_HEIGHT;
    shr_rect r = {PAD, PAD, PAD + cols * SHR_CELL_WIDTH, PAD + rows * SHR_CELL_HEIGHT};
    if (!a->term) check("ghostty_terminal_new", ghostty_terminal_new(&a->term_alloc, &a->term, (uint16_t)cols, (uint16_t)rows));
    else check("ghostty_terminal_resize", ghostty_terminal_resize(a->term, (uint16_t)cols, (uint16_t)rows,
                                                                  SHR_CELL_WIDTH, SHR_CELL_HEIGHT));
    if (!a->grid) check("layer", shr_lyr_create(a->ctx, 0, r, &a->grid));
    else check("layer rect", shr_lyr_set_rect(a->grid, r));
    check("tilemap", shr_pl_lyr_tilemap_resize(a->grid, a->font, rows, cols, &a->screen.clear));
    a->rows_n = rows, a->cols_n = cols;
}

static shr_color rgb(GhosttyColorRgb c) { return SHR_RGB(c.r, c.g, c.b); }

static void draw_cursor(app *a, const GhosttyRenderStateCursor *c, shr_color color, uint32_t span) {
    shr_lyr *l = a->cursor;
    check("cursor begin", shr_lyr_cmd_begin(l));
    if (c->visible && c->viewport_has_value) {
        int32_t x = PAD + (c->viewport_x - (c->wide_tail ? 1 : 0)) * SHR_CELL_WIDTH, y = PAD + c->viewport_y * SHR_CELL_HEIGHT;
        shr_rect r = {x, y, x + (int32_t)span * SHR_CELL_WIDTH, y + SHR_CELL_HEIGHT};
        shr_rect e[4] = {r, r, r, r};
        int n = 1;
        if (c->visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR) e[0].x1 = r.x0 + 2;
        if (c->visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE) e[0].y0 = r.y1 - 2;
        if (c->visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW) {
            e[0].y1 = r.y0 + 1, e[1].y0 = r.y1 - 1, e[2].x1 = r.x0 + 1, e[3].x0 = r.x1 - 1;
            n = 4;
        }
        for (int i = 0; i < n; i++) check("cursor fill", shr_lyr_cmd_fill(l, e[i], color));
    }
    check("cursor commit", shr_lyr_cmd_commit(l));
}

/* Copies the dirty rows of the borrowed RenderState into the tilemap. */
static void update(app *a) {
    check("render state update", ghostty_render_state_update(a->rs, a->term));
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    GhosttyRenderStateCursor cur = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    check("colors", ghostty_render_state_get(a->rs, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors));
    check("cursor", ghostty_render_state_get(a->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cur));
    check("dirty", ghostty_render_state_get(a->rs, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty));

    shr_color clear = rgb(colors.background);
    if (clear != a->screen.clear) {
        a->screen.clear = clear;
        shr_status st;
        while ((st = shr_screen_configure(a->ctx, &a->screen)) == SHR_E_WOULD_BLOCK) shr_pump(a->ctx);
        check("screen", st);
        check("tilemap", shr_pl_lyr_tilemap_resize(a->grid, a->font, a->rows_n, a->cols_n, &clear));
    }
    int32_t cur_row = cur.viewport_has_value ? cur.viewport_y : -1, cur_col = cur.viewport_x - (cur.wide_tail ? 1 : 0);
    uint32_t cur_span = 1;
    check("row iterator", ghostty_render_state_get(a->rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &a->rows));
    for (int32_t y = 0; ghostty_render_state_row_iterator_next(a->rows); y++) {
        bool row_dirty = dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL;
        if (!row_dirty) ghostty_render_state_row_get(a->rows, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty);
        if (!row_dirty && y != cur_row) continue;
        check("row cells", ghostty_render_state_row_get(a->rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &a->cells));
        for (int32_t x = 0; ghostty_render_state_row_cells_next(a->cells); x++) {
            GhosttyCell raw;
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            ghostty_render_state_row_cells_get(a->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
            ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
            if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) continue; /* covered by the wide cell before it */
            uint32_t span = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1;
            if (y == cur_row && x == cur_col) cur_span = span;
            if (!row_dirty) continue;

            GhosttyStyle s = GHOSTTY_INIT_SIZED(GhosttyStyle);
            ghostty_render_state_row_cells_get(a->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &s);
            GhosttyColorRgb fg = colors.foreground, bg = colors.background, t;
            bool has_bg = false;
            if (ghostty_render_state_row_cells_get(a->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &t) == GHOSTTY_SUCCESS)
                fg = t;
            if (ghostty_render_state_row_cells_get(a->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &t) == GHOSTTY_SUCCESS)
                bg = t, has_bg = true;
            if (s.inverse) t = fg, fg = bg, bg = t, has_bg = true;
            uint32_t flags = (s.bold ? SHR_STYLE_BOLD : 0) | (s.italic ? SHR_STYLE_ITALIC : 0) |
                             (s.faint ? SHR_STYLE_DIM : 0) | (s.underline ? SHR_STYLE_UNDERLINE : 0) |
                             (s.strikethrough ? SHR_STYLE_STRIKE : 0) | (s.blink ? SHR_STYLE_BLINK : 0) |
                             (s.invisible ? SHR_STYLE_CONCEAL : 0) | (has_bg ? SHR_STYLE_BG : 0);

            GhosttyBuffer gb = {a->buf, a->buf_len, 0};
            GhosttyResult r = ghostty_render_state_row_cells_get(a->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &gb);
            if (r == GHOSTTY_OUT_OF_SPACE) memcpy(a->buf, "\xEF\xBF\xBD", 3), gb.len = 3; /* over the renderer's limit */
            else if (r != GHOSTTY_SUCCESS) gb.len = 0;
            shr_status st = shr_pl_lyr_tilemap_set_cell(a->grid, y, x, (const char *)a->buf, gb.len, span,
                                                        (shr_text_style){rgb(fg), rgb(bg), flags});
            if (st != SHR_OK) fprintf(stderr, "cell %d,%d: %s\n", y, x, shr_status_name(st));
        }
    }
    ghostty_render_state_clean(a->rs);
    draw_cursor(a, &cur, rgb(cur.visible && colors.cursor_has_value ? colors.cursor : colors.foreground), cur_span);
    check("submit", shr_submit(a->ctx));
}

static bool settle(shr_context *ctx) {
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    for (int i = 0; i < 256 && dl.kind == SHR_DEADLINE_NOW; i++) {
        shr_pump(ctx);
        shr_next_deadline(ctx, &dl);
    }
    bool failed = false;
    shr_event ev;
    while (shr_poll_event(ctx, &ev) == SHR_OK)
        if (ev.kind == SHR_EVENT_PRESENT_FAILED || ev.kind == SHR_EVENT_RESOURCE_FAILED) {
            fprintf(stderr, "event %d: %s\n", ev.kind, shr_status_name(ev.status));
            failed |= ev.kind == SHR_EVENT_PRESENT_FAILED;
        }
    return !failed;
}

static void write_ppm(const app *a, const char *prefix, int n) {
    char path[1024];
    snprintf(path, sizeof(path), "%s-%d.ppm", prefix, n);
    FILE *f = fopen(path, "wb");
    if (!f) check("open output", 1);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint8_t px[3];
        if (BPP == 2) {
            uint16_t v;
            memcpy(&v, a->pixels + i * 2, 2);
            px[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31), px[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63),
            px[2] = (uint8_t)((v & 31) * 255 / 31);
        } else {
            memcpy(px, a->pixels + i * 4, 3);
        }
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s (%dx%d cells, %d frame(s) presented)\n", path, a->cols_n, a->rows_n, a->presented);
    /* Ghostty maps terminal pages directly; only its allocator traffic is counted. */
    printf("  heap peak KiB: terminal allocator %zu (+ mmap'd pages), RenderState %zu, app %zu B, Shiroko %zu\n",
           a->term_mem.peak >> 10, a->rs_mem.peak >> 10, a->buf_len, a->shr_mem.peak >> 10);
}

static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc(n > 0 ? (size_t)n : 1);
    *len = p && n > 0 ? fread(p, 1, (size_t)n, f) : 0;
    fclose(f);
    return p;
}

int main(int argc, char **argv) {
    const char *prefix = argc > 1 ? argv[1] : "shiroko-vt";
    const char *font_dir = argc > 2 ? argv[2] : "build/vt-example/fonts";
    const char *fixture = argc > 3 ? argv[3] : "examples/vt/fixture.ans";
    app a = {.pixels = calloc((size_t)W * H, BPP)};
    a.term_alloc = (GhosttyAllocator){&a.term_mem, &G_VTABLE};
    a.rs_alloc = (GhosttyAllocator){&a.rs_mem, &G_VTABLE};
    shr_allocator sa = {&a.shr_mem, s_alloc, s_free};
    shr_text_limits limits;
    shr_pl_lyr_tilemap_limits_get(&limits);
    a.buf_len = limits.max_cell_bytes;
    a.buf = malloc(a.buf_len);

    shr_framebuffer_driver drv;
    check("driver", shr_software_driver_create(&sa, 1u << 20, 256, &drv));
    shr_output out;
    shr_output_init(&out);
    out.user = &a, out.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT;
    out.acquire = out_acquire, out.present = out_present, out.discard = out_discard;
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.allocator = &sa, cd.driver = &drv, cd.output = &out, cd.log = on_log, cd.page_cache_bytes = 8u << 20;
    cd.io_retry_ns = cd.io_timeout_ns = 0; /* synchronous files, no clock */
    check("shr_create", shr_create(&cd, &a.ctx));
    shr_screen_desc_init(&a.screen);
    a.screen.width = W, a.screen.height = H;
    check("screen", shr_screen_configure(a.ctx, &a.screen));

    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.user = (void *)font_dir, fd.open = open_package, fd.locale = "ko";
    check("font", shr_pl_res_bitmap_font_create(a.ctx, &fd, &a.font));
    resize(&a, W - 2 * PAD, H - 2 * PAD);
    check("cursor layer", shr_lyr_create(a.ctx, 1, (shr_rect){0, 0, W, H}, &a.cursor));
    check("render state", ghostty_render_state_new(&a.rs_alloc, &a.rs) || ghostty_render_state_row_iterator_new(&a.rs_alloc, &a.rows) ||
                              ghostty_render_state_row_cells_new(&a.rs_alloc, &a.cells));

    size_t len = 0;
    uint8_t *ansi = read_file(fixture, &len);
    if (!ansi) check("read fixture", 1);
    ghostty_terminal_vt_write(a.term, ansi, len);
    free(ansi);
    update(&a);
    bool ok = settle(a.ctx);
    write_ppm(&a, prefix, 1);

    resize(&a, 36 * SHR_CELL_WIDTH, H - 2 * PAD);
    update(&a);
    ok &= settle(a.ctx);
    write_ppm(&a, prefix, 2);

    shr_lyr_destroy(a.cursor);
    shr_lyr_destroy(a.grid);
    ghostty_render_state_row_cells_free(a.cells);
    ghostty_render_state_row_iterator_free(a.rows);
    ghostty_render_state_free(a.rs);
    ghostty_terminal_free(a.term);
    shr_begin_shutdown(a.ctx);
    while (shr_pl_res_bitmap_font_destroy(a.font) == SHR_E_STATE) shr_pump(a.ctx);
    while (shr_destroy(a.ctx) == SHR_E_WOULD_BLOCK) shr_pump(a.ctx);
    shr_software_driver_destroy(&drv);
    free(a.buf), free(a.pixels);
    return a.presented && ok ? 0 : 1;
}
