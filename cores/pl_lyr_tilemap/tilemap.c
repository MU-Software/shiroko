#include "shr_bitmap_font.h"
#include "shr_hash.h"
#include "shr_text.h"

_Static_assert((int64_t)SHR_MAX_GRID * SHR_CELL_WIDTH <= INT32_MAX && (int64_t)SHR_MAX_GRID * SHR_CELL_HEIGHT <= INT32_MAX,
               "grid pixel extent must fit int32");
_Static_assert(SHR_MAX_GRID <= 1 << 15 && SHR_MAX_SPAN <= 256 && SHR_STYLE_KNOWN_FLAGS <= 0xFF,
               "a row key packs column, span - 1, flags and has_glyph into 32 bits");
_Static_assert(SHR_MAX_GRID <= UINT16_MAX, "row commands hold columns");

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

typedef struct shr__tilemap {
    shr__alloc al;
    shr_lyr *layer;
    shr_context *ctx;
    shr_pl_res_bitmap_font *font;
    int32_t rows, cols;
    bool has_bg, long_text; /* long_text: a cell got text beyond CELL_INLINE */
    shr_color bg;
    shr__cell *cells;
    uint32_t *line; /* per screen row: its row of `cells`, | DIRTY */
    shr__vec keys; /* uint64_t, the bytes a row's cache key hashes: two per head */
    shr__vec placed; /* shr__placed, set_text's cells until the text is valid, kept for the next call */
    shr__memo memo[1024];
} shr__tilemap;

static const char tilemap_kind;

/* The memo and the row keys are used every frame. */
enum { TILEMAP_KIND = SHR_ALLOC_DESCRIPTOR | SHR_ALLOC_HOT };

static shr__cell *row_at(const shr__tilemap *t, int32_t r) {
    return &t->cells[(size_t)(t->line[r] & ~DIRTY) * (size_t)t->cols];
}

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

/* One group per row, in row coordinates placed at the row's y: the backgrounds as runs of one colour, then per cell
 * glyph and decorations, which stay inside their cells, so a glyph lies on its cell's background alone. Only rows that
 * opaque backgrounds cover completely get a cache hint, since a cached row replaces what lies under it. The first pass
 * counts the commands, the second writes the runs and the cells' commands into their two parts of the group. */
