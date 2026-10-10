/* The Tab5 example's load modes (examples/tab5, as in examples/desktop), shared with the replay benchmark so both
 * draw the same frames: the scene state, its contents, the frame pump and the context values that decide what the
 * compositor draws. A failing call is kept in `st` (and `what`) and the scene goes on; callers check it. */
#ifndef SHIROKO_TAB5_SCENES_H
#define SHIROKO_TAB5_SCENES_H

#include "text_scenes.h"

#include <stdbool.h>
#include <stdlib.h>

#define T5_N(a) (sizeof(a) / sizeof((a)[0]))
#define T5_FRAMES 60
#define T5_SCROLL_FRAMES 300 /* scroll loads */
#define T5_BLINK_NS 200000u  /* short: the blink scene flips at every frame the cap allows */
#define T5_CHURN_CELLS 1000
#define T5_SPRITES 16
#define T5_SPRITE 96
/* Preloaded before the first frame: the latin package, the first symbols and Nerd pages, the KS X 1001
 * punctuation, jamo and most frequent Hangul of cjk-ko (7 pages at 8x16 with 64x512 pages: 99.8 % of the Hangul in
 * the 2005 frequency survey of fontpack's order) and the emoji package's hot pages; for tabs also the Nerd pages its
 * editor screen draws from (pages 1-5 at 8x16 with 64x512 pages), which it first shows after the first frame. */
#define T5_CJK_HOT_PAGES 7
#define T5_EMOJI_HOT_PAGES 9
#define T5_TABS_NERD_PAGES 6
/* The record pass (rec.h): the clock held from T0 and advanced an odd number of blink phases per loop frame, so
 * that blinking cells change at every frame. */
#define T5_REC_T0_NS 1000000000ull
#define T5_REC_STEP_NS (167ull * T5_BLINK_NS)
#ifndef TAB5_STEPS
#define TAB5_STEPS 1
#endif

/* The scenes (page: a new screen every 20 frames; restyle4: four styles in turn; tabs: a shell and an editor, each a
 * layer of its own, shown in turn), then (from COST_CELL) the small ones of the app cost table: a cell, a row, the
 * screen rewritten in Latin text, a line scrolled in, a row's style flipped, one sprite moved (with churn-ko for
 * Hangul). */
enum { SCROLL, CHURN, RESTYLE, BLINK, IMAGES, CHURN_KO, CODE, CJK_MIX, SCROLL_API, SCROLL_STATUS, SCROLL_CURSOR,
       SCROLL_DOWN, SCROLL_BURST, SCROLL_IMAGES, PAGE, RESTYLE4, TABS, COST_CELL, COST_ROW, COST_SCREEN, COST_SCROLL,
       COST_RESTYLE, COST_SPRITE, LOADS };
#define T5_COST_LOADS ((1u << CHURN_KO) | ((1u << LOADS) - (1u << COST_CELL)))
static const char *const load_names[LOADS] = {"scroll",           "churn",          "restyle",          "blink",
                                              "images",           "churn-ko",       "code",             "cjk-mix",
                                              "scroll-api",       "scroll-api-status", "scroll-api-cursor",
                                              "scroll-api-down",  "scroll-api-burst", "scroll-api-images",
                                              "page",             "restyle4",       "tabs",
                                              "cost-cell",        "cost-row",       "cost-screen",      "cost-scroll",
                                              "cost-restyle",     "cost-sprite"};

typedef struct t5_sprite {
    shr_lyr *l;
    int32_t x, y, dx, dy;
} t5_sprite;

typedef struct t5_scene {
    shr_context *ctx;
    shr_pl_res_bitmap_font *font;
    shr_lyr *grid, *cursor, *alt;
    shr_text_line *lines; /* paint_row's underlines, one per column */
    shr_pl_res_image *img[4];
    t5_sprite sprites[T5_SPRITES];
    int load, frames, nsprites;
    int32_t width, height, rows, cols;
    uint64_t frozen, tick; /* frozen: the clock while it is held (0: now_ns) */
    uint64_t (*now_ns)(void *user);
    void (*sleep)(void *user, uint64_t at_ns); /* NULL: the held clock jumps to the deadline */
    void *user;
    uint32_t rng;
    ts_prose prose;
    long resource_failed;
    uint32_t shown; /* frames the output accepted */
    shr_status st;
    const char *what;
} t5_scene;

