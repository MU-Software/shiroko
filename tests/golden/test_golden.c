/* Fixed-image comparison: a deterministic screen (tilemap text with the baked fonts, application layers,
 * an image with alpha) rendered by the software driver must match the recorded XXH3-128 exactly, directly
 * and through the composition surface (rotation, format conversion). Entries are per cell size and screen
 * format; SHR_UPDATE_GOLDEN=1 rewrites this build's entries. Every image is written next to the test. */
#include "harness.h"
#include "shr_hash.h"

#define GW 256
#define GH 128
#define CW SHR_CELL_WIDTH
#define CH SHR_CELL_HEIGHT
#define FMT_NAME(f) ((f) == SHR_FORMAT_RGB565 ? "rgb565" : "rgbx8888")

typedef struct out {
    uint8_t px[GW * GH * 4];
    int32_t w, h;
    shr_pixel_format fmt;
    int presents;
} out;

static shr_status g_acquire(void *user, shr_surface *s) {
    out *o = user;
    size_t bpp = o->fmt == SHR_FORMAT_RGB565 ? 2 : 4;
    *s = (shr_surface){o->px, o->w, o->h, (size_t)o->w * bpp, (size_t)o->w * (size_t)o->h * bpp, o->fmt, 0,
                       SHR_MEMORY_CPU, 0};
    return SHR_OK;
}

static shr_status g_present(void *user, const shr_surface *s, uint64_t id) {
    (void)s, (void)id;
    ((out *)user)->presents++;
    return SHR_OK;
}

static void g_discard(void *user, const shr_surface *s) { (void)user, (void)s; }

static shr_status no_packages(void *user, const char *name, shr_asset_source *src) {
    (void)user, (void)name, (void)src;
    return SHR_E_NOT_FOUND;
}

static shr_lyr *tilemap(shr_context *ctx, shr_pl_res_bitmap_font *font, shr_rect r) {
    shr_lyr *l;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 1, r, &l), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, font, (r.y1 - r.y0) / CH, (r.x1 - r.x0) / CW, NULL), SHR_OK);
    return l;
}

static void cell(shr_lyr *l, int32_t col, const char *s, uint32_t span, shr_color fg, shr_color bg, uint32_t flags) {
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(l, 0, col, s, strlen(s), span, (shr_text_style){fg, bg, flags}), SHR_OK);
}