static shr_status build_row(shr__tilemap *t, int32_t r) {
    const shr__cell *row = row_at(t, r);
    const shr__line_metrics lm = shr__bitmap_font_line_metrics();
    shr__res *res = shr__bitmap_font_res(t->font);
    t->keys.len = 0;
    if (!shr__vec_reserve(&t->keys, &t->al, 2 * (size_t)t->cols + 2)) return SHR_E_NO_MEMORY;
    uint64_t *keys = t->keys.data, *k = keys;
    *k++ = res->serial, *k++ = (uint64_t)t->has_bg << 32 | t->bg;
    int32_t covered = 0;
    size_t runs = 0, n = 0;
    bool was = false; /* the previous cell is filled with `was_color` */
    shr_color was_color = 0;
    for (int32_t c = 0; c < t->cols; c++) {
        const shr__cell *e = &row[c];
        if (e->kind == CELL_CONT) continue;
        bool is_head = e->kind == CELL_HEAD;
        uint32_t f = is_head ? e->flags : 0;
        bool filled = t->has_bg || (f & SHR_STYLE_BG);
        shr_color color = (f & SHR_STYLE_BG) ? e->bg : t->bg;
        runs += filled && !(was && color == was_color);
        was = filled, was_color = color;
        if (!is_head) continue;
        covered += filled ? e->span : 0;
        k[0] = (uint64_t)e->fg << 32 | e->bg;
        k[1] = (uint64_t)e->glyph << 32 | (uint32_t)c << 17 | (uint32_t)(e->span - 1) << 9 | f << 1 | e->has_glyph, k += 2;
        n += !(f & SHR_STYLE_CONCEAL) * (e->has_glyph + !!(f & SHR_STYLE_UNDERLINE) + !!(f & SHR_STYLE_STRIKE));
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
        uint32_t f = is_head ? e->flags : 0;
        bool filled = c < t->cols && (t->has_bg || (f & SHR_STYLE_BG));
        shr_color color = (f & SHR_STYLE_BG) ? e->bg : t->bg;
        bool same = was && filled && color == was_color;
        if (was && !same) put(fills++, SHR__LCMD_FILL, 0, run, c, 0, SHR_CELL_HEIGHT, was_color);
        if (filled && !same) run = c;
        was = filled, was_color = color;
        if (!is_head || (f & SHR_STYLE_CONCEAL)) continue;
        shr_color bg = (f & SHR_STYLE_BG) ? e->bg : t->bg; /* 0 without one */
        uint32_t fx = ((f & SHR_STYLE_DIM) ? SHR__LCMD_DIM : 0) | ((f & SHR_STYLE_BLINK) ? SHR__LCMD_BLINK : 0);
        uint32_t gx = fx | ((f & SHR_STYLE_BOLD) ? SHR__LCMD_BOLD : 0) | ((f & SHR_STYLE_ITALIC) ? SHR__LCMD_ITALIC : 0);
        gx |= (t->has_bg || (f & SHR_STYLE_BG)) ? SHR__LCMD_ON_FILL : 0; /* glyph flags: not on the lines */
        int32_t c1 = c + e->span;
        if (e->has_glyph) {
            put(out, SHR__LCMD_GLYPH, gx, c, c1, 0, SHR_CELL_HEIGHT, e->fg);
            out->id = e->glyph, out->bg = bg, out++;
        }
        if (f & SHR_STYLE_UNDERLINE) put(out++, SHR__LCMD_FILL, fx, c, c1, lm.underline_y, lm.underline_y + 1, e->fg);
        if (f & SHR_STYLE_STRIKE) put(out++, SHR__LCMD_FILL, fx, c, c1, lm.strike_y, lm.strike_y + 1, e->fg);
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
    SHR_FREE_ARRAY(&t->al, t->cells, shr__cell, (size_t)t->rows * (size_t)t->cols);
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
    uint32_t *line = SHR_NEW_HOT_ARRAY(al, uint32_t, (size_t)rows);
    shr_status st = cells && line ? SHR_OK : SHR_E_NO_MEMORY;
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
                cluster_decode(cell_text(d), d->len, cps, &n); /* validated when it was set */
                shr__classify(cps, n, &cls);
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
            if ((st = shr__lyr_attach(layer, &tilemap_kind, t, tilemap_destroy, flush)) != SHR_OK)
                shr__free(al, t, sizeof(*t), SHR_ALIGNOF(shr__tilemap), TILEMAP_KIND), t = NULL;
            else
                res->users++;
        }
    }
    if (st != SHR_OK) {
        SHR_FREE_ARRAY(al, cells, shr__cell, (size_t)rows * (size_t)cols);
        SHR_FREE_HOT_ARRAY(al, line, uint32_t, (size_t)rows);
        return st;
    }
    for (int32_t r = 0; r < t->rows; r++)
        for (int32_t c = 0; c < t->cols; c++) {
            shr__cell *e = &row_at(t, r)[c];
            if (kept(e, r, c, rows, cols)) *e = (shr__cell){0}; /* now owned by the new grid */
        }
    free_grid(t);
    if (font != t->font) {
        memset(t->memo, 0, sizeof(t->memo)); /* cluster ids are the font's */
        shr__bitmap_font_res(t->font)->users--;
        res->users++;
        t->font = font;
    }
    t->rows = rows, t->cols = cols, t->cells = cells, t->line = line;
    t->has_bg = background != NULL;
    t->bg = background ? *background : 0;
    for (int32_t r = 0; r < rows; r++) line[r] = (uint32_t)r | DIRTY;
    shr__lyr_groups_clear(layer, 2 * (size_t)cols + 2); /* cannot fail: the context was checked above */
    return SHR_OK;
}

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "memo keys are cell text bytes read as words");