static void t5_try(t5_scene *s, shr_status st, const char *what) {
    if (s->st == SHR_OK && st != SHR_OK) s->st = st, s->what = what;
}

static uint64_t t5_clock(void *user) {
    t5_scene *s = user;
    return s->frozen ? s->frozen : s->now_ns(s->user);
}

static const struct {
    const char *text;
    uint32_t span;
} glyphs[] = {{"a", 1},  {"Z", 1},  {"#", 1},  {"0", 1},   {"~", 1},  {"─", 1},  {"e\xCC\x81", 1},
              {"|", 1},  {"가", 2}, {"한", 2}, {"글", 2}, {"😀", 2}, {"❤️", 2}, {"👍", 2}};
static const shr_color fgs[] = {SHR_RGB(0xF8, 0xF8, 0xF2), SHR_RGB(0xFF, 0x79, 0xC6), SHR_RGB(0x50, 0xFA, 0x7B),
                                SHR_RGB(0xF1, 0xFA, 0x8C), SHR_RGB(0x8B, 0xE9, 0xFD), SHR_RGB(0xBD, 0x93, 0xF9)};
static const shr_color bgs[] = {SHR_RGB(0x28, 0x2A, 0x36), SHR_RGB(0x44, 0x47, 0x5A), SHR_RGB(0x62, 0x72, 0xA4)};
#define T5_UNDERLINE (1u << 31) /* paint_row: a line under every cell, in its colour */
static const uint32_t line_styles[] = {0, SHR_STYLE_BOLD, 0, SHR_STYLE_ITALIC, T5_UNDERLINE, 0,
                                       SHR_STYLE_BOLD | SHR_STYLE_ITALIC};

static uint32_t mix(uint32_t x) {
    x ^= x >> 16, x *= 0x7FEB352Du, x ^= x >> 15, x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}

static uint32_t next_random(t5_scene *s) {
    s->rng ^= s->rng << 13, s->rng ^= s->rng >> 17, s->rng ^= s->rng << 5;
    return s->rng;
}

/* Appends `e` to the row's lines, joining it to the last one when it continues it alike: the cells' underlines
 * become runs, as a VT connection hands them over. */
static size_t t5_line_add(shr_text_line *l, size_t n, shr_text_line e) {
    shr_text_line *p = n ? &l[n - 1] : NULL;
    if (p && p->col + p->cols == e.col && p->kind == e.kind && p->shape == e.shape && p->flags == e.flags &&
        p->color == e.color)
        return p->cols = (uint16_t)(p->cols + e.cols), n;
    return l[n] = e, n + 1;
}

/* One cell as a VT engine hands it over, its underline into `lines` (NULL: none); returns its span. */
static int32_t paint(t5_scene *s, int32_t row, int32_t col, uint32_t h, uint32_t flags, shr_text_line *lines, size_t *n) {
    uint32_t g = h % T5_N(glyphs), span = glyphs[g].span;
    const char *t = glyphs[g].text;
    if (col + (int32_t)span > s->cols) t = " ", span = 1;
    shr_text_style st = {fgs[(h >> 8) % T5_N(fgs)], (h >> 16) % 4 ? 0 : bgs[(h >> 12) % T5_N(bgs)], flags};
    t5_try(s, shr_pl_lyr_tilemap_set_cell(s->grid, row, col, t, strlen(t), span, st), "set_cell");
    uint16_t blink = flags & SHR_STYLE_BLINK ? SHR_TEXT_LINE_BLINK : 0;
    shr_text_line under = {(uint16_t)col, (uint16_t)span, SHR_LINE_UNDER, SHR_LINE_SINGLE, blink, st.fg};
    if (lines) *n = t5_line_add(lines, *n, under);
    return (int32_t)span;
}

