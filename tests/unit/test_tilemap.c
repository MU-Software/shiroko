#include "harness.h"

#include "compositor.h"
#include "font_core.h"
#include "shr_bitmap_font.h"
#include "shr_text.h"

#define CW SHR_CELL_WIDTH
#define CH SHR_CELL_HEIGHT
#define WHITE SHR_RGB(255, 255, 255)
#define RED SHR_RGB(255, 0, 0)
#define BLUE SHR_RGB(0, 0, 255)

static const shr_text_style plain = {WHITE, 0, 0};
static const shr_text_style on_blue = {WHITE, BLUE, SHR_STYLE_BG};

/* ===== Layout ===== */

#define MAXCL 64
static shr_text_cluster cl[MAXCL];
static shr_text_extent ext;

static shr_status measure_ex(const char *s, size_t len, int32_t cols, uint32_t flags, shr_error_info *err) {
    return shr_pl_lyr_tilemap_measure(s, len, cols, flags, cl, MAXCL, &ext, err);
}

static void measure(const char *s) { ASSERT_EQ_LL(measure_ex(s, strlen(s), 80, 0, NULL), SHR_OK); }

static shr_text_cluster at(size_t i) {
    ASSERT_EQ_LL(i < ext.clusters && i < MAXCL, 1);
    return cl[i];
}

/* Cells of the single cluster making up `s`. */
static int32_t cells_of(const char *s, uint32_t *flags) {
    measure(s);
    ASSERT_EQ_LL(ext.clusters, 1);
    if (flags) *flags = cl[0].flags;
    return cl[0].cells;
}

static shr_status measure_status(const char *s, size_t len, shr_error_info *err) { return measure_ex(s, len, 80, 0, err); }

TEST profile_and_limits_report_build_constants(void) {
    shr_text_profile_info p = {0};
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_profile_get(&p), SHR_OK);
    ASSERT_STR_EQ(p.unicode_version, "17.0.0");
    ASSERT_EQ_LL(p.tab_stop, 4);
    ASSERT_EQ_LL(p.cluster_max_scalars, 16);
    ASSERT(p.text_profile_id[0] | p.text_profile_id[31]);
    shr_text_limits l = {0};
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_limits_get(&l), SHR_OK);
    ASSERT_EQ_LL(l.max_text_bytes, 1 << 20);
    ASSERT_EQ_LL(l.max_cell_bytes, p.cluster_max_bytes);
    ASSERT_EQ_LL(l.max_cell_scalars, p.cluster_max_scalars);
    ASSERT_EQ_LL(l.max_span, 256);
    ASSERT_EQ_LL(l.max_rows, 1 << 15);
    ASSERT_EQ_LL(l.max_cols, 1 << 15);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_profile_get(NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_limits_get(NULL), SHR_E_INVALID_ARG);
    PASS();
}

TEST measure_places_mixed_widths(void) {
    measure("A\xEA\xB0\x80\xF0\x9F\x98\x80" "B"); /* A가😀B */
    ASSERT_EQ_LL(ext.clusters, 4);
    const int32_t cells[] = {1, 2, 2, 1}, cols[] = {0, 1, 3, 5};
    for (size_t i = 0; i < 4; i++) {
        ASSERT_EQ_LL(at(i).cells, cells[i]);
        ASSERT_EQ_LL(at(i).column, cols[i]);
        ASSERT_EQ_LL(at(i).row, 0);
    }
    ASSERT_EQ_LL(ext.rows, 1);
    ASSERT_EQ_LL(ext.cols, 6);

    const char *four[] = {"ABCD", "\xED\x95\x9C\xEA\xB5\xAD", "\xF0\x9F\x98\x80\xF0\x9F\x98\x80"};
    for (int i = 0; i < 3; i++) {
        measure(four[i]);
        ASSERT_EQ_LL(ext.cols, 4);
    }
    PASS();
}

TEST cluster_widths_follow_emoji_and_selector_rules(void) {
    uint32_t f = 0;
    ASSERT_EQ_LL(cells_of("\xCC\x81", &f), 1); /* lone U+0301 */
    ASSERT(f & SHR_CLUSTER_REPLACEMENT);
    ASSERT_EQ_LL(cells_of("\xEF\xB8\x8F", &f), 0); /* lone VS16 */
    ASSERT(f & SHR_CLUSTER_INVISIBLE);
    ASSERT_EQ_LL(cells_of("\xF0\x9F\x8F\xBB", NULL), 2);            /* U+1F3FB alone */
    ASSERT_EQ_LL(cells_of("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", &f), 2); /* 👍🏽 */
    ASSERT(f & SHR_CLUSTER_EMOJI);
    ASSERT_EQ_LL(cells_of("\xE2\x9D\xA4", &f), 1); /* ❤ text default */
    ASSERT(!(f & SHR_CLUSTER_EMOJI));
    ASSERT_EQ_LL(cells_of("\xE2\x9D\xA4\xEF\xB8\x8F", &f), 2); /* ❤️ */
    ASSERT(f & SHR_CLUSTER_EMOJI);
    ASSERT_EQ_LL(cells_of("A\xEF\xB8\x8F", NULL), 1);          /* unregistered A+VS16 */
    ASSERT_EQ_LL(cells_of("\xE2\x8C\x9A\xEF\xB8\x8E", &f), 2); /* ⌚+VS15: text, EAW W */
    ASSERT(!(f & SHR_CLUSTER_EMOJI));
    ASSERT_EQ_LL(cells_of("\xE2\x98\x80\xEF\xB8\x8E", NULL), 1);             /* ☀+VS15: text, EAW N */
    ASSERT_EQ_LL(cells_of("e\xCC\x81", NULL), 1);                           /* Latin NFD */
    ASSERT_EQ_LL(cells_of("\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8", NULL), 2); /* L+V+T */
    ASSERT_EQ_LL(cells_of("\xEA\xB0\x80\xE1\x86\xA8", NULL), 2);             /* LV+T */
    ASSERT_EQ_LL(cells_of("\xF0\x9F\x87\xB0\xF0\x9F\x87\xB7", NULL), 2);     /* 🇰🇷 */
    ASSERT_EQ_LL(cells_of("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB", NULL), 2); /* 👩‍💻 */
    ASSERT_EQ_LL(cells_of("\xF0\x9F\x91\x81\xE2\x80\x8D\xF0\x9F\x97\xA8", &f), 2); /* unqualified alias */
    ASSERT(f & SHR_CLUSTER_EMOJI);
    ASSERT_EQ_LL(cells_of("\xEE\x82\xB0", NULL), 1);             /* U+E0B0 Powerline (PUA) */
    ASSERT_EQ_LL(cells_of("\xE2\x9A\xA1", NULL), 2);             /* ⚡ emoji presentation */
    ASSERT_EQ_LL(cells_of("\xE2\x9A\xA1\xEF\xB8\x8E", NULL), 2); /* ⚡+VS15: text path, EAW W keeps 2 */
    ASSERT_EQ_LL(cells_of("\xE2\x8F\xBB", NULL), 1);             /* U+23FB power symbol: EAW N */
    ASSERT_EQ_LL(cells_of("\xE3\x80\x80", NULL), 2);             /* U+3000 */
    ASSERT_EQ_LL(cells_of("\xE2\x80\x8B", &f), 0);               /* ZWSP */
    ASSERT(f & SHR_CLUSTER_INVISIBLE);
    ASSERT_EQ_LL(cells_of("\xEF\xBF\xBD", &f), 1); /* valid U+FFFD is a normal char */
    ASSERT(!(f & SHR_CLUSTER_REPLACEMENT));
    ASSERT_EQ_LL(cells_of("1\xEF\xB8\x8F\xE2\x83\xA3", &f), 2); /* keycap: one base, three scalars */
    ASSERT(f & SHR_CLUSTER_EMOJI);
    ASSERT_EQ_LL(cells_of("A\xEF\xB8\x8E", NULL), 1);              /* unregistered A+VS15 */
    ASSERT_EQ_LL(cells_of("\xF0\x9F\x98\x80\xEF\xB8\x8F", &f), 2); /* unregistered 😀+VS16: text path */
    ASSERT(f & SHR_CLUSTER_EMOJI);
    /* A selector right after the base applies when only marks or ignorables follow. */
    ASSERT_EQ_LL(cells_of("\xE2\x9D\xA4\xEF\xB8\x8F\xCC\x81", &f), 2); /* ❤+VS16+U+0301 */
    ASSERT(f & SHR_CLUSTER_EMOJI);
    ASSERT_EQ_LL(cells_of("\xE2\x8C\x9A\xEF\xB8\x8E\xCC\x81", &f), 2); /* ⌚+VS15+U+0301: text, EAW W */
    ASSERT(!(f & SHR_CLUSTER_EMOJI));
    ASSERT_EQ_LL(cells_of("\xE2\x98\x80\xEF\xB8\x8E\xCC\x81", NULL), 1); /* ☀+VS15+U+0301 */
    ASSERT_EQ_LL(cells_of("\xE2\x9D\xA4\xCC\x81", &f), 1);               /* ❤+U+0301: no selector */
    ASSERT(!(f & SHR_CLUSTER_EMOJI));
    ASSERT_EQ_LL(cells_of("\xE2\x9D\xA4\xEF\xB8\x8F\xEF\xB8\x8F", &f), 2); /* a second selector is ignorable */
    ASSERT(f & SHR_CLUSTER_EMOJI);
    ASSERT_EQ_LL(cells_of("\xE2\x98\x80\xEF\xB8\x8E\xF0\x9F\x8F\xBB", &f), 2); /* a modifier is a second base */
    ASSERT(!(f & SHR_CLUSTER_EMOJI));
    PASS();
}

