#include "shr_bitmap_font.h"
#include "shr_hash.h"
#include "shr_text.h"

_Static_assert((int64_t)SHR_MAX_GRID * SHR_CELL_WIDTH <= INT32_MAX && (int64_t)SHR_MAX_GRID * SHR_CELL_HEIGHT <= INT32_MAX,
               "grid pixel extent must fit int32");

enum { CELL_EMPTY = 0, CELL_HEAD, CELL_CONT };
#define CELL_INLINE 16

typedef struct shr__cell {
    shr_text_style style;
    uint64_t glyph;
    union {
        char in[CELL_INLINE];
        char *heap;
    } text;
    uint16_t len;
    uint16_t span; /* HEAD: cells covered; CONT: distance back to the head */
    uint8_t kind;
    bool has_glyph;
} shr__cell;

typedef struct shr__tilemap {
    shr__alloc al;
    shr_lyr *layer;
    shr_pl_res_bitmap_font *font;
    int32_t rows, cols;
    bool has_bg;
    shr_color bg;
    shr__cell *cells;
    uint8_t *dirty;
    shr__vec cmds; /* shr__lcmd, reused per row */
    shr__vec keys; /* uint64_t, the bytes a row's cache key hashes */
} shr__tilemap;

static const char tilemap_kind;

static const char *cell_text(const shr__cell *e) { return e->len > CELL_INLINE ? e->text.heap : e->text.in; }

static void cell_free(const shr__alloc *al, shr__cell *e) {
    if (e->kind == CELL_HEAD && e->len > CELL_INLINE) shr__free(al, e->text.heap, e->len, 1, SHR_ALLOC_PAYLOAD);
    *e = (shr__cell){0};
}

/* Validates one cluster as set_cell receives it. */
static shr_status cluster_decode(const char *utf8, size_t len, uint32_t *cps, size_t *n) {
    size_t pos = 0;
    *n = 0;
    while (pos < len) {
        uint32_t cp;
        if (shr__utf8_next((const uint8_t *)utf8, len, &pos, &cp)) return SHR_E_INVALID_UTF8;
        if (shr__is_control(cp)) return SHR_E_CONTROL_CHAR;
        if (*n == shr__cluster_max_scalars) return SHR_E_LIMIT;
        cps[(*n)++] = cp;
    }
    return SHR_OK;
}

static shr_status cell_glyph(shr_pl_res_bitmap_font *font, shr__cell *e, const uint32_t *cps, size_t n,
                             const shr__cluster_class *cls) {
    e->has_glyph = false;
    if (!n) return SHR_OK;
    shr_status st = shr__bitmap_font_glyph(font, cps, n, cls, &e->glyph);
    e->has_glyph = st == SHR_OK;
    return st == SHR_E_NOT_FOUND ? SHR_OK : st;
}

/* `utf8` is a valid cluster within the cell limits, decoded into `cps` and classified as `cls`. */
static shr_status cell_make(shr__tilemap *t, const char *utf8, size_t len, uint32_t span, shr_text_style style,
                            const uint32_t *cps, size_t n, const shr__cluster_class *cls, shr__cell *out) {
    *out = (shr__cell){style, 0, {{0}}, (uint16_t)len, (uint16_t)span, CELL_HEAD, false};
    char *dst = out->text.in;
    if (len > CELL_INLINE && !(dst = out->text.heap = shr__malloc(&t->al, len, 1, SHR_ALLOC_PAYLOAD)))
        return SHR_E_NO_MEMORY;
    if (len) memcpy(dst, utf8, len);
    shr_status st = cell_glyph(t->font, out, cps, n, cls);
    if (st != SHR_OK) cell_free(&t->al, out);
    return st;
}

static shr__cell blank(shr_text_style style) { return (shr__cell){style, 0, {{0}}, 0, 1, CELL_HEAD, false}; }

/* Clears [*c0, *c1) of row r widened to the wide cells reaching into it. */
static void clear_cells(shr__tilemap *t, int32_t r, int32_t *c0, int32_t *c1) {
    if (*c0 >= *c1) return;
    shr__cell *row = &t->cells[(size_t)r * (size_t)t->cols];
    if (row[*c0].kind == CELL_CONT) *c0 -= row[*c0].span;
    int32_t last = *c1 - 1;
    if (row[last].kind == CELL_CONT) last -= row[last].span;
    if (row[last].kind == CELL_HEAD && last + row[last].span > *c1) *c1 = last + row[last].span;
    for (int32_t c = *c0; c < *c1; c++) cell_free(&t->al, &row[c]);
    t->dirty[r] = 1;
}

