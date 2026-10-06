#include "check.h"
#include "shr_text.h"

/* Controls other than TAB, LF and CR are rejected as text: U+2028 has the same break property. */
#define CONTROL_STANDIN 0x2028u

static size_t put_utf8(char *s, uint32_t cp) {
    if (cp < 0x80) return s[0] = (char)cp, 1;
    if (cp < 0x800) return s[0] = (char)(0xC0 | cp >> 6), s[1] = (char)(0x80 | (cp & 0x3F)), 2;
    if (cp < 0x10000)
        return s[0] = (char)(0xE0 | cp >> 12), s[1] = (char)(0x80 | (cp >> 6 & 0x3F)), s[2] = (char)(0x80 | (cp & 0x3F)), 3;
    s[0] = (char)(0xF0 | cp >> 18), s[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    s[2] = (char)(0x80 | (cp >> 6 & 0x3F)), s[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

TEST grapheme_breaks_match_uax29_test_file(void) {
    ASSERT_EQ_LL(shr__gcb(shr__uprops(CONTROL_STANDIN)), SHR_GCB_CONTROL);
    FILE *f = fopen(SHR_UCD_DIR "/GraphemeBreakTest.txt", "r");
    ASSERT(f != NULL);
    char line[4096];
    int cases = 0;
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char text[64 * 4];
        size_t breaks[64], nb = 0, len = 0, n = 0; /* byte offsets where a break is expected */
        char *p = line;
        while (*p) {
            if ((unsigned char)p[0] == 0xC3 && (unsigned char)p[1] == 0xB7) { /* ÷ */
                if (n > 0) breaks[nb++] = len;
                p += 2;
            } else if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F')) {
                uint32_t cp = (uint32_t)strtoul(p, &p, 16);
                if (shr__is_control(cp) && cp != '\t' && cp != '\n' && cp != '\r') cp = CONTROL_STANDIN;
                len += put_utf8(text + len, cp);
                n++;
            } else {
                p++;
            }
        }
        if (n == 0) continue;
        cases++;
        shr_text_cluster cl[64];
        shr_text_extent ext;
        ASSERT_EQ_LL(shr_pl_lyr_tilemap_measure(text, len, 0, 0, cl, 64, &ext, NULL), SHR_OK);
        bool same = ext.clusters == nb;
        for (size_t i = 0; same && i < nb; i++) same = cl[i].byte_offset + cl[i].byte_length == breaks[i];
        if (!same) fprintf(stderr, "  line: %s\n", line);
        ASSERT(same);
    }
    fclose(f);
    ASSERT(cases > 700);
    PASS();
}

/* Marks out[cp] = i + 1 where field 1 of a UCD line is names[i]. */
static bool load_ucd(const char *file, const char *const *names, size_t count, uint8_t *out) {
    char path[512], line[512];
    snprintf(path, sizeof(path), "%s/%s", SHR_UCD_DIR, file);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    while (fgets(line, sizeof(line), f)) {
        char *end = line + strcspn(line, "#\n"), *p;
        *end = 0;
        if (!strchr(line, ';')) continue;
        uint32_t lo = (uint32_t)strtoul(line, &p, 16), hi = lo;
        if (p[0] == '.' && p[1] == '.') hi = (uint32_t)strtoul(p + 2, &p, 16);
        p = strchr(p, ';') + 1;
        while (*p == ' ') p++;
        while (end > p && end[-1] == ' ') *--end = 0;
        for (size_t i = 0; i < count; i++)
            if (!strcmp(p, names[i])) memset(out + lo, (int)i + 1, hi - lo + 1);
    }
    fclose(f);
    return true;
}

TEST uprops_match_ucd_for_every_scalar(void) {
    static const char *const gcb[] = {"CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator", "Prepend",
                                      "SpacingMark", "L", "V", "T", "LV", "LVT"};
    static const char *const extpict[] = {"Extended_Pictographic"}, *const wide[] = {"W", "F"};
    enum { N = 0x110000 };
    uint8_t *g = calloc(N, 1), *e = calloc(N, 1), *w = calloc(N, 1);
    ASSERT(g && e && w);
    ASSERT(load_ucd("GraphemeBreakProperty.txt", gcb, 13, g));
    ASSERT(load_ucd("emoji-data.txt", extpict, 1, e));
    ASSERT(load_ucd("EastAsianWidth.txt", wide, 2, w));
    uint32_t bad = N;
    for (uint32_t cp = 0; cp < N && bad == N; cp++) {
        uint32_t p = shr__uprops(cp);
        if (shr__gcb(p) != g[cp] || !(p & SHR_UP_EXTPICT) != !e[cp] || !(p & SHR_UP_WIDE) != !w[cp]) bad = cp;
    }
    if (bad != N) fprintf(stderr, "U+%04X: props %X\n", bad, shr__uprops(bad));
    free(g), free(e), free(w);
    ASSERT_EQ_LL(bad, N);
    PASS();
}

TEST uprops_samples(void) {
    static const struct { uint32_t cp, props; } cases[] = {
        {0x0000F, SHR_GCB_CONTROL | SHR_UP_CC},
        {0x00010, SHR_GCB_CONTROL | SHR_UP_CC},
        {0x003FF, 0},
        {0x00400, 0},
        {0x0FFFF, 0},
        {0x10000, 0},
        {0x0ABFF, 0},
        {0x0AC00, SHR_GCB_LV | SHR_UP_WIDE | SHR_UP_CJK},
        {0x0D7A3, SHR_GCB_LVT | SHR_UP_WIDE | SHR_UP_CJK},
        {0x0D7A4, 0},
        {0x0DFFF, 0},
        {0x0E000, SHR_UP_CO | SHR_UP_NERD},
        {0x1F1E5, SHR_UP_EXTPICT},
        {0x1F1E6, SHR_GCB_RI | SHR_UP_EMOJI | SHR_UP_EPRES},
        {0x1F1FF, SHR_GCB_RI | SHR_UP_EMOJI | SHR_UP_EPRES},
        {0x10FFFD, SHR_UP_CO},
        {0x10FFFF, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t p = shr__uprops(cases[i].cp);
        if (p != cases[i].props) fprintf(stderr, "U+%04X: props %X, expected %X\n", cases[i].cp, p, cases[i].props);
        ASSERT_EQ_LL(p, cases[i].props);
    }
    ASSERT_EQ_LL(shr__uprops(0x110000), shr__uprops(0x10FFFF));
    ASSERT_EQ_LL(shr__uprops(UINT32_MAX), shr__uprops(0x10FFFF));
    PASS();
}

TEST emoji_selector_bases_and_sequences_start_with_an_emoji(void) { /* shr__classify relies on it */
    for (uint32_t i = 0; i < shr__emoji_vs_base_count; i++) ASSERT(shr__uprops(shr__emoji_vs_base[i]) & SHR_UP_EMOJI);
    for (uint32_t i = 0; i < shr__emoji_seq_count; i++)
        ASSERT(shr__uprops(shr__emoji_seq_pool[shr__emoji_seq_index[i].offset]) & SHR_UP_EMOJI);
    PASS();
}

TEST seq_cmp_orders_by_prefix_then_length(void) {
    const uint32_t a[2] = {1, 2}, b[2] = {1, 3};
    ASSERT_EQ_LL(shr__seq_cmp(a, 2, a, 2), 0);
    ASSERT_EQ_LL(shr__seq_cmp(a, 2, a, 1), 1);
    ASSERT_EQ_LL(shr__seq_cmp(a, 1, a, 2), -1);
    ASSERT_EQ_LL(shr__seq_cmp(a, 2, b, 2), -1);
    ASSERT_EQ_LL(shr__seq_cmp(b, 2, a, 2), 1);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(grapheme_breaks_match_uax29_test_file);
    RUN_TEST(uprops_match_ucd_for_every_scalar);
    RUN_TEST(uprops_samples);
    RUN_TEST(emoji_selector_bases_and_sequences_start_with_an_emoji);
    RUN_TEST(seq_cmp_orders_by_prefix_then_length);
    GREATEST_MAIN_END();
}