TEST tabs_and_newlines_advance_columns_and_rows(void) {
    measure("A\tB");
    ASSERT_EQ_LL(ext.clusters, 3);
    ASSERT_EQ_LL(at(1).cells, 3);
    ASSERT_EQ_LL(at(1).flags, SHR_CLUSTER_TAB);
    ASSERT_EQ_LL(at(2).column, 4);

    measure("ABCD\tE"); /* on a stop: to the next one */
    ASSERT_EQ_LL(at(4).cells, 4);
    ASSERT_EQ_LL(at(5).column, 8);

    measure("\t\t");
    ASSERT_EQ_LL(at(1).column, 4);
    ASSERT_EQ_LL(at(1).cells, 4);

    measure("A\r\nB\rC\nD");
    ASSERT_EQ_LL(ext.clusters, 7);
    ASSERT_EQ_LL(at(1).byte_length, 2); /* CRLF counts once */
    ASSERT_EQ_LL(at(1).flags, SHR_CLUSTER_NEWLINE);
    ASSERT_EQ_LL(at(1).cells, 0);
    ASSERT_EQ_LL(at(2).row, 1);
    ASSERT_EQ_LL(at(4).row, 2);
    ASSERT_EQ_LL(at(6).row, 3);
    ASSERT_EQ_LL(at(6).column, 0);

    measure("\n\nA  ");
    ASSERT_EQ_LL(ext.rows, 3);
    ASSERT_EQ_LL(ext.cols, 3); /* trailing spaces kept */
    PASS();
}

TEST wrap_moves_whole_clusters_to_the_next_row(void) {
    shr_status st;
    st = measure_ex("ABCDEFGHIJKLMNOPQRST", 20, 10, SHR_TEXT_WRAP, NULL);
    ASSERT_EQ_LL(st, SHR_OK);
    ASSERT_EQ_LL(ext.clusters, 20);
    ASSERT_EQ_LL(at(9).row, 0);
    ASSERT_EQ_LL(at(9).column, 9);
    ASSERT_EQ_LL(at(10).row, 1);
    ASSERT_EQ_LL(at(10).column, 0);
    ASSERT_EQ_LL(at(10).flags, SHR_CLUSTER_WRAPPED);
    ASSERT_EQ_LL(ext.rows, 2);
    ASSERT_EQ_LL(ext.cols, 10);

    st = measure_ex("A\xEA\xB0\x80", 4, 2, SHR_TEXT_WRAP, NULL); /* 가 does not fit after A */
    ASSERT_EQ_LL(at(1).row, 1);
    ASSERT_EQ_LL(at(1).column, 0);

    st = measure_ex("AB\xE2\x80\x8B", 5, 2, SHR_TEXT_WRAP, NULL); /* zero cells never wrap */
    ASSERT_EQ_LL(at(2).row, 0);
    ASSERT_EQ_LL(at(2).column, 2);

    char longs[MAXCL];
    memset(longs, 'x', sizeof(longs));
    st = measure_ex(longs, sizeof(longs), 10, 0, NULL); /* without wrap columns keep counting */
    ASSERT_EQ_LL(at(MAXCL - 1).column, MAXCL - 1);
    ASSERT_EQ_LL(at(MAXCL - 1).row, 0);
    PASS();
}

TEST tab_spaces_continue_across_a_wrap(void) {
    shr_status st;
    st = measure_ex("ABCDE\tX", 7, 6, SHR_TEXT_WRAP, NULL);
    ASSERT_EQ_LL(st, SHR_OK);
    ASSERT_EQ_LL(ext.clusters, 8);
    shr_text_cluster a = at(5), b = at(6);
    ASSERT_EQ_LL(a.byte_offset, 5);
    ASSERT_EQ_LL(a.row, 0);
    ASSERT_EQ_LL(a.column, 5);
    ASSERT_EQ_LL(a.cells, 1);
    ASSERT_EQ_LL(a.flags, SHR_CLUSTER_TAB);
    ASSERT_EQ_LL(b.byte_offset, 5);
    ASSERT_EQ_LL(b.row, 1);
    ASSERT_EQ_LL(b.column, 0);
    ASSERT_EQ_LL(b.cells, 2);
    ASSERT_EQ_LL(b.flags, SHR_CLUSTER_TAB_CONT | SHR_CLUSTER_WRAPPED);
    ASSERT_EQ_LL(at(7).column, 2);

    st = measure_ex("A\t", 2, 3, SHR_TEXT_WRAP, NULL); /* the wrapped TAB's first row counts in the extent */
    ASSERT_EQ_LL(ext.rows, 2);
    ASSERT_EQ_LL(ext.cols, 3);

    st = measure_ex("ABCDEF\tX", 8, 6, SHR_TEXT_WRAP, NULL); /* starts on the next row */
    ASSERT_EQ_LL(ext.clusters, 8);
    ASSERT_EQ_LL(at(6).row, 1);
    ASSERT_EQ_LL(at(6).cells, 2);
    ASSERT_EQ_LL(at(6).flags, SHR_CLUSTER_TAB | SHR_CLUSTER_WRAPPED);
    PASS();
}

TEST wrap_errors_without_room(void) {
    shr_status st;
    shr_error_info err;
    st = measure_ex("", 0, 0, SHR_TEXT_WRAP, &err); /* nothing to place */
    ASSERT_EQ_LL(st, SHR_OK);
    st = measure_ex("\xE2\x80\x8B", 3, 0, SHR_TEXT_WRAP, &err);
    ASSERT_EQ_LL(st, SHR_OK);
    st = measure_ex("A", 1, 0, SHR_TEXT_WRAP, &err);
    ASSERT_EQ_LL(st, SHR_E_WRAP_NO_SPACE);
    ASSERT_EQ_LL(err.status, SHR_E_WRAP_NO_SPACE);
    ASSERT_EQ_LL(ext.clusters, 0);
    st = measure_ex("\t", 1, 0, SHR_TEXT_WRAP, &err);
    ASSERT_EQ_LL(st, SHR_E_WRAP_NO_SPACE);
    st = measure_ex("A\xEA\xB0\x80", 4, 1, SHR_TEXT_WRAP, &err); /* 2 cells never fit in 1 column */
    ASSERT_EQ_LL(st, SHR_E_CLUSTER_TOO_WIDE);
    ASSERT_EQ_LL(err.byte_offset, 1);
    PASS();
}

