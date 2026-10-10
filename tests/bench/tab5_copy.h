/* The Tab5 example's keep copier (examples/tab5): which copies it leaves to the CPU, and why. The example and the
 * host checks (replay with SHR_REPLAY_COPIER=tab5, test_tab5_copy) decide by this one function. */
#ifndef TAB5_COPY_H
#define TAB5_COPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAB5_COPY_LINE 128 /* the ESP32-P4's L2 cache line: the DMA writes whole ones from dst on */
#define TAB5_COPY_MIN 8192 /* smaller copies cost the CPU less than a DMA2D submission */
#ifndef TAB5_COPY_LEN
#define TAB5_COPY_LEN 2 /* row and source alignment once the boot self-test proved rows of any length right */
#endif
#define TAB5_COPY_LEN_SAFE 32 /* else */

/* SMALL and LINE (dst off a cache line: an image at any x) are by design; RULE alone (src or bytes off `len`, dst rows
 * off cache lines) means a copy the DMA2D could take goes to the CPU. */
enum { TAB5_COPY_SMALL = 1, TAB5_COPY_LINE_START = 2, TAB5_COPY_RULE = 4 };

/* Why a copy of rows of `bytes` from src to dst, `total` bytes in all, goes to the CPU; 0: the DMA2D takes it.
 * `packed`: dst rows may follow each other (dst_stride == bytes) instead of starting cache lines. */
static inline unsigned tab5_copy_why(uintptr_t dst, size_t dst_stride, uintptr_t src, size_t bytes, size_t total,
                                     size_t len, bool packed) {
    unsigned rule =
        ((src | bytes) % len != 0) | ((dst_stride % TAB5_COPY_LINE != 0) & !(packed & (dst_stride == bytes)));
    return (total < TAB5_COPY_MIN) * TAB5_COPY_SMALL | (dst % TAB5_COPY_LINE != 0) * TAB5_COPY_LINE_START |
           rule * TAB5_COPY_RULE;
}

#endif