static void place(shr__tilemap *t, int32_t r, int32_t c, const shr__cell *e) {
    int32_t c0 = c, c1 = c + e->span;
    clear_cells(t, r, &c0, &c1);
    shr__cell *row = &t->cells[(size_t)r * (size_t)t->cols];
    row[c] = *e;
    for (uint16_t k = 1; k < e->span; k++) row[c + k] = (shr__cell){.span = k, .kind = CELL_CONT};
}

/* One group per row: the backgrounds as runs of one colour, then per cell glyph and decorations, which stay inside
 * their cells. Only rows that opaque backgrounds cover completely get a cache hint, since a cached row replaces what
 * lies under it. */
static shr_status build_row(shr__tilemap *t, int32_t r) {
    const shr__cell *row = &t->cells[(size_t)r * (size_t)t->cols];
    const shr__line_metrics lm = shr__bitmap_font_line_metrics();
    shr__res *res = shr__bitmap_font_res(t->font);
    const int32_t y = r * SHR_CELL_HEIGHT;
    size_t most = 4 * (size_t)t->cols + 2; /* cache pair, a background and three commands per cell */
    t->cmds.len = t->keys.len = 0;
    if (!shr__vec_reserve(&t->cmds, &t->al, most) || !shr__vec_reserve(&t->keys, &t->al, most))
        return SHR_E_NO_MEMORY;
    shr__lcmd *cmds = t->cmds.data, *out = cmds + 1;
    uint64_t *keys = t->keys.data, *k = keys;
    *k++ = res->serial, *k++ = (uint64_t)t->has_bg << 32 | t->bg;
    int32_t covered = 0, run = -1;
    shr_color run_color = 0;
    for (int32_t c = 0; c <= t->cols; c++) {
        const shr__cell *e = &row[c < t->cols ? c : 0];
        if (c < t->cols && e->kind == CELL_CONT) continue;
        bool head = c < t->cols && e->kind == CELL_HEAD, bg = head && (e->style.flags & SHR_STYLE_BG);
        bool filled = c < t->cols && (bg || t->has_bg);
        shr_color color = bg ? e->style.bg : t->bg;
        if (run >= 0 && (!filled || color != run_color))
            *out++ = (shr__lcmd){SHR__LCMD_FILL, 0, {run * SHR_CELL_WIDTH, y, c * SHR_CELL_WIDTH, y + SHR_CELL_HEIGHT},
                                 {0, 0}, run_color, NULL, 0, {0}},
            run = -1;
        if (filled && run < 0) run = c, run_color = color;
        if (!head) continue;
        covered += filled ? e->span : 0;
        k[0] = (uint64_t)c << 32 | e->span, k[1] = (uint64_t)e->style.fg << 32 | e->style.bg;
        k[2] = (uint64_t)e->style.flags << 1 | e->has_glyph, k[3] = e->glyph, k += 4;
    }
    for (int32_t c = 0; c < t->cols; c++) {
        const shr__cell *e = &row[c];
        uint32_t f = e->style.flags;
        if (e->kind != CELL_HEAD || (f & SHR_STYLE_CONCEAL)) continue;
        uint32_t fx = ((f & SHR_STYLE_DIM) ? SHR__LCMD_DIM : 0) | ((f & SHR_STYLE_BLINK) ? SHR__LCMD_BLINK : 0);
        uint32_t gx = fx | ((f & SHR_STYLE_BOLD) ? SHR__LCMD_BOLD : 0) |
                      ((f & SHR_STYLE_ITALIC) ? SHR__LCMD_ITALIC : 0); /* glyph styles: not on the lines */
        shr_rect dst = {c * SHR_CELL_WIDTH, y, (c + e->span) * SHR_CELL_WIDTH, y + SHR_CELL_HEIGHT};
        if (e->has_glyph)
            *out++ = (shr__lcmd){SHR__LCMD_GLYPH, gx, dst, {dst.x0, dst.y0}, e->style.fg, res, e->glyph, {0}};
        if (f & SHR_STYLE_UNDERLINE)
            *out++ = (shr__lcmd){SHR__LCMD_FILL, fx, {dst.x0, y + lm.underline_y, dst.x1, y + lm.underline_y + 1},
                                 {0, 0}, e->style.fg, NULL, 0, {0}};
        if (f & SHR_STYLE_STRIKE)
            *out++ = (shr__lcmd){SHR__LCMD_FILL, fx, {dst.x0, y + lm.strike_y, dst.x1, y + lm.strike_y + 1},
                                 {0, 0}, e->style.fg, NULL, 0, {0}};
    }
    size_t n = (size_t)(out - cmds);
    if (n == 1) return shr__lyr_group_set(t->layer, (uint32_t)r, NULL, 0);
    if (covered < t->cols && !t->has_bg) return shr__lyr_group_set(t->layer, (uint32_t)r, cmds + 1, n - 1);
    XXH128_hash_t h = XXH3_128bits(keys, (size_t)(k - keys) * sizeof(*keys));
    cmds[0] = (shr__lcmd){SHR__LCMD_CACHE_BEGIN, 0, {0, y, t->cols * SHR_CELL_WIDTH, y + SHR_CELL_HEIGHT},
                          {0, 0}, 0, NULL, 0, {h.low64, h.high64}};
    *out = (shr__lcmd){.kind = SHR__LCMD_CACHE_END};
    return shr__lyr_group_set(t->layer, (uint32_t)r, cmds, n + 1);
}

