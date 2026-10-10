/* Scene catalog: public API only. Sizes are in cells where text is involved, so every cell size renders. */
#include "render.h"

#define FG SHR_RGB(0xF8, 0xF8, 0xF2)
#define BAR SHR_RGB(0x33, 0x37, 0x48)
#define SEL SHR_RGB(0x44, 0x47, 0x5A)
#define EDGE SHR_RGB(0x62, 0x72, 0xA4)
#define PINK SHR_RGB(0xFF, 0x79, 0xC6)
#define GREEN SHR_RGB(0x50, 0xFA, 0x7B)
#define YELLOW SHR_RGB(0xF1, 0xFA, 0x8C)
#define CYAN SHR_RGB(0x8B, 0xE9, 0xFD)
#define PURPLE SHR_RGB(0xBD, 0x93, 0xF9)
#define ORANGE SHR_RGB(0xFF, 0xB8, 0x6C)
#define RED SHR_RGB(0xFF, 0x55, 0x55)
#define DARK SHR_RGB(0x28, 0x2A, 0x36)
#define N(a) (sizeof(a) / sizeof((a)[0]))
#define STYLE(fg, bg, flags) ((shr_text_style){(fg), (bg), (flags)})
#define LINE(kind, color) ((shr_text_line){0, 0, (kind), SHR_LINE_SINGLE, 0, (color)})
#define UNDER(col, cols, color) ((shr_text_line){(col), (cols), SHR_LINE_UNDER, SHR_LINE_SINGLE, 0, (color)})

static const shr_text_style plain = {FG, 0, 0};

static shr_rect cells(int32_t col, int32_t row, int32_t cols, int32_t rows) {
    return (shr_rect){col * CW, row * CH, (col + cols) * CW, (row + rows) * CH};
}

/* `line` under all of `utf8` as stage_text places it from (row, col) of a grid `cols` wide. */
static void text_line(stage *s, shr_lyr *l, int32_t row, int32_t col, int32_t cols, const char *utf8, shr_text_line line) {
    shr_text_line out[64];
    stage_lines(s, l, row, out, stage_text_lines(utf8, 0, strlen(utf8), row, col, cols, 0, row, line, out, 0));
}

/* ===== The scene of the original golden test, kept pixel for pixel ===== */

#define GW 256
#define GH 128

