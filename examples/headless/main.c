/* Headless consumer: tilemap text (laid out and engine-placed), application
 * shapes and an image, rendered with the font packages in FONT_DIR into a
 * PPM image. Uses only the public headers. */
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 520
#define H 224
#define BPP (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? 2 : 4)

typedef struct image_output {
    uint8_t *pixels;
    int presented;
} image_output;

static shr_status out_acquire(void *user, shr_surface *s) {
    image_output *o = user;
    *s = (shr_surface){o->pixels, W, H, W * BPP, (size_t)W * H * BPP, SHR_PIXEL_FORMAT, 0, SHR_MEMORY_CPU, 0};
    return SHR_OK;
}

static shr_status out_present(void *user, const shr_surface *s, uint64_t frame_id) {
    (void)s, (void)frame_id;
    ((image_output *)user)->presented++;
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

static void check(const char *what, shr_status st, const shr_error_info *err) {
    if (st == SHR_OK) return;
    fprintf(stderr, "%s: %s", what, shr_status_name(st));
    if (err && err->reason) fprintf(stderr, " (%s at byte %zu)", err->reason, err->byte_offset);
    fprintf(stderr, "\n");
    exit(1);
}

static shr_lyr *tilemap(shr_context *ctx, shr_pl_res_bitmap_font *font, int32_t z, shr_rect r) {
    shr_lyr *l;
    check("layer", shr_lyr_create(ctx, z, r, &l), NULL);
    check("tilemap",
          shr_pl_lyr_tilemap_resize(l, font, (r.y1 - r.y0) / SHR_CELL_HEIGHT, (r.x1 - r.x0) / SHR_CELL_WIDTH, NULL), NULL);
    return l;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "shiroko-headless.ppm";
    const char *font_dir = argc > 2 ? argv[2] : "build/host/fonts";
    image_output out = {calloc((size_t)W * H, BPP), 0};
    shr_framebuffer_driver drv;
    check("driver", shr_software_driver_create(NULL, 1u << 20, &drv), NULL);
    shr_output output;
    shr_output_init(&output);
    output.user = &out;
    output.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT;
    output.acquire = out_acquire;
    output.present = out_present;
    output.discard = out_discard;
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.driver = &drv;
    cd.output = &output;
    cd.log = on_log;
    cd.page_cache_bytes = 8u << 20;
    cd.io_retry_ns = cd.io_timeout_ns = 0; /* synchronous files, no clock */
    shr_context *ctx;
    check("create", shr_create(&cd, &ctx), NULL);
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = W, sd.height = H, sd.clear = SHR_RGB(0x1E, 0x1F, 0x29);
    check("screen", shr_screen_configure(ctx, &sd), NULL);

    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.user = (void *)font_dir, fd.open = open_package, fd.locale = "ko";
    shr_pl_res_bitmap_font *font;
    check("font", shr_pl_res_bitmap_font_create(ctx, &fd, &font), NULL);

    const int32_t cw = SHR_CELL_WIDTH, ch = SHR_CELL_HEIGHT;
    shr_lyr *shapes;
    check("shapes", shr_lyr_create(ctx, 0, (shr_rect){0, 0, W, H}, &shapes), NULL);
    shr_lyr_cmd_begin(shapes);
    shr_lyr_cmd_fill(shapes, (shr_rect){0, 0, W, ch}, SHR_RGB(0x33, 0x37, 0x48));
    shr_rect box = {40 * cw, 2 * ch, W - 4, H - 2 * ch}, edge[4] = {
        {box.x0, box.y0, box.x1, box.y0 + 1}, {box.x0, box.y1 - 1, box.x1, box.y1},
        {box.x0, box.y0, box.x0 + 1, box.y1}, {box.x1 - 1, box.y0, box.x1, box.y1}};
    for (int i = 0; i < 4; i++) shr_lyr_cmd_fill(shapes, edge[i], SHR_RGB(0x62, 0x72, 0xA4));
    check("shapes", shr_lyr_cmd_commit(shapes), NULL);

    shr_error_info err;
    shr_color fg = SHR_RGB(0xF8, 0xF8, 0xF2);
    shr_lyr *title = tilemap(ctx, font, 1, (shr_rect){0, 0, W, ch});
    const char *t = "Shiroko \xEB\xA0\x8C\xEB\x8D\x94\xEB\x9F\xAC";
    check("title", shr_pl_lyr_tilemap_set_text(title, 0, 1, t, strlen(t), (shr_text_style){fg, 0, SHR_STYLE_BOLD}, NULL,
                                               0, 0, &err),
          &err);

    const char *body = "A\xEA\xB0\x80\xF0\x9F\x98\x80" "B\tTAB \xED\x95\x9C\xEA\xB8\x80 \xE2\x9D\xA4\xEF\xB8\x8F "
                       "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB\n"
                       "wrap: the quick brown fox jumps over the lazy dog \xE6\xBC\xA2\xE5\xAD\x97";
    shr_style_run run = {0, 1, {SHR_RGB(0xFF, 0x79, 0xC6), 0, SHR_STYLE_UNDERLINE}};
    shr_lyr *text = tilemap(ctx, font, 1, (shr_rect){cw, 2 * ch, 39 * cw, H - 2 * ch});
    check("body", shr_pl_lyr_tilemap_set_text(text, 0, 0, body, strlen(body), (shr_text_style){fg, 0, 0}, &run, 1,
                                              SHR_TEXT_WRAP, &err),
          &err);
    shr_lyr *labels = tilemap(ctx, font, 1, (shr_rect){box.x0 + cw, box.y0 + ch, box.x1 - cw, box.y1 - ch});
    check("label", shr_pl_lyr_tilemap_set_text(labels, 0, 0, "[ OK ]", 6,
                                               (shr_text_style){SHR_RGB(0x28, 0x2A, 0x36), SHR_RGB(0x50, 0xFA, 0x7B),
                                                                SHR_STYLE_BG},
                                               NULL, 0, 0, &err),
          &err);
    check("label", shr_pl_lyr_tilemap_set_text(labels, 1, 0, "dim + strike", 12,
                                               (shr_text_style){fg, 0, SHR_STYLE_DIM | SHR_STYLE_STRIKE}, NULL, 0, 0, &err),
          &err);

    /* A row as an external engine hands it over: one cell per display unit,
     * spans decided by the engine, colours already resolved. */
    shr_color seg1 = SHR_RGB(0x33, 0x37, 0x48), seg2 = SHR_RGB(0x62, 0x72, 0xA4);
    struct {
        const char *text;
        shr_color fg, bg;
    } cells[] = {{"\xEF\x81\xBC", fg, seg1}, {" ", fg, seg1}, {"~", fg, seg1}, {"/", fg, seg1}, {"s", fg, seg1},
                 {"r", fg, seg1}, {"c", fg, seg1}, {"\xEE\x82\xB0", seg1, seg2}, {"\xEF\x84\x93", fg, seg2},
                 {" ", fg, seg2}, {"m", fg, seg2}, {"a", fg, seg2}, {"i", fg, seg2}, {"n", fg, seg2},
                 {"\xEE\x82\xB0", seg2, SHR_RGB(0x1E, 0x1F, 0x29)}};
    shr_lyr *prompt = tilemap(ctx, font, 1, (shr_rect){cw, H - 2 * ch, W, H});
    for (int32_t i = 0; i < (int32_t)(sizeof(cells) / sizeof(cells[0])); i++)
        check("cell", shr_pl_lyr_tilemap_set_cell(prompt, 0, i, cells[i].text, strlen(cells[i].text), 1,
                                                  (shr_text_style){cells[i].fg, cells[i].bg, SHR_STYLE_BG}),
              NULL);
    check("cell", shr_pl_lyr_tilemap_set_cell(prompt, 1, 0, "$", 1, 1, (shr_text_style){fg, 0, 0}), NULL);
    check("cell", shr_pl_lyr_tilemap_set_cell(prompt, 1, 2, "", 0, 1, (shr_text_style){0, fg, SHR_STYLE_BG}), NULL);
    check("cell", shr_pl_lyr_tilemap_set_cell(prompt, 1, 4, "\xEA\xB0\x80", 3, 2, (shr_text_style){fg, 0, 0}), NULL);

    uint8_t rgba[32 * 32 * 4];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            uint8_t *p = rgba + (y * 32 + x) * 4;
            p[0] = (uint8_t)(x * 8), p[1] = (uint8_t)(y * 8), p[2] = 0xC0, p[3] = (uint8_t)(255 - (x + y) * 4);
        }
    shr_pl_res_image *img;
    check("image", shr_pl_res_image_create(ctx, 32, 32, rgba, 32 * 4, &img), NULL);
    shr_lyr *pic;
    check("image layer", shr_lyr_create(ctx, 2, (shr_rect){box.x1 - 40, box.y1 - 40, box.x1 - 8, box.y1 - 8}, &pic), NULL);
    shr_lyr_cmd_begin(pic);
    check("image cmd", shr_lyr_cmd_image(pic, img, (shr_rect){0, 0, 32, 32}, (shr_point){0, 0}), NULL);
    check("image layer", shr_lyr_cmd_commit(pic), NULL);

    check("submit", shr_submit(ctx), NULL);
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    for (int i = 0; i < 64 && dl.kind == SHR_DEADLINE_NOW; i++) {
        shr_pump(ctx);
        shr_next_deadline(ctx, &dl);
    }
    shr_event ev;
    bool failed = false;
    while (shr_poll_event(ctx, &ev) == SHR_OK)
        if (ev.kind == SHR_EVENT_PRESENT_FAILED || ev.kind == SHR_EVENT_RESOURCE_FAILED) {
            fprintf(stderr, "event %d: %s\n", ev.kind, shr_status_name(ev.status));
            failed |= ev.kind == SHR_EVENT_PRESENT_FAILED;
        }
    printf("%d frame(s) presented\n", out.presented);

    FILE *f = fopen(path, "wb");
    if (!f) check("open output", SHR_E_IO, NULL);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint8_t rgb[3];
        if (BPP == 2) {
            uint16_t v;
            memcpy(&v, out.pixels + i * 2, 2);
            rgb[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31), rgb[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63),
            rgb[2] = (uint8_t)((v & 31) * 255 / 31);
        } else {
            memcpy(rgb, out.pixels + i * 4, 3);
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", path);

    shr_lyr *layers[] = {shapes, title, text, labels, prompt, pic};
    for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) shr_lyr_destroy(layers[i]);
    shr_pl_res_image_release(img);
    shr_begin_shutdown(ctx);
    while (shr_pl_res_bitmap_font_destroy(font) == SHR_E_STATE) shr_pump(ctx);
    while (shr_destroy(ctx) == SHR_E_WOULD_BLOCK) shr_pump(ctx);
    shr_software_driver_destroy(&drv);
    free(out.pixels);
    return out.presented && !failed ? 0 : 1;
}
