#include <string.h>

#include "shr_text.h"

const char *shr__utf8_next(const uint8_t *s, size_t len, size_t *pos, uint32_t *cp) {
    size_t i = *pos, need;
    uint8_t b0 = s[i];
    uint32_t v, min;
    if (b0 < 0x80) {
        *cp = b0;
        *pos = i + 1;
        return NULL;
    }
    if (b0 >= 0xC2 && b0 <= 0xDF)
        need = 1, v = b0 & 0x1F, min = 0x80;
    else if (b0 >= 0xE0 && b0 <= 0xEF)
        need = 2, v = b0 & 0x0F, min = 0x800;
    else if (b0 >= 0xF0 && b0 <= 0xF4)
        need = 3, v = b0 & 0x07, min = 0x10000;
    else if (b0 == 0xC0 || b0 == 0xC1)
        return "overlong encoding";
    else if (b0 <= 0xBF)
        return "unexpected continuation byte";
    else
        return "code point out of range";
    for (size_t k = 1; k <= need; k++) {
        if (i + k >= len || (s[i + k] & 0xC0) != 0x80) return "truncated sequence";
        v = (v << 6) | (s[i + k] & 0x3F);
    }
    if (v < min) return "overlong encoding";
    if (v >= 0xD800 && v <= 0xDFFF) return "surrogate code point";
    if (v > 0x10FFFF) return "code point out of range";
    *cp = v;
    *pos = i + need + 1;
    return NULL;
}

/* UAX #29 extended grapheme clusters, one scalar at a time. */
typedef struct gb_state {
    unsigned prev;
    int emo;  /* GB11: 1 after ExtPict Extend*, 2 after ZWJ */
    int incb; /* GB9c: 1 after Consonant, 2 after Linker */
    size_t ri;
} gb_state;

static void gb_start(gb_state *g, uint32_t cp) {
    uint32_t pp = shr__uprops(cp);
    g->prev = shr__gcb(pp);
    g->emo = (pp & SHR_UP_EXTPICT) ? 1 : 0;
    g->incb = shr__incb(pp) == SHR_INCB_CONSONANT ? 1 : 0;
    g->ri = g->prev == SHR_GCB_RI;
}

/* Whether `c` continues the cluster; the state advances when it does. */
static bool gb_join(gb_state *g, uint32_t c) {
    uint32_t cp = shr__uprops(c);
    unsigned prev = g->prev, cur = shr__gcb(cp);
    bool join;
    if (prev == SHR_GCB_CR && cur == SHR_GCB_LF)
        join = true;
    else if (prev == SHR_GCB_CONTROL || prev == SHR_GCB_CR || prev == SHR_GCB_LF || cur == SHR_GCB_CONTROL ||
             cur == SHR_GCB_CR || cur == SHR_GCB_LF)
        join = false;
    else if (prev == SHR_GCB_L && (cur == SHR_GCB_L || cur == SHR_GCB_V || cur == SHR_GCB_LV || cur == SHR_GCB_LVT))
        join = true;
    else if ((prev == SHR_GCB_LV || prev == SHR_GCB_V) && (cur == SHR_GCB_V || cur == SHR_GCB_T))
        join = true;
    else if ((prev == SHR_GCB_LVT || prev == SHR_GCB_T) && cur == SHR_GCB_T)
        join = true;
    else
        join = cur == SHR_GCB_EXTEND || cur == SHR_GCB_ZWJ || cur == SHR_GCB_SPACINGMARK || prev == SHR_GCB_PREPEND ||
               (shr__incb(cp) == SHR_INCB_CONSONANT && g->incb == 2) ||
               (prev == SHR_GCB_ZWJ && (cp & SHR_UP_EXTPICT) && g->emo == 2) ||
               (prev == SHR_GCB_RI && cur == SHR_GCB_RI && g->ri % 2 == 1);
    if (!join) return false;
    int emo = g->emo;
    g->emo = (cp & SHR_UP_EXTPICT) ? 1 : (emo == 1 && cur == SHR_GCB_EXTEND) ? 1 : (emo == 1 && cur == SHR_GCB_ZWJ) ? 2 : 0;
    unsigned ib = shr__incb(cp);
    g->incb = ib == SHR_INCB_CONSONANT ? 1 : (g->incb && ib == SHR_INCB_LINKER) ? 2 : (g->incb && ib == SHR_INCB_EXTEND) ? g->incb : 0;
    g->ri = cur == SHR_GCB_RI ? g->ri + 1 : 0;
    g->prev = cur;
    return true;
}

