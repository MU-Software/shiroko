#include "shr_bitmap_font.h"
#include "shr_hash.h"
#include "shr_text.h"

_Static_assert((int64_t)SHR_MAX_GRID * SHR_CELL_WIDTH <= INT32_MAX && (int64_t)SHR_MAX_GRID * SHR_CELL_HEIGHT <= INT32_MAX,
               "grid pixel extent must fit int32");
_Static_assert(SHR_MAX_GRID <= 1 << 15 && SHR_MAX_SPAN <= 256 && SHR_STYLE_KNOWN_FLAGS <= 0xFF,
               "a row key packs column, span - 1, flags and has_glyph into 32 bits");
_Static_assert(SHR_MAX_GRID <= UINT16_MAX, "row commands and lines hold columns");
_Static_assert(SHR_CELL_WIDTH <= SHR_LINE_MAX_PERIOD && sizeof(shr_text_line) == 12, "line patterns are a cell wide");

enum { CELL_EMPTY = 0, CELL_HEAD, CELL_CONT };
#define CELL_INLINE 12
#define DIRTY 0x80000000u /* in `line`: the row changed since its group was built */

/* 32 bytes: the rows a submit rebuilds are read from PSRAM on small targets. */
typedef struct shr__cell {
    shr_color fg, bg;
    uint32_t glyph; /* glyph ids fit 32 bits */
    char text[CELL_INLINE]; /* the cluster, or past CELL_INLINE bytes a pointer to it */
    uint16_t len;
    uint16_t span; /* HEAD: cells covered; CONT: distance back to the head */
    uint8_t flags, kind;
    bool has_glyph;
} shr__cell;
_Static_assert(sizeof(char *) <= CELL_INLINE && SHR_STYLE_KNOWN_FLAGS <= UINT8_MAX, "cell fields");

/* A cluster of up to 8 bytes set before, zero-padded (valid text has no NUL), and its glyph; len 0: empty. */
typedef struct shr__memo {
    uint64_t key;
    uint32_t glyph;
    uint8_t len;
    bool has_glyph;
} shr__memo;

typedef struct shr__placed {
    int32_t row, col;
    shr__cell cell;
} shr__placed;

/* The lines of a row, `cap` entries at `at`. */
typedef struct shr__lines {
    shr_text_line *at;
    uint32_t n, cap;
} shr__lines;

typedef struct shr__tilemap {
    shr__alloc al;
    shr_lyr *layer;
    shr_context *ctx;
    shr_pl_res_bitmap_font *font;
    int32_t rows, cols;
    bool has_bg, long_text; /* long_text: a cell got text beyond CELL_INLINE */
    shr_color bg;
    shr__cell *cells;
    shr__lines *lines; /* per row of `cells` */
    uint32_t *line; /* per screen row: its row of `cells` and `lines`, | DIRTY */
    shr__vec keys; /* uint64_t, the bytes a row's cache key hashes: two per head and per line */
    shr__vec placed; /* shr__placed, set_text's cells until the text is valid, kept for the next call */
    uint8_t band[3][5][2]; /* pixel rows [y0, y1) of the lines by kind and shape */
    shr__memo memo[1024];
} shr__tilemap;

static const char tilemap_kind;

/* The memo and the row keys are used every frame. */
enum { TILEMAP_KIND = SHR_ALLOC_DESCRIPTOR | SHR_ALLOC_HOT };

static shr__cell *row_at(const shr__tilemap *t, int32_t r) {
    return &t->cells[(size_t)(t->line[r] & ~DIRTY) * (size_t)t->cols];
}

static shr__lines *row_lines(const shr__tilemap *t, int32_t r) { return &t->lines[t->line[r] & ~DIRTY]; }

static char *cell_heap(const shr__cell *e) {
    char *p;
    memcpy(&p, e->text, sizeof(p));
    return p;
}

static const char *cell_text(const shr__cell *e) { return e->len > CELL_INLINE ? cell_heap(e) : e->text; }

static void heap_free(const shr__alloc *al, const shr__cell *e) {
    if (e->len > CELL_INLINE) shr__free(al, cell_heap(e), e->len, 1, SHR_ALLOC_PAYLOAD);
}

static void cell_free(const shr__alloc *al, shr__cell *e) {
    if (e->kind == CELL_HEAD) heap_free(al, e);
    *e = (shr__cell){0};
}

/* A head without text. */
static shr__cell head(shr_text_style style, uint32_t glyph, size_t len, uint32_t span, bool has_glyph) {
    return (shr__cell){style.fg, style.bg, glyph, {0}, (uint16_t)len, (uint16_t)span, (uint8_t)style.flags, CELL_HEAD,
                       has_glyph};
}

/* Validates one cluster as set_cell receives it and classifies it; cps keeps its first scalars, n counts them all. */
static shr_status cluster_decode(const char *utf8, size_t len, uint32_t *cps, size_t *n, shr__cluster_class *cls) {
    size_t pos = 0;
    *n = 0;
    while (pos < len) {
        uint32_t cp;
        if (shr__utf8_next((const uint8_t *)utf8, len, &pos, &cp)) return SHR_E_INVALID_UTF8;
        if (shr__is_control(cp)) return SHR_E_CONTROL_CHAR;
        if (*n < shr__cluster_max_scalars) cps[*n] = cp;
        ++*n;
    }
    shr__classify_cluster(cps, *n, cls);
    if (*n > shr__cluster_max_scalars) *n = shr__cluster_max_scalars;
    return SHR_OK;
}

static shr_status cell_glyph(shr_pl_res_bitmap_font *font, shr__cell *e, const uint32_t *cps, size_t n,
                             const shr__cluster_class *cls) {
    e->has_glyph = false;
    if (!n) return SHR_OK;
    uint64_t glyph = e->glyph;
    shr_status st = shr__bitmap_font_glyph(font, cps, n, cls, &glyph);
    e->glyph = (uint32_t)glyph, e->has_glyph = st == SHR_OK;
    return st == SHR_E_NOT_FOUND ? SHR_OK : st;
}

