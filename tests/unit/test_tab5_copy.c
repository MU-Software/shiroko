/* The Tab5 example's keep copier (tests/bench/tab5_copy.h): for every cell size the build accepts, the keep copies the
 * example makes go to the DMA2D, unless small (a copy
 * left to the CPU for RULE alone fails). */
#include <stdio.h>

#include "harness.h"
#include "tab5_copy.h"

/* The example's geometry: the 1280 x 720 landscape screen in bands of 1280 x 16 at a cache line, keep slots of one
 * logical row (1280 x CH pixels) at cache lines, the grid from column 0. */
#define SCREEN_W 1280
#define SCREEN_H 720
#define BAND_H 16
#define BAND_AT ((uintptr_t)0x30000000u)
#define SLOT_AT ((uintptr_t)0x48000000u)

TEST why_names_each_reason(void) {
    ASSERT_EQ(tab5_copy_why(BAND_AT, 2560, SLOT_AT, 2560, 2560 * 16, TAB5_COPY_LEN, false), 0u);
    ASSERT_EQ(tab5_copy_why(BAND_AT, 2560, SLOT_AT, 2560, 2560 * 3, TAB5_COPY_LEN, false), (unsigned)TAB5_COPY_SMALL);
    ASSERT_EQ(tab5_copy_why(BAND_AT + 64, 2560, SLOT_AT, 2496, 2496 * 16, TAB5_COPY_LEN, false),
              (unsigned)TAB5_COPY_LINE_START);
    ASSERT_EQ(tab5_copy_why(BAND_AT, 2550, SLOT_AT, 2550, 2550 * 16, TAB5_COPY_LEN, false), (unsigned)TAB5_COPY_RULE);
    ASSERT_EQ(tab5_copy_why(SLOT_AT, 2544, BAND_AT, 2544, 2544 * 16, TAB5_COPY_LEN, true), 0u);
    ASSERT_EQ(tab5_copy_why(SLOT_AT, 2550, BAND_AT, 2550, 2550 * 16, TAB5_COPY_LEN, true), /* ends off a line */
              (unsigned)TAB5_COPY_LINE_START);
    ASSERT_EQ(tab5_copy_why(SLOT_AT, 2544, BAND_AT, 2544, 2544 * 16, TAB5_COPY_LEN_SAFE, true),
              (unsigned)TAB5_COPY_RULE);
    PASS();
}

/* Each keep part a band takes: whole rows stored from a band, drawn into one, and drawn trimmed from every
 * cache-line column to the row's end. */
TEST every_cell_size_copies_by_dma(void) {
    for (int32_t cw = 6; cw <= 64; cw++)
        for (int32_t ch = 8; ch <= 127; ch++) {
            size_t row = (size_t)(SCREEN_W / cw * cw * 2), stride = SCREEN_W * 2;
            for (int32_t y0 = 0; y0 + ch <= SCREEN_H; y0 += ch)
                for (int32_t b = y0 / BAND_H * BAND_H; b < y0 + ch; b += BAND_H) {
                    int32_t p0 = b > y0 ? b : y0, p1 = b + BAND_H < y0 + ch ? b + BAND_H : y0 + ch, n = p1 - p0;
                    uintptr_t band = BAND_AT + (size_t)(p0 - b) * stride, keep = SLOT_AT + (size_t)(p0 - y0) * row;
                    size_t all = row * (size_t)n;
                    bool lost = tab5_copy_why(band, stride, keep, row, all, TAB5_COPY_LEN, true) == TAB5_COPY_RULE;
                    if (n == ch) /* stored from the band */
                        lost |= tab5_copy_why(SLOT_AT, row, band, row, all, TAB5_COPY_LEN, true) == TAB5_COPY_RULE;
                    for (size_t c0 = TAB5_COPY_LINE; c0 < row; c0 += TAB5_COPY_LINE)
                        lost |= tab5_copy_why(band + c0, stride, keep + c0, row - c0, all, TAB5_COPY_LEN, false) ==
                                TAB5_COPY_RULE;
                    if (lost) {
                        fprintf(stderr, "cell %dx%d, rows %d..%d: a copy left to the CPU\n", (int)cw, (int)ch, (int)p0,
                                (int)p1);
                        FAIL();
                    }
                }
        }
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(why_names_each_reason);
    RUN_TEST(every_cell_size_copies_by_dma);
    GREATEST_MAIN_END();
}