TEST invalid_utf8_and_controls_report_byte_offsets(void) {
    shr_error_info err;
    ASSERT_EQ_LL(measure_status("\x41\xE1\x80\x42", 4, &err), SHR_E_INVALID_UTF8);
    ASSERT_EQ_LL(err.byte_offset, 1);
    ASSERT_EQ_LL(err.item_index, SIZE_MAX);
    ASSERT_EQ_LL(measure_status("\xC0\x80", 2, &err), SHR_E_INVALID_UTF8);
    ASSERT_EQ_LL(err.byte_offset, 0);
    ASSERT_EQ_LL(measure_status("A\xE0\x80\x80", 4, &err), SHR_E_INVALID_UTF8); /* overlong 3-byte */
    ASSERT_EQ_LL(err.byte_offset, 1);
    ASSERT_EQ_LL(measure_status("\xED\xA0\x80", 3, &err), SHR_E_INVALID_UTF8); /* surrogate */
    ASSERT_EQ_LL(measure_status("\xF4\x90\x80\x80", 4, &err), SHR_E_INVALID_UTF8);
    ASSERT_EQ_LL(measure_status("AB\xE1\x80", 4, &err), SHR_E_INVALID_UTF8);
    ASSERT_EQ_LL(err.byte_offset, 2);
    ASSERT_EQ_LL(measure_status("\xC1\x81", 2, &err), SHR_E_INVALID_UTF8);
    ASSERT_EQ_LL(measure_status("\x80", 1, &err), SHR_E_INVALID_UTF8);
    ASSERT_EQ_LL(measure_status("\xF5\x80\x80\x80", 4, &err), SHR_E_INVALID_UTF8);
    ASSERT_EQ_LL(measure_status(NULL, 1, &err), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(measure_status(NULL, 0, &err), SHR_OK);
    ASSERT_EQ_LL(measure_status("A", ((size_t)1 << 20) + 1, &err), SHR_E_LIMIT); /* rejected before reading */

    ASSERT_EQ_LL(measure_status("A\x1B[31m", 6, &err), SHR_E_CONTROL_CHAR);
    ASSERT_EQ_LL(err.byte_offset, 1);
    ASSERT_EQ_LL(measure_status("\x7F", 1, &err), SHR_E_CONTROL_CHAR);
    ASSERT_EQ_LL(measure_status("\xC2\x85", 2, &err), SHR_E_CONTROL_CHAR);
    ASSERT_EQ_LL(measure_status("A\0B", 3, &err), SHR_E_CONTROL_CHAR);
    ASSERT_EQ_LL(measure_status("\\n\\x1b", 6, &err), SHR_OK); /* backslash notation is plain text */
    PASS();
}

TEST clusters_longer_than_the_profile_limit_fail(void) {
    char zalgo[2 + 16 * 2];
    zalgo[0] = 'A', zalgo[1] = 'e';
    for (int i = 0; i < 16; i++) zalgo[2 + 2 * i] = (char)0xCC, zalgo[3 + 2 * i] = (char)0x81;
    shr_error_info err;
    ASSERT_EQ_LL(measure_status(zalgo, sizeof(zalgo) - 2, &err), SHR_OK); /* 16 scalars */
    ASSERT_EQ_LL(measure_status(zalgo, sizeof(zalgo), &err), SHR_E_LIMIT);
    ASSERT_EQ_LL(err.byte_offset, 1);
    PASS();
}

TEST measure_rejects_bad_arguments(void) {
    shr_error_info err;
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_measure("A", 1, 1, 0, cl, 1, NULL, &err), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(err.status, SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_measure("A", 1, 1, 0, NULL, 1, &ext, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_measure("A", 1, -1, 0, cl, 1, &ext, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_measure("A", 1, 1, 1u << 1, cl, 1, &ext, NULL), SHR_E_INVALID_ARG);
    ext.clusters = 9;
    ASSERT_EQ_LL(measure_status("A\x80", 2, NULL), SHR_E_INVALID_UTF8);
    ASSERT_EQ_LL(ext.clusters, 0); /* no extent on errors */
    PASS();
}

TEST measure_fills_up_to_capacity(void) {
    shr_error_info err;
    shr_text_cluster two[2] = {{0}};
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_measure("ab\tc", 4, 8, 0, two, 2, &ext, &err), SHR_E_LIMIT);
    ASSERT_EQ_LL(err.status, SHR_E_LIMIT);
    ASSERT_EQ_LL(ext.clusters, 4); /* the extent is complete */
    ASSERT_EQ_LL(ext.rows, 1);
    ASSERT_EQ_LL(ext.cols, 5);
    ASSERT_EQ_LL(two[1].byte_offset, 1);
    ASSERT_EQ_LL(two[1].column, 1);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_measure("ab\tc", 4, 8, 0, NULL, 0, &ext, NULL), SHR_E_LIMIT); /* count only */
    ASSERT_EQ_LL(ext.clusters, 4);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_measure("", 0, 8, 0, NULL, 0, &ext, NULL), SHR_OK);
    ASSERT_EQ_LL(ext.clusters, 0);
    ASSERT_EQ_LL(ext.rows, 1);
    PASS();
}

/* Rows and columns stop at the coordinate limit (2^20) before the text limit. */
TEST coordinates_stop_at_the_limit(void) {
    const size_t tabs = 349525; /* wrapped at 1 column: 3 rows each after the first */
    char *s = malloc(tabs + 3);
    ASSERT(s != NULL);
    memset(s, '\t', tabs + 3);
    shr_text_cluster *out = malloc(sizeof(shr_text_cluster) * 4 * (tabs + 3));
    ASSERT(out != NULL);
    shr_status st;
    shr_error_info err;
#define measure_ex(s, len, cols, flags, err) shr_pl_lyr_tilemap_measure(s, len, cols, flags, out, 4 * (tabs + 3), &ext, err)
    st = measure_ex(s, 262145, 0, 0, &err); /* columns through TABs */
    ASSERT_EQ_LL(st, SHR_E_LIMIT);
    s[262144] = 'A';
    st = measure_ex(s, 262145, 0, 0, &err); /* columns through a cluster */
    ASSERT_EQ_LL(st, SHR_E_LIMIT);
    memset(s, '\t', tabs + 3);
    st = measure_ex(s, tabs, 1, SHR_TEXT_WRAP, &err);
    ASSERT_EQ_LL(st, SHR_OK);
    st = measure_ex(s, tabs + 1, 1, SHR_TEXT_WRAP, &err); /* rows through wrapped TABs */
    ASSERT_EQ_LL(st, SHR_E_LIMIT);
    s[tabs] = 'A', s[tabs + 1] = 'A';
    st = measure_ex(s, tabs + 2, 1, SHR_TEXT_WRAP, &err); /* rows through wrapped clusters */
    ASSERT_EQ_LL(st, SHR_E_LIMIT);
    s[tabs + 1] = '\n';
    st = measure_ex(s, tabs + 2, 1, SHR_TEXT_WRAP, &err); /* rows through a newline */
    ASSERT_EQ_LL(st, SHR_E_LIMIT);
#undef measure_ex
    free(out);
    free(s);
    PASS();
}

/* ===== Tilemap ===== */

static harness H;

static shr_lyr *new_layer(void) {
    shr_lyr *l = NULL;
    ASSERT_EQ_LL(shr_lyr_create(H.ctx, 0, (shr_rect){0, 0, HW, HH}, &l), SHR_OK);
    return l;
}

static shr_lyr *open_grid(int32_t rows, int32_t cols, void (*tweak)(shr_context_desc *, shr_framebuffer_driver *)) {
    harness_open(&H, SHR_OUTPUT_RELEASE_ON_PRESENT, tweak);
    harness_font(&H);
    shr_lyr *l = new_layer();
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, rows, cols, NULL), SHR_OK);
    return l;
}

static void close_grid(shr_lyr *l) {
    ASSERT_EQ_LL(shr_lyr_destroy(l), SHR_OK);
    harness_close(&H);
}

/* Rows are rebuilt at submit; inspecting them first applies the pending changes. */
static const shr__group *group_of(const shr_lyr *l, int32_t row) {
    ASSERT_EQ_LL(l->flush(l->state), SHR_OK);
    for (size_t i = 0; i < l->groups.len; i++) {
        const shr__group *g = SHR_VEC_AT(&l->groups, shr__group, i);
        if (g->id == (uint32_t)row) return g;
    }
    return NULL;
}

static size_t ncmds(const shr_lyr *l, int32_t row) {
    const shr__group *g = group_of(l, row);
    return g ? g->n : 0;
}

static const shr__lcmd *cmd(const shr_lyr *l, int32_t row, size_t i) {
    ASSERT_EQ_LL(i < ncmds(l, row), 1);
    return &group_of(l, row)->cmds[i];
}

static size_t count_kind(const shr_lyr *l, int32_t row, uint8_t kind) {
    size_t n = 0;
    for (size_t i = 0; i < ncmds(l, row); i++) n += cmd(l, row, i)->kind == kind;
    return n;
}

/* The glyphs of a row, one character per column: ASCII glyph, '*' other glyph, '-' covered by the glyph
 * to its left, '.' none. */
static const char *row_text(const shr_lyr *l, int32_t row) {
    static char s[HW / CW + 1];
    memset(s, '.', HW / CW);
    s[HW / CW] = 0;
    for (size_t i = 0; i < ncmds(l, row); i++) {
        const shr__lcmd *c = cmd(l, row, i);
        if (c->kind != SHR__LCMD_GLYPH) continue;
        uint64_t v = c->id & (SHR_ID_VALUE | SHR_ID_EMOJI | SHR_ID_CLUSTER);
        int32_t c0 = c->dst.x0 / CW, c1 = c->dst.x1 / CW;
        s[c0] = v < 0x80 ? (char)v : '*';
        for (int32_t k = c0 + 1; k < c1; k++) s[k] = '-';
    }
    return s;
}

static shr_status set_cell(shr_lyr *l, int32_t row, int32_t col, const char *s, uint32_t span, shr_text_style st) {
    return shr_pl_lyr_tilemap_set_cell(l, row, col, s, s ? strlen(s) : 1, span, st);
}

static shr_status set_text(shr_lyr *l, int32_t row, int32_t col, const char *s, shr_text_style st, uint32_t flags,
                           shr_error_info *err) {
    return shr_pl_lyr_tilemap_set_text(l, row, col, s, strlen(s), st, NULL, 0, flags, err);
}