/* `utf8` is a valid cluster within the cell limits, decoded into `cps` and classified as `cls`. */
static shr_status cell_make(shr__tilemap *t, const char *utf8, size_t len, uint32_t span, shr_text_style style,
                            const uint32_t *cps, size_t n, const shr__cluster_class *cls, shr__cell *out) {
    *out = head(style, 0, len, span, false);
    char *dst = out->text;
    if (len > CELL_INLINE) {
        if (!(dst = shr__malloc(&t->al, len, 1, SHR_ALLOC_PAYLOAD))) return SHR_E_NO_MEMORY;
        memcpy(out->text, &dst, sizeof(dst));
    }
    if (len) memcpy(dst, utf8, len);
    shr_status st = cell_glyph(t->font, out, cps, n, cls);
    if (st != SHR_OK) cell_free(&t->al, out);
    return st;
}

static shr__cell blank(shr_text_style style) { return head(style, 0, 0, 1, false); }

static void lines_free(const shr__alloc *al, shr__lines *l) {
    shr__free(al, l->at, l->cap * sizeof(*l->at), SHR_ALIGNOF(shr_text_line), SHR_ALLOC_PAYLOAD);
}

/* Room for n lines, keeping those held: rows keep their room when they empty, and a row that needs more trades with
 * the smallest empty row that has it, else takes new room in powers of two. */
static bool lines_room(shr__tilemap *t, shr__lines *l, size_t n) {
    if (n <= l->cap) return true;
    shr__lines *e = NULL;
    for (int32_t r = 0; r < t->rows; r++) {
        shr__lines *x = &t->lines[r];
        if (!x->n && x->cap >= n && (!e || x->cap < e->cap)) e = x;
    }
    size_t cap = 4;
    while (cap < n) cap *= 2;
    shr__lines got = e ? *e : (shr__lines){shr__malloc(&t->al, cap * sizeof(*l->at), SHR_ALIGNOF(shr_text_line),
                                                       SHR_ALLOC_PAYLOAD), 0, (uint32_t)cap};
    if (!got.at) return false;
    if (l->n) memcpy(got.at, l->at, l->n * sizeof(*l->at));
    got.n = l->n, l->n = 0;
    if (e)
        *e = *l;
    else
        lines_free(&t->al, l);
    *l = got;
    return true;
}

/* Lines that hold columns on both sides of [c0, c1): cutting it out splits them. */
static size_t lines_split(const shr__lines *l, int32_t c0, int32_t c1) {
    size_t n = 0;
    for (uint32_t i = 0; i < l->n; i++) n += l->at[i].col < c0 && l->at[i].col + l->at[i].cols > c1;
    return n;
}

/* Cuts columns [c0, c1) out of the lines, with room for the splits: the lines move to the end of the room and come
 * back piece by piece, never past one not read yet. */
static void lines_cut(shr__lines *l, int32_t c0, int32_t c1) {
    if (!l->n) return;
    shr_text_line *at = l->at, *src = at + l->cap - l->n;
    uint32_t n = l->n, w = 0;
    memmove(src, at, n * sizeof(*src));
    for (uint32_t i = 0; i < n; i++) {
        shr_text_line e = src[i], x = e;
        int32_t a = e.col, b = e.col + e.cols;
        if (b <= c0 || a >= c1) {
            at[w++] = e;
            continue;
        }
        if (a < c0) x.cols = (uint16_t)(c0 - a), at[w++] = x;
        if (b > c1) x = e, x.col = (uint16_t)c1, x.cols = (uint16_t)(b - c1), at[w++] = x;
    }
    l->n = w;
}

/* Clears [*c0, *c1) of row r widened to the wide cells reaching into it. */
static void clear_cells(shr__tilemap *t, int32_t r, int32_t *c0, int32_t *c1) {
    if (*c0 >= *c1) return;
    shr__cell *row = row_at(t, r);
    if (row[*c0].kind == CELL_CONT) *c0 -= row[*c0].span;
    int32_t last = *c1 - 1;
    if (row[last].kind == CELL_CONT) last -= row[last].span;
    if (row[last].kind == CELL_HEAD && last + row[last].span > *c1) *c1 = last + row[last].span;
    for (int32_t c = *c0; c < *c1; c++) cell_free(&t->al, &row[c]);
    t->line[r] |= DIRTY;
}

/* Frees what a head of `span` cells at (r, c) covers, empties what it leaves of wide cells it cuts and writes its
 * continuations; the caller writes the head. */
static shr__cell *place(shr__tilemap *t, int32_t r, int32_t c, uint32_t span) {
    shr__cell *row = row_at(t, r);
    t->line[r] |= DIRTY;
    if (row[c].kind == CELL_HEAD && row[c].span == span) { /* same footprint: its continuations stay */
        heap_free(&t->al, &row[c]);
        return &row[c];
    }
    int32_t end = c + (int32_t)span, last = end - 1;
    int32_t c0 = c - (row[c].kind == CELL_CONT ? row[c].span : 0);
    int32_t lh = last - (row[last].kind == CELL_CONT ? row[last].span : 0);
    int32_t c1 = row[lh].kind == CELL_HEAD && lh + row[lh].span > end ? lh + row[lh].span : end;
    for (int32_t k = c0; t->long_text && k < c1; k++)
        if (row[k].kind == CELL_HEAD) heap_free(&t->al, &row[k]);
    row[c0].kind = row[c1 - 1].kind = CELL_EMPTY; /* inside the new cell the writes below win */
    for (int32_t k = c0 + 1; k < c; k++) row[k].kind = CELL_EMPTY;
    for (int32_t k = end; k < c1 - 1; k++) row[k].kind = CELL_EMPTY;
    row[last] = (shr__cell){.span = (uint16_t)(span - 1), .kind = CELL_CONT};
    for (uint32_t k = 1; k + 1 < span; k++) row[c + k] = (shr__cell){.span = (uint16_t)k, .kind = CELL_CONT};
    return &row[c];
}