/* Text line `line` into `row`; with SHR_STYLE_BLINK every other cell blinks. */
static void paint_row(t5_scene *s, int32_t row, uint32_t line, uint32_t flags) {
    size_t n = 0;
    bool under = (flags & T5_UNDERLINE) && s->lines;
    for (int32_t c = 0; c < s->cols;) {
        uint32_t f = ((row + c) & 1 ? flags & ~(uint32_t)SHR_STYLE_BLINK : flags) & ~T5_UNDERLINE;
        c += paint(s, row, c, mix(line * 0x9E3779B1u + (uint32_t)c), f, under ? s->lines : NULL, &n);
    }
    t5_try(s, shr_pl_lyr_tilemap_set_lines(s->grid, row, s->lines, n, NULL), "set_lines");
}

/* Latin text: words of printable ASCII, a colour per word. */
static void latin_row(t5_scene *s, int32_t row, uint32_t line, uint32_t flags) {
    for (int32_t c = 0; c < s->cols; c++) {
        uint32_t h = mix(line * 0x9E3779B1u + (uint32_t)c / 6), ch = (line * 31 + (uint32_t)c * 7) % 94 + 33;
        char t = c % 6 == 5 ? ' ' : (char)ch;
        shr_text_style st = {fgs[h % T5_N(fgs)], 0, flags};
        t5_try(s, shr_pl_lyr_tilemap_set_cell(s->grid, row, c, &t, 1, 1, st), "set_cell");
    }
}

static void set_text(void *user, int32_t row, int32_t col, const char *t, uint32_t span, shr_text_style st) {
    t5_scene *s = user;
    t5_try(s, shr_pl_lyr_tilemap_set_cell(s->grid, row, col, t, strlen(t), span, st), "set_cell");
}

static void build_sprites(t5_scene *s) {
    uint8_t *px = malloc(T5_SPRITE * T5_SPRITE * 4);
    if (!px) {
        t5_try(s, SHR_E_NO_MEMORY, "sprite pixels");
        return;
    }
    for (int k = 0; k < 4 && k < s->nsprites; k++) {
        for (int y = 0; y < T5_SPRITE; y++)
            for (int x = 0; x < T5_SPRITE; x++) {
                int dx = 2 * x - T5_SPRITE + 1, dy = 2 * y - T5_SPRITE + 1, d2 = dx * dx + dy * dy, r2 = T5_SPRITE * T5_SPRITE;
                uint8_t *p = px + (y * T5_SPRITE + x) * 4;
                p[0] = (uint8_t)(k & 1 ? 255 - x * 2 : x * 2), p[1] = (uint8_t)(y * 2), p[2] = (uint8_t)(k & 2 ? 255 : 96);
                p[3] = (uint8_t)(d2 >= r2 ? 0 : 255 - 255 * d2 / r2);
            }
        t5_try(s, shr_pl_res_image_create(s->ctx, T5_SPRITE, T5_SPRITE, px, T5_SPRITE * 4, &s->img[k]), "image_create");
    }
    free(px);
    for (int i = 0; i < s->nsprites && s->st == SHR_OK; i++) {
        t5_sprite *p = &s->sprites[i];
        p->x = (int32_t)(next_random(s) % (uint32_t)(s->width - T5_SPRITE));
        p->y = (int32_t)(next_random(s) % (uint32_t)(s->height - T5_SPRITE));
        p->dx = (int32_t)(next_random(s) % 7) - 3, p->dy = (int32_t)(next_random(s) % 7) - 3;
        p->dx += !p->dx, p->dy += !p->dy;
        t5_try(s, shr_lyr_create(s->ctx, 1, (shr_rect){p->x, p->y, p->x + T5_SPRITE, p->y + T5_SPRITE}, &p->l), "layer");
        t5_try(s, shr_lyr_cmd_begin(p->l), "cmd_begin");
        t5_try(s, shr_lyr_cmd_image(p->l, s->img[i % 4], (shr_rect){0, 0, T5_SPRITE, T5_SPRITE}, (shr_point){0, 0}),
               "cmd_image");
        t5_try(s, shr_lyr_cmd_commit(p->l), "commit");
    }
}