static shr_status next_scalar(const shr__layout_in *in, size_t *pos, uint32_t *cp, shr_error_info *err) {
    size_t at = *pos;
    const char *why = shr__utf8_next((const uint8_t *)in->utf8, in->len, pos, cp);
    if (why) return shr__fail(err, SHR_E_INVALID_UTF8, at, SIZE_MAX, why);
    if (shr__is_control(*cp) && *cp != '\t' && *cp != '\n' && *cp != '\r')
        return shr__fail(err, SHR_E_CONTROL_CHAR, at, SIZE_MAX, "control character");
    return SHR_OK;
}

static shr_status runs_check(const shr__layout_in *in, shr_error_info *err) {
    /* Runs are non-empty, so at most one per byte. */
    if (in->run_count && (!in->runs || in->run_count > in->len))
        return shr__fail(err, SHR_E_INVALID_ARG, 0, SIZE_MAX, "invalid style runs");
    size_t prev_end = 0;
    for (size_t r = 0; r < in->run_count; r++) {
        const shr_style_run *run = &in->runs[r];
        if (run->byte_start >= run->byte_end || run->byte_end > in->len || run->byte_start < prev_end)
            return shr__fail(err, SHR_E_INVALID_ARG, run->byte_start, r, "style runs must be non-empty, sorted and disjoint");
        shr_status st = shr__style_check(&run->style, err, r);
        if (st != SHR_OK) return st;
        prev_end = run->byte_end;
    }
    return SHR_OK;
}

/* Run edges (start, end, start, ...) are non-decreasing; each must fall on a cluster start or the end. */
static size_t edge_at(const shr__layout_in *in, size_t e) {
    return e & 1 ? in->runs[e / 2].byte_end : in->runs[e / 2].byte_start;
}

static shr_status edges_reach(const shr__layout_in *in, size_t *edge, size_t off, shr_error_info *err) {
    for (; *edge < 2 * in->run_count && edge_at(in, *edge) <= off; ++*edge)
        if (edge_at(in, *edge) < off)
            return shr__fail(err, SHR_E_STYLE_BOUNDARY, edge_at(in, *edge), *edge / 2, "style boundary inside a cluster");
    return SHR_OK;
}

shr_status shr__layout(const shr__layout_in *in, int32_t *out_rows, int32_t *out_cols, shr_error_info *err) {
    if (in->len > SHR_MAX_TEXT_BYTES) return shr__fail(err, SHR_E_LIMIT, 0, SIZE_MAX, "text too long");
    if (in->len && !in->utf8) return shr__fail(err, SHR_E_INVALID_ARG, 0, SIZE_MAX, "null text");
    shr_status st = runs_check(in, err);
    if (st != SHR_OK) return st;
    const bool clip = in->avail_rows > 0;
    uint32_t cps[SHR_CLUSTER_SCALARS], next = 0;
    size_t pos = 0, next_off = 0, edge = 0, cursor = 0;
    bool pending = false, done = false; /* done: the rest lies below the grid */
    int64_t row = 0, col = 0, max_col = 0;
    while (pending || pos < in->len) {
        size_t boff = pending ? next_off : pos, n = 1;
        if (!pending && (st = next_scalar(in, &pos, &next, err)) != SHR_OK) return st;
        cps[0] = next;
        pending = false;
        gb_state g;
        gb_start(&g, next);
        while (pos < in->len) {
            next_off = pos;
            if ((st = next_scalar(in, &pos, &next, err)) != SHR_OK) return st;
            if (!gb_join(&g, next)) {
                pending = true;
                break;
            }
            if (n < shr__cluster_max_scalars) cps[n] = next;
            n++;
        }
        size_t blen = (pending ? next_off : pos) - boff;
        if ((st = edges_reach(in, &edge, boff, err)) != SHR_OK) return st;
        while (cursor < in->run_count && in->runs[cursor].byte_end <= boff) cursor++;
        size_t kept = n < shr__cluster_max_scalars ? n : shr__cluster_max_scalars;
        shr__piece p = {boff, blen, 0, 0, 0, 0, 0, cps, kept, NULL};
        p.style = cursor < in->run_count && in->runs[cursor].byte_start <= boff ? (uint32_t)cursor + 1 : 0;
        bool nl = cps[0] == '\n' || cps[0] == '\r', tab = cps[0] == '\t';
        shr__cluster_class cls = {0};
        if (!nl && !tab) shr__classify_cluster(cps, n, &cls);
        if (in->wrap && (tab || cls.cells > 0)) {
            if (in->avail_cols <= 0) return shr__fail(err, SHR_E_WRAP_NO_SPACE, boff, SIZE_MAX, "no columns available");
            if (cls.cells > in->avail_cols)
                return shr__fail(err, SHR_E_CLUSTER_TOO_WIDE, boff, SIZE_MAX, "cluster wider than the available columns");
        }
        if (done || (clip && !nl && !in->wrap && col >= in->avail_cols)) continue;
        int64_t spaces = tab ? (col / shr__tab_stop + 1) * shr__tab_stop - col : cls.cells;
        p.flags = nl ? SHR_CLUSTER_NEWLINE : tab ? SHR_CLUSTER_TAB : cls.flags;
        p.cls = nl || tab ? NULL : &cls;
        do { /* a TAB continues across wraps, a cluster moves whole */
            int64_t take = spaces;
            if (in->wrap && spaces > 0) {
                if (col + (tab ? 1 : spaces) > in->avail_cols) row++, col = 0, p.flags |= SHR_CLUSTER_WRAPPED;
                if (take > in->avail_cols - col) take = in->avail_cols - col;
            }
            if (row > SHR_MAX_LAYOUT_COORD) goto limit;
            if ((done = clip && row >= in->avail_rows)) break;
            p.row = (int32_t)row, p.column = (int32_t)col, p.cells = (int32_t)take;
            if ((st = in->emit(in->user, &p)) != SHR_OK) return shr__fail(err, st, boff, SIZE_MAX, "placing a cluster failed");
            p.flags = SHR_CLUSTER_TAB_CONT;
            col += take;
            spaces -= take;
            if (col > max_col) max_col = col;
            if (col > SHR_MAX_LAYOUT_COORD) goto limit;
        } while (spaces > 0);
        if (nl) {
            col = 0;
            if (++row > SHR_MAX_LAYOUT_COORD) goto limit;
            done = clip && row >= in->avail_rows;
        }
    }
    if ((st = edges_reach(in, &edge, in->len, err)) != SHR_OK) return st;
    if (out_rows) *out_rows = (int32_t)(row + 1);
    if (out_cols) *out_cols = (int32_t)max_col;
    return SHR_OK;
limit:
    return shr__fail(err, SHR_E_LIMIT, in->len, SIZE_MAX, "row or column limit");
}