/* A row command over cells [c0, c1), pixel rows [y0, y1). */
static void put(shr__rcmd *c, uint8_t kind, uint32_t flags, int32_t c0, int32_t c1, int32_t y0, int32_t y1,
                shr_color color) {
    *c = (shr__rcmd){(uint16_t)c0, (uint16_t)c1, (uint8_t)y0, (uint8_t)y1, kind, (uint8_t)flags, color, 0, 0};
}

/* The band of each kind and shape in the cell: SINGLE and DASHED one row at the line, DOUBLE and DOTTED three around
 * it, CURLY a wave of amplitude min(w / pi, rows below the baseline - 1) below an underline, around the others; moved
 * inside the cell. */
static void line_bands(shr__tilemap *t) {
    const shr__line_metrics lm = shr__bitmap_font_line_metrics();
    int32_t amp = SHR_CELL_WIDTH * 113 / 355, below = SHR_CELL_HEIGHT - lm.baseline - 1;
    amp = amp < below ? amp : below;
    amp = amp < 1 ? 1 : amp > SHR_LINE_MAX_BAND - 1 ? SHR_LINE_MAX_BAND - 1 : amp;
    for (int kind = SHR_LINE_UNDER; kind <= SHR_LINE_OVER; kind++)
        for (int shape = SHR_LINE_SINGLE; shape <= SHR_LINE_DASHED; shape++) {
            int32_t y = kind == SHR_LINE_UNDER ? lm.underline_y : kind == SHR_LINE_STRIKE ? lm.strike_y : 0;
            bool thin = shape == SHR_LINE_SINGLE || shape == SHR_LINE_DASHED, curly = shape == SHR_LINE_CURLY;
            int32_t h = thin ? 1 : curly ? amp + 1 : 3;
            int32_t top = thin || (curly && kind == SHR_LINE_UNDER) ? y : y - (h - 1) / 2;
            top = top > SHR_CELL_HEIGHT - h ? SHR_CELL_HEIGHT - h : top;
            top = top < 0 ? 0 : top;
            t->band[kind][shape][0] = (uint8_t)top;
            t->band[kind][shape][1] = (uint8_t)(top + (h < SHR_CELL_HEIGHT ? h : SHR_CELL_HEIGHT));
        }
}

static bool filled(const shr__tilemap *t, const shr__cell *e) { return t->has_bg || (e->bg >> 24) != 0; }

/* One group per row, in row coordinates placed at the row's y: the backgrounds as runs of one colour, then the glyphs,
 * which stay inside their cells, so a glyph lies on its cell's background alone, then the lines. Only rows that opaque
 * backgrounds cover completely get a cache hint, since a cached row replaces what lies under it. The first pass counts
 * the commands, the second writes the runs and the other commands into their two parts of the group. */
static shr_status build_row(shr__tilemap *t, int32_t r) {
    const shr__cell *row = row_at(t, r);
    const shr__lines *lines = row_lines(t, r);
    const shr_text_line *at = lines->at;
    shr__res *res = shr__bitmap_font_res(t->font);
    t->keys.len = 0;
    if (!shr__vec_reserve(&t->keys, &t->al, 2 * (size_t)t->cols + 2 * (size_t)lines->n + 2)) return SHR_E_NO_MEMORY;
    uint64_t *keys = t->keys.data, *k = keys;
    *k++ = res->serial, *k++ = (uint64_t)t->has_bg << 32 | t->bg;
    int32_t covered = 0;
    size_t runs = 0, n = 0;
    bool was = false; /* the previous cell is filled with `was_color` */
    shr_color was_color = 0;
    for (int32_t c = 0; c < t->cols; c++) {
        const shr__cell *e = &row[c];
        if (e->kind == CELL_CONT) continue;
        bool is_head = e->kind == CELL_HEAD, fill = is_head ? filled(t, e) : t->has_bg;
        shr_color color = is_head && (e->bg >> 24) ? e->bg : t->bg;
        runs += fill && !(was && color == was_color);
        was = fill, was_color = color;
        if (!is_head) continue;
        covered += fill ? e->span : 0;
        k[0] = (uint64_t)e->fg << 32 | e->bg;
        k[1] = (uint64_t)e->glyph << 32 | (uint32_t)c << 17 | (uint32_t)(e->span - 1) << 9 | (uint32_t)e->flags << 1 |
               e->has_glyph,
        k += 2;
        n += e->has_glyph && (e->fg >> 24);
    }
    for (uint32_t i = 0; i < lines->n; i++) {
        const shr_text_line *e = &at[i];
        k[0] = e->col | (uint64_t)e->cols << 16 | (uint64_t)e->kind << 32 | (uint64_t)e->shape << 40 |
               (uint64_t)e->flags << 48;
        k[1] = e->color, k += 2;
        n += (e->color >> 24) != 0;
    }
    if (!runs && !n) return shr__lyr_group_set(t->layer, (uint32_t)r, NULL, 0);
    size_t cached = covered >= t->cols || t->has_bg, total = runs + n + 2 * cached;
    shr__rcmd *cmds = shr__lyr_row_begin(t->layer, total);
    if (!cmds) return SHR_E_NO_MEMORY;
    shr__rcmd *fills = cmds + cached, *out = fills + runs;
    int32_t run = 0;
    was = false;
    for (int32_t c = 0; c <= t->cols; c++) {
        const shr__cell *e = &row[c < t->cols ? c : 0];
        if (c < t->cols && e->kind == CELL_CONT) continue;
        bool is_head = c < t->cols && e->kind == CELL_HEAD;
        bool fill = c < t->cols && (is_head ? filled(t, e) : t->has_bg);
        shr_color color = is_head && (e->bg >> 24) ? e->bg : t->bg;
        bool same = was && fill && color == was_color;
        if (was && !same) put(fills++, SHR__LCMD_FILL, 0, run, c, 0, SHR_CELL_HEIGHT, was_color | 0xFF000000u);
        if (fill && !same) run = c;
        was = fill, was_color = color;
        if (!is_head || !e->has_glyph || !(e->fg >> 24)) continue;
        uint32_t f = e->flags, gx = ((e->fg >> 24) == 128 ? SHR__LCMD_DIM : 0) | (fill ? SHR__LCMD_ON_FILL : 0);
        gx |= ((f & SHR_STYLE_BLINK) ? SHR__LCMD_BLINK : 0) | ((f & SHR_STYLE_BOLD) ? SHR__LCMD_BOLD : 0) |
              ((f & SHR_STYLE_ITALIC) ? SHR__LCMD_ITALIC : 0);
        put(out, SHR__LCMD_GLYPH, gx, c, c + e->span, 0, SHR_CELL_HEIGHT, e->fg | 0xFF000000u);
        out->id = e->glyph, out->bg = fill ? color | 0xFF000000u : 0, out++;
    }
    for (uint32_t i = 0; i < lines->n; i++) {
        const shr_text_line *e = &at[i];
        if (!(e->color >> 24)) continue;
        const uint8_t *y = t->band[e->kind][e->shape];
        uint32_t fx = ((e->color >> 24) == 128 ? SHR__LCMD_DIM : 0) |
                      ((e->flags & SHR_TEXT_LINE_BLINK) ? SHR__LCMD_BLINK : 0);
        put(out, SHR__LCMD_LINE, fx, e->col, e->col + e->cols, y[0], y[1], e->color | 0xFF000000u);
        out->id = e->shape, out++;
    }
    uint64_t key[2] = {0, 0};
    if (cached) {
        XXH128_hash_t h = XXH3_128bits(keys, (size_t)(k - keys) * sizeof(*keys));
        put(cmds, SHR__LCMD_CACHE_BEGIN, 0, 0, t->cols, 0, SHR_CELL_HEIGHT, 0);
        key[0] = h.low64, key[1] = h.high64;
        put(out, SHR__LCMD_CACHE_END, 0, 0, 0, 0, 0, 0);
    }
    return shr__lyr_row_commit(t->layer, (uint32_t)r, r * SHR_CELL_HEIGHT, res, key, cmds, total);
}