static void move_sprites(t5_scene *s) {
    for (int i = 0; i < s->nsprites; i++) {
        t5_sprite *p = &s->sprites[i];
        if (p->x + p->dx < 0 || p->x + p->dx > s->width - T5_SPRITE) p->dx = -p->dx;
        if (p->y + p->dy < 0 || p->y + p->dy > s->height - T5_SPRITE) p->dy = -p->dy;
        p->x += p->dx, p->y += p->dy;
        t5_try(s, shr_lyr_set_rect(p->l, (shr_rect){p->x, p->y, p->x + T5_SPRITE, p->y + T5_SPRITE}), "set_rect");
    }
}

/* A shell session: prompts and the short lines of a long listing, git status and a build, the rows below empty. */
static void shell_screen(t5_scene *s) {
    static const char *const cmds[] = {"ls -l cores/compositor", "git status --short", "make test"};
    shr_text_style user = {fgs[2], 0, SHR_STYLE_BOLD}, path = {fgs[4], 0, SHR_STYLE_BOLD};
    shr_text_style out = {fgs[0], 0, 0};
    char t[96];
    int32_t r = 0;
    for (int k = 0; r < s->rows - 8; k++) {
        ts_line w = {set_text, s, r++, 0};
        ts_put(&w, "user@tab5", s->cols, user);
        ts_put(&w, ":", s->cols, out);
        ts_put(&w, "~/shiroko", s->cols, path);
        snprintf(t, sizeof(t), "$ %s", cmds[k % 3]);
        ts_put(&w, t, s->cols, out);
        ts_pad(&w, s->cols, out);
        for (int i = 0; i < 6 + k % 5 && r < s->rows - 8; i++, r++) {
            uint32_t h = mix((uint32_t)(k * 31 + i));
            const char *f = ts_files[h % TS_N(ts_files)][1];
            w = (ts_line){set_text, s, r, 0};
            if (k % 3 == 0) {
                snprintf(t, sizeof(t), "-rw-r--r--  1 user staff %6u Oct  6 01:%02u %s", (unsigned)(h >> 8) % 60000,
                         (unsigned)(h >> 4) % 60, f);
                ts_put(&w, t, s->cols, out);
            } else if (k % 3 == 1) {
                ts_put(&w, h & 1 ? " M " : "?? ", s->cols, (shr_text_style){fgs[h & 1 ? 1 : 3], 0, 0});
                snprintf(t, sizeof(t), "%s/%s", ts_dirs[(h >> 8) % TS_N(ts_dirs)], f);
                ts_put(&w, t, s->cols, out);
            } else {
                snprintf(t, sizeof(t), "[%3u%%] Building C object %s/CMakeFiles/shiroko.dir/%s.o",
                         (unsigned)(i * 100 / 9), ts_dirs[(h >> 8) % TS_N(ts_dirs)], f);
                ts_put(&w, t, s->cols, out);
            }
            ts_pad(&w, s->cols, out);
        }
    }
    ts_line w = {set_text, s, r++, 0};
    ts_put(&w, "user@tab5", s->cols, user);
    ts_put(&w, ":", s->cols, out);
    ts_put(&w, "~/shiroko", s->cols, path);
    ts_put(&w, "$ ", s->cols, out);
    ts_pad(&w, s->cols, out);
    for (; r < s->rows; r++) {
        w = (ts_line){set_text, s, r, 0};
        ts_pad(&w, s->cols, out);
    }
}

/* The grid scrolled as a VT engine scrolls it: n rows up (5 for burst, down one for down) in rows [0, bottom), the
 * status line below them (status) changing a few cells, the uncovered rows painted; row r then shows line r + n * tick,
 * which the scroll load writes cell by cell. The cursor follows the last row. */
