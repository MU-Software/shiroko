#include <stdio.h>

#include "check.h"
#include "shr_alloc.h"
#include "shr_err.h"
#include "shr_hash.h"
#include "shr_lru.h"
#include "shr_rect.h"
#include "shr_unicode.h"
#include "shr_vec.h"

TEST checked_sizes_detect_overflow(void) {
    size_t r = 0;
    ASSERT(shr__add_size(SIZE_MAX - 1, 1, &r));
    ASSERT_EQ_LL(r == SIZE_MAX, 1);
    ASSERT(!shr__add_size(SIZE_MAX, 1, &r));
    ASSERT(shr__mul_size(SIZE_MAX / 2, 2, &r));
    ASSERT(!shr__mul_size(SIZE_MAX / 2 + 1, 2, &r));
    PASS();
}

TEST alloc_init_defaults_and_rejects_half_allocators(void) {
    shr__alloc al;
    ASSERT(shr__alloc_init(&al, NULL));
    ASSERT(al.a.alloc == shr__default_alloc && al.a.free == shr__default_free);
    shr_allocator a = {NULL, NULL, NULL};
    ASSERT(shr__alloc_init(&al, &a));
    ASSERT(al.a.alloc == shr__default_alloc);
    a = (shr_allocator){NULL, fa_alloc, NULL};
    ASSERT(!shr__alloc_init(&al, &a));
    a = (shr_allocator){NULL, NULL, fa_free};
    ASSERT(!shr__alloc_init(&al, &a));
    fail_alloc f = {-1, 0};
    a = fail_allocator(&f);
    ASSERT(shr__alloc_init(&al, &a));
    ASSERT(al.a.alloc == fa_alloc && al.a.user == &f);
    PASS();
}

TEST default_allocator_honours_large_alignment(void) {
    shr__alloc al;
    shr__alloc_init(&al, NULL);
    void *p = shr__malloc(&al, 3, 256, SHR_ALLOC_PAYLOAD);
    ASSERT(p && (uintptr_t)p % 256 == 0);
    shr__free(&al, p, 3, 256, SHR_ALLOC_PAYLOAD);
    uint64_t *q = SHR_NEW_ARRAY(&al, uint64_t, 4);
    ASSERT(q && q[0] == 0 && q[3] == 0);
    SHR_FREE_ARRAY(&al, q, uint64_t, 4);
    PASS();
}

TEST calloc_zeroes_and_fails_cleanly(void) {
    fail_alloc f = {-1, 0};
    shr_allocator a = fail_allocator(&f);
    shr__alloc al;
    shr__alloc_init(&al, &a);
    ASSERT(!shr__calloc(&al, SIZE_MAX, 2, 8, SHR_ALLOC_PAYLOAD));
    ASSERT_EQ_LL(f.live, 0);
    uint8_t *z = shr__calloc(&al, 0, 4, 1, SHR_ALLOC_PAYLOAD); /* zero bytes still gives a pointer */
    ASSERT(z && z[0] == 0);
    shr__free(&al, z, 0, 1, SHR_ALLOC_PAYLOAD);
    shr_rect *r = SHR_NEW(&al, shr_rect);
    ASSERT(r && r->x0 == 0 && r->y1 == 0);
    SHR_DELETE(&al, r, shr_rect);
    shr__free(&al, NULL, 4, 1, SHR_ALLOC_PAYLOAD);
    ASSERT_EQ_LL(f.live, 0);
    f.budget = 0;
    ASSERT(!SHR_NEW(&al, shr_rect));
    PASS();
}