/* Rows that fail stay dirty. */
static shr_status build_rows(shr__tilemap *t, int32_t r0, int32_t r1) {
    for (int32_t r = r0; r < r1; r++) {
        if (!(t->line[r] & DIRTY)) continue;
        shr_status st = build_row(t, r);
        if (st != SHR_OK) return st;
        t->line[r] &= ~DIRTY;
    }
    return SHR_OK;
}

/* Changed rows are rebuilt once per submit. */
static shr_status flush(void *state) {
    shr__tilemap *t = state;
    return build_rows(t, 0, t->rows);
}

static void free_grid(shr__tilemap *t) {
    for (size_t i = 0; i < (size_t)t->rows * (size_t)t->cols; i++) cell_free(&t->al, &t->cells[i]);
    for (int32_t r = 0; t->lines && r < t->rows; r++) lines_free(&t->al, &t->lines[r]);
    SHR_FREE_ARRAY(&t->al, t->cells, shr__cell, (size_t)t->rows * (size_t)t->cols);
    SHR_FREE_ARRAY(&t->al, t->lines, shr__lines, (size_t)t->rows);
    SHR_FREE_HOT_ARRAY(&t->al, t->line, uint32_t, (size_t)t->rows);
}

static void tilemap_destroy(void *state) {
    shr__tilemap *t = state;
    shr__alloc al = t->al;
    free_grid(t);
    shr__vec_free(&t->keys, &al);
    shr__vec_free(&t->placed, &al);
    shr__bitmap_font_res(t->font)->users--;
    shr__free(&al, t, sizeof(*t), SHR_ALIGNOF(shr__tilemap), TILEMAP_KIND);
}

static shr_status tilemap_get(shr_lyr *layer, shr__tilemap **out) {
    if (!layer) return SHR_E_INVALID_ARG;
    if (!(*out = shr__lyr_state(layer, &tilemap_kind))) return SHR_E_STATE;
    return shr__ctx_refused((*out)->ctx) ? SHR_E_STATE : SHR_OK;
}

/* A head of the old grid that the new rows x cols grid keeps. */
static bool kept(const shr__cell *e, int32_t r, int32_t c, int32_t rows, int32_t cols) {
    return e->kind == CELL_HEAD && r < rows && c + e->span <= cols;
}