static shr_status overview(stage *s) {
    shr_rect box = {GW - 72, CH + 4, GW - 4, GH - CH - 4};
    shr_lyr *shapes = stage_layer(s, 0, (shr_rect){0, 0, GW, GH});
    shr_lyr_cmd_begin(shapes);
    stage_fill(s, shapes, (shr_rect){0, 0, GW, CH}, BAR);
    stage_fill(s, shapes, box, DARK);
    shr_rect edges[4] = {{box.x0, box.y0, box.x1, box.y0 + 2}, {box.x0, box.y1 - 2, box.x1, box.y1},
                         {box.x0, box.y0, box.x0 + 2, box.y1}, {box.x1 - 2, box.y0, box.x1, box.y1}};
    for (int i = 0; i < 4; i++) stage_fill(s, shapes, edges[i], EDGE);
    stage_ok(s, shr_lyr_cmd_commit(shapes), "commit");

    /* Created on top, then moved under everything: shows only where no other layer draws. */
    shr_lyr *under = stage_layer(s, 3, (shr_rect){0, GH - CH - 6, GW, GH - CH});
    shr_lyr_cmd_begin(under);
    stage_fill(s, under, (shr_rect){0, 0, GW, 6}, YELLOW);
    stage_ok(s, shr_lyr_cmd_commit(under), "commit");
    stage_ok(s, shr_lyr_set_z(under, -1), "set_z");

    shr_lyr *hidden = stage_layer(s, 9, (shr_rect){0, 0, GW, GH});
    shr_lyr_cmd_begin(hidden);
    stage_fill(s, hidden, (shr_rect){0, 0, GW, GH}, SHR_RGB(255, 0, 0));
    stage_ok(s, shr_lyr_cmd_commit(hidden), "commit");
    stage_ok(s, shr_lyr_set_visible(hidden, false), "set_visible");

    shr_lyr *corner = stage_layer(s, 2, (shr_rect){0, 0, 24, 24});
    shr_lyr_cmd_begin(corner);
    stage_fill(s, corner, (shr_rect){0, 0, 24, 24}, CYAN);
    stage_fill(s, corner, (shr_rect){4, 4, 8, 8}, DARK);
    stage_ok(s, shr_lyr_cmd_commit(corner), "commit");
    stage_ok(s, shr_lyr_set_rect(corner, (shr_rect){GW - 20, CH + 36, GW + 28, CH + 84}), "set_rect");

    shr_lyr *title = stage_grid(s, 1, (shr_rect){0, 0, GW, CH}, NULL);
    stage_text(s, title, 0, 1, "Shiroko 렌더러 ✓", STYLE(FG, 0, SHR_STYLE_BOLD), 0);

    const char *body = "A가\U0001F600B\tx\nwrap 한글 é ❤️ \U0001F469‍\U0001F4BB "
                       "\U0001F1F0\U0001F1F7 italic 漢字 end";
    shr_style_run runs[2] = {{0, 1, {PINK, 0, SHR_STYLE_BOLD}}, {0, 6, {GREEN, 0, SHR_STYLE_ITALIC}}};
    runs[1].byte_start = (size_t)(strstr(body, "italic") - body), runs[1].byte_end = runs[1].byte_start + 6;
    int32_t rows = (GH - 3 * CH) / CH, cols = (GW - 80) / CW;
    shr_lyr *text = stage_grid(s, 1, (shr_rect){0, CH, GW - 80, GH - 2 * CH}, NULL);
    if (text) {
        shr_error_info err;
        stage_ok(s, shr_pl_lyr_tilemap_set_text(text, 0, 0, body, strlen(body), plain, runs, 2, SHR_TEXT_WRAP, &err),
                 "set_text");
        for (int32_t r = 0; r < rows; r++) { /* underlined but the runs */
            shr_text_line ul[32], line = LINE(SHR_LINE_UNDER, FG);
            size_t n = stage_text_lines(body, 1, runs[1].byte_start, 0, 0, cols, SHR_TEXT_WRAP, r, line, ul, 0);
            n = stage_text_lines(body, runs[1].byte_end, strlen(body), 0, 0, cols, SHR_TEXT_WRAP, r, line, ul, n);
            stage_lines(s, text, r, ul, n);
        }
        if ((GH - 2 * CH) / CH > 4 && cols >= 22) /* other cell sizes: the grid may be smaller */
            stage_ok(s, shr_pl_lyr_tilemap_clear(text, 4, 16, 1, 6, STYLE(0, SEL, 0)), "clear");
    }

    shr_lyr *prompt = stage_grid(s, 1, (shr_rect){0, GH - CH, GW, GH}, NULL);
    stage_cell(s, prompt, 0, 0, "$", 1, STYLE(FG, SEL, 0));
    stage_cell(s, prompt, 0, 1, "", 1, STYLE(SEL, 0, 0));
    stage_cell(s, prompt, 0, 2, "", 1, STYLE(0, FG, 0));
    stage_cell(s, prompt, 0, 4, "d", 1, STYLE(DIM(FG), 0, 0));
    stage_cell(s, prompt, 0, 5, "", 1, STYLE(ORANGE, 0, 0));
    stage_cell(s, prompt, 0, 7, "각", 2, plain);
    stage_cell(s, prompt, 0, 9, "\U0001F600", 2, plain);
    stage_cell(s, prompt, 0, 11, "\U0001F469‍\U0001F4BB", 2, plain);
    stage_cell(s, prompt, 0, 13, "x", 1, STYLE(HIDDEN(FG), PURPLE, 0));
    stage_cell(s, prompt, 0, 14, "b", 1, STYLE(FG, 0, SHR_STYLE_BLINK | SHR_STYLE_BOLD));
    stage_cell(s, prompt, 0, 15, "█", 1, STYLE(GREEN, 0, 0));
    stage_cell(s, prompt, 0, 16, "─", 1, plain);
    stage_cell(s, prompt, 0, 17, "�", 1, plain);
    shr_text_line pl[2] = {{4, 1, SHR_LINE_STRIKE, SHR_LINE_SINGLE, 0, DIM(FG)}, UNDER(7, 2, FG)};
    stage_lines(s, prompt, 0, pl, 2);

    uint8_t rgba[24 * 24 * 4];
    for (int y = 0; y < 24; y++)
        for (int x = 0; x < 24; x++) {
            uint8_t *p = rgba + (y * 24 + x) * 4;
            p[0] = (uint8_t)(x * 10), p[1] = (uint8_t)(y * 10), p[2] = 0xC0, p[3] = (uint8_t)(x < 12 ? 255 : (23 - x) * 20);
        }
    shr_pl_res_image *img = stage_image(s, 24, 24, rgba);
    uint8_t white[4 * 4 * 4];
    memset(white, 0xFF, sizeof(white));
    if (img) stage_ok(s, shr_pl_res_image_update(img, (shr_rect){2, 2, 6, 6}, white, 16), "image_update");
    shr_lyr *pic = stage_layer(s, 2, (shr_rect){box.x0 + 6, box.y0 + 6, box.x1 - 6, box.y1 - 6});
    shr_lyr_cmd_begin(pic);
    stage_ok(s, shr_lyr_cmd_image(pic, img, (shr_rect){0, 0, 24, 24}, (shr_point){0, 0}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_image(pic, img, (shr_rect){8, 8, 24, 24}, (shr_point){30, 40}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_image(pic, img, (shr_rect){0, 0, 24, 24}, (shr_point){44, 4}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_commit(pic), "commit");
    return s->st;
}

/* ===== Wide clusters at the last column, cut by the grid and by resize ===== */

#define WIDE_COLS 16

/* Rows set_text lays out the same way at WIDE_COLS and WIDE_COLS - 1 columns. */
static void wide_common(stage *s, shr_lyr *l) {
    shr_text_style cut = STYLE(FG, PURPLE, 0);
    stage_text(s, l, 0, 0, "abcdefghijklmno가", cut, 0);
    stage_text(s, l, 1, 0, "abcdefghijklmno\U0001F600", cut, 0);
    stage_text(s, l, 2, 0, "abcdefghijklmno가나", STYLE(FG, SEL, 0), SHR_TEXT_WRAP);
    stage_cell(s, l, 6, 0, "漢", 2, plain);
    stage_lines(s, l, 6, &UNDER(0, 2, FG), 1);
    stage_cell(s, l, 6, 1, "x", 1, STYLE(YELLOW, 0, 0)); /* overlaps the wide cell, which is cleared, and its line */
    stage_lines(s, l, 6, NULL, 0);
    stage_text(s, l, 7, 1, "漢字漢字漢字漢字", STYLE(FG, PURPLE, 0), 0);
}

static shr_lyr *wide_grid(stage *s, int32_t cols) {
    shr_lyr *under = stage_layer(s, 0, cells(1, 1, WIDE_COLS, 8));
    shr_lyr_cmd_begin(under);
    stage_fill(s, under, cells(0, 0, WIDE_COLS, 8), BAR);
    stage_ok(s, shr_lyr_cmd_commit(under), "commit");
    shr_lyr *l = stage_layer(s, 1, cells(1, 1, WIDE_COLS, 8));
    if (l) stage_ok(s, shr_pl_lyr_tilemap_resize(l, s->font, 8, cols, NULL), "resize");
    return l;
}

static shr_status wide_edge(stage *s) {
    shr_lyr *l = wide_grid(s, WIDE_COLS);
    wide_common(s, l);
    stage_cell(s, l, 4, WIDE_COLS - 2, "가", 2, STYLE(FG, SEL, 0));
    stage_cell(s, l, 5, WIDE_COLS - 2, "\U0001F469‍\U0001F4BB", 2, plain);
    if (l) {
        shr_text_style st = plain;
        stage_expect(s, shr_pl_lyr_tilemap_set_cell(l, 4, WIDE_COLS - 1, "가", 3, 2, st), SHR_E_INVALID_ARG,
                     "wide set_cell past the last column");
    }
    return s->st;
}

static shr_status wide_resize(stage *s) {
    shr_lyr *l = s->nlayers ? s->layers[s->nlayers - 1] : NULL;
    if (l) stage_ok(s, shr_pl_lyr_tilemap_resize(l, s->font, 8, WIDE_COLS - 1, NULL), "resize");
    return s->st;
}

static shr_status wide_resize_ref(stage *s) {
    wide_common(s, wide_grid(s, WIDE_COLS - 1));
    return s->st;
}

/* ===== Clusters ===== */

static shr_status combining(stage *s) {
    static const char *rows[] = {
        "é à́̂ ö̲ ñ é (NFC)",
        "각 한 한글 가",
        "क्षि नमस्ते",
        "กำ ที่ العربية",
        "́ leading mark, x‍y zwj, a​b zwsp",
        "A️ 漢\U000E0100 葛\U000E0101 IVS,  ",
        "Z̤̹̗a̭̠l̵g̡o̷ zalgo",
        "กำำำ िि stacked",
    };
    shr_lyr *l = stage_grid(s, 1, cells(1, 1, 38, (int32_t)N(rows)), NULL);
    for (size_t i = 0; i < N(rows); i++) stage_text(s, l, (int32_t)i, 0, rows[i], plain, 0);
    /* More scalars than one cluster may hold: U+FFFD. */
    char big[128] = "x";
    for (int i = 0; i < 20; i++) strcat(big, "́");
    shr_error_info err;
    if (l)
        stage_expect(s, shr_pl_lyr_tilemap_set_text(l, 0, 30, big, strlen(big), plain, NULL, 0, 0, &err), SHR_OK,
                     "cluster over the profile limit");
    return s->st;
}

static shr_status emoji_sequences(stage *s) {
    static const char *rows[] = {
        "zwj   \U0001F469‍\U0001F4BB \U0001F468‍\U0001F469‍\U0001F467‍\U0001F466 "
        "\U0001F3F3️‍\U0001F308 \U0001F9D1\U0001F3FD‍\U0001F680 \U0001F3F4‍☠️ "
        "❤️‍\U0001F525",
        "flag  \U0001F1F0\U0001F1F7 \U0001F1EF\U0001F1F5 \U0001F1FA\U0001F1F3 \U0001F1E6 "
        "\U0001F1F0\U0001F1F7\U0001F1E6",
        "keys  #️⃣ 1️⃣ *️⃣ 1⃣ #",
        "vs15  ☺︎ ❤︎ ⌚︎ ©︎ ↔︎",
        "vs16  ☺️ ❤️ ⌚️ ©️ ↔️",
        "bare  ☺ ❤ ⌚ © ↔ \U0001F600",
        "skin  \U0001F44D\U0001F3FD \U0001F44B\U0001F3FF \U0001F3FD \U0001F9D1\U0001F3FB",
        "tags  \U0001F3F4\U000E0067\U000E0062\U000E0073\U000E0063\U000E0074\U000E007F "
        "\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007F",
    };
    const int32_t cols = 40;
    shr_lyr *l = stage_grid(s, 1, cells(1, 1, cols, (int32_t)N(rows) + 2), NULL);
    for (size_t i = 0; i < N(rows); i++) stage_text(s, l, (int32_t)i, 0, rows[i], plain, 0);
    int32_t r = (int32_t)N(rows);
    stage_text(s, l, r, cols - 5, "edge\U0001F600", STYLE(FG, SEL, 0), 0);
    stage_cell(s, l, r + 1, cols - 2, "\U0001F1F0\U0001F1F7", 2, plain);
    stage_cell(s, l, r + 1, cols - 4, "#️⃣", 2, plain);
    stage_cell(s, l, r + 1, cols - 6, "☺︎", 1, plain);
    stage_cell(s, l, r + 1, cols - 8, "☺️", 2, plain);
    return s->st;
}

/* ===== Styles; the blink row shows the phase the scene's clock is in ===== */

static shr_status styles(stage *s) {
    enum { UL = 1, ST = 2 }; /* the lines over the sample; concealed text has none */
    static const struct {
        const char *label;
        uint32_t alpha, flags, lines;
        bool bg;
    } rows[] = {
        {"plain", 255, 0, 0, false},
        {"bold", 255, SHR_STYLE_BOLD, 0, false},
        {"italic", 255, SHR_STYLE_ITALIC, 0, false},
        {"bold italic", 255, SHR_STYLE_BOLD | SHR_STYLE_ITALIC, 0, false},
        {"dim", 128, 0, 0, false},
        {"underline", 255, 0, UL, false},
        {"strike", 255, 0, ST, false},
        {"under+strike", 255, 0, UL | ST, false},
        {"blink", 255, SHR_STYLE_BLINK, 0, false},
        {"blink+bg+ul", 255, SHR_STYLE_BLINK, UL, true},
        {"conceal", 0, 0, 0, false},
        {"conceal+bg+ul", 0, 0, 0, true},
        {"bg", 255, 0, 0, true},
        {"dim+bg", 128, 0, 0, true},
        {"all", 0, SHR_STYLE_KNOWN_FLAGS, 0, true},
    };
    const char *sample = "Agjy가漢\U0001F600─█é";
    shr_lyr *l = stage_grid(s, 1, cells(1, 1, 34, (int32_t)N(rows) + 1), NULL);
    for (size_t i = 0; i < N(rows); i++) {
        int32_t r = (int32_t)i;
        shr_text_style st = STYLE(HIDDEN(i % 2 ? CYAN : FG) | rows[i].alpha << 24, rows[i].bg ? SEL : 0, rows[i].flags);
        stage_text(s, l, r, 0, rows[i].label, STYLE(EDGE, 0, 0), 0);
        stage_text(s, l, r, 14, sample, st, 0);
        shr_text_line line = LINE(SHR_LINE_UNDER, st.fg), out[4];
        line.flags = st.flags & SHR_STYLE_BLINK ? SHR_TEXT_LINE_BLINK : 0;
        size_t n = rows[i].lines & UL ? stage_text_lines(sample, 0, strlen(sample), r, 14, 34, 0, r, line, out, 0) : 0;
        line.kind = SHR_LINE_STRIKE;
        n = rows[i].lines & ST ? stage_text_lines(sample, 0, strlen(sample), r, 14, 34, 0, r, line, out, n) : n;
        if (n) stage_lines(s, l, r, out, n);
    }
    int32_t r = (int32_t)N(rows);
    stage_text(s, l, r, 0, "fg == bg", STYLE(EDGE, 0, 0), 0);
    stage_text(s, l, r, 14, sample, STYLE(PINK, PINK, 0), 0);
    return s->st;
}

/* Sprites, emoji and icons are drawn as they are, whatever BOLD and ITALIC say. */
static shr_status styled_exempt_with(stage *s, uint32_t flags) {
    shr_lyr *l = stage_grid(s, 1, cells(1, 1, 14, 1), NULL);
    stage_text(s, l, 0, 0, "─│┼█▒\U0001FB00⣿\uF5D1\U0001CDE5 \U0001F600 \uE0B0", STYLE(FG, 0, flags), 0);
    return s->st;
}

static shr_status styled_exempt(stage *s) { return styled_exempt_with(s, SHR_STYLE_BOLD | SHR_STYLE_ITALIC); }
static shr_status styled_exempt_ref(stage *s) { return styled_exempt_with(s, 0); }

/* ===== Tilemap background vs transparent cells over an image ===== */

static void gradient(uint8_t *rgba, int32_t w, int32_t h, bool holes) {
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            p[0] = (uint8_t)(x * 255 / (w - 1)), p[1] = (uint8_t)(y * 255 / (h - 1)), p[2] = (uint8_t)(255 - x * 255 / (w - 1));
            p[3] = holes && ((x / 8 + y / 8) % 3 == 0) ? 128 : 255;
        }
}

static shr_status tilemap_over_image(stage *s) {
    int32_t w = 40 * CW, h = 8 * CH;
    uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
    if (!rgba) return SHR_E_NO_MEMORY;
    gradient(rgba, w, h, true);
    shr_pl_res_image *img = stage_image(s, w, h, rgba);
    free(rgba);
    shr_lyr *pic = stage_layer(s, 0, cells(0, 0, 40, 8));
    shr_lyr_cmd_begin(pic);
    stage_ok(s, shr_lyr_cmd_image(pic, img, (shr_rect){0, 0, w, h}, (shr_point){0, 0}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_commit(pic), "commit");
    const shr_color bg = DARK;
    shr_lyr *opaque = stage_grid(s, 1, cells(1, 1, 18, 6), &bg), *clear = stage_grid(s, 1, cells(21, 1, 18, 6), NULL);
    shr_lyr *both[2] = {opaque, clear};
    for (int i = 0; i < 2; i++) {
        stage_text(s, both[i], 0, 0, i ? "transparent" : "background", STYLE(YELLOW, 0, SHR_STYLE_BOLD), 0);
        stage_text(s, both[i], 1, 0, "text 한글 \U0001F600 █▒", plain, 0);
        stage_text(s, both[i], 2, 2, "bg cells", STYLE(FG, SEL, 0), 0);
        stage_text(s, both[i], 3, 0, "underline dim", STYLE(DIM(FG), 0, 0), 0);
        text_line(s, both[i], 3, 0, 18, "underline dim", LINE(SHR_LINE_UNDER, DIM(FG)));
        stage_cell(s, both[i], 4, 3, "", 4, STYLE(0, PURPLE, 0));
        stage_text(s, both[i], 5, 0, "──┼── ", STYLE(CYAN, 0, 0), 0);
    }
    return s->st;
}

static void tm_text(stage *s, shr_lyr *l) {
    stage_text(s, l, 0, 0, "Background 한글 \U0001F600 é", plain, 0);
    stage_text(s, l, 1, 2, "under█lined", STYLE(PINK, 0, 0), 0);
    text_line(s, l, 1, 2, 24, "under█lined", LINE(SHR_LINE_UNDER, PINK));
    stage_text(s, l, 2, 0, "own bg", STYLE(FG, SEL, 0), 0);
}

static shr_status tm_background(stage *s) {
    const shr_color bg = DARK;
    tm_text(s, stage_grid(s, 1, cells(1, 1, 24, 4), &bg));
    return s->st;
}

static shr_status tm_background_ref(stage *s) {
    shr_lyr *under = stage_layer(s, 0, cells(1, 1, 24, 4));
    shr_lyr_cmd_begin(under);
    stage_fill(s, under, cells(0, 0, 24, 4), DARK);
    stage_ok(s, shr_lyr_cmd_commit(under), "commit");
    tm_text(s, stage_grid(s, 1, cells(1, 1, 24, 4), NULL));
    return s->st;
}

/* ===== Layers ===== */

#define LW 256
#define LH 160

static shr_lyr *box_layer(stage *s, int32_t z, shr_rect r, shr_color c) {
    shr_lyr *l = stage_layer(s, z, r);
    shr_lyr_cmd_begin(l);
    stage_fill(s, l, (shr_rect){0, 0, r.x1 - r.x0, r.y1 - r.y0}, c);
    stage_fill(s, l, (shr_rect){2, 2, 6, 6}, DARK);
    stage_ok(s, shr_lyr_cmd_commit(l), "commit");
    return l;
}

static shr_status layers(stage *s) {
    shr_lyr *big = stage_layer(s, -5, (shr_rect){-40, -40, LW + 40, LH + 40});
    shr_lyr_cmd_begin(big);
    for (int32_t x = 0; x < LW + 80; x += 16) stage_fill(s, big, (shr_rect){x, 0, x + 8, LH + 80}, BAR);
    stage_ok(s, shr_lyr_cmd_commit(big), "commit");

    box_layer(s, 0, (shr_rect){16, 16, 80, 64}, PINK);
    box_layer(s, 1, (shr_rect){48, 40, 112, 88}, GREEN);
    box_layer(s, 2, (shr_rect){80, 24, 144, 72}, CYAN);
    box_layer(s, 1, (shr_rect){96, 56, 136, 100}, ORANGE); /* same z: later creation on top */
    stage_ok(s, shr_lyr_set_visible(box_layer(s, 7, (shr_rect){0, 0, LW, LH}, RED), false), "set_visible");
    box_layer(s, 3, (shr_rect){LW - 20, LH - 24, LW + 20, LH + 24}, YELLOW);
    box_layer(s, 3, (shr_rect){-12, -10, 20, 22}, PURPLE);
    box_layer(s, 3, (shr_rect){LW + 8, 10, LW + 40, 40}, RED);  /* off screen */
    box_layer(s, 3, (shr_rect){20, -40, 60, -2}, RED);          /* off screen */
    box_layer(s, 3, (shr_rect){200, 100, 201, 101}, FG);        /* one pixel */
    box_layer(s, 3, (shr_rect){210, 100, 210, 140}, RED);       /* empty rect */
    stage_layer(s, 4, (shr_rect){0, 0, LW, LH});                /* no commands */

    shr_lyr *clip = stage_layer(s, 4, (shr_rect){160, 90, 220, 140});
    shr_lyr_cmd_begin(clip);
    stage_fill(s, clip, (shr_rect){-100, -100, 1000, 1000}, SEL);
    stage_fill(s, clip, (shr_rect){-4, -4, 8, 8}, GREEN);
    stage_fill(s, clip, (shr_rect){52, 42, 70, 70}, GREEN);
    stage_fill(s, clip, (shr_rect){20, 20, 20, 30}, RED); /* empty */
    stage_ok(s, shr_lyr_cmd_commit(clip), "commit");

    shr_lyr *moved = box_layer(s, 5, (shr_rect){0, 0, 30, 20}, EDGE);
    stage_ok(s, shr_lyr_set_rect(moved, (shr_rect){150, 120, 190, 150}), "set_rect");
    stage_ok(s, shr_lyr_set_z(moved, -1), "set_z");

    shr_lyr *g = stage_grid(s, 6, (shr_rect){-CW, LH - 2 * CH + 4, 20 * CW, LH + 4}, NULL);
    stage_text(s, g, 0, 0, "-off the left and bottom 가", STYLE(FG, SEL, 0), 0);
    stage_text(s, g, 1, 0, "-second row is cut", STYLE(FG, SEL, 0), 0);
    return s->st;
}

static void occluder(stage *s) {
    box_layer(s, 0, (shr_rect){8, 8, 120, 72}, CYAN);
    shr_lyr *top = stage_layer(s, 1, (shr_rect){0, 0, 128, 80});
    shr_lyr_cmd_begin(top);
    stage_fill(s, top, (shr_rect){0, 0, 128, 80}, DARK);
    stage_fill(s, top, (shr_rect){40, 20, 90, 60}, PINK);
    stage_ok(s, shr_lyr_cmd_commit(top), "commit");
}

static shr_status occluded(stage *s) {
    box_layer(s, -1, (shr_rect){4, 4, 100, 60}, RED);
    box_layer(s, 0, (shr_rect){-20, 30, 60, 100}, RED);
    occluder(s);
    return s->st;
}

static shr_status occluded_ref(stage *s) {
    occluder(s);
    return s->st;
}

static shr_status hidden(stage *s) {
    occluder(s);
    shr_lyr *l = box_layer(s, 9, (shr_rect){10, 10, 100, 60}, RED);
    stage_ok(s, shr_lyr_set_visible(l, false), "set_visible");
    shr_lyr *g = stage_grid(s, 9, cells(1, 1, 8, 2), NULL);
    stage_text(s, g, 0, 0, "hidden가", STYLE(RED, FG, 0), 0);
    stage_ok(s, shr_lyr_set_visible(g, false), "set_visible");
    return s->st;
}

static shr_status moved(stage *s) {
    occluder(s);
    shr_lyr *l = box_layer(s, 2, (shr_rect){0, 0, 50, 30}, GREEN);
    stage_ok(s, shr_lyr_set_rect(l, (shr_rect){-30, 40, 20, 70}), "set_rect");
    stage_ok(s, shr_lyr_set_rect(l, (shr_rect){90, 50, 140, 80}), "set_rect");
    return s->st;
}

static shr_status moved_ref(stage *s) {
    occluder(s);
    box_layer(s, 2, (shr_rect){90, 50, 140, 80}, GREEN);
    return s->st;
}

static shr_status zorder(stage *s) {
    shr_lyr *a = box_layer(s, 5, (shr_rect){10, 10, 70, 50}, PINK);
    shr_lyr *b = box_layer(s, 4, (shr_rect){30, 20, 90, 60}, GREEN);
    shr_lyr *c = box_layer(s, 3, (shr_rect){50, 30, 110, 70}, CYAN);
    stage_ok(s, shr_lyr_set_z(a, 0), "set_z");
    stage_ok(s, shr_lyr_set_z(b, 1), "set_z");
    stage_ok(s, shr_lyr_set_z(c, 2), "set_z");
    return s->st;
}

static shr_status zorder_ref(stage *s) {
    box_layer(s, 0, (shr_rect){10, 10, 70, 50}, PINK);
    box_layer(s, 1, (shr_rect){30, 20, 90, 60}, GREEN);
    box_layer(s, 2, (shr_rect){50, 30, 110, 70}, CYAN);
    return s->st;
}

/* ===== Many commands, partial updates ===== */

#define MW 256
#define MH 128

static shr_color hashed(uint32_t i) { return SHR_RGB((i * 67 + 13) % 256, (i * 151 + 7) % 256, (i * 29 + 101) % 256); }

static void many_list(stage *s, shr_lyr *l, bool changed) {
    shr_lyr_cmd_begin(l);
    for (uint32_t i = 0; i < 64u * 32u; i++) {
        int32_t x = (int32_t)(i % 64) * 4, y = (int32_t)(i / 64) * 4;
        bool hit = changed && (i % 97 == 5 || i == 700);
        if (changed && (i == 300 || i == 1500)) x += 2, y += 1; /* moved */
        stage_fill(s, l, (shr_rect){x, y, x + 4, y + 4}, hit ? SHR_RGB(255, 255, 255) : hashed(i));
    }
    shr_pl_res_image *img = s->nimages ? s->images[0] : NULL;
    for (int32_t i = 0; i < 16; i++) {
        int32_t at = changed && i == 7 ? 3 : 0;
        stage_ok(s, shr_lyr_cmd_image(l, img, (shr_rect){0, 0, 12, 12}, (shr_point){i * 16 + at, 64 + (i % 4) * 14}),
                 "cmd_image");
    }
    stage_ok(s, shr_lyr_cmd_commit(l), "commit");
}

static shr_lyr *many_setup(stage *s) {
    uint8_t rgba[12 * 12 * 4];
    gradient(rgba, 12, 12, true);
    stage_image(s, 12, 12, rgba);
    return stage_layer(s, 0, (shr_rect){0, 0, MW, MH});
}

static shr_status many(stage *s) {
    many_list(s, many_setup(s), false);
    return s->st;
}

static shr_status many_update(stage *s) {
    many_list(s, s->nlayers ? s->layers[0] : NULL, true);
    return s->st;
}

static shr_status many_ref(stage *s) {
    many_list(s, many_setup(s), true);
    return s->st;
}

static void tm_initial(stage *s, shr_lyr *l) {
    for (int32_t r = 0; r < 6; r++) stage_text(s, l, r, 0, "row 한글 \U0001F600 0123456789", plain, 0);
}

static shr_status tm_update_build(stage *s) {
    tm_initial(s, stage_grid(s, 1, cells(1, 1, 26, 6), NULL));
    return s->st;
}

static void tm_changes(stage *s, shr_lyr *l) {
    stage_cell(s, l, 1, 4, "X", 1, STYLE(PINK, 0, SHR_STYLE_BOLD));
    stage_cell(s, l, 2, 5, "漢", 2, STYLE(FG, SEL, 0)); /* overlaps the second half of a wide cell */
    stage_text(s, l, 3, 9, "\U0001F469‍\U0001F4BB!", STYLE(GREEN, 0, 0), 0);
    text_line(s, l, 3, 9, 26, "\U0001F469‍\U0001F4BB!", LINE(SHR_LINE_UNDER, GREEN));
    if (l) stage_ok(s, shr_pl_lyr_tilemap_clear(l, 4, 3, 1, 4, STYLE(0, PURPLE, 0)), "clear");
    if (l) stage_ok(s, shr_pl_lyr_tilemap_clear(l, 5, 0, 1, 26, plain), "clear");
}

static shr_status tm_update(stage *s) {
    tm_changes(s, s->nlayers ? s->layers[0] : NULL);
    return s->st;
}

static shr_status tm_update_ref(stage *s) {
    shr_lyr *l = stage_grid(s, 1, cells(1, 1, 26, 6), NULL);
    tm_initial(s, l);
    tm_changes(s, l);
    return s->st;
}

/* Restyled cached rows: the update only adds BOLD and ITALIC to the text the build drew. */
static const char restyle_text[] = "abc 한글 ─ \U0001F600";

static shr_status tm_restyle_build(stage *s) {
    const shr_color bg = DARK;
    stage_text(s, stage_grid(s, 1, cells(1, 1, 16, 1), &bg), 0, 0, restyle_text, plain, 0);
    return s->st;
}

static shr_status tm_restyle(stage *s) {
    stage_text(s, s->nlayers ? s->layers[0] : NULL, 0, 0, restyle_text,
               STYLE(FG, 0, SHR_STYLE_BOLD | SHR_STYLE_ITALIC), 0);
    return s->st;
}

static shr_status tm_restyle_ref(stage *s) {
    const shr_color bg = DARK;
    stage_text(s, stage_grid(s, 1, cells(1, 1, 16, 1), &bg), 0, 0, restyle_text,
               STYLE(FG, 0, SHR_STYLE_BOLD | SHR_STYLE_ITALIC), 0);
    return s->st;
}

/* Clear: wide cells reaching into the range are cleared whole, to the style's bg. */
static shr_status cleared(stage *s) {
    shr_lyr *l = stage_grid(s, 1, cells(1, 1, 12, 4), NULL);
    stage_text(s, l, 0, 0, "keep", plain, 0);
    stage_text(s, l, 1, 0, "gone gone", plain, 0);
    stage_text(s, l, 2, 0, "가나다라", plain, 0);
    if (l) stage_ok(s, shr_pl_lyr_tilemap_clear(l, 1, 0, 1, 12, plain), "clear");
    if (l) stage_ok(s, shr_pl_lyr_tilemap_clear(l, 2, 3, 1, 2, STYLE(0, PURPLE, 0)), "clear");
    return s->st;
}

static shr_status cleared_ref(stage *s) {
    shr_lyr *l = stage_grid(s, 1, cells(1, 1, 12, 4), NULL);
    stage_text(s, l, 0, 0, "keep", plain, 0);
    stage_cell(s, l, 2, 0, "가", 2, plain);
    for (int32_t c = 2; c < 6; c++) stage_cell(s, l, 2, c, "", 1, STYLE(0, PURPLE, 0));
    stage_cell(s, l, 2, 6, "라", 2, plain);
    return s->st;
}

/* set_text lays out what a VT engine would place cell by cell. */
static shr_status text_layout(stage *s) {
    shr_lyr *l = stage_grid(s, 1, cells(1, 1, 16, 2), NULL);
    const char *t = "A가\U0001F600é 漢\t|";
    shr_style_run run = {1, 4, {PINK, SEL, 0}};
    shr_error_info err;
    if (l) stage_ok(s, shr_pl_lyr_tilemap_set_text(l, 0, 0, t, strlen(t), plain, &run, 1, 0, &err), "set_text");
    shr_text_line ul[2];
    stage_lines(s, l, 0, ul, stage_text_lines(t, run.byte_start, run.byte_end, 0, 0, 16, 0, 0, LINE(SHR_LINE_UNDER, PINK), ul, 0));
    stage_text(s, l, 1, 3, "x\U0001F1F0\U0001F1F7#️⃣", plain, 0);
    return s->st;
}

static shr_status text_cells(stage *s) {
    shr_lyr *l = stage_grid(s, 1, cells(1, 1, 16, 2), NULL);
    stage_cell(s, l, 0, 0, "A", 1, plain);
    stage_cell(s, l, 0, 1, "가", 2, STYLE(PINK, SEL, 0));
    stage_lines(s, l, 0, &UNDER(1, 2, PINK), 1);
    stage_cell(s, l, 0, 3, "\U0001F600", 2, plain);
    stage_cell(s, l, 0, 5, "é", 1, plain);
    stage_cell(s, l, 0, 6, " ", 1, plain);
    stage_cell(s, l, 0, 7, "漢", 2, plain);
    shr_text_profile_info prof;
    stage_ok(s, shr_pl_lyr_tilemap_profile_get(&prof), "profile_get");
    int32_t stop = (9 / (int32_t)prof.tab_stop + 1) * (int32_t)prof.tab_stop;
    stage_cell(s, l, 0, 9, "", (uint32_t)(stop - 9), plain); /* TAB: blank cells of its style */
    if (stop < 16) stage_cell(s, l, 0, stop, "|", 1, plain);
    stage_cell(s, l, 1, 3, "x", 1, plain);
    stage_cell(s, l, 1, 4, "\U0001F1F0\U0001F1F7", 2, plain);
    stage_cell(s, l, 1, 6, "#️⃣", 2, plain);
    return s->st;
}

/* ===== Scrolling: rows moved as pixels against the same rows drawn directly ===== */

#define SC_COLS 24
#define SC_ROWS 8

static void scroll_line(stage *s, shr_lyr *l, int32_t row, int line) {
    static const char *const text[] = {"line 0 abc 한글", "line 1 \U0001F600 wide 漢字", "line 2 ─── box ───",
                                       "line 3 é combining", "line 4 tab\there", "line 5 \U0001F469‍\U0001F4BB zwj",
                                       "line 6 0123456789", "line 7 last 끝"};
    static const uint32_t flags[] = {0, SHR_STYLE_BOLD, 0, SHR_STYLE_ITALIC, 0}; /* 2 underlined, 4 on SEL */
    shr_color fg = line % 2 ? PINK : FG;
    stage_text(s, l, row, 0, text[line % 8], STYLE(fg, line % 5 == 4 ? SEL : 0, flags[line % 5]), 0);
    if (line % 5 == 2) text_line(s, l, row, 0, SC_COLS, text[line % 8], LINE(SHR_LINE_UNDER, fg));
}

static shr_lyr *scroll_grid(stage *s) {
    const shr_color bg = DARK;
    return stage_grid(s, 1, cells(1, 1, SC_COLS, SC_ROWS), &bg);
}

/* A cursor above the grid and a sprite above both. */
static void scroll_overlays(stage *s, int32_t cursor_row) {
    shr_lyr *cursor = stage_layer(s, 2, cells(4, 1 + cursor_row, 1, 1));
    if (cursor) shr_lyr_cmd_begin(cursor);
    stage_fill(s, cursor, cells(0, 0, 1, 1), GREEN);
    if (cursor) stage_ok(s, shr_lyr_cmd_commit(cursor), "commit");
    uint8_t px[16 * 16 * 4]; /* small: larger layers above make the scroll a redraw */
    for (int i = 0; i < 16 * 16; i++)
        px[4 * i] = 240, px[4 * i + 1] = (uint8_t)(i / 16 * 14), px[4 * i + 2] = 60, px[4 * i + 3] = (uint8_t)(i % 16 * 16);
    shr_pl_res_image *img = stage_image(s, 16, 16, px);
    shr_lyr *sp = stage_layer(s, 3, (shr_rect){9 * CW + 3, 2 * CH + 5, 9 * CW + 19, 2 * CH + 21});
    if (sp) shr_lyr_cmd_begin(sp);
    if (sp) stage_ok(s, shr_lyr_cmd_image(sp, img, (shr_rect){0, 0, 16, 16}, (shr_point){0, 0}), "cmd_image");
    if (sp) stage_ok(s, shr_lyr_cmd_commit(sp), "commit");
}

static shr_status scroll_build(stage *s) {
    shr_lyr *g = scroll_grid(s);
    for (int32_t r = 0; r < SC_ROWS; r++) scroll_line(s, g, r, r);
    scroll_overlays(s, 6);
    return s->st;
}

static const shr_text_style scroll_status = {FG, SEL, 0};

/* Up two rows, then the rows between the first and the last down one; the cursor follows its line. */
static shr_status scroll_update(stage *s) {
    shr_lyr *g = s->nlayers ? s->layers[0] : NULL, *cursor = s->nlayers > 1 ? s->layers[1] : NULL;
    if (g) stage_ok(s, shr_pl_lyr_tilemap_scroll(g, 0, SC_ROWS, 2, scroll_status), "scroll");
    if (g) stage_ok(s, shr_pl_lyr_tilemap_scroll(g, 1, SC_ROWS - 1, -1, plain), "scroll");
    stage_text(s, g, 1, 2, "new line", plain, 0);
    stage_text(s, g, SC_ROWS - 1, 0, "-- status --", scroll_status, 0);
    if (cursor) stage_ok(s, shr_lyr_set_rect(cursor, cells(4, 1 + 5, 1, 1)), "set_rect");
    return s->st;
}

static shr_status scroll_ref(stage *s) {
    shr_lyr *g = scroll_grid(s);
    scroll_line(s, g, 0, 2);
    stage_text(s, g, 1, 2, "new line", plain, 0);
    for (int32_t r = 2; r < SC_ROWS - 1; r++) scroll_line(s, g, r, r + 1);
    if (g) stage_ok(s, shr_pl_lyr_tilemap_clear(g, SC_ROWS - 1, 0, 1, SC_COLS, scroll_status), "clear");
    stage_text(s, g, SC_ROWS - 1, 0, "-- status --", scroll_status, 0);
    scroll_overlays(s, 5);
    return s->st;
}

/* ===== Images ===== */

#define AW 256
#define AH 128

static const uint8_t alphas[5] = {0, 1, 128, 254, 255};

static void alpha_chart(uint8_t *rgba) {
    static const uint8_t colors[5][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}, {0, 0, 0}};
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 160; x++) {
            uint8_t *p = rgba + (y * 160 + x) * 4;
            memcpy(p, colors[y / 8], 3);
            p[3] = alphas[x / 32];
        }
}

static void checkerboard(stage *s, int32_t w, int32_t h) {
    shr_lyr *l = stage_layer(s, -1, (shr_rect){0, 0, w, h});
    shr_lyr_cmd_begin(l);
    stage_fill(s, l, (shr_rect){0, 0, w, h}, SHR_RGB(0x60, 0x60, 0x60));
    for (int32_t y = 0; y < h; y += 8)
        for (int32_t x = (y / 8 % 2) * 8; x < w; x += 16) stage_fill(s, l, (shr_rect){x, y, x + 8, y + 8}, SHR_RGB(0xA0, 0xA0, 0xA0));
    stage_ok(s, shr_lyr_cmd_commit(l), "commit");
}

static void alpha_layer(stage *s, shr_pl_res_image *img) {
    shr_lyr *l = stage_layer(s, 0, (shr_rect){8, 8, AW - 8, AH - 8});
    shr_lyr_cmd_begin(l);
    stage_ok(s, shr_lyr_cmd_image(l, img, (shr_rect){0, 0, 160, 40}, (shr_point){0, 0}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_image(l, img, (shr_rect){0, 0, 160, 40}, (shr_point){-16, 50}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_image(l, img, (shr_rect){32, 8, 128, 32}, (shr_point){150, 50}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_image(l, img, (shr_rect){64, 0, 96, 40}, (shr_point){150, 70}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_image(l, img, (shr_rect){64, 0, 96, 40}, (shr_point){166, 80}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_image(l, img, (shr_rect){0, 0, 160, 40}, (shr_point){180, 100}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_commit(l), "commit");
}

static shr_status image_alpha(stage *s) {
    checkerboard(s, AW, AH);
    uint8_t rgba[160 * 40 * 4];
    alpha_chart(rgba);
    alpha_layer(s, stage_image(s, 160, 40, rgba));
    return s->st;
}

static shr_status image_update(stage *s) {
    uint8_t rgba[32 * 40 * 4];
    for (int i = 0; i < 32 * 40; i++) memcpy(rgba + i * 4, (uint8_t[4]){255, 0, 255, (uint8_t)(i % 32 * 8)}, 4);
    shr_pl_res_image *img = s->nimages ? s->images[0] : NULL;
    if (img) stage_ok(s, shr_pl_res_image_update(img, (shr_rect){64, 0, 96, 40}, rgba, 32 * 4), "image_update");
    return s->st;
}

static shr_status image_update_ref(stage *s) {
    checkerboard(s, AW, AH);
    uint8_t rgba[160 * 40 * 4];
    alpha_chart(rgba);
    for (int y = 0; y < 40; y++)
        for (int x = 64; x < 96; x++) memcpy(rgba + (y * 160 + x) * 4, (uint8_t[4]){255, 0, 255, (uint8_t)((x - 64) * 8)}, 4);
    alpha_layer(s, stage_image(s, 160, 40, rgba));
    return s->st;
}

/* Scaled images: the alpha chart up, a part of it down, all of it down by box, a gray ramp up, and a crop of the
 * enlarged chart; as copies, or as views (driver-scaled except box), which must give the same pixels. The opaque ramp
 * is kept as RGB565 on RGB565 screens, so its copy is made from the image too. */
static shr_pl_res_image *kept(stage *s, shr_status st, shr_pl_res_image *img, const char *what) {
    if (!stage_ok(s, st, what)) return NULL;
    if (s->nimages == sizeof(s->images) / sizeof(s->images[0])) {
        stage_ok(s, SHR_E_LIMIT, "too many images");
        return NULL;
    }
    return s->images[s->nimages++] = img;
}

static void gray_ramp(uint8_t *g, uint8_t *rgba) {
    for (int i = 0; i < 16 * 8; i++) {
        g[i] = (uint8_t)(i % 16 * 16 + i / 16 * 2);
        memset(rgba + i * 4, g[i], 3), rgba[i * 4 + 3] = 255;
    }
}

/* The software driver scales the ramp from its RGB565 pixels widened, a GPU without SHR_DRIVER_IMAGE_565 from 8 bits:
 * one step apart on about 900 pixels. */
#define SCALED_GPU {SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565, 1024}

static const shr_rect scaled_src[4] = {{0, 0, 160, 40}, {32, 8, 128, 32}, {0, 0, 160, 40}, {0, 0, 16, 8}};
static const int32_t scaled_w[4] = {236, 60, 100, 64}, scaled_h[4] = {59, 15, 25, 32};

static void scaled_layer(stage *s, shr_pl_res_image *const *img) {
    static const shr_point at[4] = {{10, 4}, {10, 68}, {80, 68}, {190, 68}};
    shr_lyr *l = stage_layer(s, 0, (shr_rect){0, 0, AW, AH});
    shr_lyr_cmd_begin(l);
    for (int i = 0; i < 4; i++)
        stage_ok(s, shr_lyr_cmd_image(l, img[i], (shr_rect){0, 0, scaled_w[i], scaled_h[i]}, at[i]), "cmd_image");
    stage_ok(s, shr_lyr_cmd_image(l, img[0], (shr_rect){50, 10, 150, 40}, (shr_point){10, 96}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_commit(l), "commit");
}

/* Copies of `chart`, the box one of `box_chart`. */
static shr_status scaled_copies(stage *s, const uint8_t *chart, const uint8_t *box_chart) {
    checkerboard(s, AW, AH);
    uint8_t g[16 * 8], rgba[16 * 8 * 4];
    gray_ramp(g, rgba);
    shr_image_source src[3] = {{160, 40, SHR_IMAGE_SRC_RGBA8888, chart, 160 * 4},
                               {160, 40, SHR_IMAGE_SRC_RGBA8888, chart, 160 * 4},
                               {160, 40, SHR_IMAGE_SRC_RGBA8888, box_chart, 160 * 4}};
    shr_pl_res_image *img[4] = {NULL}, *ramp = stage_image(s, 16, 8, rgba);
    for (int i = 0; i < 4 && s->st == SHR_OK; i++) {
        uint32_t filter = i == 2 ? SHR_SCALE_BOX : SHR_SCALE_BILINEAR;
        shr_status st = i == 3 ? shr_pl_res_image_view(ramp, scaled_src[i], scaled_w[i], scaled_h[i], SHR_SCALE_COPY,
                                                       &img[i])
                               : shr_pl_res_image_create_scaled(s->ctx, &src[i], scaled_src[i], scaled_w[i],
                                                                scaled_h[i], filter, &img[i]);
        img[i] = kept(s, st, img[i], "create_scaled");
    }
    if (s->st == SHR_OK) scaled_layer(s, img);
    return s->st;
}

static shr_status image_scaled(stage *s) {
    uint8_t chart[160 * 40 * 4];
    alpha_chart(chart);
    return scaled_copies(s, chart, chart);
}

static shr_status image_scaled_view(stage *s) {
    checkerboard(s, AW, AH);
    uint8_t chart[160 * 40 * 4], g[16 * 8], rgba[16 * 8 * 4];
    alpha_chart(chart);
    gray_ramp(g, rgba);
    shr_pl_res_image *base = stage_image(s, 160, 40, chart), *ramp = stage_image(s, 16, 8, rgba), *img[4] = {NULL};
    static const uint32_t flags[4] = {SHR_SCALE_DRIVER, 0, SHR_SCALE_BOX, SHR_SCALE_DRIVER};
    for (int i = 0; i < 4 && s->st == SHR_OK; i++) {
        shr_status st = shr_pl_res_image_view(i == 3 ? ramp : base, scaled_src[i], scaled_w[i], scaled_h[i], flags[i],
                                              &img[i]);
        img[i] = kept(s, st, img[i], "view");
    }
    if (s->st == SHR_OK) scaled_layer(s, img);
    return s->st;
}

/* A magenta ramp over 32 x 8 pixels: what the update writes into the chart at (40, 8). */
static void chart_patch(uint8_t *rgba, size_t stride) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 32; x++)
            memcpy(rgba + (size_t)y * stride + (size_t)x * 4, (uint8_t[4]){255, 0, 255, (uint8_t)(x * 8)}, 4);
}

/* The views the driver scales show it; the box view, a copy, does not. */
static shr_status image_scaled_update(stage *s) {
    uint8_t patch[32 * 8 * 4];
    chart_patch(patch, 32 * 4);
    shr_pl_res_image *base = s->nimages ? s->images[0] : NULL;
    if (base) stage_ok(s, shr_pl_res_image_update(base, (shr_rect){40, 8, 72, 16}, patch, 32 * 4), "image_update");
    return s->st;
}

static shr_status image_scaled_update_ref(stage *s) {
    uint8_t chart[160 * 40 * 4], box[160 * 40 * 4];
    alpha_chart(chart), alpha_chart(box);
    chart_patch(chart + (8 * 160 + 40) * 4, 160 * 4);
    return scaled_copies(s, chart, box);
}

static shr_status alpha_zero(stage *s) {
    checkerboard(s, 64, 64);
    uint8_t rgba[32 * 32 * 4];
    for (int i = 0; i < 32 * 32; i++) memcpy(rgba + i * 4, (uint8_t[4]){(uint8_t)i, 255, 0, 0}, 4);
    shr_pl_res_image *img = stage_image(s, 32, 32, rgba);
    shr_lyr *l = stage_layer(s, 0, (shr_rect){-8, 8, 56, 72});
    shr_lyr_cmd_begin(l);
    stage_ok(s, shr_lyr_cmd_image(l, img, (shr_rect){0, 0, 32, 32}, (shr_point){0, 0}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_image(l, img, (shr_rect){0, 0, 32, 32}, (shr_point){20, 20}), "cmd_image");
    stage_ok(s, shr_lyr_cmd_commit(l), "commit");
    return s->st;
}

static shr_status alpha_zero_ref(stage *s) {
    checkerboard(s, 64, 64);
    return s->st;
}

/* Every level of each channel and grey, as fills or as opaque images. */
static shr_status ramp(stage *s, bool images) {
    shr_lyr *l = stage_layer(s, 0, (shr_rect){0, 0, 256, 64});
    shr_lyr_cmd_begin(l);
    static uint8_t rgba[256 * 4 * 4];
    for (int c = 0; c < 4; c++)
        for (int v = 0; v < 256; v++) {
            uint8_t px[4] = {(uint8_t)(c == 0 || c == 3 ? v : 0), (uint8_t)(c == 1 || c == 3 ? v : 0),
                             (uint8_t)(c == 2 || c == 3 ? v : 0), 255};
            memcpy(rgba + (c * 256 + v) * 4, px, 4);
            if (!images) stage_fill(s, l, (shr_rect){v, c * 16, v + 1, c * 16 + 16}, SHR_RGB(px[0], px[1], px[2]));
        }
    if (images) {
        shr_pl_res_image *img = stage_image(s, 256, 4, rgba);
        for (int32_t c = 0; c < 4; c++)
            for (int32_t y = 0; y < 16; y++)
                stage_ok(s, shr_lyr_cmd_image(l, img, (shr_rect){0, c, 256, c + 1}, (shr_point){0, c * 16 + y}), "cmd_image");
    }
    stage_ok(s, shr_lyr_cmd_commit(l), "commit");
    return s->st;
}

static shr_status ramp_fill(stage *s) { return ramp(s, false); }
static shr_status ramp_image(stage *s) { return ramp(s, true); }

/* ===== Rotation: a composed, rotated screen vs the same content pre-rotated on an unrotated screen ===== */

#define RW 96
#define RH 64
#define RIW 24
#define RIH 16

static shr_point rot_pt(shr_rotation r, int32_t x, int32_t y, int32_t w, int32_t h) {
    shr_point p = {x, y};
    shr_rotation_map_point(r, w, h, (shr_point){x, y}, false, &p);
    return p;
}

static shr_rect rot_rect(shr_rotation r, shr_rect a) {
    shr_point p = rot_pt(r, a.x0, a.y0, RW, RH), q = rot_pt(r, a.x1 - 1, a.y1 - 1, RW, RH);
    return (shr_rect){p.x < q.x ? p.x : q.x, p.y < q.y ? p.y : q.y, (p.x > q.x ? p.x : q.x) + 1, (p.y > q.y ? p.y : q.y) + 1};
}

static shr_status rotated(stage *s, shr_rotation r) {
    static const shr_rect fills[] = {{0, 0, RW, 8}, {4, 12, 40, 30}, {30, 20, 70, 60}, {RW - 6, 0, RW, RH}, {10, 50, 12, 52}};
    static const shr_color colors[] = {BAR, PINK, GREEN, CYAN, FG};
    shr_lyr *l = stage_layer(s, 0, (shr_rect){0, 0, RW, RH});
    if (r && l) stage_ok(s, shr_lyr_set_rect(l, rot_rect(r, (shr_rect){0, 0, RW, RH})), "set_rect");
    shr_lyr_cmd_begin(l);
    for (size_t i = 0; i < N(fills); i++) stage_fill(s, l, r ? rot_rect(r, fills[i]) : fills[i], colors[i]);
    stage_ok(s, shr_lyr_cmd_commit(l), "commit");
    uint8_t src[RIW * RIH * 4], dst[RIW * RIH * 4];
    gradient(src, RIW, RIH, true);
    for (int32_t i = 0; i < RIW * RIH; i++) src[i * 4 + 3] = (uint8_t)(i % 5 == 0 ? 0 : i % 5 == 1 ? 128 : 255);
    bool quarter = r == SHR_ROTATE_90_CW || r == SHR_ROTATE_90_CCW;
    int32_t iw = quarter ? RIH : RIW, ih = quarter ? RIW : RIH;
    for (int32_t y = 0; y < RIH; y++)
        for (int32_t x = 0; x < RIW; x++) {
            shr_point p = rot_pt(r, x, y, RIW, RIH);
            memcpy(dst + ((size_t)p.y * (size_t)iw + (size_t)p.x) * 4, src + ((size_t)y * RIW + (size_t)x) * 4, 4);
        }
    shr_pl_res_image *img = stage_image(s, iw, ih, dst);
    static const shr_point at[] = {{20, 24}, {60, 10}, {-8, 52}, {RW - 12, RH - 6}};
    for (size_t i = 0; i < N(at); i++) {
        shr_rect a = {at[i].x, at[i].y, at[i].x + RIW, at[i].y + RIH};
        shr_lyr *p = stage_layer(s, 1, r ? rot_rect(r, a) : a);
        shr_lyr_cmd_begin(p);
        stage_ok(s, shr_lyr_cmd_image(p, img, (shr_rect){0, 0, iw, ih}, (shr_point){0, 0}), "cmd_image");
        stage_ok(s, shr_lyr_cmd_commit(p), "commit");
    }
    return s->st;
}

static shr_status rotate_composed(stage *s) { return rotated(s, SHR_ROTATE_NONE); }
static shr_status rotate_90cw_ref(stage *s) { return rotated(s, SHR_ROTATE_90_CW); }
static shr_status rotate_180_ref(stage *s) { return rotated(s, SHR_ROTATE_180); }
static shr_status rotate_90ccw_ref(stage *s) { return rotated(s, SHR_ROTATE_90_CCW); }

/* ===== Sprite-like glyph grids (cell-edge alignment): every glyph 2x2 times on a checkerboard, then a gap ===== */

static const struct {
    uint32_t first, last;
} sprite_ranges[] = {
    {0x2500, 0x257F}, {0x2580, 0x259F}, {0x2800, 0x28FF}, {0x1FB00, 0x1FBFF}, {0xE0A0, 0xE0D7}, {0xF000, 0xF0FF},
};

static size_t utf8_put(char *s, uint32_t cp) {
    if (cp < 0x80) return s[0] = (char)cp, 1;
    if (cp < 0x800) return s[0] = (char)(0xC0 | cp >> 6), s[1] = (char)(0x80 | (cp & 0x3F)), 2;
    if (cp < 0x10000)
        return s[0] = (char)(0xE0 | cp >> 12), s[1] = (char)(0x80 | (cp >> 6 & 0x3F)), s[2] = (char)(0x80 | (cp & 0x3F)), 3;
    s[0] = (char)(0xF0 | cp >> 18), s[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    s[2] = (char)(0x80 | (cp >> 6 & 0x3F)), s[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static shr_status sprites(stage *s, size_t range) {
    uint32_t first = sprite_ranges[range].first, n = sprite_ranges[range].last - first + 1;
    int32_t rows = (int32_t)(n + 15) / 16;
    shr_lyr *l = stage_grid(s, 1, cells(0, 0, 48, rows * 3), NULL);
    for (uint32_t i = 0; i < n; i++) {
        char u[5] = {0};
        utf8_put(u, first + i);
        int32_t r0 = (int32_t)(i / 16) * 3, c0 = (int32_t)(i % 16) * 3;
        for (int32_t k = 0; k < 4; k++)
            stage_cell(s, l, r0 + k / 2, c0 + k % 2, u, 1, STYLE(FG, (k + k / 2) % 2 ? SEL : DARK, 0));
    }
    return s->st;
}

static shr_status sprite_box(stage *s) { return sprites(s, 0); }
static shr_status sprite_blocks(stage *s) { return sprites(s, 1); }
static shr_status sprite_braille(stage *s) { return sprites(s, 2); }
static shr_status sprite_legacy(stage *s) { return sprites(s, 3); }
static shr_status sprite_powerline(stage *s) { return sprites(s, 4); }
static shr_status sprite_nerd(stage *s) { return sprites(s, 5); }

/* ===== Fonts missing, loading, loaded ===== */

static const char *font_sample[] = {
    "latin abc éßŁ ΑΩ Жя",
    "한글 漢字 あア",
    "\U0001F600\U0001F469‍\U0001F4BB \U0001F1F0\U0001F1F7 ❤️",
    "─│┼█▒ ✓→∞  ",
};

static shr_status font_page(stage *s) {
    shr_lyr *l = stage_grid(s, 1, cells(1, 1, 30, (int32_t)N(font_sample)), NULL);
    for (size_t i = 0; i < N(font_sample); i++) stage_text(s, l, (int32_t)i, 0, font_sample[i], plain, 0);
    return s->st;
}

static shr_status release_reads(stage *s) { return stage_release_reads(s); }

/* ===== Unicode data driven pages ===== */

#define PAGE_COLS 64
#define PAGE_ROWS 40

typedef struct seq {
    char utf8[64];
    size_t len;
} seq;

static seq *emoji;
static size_t emoji_count;

/* Fully-qualified sequences of emoji-test.txt, in file order. */
static int emoji_pages(void) {
    if (!emoji) {
        FILE *f = fopen(SHR_UCD_DIR "/emoji-test.txt", "r");
        char line[512];
        size_t cap = 0;
        while (f && fgets(line, sizeof(line), f)) {
            char *semi = strchr(line, ';');
            if (line[0] == '#' || !semi || strncmp(semi + 1, " fully-qualified", 16)) continue;
            if (emoji_count == cap) {
                seq *grown = realloc(emoji, (cap = cap ? cap * 2 : 1024) * sizeof(seq));
                if (!grown) break;
                emoji = grown;
            }
            seq *e = &emoji[emoji_count++];
            e->len = 0;
            for (char *p = line; p < semi;) {
                char *end;
                unsigned long cp = strtoul(p, &end, 16);
                if (end == p) break;
                e->len += utf8_put(e->utf8 + e->len, (uint32_t)cp);
                p = end;
            }
        }
        if (f) fclose(f);
    }
    const size_t per_page = PAGE_ROWS * (PAGE_COLS / 3);
    return (int)((emoji_count + per_page - 1) / per_page);
}

static shr_status emoji_page(stage *s) {
    const size_t per_row = PAGE_COLS / 3, per_page = PAGE_ROWS * per_row;
    shr_lyr *l = stage_grid(s, 1, cells(0, 0, PAGE_COLS, PAGE_ROWS), NULL);
    size_t first = (size_t)(s->page - 1) * per_page, odd = 0;
    for (size_t i = first; i < emoji_count && i < first + per_page && s->st == SHR_OK; i++) {
        const seq *e = &emoji[i];
        shr_text_cluster cl[2];
        shr_text_extent ext;
        shr_status st = shr_pl_lyr_tilemap_measure(e->utf8, e->len, PAGE_COLS, 0, cl, 2, &ext, NULL);
        if (!stage_ok(s, st, "measure emoji") || !stage_expect(s, ext.clusters == 1 ? SHR_OK : SHR_E_FORMAT, SHR_OK,
                                                                 "fully-qualified emoji is one cluster"))
            break;
        odd += cl[0].cells != 2 || !(cl[0].flags & SHR_CLUSTER_EMOJI);
        shr_error_info err;
        size_t k = i - first;
        stage_ok(s, shr_pl_lyr_tilemap_set_text(l, (int32_t)(k / per_row), (int32_t)(k % per_row) * 3, e->utf8, e->len,
                                                plain, NULL, 0, 0, &err),
                 "set_text emoji");
    }
    if (odd) printf("emoji-test page %d: %zu sequences not measured as one 2-cell emoji cluster\n", s->page, odd);
    return s->st;
}

/* GraphemeBreakTest.txt cases, controls replaced by U+2028 (set_text takes no controls): each case packed into a
 * row, its clusters on alternating backgrounds. */
static seq *cases;
static size_t case_count;

static int grapheme_pages(void) {
    if (!cases) {
        FILE *f = fopen(SHR_UCD_DIR "/GraphemeBreakTest.txt", "r");
        char line[1024];
        size_t cap = 0;
        while (f && fgets(line, sizeof(line), f)) {
            char *hash = strchr(line, '#');
            if (hash) *hash = 0;
            if (case_count == cap) {
                seq *grown = realloc(cases, (cap = cap ? cap * 2 : 1024) * sizeof(seq));
                if (!grown) break;
                cases = grown;
            }
            seq *c = &cases[case_count];
            c->len = 0;
            for (char *p = line; *p;) {
                char *end;
                unsigned long cp = strtoul(p, &end, 16);
                if (end == p) {
                    p++;
                    continue;
                }
                p = end;
                if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) cp = 0x2028;
                if (c->len + 4 < sizeof(c->utf8)) c->len += utf8_put(c->utf8 + c->len, (uint32_t)cp);
            }
            case_count += c->len > 0;
        }
        if (f) fclose(f);
    }
    /* Layout is computed in grapheme_page; a page holds PAGE_ROWS rows. */
    int32_t row = 0, col = 0;
    for (size_t i = 0; i < case_count; i++) {
        shr_text_extent ext;
        shr_pl_lyr_tilemap_measure(cases[i].utf8, cases[i].len, 0, 0, NULL, 0, &ext, NULL);
        if (col + ext.cols + 1 > PAGE_COLS) row++, col = 0;
        col += ext.cols + 1;
    }
    return row / PAGE_ROWS + 1;
}

static shr_status grapheme_page(stage *s) {
    shr_lyr *l = stage_grid(s, 1, cells(0, 0, PAGE_COLS, PAGE_ROWS), NULL);
    int32_t row = 0, col = 0;
    for (size_t i = 0; i < case_count && s->st == SHR_OK; i++) {
        shr_text_cluster cl[64];
        shr_text_extent ext;
        if (!stage_ok(s, shr_pl_lyr_tilemap_measure(cases[i].utf8, cases[i].len, 0, 0, cl, 64, &ext, NULL), "measure"))
            break;
        if (col + ext.cols + 1 > PAGE_COLS) row++, col = 0;
        int32_t page_row = row - (s->page - 1) * PAGE_ROWS;
        if (page_row >= 0 && page_row < PAGE_ROWS) {
            shr_style_run runs[64];
            for (size_t k = 0; k < ext.clusters; k++)
                runs[k] = (shr_style_run){cl[k].byte_offset, cl[k].byte_offset + cl[k].byte_length,
                                          STYLE(FG, k % 2 ? SEL : EDGE, 0)};
            shr_error_info err;
            stage_ok(s, shr_pl_lyr_tilemap_set_text(l, page_row, col, cases[i].utf8, cases[i].len, plain, runs,
                                                    ext.clusters, 0, &err),
                     "set_text grapheme case");
        }
        col += ext.cols + 1;
    }
    return s->st;
}

/* Markus Kuhn's UTF-8 decoder stress cases: measure and set_text must agree, reject malformed input with the
 * offset of the first bad byte and leave the grid unchanged (the #s stay); well-formed text replaces the #s. */
static const struct {
    const char *id, *bytes;
    shr_status want;
    size_t at; /* byte offset of the error */
} kuhn[] = {
    {"1 kosme", "\xCE\xBA\xE1\xBD\xB9\xCF\x83\xCE\xBC\xCE\xB5", SHR_OK, 0},
    {"2.1.2 U+0080", "\xC2\x80", SHR_E_CONTROL_CHAR, 0},
    {"2.1.3 U+0800", "\xE0\xA0\x80", SHR_OK, 0},
    {"2.1.4 U+10000", "\xF0\x90\x80\x80", SHR_OK, 0},
    {"2.1.5 5 bytes", "\xF8\x88\x80\x80\x80", SHR_E_INVALID_UTF8, 0},
    {"2.1.6 6 bytes", "\xFC\x84\x80\x80\x80\x80", SHR_E_INVALID_UTF8, 0},
    {"2.2.1 U+007F", "\x7F", SHR_E_CONTROL_CHAR, 0},
    {"2.2.2 U+07FF", "\xDF\xBF", SHR_OK, 0},
    {"2.2.3 U+FFFF", "\xEF\xBF\xBF", SHR_OK, 0},
    {"2.2.4 U+1FFFFF", "\xF7\xBF\xBF\xBF", SHR_E_INVALID_UTF8, 0},
    {"2.3.1 U+D7FF", "\xED\x9F\xBF", SHR_OK, 0},
    {"2.3.2 U+E000", "\xEE\x80\x80", SHR_OK, 0},
    {"2.3.3 U+FFFD", "\xEF\xBF\xBD", SHR_OK, 0},
    {"2.3.4 U+10FFFF", "\xF4\x8F\xBF\xBF", SHR_OK, 0},
    {"2.3.5 U+110000", "\xF4\x90\x80\x80", SHR_E_INVALID_UTF8, 0},
    {"3.1.1 cont 80", "\x80", SHR_E_INVALID_UTF8, 0},
    {"3.1.2 cont BF", "\xBF", SHR_E_INVALID_UTF8, 0},
    {"3.1.9 a + conts", "a\x80\xBF\x80\xBF", SHR_E_INVALID_UTF8, 1},
    {"3.2.1 lone C0", "\xC0 ", SHR_E_INVALID_UTF8, 0},
    {"3.2.2 lone E0", "ab\xE0 ", SHR_E_INVALID_UTF8, 2},
    {"3.2.3 lone F0", "\xF0 ", SHR_E_INVALID_UTF8, 0},
    {"3.2.4 lone F8", "\xF8 ", SHR_E_INVALID_UTF8, 0},
    {"3.2.5 lone FC", "\xFC ", SHR_E_INVALID_UTF8, 0},
    {"3.3.2 E0 80 cut", "\xE0\x80", SHR_E_INVALID_UTF8, 0},
    {"3.3.3 F0 80 80 cut", "\xF0\x80\x80", SHR_E_INVALID_UTF8, 0},
    {"3.3.7 DF cut", "x\xDF", SHR_E_INVALID_UTF8, 1},
    {"3.3.8 EF BF cut", "\xEF\xBF", SHR_E_INVALID_UTF8, 0},
    {"3.3.9 F7 BF BF cut", "\xF7\xBF\xBF", SHR_E_INVALID_UTF8, 0},
    {"3.4 concatenated", "\xC0\xE0\x80\xF0\x80\x80\xF8\x80\x80\x80\xDF\xEF\xBF", SHR_E_INVALID_UTF8, 0},
    {"3.5.1 FE", "\xFE", SHR_E_INVALID_UTF8, 0},
    {"3.5.2 FF", "\xFF", SHR_E_INVALID_UTF8, 0},
    {"3.5.3 FE FE FF FF", "\xFE\xFE\xFF\xFF", SHR_E_INVALID_UTF8, 0},
    {"4.1.1 overlong /2", "\xC0\xAF", SHR_E_INVALID_UTF8, 0},
    {"4.1.2 overlong /3", "\xE0\x80\xAF", SHR_E_INVALID_UTF8, 0},
    {"4.1.3 overlong /4", "\xF0\x80\x80\xAF", SHR_E_INVALID_UTF8, 0},
    {"4.2.1 max overl 2", "\xC1\xBF", SHR_E_INVALID_UTF8, 0},
    {"4.2.2 max overl 3", "\xE0\x9F\xBF", SHR_E_INVALID_UTF8, 0},
    {"4.2.3 max overl 4", "\xF0\x8F\xBF\xBF", SHR_E_INVALID_UTF8, 0},
    {"4.3.1 overlong NUL", "\xC0\x80", SHR_E_INVALID_UTF8, 0},
    {"4.3.2 overlong NUL3", "\xE0\x80\x80", SHR_E_INVALID_UTF8, 0},
    {"5.1.1 U+D800", "\xED\xA0\x80", SHR_E_INVALID_UTF8, 0},
    {"5.1.5 U+DC00", "ok\xED\xB0\x80", SHR_E_INVALID_UTF8, 2},
    {"5.1.7 U+DFFF", "\xED\xBF\xBF", SHR_E_INVALID_UTF8, 0},
    {"5.2.1 D800 DC00", "\xED\xA0\x80\xED\xB0\x80", SHR_E_INVALID_UTF8, 0},
    {"5.3.1 U+FFFE", "\xEF\xBF\xBE", SHR_OK, 0},
    {"5.3.3 U+FDD0", "\xEF\xB7\x90", SHR_OK, 0},
    {"5.3.4 U+1FFFE", "\xF0\x9F\xBF\xBE", SHR_OK, 0},
    {"mark after bad", "e\xCC", SHR_E_INVALID_UTF8, 1},
    {"NUL inside", "a\0b", SHR_E_CONTROL_CHAR, 1},
};

static shr_status utf8_stress(stage *s) {
    shr_lyr *l = stage_grid(s, 1, cells(0, 0, 56, (int32_t)N(kuhn)), NULL);
    if (!l) return s->st;
    for (size_t i = 0; i < N(kuhn); i++) {
        int32_t r = (int32_t)i;
        size_t len = !strcmp(kuhn[i].id, "NUL inside") ? 3 : strlen(kuhn[i].bytes);
        shr_text_cluster cl[16];
        shr_text_extent ext;
        shr_error_info me = {0}, te = {0};
        shr_status m = shr_pl_lyr_tilemap_measure(kuhn[i].bytes, len, 12, 0, cl, 16, &ext, &me);
        stage_text(s, l, r, 0, "############", STYLE(RED, 0, 0), 0);
        shr_status t = shr_pl_lyr_tilemap_set_text(l, r, 0, kuhn[i].bytes, len, plain, NULL, 0, 0, &te);
        char what[128];
        snprintf(what, sizeof(what), "Kuhn %s set_text", kuhn[i].id);
        stage_expect(s, t, kuhn[i].want, what);
        snprintf(what, sizeof(what), "Kuhn %s measure", kuhn[i].id);
        stage_expect(s, m, t, what);
        if (t != SHR_OK) {
            snprintf(what, sizeof(what), "Kuhn %s error offset %zu/%zu", kuhn[i].id, te.byte_offset, me.byte_offset);
            stage_expect(s, te.byte_offset == kuhn[i].at && me.byte_offset == kuhn[i].at ? SHR_OK : SHR_E_STATE, SHR_OK,
                         what);
        } else if (stage_ok(s, shr_pl_lyr_tilemap_clear(l, r, 0, 1, 12, plain), "clear")) {
            stage_text(s, l, r, 0, kuhn[i].bytes, plain, 0);
        }
        snprintf(what, sizeof(what), "%-19s%-17s@%zu", kuhn[i].id, shr_status_name(t), t ? te.byte_offset : 0);
        stage_text(s, l, r, 13, what, STYLE(t == SHR_OK ? GREEN : ORANGE, 0, 0), 0);
        snprintf(what, sizeof(what), "Kuhn %s set_cell", kuhn[i].id);
        stage_expect(s, shr_pl_lyr_tilemap_set_cell(l, r, 54, kuhn[i].bytes, len, 2, STYLE(FG, SEL, 0)), t,
                     what);
    }
    return s->st;
}

#define OTHER_FORMAT (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? SHR_FORMAT_RGBX8888 : SHR_FORMAT_RGB565)
#define TO_OTHER (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? "to-rgbx8888" : "to-rgb565")
#define SCREEN(cols, rows) ((cols) * CW), ((rows) * CH)

const scene scenes[] = {
    {.name = "fonts", .w = GW, .h = GH, .build = overview},
    {.name = "builtin", .w = GW, .h = GH, .flags = FONTS_NONE, .build = overview},
    {.name = "rot90cw", .w = GW, .h = GH, .rotation = SHR_ROTATE_90_CW, .build = overview},
    {.name = "rot180", .w = GW, .h = GH, .rotation = SHR_ROTATE_180, .build = overview},
    {.name = "rot90ccw", .w = GW, .h = GH, .rotation = SHR_ROTATE_90_CCW, .build = overview},
    {.name = TO_OTHER, .w = GW, .h = GH, .output_format = OTHER_FORMAT, .build = overview},
    {.name = "fonts-composed", .w = GW, .h = GH, .screen_flags = SHR_SCREEN_COMPOSITION, .build = overview},
    {.name = "fonts-unpreserved", .w = GW, .h = GH, .flags = PRESERVE_NONE, .build = overview},
    {.name = "wide-edge", SCREEN(WIDE_COLS + 2, 10), .build = wide_edge},
    {.name = "wide-resize", SCREEN(WIDE_COLS + 2, 10), .build = wide_edge, .update = wide_resize},
    {.name = "wide-resize-ref", SCREEN(WIDE_COLS + 2, 10), .build = wide_resize_ref},
    {.name = "combining", SCREEN(40, 10), .build = combining},
    {.name = "emoji-sequences", SCREEN(42, 12), .build = emoji_sequences},
    {.name = "styles", SCREEN(36, 18), .build = styles},
    {.name = "styles-blink-off", SCREEN(36, 18), .now_ns = BLINK_NS, .build = styles},
    {.name = "styles-rot90cw", SCREEN(36, 18), .rotation = SHR_ROTATE_90_CW, .build = styles},
    {.name = "styled-exempt", SCREEN(16, 3), .build = styled_exempt},
    {.name = "styled-exempt-ref", SCREEN(16, 3), .build = styled_exempt_ref},
    {.name = "tilemap-over-image", SCREEN(40, 8), .build = tilemap_over_image},
    {.name = "tm-background", SCREEN(26, 6), .build = tm_background},
    {.name = "tm-background-ref", SCREEN(26, 6), .build = tm_background_ref},
    {.name = "tm-update", SCREEN(28, 8), .build = tm_update_build, .update = tm_update},
    {.name = "tm-update-ref", SCREEN(28, 8), .build = tm_update_ref},
    {.name = "tm-update-rot180", SCREEN(28, 8), .rotation = SHR_ROTATE_180, .build = tm_update_build, .update = tm_update},
    {.name = "tm-update-rot180-ref", SCREEN(28, 8), .rotation = SHR_ROTATE_180, .build = tm_update_ref},
    {.name = "tm-restyle", SCREEN(18, 3), .build = tm_restyle_build, .update = tm_restyle},
    {.name = "tm-restyle-ref", SCREEN(18, 3), .build = tm_restyle_ref},
    {.name = "scroll", SCREEN(SC_COLS + 2, SC_ROWS + 2), .build = scroll_build, .update = scroll_update},
    {.name = "scroll-ref", SCREEN(SC_COLS + 2, SC_ROWS + 2), .build = scroll_ref},
    {.name = "scroll-rot90cw", SCREEN(SC_COLS + 2, SC_ROWS + 2), .rotation = SHR_ROTATE_90_CW, .build = scroll_build,
     .update = scroll_update},
    {.name = "scroll-rot90cw-ref", SCREEN(SC_COLS + 2, SC_ROWS + 2), .rotation = SHR_ROTATE_90_CW, .build = scroll_ref},
    {.name = "cleared", SCREEN(14, 6), .build = cleared},
    {.name = "cleared-ref", SCREEN(14, 6), .build = cleared_ref},
    {.name = "text-layout", SCREEN(18, 4), .build = text_layout},
    {.name = "text-cells", SCREEN(18, 4), .build = text_cells},
    {.name = "layers", .w = LW, .h = LH, .build = layers},
    {.name = "layers-rot90ccw", .w = LW, .h = LH, .rotation = SHR_ROTATE_90_CCW, .build = layers},
    {.name = "occluded", .w = 128, .h = 80, .build = occluded},
    {.name = "occluded-ref", .w = 128, .h = 80, .build = occluded_ref},
    {.name = "hidden", .w = 128, .h = 80, .build = hidden},
    {.name = "moved", .w = 128, .h = 80, .build = moved},
    {.name = "moved-ref", .w = 128, .h = 80, .build = moved_ref},
    {.name = "zorder", .w = 128, .h = 80, .build = zorder},
    {.name = "zorder-ref", .w = 128, .h = 80, .build = zorder_ref},
    {.name = "many-commands", .w = MW, .h = MH, .build = many, .update = many_update},
    {.name = "many-commands-ref", .w = MW, .h = MH, .build = many_ref},
    {.name = "many-commands-rot90cw", .w = MW, .h = MH, .rotation = SHR_ROTATE_90_CW, .build = many, .update = many_update},
    {.name = "many-commands-rot90cw-ref", .w = MW, .h = MH, .rotation = SHR_ROTATE_90_CW, .build = many_ref},
    {.name = "image-alpha", .w = AW, .h = AH, .build = image_alpha},
    {.name = "image-update", .w = AW, .h = AH, .build = image_alpha, .update = image_update},
    {.name = "image-update-ref", .w = AW, .h = AH, .build = image_update_ref},
    {.name = "image-scaled", .w = AW, .h = AH, .build = image_scaled, .gpu = SCALED_GPU},
    {.name = "image-scaled-view", .w = AW, .h = AH, .build = image_scaled_view, .gpu = SCALED_GPU},
    {.name = "image-scaled-update", .w = AW, .h = AH, .build = image_scaled_view, .update = image_scaled_update,
     .gpu = SCALED_GPU},
    {.name = "image-scaled-update-ref", .w = AW, .h = AH, .build = image_scaled_update_ref, .gpu = SCALED_GPU},
    {.name = "alpha-zero", .w = 64, .h = 64, .build = alpha_zero},
    {.name = "alpha-zero-ref", .w = 64, .h = 64, .build = alpha_zero_ref},
    {.name = "ramp-fill", .w = 256, .h = 64, .build = ramp_fill},
    {.name = "ramp-image", .w = 256, .h = 64, .build = ramp_image},
    {.name = "ramp-fill-to-other", .w = 256, .h = 64, .output_format = OTHER_FORMAT, .build = ramp_fill},
    {.name = "rotate-90cw", .w = RW, .h = RH, .rotation = SHR_ROTATE_90_CW, .build = rotate_composed},
    {.name = "rotate-90cw-ref", .w = RH, .h = RW, .build = rotate_90cw_ref},
    {.name = "rotate-180", .w = RW, .h = RH, .rotation = SHR_ROTATE_180, .build = rotate_composed},
    {.name = "rotate-180-ref", .w = RW, .h = RH, .build = rotate_180_ref},
    {.name = "rotate-90ccw", .w = RW, .h = RH, .rotation = SHR_ROTATE_90_CCW, .build = rotate_composed},
    {.name = "rotate-90ccw-ref", .w = RH, .h = RW, .build = rotate_90ccw_ref},
    {.name = "sprite-box", SCREEN(48, 24), .build = sprite_box},
    {.name = "sprite-blocks", SCREEN(48, 6), .build = sprite_blocks},
    {.name = "sprite-braille", SCREEN(48, 48), .build = sprite_braille},
    {.name = "sprite-legacy", SCREEN(48, 48), .build = sprite_legacy},
    {.name = "sprite-powerline", SCREEN(48, 12), .build = sprite_powerline},
    {.name = "sprite-nerd", SCREEN(48, 48), .build = sprite_nerd},
    {.name = "font-page", SCREEN(32, 6), .build = font_page},
    {.name = "fallback-latin", SCREEN(32, 6), .flags = FONTS_LATIN, .build = font_page},
    {.name = "fallback-none", SCREEN(32, 6), .flags = FONTS_NONE, .build = font_page},
    {.name = "provisional", SCREEN(32, 6), .flags = FONTS_ASYNC, .build = font_page},
    {.name = "async-loaded", SCREEN(32, 6), .flags = FONTS_ASYNC, .build = font_page, .update = release_reads},
    {.name = "emoji-test", SCREEN(PAGE_COLS, PAGE_ROWS), .build = emoji_page, .pages = emoji_pages},
    {.name = "graphemes", SCREEN(PAGE_COLS, PAGE_ROWS), .build = grapheme_page, .pages = grapheme_pages},
    {.name = "utf8-stress", SCREEN(56, (int32_t)N(kuhn)), .build = utf8_stress},
};
const size_t scene_count = N(scenes);

const reftest reftests[] = {
    {"fonts-composed", "fonts"},
    {"fonts-unpreserved", "fonts"},
    {"wide-resize", "wide-resize-ref"},
    {"tm-background", "tm-background-ref"},
    {"tm-update", "tm-update-ref"},
    {"tm-update-rot180", "tm-update-rot180-ref"},
    {"styled-exempt", "styled-exempt-ref"},
    {"tm-restyle", "tm-restyle-ref"},
    {"scroll", "scroll-ref"},
    {"scroll-rot90cw", "scroll-rot90cw-ref"},
    {"cleared", "cleared-ref"},
    {"text-layout", "text-cells"},
    {"occluded", "occluded-ref"},
    {"hidden", "occluded-ref"},
    {"moved", "moved-ref"},
    {"zorder", "zorder-ref"},
    {"many-commands", "many-commands-ref"},
    {"many-commands-rot90cw", "many-commands-rot90cw-ref"},
    {"image-update", "image-update-ref"},
    {"image-scaled-view", "image-scaled"},
    {"image-scaled-update", "image-scaled-update-ref"},
    {"alpha-zero", "alpha-zero-ref"},
    {"ramp-image", "ramp-fill"},
    {"rotate-90cw", "rotate-90cw-ref"},
    {"rotate-180", "rotate-180-ref"},
    {"rotate-90ccw", "rotate-90ccw-ref"},
    {"async-loaded", "font-page"},
    {"provisional", "fallback-none"},
};
const size_t reftest_count = N(reftests);