static void scroll_api_step(t5_scene *s) {
    int32_t n = s->load == SCROLL_BURST ? 5 : s->load == SCROLL_DOWN ? -1 : 1, t = (int32_t)s->tick;
    int32_t bottom = s->rows - (s->load == SCROLL_STATUS);
    for (int32_t step = (t - 1) * TAB5_STEPS + 1; step <= t * TAB5_STEPS; step++) {
        t5_try(s, shr_pl_lyr_tilemap_scroll(s->grid, 0, bottom, n, (shr_text_style){0}), "scroll");
        for (int32_t r = n > 0 ? bottom - n : 0; r < (n > 0 ? bottom : -n); r++) {
            uint32_t line = (uint32_t)(r + n * step);
            paint_row(s, r, line, line_styles[line % T5_N(line_styles)]);
        }
    }
    char status[16];
    const shr_text_style st = {fgs[0], bgs[1], 0};
    int len = s->load == SCROLL_STATUS ? snprintf(status, sizeof(status), "frame %d", (int)t) : 0;
    for (int i = 0; i < len && i < s->cols; i++)
        t5_try(s, shr_pl_lyr_tilemap_set_cell(s->grid, s->rows - 1, i, &status[i], 1, 1, st), "set_cell");
    if (s->cursor)
        t5_try(s,
               shr_lyr_set_rect(s->cursor, (shr_rect){t % s->cols * SHR_CELL_WIDTH, (bottom - 1) * SHR_CELL_HEIGHT,
                                                      t % s->cols * SHR_CELL_WIDTH + SHR_CELL_WIDTH,
                                                      bottom * SHR_CELL_HEIGHT}),
               "set_rect");
    if (s->load == SCROLL_IMAGES) move_sprites(s);
}

/* Changes the screen for the next frame; false when the frame comes from the clock alone. */
static bool load_step(t5_scene *s) {
    s->tick++;
    switch (s->load) {
    case SCROLL:
        for (int32_t r = 0; r < s->rows; r++) {
            uint32_t line = (uint32_t)(r + (int32_t)s->tick);
            paint_row(s, r, line, line_styles[line % T5_N(line_styles)]);
        }
        break;
    case CHURN:
        for (int i = 0; i < T5_CHURN_CELLS; i++) {
            uint32_t h = next_random(s);
            paint(s, (int32_t)(h % (uint32_t)s->rows), (int32_t)((h >> 8) % (uint32_t)s->cols), mix(h), 0, NULL, NULL);
        }
        break;
    case RESTYLE:
        for (int32_t r = 0; r < s->rows; r++)
            paint_row(s, r, (uint32_t)r, s->tick & 1 ? SHR_STYLE_BOLD | SHR_STYLE_ITALIC | T5_UNDERLINE : 0);
        break;
    case BLINK: return false;
    case CHURN_KO:
    case CJK_MIX: ts_prose_churn(&s->prose, s->rows, s->cols, T5_CHURN_CELLS, set_text, s); break;
    case CODE: ts_code_screen((uint32_t)s->tick, s->rows, s->cols, set_text, s); break;
    case IMAGES:
    case COST_SPRITE: move_sprites(s); break;
    case PAGE:
        if (s->tick % 20) return false;
        for (int32_t r = 0; r < s->rows; r++) paint_row(s, r, (uint32_t)(s->tick * 64 + (uint64_t)r), 0);
        break;
    case RESTYLE4: {
        static const uint32_t styles[4] = {0, SHR_STYLE_BOLD | SHR_STYLE_ITALIC | T5_UNDERLINE, SHR_STYLE_BOLD,
                                           SHR_STYLE_ITALIC};
        for (int32_t r = 0; r < s->rows; r++) paint_row(s, r, (uint32_t)r, styles[s->tick % 4]);
        break;
    }
    case COST_CELL: {
        uint32_t h = next_random(s), line = mix(h);
        char t = (char)(line % 94 + 33);
        t5_try(s, shr_pl_lyr_tilemap_set_cell(s->grid, (int32_t)(h % (uint32_t)s->rows), (int32_t)((h >> 8) % (uint32_t)s->cols),
                                              &t, 1, 1, (shr_text_style){fgs[line % T5_N(fgs)], 0, 0}),
               "set_cell");
        break;
    }
    case COST_ROW: latin_row(s, (int32_t)(s->tick % (uint64_t)s->rows), (uint32_t)(s->rows + (int32_t)s->tick), 0); break;
    case COST_SCREEN:
        for (int32_t r = 0; r < s->rows; r++) latin_row(s, r, (uint32_t)(r + (int32_t)s->tick), 0);
        break;
    case COST_SCROLL:
        t5_try(s, shr_pl_lyr_tilemap_scroll(s->grid, 0, s->rows, 1, (shr_text_style){0}), "scroll");
        latin_row(s, s->rows - 1, (uint32_t)(s->rows - 1 + (int32_t)s->tick), 0);
        break;
    case COST_RESTYLE: latin_row(s, s->rows / 2, (uint32_t)(s->rows / 2), s->tick & 1 ? SHR_STYLE_BOLD | SHR_STYLE_ITALIC : 0); break;
    case TABS: t5_try(s, shr_lyr_set_visible(s->alt, s->tick & 1), "set_visible"); break;
    default: scroll_api_step(s);
    }
    return true;
}