shr_status shr_pl_lyr_tilemap_resize(shr_lyr *layer, shr_pl_res_bitmap_font *font, int32_t rows, int32_t cols,
                                     const shr_color *background) {
    if (!layer || !font || rows < 0 || cols < 0) return SHR_E_INVALID_ARG;
    shr_context *ctx = shr__lyr_ctx(layer);
    shr__res *res = shr__bitmap_font_res(font);
    if (res->ctx != ctx) return SHR_E_INVALID_ARG;
    if (rows > SHR_MAX_GRID || cols > SHR_MAX_GRID) return SHR_E_LIMIT;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    shr__tilemap *t = shr__lyr_state(layer, &tilemap_kind);
    const shr__alloc *al = t ? &t->al : shr__ctx_alloc(ctx);
    shr__cell *cells = SHR_NEW_ARRAY(al, shr__cell, (size_t)rows * (size_t)cols);
    shr__lines *lines = SHR_NEW_ARRAY(al, shr__lines, (size_t)rows);
    uint32_t *line = SHR_NEW_HOT_ARRAY(al, uint32_t, (size_t)rows);
    shr_status st = cells && lines && line ? SHR_OK : SHR_E_NO_MEMORY;
    /* Kept heads are copied (sharing their text) so the old grid stays intact until everything succeeded. */
    for (int32_t r = 0; t && st == SHR_OK && r < t->rows; r++)
        for (int32_t c = 0; st == SHR_OK && c < t->cols; c++) {
            const shr__cell *e = &row_at(t, r)[c];
            if (!kept(e, r, c, rows, cols)) continue;
            shr__cell *d = &cells[(size_t)r * (size_t)cols + (size_t)c];
            *d = *e;
            for (uint16_t k = 1; k < e->span; k++) d[k] = (shr__cell){.span = k, .kind = CELL_CONT};
            if (font != t->font) {
                uint32_t cps[SHR_CLUSTER_SCALARS];
                size_t n;
                shr__cluster_class cls;
                cluster_decode(cell_text(d), d->len, cps, &n, &cls); /* validated when it was set */
                st = cell_glyph(font, d, cps, n, &cls);
            }
        }
    if (st == SHR_OK && !t) {
        if (!(t = shr__calloc(al, 1, sizeof(*t), SHR_ALIGNOF(shr__tilemap), TILEMAP_KIND))) {
            st = SHR_E_NO_MEMORY;
        } else {
            *t = (shr__tilemap){.al = *al, .layer = layer, .ctx = ctx, .font = font};
            SHR_VEC_INIT_HOT(&t->keys, uint64_t);
            SHR_VEC_INIT(&t->placed, shr__placed);
            line_bands(t);
            if ((st = shr__lyr_attach(layer, &tilemap_kind, t, tilemap_destroy, flush)) != SHR_OK)
                shr__free(al, t, sizeof(*t), SHR_ALIGNOF(shr__tilemap), TILEMAP_KIND), t = NULL;
            else
                res->users++;
        }
    }
    if (st != SHR_OK) {
        SHR_FREE_ARRAY(al, cells, shr__cell, (size_t)rows * (size_t)cols);
        SHR_FREE_ARRAY(al, lines, shr__lines, (size_t)rows);
        SHR_FREE_HOT_ARRAY(al, line, uint32_t, (size_t)rows);
        return st;
    }
    for (int32_t r = 0; r < t->rows; r++) {
        for (int32_t c = 0; c < t->cols; c++) {
            shr__cell *e = &row_at(t, r)[c];
            if (kept(e, r, c, rows, cols)) *e = (shr__cell){0}; /* now owned by the new grid */
        }
        if (r >= rows) continue;
        shr__lines *l = row_lines(t, r);
        lines_cut(l, cols, INT32_MAX);
        lines[r] = *l, *l = (shr__lines){0};
    }
    free_grid(t);
    if (font != t->font) {
        memset(t->memo, 0, sizeof(t->memo)); /* cluster ids are the font's */
        shr__bitmap_font_res(t->font)->users--;
        res->users++;
        t->font = font;
    }
    t->rows = rows, t->cols = cols, t->cells = cells, t->lines = lines, t->line = line;
    t->has_bg = background != NULL;
    t->bg = background ? *background : 0;
    for (int32_t r = 0; r < rows; r++) line[r] = (uint32_t)r | DIRTY;
    shr__lyr_groups_clear(layer, 2 * (size_t)cols + 2); /* cannot fail: the context was checked above */
    return SHR_OK;
}

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "memo keys are cell text bytes read as words");

static shr__memo *memo_slot(shr__tilemap *t, uint64_t key) {
    uint32_t lo = (uint32_t)key, hi = (uint32_t)(key >> 32);
    return &t->memo[(uint64_t)(lo ^ hi) * 0x9E3779B9u >> 22 & 1023];
}

/* The head at (r, c) already holds this cluster: its text in `key` up to 8 bytes, else at `utf8`. */
static inline __attribute__((always_inline)) bool cell_same(const shr__tilemap *t, int32_t r, int32_t c,
                                                            const char *utf8, size_t len, uint64_t key, uint32_t span,
                                                            shr_text_style s) {
    const shr__cell *old = &row_at(t, r)[c];
    uint64_t was;
    memcpy(&was, old->text, sizeof(was));
    bool same = (old->kind == CELL_HEAD) & (old->span == span) & (old->len == len) & (old->fg == s.fg) &
                (old->bg == s.bg) & (old->flags == s.flags);
    return same && (len <= 8 ? was == key : !memcmp(cell_text(old), utf8, len));
}

/* A text of 1 to 8 bytes zero-padded into a word, without reads past its end: the first 4 bytes and the ones after them
 * as words, and bytes 0, len / 2 and len - 1 of up to 3 (from 4 bytes on, ones the first word holds). */
static inline __attribute__((always_inline)) uint64_t text_key(const char *utf8, size_t len) {
    static const uint8_t none[4];
    const uint8_t *u = (const uint8_t *)utf8;
    size_t h = (len >> 1) & 3, l = (len - 1) & 3;
    uint32_t lo, hi;
    memcpy(&lo, len >= 4 ? u : none, 4), memcpy(&hi, len > 4 ? u + len - 4 : none, 4);
    lo |= u[0] | (uint32_t)u[h] << 8 * h | (uint32_t)u[l] << 8 * l;
    hi >>= 8 * (8 - len) & 31;
    return lo | (uint64_t)hi << 32;
}

/* The head of a valid cluster within the cell limits, its glyph from the font. */
static shr_status cell_new(shr__tilemap *t, const char *utf8, size_t len, uint32_t span, shr_text_style s,
                           shr__cell *out) {
    uint32_t cps[SHR_CLUSTER_SCALARS];
    size_t n;
    shr__cluster_class cls;
    shr_status st = cluster_decode(utf8, len, cps, &n, &cls);
    return st == SHR_OK ? cell_make(t, utf8, len, span, s, cps, n, &cls, out) : st;
}

/* The head a valid cluster within the cell limits makes at (r, c), from the memo when it holds the text; an empty
 * cell when the grid holds it already. */
