#ifndef SHIROKO_SHR_TEXT_H
#define SHIROKO_SHR_TEXT_H

#include <shiroko/shiroko.h>

#include "shr_err.h"
#include "shr_unicode.h"

#define SHR_MAX_TEXT_BYTES ((size_t)1 << 20)
#define SHR_CLUSTER_SCALARS 16u /* buffer size; the generator keeps the profile's cluster limit at most 16 */
#define SHR_MAX_SPAN 256u
#define SHR_MAX_GRID (1 << 15)
#define SHR_MAX_LAYOUT_COORD (1 << 20)

const char *shr__utf8_next(const uint8_t *s, size_t len, size_t *pos, uint32_t *cp);

static inline bool shr__is_control(uint32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F); }

static inline shr_status shr__style_check(const shr_text_style *s, shr_error_info *err, size_t item) {
    if (s->flags & ~SHR_STYLE_KNOWN_FLAGS) return shr__fail(err, SHR_E_UNKNOWN_STYLE, 0, item, "unknown style flag");
    return SHR_OK;
}

typedef struct shr__piece {
    size_t byte_offset, byte_length;
    int32_t row, column, cells;
    uint32_t flags, style;         /* style: 0 = base style, else run index + 1 */
    const uint32_t *cps;           /* the cluster's scalars during emit() */
    size_t n;
    const shr__cluster_class *cls; /* NULL for newlines and TABs */
} shr__piece;

typedef struct shr__layout_in {
    const char *utf8;
    size_t len;
    const shr_style_run *runs;
    size_t run_count;
    bool wrap;
    int64_t avail_cols;
    /* > 0: only pieces inside avail_rows x avail_cols are emitted and coordinates stop there. */
    int64_t avail_rows;
    shr_status (*emit)(void *user, const shr__piece *piece);
    void *user;
} shr__layout_in;

/* Validates the whole text and emits its pieces in text order; an emit() error ends the layout. */
shr_status shr__layout(const shr__layout_in *in, int32_t *rows, int32_t *cols, shr_error_info *err);

#endif