/* A new scene of `load` on a width x height (landscape) screen; the host then creates the context (t5_context_desc). */
static void t5_scene_reset(t5_scene *s, int load, int32_t width, int32_t height) {
    s->load = load, s->frozen = s->tick = 0, s->rng = 0x9E3779B9u, s->resource_failed = 0;
    s->st = SHR_OK, s->what = NULL;
    s->frames = load == SCROLL || (load >= SCROLL_API && load <= SCROLL_IMAGES) ? T5_SCROLL_FRAMES : T5_FRAMES;
    s->nsprites = load == COST_SPRITE ? 1 : load == IMAGES || load == SCROLL_IMAGES ? T5_SPRITES : 0;
    s->prose = (ts_prose){.rng = 0x2545F491u, .mix = load == CJK_MIX, .left = -1};
    s->width = width, s->height = height;
    s->rows = height / SHR_CELL_HEIGHT, s->cols = width / SHR_CELL_WIDTH;
}

/* The context values the scenes run with; the host adds the driver, output, allocator, trace and frame-rate cap. */
static void t5_context_desc(t5_scene *s, shr_context_desc *cd) {
    shr_context_desc_init(cd);
    cd->now_ns = t5_clock, cd->user = s;
    cd->blink = (shr_blink_profile){T5_BLINK_NS, 0, true};
    cd->max_commands = 1u << 20, cd->page_cache_bytes = 16u << 20, cd->image_bytes = 4u << 20;
    cd->io_retry_ns = cd->io_timeout_ns = 0;
}

/* The screen, turned a quarter clockwise onto the portrait output; the host adds the bands. */
static void t5_screen_desc(const t5_scene *s, shr_screen_desc *sd) {
    shr_screen_desc_init(sd);
    sd->width = s->width, sd->height = s->height, sd->rotation = SHR_ROTATE_90_CW;
    sd->clear = SHR_RGB(0x1E, 0x1F, 0x29);
}

static void t5_preload(t5_scene *s) {
    t5_try(s, shr_pl_res_bitmap_font_preload(s->font, "shiroko-emoji.shrf", T5_EMOJI_HOT_PAGES), "preload");
    t5_try(s, shr_pl_res_bitmap_font_preload(s->font, "shiroko-latin.shrf", UINT32_MAX), "preload");
    t5_try(s, shr_pl_res_bitmap_font_preload(s->font, "shiroko-cjk-ko.shrf", T5_CJK_HOT_PAGES), "preload");
    t5_try(s, shr_pl_res_bitmap_font_preload(s->font, "shiroko-symbols.shrf", 1), "preload");
    t5_try(s, shr_pl_res_bitmap_font_preload(s->font, "shiroko-nerd.shrf", s->load == TABS ? T5_TABS_NERD_PAGES : 1), "preload");
}