/* Layers are created into `ls` (destroyed by the caller). */
static size_t scene(shr_context *ctx, shr_pl_res_bitmap_font *font, shr_lyr **ls, shr_pl_res_image **img) {
    const shr_color fg = SHR_RGB(0xF8, 0xF8, 0xF2), bar = SHR_RGB(0x33, 0x37, 0x48), edge = SHR_RGB(0x62, 0x72, 0xA4);
    size_t n = 0;
    shr_rect box = {GW - 72, CH + 4, GW - 4, GH - CH - 4};

    shr_lyr *shapes;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, GW, GH}, &shapes), SHR_OK);
    ls[n++] = shapes;
    shr_lyr_cmd_begin(shapes);
    shr_lyr_cmd_fill(shapes, (shr_rect){0, 0, GW, CH}, bar);
    shr_lyr_cmd_fill(shapes, box, SHR_RGB(0x28, 0x2A, 0x36));
    shr_rect edges[4] = {{box.x0, box.y0, box.x1, box.y0 + 2}, {box.x0, box.y1 - 2, box.x1, box.y1},
                         {box.x0, box.y0, box.x0 + 2, box.y1}, {box.x1 - 2, box.y0, box.x1, box.y1}};
    for (int i = 0; i < 4; i++) shr_lyr_cmd_fill(shapes, edges[i], edge);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(shapes), SHR_OK);

    /* Created on top, then moved under everything: shows only where no other layer draws. */
    shr_lyr *under;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 3, (shr_rect){0, GH - CH - 6, GW, GH - CH}, &under), SHR_OK);
    ls[n++] = under;
    shr_lyr_cmd_begin(under);
    shr_lyr_cmd_fill(under, (shr_rect){0, 0, GW, 6}, SHR_RGB(0xF1, 0xFA, 0x8C));
    shr_lyr_cmd_commit(under);
    ASSERT_EQ_LL(shr_lyr_set_z(under, -1), SHR_OK);

    shr_lyr *hidden;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 9, (shr_rect){0, 0, GW, GH}, &hidden), SHR_OK);
    ls[n++] = hidden;
    shr_lyr_cmd_begin(hidden);
    shr_lyr_cmd_fill(hidden, (shr_rect){0, 0, GW, GH}, SHR_RGB(255, 0, 0));
    shr_lyr_cmd_commit(hidden);
    ASSERT_EQ_LL(shr_lyr_set_visible(hidden, false), SHR_OK);

    /* Moved partly off the screen. */
    shr_lyr *corner;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 2, (shr_rect){0, 0, 24, 24}, &corner), SHR_OK);
    ls[n++] = corner;
    shr_lyr_cmd_begin(corner);
    shr_lyr_cmd_fill(corner, (shr_rect){0, 0, 24, 24}, SHR_RGB(0x8B, 0xE9, 0xFD));
    shr_lyr_cmd_fill(corner, (shr_rect){4, 4, 8, 8}, SHR_RGB(0x28, 0x2A, 0x36));
    shr_lyr_cmd_commit(corner);
    ASSERT_EQ_LL(shr_lyr_set_rect(corner, (shr_rect){GW - 20, CH + 36, GW + 28, CH + 84}), SHR_OK);

    shr_error_info err;
    shr_lyr *title = tilemap(ctx, font, (shr_rect){0, 0, GW, CH});
    ls[n++] = title;
    const char *t = "Shiroko \xEB\xA0\x8C\xEB\x8D\x94\xEB\x9F\xAC \xE2\x9C\x93";
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_text(title, 0, 1, t, strlen(t), (shr_text_style){fg, 0, SHR_STYLE_BOLD}, NULL,
                                             0, 0, &err),
                 SHR_OK);

    const char *body = "A\xEA\xB0\x80\xF0\x9F\x98\x80" "B\tx\nwrap \xED\x95\x9C\xEA\xB8\x80 e\xCC\x81 "
                       "\xE2\x9D\xA4\xEF\xB8\x8F \xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB \xF0\x9F\x87\xB0\xF0\x9F\x87\xB7 "
                       "italic \xE6\xBC\xA2\xE5\xAD\x97 end";
    shr_style_run runs[2] = {{0, 1, {SHR_RGB(0xFF, 0x79, 0xC6), 0, SHR_STYLE_BOLD}},
                             {0, 6, {SHR_RGB(0x50, 0xFA, 0x7B), 0, SHR_STYLE_ITALIC}}};
    runs[1].byte_start = (size_t)(strstr(body, "italic") - body), runs[1].byte_end = runs[1].byte_start + 6;
    shr_lyr *text = tilemap(ctx, font, (shr_rect){0, CH, GW - 80, GH - 2 * CH});
    ls[n++] = text;
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_text(text, 0, 0, body, strlen(body), (shr_text_style){fg, 0, SHR_STYLE_UNDERLINE},
                                             runs, 2, SHR_TEXT_WRAP, &err),
                 SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(text, 4, 16, 1, 6, (shr_text_style){0, SHR_RGB(0x44, 0x47, 0x5A), SHR_STYLE_BG}),
                 SHR_OK);

    /* A row as a VT engine places it: spans decided by the engine, colours resolved. */
    shr_lyr *prompt = tilemap(ctx, font, (shr_rect){0, GH - CH, GW, GH});
    ls[n++] = prompt;
    const shr_color seg = SHR_RGB(0x44, 0x47, 0x5A);
    cell(prompt, 0, "$", 1, fg, seg, SHR_STYLE_BG);
    cell(prompt, 1, "\xEE\x82\xB0", 1, seg, 0, 0);
    cell(prompt, 2, "", 1, 0, fg, SHR_STYLE_BG);
    cell(prompt, 4, "d", 1, fg, 0, SHR_STYLE_DIM | SHR_STYLE_STRIKE);
    cell(prompt, 5, "\xEF\x84\x93", 1, SHR_RGB(0xFF, 0xB8, 0x6C), 0, 0);
    cell(prompt, 7, "\xEA\xB0\x81", 2, fg, 0, SHR_STYLE_UNDERLINE);
    cell(prompt, 9, "\xF0\x9F\x98\x80", 2, fg, 0, 0);
    cell(prompt, 11, "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB", 2, fg, 0, 0);
    cell(prompt, 13, "x", 1, fg, SHR_RGB(0xBD, 0x93, 0xF9), SHR_STYLE_BG | SHR_STYLE_CONCEAL);
    cell(prompt, 14, "b", 1, fg, 0, SHR_STYLE_BLINK | SHR_STYLE_BOLD);
    cell(prompt, 15, "\xE2\x96\x88", 1, SHR_RGB(0x50, 0xFA, 0x7B), 0, 0);
    cell(prompt, 16, "\xE2\x94\x80", 1, fg, 0, 0);
    cell(prompt, 17, "\xEF\xBF\xBD", 1, fg, 0, 0);

    uint8_t rgba[24 * 24 * 4];
    for (int y = 0; y < 24; y++)
        for (int x = 0; x < 24; x++) {
            uint8_t *p = rgba + (y * 24 + x) * 4;
            p[0] = (uint8_t)(x * 10), p[1] = (uint8_t)(y * 10), p[2] = 0xC0, p[3] = (uint8_t)(x < 12 ? 255 : (23 - x) * 20);
        }
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 24, 24, rgba, 24 * 4, img), SHR_OK);
    uint8_t white[4 * 4 * 4];
    memset(white, 0xFF, sizeof(white));
    ASSERT_EQ_LL(shr_pl_res_image_update(*img, (shr_rect){2, 2, 6, 6}, white, 16), SHR_OK);
    shr_lyr *pic;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 2, (shr_rect){box.x0 + 6, box.y0 + 6, box.x1 - 6, box.y1 - 6}, &pic), SHR_OK);
    ls[n++] = pic;
    shr_lyr_cmd_begin(pic);
    ASSERT_EQ_LL(shr_lyr_cmd_image(pic, *img, (shr_rect){0, 0, 24, 24}, (shr_point){0, 0}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(pic, *img, (shr_rect){8, 8, 24, 24}, (shr_point){30, 40}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(pic, *img, (shr_rect){0, 0, 24, 24}, (shr_point){44, 4}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(pic), SHR_OK);
    return n;
}

static char results[16][128];
static int nresults;

static void record(const char *name, const out *o) {
    size_t bpp = o->fmt == SHR_FORMAT_RGB565 ? 2 : 4, bytes = (size_t)o->w * (size_t)o->h * bpp;
    XXH128_hash_t h = XXH3_128bits(o->px, bytes);
    char *line = results[nresults++];
    snprintf(line, 128, "%s-%dx%d-%s %016llx%016llx", name, CW, CH, FMT_NAME(SHR_PIXEL_FORMAT),
             (unsigned long long)h.high64, (unsigned long long)h.low64);
    char ppm[256];
    snprintf(ppm, sizeof(ppm), "golden-%.*s.ppm", (int)strcspn(line, " "), line);
    FILE *f = fopen(ppm, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", o->w, o->h);
    for (size_t i = 0; i < (size_t)o->w * (size_t)o->h; i++) {
        uint8_t rgb[3];
        if (bpp == 2) {
            uint16_t v;
            memcpy(&v, o->px + 2 * i, 2);
            rgb[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31), rgb[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
            rgb[2] = (uint8_t)((v & 31) * 255 / 31);
        } else {
            memcpy(rgb, o->px + 4 * i, 3);
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void render(const char *name, bool fonts, shr_rotation rot, shr_pixel_format output_format) {
    static out o;
    bool quarter = rot == SHR_ROTATE_90_CW || rot == SHR_ROTATE_90_CCW;
    memset(&o, 0, sizeof(o));
    o.w = quarter ? GH : GW, o.h = quarter ? GW : GH;
    o.fmt = output_format ? output_format : SHR_PIXEL_FORMAT;
    shr_framebuffer_driver drv;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1u << 20, &drv), SHR_OK);
    shr_output output;
    shr_output_init(&output);
    output.user = &o, output.flags = SHR_OUTPUT_RELEASE_ON_PRESENT;
    output.acquire = g_acquire, output.present = g_present, output.discard = g_discard;
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.driver = &drv, cd.output = &output, cd.page_cache_bytes = 64u << 20;
    cd.io_retry_ns = cd.io_timeout_ns = 0; /* synchronous files, no clock */
    shr_context *ctx;
    ASSERT_EQ_LL(shr_create(&cd, &ctx), SHR_OK);
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = GW, sd.height = GH, sd.rotation = rot, sd.output_format = output_format;
    sd.clear = SHR_RGB(0x1E, 0x1F, 0x29);
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_OK);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = fonts ? font_dir_open : no_packages, fd.locale = "ko";
    shr_pl_res_bitmap_font *font;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(ctx, &fd, &font), SHR_OK);
    shr_lyr *ls[16];
    shr_pl_res_image *img;
    size_t n = scene(ctx, font, ls, &img);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    settle(ctx);
    shr_event ev;
    while (shr_poll_event(ctx, &ev) == SHR_OK)
        if (ev.kind == SHR_EVENT_PRESENT_FAILED || ev.kind == SHR_EVENT_RESOURCE_FAILED) {
            fprintf(stderr, "%s: event %d (%s)\n", name, ev.kind, shr_status_name(ev.status));
            FAIL_WITH_LONGJMPm("frame or resource failed");
        }
    ASSERT_EQ_LL(o.presents > 0, 1);
    record(name, &o);
    for (size_t i = 0; i < n; i++) shr_lyr_destroy(ls[i]);
    shr_pl_res_image_release(img);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(font), SHR_OK);
    shr_begin_shutdown(ctx);
    for (int i = 0; i < 16 && shr_destroy(ctx) == SHR_E_WOULD_BLOCK; i++) shr_pump(ctx);
    shr_software_driver_destroy(&drv);
}

static void update_file(const char *path) {
    static char keep[64][160];
    int nkeep = 0;
    FILE *f = fopen(path, "r");
    char line[160];
    while (f && fgets(line, sizeof(line), f) && nkeep < 64) {
        size_t n = strcspn(line, " ");
        bool ours = false;
        for (int i = 0; i < nresults && !ours; i++) ours = !strncmp(line, results[i], n) && results[i][n] == ' ';
        if (!ours && line[0] != '\n') snprintf(keep[nkeep++], sizeof(keep[0]), "%s", line);
    }
    if (f) fclose(f);
    f = fopen(path, "w");
    ASSERT_EQ_LL(f != NULL, 1);
    for (int i = 0; i < nkeep; i++) fputs(keep[i], f);
    for (int i = 0; i < nresults; i++) fprintf(f, "%s\n", results[i]);
    fclose(f);
    printf("updated %s\n", path);
}

static bool skipped;

TEST test_golden(void) {
    const shr_pixel_format other = SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? SHR_FORMAT_RGBX8888 : SHR_FORMAT_RGB565;
    render("fonts", true, SHR_ROTATE_NONE, 0);
    render("builtin", false, SHR_ROTATE_NONE, 0);
    render("rot90cw", true, SHR_ROTATE_90_CW, 0);
    render("rot180", true, SHR_ROTATE_180, 0);
    render("rot90ccw", true, SHR_ROTATE_90_CCW, 0);
    render(other == SHR_FORMAT_RGB565 ? "to-rgb565" : "to-rgbx8888", true, SHR_ROTATE_NONE, other);
    const char *path = SHR_GOLDEN_DIR "/golden.txt";
    if (getenv("SHR_UPDATE_GOLDEN")) {
        update_file(path);
        PASS();
    }
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL);
    char line[160], cell[16];
    snprintf(cell, sizeof(cell), "-%dx%d-", CW, CH);
    int matched = 0, mismatched = 0, this_cell = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        size_t n = strcspn(line, " ");
        this_cell += strstr(line, cell) && (size_t)(strstr(line, cell) - line) < n;
        for (int i = 0; i < nresults; i++) {
            if (strncmp(line, results[i], n) || results[i][n] != ' ') continue;
            matched++;
            if (strcmp(line, results[i])) {
                mismatched++;
                fprintf(stderr, "golden mismatch: %s\n  expected %s\n  (image written next to the test)\n", results[i],
                        line);
            }
        }
    }
    fclose(f);
    if (!this_cell) {
        printf("no golden entries for cell %dx%d: skipped\n", CW, CH);
        skipped = true;
        SKIP();
    }
    ASSERT_EQ_LL(mismatched, 0);
    ASSERT_EQ_LL(matched, nresults);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(test_golden);
    GREATEST_PRINT_REPORT();
    return !greatest_all_passed() ? EXIT_FAILURE : skipped ? 77 : EXIT_SUCCESS;
}