shr_status shr_pl_lyr_tilemap_profile_get(shr_text_profile_info *out) {
    if (!out) return SHR_E_INVALID_ARG;
    *out = (shr_text_profile_info){{0}, shr__unicode_version, shr__text_rules_version, shr__tab_stop,
                                   shr__cluster_max_scalars, shr__cluster_max_bytes};
    memcpy(out->text_profile_id, shr__text_profile_id, 32);
    return SHR_OK;
}

shr_status shr_pl_lyr_tilemap_limits_get(shr_text_limits *out) {
    if (!out) return SHR_E_INVALID_ARG;
    *out = (shr_text_limits){SHR_MAX_TEXT_BYTES, shr__cluster_max_bytes, shr__cluster_max_scalars, SHR_MAX_SPAN,
                             SHR_MAX_GRID, SHR_MAX_GRID};
    return SHR_OK;
}

typedef struct measure_sink {
    shr_text_cluster *out;
    size_t capacity, n;
} measure_sink;

static shr_status measure_emit(void *user, const shr__piece *p) {
    measure_sink *k = user;
    if (k->n < k->capacity)
        k->out[k->n] = (shr_text_cluster){p->byte_offset, p->byte_length, p->row, p->column, p->cells, p->flags};
    k->n++;
    return SHR_OK;
}

shr_status shr_pl_lyr_tilemap_measure(const char *utf8, size_t length, int32_t cols, uint32_t flags,
                                      shr_text_cluster *out, size_t capacity, shr_text_extent *extent,
                                      shr_error_info *err) {
    shr__err_clear(err);
    if (extent) *extent = (shr_text_extent){0};
    if (!extent || (capacity && !out) || cols < 0) return shr__fail(err, SHR_E_INVALID_ARG, 0, SIZE_MAX, "invalid argument");
    if (flags & ~(uint32_t)SHR_TEXT_WRAP) return shr__fail(err, SHR_E_INVALID_ARG, 0, SIZE_MAX, "unknown text flag");
    measure_sink sink = {out, capacity, 0};
    shr__layout_in in = {utf8, length, NULL, 0, (flags & SHR_TEXT_WRAP) != 0, cols, 0, measure_emit, &sink};
    shr_text_extent e = {0};
    shr_status st = shr__layout(&in, &e.rows, &e.cols, err);
    if (st != SHR_OK) return st;
    e.clusters = sink.n;
    *extent = e;
    if (sink.n > capacity) return shr__fail(err, SHR_E_LIMIT, SIZE_MAX, SIZE_MAX, "more clusters than capacity");
    return SHR_OK;
}