TEST vec_grows_by_doubling_and_keeps_contents(void) {
    fail_alloc f = {-1, 0};
    shr_allocator a = fail_allocator(&f);
    shr__alloc al;
    shr__alloc_init(&al, &a);
    shr__vec v;
    SHR_VEC_INIT(&v, uint32_t);
    for (uint32_t i = 0; i < 9; i++) {
        uint32_t *p = shr__vec_push(&v, &al);
        ASSERT(p && *p == 0);
        *p = i * 3;
    }
    ASSERT_EQ_LL(v.len, 9);
    ASSERT_EQ_LL(v.cap, 16);
    ASSERT(shr__vec_reserve(&v, &al, 7)); /* fits: no reallocation */
    ASSERT_EQ_LL(v.cap, 16);
    ASSERT(shr__vec_reserve(&v, &al, 40));
    ASSERT_EQ_LL(v.cap, 64);
    for (uint32_t i = 0; i < 9; i++) ASSERT_EQ_LL(*SHR_VEC_AT(&v, uint32_t, i), i * 3);
    shr__vec_free(&v, &al);
    ASSERT(!v.data && v.len == 0 && v.cap == 0);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

TEST vec_failures_leave_it_unchanged(void) {
    fail_alloc f = {0, 0};
    shr_allocator a = fail_allocator(&f);
    shr__alloc al;
    shr__alloc_init(&al, &a);
    shr__vec v;
    SHR_VEC_INIT(&v, uint32_t);
    ASSERT(!shr__vec_push(&v, &al));
    ASSERT(!v.data && v.len == 0);
    f.budget = 1;
    *(uint32_t *)shr__vec_push(&v, &al) = 7;
    ASSERT(!shr__vec_reserve(&v, &al, 100));
    ASSERT(!shr__vec_reserve(&v, &al, SIZE_MAX));             /* len + extra overflows */
    ASSERT(!shr__vec_reserve(&v, &al, SIZE_MAX / 2 + 2));     /* doubling overflows */
    ASSERT_EQ_LL(v.len, 1);
    ASSERT_EQ_LL(v.cap, 8);
    ASSERT_EQ_LL(*SHR_VEC_AT(&v, uint32_t, 0), 7);
    shr__vec huge = {NULL, 0, 0, SIZE_MAX / 4, 1}; /* bytes overflow */
    ASSERT(!shr__vec_reserve(&huge, &al, 1));
    shr__vec_free(&v, &al);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

typedef struct item {
    int id;
    shr__lru_node lru;
} item;

TEST lru_orders_most_recent_first(void) {
    item it[3] = {{0, {0}}, {1, {0}}, {2, {0}}};
    shr__lru_node *head = NULL;
    ASSERT(!shr__lru_oldest(head));
    for (int i = 0; i < 3; i++) shr__lru_push(&head, &it[i].lru);
    ASSERT_EQ_LL(SHR_CONTAINER(head, item, lru)->id, 2);
    ASSERT_EQ_LL(SHR_CONTAINER(shr__lru_oldest(head), item, lru)->id, 0);
    shr__lru_remove(&head, &it[0].lru); /* oldest */
    ASSERT(!it[0].lru.prev && !it[0].lru.next);
    shr__lru_push(&head, &it[0].lru);   /* touched: 0, 2, 1 */
    ASSERT_EQ_LL(SHR_CONTAINER(shr__lru_oldest(head), item, lru)->id, 1);
    shr__lru_remove(&head, &it[2].lru); /* middle: 0, 1 */
    ASSERT(head == &it[0].lru && head->next == &it[1].lru && shr__lru_oldest(head) == &it[1].lru);
    shr__lru_remove(&head, &it[0].lru); /* newest: 1 */
    ASSERT(head == &it[1].lru && shr__lru_oldest(head) == &it[1].lru);
    shr__lru_remove(&head, &it[1].lru);
    ASSERT(!head);
    PASS();
}

TEST rect_helpers(void) {
    ASSERT(shr__rect_empty((shr_rect){0, 0, 0, 5}));
    ASSERT(shr__rect_empty((shr_rect){0, 0, 5, 0}));
    ASSERT(!shr__rect_empty((shr_rect){0, 0, 1, 1}));
    ASSERT(shr__rect_valid((shr_rect){1, 1, 1, 1}));
    ASSERT(!shr__rect_valid((shr_rect){2, 0, 1, 1}));
    ASSERT(!shr__rect_valid((shr_rect){0, 2, 1, 1}));
    shr_rect i = shr__rect_intersect((shr_rect){0, 0, 10, 10}, (shr_rect){5, -3, 20, 7});
    ASSERT(i.x0 == 5 && i.y0 == 0 && i.x1 == 10 && i.y1 == 7);
    i = shr__rect_intersect((shr_rect){5, -3, 20, 7}, (shr_rect){0, 0, 10, 10});
    ASSERT(i.x0 == 5 && i.y0 == 0 && i.x1 == 10 && i.y1 == 7);
    i = shr__rect_intersect((shr_rect){0, 0, 4, 4}, (shr_rect){6, 1, 9, 3}); /* disjoint: collapsed */
    ASSERT(i.x0 == 6 && i.x1 == 6 && i.y0 == 1 && i.y1 == 1);
    shr_rect a = {1, 2, 3, 4}, e = {9, 9, 9, 9};
    shr_rect u = shr__rect_union(e, a);
    ASSERT(u.x0 == 1 && u.y1 == 4);
    u = shr__rect_union(a, e);
    ASSERT(u.x0 == 1 && u.y1 == 4);
    u = shr__rect_union(a, (shr_rect){-1, 3, 2, 8});
    ASSERT(u.x0 == -1 && u.y0 == 2 && u.x1 == 3 && u.y1 == 8);
    u = shr__rect_union((shr_rect){-1, 3, 2, 8}, a);
    ASSERT(u.x0 == -1 && u.y0 == 2 && u.x1 == 3 && u.y1 == 8);
    ASSERT_EQ_LL(shr__rect_area((shr_rect){-1000000000, 0, 1000000000, 3}), 6000000000ll);
    ASSERT(shr__fits_i32(INT32_MIN) && shr__fits_i32(INT32_MAX));
    ASSERT(!shr__fits_i32((int64_t)INT32_MIN - 1) && !shr__fits_i32((int64_t)INT32_MAX + 1));
    PASS();
}

TEST reads_little_endian(void) {
    const uint8_t b[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0xF8};
    ASSERT_EQ_LL(shr__rd16(b), 0x0201);
    ASSERT_EQ_LL(shr__rd16(b + 6), 0xF807);
    ASSERT_EQ_LL(shr__rd32(b), 0x04030201);
    ASSERT_EQ_LL(shr__rd32(b + 4), 0xF8070605u);
    ASSERT_EQ_LL(shr__rd64(b) == 0xF807060504030201ull, 1);
    PASS();
}

TEST error_info_helpers(void) {
    shr_error_info e;
    ASSERT_EQ_LL(shr__fail(&e, SHR_E_FORMAT, 12, 3, "bad"), SHR_E_FORMAT);
    ASSERT(e.status == SHR_E_FORMAT && e.byte_offset == 12 && e.item_index == 3 && strcmp(e.reason, "bad") == 0);
    ASSERT_EQ_LL(shr__fail(NULL, SHR_E_IO, 0, 0, NULL), SHR_E_IO);
    shr__err_clear(&e);
    ASSERT(e.status == SHR_OK && e.byte_offset == SIZE_MAX && e.item_index == SIZE_MAX && !e.reason);
    shr__err_clear(NULL);
    PASS();
}

TEST status_names(void) {
    ASSERT_STR_EQ("OK", shr_status_name(SHR_OK));
    ASSERT_STR_EQ("IN_PROGRESS", shr_status_name(SHR_IN_PROGRESS));
    ASSERT_STR_EQ("NO_MEMORY", shr_status_name(SHR_E_NO_MEMORY));
    ASSERT_STR_EQ("WOULD_BLOCK", shr_status_name(SHR_E_WOULD_BLOCK));
    ASSERT_STR_EQ("DEVICE", shr_status_name(SHR_E_DEVICE));
    for (int s = SHR_OK; s <= SHR_E_DEVICE; s++) ASSERT(strcmp(shr_status_name((shr_status)s), "UNKNOWN") != 0);
    ASSERT_STR_EQ("UNKNOWN", shr_status_name((shr_status)(SHR_E_DEVICE + 1)));
    ASSERT_STR_EQ("UNKNOWN", shr_status_name((shr_status)-1));
    PASS();
}

TEST unicode_property_lookup(void) {
    ASSERT_STR_EQ("17.0.0", shr__unicode_version);
    ASSERT_EQ_LL(shr__tab_stop, 4);
    const struct {
        uint32_t cp;
        unsigned gcb, incb;
        uint32_t set, clear;
    } cases[] = {
        {0x0000, SHR_GCB_CONTROL, 0, SHR_UP_CC, SHR_UP_WIDE},
        {'\r', SHR_GCB_CR, 0, SHR_UP_CC, 0},
        {'\n', SHR_GCB_LF, 0, SHR_UP_CC, 0},
        {'A', SHR_GCB_OTHER, 0, 0, SHR_UP_WIDE | SHR_UP_MARK | SHR_UP_EMOJI | SHR_UP_CJK},
        {0x00AD, SHR_GCB_CONTROL, 0, SHR_UP_DI, 0},
        {0x0301, SHR_GCB_EXTEND, SHR_INCB_EXTEND, SHR_UP_MARK, SHR_UP_DI},
        {0x200D, SHR_GCB_ZWJ, SHR_INCB_EXTEND, SHR_UP_DI, 0},
        {0x0600, SHR_GCB_PREPEND, 0, 0, 0},
        {0x0903, SHR_GCB_SPACINGMARK, 0, SHR_UP_MARK, 0},
        {0x0915, SHR_GCB_OTHER, SHR_INCB_CONSONANT, 0, 0},
        {0x094D, SHR_GCB_EXTEND, SHR_INCB_LINKER, SHR_UP_MARK, 0},
        {0x1100, SHR_GCB_L, 0, SHR_UP_WIDE | SHR_UP_CJK, 0},
        {0x1161, SHR_GCB_V, 0, 0, SHR_UP_WIDE},
        {0x11A8, SHR_GCB_T, 0, 0, SHR_UP_WIDE},
        {0xAC00, SHR_GCB_LV, 0, SHR_UP_WIDE, 0},
        {0xAC01, SHR_GCB_LVT, 0, SHR_UP_WIDE, 0},
        {0x1F1E6, SHR_GCB_RI, 0, SHR_UP_EMOJI | SHR_UP_EPRES, SHR_UP_WIDE},
        {0x1F600, SHR_GCB_OTHER, 0, SHR_UP_EXTPICT | SHR_UP_EMOJI | SHR_UP_EPRES | SHR_UP_WIDE, 0},
        {0x1F3FB, SHR_GCB_EXTEND, SHR_INCB_EXTEND, SHR_UP_EMOD | SHR_UP_EPRES | SHR_UP_WIDE, 0},
        {0x2764, SHR_GCB_OTHER, 0, SHR_UP_EXTPICT | SHR_UP_EMOJI, SHR_UP_EPRES | SHR_UP_WIDE},
        {0x4E00, SHR_GCB_OTHER, 0, SHR_UP_WIDE | SHR_UP_CJK, 0},
        {0xE000, SHR_GCB_OTHER, 0, SHR_UP_CO, SHR_UP_WIDE},
        {0xE0B0, SHR_GCB_OTHER, 0, SHR_UP_CO | SHR_UP_NERD, 0},
        {0xFE0F, SHR_GCB_EXTEND, SHR_INCB_EXTEND, SHR_UP_DI | SHR_UP_MARK, 0},
        {0x10FFFF, SHR_GCB_OTHER, 0, 0, SHR_UP_WIDE},
        {0x110000, SHR_GCB_OTHER, 0, 0, SHR_UP_WIDE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t p = shr__uprops(cases[i].cp);
        if (shr__gcb(p) != cases[i].gcb || shr__incb(p) != cases[i].incb || (p & cases[i].set) != cases[i].set ||
            (p & cases[i].clear))
        {
            fprintf(stderr, "U+%04X: props %X\n", cases[i].cp, p);
            FAIL();
        }
    }
    PASS();
}

TEST emoji_tables(void) {
    const uint32_t tech[] = {0x1F469, 0x200D, 0x1F4BB}, flag[] = {0x1F1F0, 0x1F1F7}, keycap[] = {'1', 0xFE0F, 0x20E3};
    ASSERT(shr__emoji_sequence(tech, 3));
    ASSERT(!shr__emoji_sequence(tech, 2));
    ASSERT(shr__emoji_sequence(flag, 2));
    ASSERT(shr__emoji_sequence(keycap, 3));
    ASSERT(!shr__emoji_sequence(keycap, 1));
    const uint32_t none[] = {0x10FFFF, 0x10FFFF};
    ASSERT(!shr__emoji_sequence(none, 2));
    ASSERT(!shr__emoji_sequence(none, 0));
    ASSERT(shr__emoji_vs_registered(0x2764));
    ASSERT(shr__emoji_vs_registered('#'));
    ASSERT(!shr__emoji_vs_registered('A'));
    ASSERT(!shr__emoji_vs_registered(0x1F600));
    ASSERT(!shr__emoji_vs_registered(0x10FFFF));
    ASSERT(!shr__emoji_vs_registered(0));
    const uint32_t a[] = {1, 2, 3}, b[] = {1, 2, 4};
    ASSERT_EQ_LL(shr__seq_cmp(a, 3, a, 3), 0);
    ASSERT_EQ_LL(shr__seq_cmp(a, 3, b, 3), -1);
    ASSERT_EQ_LL(shr__seq_cmp(b, 3, a, 3), 1);
    ASSERT_EQ_LL(shr__seq_cmp(a, 2, a, 3), -1);
    ASSERT_EQ_LL(shr__seq_cmp(a, 3, a, 2), 1);
    PASS();
}

typedef struct cls_case {
    uint32_t cps[4];
    size_t n;
    int32_t cells;
    uint32_t flags;
    uint8_t kind;
    uint32_t glyph;
} cls_case;

TEST classify_width_rules(void) {
    enum { R = SHR_CLUSTER_REPLACEMENT, I = SHR_CLUSTER_INVISIBLE, E = SHR_CLUSTER_EMOJI };
    enum { NO = SHR_GLYPH_NONE, SC = SHR_GLYPH_SCALAR, SQ = SHR_GLYPH_SEQUENCE, RP = SHR_GLYPH_REPLACEMENT };
    const cls_case cases[] = {
        {{0}, 0, 0, I, NO, 0},
        {{0x0301}, 1, 1, R, RP, 0},                              /* lone mark */
        {{0xFE0F}, 1, 0, I, NO, 0},                              /* lone selector */
        {{0x200B}, 1, 0, I, NO, 0},                              /* ZWSP */
        {{'A'}, 1, 1, 0, SC, 'A'},
        {{0xFFFD}, 1, 1, 0, SC, 0xFFFD},
        {{0x3000}, 1, 2, 0, SC, 0x3000},
        {{0x1F3FB}, 1, 2, E, SC, 0x1F3FB},                       /* modifier alone */
        {{0x26A1}, 1, 2, E, SC, 0x26A1},                         /* emoji presentation */
        {{0x2764}, 1, 1, 0, SC, 0x2764},                         /* text default */
        {{0x23FB}, 1, 1, 0, SC, 0x23FB},                         /* EAW N */
        {{0xE0B0}, 1, 1, 0, SC, 0xE0B0},                         /* Powerline PUA */
        {{0x2764, 0xFE0F}, 2, 2, E, SC, 0x2764},
        {{0x1F44D, 0x1F3FD}, 2, 2, E, SQ, 0},
        {{'A', 0xFE0F}, 2, 1, 0, SC, 'A'},                       /* unregistered base */
        {{'A', 0xFE0E}, 2, 1, 0, SC, 'A'},
        {{0x231A, 0xFE0E}, 2, 2, 0, SC, 0x231A},                 /* text, EAW W */
        {{0x2600, 0xFE0E}, 2, 1, 0, SC, 0x2600},                 /* text, EAW N */
        {{0x26A1, 0xFE0E}, 2, 2, 0, SC, 0x26A1},                 /* text, EAW W keeps 2 */
        {{0x1F600, 0xFE0F}, 2, 2, E, SC, 0x1F600},               /* unregistered: text path */
        {{'e', 0x0301}, 2, 1, 0, SQ, 0},
        {{0xAC00, 0x11A8}, 2, 2, 0, SQ, 0},
        {{0x1F1F0, 0x1F1F7}, 2, 2, E, SQ, 0},
        {{0x2764, 0x0301}, 2, 1, 0, SQ, 0},                      /* no selector */
        {{0x1100, 0x1161, 0x11A8}, 3, 2, 0, SQ, 0},
        {{0x1F469, 0x200D, 0x1F4BB}, 3, 2, E, SQ, 0},
        {{0x1F441, 0x200D, 0x1F5E8}, 3, 2, E, SQ, 0},            /* unqualified alias */
        {{'1', 0xFE0F, 0x20E3}, 3, 2, E, SQ, 0},                 /* keycap */
        {{0x2764, 0xFE0F, 0x0301}, 3, 2, E, SQ, 0},              /* selector then only marks */
        {{0x2764, 0xFE0F, 0xFE0F}, 3, 2, E, SQ, 0},              /* a second selector is ignorable */
        {{0x231A, 0xFE0E, 0x0301}, 3, 2, 0, SQ, 0},
        {{0x2600, 0xFE0E, 0x0301}, 3, 1, 0, SQ, 0},
        {{0x2600, 0xFE0E, 0x1F3FB}, 3, 2, 0, SQ, 0},             /* a modifier is a second base */
        {{0x0301, 0x0302}, 2, 1, R, RP, 0},
        {{0x200B, 0xFE0F}, 2, 0, I, NO, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const cls_case *c = &cases[i];
        shr__cluster_class k;
        memset(&k, 0xAA, sizeof(k));
        shr__classify(c->cps, c->n, &k);
        if (k.cells != c->cells || k.flags != c->flags || k.glyph_kind != c->kind || k.glyph_cp != c->glyph) {
            fprintf(stderr, "case %zu (U+%04X): cells %d flags %u kind %u cp %X\n", i, c->cps[0], k.cells, k.flags,
                    k.glyph_kind, k.glyph_cp);
            FAIL();
        }
    }
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(checked_sizes_detect_overflow);
    RUN_TEST(alloc_init_defaults_and_rejects_half_allocators);
    RUN_TEST(default_allocator_honours_large_alignment);
    RUN_TEST(calloc_zeroes_and_fails_cleanly);
    RUN_TEST(vec_grows_by_doubling_and_keeps_contents);
    RUN_TEST(vec_failures_leave_it_unchanged);
    RUN_TEST(lru_orders_most_recent_first);
    RUN_TEST(rect_helpers);
    RUN_TEST(reads_little_endian);
    RUN_TEST(error_info_helpers);
    RUN_TEST(status_names);
    RUN_TEST(unicode_property_lookup);
    RUN_TEST(emoji_tables);
    RUN_TEST(classify_width_rules);
    GREATEST_MAIN_END();
}