static shr_status cell_for(shr__tilemap *t, int32_t r, int32_t c, const char *utf8, size_t len, uint32_t span,
                           shr_text_style s, shr__cell *out) {
    uint64_t key = len && len <= 8 ? text_key(utf8, len) : 0;
    if (cell_same(t, r, c, utf8, len, key, span, s)) return *out = (shr__cell){0}, SHR_OK;
    shr__memo *m = len && len <= 8 ? memo_slot(t, key) : NULL;
    if (m && m->len == len && m->key == key) {
        *out = head(s, m->glyph, len, span, m->has_glyph);
        memcpy(out->text, &key, sizeof(key));
        return SHR_OK;
    }
    shr_status st = cell_new(t, utf8, len, span, s, out);
    if (st == SHR_OK && m) *m = (shr__memo){key, out->glyph, (uint8_t)len, out->has_glyph};
    return st;
}

static void cell_put(shr__tilemap *t, int32_t r, int32_t c, const shr__cell *e) {
    if (e->kind != CELL_HEAD) return;
    t->long_text |= e->len > CELL_INLINE;
    *place(t, r, c, e->span) = *e;
}

/* cell_for() and cell_put(), a head from the memo written in place. Forced inline, as are cell_same() and text_key():
 * clang takes the code after set_cell's checks for cold and inlines next to nothing there. */
static inline __attribute__((always_inline)) shr_status cell_set(shr__tilemap *t, int32_t r, int32_t c,
                                                                 const char *utf8, size_t len, uint32_t span,
                                                                 shr_text_style s) {
    uint64_t key = len && len <= 8 ? text_key(utf8, len) : 0;
    if (cell_same(t, r, c, utf8, len, key, span, s)) return SHR_OK;
    shr__memo *m = len && len <= 8 ? memo_slot(t, key) : NULL;
    if (m && m->len == len && m->key == key) {
        shr__cell *e = place(t, r, c, span);
        *e = head(s, m->glyph, len, span, m->has_glyph);
        memcpy(e->text, &key, sizeof(key));
        return SHR_OK;
    }
    shr__cell e;
    shr_status st = cell_new(t, utf8, len, span, s, &e);
    if (st != SHR_OK) return st;
    if (m) *m = (shr__memo){key, e.glyph, (uint8_t)len, e.has_glyph};
    cell_put(t, r, c, &e);
    return SHR_OK;
}

shr_status shr_pl_lyr_tilemap_set_cell(shr_lyr *layer, int32_t row, int32_t col, const char *utf8, size_t length,
                                       uint32_t span, shr_text_style style) {
    shr__tilemap *t;
    shr_status st = tilemap_get(layer, &t);
    if (st != SHR_OK) return st;
    if ((length && !utf8) || span == 0) return SHR_E_INVALID_ARG;
    if ((st = shr__style_check(&style, NULL, 0)) != SHR_OK) return st;
    if (length > SHR_MAX_TEXT_BYTES || span > SHR_MAX_SPAN) return SHR_E_LIMIT;
    if (row < 0 || row >= t->rows || col < 0 || (int64_t)col + span > t->cols) return SHR_E_INVALID_ARG;
    if (length > shr__cluster_max_bytes) { /* past the scalar limit too: the cell keeps U+FFFD */
        uint32_t cps[SHR_CLUSTER_SCALARS];
        size_t n;
        shr__cluster_class cls;
        if ((st = cluster_decode(utf8, length, cps, &n, &cls)) != SHR_OK) return st;
        utf8 = SHR_REPLACEMENT_UTF8, length = 3;
    }
    return cell_set(t, row, col, utf8, length, span, style);
}

static shr_status lines_check(const shr__tilemap *t, const shr_text_line *in, size_t n, shr_error_info *err) {
    if (n && !in) return shr__fail(err, SHR_E_INVALID_ARG, 0, SIZE_MAX, "missing lines");
    if (n > 4 * (size_t)t->cols) return shr__fail(err, SHR_E_LIMIT, 0, SIZE_MAX, "more than 4 lines per column");
    for (size_t i = 0; i < n; i++) {
        const shr_text_line *e = &in[i];
        if (!e->cols || e->col + e->cols > t->cols) return shr__fail(err, SHR_E_INVALID_ARG, 0, i, "line outside the grid");
        if (e->kind > SHR_LINE_OVER || e->shape > SHR_LINE_DASHED || (e->flags & ~SHR_TEXT_LINE_BLINK) ||
            !shr__alpha_known(e->color, true))
            return shr__fail(err, SHR_E_UNKNOWN_STYLE, 0, i, "unknown line kind, shape, flag or colour alpha");
    }
    return SHR_OK;
}

/* Checked lines of row r, with room for them. */
static void lines_set(shr__tilemap *t, int32_t r, const shr_text_line *in, size_t n) {
    shr__lines *l = row_lines(t, r);
    if (l->n == n && (!n || !memcmp(l->at, in, n * sizeof(*in)))) return; /* unchanged */
    if (n) memcpy(l->at, in, n * sizeof(*in));
    l->n = (uint32_t)n, t->line[r] |= DIRTY;
}

shr_status shr_pl_lyr_tilemap_set_lines(shr_lyr *layer, int32_t row, const shr_text_line *lines, size_t count,
                                        shr_error_info *err) {
    shr__err_clear(err);
    shr__tilemap *t;
    shr_status st = tilemap_get(layer, &t);
    if (st != SHR_OK) return shr__fail(err, st, 0, SIZE_MAX, "not a tilemap layer or context busy");
    if (row < 0 || row >= t->rows) return shr__fail(err, SHR_E_INVALID_ARG, 0, SIZE_MAX, "row outside the grid");
    if ((st = lines_check(t, lines, count, err)) != SHR_OK) return st;
    if (!lines_room(t, row_lines(t, row), count)) return shr__fail(err, SHR_E_NO_MEMORY, 0, SIZE_MAX, "no memory");
    lines_set(t, row, lines, count);
    return SHR_OK;
}