/* Changed rows are rebuilt once per submit; rows that fail stay dirty for the next one. */
static shr_status flush(void *state) {
    shr__tilemap *t = state;
    for (int32_t r = 0; r < t->rows; r++) {
        if (!t->dirty[r]) continue;
        shr_status st = build_row(t, r);
        if (st != SHR_OK) return st;
        t->dirty[r] = 0;
    }
    return SHR_OK;
}

static void free_grid(shr__tilemap *t) {
    for (size_t i = 0; i < (size_t)t->rows * (size_t)t->cols; i++) cell_free(&t->al, &t->cells[i]);
    SHR_FREE_ARRAY(&t->al, t->cells, shr__cell, (size_t)t->rows * (size_t)t->cols);
    SHR_FREE_ARRAY(&t->al, t->dirty, uint8_t, (size_t)t->rows);
}

static void tilemap_destroy(void *state) {
    shr__tilemap *t = state;
    shr__alloc al = t->al;
    free_grid(t);
    shr__vec_free(&t->cmds, &al);
    shr__vec_free(&t->keys, &al);
    shr__bitmap_font_res(t->font)->users--;
    SHR_DELETE(&al, t, shr__tilemap);
}

static shr_status tilemap_get(shr_lyr *layer, shr__tilemap **out) {
    if (!layer) return SHR_E_INVALID_ARG;
    if (!(*out = shr__lyr_state(layer, &tilemap_kind))) return SHR_E_STATE;
    return shr__ctx_refused(shr__lyr_ctx(layer)) ? SHR_E_STATE : SHR_OK;
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
    uint8_t *dirty = SHR_NEW_ARRAY(al, uint8_t, (size_t)rows);
    shr_status st = cells && dirty ? SHR_OK : SHR_E_NO_MEMORY;
    /* Kept heads are copied (sharing their text) so the old grid stays intact until everything succeeded. */
    for (int32_t r = 0; t && st == SHR_OK && r < t->rows; r++)
        for (int32_t c = 0; st == SHR_OK && c < t->cols; c++) {
            const shr__cell *e = &t->cells[(size_t)r * (size_t)t->cols + (size_t)c];
            if (!kept(e, r, c, rows, cols)) continue;
            shr__cell *d = &cells[(size_t)r * (size_t)cols + (size_t)c];
            *d = *e;
            for (uint16_t k = 1; k < e->span; k++) d[k] = (shr__cell){.span = k, .kind = CELL_CONT};
            if (font != t->font) {
                uint32_t cps[SHR_CLUSTER_SCALARS];
                size_t n;
                shr__cluster_class cls;
                cluster_decode(cell_text(d), d->len, cps, &n); /* validated when it was set */
                shr__classify(cps, n, &cls);
                st = cell_glyph(font, d, cps, n, &cls);
            }
        }
    if (st == SHR_OK && !t) {
        if (!(t = SHR_NEW(al, shr__tilemap))) {
            st = SHR_E_NO_MEMORY;
        } else {
            *t = (shr__tilemap){.al = *al, .layer = layer, .font = font};
            SHR_VEC_INIT(&t->cmds, shr__lcmd);
            SHR_VEC_INIT(&t->keys, uint64_t);
            if ((st = shr__lyr_attach(layer, &tilemap_kind, t, tilemap_destroy, flush)) != SHR_OK)
                SHR_DELETE(al, t, shr__tilemap), t = NULL;
            else
                res->users++;
        }
    }
    if (st != SHR_OK) {
        SHR_FREE_ARRAY(al, cells, shr__cell, (size_t)rows * (size_t)cols);
        SHR_FREE_ARRAY(al, dirty, uint8_t, (size_t)rows);
        return st;
    }
    for (int32_t r = 0; r < t->rows; r++)
        for (int32_t c = 0; c < t->cols; c++) {
            shr__cell *e = &t->cells[(size_t)r * (size_t)t->cols + (size_t)c];
            if (kept(e, r, c, rows, cols)) *e = (shr__cell){0}; /* now owned by the new grid */
        }
    free_grid(t);
    if (font != t->font) {
        shr__bitmap_font_res(t->font)->users--;
        res->users++;
        t->font = font;
    }
    t->rows = rows, t->cols = cols, t->cells = cells, t->dirty = dirty;
    t->has_bg = background != NULL;
    t->bg = background ? *background : 0;
    memset(dirty, 1, (size_t)rows);
    shr__lyr_groups_clear(layer); /* cannot fail: the context was checked above */
    return SHR_OK;
}