TEST resize_attaches_and_validates_arguments(void) {
    shr_lyr *l = open_grid(3, 8, NULL), *other = new_layer();
    ASSERT_EQ_LL(shr_lyr_cmd_begin(l), SHR_E_STATE);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(NULL, H.font, 1, 1, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, NULL, 1, 1, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, -1, 1, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, 1, -1, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, SHR_MAX_GRID + 1, 1, NULL), SHR_E_LIMIT);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, 1, SHR_MAX_GRID + 1, NULL), SHR_E_LIMIT);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(H.font), SHR_E_STATE); /* in use */

    static harness h2;
    harness_open(&h2, SHR_OUTPUT_RELEASE_ON_PRESENT, NULL);
    harness_font(&h2);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, h2.font, 1, 1, NULL), SHR_E_INVALID_ARG); /* another context's font */
    harness_close(&h2);

    shr_error_info err;
    ASSERT_EQ_LL(set_cell(NULL, 0, 0, "A", 1, plain), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(set_text(NULL, 0, 0, "A", plain, 0, &err), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(err.status, SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(NULL, 0, 0, 1, 1, plain), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(set_cell(other, 0, 0, "A", 1, plain), SHR_E_STATE); /* not a tilemap */
    ASSERT_EQ_LL(set_text(other, 0, 0, "A", plain, 0, &err), SHR_E_STATE);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(other, 0, 0, 1, 1, plain), SHR_E_STATE);
    ASSERT_EQ_LL(shr_lyr_destroy(other), SHR_OK);
    close_grid(l); /* the font is free again: harness_close destroys it */
    PASS();
}

static const char other_kind;

TEST resize_refuses_layers_owned_elsewhere(void) {
    shr_lyr *l = open_grid(1, 1, NULL);
    shr_lyr *app = new_layer(), *building = new_layer(), *plugin = new_layer();
    ASSERT_EQ_LL(shr_lyr_cmd_begin(app), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_fill(app, (shr_rect){0, 0, 4, 4}, RED), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(app), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(app, H.font, 1, 1, NULL), SHR_E_STATE);
    ASSERT_EQ_LL(shr_lyr_cmd_begin(building), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(building, H.font, 1, 1, NULL), SHR_E_STATE);
    ASSERT_EQ_LL(shr__lyr_attach(plugin, &other_kind, NULL, NULL, NULL), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(plugin, H.font, 1, 1, NULL), SHR_E_STATE);
    ASSERT_EQ_LL(set_cell(plugin, 0, 0, "A", 1, plain), SHR_E_STATE);
    shr_lyr_destroy(app);
    shr_lyr_destroy(building);
    shr_lyr_destroy(plugin);
    close_grid(l);
    PASS();
}

TEST resize_keeps_overlapping_cells_and_drops_cut_wide_cells(void) {
    static const char family[] = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"; /* 18 bytes */
    shr_lyr *l = open_grid(3, 8, NULL);
    ASSERT_EQ_LL(set_cell(l, 0, 0, "A", 1, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 2, family, 2, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 6, "\xEA\xB0\x80", 2, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 1, 2, "\xEA\xB0\x80", 2, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 2, 0, "C", 1, plain), SHR_OK);
    uint64_t family_id = cmd(l, 0, 1)->id;
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, 2, 7, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "A.*-....");
    ASSERT_STR_EQ(row_text(l, 1), "..*-....");
    ASSERT(group_of(l, 2) == NULL);
    ASSERT_EQ_LL(cmd(l, 0, 1)->id, family_id);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, 3, 8, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "A.*-...."); /* the cut cell stays gone */
    ASSERT(group_of(l, 2) == NULL);
    ASSERT_EQ_LL(set_cell(l, 1, 3, "x", 1, plain), SHR_OK); /* the kept wide cell still owns its tail */
    ASSERT_STR_EQ(row_text(l, 1), "...x....");
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, 0, 0, NULL), SHR_OK);
    ASSERT_EQ_LL(l->groups.len, 0);
    ASSERT_EQ_LL(set_cell(l, 0, 0, "A", 1, plain), SHR_E_INVALID_ARG);
    close_grid(l);
    PASS();
}

TEST resize_switches_font(void) {
    shr_lyr *l = open_grid(1, 8, NULL);
    ASSERT_EQ_LL(set_cell(l, 0, 0, "A", 1, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 1, "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB", 2, plain), SHR_OK);
    const char *family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";
    ASSERT_EQ_LL(set_cell(l, 0, 3, family, 2, plain), SHR_OK); /* text kept on the heap */
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = font_dir_open;
    shr_pl_res_bitmap_font *f2;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(H.ctx, &fd, &f2), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, f2, 1, 8, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "A*-*-...");
    for (size_t i = 0; i < ncmds(l, 0); i++) ASSERT(cmd(l, 0, i)->res == shr__bitmap_font_res(f2));
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(f2), SHR_E_STATE);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(H.font), SHR_OK); /* no longer used */
    H.font = f2;
    close_grid(l);
    PASS();
}

TEST set_cell_validates_placed_input(void) {
    char many[1 + 16 * 2]; /* e + 16 x U+0301: 17 scalars in 33 bytes */
    many[0] = 'e';
    for (int i = 0; i < 16; i++) many[1 + 2 * i] = (char)0xCC, many[2 + 2 * i] = (char)0x81;
    char big[65];
    memset(big, 'a', sizeof(big));
    shr_lyr *l = open_grid(3, 8, NULL);
    ASSERT_EQ_LL(set_cell(l, 0, 0, "Z", 1, plain), SHR_OK);
    const struct {
        int32_t row, col;
        const char *s;
        size_t len;
        uint32_t span, flags;
        shr_status want;
    } cases[] = {
        {0, 0, NULL, 1, 1, 0, SHR_E_INVALID_ARG},
        {0, 0, "A", 1, 0, 0, SHR_E_INVALID_ARG},
        {0, 0, "A", 1, 1, 1u << 8, SHR_E_UNKNOWN_STYLE},
        {0, 0, big, 65, 1, 0, SHR_E_LIMIT},
        {0, 0, "A", 1, SHR_MAX_SPAN + 1, 0, SHR_E_LIMIT},
        {-1, 0, "A", 1, 1, 0, SHR_E_INVALID_ARG},
        {3, 0, "A", 1, 1, 0, SHR_E_INVALID_ARG},
        {0, -1, "A", 1, 1, 0, SHR_E_INVALID_ARG},
        {0, 7, "A", 1, 2, 0, SHR_E_INVALID_ARG},
        {0, 0, "A\xE1\x80", 3, 1, 0, SHR_E_INVALID_UTF8},
        {0, 0, "\n", 1, 1, 0, SHR_E_CONTROL_CHAR},
        {0, 0, "\t", 1, 1, 0, SHR_E_CONTROL_CHAR},
        {0, 0, "A\0", 2, 1, 0, SHR_E_CONTROL_CHAR},
        {0, 0, "\x7F", 1, 1, 0, SHR_E_CONTROL_CHAR},
        {0, 0, "\xC2\x85", 2, 1, 0, SHR_E_CONTROL_CHAR},
        {0, 0, many, 33, 1, 0, SHR_E_LIMIT}, /* the same cluster limit as set_text and measure */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        shr_text_style s = {WHITE, 0, cases[i].flags};
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(l, cases[i].row, cases[i].col, cases[i].s, cases[i].len, cases[i].span, s),
                     cases[i].want);
    }
    ASSERT_STR_EQ(row_text(l, 0), "Z.......");

    ASSERT_EQ_LL(measure_status(many, 33, NULL), SHR_E_LIMIT);
    char full[64]; /* 16 scalars in 64 bytes: both limits */
    for (int i = 0; i < 16; i++) memcpy(full + 4 * i, "\xF0\x9F\x98\x80", 4);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(l, 0, 0, full, 64, 1, on_blue), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_cell(l, 0, 0, many, 31, 1, on_blue), SHR_OK); /* 16 scalars */
    ASSERT_EQ_LL(set_cell(l, 0, 1, "", 1, on_blue), SHR_OK);                          /* background only */
    ASSERT_EQ_LL(set_cell(l, 0, 2, "\xEF\xB8\x8F", 1, on_blue), SHR_OK);              /* lone VS16: no glyph */
    ASSERT_EQ_LL(set_cell(l, 0, 3, "\xEA\xB0\x80", 1, plain), SHR_OK);                /* kept as given */
    ASSERT_EQ_LL(set_cell(l, 0, 4, "\xF0\x9F\x91\xA9\xE2\x80\x8D", 2, plain), SHR_OK); /* dangling ZWJ */
    ASSERT_STR_EQ(row_text(l, 0), "*..**-..");
    ASSERT_EQ_LL(count_kind(l, 0, SHR__LCMD_FILL), 3);
    ASSERT_EQ_LL(set_cell(l, 1, 0, "A", 8, plain), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 1), "A-------");
    close_grid(l);
    PASS();
}

TEST set_cell_clears_the_wide_cells_it_overlaps(void) {
    shr_lyr *l = open_grid(1, 8, NULL);
    ASSERT_EQ_LL(set_cell(l, 0, 2, "\xEA\xB0\x80", 2, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 3, "x", 1, plain), SHR_OK); /* on the tail */
    ASSERT_STR_EQ(row_text(l, 0), "...x....");
    ASSERT_EQ_LL(set_cell(l, 0, 0, "\xEA\xB0\x80", 2, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 2, "\xEA\xB0\x80", 2, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 1, "\xEA\xB0\x80", 2, plain), SHR_OK); /* straddles both */
    ASSERT_STR_EQ(row_text(l, 0), ".*-.....");
    ASSERT_EQ_LL(set_cell(l, 0, 5, "\xEA\xB0\x80", 2, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 3, "y", 3, plain), SHR_OK); /* its last cell hits the head */
    ASSERT_STR_EQ(row_text(l, 0), ".*-y--..");
    close_grid(l);
    PASS();
}