/* The UTF-8 of a code point in the low bytes of a word, and its length. */
static size_t cp_utf8(uint32_t cp, uint64_t *w) {
    if (cp < 0x80) return *w = cp, 1;
    if (cp < 0x800) return *w = (0xC0 | cp >> 6) | (0x80 | (cp & 63)) << 8, 2;
    if (cp < 0x10000) return *w = (0xE0 | cp >> 12) | (0x80 | (cp >> 6 & 63)) << 8 | (0x80 | (cp & 63)) << 16, 3;
    *w = (0xF0 | cp >> 18) | (0x80 | (cp >> 12 & 63)) << 8 | (0x80 | (cp >> 6 & 63)) << 16 |
         (uint64_t)(0x80 | (cp & 63)) << 24;
    return 4;
}

static shr_status cp_check(uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return SHR_E_INVALID_UTF8;
    return shr__is_control(cp) ? SHR_E_CONTROL_CHAR : SHR_OK;
}

/* A cell of several code points made into `placed` as set_cell makes their UTF-8 (making may fail: the font's cluster
 * table, long text). */
static shr_status row_cluster(shr__tilemap *t, int32_t r, int32_t c, const shr_row *in, const shr_row_cell *e) {
    if (e->text > in->scalar_count || e->scalars > in->scalar_count - e->text) return SHR_E_INVALID_ARG;
    char u[SHR_CLUSTER_BYTES];
    size_t len = 0;
    for (size_t k = 0; k < e->scalars; k++) {
        uint32_t cp = in->scalars[e->text + k];
        uint64_t w;
        shr_status st = cp_check(cp);
        if (st != SHR_OK) return st;
        size_t l = cp_utf8(cp, &w);
        if (len + l <= shr__cluster_max_bytes) memcpy(u + len, &w, l);
        len += l;
    }
    if (len > shr__cluster_max_bytes) memcpy(u, SHR_REPLACEMENT_UTF8, 3), len = 3; /* as set_cell keeps it */
    shr__placed *d = shr__vec_push(&t->placed, &t->al);
    if (!d) return SHR_E_NO_MEMORY;
    d->row = r, d->col = c;
    shr_status st = cell_for(t, r, c, u, len, e->span, in->styles[e->style], &d->cell);
    if (st != SHR_OK) t->placed.len--;
    return st;
}

shr_status shr_pl_lyr_tilemap_set_row(shr_lyr *layer, int32_t row, int32_t col, const shr_row *in,
                                      shr_error_info *err) {
    shr__err_clear(err);
    shr__tilemap *t;
    shr_status st = tilemap_get(layer, &t);
    if (st != SHR_OK) return shr__fail(err, st, SIZE_MAX, SIZE_MAX, "not a tilemap layer or context busy");
    if (!in || (in->cell_count && !in->cells) || (in->style_count && !in->styles) || (in->scalar_count && !in->scalars))
        return shr__fail(err, SHR_E_INVALID_ARG, SIZE_MAX, SIZE_MAX, "missing array");
    if (row < 0 || row >= t->rows || col < 0 || col > t->cols)
        return shr__fail(err, SHR_E_INVALID_ARG, SIZE_MAX, SIZE_MAX, "position outside the grid");
    for (size_t s = 0; s < in->style_count; s++)
        if ((st = shr__style_check(&in->styles[s], err, s)) != SHR_OK) return st;
    if (in->lines && (st = lines_check(t, in->lines, in->line_count, err)) != SHR_OK) return st;
    if (in->lines && !lines_room(t, row_lines(t, row), in->line_count))
        return shr__fail(err, SHR_E_NO_MEMORY, SIZE_MAX, SIZE_MAX, "no memory");
    /* Cells of several code points are made first; the others take their glyph from the memo or the font without
     * allocating, so nothing after this loop fails. */
    t->placed.len = 0;
    int64_t c = col;
    size_t i = 0;
    for (; i < in->cell_count; i++) {
        const shr_row_cell *e = &in->cells[i];
        c += e->span;
        if (!e->span || e->style >= in->style_count || c > t->cols)
            st = SHR_E_INVALID_ARG;
        else if (e->scalars == 1)
            st = cp_check(e->text);
        else if (e->scalars > 1)
            st = row_cluster(t, row, (int32_t)(c - e->span), in, e);
        if (st != SHR_OK) break;
    }
    if (st != SHR_OK) {
        for (size_t k = 0; k < t->placed.len; k++) cell_free(&t->al, &SHR_VEC_AT(&t->placed, shr__placed, k)->cell);
        t->placed.len = 0;
        return shr__fail(err, st, SIZE_MAX, i, "invalid cell");
    }
    size_t made = 0;
    c = col;
    for (i = 0; i < in->cell_count; i++) {
        const shr_row_cell *e = &in->cells[i];
        int32_t at = (int32_t)c;
        c += e->span;
        if (e->scalars > 1) {
            cell_put(t, row, at, &SHR_VEC_AT(&t->placed, shr__placed, made++)->cell);
        } else {
            uint64_t w = 0;
            size_t len = e->scalars ? cp_utf8(e->text, &w) : 0;
            cell_set(t, row, at, (const char *)&w, len, e->span, in->styles[e->style]);
        }
    }
    t->placed.len = 0;
    if (in->lines) lines_set(t, row, in->lines, in->line_count);
    return SHR_OK;
}

typedef struct text_sink {
    shr__tilemap *t;
    int32_t row, col;
    const char *utf8;
    shr_text_style style;
    const shr_style_run *runs;
} text_sink;