shr_status shr_pl_lyr_tilemap_set_cell(shr_lyr *layer, int32_t row, int32_t col, const char *utf8, size_t length,
                                       uint32_t span, shr_text_style style) {
    shr__tilemap *t;
    shr_status st = tilemap_get(layer, &t);
    if (st != SHR_OK) return st;
    if ((length && !utf8) || span == 0) return SHR_E_INVALID_ARG;
    if (style.flags & ~SHR_STYLE_KNOWN_FLAGS) return SHR_E_UNKNOWN_STYLE;
    if (length > shr__cluster_max_bytes || span > SHR_MAX_SPAN) return SHR_E_LIMIT;
    if (row < 0 || row >= t->rows || col < 0 || (int64_t)col + span > t->cols) return SHR_E_INVALID_ARG;
    uint64_t key = 0, was;
    shr__memo *m = NULL;
    if (length && length <= 8) {
        /* The text zero-padded in 32-bit halves, without reads past the end: the first 4 bytes and the ones after them
         * as words, and bytes 0, length / 2 and length - 1 of up to 3 (from 4 bytes on, ones the first word holds). */
        static const uint8_t none[4];
        const uint8_t *u = (const uint8_t *)utf8;
        size_t h = (length >> 1) & 3, l = (length - 1) & 3;
        uint32_t lo, hi;
        memcpy(&lo, length >= 4 ? u : none, 4), memcpy(&hi, length > 4 ? u + length - 4 : none, 4);
        lo |= u[0] | (uint32_t)u[h] << 8 * h | (uint32_t)u[l] << 8 * l;
        hi >>= 8 * (8 - length) & 31;
        key = lo | (uint64_t)hi << 32;
        m = &t->memo[(uint64_t)(lo ^ hi) * 0x9E3779B9u >> 22 & 1023];
    }
    const shr__cell *old = &row_at(t, row)[col];
    memcpy(&was, old->text, sizeof(was));
    bool same = (old->kind == CELL_HEAD) & (old->span == span) & (old->len == length) & (old->fg == style.fg) &
                (old->bg == style.bg) & (old->flags == style.flags);
    if (same && (length <= 8 ? was == key : !memcmp(cell_text(old), utf8, length))) return SHR_OK; /* unchanged */
    if (m && m->len == length && m->key == key) {
        shr__cell *e = place(t, row, col, span);
        *e = head(style, m->glyph, length, span, m->has_glyph);
        memcpy(e->text, &key, sizeof(key));
        return SHR_OK;
    }
    uint32_t cps[SHR_CLUSTER_SCALARS];
    size_t n;
    shr__cluster_class cls;
    shr__cell e;
    if ((st = cluster_decode(utf8, length, cps, &n)) != SHR_OK) return st;
    shr__classify(cps, n, &cls);
    if ((st = cell_make(t, utf8, length, span, style, cps, n, &cls, &e)) != SHR_OK) return st;
    if (m) *m = (shr__memo){key, e.glyph, (uint8_t)length, e.has_glyph};
    t->long_text |= length > CELL_INLINE;
    *place(t, row, col, span) = e;
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
    bool cut = !p->cls || c + p->cells > t->cols;
    int32_t n = cut ? (p->cells < t->cols - c ? p->cells : t->cols - c) : p->cells > 0;
    for (int32_t i = 0; i < n; i++) {
        shr__placed *d = shr__vec_push(&t->placed, &t->al);
        if (!d) return SHR_E_NO_MEMORY;
        *d = (shr__placed){r, c + i, blank(s)};
        shr_status st = cut ? SHR_OK
                            : cell_make(t, k->utf8 + p->byte_offset, p->byte_length, (uint32_t)p->cells, s, p->cps, p->n,
                                        p->cls, &d->cell);
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
        for (int32_t c = c0; bg && c < c1; c++) row_at(t, r)[c] = e;
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
    if (style.flags & ~SHR_STYLE_KNOWN_FLAGS) return SHR_E_UNKNOWN_STYLE;
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