TEST set_text_continues_lines_at_the_start_column(void) {
    shr_lyr *l = open_grid(3, 8, NULL);
    ASSERT_EQ_LL(set_text(l, 0, 2, "ab\ncd\r\nef", plain, 0, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "..ab....");
    ASSERT_STR_EQ(row_text(l, 1), "..cd....");
    ASSERT_STR_EQ(row_text(l, 2), "..ef....");
    ASSERT_EQ_LL(set_text(l, 0, 5, "a\xEA\xB0\x80" "bcd", plain, SHR_TEXT_WRAP, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "..ab.a*-"); /* 가 still fits at the last two columns */
    ASSERT_STR_EQ(row_text(l, 1), "..cd.bcd");
    ASSERT_EQ_LL(set_text(l, 0, 6, "xy\xEA\xB0\x80", plain, SHR_TEXT_WRAP, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "..ab.axy");
    ASSERT_STR_EQ(row_text(l, 1), "..cd.b*-");
    close_grid(l);
    PASS();
}

TEST set_text_drops_what_leaves_the_grid(void) {
    shr_lyr *l = open_grid(3, 8, NULL);
    ASSERT_EQ_LL(set_text(l, 0, 5, "abcdef", plain, 0, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), ".....abc");
    ASSERT_EQ_LL(set_text(l, 0, 0, "\xE2\x80\x8B" "1234567\xEA\xB0\x80", plain, 0, NULL), SHR_OK); /* no room for 가 */
    ASSERT_STR_EQ(row_text(l, 0), "1234567."); /* its visible half is blank, not the old 'c' */
    ASSERT_EQ_LL(set_text(l, 1, 0, "a\nb\nc", plain, 0, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 1), "a.......");
    ASSERT_STR_EQ(row_text(l, 2), "b.......");
    ASSERT_EQ_LL(set_text(l, 2, 6, "wxyz", plain, SHR_TEXT_WRAP, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 2), "b.....wx");
    close_grid(l);
    PASS();
}

TEST set_text_tabs_become_background_cells(void) {
    shr_lyr *l = open_grid(2, 8, NULL);
    ASSERT_EQ_LL(set_text(l, 0, 0, "a\tb", on_blue, 0, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "a...b...");
    ASSERT_EQ_LL(count_kind(l, 0, SHR__LCMD_FILL), 5);
    ASSERT_EQ_LL(set_text(l, 1, 6, "a\t", on_blue, 0, NULL), SHR_OK); /* the TAB runs past the grid */
    ASSERT_EQ_LL(count_kind(l, 1, SHR__LCMD_FILL), 2);
    ASSERT_EQ_LL(cmd(l, 1, 1)->kind, SHR__LCMD_GLYPH);
    ASSERT_EQ_LL(cmd(l, 1, 2)->dst.x0, 7 * CW);
    close_grid(l);
    PASS();
}

TEST set_text_errors_change_nothing(void) {
    shr_lyr *l = open_grid(3, 8, NULL);
    ASSERT_EQ_LL(set_cell(l, 0, 0, "Z", 1, plain), SHR_OK);
    shr_error_info err;
    ASSERT_EQ_LL(set_text(l, 0, 0, "ab\xE1", plain, 0, &err), SHR_E_INVALID_UTF8);
    ASSERT_EQ_LL(err.byte_offset, 2);
    ASSERT_EQ_LL(set_text(l, 0, 0, "ab\x1b", plain, 0, &err), SHR_E_CONTROL_CHAR);
    ASSERT_EQ_LL(set_text(l, 0, 7, "\xEA\xB0\x80", plain, SHR_TEXT_WRAP, &err), SHR_E_CLUSTER_TOO_WIDE);
    ASSERT_EQ_LL(set_text(l, 0, 0, "ab", plain, 1u << 1, &err), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(set_text(l, 0, 0, "ab", (shr_text_style){WHITE, 0, 1u << 8}, 0, &err), SHR_E_UNKNOWN_STYLE);
    ASSERT_EQ_LL(err.item_index, SIZE_MAX);
    ASSERT_EQ_LL(set_text(l, -1, 0, "ab", plain, 0, &err), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(set_text(l, 3, 0, "ab", plain, 0, &err), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(set_text(l, 0, -1, "ab", plain, 0, &err), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(set_text(l, 0, 8, "ab", plain, 0, &err), SHR_E_INVALID_ARG);
    ASSERT_STR_EQ(row_text(l, 0), "Z.......");
    close_grid(l);
    PASS();
}

TEST set_text_style_runs_on_cluster_boundaries(void) {
    shr_lyr *l = open_grid(1, 8, NULL);
    const char *s = "\xEA\xB0\x80" "e\xCC\x81" "B"; /* 가 é B */
    shr_error_info err;
    shr_style_run run = {1, 3, {RED, RED, SHR_STYLE_BG}};
#define RUNS(r, n) shr_pl_lyr_tilemap_set_text(l, 0, 0, s, 7, plain, r, n, 0, &err)
    ASSERT_EQ_LL(RUNS(&run, 1), SHR_E_STYLE_BOUNDARY); /* inside 가 */
    ASSERT_EQ_LL(err.byte_offset, 1);
    ASSERT_EQ_LL(err.item_index, 0);
    run.byte_start = 3, run.byte_end = 4; /* splits e + U+0301 */
    ASSERT_EQ_LL(RUNS(&run, 1), SHR_E_STYLE_BOUNDARY);
    ASSERT_EQ_LL(err.byte_offset, 4);
    ASSERT(group_of(l, 0) == NULL);
    run.byte_start = 3, run.byte_end = 6;
    ASSERT_EQ_LL(RUNS(&run, 1), SHR_OK);
    ASSERT_EQ_LL(count_kind(l, 0, SHR__LCMD_FILL), 1);
    ASSERT_EQ_LL(cmd(l, 0, 1)->kind, SHR__LCMD_FILL);
    ASSERT_EQ_LL(cmd(l, 0, 1)->dst.x0, 2 * CW);
    ASSERT_EQ_LL(cmd(l, 0, 1)->color, RED);
    ASSERT_EQ_LL(cmd(l, 0, 3)->color, WHITE); /* B keeps the base style */
    run.style.flags = 1u << 8;
    ASSERT_EQ_LL(RUNS(&run, 1), SHR_E_UNKNOWN_STYLE);
    ASSERT_EQ_LL(err.item_index, 0);

    shr_style_run two[2] = {{0, 3, plain}, {6, 7, {WHITE, 0, SHR_STYLE_BOLD}}}; /* the last one ends the text */
    ASSERT_EQ_LL(RUNS(two, 2), SHR_OK);
    ASSERT_EQ_LL(cmd(l, 0, 2)->id >> SHR_ID_STYLE_SHIFT, 1);
    two[1].byte_start = 2;
    ASSERT_EQ_LL(RUNS(two, 2), SHR_E_INVALID_ARG); /* overlaps */
    ASSERT_EQ_LL(err.item_index, 1);
    two[1].byte_start = 7, two[1].byte_end = 6;
    ASSERT_EQ_LL(RUNS(two, 2), SHR_E_INVALID_ARG);
    two[1].byte_start = 6, two[1].byte_end = 8;
    ASSERT_EQ_LL(RUNS(two, 2), SHR_E_INVALID_ARG);
    two[1] = (shr_style_run){6, 6, plain};
    ASSERT_EQ_LL(RUNS(two, 2), SHR_E_INVALID_ARG); /* empty */
    ASSERT_EQ_LL(RUNS(NULL, 1), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(RUNS(two, 8), SHR_E_INVALID_ARG); /* more runs than bytes: some would be empty */
#undef RUNS
    run = (shr_style_run){2, 4, plain}; /* starts inside the last cluster */
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_text(l, 0, 0, "A\xEA\xB0\x80", 4, plain, &run, 1, 0, &err), SHR_E_STYLE_BOUNDARY);
    ASSERT_EQ_LL(err.byte_offset, 2);
    close_grid(l);
    PASS();
}

TEST clear_resets_cells_to_the_background(void) {
    shr_lyr *l = open_grid(3, 8, NULL);
    ASSERT_EQ_LL(set_text(l, 0, 0, "abcdefgh", plain, 0, NULL), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 2, 1, 3, (shr_text_style){RED, BLUE, SHR_STYLE_BG | SHR_STYLE_UNDERLINE}),
                 SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "ab...fgh");
    ASSERT_EQ_LL(count_kind(l, 0, SHR__LCMD_FILL), 3); /* background only */
    ASSERT_EQ_LL(cmd(l, 0, 2)->color, BLUE);
    ASSERT_EQ_LL(cmd(l, 0, 2)->dst.x0, 2 * CW);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 0, 1, 8, plain), SHR_OK);
    ASSERT(group_of(l, 0) == NULL);
    ASSERT_EQ_LL(set_cell(l, 1, 2, "\xEA\xB0\x80", 2, plain), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 1, 3, 2, 1, plain), SHR_OK); /* the tail clears the whole cell */
    ASSERT(group_of(l, 1) == NULL);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 0, 3, 0, plain), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, -1, 0, 1, 1, plain), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, -1, 1, 1, plain), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 0, -1, 1, plain), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 0, 1, -1, plain), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 1, 0, 3, 1, plain), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 4, 1, 5, plain), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 0, 1, 1, (shr_text_style){0, 0, 1u << 8}), SHR_E_UNKNOWN_STYLE);
    close_grid(l);
    PASS();
}