/* TAB spaces and the in-grid part of a cluster the last column cuts become blank cells of its style. */
static shr_status text_emit(void *user, const shr__piece *p) {
    text_sink *k = user;
    shr__tilemap *t = k->t;
    int32_t r = k->row + p->row, c = k->col + p->column;
    shr_text_style s = p->style ? k->runs[p->style - 1].style : k->style;
    /* Past the byte limit also past the scalar limit (the generator keeps 4 bytes per scalar or more): a replacement. */
    bool cut = !p->cls || c + p->cells > t->cols, over = p->byte_length > shr__cluster_max_bytes;
    int32_t n = cut ? (p->cells < t->cols - c ? p->cells : t->cols - c) : p->cells > 0;
    for (int32_t i = 0; i < n; i++) {
        shr__placed *d = shr__vec_push(&t->placed, &t->al);
        if (!d) return SHR_E_NO_MEMORY;
        *d = (shr__placed){r, c + i, blank(s)};
        shr_status st = cut ? SHR_OK
                            : cell_make(t, over ? SHR_REPLACEMENT_UTF8 : k->utf8 + p->byte_offset, over ? 3 : p->byte_length,
                                        (uint32_t)p->cells, s, p->cps, p->n, p->cls, &d->cell);
        if (st != SHR_OK) {
            t->placed.len--;
            return st;
        }
    }
    return SHR_OK;
}

shr_status shr_pl_lyr_tilemap_set_text(shr_lyr *layer, int32_t row, int32_t col, const char *utf8, size_t length,
                                       shr_text_style style, const shr_style_run *runs, size_t run_count,
                                       uint32_t flags, shr_error_info *err) {
    shr__err_clear(err);
    shr__tilemap *t;
    shr_status st = tilemap_get(layer, &t);
    if (st != SHR_OK) return shr__fail(err, st, 0, SIZE_MAX, "not a tilemap layer or context busy");
    if (row < 0 || row >= t->rows || col < 0 || col >= t->cols)
        return shr__fail(err, SHR_E_INVALID_ARG, 0, SIZE_MAX, "position outside the grid");
    if (flags & ~(uint32_t)SHR_TEXT_WRAP) return shr__fail(err, SHR_E_INVALID_ARG, 0, SIZE_MAX, "unknown text flag");
    if ((st = shr__style_check(&style, err, SIZE_MAX)) != SHR_OK) return st;
    text_sink k = {t, row, col, utf8, style, runs};
    t->placed.len = 0;
    shr__layout_in in = {utf8, length, runs, run_count, (flags & SHR_TEXT_WRAP) != 0, t->cols - col, t->rows - row,
                         text_emit, &k};
    st = shr__layout(&in, NULL, NULL, err);
    for (size_t i = 0; i < t->placed.len; i++) {
        shr__placed *d = SHR_VEC_AT(&t->placed, shr__placed, i);
        if (st == SHR_OK)
            *place(t, d->row, d->col, d->cell.span) = d->cell, t->long_text |= d->cell.len > CELL_INLINE;
        else
            cell_free(&t->al, &d->cell);
    }
    return st;
}

/* Clears and scrolls read the flags and bg alone. */
static shr_status clear_style(shr_text_style style) {
    style.fg = 0;
    return shr__style_check(&style, NULL, 0);
}

shr_status shr_pl_lyr_tilemap_clear(shr_lyr *layer, int32_t row, int32_t col, int32_t rows, int32_t cols,
                                    shr_text_style style) {
    shr__tilemap *t;
    shr_status st = tilemap_get(layer, &t);
    if (st != SHR_OK) return st;
    if ((st = clear_style(style)) != SHR_OK) return st;
    if (row < 0 || col < 0 || rows < 0 || cols < 0 || (int64_t)row + rows > t->rows || (int64_t)col + cols > t->cols)
        return SHR_E_INVALID_ARG;
    for (int32_t r = row; r < row + rows; r++) {
        shr__lines *l = row_lines(t, r);
        if (!lines_room(t, l, l->n + lines_split(l, col, col + cols))) return SHR_E_NO_MEMORY;
    }
    const shr__cell e = blank((shr_text_style){0, style.bg, 0});
    for (int32_t r = row; r < row + rows; r++) {
        int32_t c0 = col, c1 = col + cols;
        clear_cells(t, r, &c0, &c1);
        for (int32_t c = c0; (style.bg >> 24) && c < c1; c++) row_at(t, r)[c] = e;
        lines_cut(row_lines(t, r), col, col + cols);
    }
    return SHR_OK;
}

static void reverse(uint32_t *a, int32_t lo, int32_t hi) {
    for (hi--; lo < hi; lo++, hi--) {
        uint32_t x = a[lo];
        a[lo] = a[hi], a[hi] = x;
    }
}

/* The rows trade places in `line` and their groups move with them: no cell is copied. Changed rows of the region are
 * built first, so the groups that move show what moves, also the rows an earlier scroll uncovered: scrolls between
 * submits then add up to one move. */
shr_status shr_pl_lyr_tilemap_scroll(shr_lyr *layer, int32_t top, int32_t bottom, int32_t n, shr_text_style style) {
    shr__tilemap *t;
    shr_status st = tilemap_get(layer, &t);
    if (st != SHR_OK) return st;
    if ((st = clear_style(style)) != SHR_OK) return st;
    if (top < 0 || top > bottom || bottom > t->rows) return SHR_E_INVALID_ARG;
    int32_t h = bottom - top;
    int64_t k = n < 0 ? -(int64_t)n : n;
    if (k >= h) return shr_pl_lyr_tilemap_clear(layer, top, 0, h, t->cols, style);
    if (!n || (st = build_rows(t, top, bottom)) != SHR_OK) return st;
    int32_t left = n > 0 ? n : h + n;
    reverse(t->line, top, top + left);
    reverse(t->line, top + left, bottom);
    reverse(t->line, top, bottom);
    shr__lyr_groups_shift(layer, (uint32_t)top, (uint32_t)bottom, -n,
                          (shr_rect){0, top * SHR_CELL_HEIGHT, t->cols * SHR_CELL_WIDTH, bottom * SHR_CELL_HEIGHT},
                          -n * SHR_CELL_HEIGHT);
    return shr_pl_lyr_tilemap_clear(layer, n > 0 ? bottom - n : top, 0, n > 0 ? n : -n, t->cols, style);
}
