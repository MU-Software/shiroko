/* Tilemap under random set_cell / set_text / clear / resize / measure sequences with the built-in font.
 * Invariants:
 *   - measure layouts partition the text, advance cell by cell and are deterministic
 *   - set_text accepts exactly the texts measure accepts (same columns, no runs); only measure's unbounded
 *     layout can reach the coordinate limit, which these short texts stay below
 *   - every accepted state rasters, an incremental frame equals a full redraw, and every batch the caching
 *     driver draws equals the stateless path (a stale row cache hit shows up here)
 *   - a rejected call changes nothing on screen */
#include "fuzz_common.h"

static uint64_t now;
static uint64_t clock_fn(void *user) {
    (void)user;
    return now;
}

static shr_text_style fr_style(fuzz_reader *r) {
    uint8_t f = fr_u8(r), c = fr_u8(r);
    uint32_t flags = f == 0xFF ? 1u << 9 : f; /* sometimes unknown */
    /* The low nibble picks fg, the high one bg, so either can change alone. */
    return (shr_text_style){SHR_RGB(c << 4, 255 - (c << 4), 128), SHR_RGB(0, c & 0xF0, 40), flags};
}

#define MAX_PIECES (4 * 4096) /* each byte a cluster, a TAB at most tab_stop pieces */
static shr_text_cluster pieces[MAX_PIECES], again[MAX_PIECES];

static void check_layout(const char *utf8, size_t len, int32_t cols, uint32_t flags, shr_status *out_st) {
    shr_text_profile_info info;
    shr_pl_lyr_tilemap_profile_get(&info);
    shr_error_info err;
    shr_text_extent ext, ext2;
    shr_status st = shr_pl_lyr_tilemap_measure(utf8, len, cols, flags, pieces, MAX_PIECES, &ext, &err);
    *out_st = st;
    FUZZ_CHECK(err.status == st);
    if (st != SHR_OK) {
        FUZZ_CHECK(ext.clusters == 0 && err.reason != NULL);
        FUZZ_CHECK(err.byte_offset == SIZE_MAX || err.byte_offset <= len);
        return;
    }
    size_t covered = 0;
    int32_t max_col = 0;
    shr_text_cluster p = {0}, c;
    for (size_t i = 0; i < ext.clusters; i++) {
        c = pieces[i];
        /* Pieces partition the source; wrapped TAB pieces repeat their range. */
        if (c.flags & SHR_CLUSTER_TAB_CONT) {
            FUZZ_CHECK((c.flags & SHR_CLUSTER_WRAPPED) && i > 0 && p.byte_offset == c.byte_offset);
        } else {
            FUZZ_CHECK(c.byte_offset == covered && c.byte_length > 0);
            covered = c.byte_offset + c.byte_length;
        }
        FUZZ_CHECK(c.cells >= 0 && c.row >= 0 && c.column >= 0 && c.row < ext.rows);
        if (c.flags & SHR_CLUSTER_NEWLINE) FUZZ_CHECK(c.cells == 0);
        if (c.flags & (SHR_CLUSTER_TAB | SHR_CLUSTER_TAB_CONT))
            FUZZ_CHECK(c.cells >= 1 && c.cells <= (int32_t)info.tab_stop);
        else
            FUZZ_CHECK(c.cells <= 2);
        if ((flags & SHR_TEXT_WRAP) && c.cells > 0) FUZZ_CHECK(c.column + c.cells <= cols);
        if (i > 0) {
            if ((p.flags & SHR_CLUSTER_NEWLINE) || (c.flags & SHR_CLUSTER_WRAPPED))
                FUZZ_CHECK(c.row == p.row + 1 && c.column == 0);
            else
                FUZZ_CHECK(c.row == p.row && c.column == p.column + p.cells);
        }
        if (c.column + c.cells > max_col) max_col = c.column + c.cells;
        p = c;
    }
    FUZZ_CHECK(covered == len);
    FUZZ_CHECK(ext.cols == max_col);
    size_t half = ext.clusters / 2;
    st = shr_pl_lyr_tilemap_measure(utf8, len, cols, flags, again, half, &ext2, NULL);
    FUZZ_CHECK(st == (half < ext.clusters ? SHR_E_LIMIT : SHR_OK));
    FUZZ_CHECK(memcmp(&ext, &ext2, sizeof(ext)) == 0 && memcmp(pieces, again, half * sizeof(*again)) == 0);
}