/* The colour filling a whole cell, or -1. */
static long cell_fill(const uint8_t *buf, int32_t row, int32_t col) {
    uint32_t c = px(buf, col * CW, row * CH);
    for (int y = row * CH; y < (row + 1) * CH; y++)
        for (int x = col * CW; x < (col + 1) * CW; x++)
            if (px(buf, x, y) != c) return -1;
    return (long)c;
}

static const uint8_t *frame_of(void) {
    ASSERT_EQ_LL(shr_submit(H.ctx), SHR_OK);
    settle(H.ctx);
    return H.out.shown;
}

TEST clear_paints_both_halves_of_a_cut_wide_cell(void) {
    shr_lyr *l = open_grid(1, 8, NULL);
    ASSERT_EQ_LL(set_cell(l, 0, 2, "\xEA\xB0\x80", 2, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 5, "\xEA\xB0\x80", 2, plain), SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 3, 1, 1, on_blue), SHR_OK); /* the tail only */
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 5, 1, 1, on_blue), SHR_OK); /* the head only */
    const uint8_t *s = frame_of();
    const long want[8] = {0, 0, 0x0000FF, 0x0000FF, 0, 0x0000FF, 0x0000FF, 0};
    for (int32_t c = 0; c < 8; c++) ASSERT_EQ_LL(cell_fill(s, 0, c), want[c]);
    close_grid(l);
    PASS();
}

TEST set_text_blanks_a_cluster_cut_by_the_last_column(void) {
    shr_lyr *l = open_grid(1, 4, NULL);
    ASSERT_EQ_LL(set_text(l, 0, 0, "abcd", plain, 0, NULL), SHR_OK);
    const char *s = "xyz\xEA\xB0\x80" "w\xE2\x80\x8B";
    ASSERT_EQ_LL(set_text(l, 0, 0, s, on_blue, 0, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "xyz.....");
    const uint8_t *px_ = frame_of();
    ASSERT_EQ_LL(cell_fill(px_, 0, 3), 0x0000FF); /* no stale 'd' */
    ASSERT_EQ_LL(measure_ex(s, strlen(s), 4, 0, NULL), SHR_OK);
    ASSERT_EQ_LL(at(3).column, 3); /* measure keeps the cut cluster where set_text blanks it */
    ASSERT_EQ_LL(at(3).cells, 2);
    ASSERT_EQ_LL(at(4).column, 5);
    ASSERT_EQ_LL(at(5).cells, 0);
    close_grid(l);
    PASS();
}

TEST set_text_tab_stops_count_from_the_start_column(void) {
    shr_lyr *l = open_grid(1, 8, NULL);
    ASSERT_EQ_LL(set_text(l, 0, 2, "a\tb", plain, 0, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "..a...b.");
    close_grid(l);
    PASS();
}

/* Only the grid is laid out: text beyond the coordinate limit still fits, yet all of it is validated. */
TEST set_text_stops_at_the_grid(void) {
    const size_t tabs = 349526;
    char *s = malloc(tabs + 1);
    ASSERT(s != NULL);
    memset(s, '\t', tabs);
    s[0] = 'a', s[tabs] = 0;
    shr_lyr *l = open_grid(2, 8, NULL);
    ASSERT_EQ_LL(measure_status(s, tabs, NULL), SHR_E_LIMIT);
    ASSERT_EQ_LL(set_text(l, 0, 0, s, plain, 0, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "a.......");
    s[0] = 'b';
    ASSERT_EQ_LL(set_text(l, 0, 7, s, plain, SHR_TEXT_WRAP, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "a......b");
    s[1] = 'c', s[2] = '\n', s[3] = 'd', s[4] = '\n', s[5] = 'e';
    ASSERT_EQ_LL(set_text(l, 0, 0, s, plain, 0, NULL), SHR_OK);
    ASSERT_STR_EQ(row_text(l, 0), "bc.....b");
    ASSERT_STR_EQ(row_text(l, 1), "d.......");
    s[tabs - 1] = (char)0x80;
    shr_error_info err;
    ASSERT_EQ_LL(set_text(l, 0, 0, s, plain, 0, &err), SHR_E_INVALID_UTF8);
    ASSERT_EQ_LL(err.byte_offset, tabs - 1);
    s[tabs - 1] = '\t';
    memcpy(s + tabs - 3, "\xEA\xB0\x80", 3);
    ASSERT_EQ_LL(set_text(l, 1, 7, s, plain, SHR_TEXT_WRAP, &err), SHR_E_CLUSTER_TOO_WIDE); /* as measure says */
    ASSERT_EQ_LL(err.byte_offset, tabs - 3);
    ASSERT_STR_EQ(row_text(l, 1), "d.......");
    free(s);
    close_grid(l);
    PASS();
}

TEST background_paints_cells_without_their_own(void) {
    const shr_color red = RED, blue = BLUE;
    shr_lyr *l = open_grid(2, 8, NULL);
    ASSERT_EQ_LL(set_cell(l, 0, 1, "A", 1, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 3, "", 1, on_blue), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 5, "\xEA\xB0\x80", 2, plain), SHR_OK);
    ASSERT_EQ_LL(count_kind(l, 0, SHR__LCMD_CACHE_BEGIN), 0);
    ASSERT(group_of(l, 1) == NULL);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, 2, 8, &red), SHR_OK);
    ASSERT_EQ_LL(count_kind(l, 0, SHR__LCMD_CACHE_BEGIN), 1); /* opaque now */
    ASSERT_EQ_LL(ncmds(l, 1), 3);                             /* cache, one fill, end */
    ASSERT_EQ_LL(cmd(l, 1, 1)->color, RED);
    uint64_t key = cmd(l, 1, 0)->key[0];
    const uint8_t *s = frame_of();
    const long want[8] = {0xFF0000, -1, 0xFF0000, 0x0000FF, 0xFF0000, -1, -1, 0xFF0000};
    for (int32_t c = 0; c < 8; c++) ASSERT_EQ_LL(cell_fill(s, 0, c), want[c]);
    ASSERT_EQ_LL(px(s, CW, 0), 0xFF0000);
    ASSERT_EQ_LL(px(s, 5 * CW, 0), 0xFF0000);
    for (int32_t c = 0; c < 8; c++) ASSERT_EQ_LL(cell_fill(s, 1, c), 0xFF0000);

    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 3, 1, 1, plain), SHR_OK); /* back to the background */
    s = frame_of();
    ASSERT_EQ_LL(cell_fill(s, 0, 3), 0xFF0000);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, 2, 8, &blue), SHR_OK);
    ASSERT(cmd(l, 1, 0)->key[0] != key); /* the background is part of the row key */
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, 2, 8, NULL), SHR_OK);
    ASSERT(group_of(l, 1) == NULL);
    s = frame_of();
    ASSERT_EQ_LL(cell_fill(s, 0, 0), 0);
    close_grid(l);
    PASS();
}

TEST row_commands_follow_cell_styles(void) {
    shr_lyr *l = open_grid(3, 8, NULL);
    const shr__line_metrics lm = shr__bitmap_font_line_metrics();
    shr__res *res = shr__bitmap_font_res(H.font);
    uint32_t all = SHR_STYLE_BG | SHR_STYLE_UNDERLINE | SHR_STYLE_STRIKE | SHR_STYLE_DIM | SHR_STYLE_BLINK;
    ASSERT_EQ_LL(set_cell(l, 0, 1, "A", 1, (shr_text_style){RED, BLUE, all}), SHR_OK);
    ASSERT_EQ_LL(ncmds(l, 0), 4);
    const shr__lcmd *bg = cmd(l, 0, 0), *g = cmd(l, 0, 1), *u = cmd(l, 0, 2), *s = cmd(l, 0, 3);
    shr_rect cell = {CW, 0, 2 * CW, CH};
    ASSERT_EQ_LL(bg->kind, SHR__LCMD_FILL);
    ASSERT_EQ_LL(bg->flags, 0);
    ASSERT_EQ_LL(bg->color, BLUE);
    ASSERT(memcmp(&bg->dst, &cell, sizeof(cell)) == 0);
    ASSERT_EQ_LL(g->kind, SHR__LCMD_GLYPH);
    ASSERT_EQ_LL(g->flags, SHR__LCMD_DIM | SHR__LCMD_BLINK);
    ASSERT_EQ_LL(g->color, RED);
    ASSERT(g->res == res);
    ASSERT_EQ_LL(g->id, 'A');
    ASSERT(memcmp(&g->dst, &cell, sizeof(cell)) == 0);
    ASSERT_EQ_LL(g->anchor.x, CW);
    ASSERT_EQ_LL(g->anchor.y, 0);
    ASSERT_EQ_LL(u->flags, SHR__LCMD_DIM | SHR__LCMD_BLINK);
    ASSERT_EQ_LL(u->color, RED);
    ASSERT_EQ_LL(u->dst.y0, lm.underline_y);
    ASSERT_EQ_LL(u->dst.y1, lm.underline_y + 1);
    ASSERT_EQ_LL(u->dst.x1, 2 * CW);
    ASSERT_EQ_LL(s->dst.y0, lm.strike_y);
    ASSERT_EQ_LL(s->dst.y1, lm.strike_y + 1);

    ASSERT_EQ_LL(set_cell(l, 1, 0, "A", 1, (shr_text_style){RED, BLUE, SHR_STYLE_BG | SHR_STYLE_CONCEAL | SHR_STYLE_UNDERLINE}),
                 SHR_OK);
    ASSERT_EQ_LL(ncmds(l, 1), 1); /* concealed: background only */
    ASSERT_EQ_LL(set_cell(l, 1, 3, "B", 1, (shr_text_style){RED, 0, SHR_STYLE_BOLD | SHR_STYLE_ITALIC}), SHR_OK);
    ASSERT_EQ_LL(ncmds(l, 1), 2);
    ASSERT_EQ_LL(cmd(l, 1, 1)->flags, 0);
    ASSERT_EQ_LL(cmd(l, 1, 1)->anchor.x, 3 * CW);
    ASSERT_EQ_LL(cmd(l, 1, 1)->anchor.y, CH);
    ASSERT_EQ_LL(cmd(l, 1, 1)->id, (uint64_t)3 << SHR_ID_STYLE_SHIFT | 'B');

    ASSERT_EQ_LL(set_cell(l, 2, 0, "", 2, (shr_text_style){RED, 0, SHR_STYLE_UNDERLINE}), SHR_OK);
    ASSERT_EQ_LL(ncmds(l, 2), 1); /* no glyph, just the line */
    ASSERT_EQ_LL(cmd(l, 2, 0)->dst.x1, 2 * CW);
    ASSERT_EQ_LL(cmd(l, 2, 0)->dst.y0, 2 * CH + lm.underline_y);
    close_grid(l);
    PASS();
}

