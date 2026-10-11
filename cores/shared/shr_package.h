#ifndef SHIROKO_SHR_PACKAGE_H
#define SHIROKO_SHR_PACKAGE_H

#include <stdbool.h>
#include <string.h>

#include "shr_hash.h"
#include "shr_unicode.h"

#define SHR_PKG_VERSION 6
#define SHR_PKG_HEADER 128
#define SHR_PKG_ENTRY 32 /* sidx entries */
#define SHR_PKG_PTAB 24  /* ptab entries */
#define SHR_PKG_MAX_BOXES 32
#define SHR_PKG_MAX_RECORDS (1u << 22)
#define SHR_PKG_MAX_INDEX (24 + SHR_PKG_ENTRY * SHR_PKG_MAX_BOXES)
#define SHR_PKG_MAX_META ((uint64_t)128 << 20)
#define SHR_PKG_REQUIRED 1u /* box flag */
#define SHR_PKG_ZSTD 8u     /* required feature: some box is zstd */
#ifdef SHR_ZSTD
#define SHR_PKG_FEATURES 15u
#else
#define SHR_PKG_FEATURES 7u
#endif
#define SHR_MAX_CELL_SIZE 1024

/* Package roles as stored in MANIFEST; the built-in package takes slot 0. */
enum { ROLE_BUILTIN, ROLE_LATIN, ROLE_CJK, ROLE_SYMBOLS, ROLE_EMOJI, ROLE_NERD, ROLE_COUNT };
/* The metadata boxes, in their file order. */
enum { BOX_MANI, BOX_STRS, BOX_SRCS, BOX_INST, BOX_CTRI, BOX_SEQS, BOX_POOL, BOX_PTAB, BOX_COVR, BOX_COUNT };

/* A metadata box from the index; size 0 = absent. */
typedef struct shr__box {
    uint64_t off, xxh3;
    uint32_t size, raw, flags;
} shr__box;

/* A package's header and index: `hdr` is its 128 header bytes, `size` its source's size. */
typedef struct shr__pkg_index {
    const uint8_t *hdr;
    uint64_t size;
    bool mapped;
    shr__box box[BOX_COUNT];
    uint8_t order[BOX_COUNT], nbox; /* the boxes present, by offset */
    uint64_t lo, hi, end;           /* the metadata region; the end of the last indexed box */
    uint32_t npages;
    size_t meta_len; /* bytes the decoded boxes take */
} shr__pkg_index;

/* Line positions from the cell top. */
typedef struct shr__line_metrics {
    int32_t baseline, underline_y, strike_y;
} shr__line_metrics;

/* What a package says it is, from its mani and inst boxes. */
typedef struct shr__pkg_desc {
    uint8_t role;
    char locale[9];
    uint16_t cell_width, line_height, page_width, page_height;
    uint8_t format;
    shr__line_metrics lines;
} shr__pkg_desc;

/* The built-in package (ASCII + U+FFFD), baked by the build into the library for SHR_CELL_WIDTH x SHR_CELL_HEIGHT. */
extern const uint8_t shr__builtin_package[];
extern const size_t shr__builtin_package_size;

/* The metadata box types, 4 characters each by BOX_. */
extern const char shr__box_types[];

/* Bytes the decoded box takes, in multiples of 64: none when stored in a mapped package (used in place). */
static inline size_t shr__box_room(const shr__pkg_index *ix, const shr__box *b) {
    return (((size_t)b->raw + 63) & ~(size_t)63) * ((ix->mapped == 0) | (b->flags >> 4));
}

/* A stored payload of `n` bytes is raw_size, a zstd one smaller. */
bool shr__raw_bad(uint32_t method, uint64_t n, uint32_t raw);
/* Whether the box header at `x` is the index entry's. */
bool shr__box_header_is(const shr__box *b, const char *type, const uint8_t *x);

/* Each check returns the status of the first rule `ix` fails, with its reason in `why`. */
shr_status shr__pkg_header(const shr__pkg_index *ix, const char **why);
/* The index `s` (the header's sidx_size bytes): fills the boxes and the metadata region. */
shr_status shr__pkg_index_parse(shr__pkg_index *ix, const uint8_t *s, const char **why);
/* The decoded mani box: with `role` (not ROLE_BUILTIN) it must be that role, a CJK one of `locale`. */
shr_status shr__pkg_mani(const shr__pkg_index *ix, const uint8_t *man, uint8_t role, const char *locale,
                         const char **why);
/* The decoded inst box, at any cell size. */
shr_status shr__pkg_inst(const shr__pkg_index *ix, const uint8_t *in, const char **why);

/* Checks the header, the index and the mani and inst boxes of a package, read from `src` synchronously, and
 * describes it. A read that returns SHR_IN_PROGRESS is cancelled (a source that may return it has cancel()) and
 * gives SHR_E_UNSUPPORTED; SHR_E_WOULD_BLOCK is returned, other failed reads give SHR_E_IO. */
shr_status shr__pkg_describe(const shr_asset_source *src, shr__pkg_desc *out, const char **why);

#endif
