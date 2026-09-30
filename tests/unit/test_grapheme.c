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
    RUN_TEST(seq_cmp_orders_by_prefix_then_length);
    GREATEST_MAIN_END();
}