TEST cache_hint_only_for_rows_covered_by_backgrounds(void) {
    shr_lyr *l = open_grid(2, 8, NULL);
    ASSERT_EQ_LL(set_text(l, 0, 0, "abcdefg", on_blue, 0, NULL), SHR_OK);
    ASSERT_EQ_LL(count_kind(l, 0, SHR__LCMD_CACHE_BEGIN), 0);
    ASSERT_EQ_LL(set_cell(l, 0, 7, "h", 1, on_blue), SHR_OK);
    const shr__lcmd *begin = cmd(l, 0, 0), *end = cmd(l, 0, ncmds(l, 0) - 1);
    shr_rect row = {0, 0, 8 * CW, CH};
    ASSERT_EQ_LL(begin->kind, SHR__LCMD_CACHE_BEGIN);
    ASSERT(memcmp(&begin->dst, &row, sizeof(row)) == 0);
    ASSERT_EQ_LL(end->kind, SHR__LCMD_CACHE_END);
    uint64_t key = begin->key[0];
    ASSERT_EQ_LL(set_cell(l, 0, 7, "i", 1, on_blue), SHR_OK);
    ASSERT(cmd(l, 0, 0)->key[0] != key);
    ASSERT_EQ_LL(set_cell(l, 0, 7, "h", 1, on_blue), SHR_OK);
    ASSERT_EQ_LL(cmd(l, 0, 0)->key[0], key);
    ASSERT_EQ_LL(set_cell(l, 0, 7, "h", 1, (shr_text_style){WHITE, BLUE, SHR_STYLE_BG | SHR_STYLE_BLINK}), SHR_OK);
    ASSERT(cmd(l, 0, 0)->key[0] != key);
    for (int32_t c = 0; c < 8; c += 2) ASSERT_EQ_LL(set_cell(l, 1, c, "\xEA\xB0\x80", 2, on_blue), SHR_OK);
    ASSERT_EQ_LL(cmd(l, 1, 0)->kind, SHR__LCMD_CACHE_BEGIN); /* spans count */
    close_grid(l);
    PASS();
}

typedef struct key_case {
    int32_t col;
    uint32_t span;
    shr_text_style style;
    const char *text;
    shr_color background;
    bool other_font, no_background;
} key_case;

/* The key of row 0 holding only `k`; the row must be cached. */
static XXH128_hash_t row_key(shr_lyr *l, shr_pl_res_bitmap_font *other, key_case k) {
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, k.other_font ? other : H.font, 1, 8, k.no_background ? NULL : &k.background),
                 SHR_OK);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 0, 1, 8, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, k.col, k.text, k.span, k.style), SHR_OK);
    const shr__lcmd *begin = cmd(l, 0, 0);
    ASSERT_EQ_LL(begin->kind, SHR__LCMD_CACHE_BEGIN);
    return (XXH128_hash_t){begin->key[0], begin->key[1]};
}

TEST row_key_follows_every_input(void) {
    shr_lyr *l = open_grid(1, 8, NULL);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = font_dir_open;
    shr_pl_res_bitmap_font *f2;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(H.ctx, &fd, &f2), SHR_OK);
    const key_case base = {1, 1, on_blue, "A", RED, false, false};
    key_case v[8];
    for (size_t i = 0; i < 8; i++) v[i] = base;
    v[0].col = 2;
    v[1].span = 2;
    v[2].style.fg = RED;
    v[3].style.bg = RED;
    v[4].style.flags |= SHR_STYLE_UNDERLINE;
    v[5].background = BLUE;
    v[6].other_font = true;
    v[7].text = "B";
    XXH128_hash_t k0 = row_key(l, f2, base);
    for (size_t i = 0; i < 8; i++) {
        ASSERT(!XXH128_isEqual(row_key(l, f2, v[i]), k0));
        ASSERT(XXH128_isEqual(row_key(l, f2, base), k0));
    }
    key_case a = base, b = base;
    a.col = b.col = 0, a.span = b.span = 8, a.background = b.background = 0, b.no_background = true;
    ASSERT(!XXH128_isEqual(row_key(l, f2, a), row_key(l, f2, b))); /* covered by the cell alone */
    /* Text that draws the same glyph (or none) keeps the key. */
    a = b = base;
    a.text = "", b.text = "\xE2\x80\x8B"; /* U+200B draws nothing */
    ASSERT(XXH128_isEqual(row_key(l, f2, a), row_key(l, f2, b)));
    a.text = "\xEF\xBF\xBD", b.text = "\xCC\x81"; /* a lone mark draws U+FFFD */
    ASSERT(XXH128_isEqual(row_key(l, f2, a), row_key(l, f2, b)));
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(f2), SHR_OK);
    close_grid(l);
    PASS();
}

TEST only_changed_rows_are_rebuilt(void) {
    shr_lyr *l = open_grid(3, 8, NULL);
    ASSERT_EQ_LL(set_text(l, 0, 0, "a\nb\nc", plain, 0, NULL), SHR_OK);
    const shr__lcmd *p0 = group_of(l, 0)->cmds, *p1 = group_of(l, 1)->cmds, *p2 = group_of(l, 2)->cmds;
    ASSERT_EQ_LL(set_cell(l, 1, 1, "x", 1, plain), SHR_OK);
    ASSERT(group_of(l, 0)->cmds == p0);
    ASSERT(group_of(l, 1)->cmds != p1);
    ASSERT(group_of(l, 2)->cmds == p2);
    p1 = group_of(l, 1)->cmds;
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 2, 0, 1, 1, plain), SHR_OK);
    ASSERT(group_of(l, 0)->cmds == p0);
    ASSERT(group_of(l, 1)->cmds == p1);
    ASSERT(group_of(l, 2) == NULL);
    ASSERT_EQ_LL(set_text(l, 0, 2, "y\n\nz", plain, 0, NULL), SHR_OK); /* row 1 lies between but is untouched */
    ASSERT(group_of(l, 0)->cmds != p0);
    ASSERT(group_of(l, 1)->cmds == p1);
    ASSERT_STR_EQ(row_text(l, 2), "..z.....");
    close_grid(l);
    PASS();
}

static bool cell_lit(const uint8_t *buf, int32_t row, int32_t col) {
    for (int y = row * CH; y < (row + 1) * CH; y++)
        for (int x = col * CW; x < (col + 1) * CW; x++)
            if (px(buf, x, y)) return true;
    return false;
}