shr_status shr_pl_lyr_tilemap_set_cell(shr_lyr *layer, int32_t row, int32_t col, const char *utf8, size_t length,
                                       uint32_t span, shr_text_style style) {
    shr__tilemap *t;
    shr_status st = tilemap_get(layer, &t);
    if (st != SHR_OK) return st;
    if ((length && !utf8) || span == 0) return SHR_E_INVALID_ARG;
    if (style.flags & ~SHR_STYLE_KNOWN_FLAGS) return SHR_E_UNKNOWN_STYLE;
    if (length > shr__cluster_max_bytes || span > SHR_MAX_SPAN) return SHR_E_LIMIT;
    if (row < 0 || row >= t->rows || col < 0 || (int64_t)col + span > t->cols) return SHR_E_INVALID_ARG;
    uint32_t cps[SHR_CLUSTER_SCALARS];
    size_t n;
    shr__cluster_class cls;
    shr__cell e;
    if ((st = cluster_decode(utf8, length, cps, &n)) != SHR_OK) return st;
    shr__classify(cps, n, &cls);
    if ((st = cell_make(t, utf8, length, span, style, cps, n, &cls, &e)) != SHR_OK) return st;
    place(t, row, col, &e);
    return SHR_OK;
}

typedef struct shr__placed {
    int32_t row, col;
    shr__cell cell;
} shr__placed;

typedef struct text_sink {
    shr__tilemap *t;
    int32_t row, col;
    const char *utf8;
    shr_text_style style;
    const shr_style_run *runs;
    shr__vec placed;
} text_sink;

/* TAB spaces and the in-grid part of a cluster the last column cuts become blank cells of its style. */
static shr_status text_emit(void *user, const shr__piece *p) {
    text_sink *k = user;
    shr__tilemap *t = k->t;
    int32_t r = k->row + p->row, c = k->col + p->column;
    shr_text_style s = p->style ? k->runs[p->style - 1].style : k->style;
    bool cut = !p->cls || c + p->cells > t->cols;
    int32_t n = cut ? (p->cells < t->cols - c ? p->cells : t->cols - c) : p->cells > 0;
    for (int32_t i = 0; i < n; i++) {
        shr__placed *d = shr__vec_push(&k->placed, &t->al);
        if (!d) return SHR_E_NO_MEMORY;
        *d = (shr__placed){r, c + i, blank(s)};
        shr_status st = cut ? SHR_OK
                            : cell_make(t, k->utf8 + p->byte_offset, p->byte_length, (uint32_t)p->cells, s, p->cps, p->n,
                                        p->cls, &d->cell);
        if (st != SHR_OK) {
            k->placed.len--;
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
    text_sink k = {t, row, col, utf8, style, runs, {0}};
    SHR_VEC_INIT(&k.placed, shr__placed);
    shr__layout_in in = {utf8, length, runs, run_count, (flags & SHR_TEXT_WRAP) != 0, t->cols - col, t->rows - row,
                         text_emit, &k};
    st = shr__layout(&in, NULL, NULL, err);
    for (size_t i = 0; i < k.placed.len; i++) {
        shr__placed *d = SHR_VEC_AT(&k.placed, shr__placed, i);
        if (st == SHR_OK)
            place(t, d->row, d->col, &d->cell);
        else
            cell_free(&t->al, &d->cell);
    }
    shr__vec_free(&k.placed, &t->al);
    return st;
}

shr_status shr_pl_lyr_tilemap_clear(shr_lyr *layer, int32_t row, int32_t col, int32_t rows, int32_t cols,
                                    shr_text_style style) {
    shr__tilemap *t;
    shr_status st = tilemap_get(layer, &t);
    if (st != SHR_OK) return st;
    if (style.flags & ~SHR_STYLE_KNOWN_FLAGS) return SHR_E_UNKNOWN_STYLE;
    if (row < 0 || col < 0 || rows < 0 || cols < 0 || (int64_t)row + rows > t->rows || (int64_t)col + cols > t->cols)
        return SHR_E_INVALID_ARG;
    bool bg = (style.flags & SHR_STYLE_BG) != 0;
    const shr__cell e = blank((shr_text_style){0, style.bg, SHR_STYLE_BG});
    for (int32_t r = row; r < row + rows; r++) {
        int32_t c0 = col, c1 = col + cols;
        clear_cells(t, r, &c0, &c1);
        for (int32_t c = c0; bg && c < c1; c++) t->cells[(size_t)r * (size_t)t->cols + (size_t)c] = e;
    }
    return SHR_OK;
}