/* The first screen: the grid, its text, the sprites and the cursor. */
static void t5_scene_fill(t5_scene *s) {
    shr_color bg = SHR_RGB(0x1E, 0x1F, 0x29);
    if (!(s->lines = malloc((size_t)s->cols * sizeof(*s->lines)))) t5_try(s, SHR_E_NO_MEMORY, "lines");
    t5_try(s, shr_lyr_create(s->ctx, 0, (shr_rect){0, 0, s->width, s->height}, &s->grid), "layer");
    t5_try(s, shr_pl_lyr_tilemap_resize(s->grid, s->font, s->rows, s->cols, &bg), "tilemap_resize");
    if (s->load == CHURN_KO || s->load == CJK_MIX)
        ts_prose_screen(&s->prose, s->rows, s->cols, set_text, s);
    else if (s->load == CODE)
        ts_code_screen(0, s->rows, s->cols, set_text, s);
    else if (s->load == TABS)
        shell_screen(s);
    else if (s->load >= COST_CELL)
        for (int32_t r = 0; r < s->rows; r++) latin_row(s, r, (uint32_t)r, 0);
    else if (s->load == SCROLL) /* as the loop paints it */
        for (int32_t r = 0; r < s->rows; r++) paint_row(s, r, (uint32_t)r, line_styles[(uint32_t)r % T5_N(line_styles)]);
    else
        for (int32_t r = 0; r < s->rows; r++) paint_row(s, r, (uint32_t)r, s->load == BLINK ? SHR_STYLE_BLINK : 0);
    if (s->nsprites) build_sprites(s);
    if (s->load == TABS) { /* the editor over the shell, hidden */
        shr_lyr *shell = s->grid;
        t5_try(s, shr_lyr_create(s->ctx, 1, (shr_rect){0, 0, s->width, s->height}, &s->alt), "layer");
        t5_try(s, shr_pl_lyr_tilemap_resize(s->alt, s->font, s->rows, s->cols, &bg), "tilemap_resize");
        s->grid = s->alt;
        ts_code_screen(0, s->rows, s->cols, set_text, s);
        s->grid = shell;
        t5_try(s, shr_lyr_set_visible(s->alt, false), "set_visible");
    }
    if (s->load == SCROLL_CURSOR) {
        t5_try(s, shr_lyr_create(s->ctx, 1, (shr_rect){0, 0, SHR_CELL_WIDTH, SHR_CELL_HEIGHT}, &s->cursor), "layer");
        t5_try(s, shr_lyr_cmd_begin(s->cursor), "cmd_begin");
        t5_try(s, shr_lyr_cmd_fill(s->cursor, (shr_rect){0, 0, SHR_CELL_WIDTH, SHR_CELL_HEIGHT}, SHR_RGB(0x50, 0xFA, 0x7B)),
               "cmd_fill");
        t5_try(s, shr_lyr_cmd_commit(s->cursor), "commit");
    }
}

/* Pumps, sleeping until each deadline, until the frame is shown and nothing is due now; in the blink scene the next
 * phase is due again at once, so it stops at the shown frame. */
static void t5_frame(t5_scene *s, bool submit) {
    if (submit) t5_try(s, shr_submit(s->ctx), "submit");
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    bool shown = false;
    for (int i = 0; i < 1000; i++) {
        if (dl.kind == SHR_DEADLINE_AT && s->sleep) s->sleep(s->user, dl.at_ns);
        if (dl.kind == SHR_DEADLINE_AT && !s->sleep && dl.at_ns > s->frozen) s->frozen = dl.at_ns;
        shr_pump(s->ctx);
        shr_event ev;
        while (shr_poll_event(s->ctx, &ev) == SHR_OK) {
            if (ev.kind == SHR_EVENT_PRESENT_FAILED) t5_try(s, ev.status ? ev.status : SHR_E_STATE, "frame");
            shown |= ev.kind == SHR_EVENT_PRESENT_ACCEPTED;
            s->shown += ev.kind == SHR_EVENT_PRESENT_ACCEPTED;
            s->resource_failed += ev.kind == SHR_EVENT_RESOURCE_FAILED || ev.kind == SHR_EVENT_OVERFLOW;
        }
        shr_next_deadline(s->ctx, &dl);
        if (dl.kind == SHR_DEADLINE_NONE || (shown && (dl.kind == SHR_DEADLINE_AT || s->load == BLINK))) break;
    }
}

/* Holds the clock on a visible blink phase after the next allowed start (`cap_ns` after this one) and lets the last
 * frame finish, so that the checksum does not depend on when the frames ran. */