TEST frames_show_backgrounds_glyphs_and_lines(void) {
    shr_lyr *l = open_grid(3, 8, NULL);
    const shr__line_metrics lm = shr__bitmap_font_line_metrics();
    ASSERT_EQ_LL(set_cell(l, 0, 0, "", 1, on_blue), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 0, 1, "A", 1, plain), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 1, 0, "", 1, (shr_text_style){WHITE, 0, SHR_STYLE_UNDERLINE}), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 1, 1, "", 1, (shr_text_style){WHITE, 0, SHR_STYLE_UNDERLINE | SHR_STYLE_DIM}), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 1, 2, "", 1, (shr_text_style){WHITE, 0, SHR_STYLE_STRIKE}), SHR_OK);
    ASSERT_EQ_LL(set_cell(l, 2, 0, "A", 1, (shr_text_style){WHITE, RED, SHR_STYLE_BG | SHR_STYLE_CONCEAL}), SHR_OK);
    ASSERT_EQ_LL(shr_submit(H.ctx), SHR_OK);
    settle(H.ctx);
    const uint8_t *s = H.out.shown;
    ASSERT_EQ_LL(px(s, 3, 3), 0x0000FF);
    ASSERT(cell_lit(s, 0, 1));
    ASSERT(!cell_lit(s, 0, 2));
    ASSERT_EQ_LL(px(s, 2, CH + lm.underline_y), 0xFFFFFF);
    ASSERT_EQ_LL(px(s, 2, CH + lm.underline_y - 1), 0);
    uint32_t half = px(s, CW + 2, CH + lm.underline_y);
    ASSERT(half >> 16 > 100 && half >> 16 < 160 && (half & 0xFF) > 100 && (half & 0xFF) < 160);
    ASSERT_EQ_LL(px(s, 2 * CW + 2, CH + lm.strike_y), 0xFFFFFF);
    for (int y = 2 * CH; y < 3 * CH; y++)
        for (int x = 0; x < CW; x++) ASSERT_EQ_LL(px(s, x, y), 0xFF0000);
    close_grid(l);
    PASS();
}

TEST calls_are_refused_while_shutting_down(void) {
    shr_lyr *l = open_grid(1, 1, NULL);
    ASSERT_EQ_LL(shr_begin_shutdown(H.ctx), SHR_OK);
    shr_error_info err;
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(l, H.font, 2, 2, NULL), SHR_E_STATE);
    ASSERT_EQ_LL(set_cell(l, 0, 0, "A", 1, plain), SHR_E_STATE);
    ASSERT_EQ_LL(set_text(l, 0, 0, "A", plain, 0, &err), SHR_E_STATE);
    ASSERT_EQ_LL(err.status, SHR_E_STATE);
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_clear(l, 0, 0, 1, 1, plain), SHR_E_STATE);
    close_grid(l);
    PASS();
}

static fail_alloc g_fa;
static shr_allocator g_al;

static void use_fail_alloc(shr_context_desc *d, shr_framebuffer_driver *drv) {
    (void)drv;
    g_al = fail_allocator(&g_fa);
    d->allocator = &g_al;
}

static shr_status oom_op(int op, shr_lyr *l, shr_pl_res_bitmap_font *font) {
    static const char family[] = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    static const char text[] = "a\t\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA6\n"
                               "e\xCC\x81 wrapped text runs on";
    const shr_text_style rich = {WHITE, BLUE, SHR_STYLE_BG | SHR_STYLE_UNDERLINE | SHR_STYLE_STRIKE};
    const shr_style_run run = {0, 1, {RED, RED, SHR_STYLE_BG}};
    const shr_color bg = BLUE;
    switch (op) {
    case 0: return shr_pl_lyr_tilemap_resize(l, font, 3, 7, NULL);
    case 1: return shr_pl_lyr_tilemap_clear(l, 2, 0, 1, 7, rich);
    case 2: return shr_pl_lyr_tilemap_set_cell(l, 1, 2, family, sizeof(family) - 1, 2, rich);
    case 3: return shr_pl_lyr_tilemap_set_text(l, 0, 1, text, sizeof(text) - 1, rich, &run, 1, SHR_TEXT_WRAP, NULL);
    default: return shr_pl_lyr_tilemap_resize(l, font, 2, 6, &bg);
    }
}

static void assert_same_rows(const shr_lyr *a, const shr_lyr *b) {
    ASSERT_EQ_LL(a->flush(a->state), SHR_OK);
    ASSERT_EQ_LL(b->flush(b->state), SHR_OK);
    ASSERT_EQ_LL(a->groups.len, b->groups.len);
    for (size_t i = 0; i < a->groups.len; i++) {
        const shr__group *ga = SHR_VEC_AT(&a->groups, shr__group, i), *gb = SHR_VEC_AT(&b->groups, shr__group, i);
        ASSERT_EQ_LL(ga->id, gb->id);
        ASSERT_EQ_LL(ga->n, gb->n);
        for (size_t k = 0; k < ga->n; k++) {
            const shr__lcmd *x = &ga->cmds[k], *y = &gb->cmds[k];
            ASSERT_EQ_LL(x->kind, y->kind);
            ASSERT_EQ_LL(x->flags, y->flags);
            ASSERT_EQ_LL(memcmp(&x->dst, &y->dst, sizeof(x->dst)), 0);
            ASSERT_EQ_LL(memcmp(&x->anchor, &y->anchor, sizeof(x->anchor)), 0);
            ASSERT_EQ_LL(x->color, y->color);
            ASSERT_EQ_LL(x->res == y->res, 1);
            ASSERT_EQ_LL(x->id, y->id);
            ASSERT_EQ_LL(x->key[0], y->key[0]);
            ASSERT_EQ_LL(x->key[1], y->key[1]);
        }
    }
}

/* Each operation, then the row rebuild at submit, fails at every allocation in turn: it reports
 * SHR_E_NO_MEMORY, repeating it succeeds and the rows end up as without failures (rows that failed to
 * rebuild stay dirty for the next submit). */
TEST out_of_memory_is_recoverable(void) {
    for (int op = 0; op < 5; op++) {
        shr_status st = SHR_E_NO_MEMORY;
        for (long budget = 0; st == SHR_E_NO_MEMORY; budget++) {
            g_fa = (fail_alloc){-1, 0};
            harness_open(&H, SHR_OUTPUT_RELEASE_ON_PRESENT, use_fail_alloc);
            harness_font(&H);
            shr_pl_res_bitmap_font_desc fd;
            shr_pl_res_bitmap_font_desc_init(&fd);
            fd.open = font_dir_open;
            shr_pl_res_bitmap_font *f2;
            ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(H.ctx, &fd, &f2), SHR_OK);
            shr_lyr *ref = new_layer(), *l = new_layer();
            for (int k = 0; k < op; k++) {
                ASSERT_EQ_LL(oom_op(k, l, H.font), SHR_OK);
                ASSERT_EQ_LL(oom_op(k, ref, H.font), SHR_OK);
            }
            g_fa.budget = budget;
            st = oom_op(op, l, f2);
            if (st == SHR_OK) st = l->flush(l->state);
            g_fa.budget = -1;
            ASSERT(st == SHR_OK || st == SHR_E_NO_MEMORY);
            if (st != SHR_OK) ASSERT_EQ_LL(oom_op(op, l, f2), SHR_OK);
            ASSERT_EQ_LL(oom_op(op, ref, f2), SHR_OK);
            assert_same_rows(ref, l);
            shr_lyr_destroy(ref);
            shr_lyr_destroy(l);
            ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(f2), SHR_OK);
            harness_close(&H);
            ASSERT_EQ_LL(g_fa.live, 0);
        }
    }
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(profile_and_limits_report_build_constants);
    RUN_TEST(measure_places_mixed_widths);
    RUN_TEST(cluster_widths_follow_emoji_and_selector_rules);
    RUN_TEST(tabs_and_newlines_advance_columns_and_rows);
    RUN_TEST(wrap_moves_whole_clusters_to_the_next_row);
    RUN_TEST(tab_spaces_continue_across_a_wrap);
    RUN_TEST(wrap_errors_without_room);
    RUN_TEST(invalid_utf8_and_controls_report_byte_offsets);
    RUN_TEST(clusters_longer_than_the_profile_limit_fail);
    RUN_TEST(measure_rejects_bad_arguments);
    RUN_TEST(measure_fills_up_to_capacity);
    RUN_TEST(coordinates_stop_at_the_limit);
    RUN_TEST(resize_attaches_and_validates_arguments);
    RUN_TEST(resize_refuses_layers_owned_elsewhere);
    RUN_TEST(resize_keeps_overlapping_cells_and_drops_cut_wide_cells);
    RUN_TEST(resize_switches_font);
    RUN_TEST(set_cell_validates_placed_input);
    RUN_TEST(set_cell_clears_the_wide_cells_it_overlaps);
    RUN_TEST(set_text_continues_lines_at_the_start_column);
    RUN_TEST(set_text_drops_what_leaves_the_grid);
    RUN_TEST(set_text_tabs_become_background_cells);
    RUN_TEST(set_text_errors_change_nothing);
    RUN_TEST(set_text_style_runs_on_cluster_boundaries);
    RUN_TEST(clear_resets_cells_to_the_background);
    RUN_TEST(clear_paints_both_halves_of_a_cut_wide_cell);
    RUN_TEST(set_text_blanks_a_cluster_cut_by_the_last_column);
    RUN_TEST(set_text_tab_stops_count_from_the_start_column);
    RUN_TEST(set_text_stops_at_the_grid);
    RUN_TEST(background_paints_cells_without_their_own);
    RUN_TEST(row_commands_follow_cell_styles);
    RUN_TEST(cache_hint_only_for_rows_covered_by_backgrounds);
    RUN_TEST(row_key_follows_every_input);
    RUN_TEST(only_changed_rows_are_rebuilt);
    RUN_TEST(frames_show_backgrounds_glyphs_and_lines);
    RUN_TEST(calls_are_refused_while_shutting_down);
    RUN_TEST(out_of_memory_is_recoverable);
    GREATEST_MAIN_END();
}
