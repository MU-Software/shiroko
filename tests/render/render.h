#ifndef SHIROKO_RENDER_H
#define SHIROKO_RENDER_H

#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CW SHR_CELL_WIDTH
#define CH SHR_CELL_HEIGHT
#define BLINK_NS 500000000u
#define DIM(c) (((c) & 0xFFFFFFu) | 0x80000000u) /* fg or line alpha 128: half strength */
#define HIDDEN(c) ((c) & 0xFFFFFFu)               /* fg alpha 0: concealed */

/* Pixels may differ by at most max_delta per channel (native bit depth) on at most max_pixels pixels. */
typedef struct fuzz {
    uint32_t max_delta;
    uint64_t max_pixels;
} fuzz;

enum {
    FONTS_NONE = 1u << 0,       /* every package missing: built-in glyphs and fallback only */
    FONTS_LATIN = 1u << 1,      /* only shiroko-latin.shrf is installed */
    FONTS_ASYNC = 1u << 2,      /* reads stay in flight until stage_release_reads() */
    RESOURCE_FAILS = 1u << 3,   /* RESOURCE_FAILED events are expected */
    PRESERVE_NONE = 1u << 4     /* output without SHR_OUTPUT_PRESERVES_CONTENT */
};

typedef struct held_read {
    shr_asset_source *inner;
    uint64_t offset;
    uint32_t length;
    void *dst;
    uint64_t request;
} held_read;

/* What a scene builds with. Helpers keep the first failure in `st`/`what` and do nothing after it. */
typedef struct stage {
    shr_context *ctx;
    shr_pl_res_bitmap_font *font;
    shr_lyr *layers[512];
    size_t nlayers;
    shr_pl_res_image *images[64];
    size_t nimages;
    int page;
    uint64_t now;
    held_read held[64];
    size_t nheld;
    bool hold;
    shr_status st;
    char what[256];
} stage;

typedef shr_status (*scene_fn)(stage *s);

typedef struct scene {
    const char *name;
    int32_t w, h;
    shr_rotation rotation;
    shr_pixel_format output_format; /* 0 = SHR_PIXEL_FORMAT */
    uint32_t screen_flags;
    uint32_t flags;
    uint64_t now_ns;
    scene_fn build;
    scene_fn update;     /* runs after the first frame, followed by another frame */
    int (*pages)(void);  /* NULL = one page; otherwise the scene renders once per page as name-<page> */
    fuzz gpu;            /* {0, 0} = default */
} scene;

typedef struct reftest {
    const char *test, *ref;
} reftest;

extern const scene scenes[];
extern const size_t scene_count;
extern const reftest reftests[];
extern const size_t reftest_count;

bool stage_ok(stage *s, shr_status st, const char *what);
/* Expects `want`; anything else is a failure of the scene. */
bool stage_expect(stage *s, shr_status got, shr_status want, const char *what);
shr_lyr *stage_layer(stage *s, int32_t z, shr_rect r);
/* A layer with a tilemap filling `r` (whole cells). */
shr_lyr *stage_grid(stage *s, int32_t z, shr_rect r, const shr_color *background);
shr_pl_res_image *stage_image(stage *s, int32_t w, int32_t h, const uint8_t *rgba);
void stage_text(stage *s, shr_lyr *l, int32_t row, int32_t col, const char *utf8, shr_text_style style, uint32_t flags);
void stage_cell(stage *s, shr_lyr *l, int32_t row, int32_t col, const char *utf8, uint32_t span, shr_text_style style);
void stage_fill(stage *s, shr_lyr *l, shr_rect r, shr_color c);
void stage_lines(stage *s, shr_lyr *l, int32_t row, const shr_text_line *lines, size_t count);
/* Appends `line` over the cells of bytes [b0, b1) of `utf8` that land on row `at` when set_text lays the text out from
 * (row, col) of a grid `cols` wide with `flags`, one line per run of cells; returns the new count. */
size_t stage_text_lines(const char *utf8, size_t b0, size_t b1, int32_t row, int32_t col, int32_t cols, uint32_t flags,
                        int32_t at, shr_text_line line, shr_text_line *out, size_t n);
/* Opens package `name` from `font_dir` as a scene with `flags` sees it (FONTS_NONE, FONTS_LATIN, FONTS_ASYNC). */
shr_status stage_open_package(stage *s, uint32_t flags, const char *font_dir, const char *name, shr_asset_source *out);
/* Completes every held read (more may follow) until none is left. */
shr_status stage_release_reads(stage *s);

#endif