static void t5_hold_last_phase(t5_scene *s, uint64_t cap_ns) {
    uint64_t t = t5_clock(s) + cap_ns + 2 * T5_BLINK_NS;
    s->frozen = t - t % (2 * T5_BLINK_NS);
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    for (int i = 0; i < 1000 && dl.kind == SHR_DEADLINE_NOW; i++) {
        shr_pump(s->ctx);
        shr_next_deadline(s->ctx, &dl);
    }
    shr_event ev;
    while (shr_poll_event(s->ctx, &ev) == SHR_OK)
        if (ev.kind == SHR_EVENT_PRESENT_FAILED) t5_try(s, ev.status ? ev.status : SHR_E_STATE, "frame");
}

/* Releases the scene's layers and images, then the font and the context. */
static void t5_scene_close(t5_scene *s) {
    for (int i = 0; i < T5_SPRITES; i++)
        if (s->sprites[i].l) shr_lyr_destroy(s->sprites[i].l);
    for (int k = 0; k < 4; k++)
        if (s->img[k]) shr_pl_res_image_release(s->img[k]);
    shr_lyr_destroy(s->grid);
    if (s->cursor) shr_lyr_destroy(s->cursor), s->cursor = NULL;
    if (s->alt) shr_lyr_destroy(s->alt), s->alt = NULL;
    memset(s->sprites, 0, sizeof(s->sprites)), memset(s->img, 0, sizeof(s->img));
    free(s->lines), s->lines = NULL;
    shr_begin_shutdown(s->ctx);
    bool done = false;
    for (int i = 0; i < 64 && !done; i++) {
        shr_pump(s->ctx);
        if (s->font && shr_pl_res_bitmap_font_destroy(s->font) == SHR_OK) s->font = NULL;
        done = !s->font && shr_destroy(s->ctx) == SHR_OK;
    }
    if (!done) t5_try(s, SHR_E_STATE, "shutdown");
}

#ifdef SHIROKO_BENCH_REC_H
/* Loop frames recorded: churn writes the most commands, 60 frames of them would not fit the Tab5's PSRAM. */
static inline int32_t t5_rec_frames(int load) { return load == CHURN ? 30 : 60; }
static const char *const t5_packages[] = {"shiroko-latin.shrf", "shiroko-cjk-ko.shrf", "shiroko-symbols.shrf",
                                          "shiroko-emoji.shrf", "shiroko-nerd.shrf"};

/* The record pass after the scene opened (rec_begin and rec_frame(REC_OPEN) before its context, the clock held at
 * T5_REC_T0_NS, no frame-rate cap): every loop frame of the scene with the first `limit` recorded into the commands
 * `w` and the calls `calls` (either NULL), then the frames until nothing changes and the last blink phase held, as the
 * Tab5 example measures a scene. `checksum` is the screen's (returned at the end), `idle` runs between loop frames. */
static inline uint32_t t5_record_loop(t5_scene *s, rec_writer *w, rec_writer *calls, uint32_t (*checksum)(void *user),
                                      void (*idle)(void *user), void *user) {
    rec_writer *ws[2] = {w, calls};
    uint32_t sum = checksum(user);
    for (int i = 0; i < 2; i++)
        if (ws[i]) rec_frame_done(ws[i], sum);
    for (int32_t f = 0; f < s->frames && s->st == SHR_OK; f++) {
        s->frozen += T5_REC_STEP_NS;
        for (int i = 0; i < 2; i++)
            if (ws[i]) rec_frame(ws[i], f, s->frozen);
        t5_frame(s, load_step(s));
        sum = (w && w->on) || (calls && calls->on) ? checksum(user) : 0;
        for (int i = 0; i < 2; i++)
            if (ws[i]) rec_frame_done(ws[i], sum);
        if (idle) idle(user);
    }
    for (int i = 0; i < 2; i++)
        if (ws[i]) rec_frame(ws[i], (int32_t)ws[i]->limit, s->frozen);
    for (uint32_t settle = 1, shown = s->shown; s->load != BLINK && settle < 64; settle++, shown = s->shown) {
        t5_frame(s, false);
        if (s->shown == shown) break;
    }
    t5_hold_last_phase(s, 0);
    return checksum(user);
}
#endif

#endif