static fuzz_output out;
static uint8_t shown[sizeof(out.pixels)], before[sizeof(out.pixels)], direct[sizeof(out.pixels)];

/* `user` is the caching driver; each batch is also drawn by shr_software_execute() into a copy. */
static shr_status checked_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t count,
                                  shr_fence fence) {
    const shr_framebuffer_driver *d = user;
    FUZZ_CHECK(dst->byte_length <= sizeof(direct));
    memcpy(direct, dst->pixels, dst->byte_length);
    shr_surface copy = *dst;
    copy.pixels = direct;
    shr_status want = shr_software_execute(&copy, cmds, count);
    shr_status st = d->execute(d->user, dst, cmds, count, fence);
    FUZZ_CHECK(st == want);
    if (st == SHR_OK) FUZZ_CHECK(memcmp(direct, dst->pixels, dst->byte_length) == 0);
    return st;
}

static shr_status checked_reset(void *user) {
    const shr_framebuffer_driver *d = user;
    return d->reset(d->user);
}

/* Draws the current state, then a full redraw of it, which must be identical. */
static void render(shr_context *ctx) {
    FUZZ_CHECK(shr_submit(ctx) == SHR_OK);
    fuzz_settle(ctx);
    memcpy(shown, out.pixels, sizeof(shown));
    FUZZ_CHECK(shr_request_redraw(ctx) == SHR_OK);
    FUZZ_CHECK(shr_submit(ctx) == SHR_OK);
    fuzz_settle(ctx);
    FUZZ_CHECK(memcmp(shown, out.pixels, sizeof(shown)) == 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_reader r = {data, size};
    uint8_t cfg = fr_u8(&r);
    now = 0;
    memset(&out, 0, sizeof(out));
    shr_framebuffer_driver drv, checked;
    FUZZ_CHECK(shr_software_driver_create(NULL, (cfg & 1) ? 1024u << (cfg >> 4) : 0, &drv) == SHR_OK);
    checked = drv;
    checked.user = &drv, checked.execute = checked_execute, checked.reset = checked_reset;
    shr_output o;
    fuzz_output_init(&out, &o, (cfg & 2) ? SHR_OUTPUT_PRESERVES_CONTENT : 0);
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.driver = &checked, cd.output = &o, cd.now_ns = clock_fn;
    cd.io_retry_ns = cd.io_timeout_ns = 0;
    if (cfg & 4) cd.blink = (shr_blink_profile){100, 0, (cfg & 8) != 0, SHR_BLINK_RESTART_NONE};
    shr_context *ctx;
    FUZZ_CHECK(shr_create(&cd, &ctx) == SHR_OK);
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = FUZZ_W, sd.height = FUZZ_H, sd.clear = SHR_RGB(1, 2, 3);
    FUZZ_CHECK(shr_screen_configure(ctx, &sd) == SHR_OK);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = fuzz_no_packages;
    shr_pl_res_bitmap_font *fonts[2], *font = NULL;
    FUZZ_CHECK(shr_pl_res_bitmap_font_create(ctx, &fd, &fonts[0]) == SHR_OK);
    FUZZ_CHECK(shr_pl_res_bitmap_font_create(ctx, &fd, &fonts[1]) == SHR_OK);
    shr_lyr *layer;
    FUZZ_CHECK(shr_lyr_create(ctx, 0, (shr_rect){0, 0, FUZZ_W, FUZZ_H}, &layer) == SHR_OK);
    int32_t rows = 0, cols = 0;
    FUZZ_CHECK(shr_pl_lyr_tilemap_set_cell(layer, 0, 0, "a", 1, 1, (shr_text_style){0}) == SHR_E_STATE);

    bool clean = false; /* `shown` is the current state */
    for (int steps = 0; r.n > 0 && steps < 64; steps++) {
        uint8_t op = fr_u8(&r);
        shr_status st = SHR_OK;
        bool mutates = true;
        switch (op % 8) {
        case 0: {
            int32_t row = fr_i8(&r), col = fr_i8(&r);
            uint8_t sp = fr_u8(&r);
            uint32_t span = sp == 0xFF ? 0 : sp == 0xFE ? 100000 : 1u + sp % 4;
            shr_text_style style = fr_style(&r);
            size_t len;
            const char *s = fr_bytes(&r, fr_u8(&r) % 160, &len);
            st = shr_pl_lyr_tilemap_set_cell(layer, row, col, s, len, span, style);
            if (st == SHR_OK) FUZZ_CHECK(row >= 0 && row < rows && col >= 0 && (int64_t)col + span <= cols);
            break;
        }
        case 1: {
            int32_t row = fr_i8(&r), col = fr_i8(&r);
            uint8_t fl = fr_u8(&r), ra = fr_u8(&r), rb = fr_u8(&r);
            uint32_t flags = (fl & 1) ? SHR_TEXT_WRAP : 0;
            if (fl == 0xFF) flags |= 1u << 7;
            shr_text_style style = fr_style(&r);
            size_t len;
            const char *s = fr_bytes(&r, fr_u16(&r) % 1024, &len);
            size_t a = ra % (len + 1), b = rb % (len + 1);
            shr_style_run run = {a < b ? a : b, a < b ? b : a, fr_style(&r)};
            bool with_run = (fl & 2) != 0;
            shr_error_info err;
            st = shr_pl_lyr_tilemap_set_text(layer, row, col, s, len, style, with_run ? &run : NULL, with_run, flags,
                                             &err);
            FUZZ_CHECK(err.status == st);
            if (st != SHR_OK) FUZZ_CHECK(err.reason != NULL);
            if (!with_run && !(flags & ~(uint32_t)SHR_TEXT_WRAP) && !(style.flags & ~SHR_STYLE_KNOWN_FLAGS) && row >= 0 &&
                row < rows && col >= 0 && col < cols) {
                shr_status measured;
                check_layout(s, len, cols - col, flags, &measured);
                FUZZ_CHECK(measured == st);
            }
            break;
        }
        case 2: {
            int32_t row = fr_i8(&r), col = fr_i8(&r), nr = fr_i8(&r), nc = fr_i8(&r);
            st = shr_pl_lyr_tilemap_clear(layer, row, col, nr, nc, fr_style(&r));
            break;
        }
        case 3: {
            int32_t nr = fr_u8(&r) % 8, nc = fr_u8(&r) % 20;
            font = fonts[(op >> 3) & 1];
            const shr_color bg = SHR_RGB(nr, nc, 7);
            st = shr_pl_lyr_tilemap_resize(layer, font, nr, nc, (op & 16) ? &bg : NULL);
            FUZZ_CHECK(st == SHR_OK);
            rows = nr, cols = nc;
            break;
        }
        case 4: {
            int32_t mc = fr_i8(&r);
            uint32_t flags = (fr_u8(&r) & 1) ? SHR_TEXT_WRAP : 0;
            size_t len;
            const char *s = fr_bytes(&r, fr_u16(&r) % 4096, &len);
            check_layout(s, len, mc, flags, &st);
            if (mc < 0) FUZZ_CHECK(st == SHR_E_INVALID_ARG);
            mutates = false;
            break;
        }
        case 5:
            render(ctx);
            clean = true;
            mutates = false;
            break;
        case 6:
            now += fr_u8(&r) * 10u;
            clean = false;
            mutates = false;
            break;
        case 7: {
            int32_t x = fr_i8(&r) % 24, y = fr_i8(&r) % 24;
            st = shr_lyr_set_rect(layer, (shr_rect){x, y, x + cols * SHR_CELL_WIDTH, y + rows * SHR_CELL_HEIGHT});
            FUZZ_CHECK(st == SHR_OK);
            if (op & 8) FUZZ_CHECK(shr_lyr_set_visible(layer, !(op & 16)) == SHR_OK);
            break;
        }
        }
        if (!mutates) continue;
        if (st == SHR_OK) {
            clean = false;
        } else if (clean) {
            memcpy(before, shown, sizeof(before));
            render(ctx);
            FUZZ_CHECK(memcmp(before, shown, sizeof(before)) == 0);
        }
    }
    render(ctx);

    if (font) FUZZ_CHECK(shr_pl_res_bitmap_font_destroy(font) == SHR_E_STATE);
    FUZZ_CHECK(shr_lyr_destroy(layer) == SHR_OK);
    for (int i = 0; i < 2; i++) FUZZ_CHECK(shr_pl_res_bitmap_font_destroy(fonts[i]) == SHR_OK);
    FUZZ_CHECK(shr_begin_shutdown(ctx) == SHR_OK);
    shr_pump(ctx);
    FUZZ_CHECK(shr_destroy(ctx) == SHR_OK);
    FUZZ_CHECK(shr_software_driver_destroy(&drv) == SHR_OK);
    return 0;
}
