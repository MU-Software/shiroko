#ifndef SHIROKO_FONT_CORE_H
#define SHIROKO_FONT_CORE_H

#include "shr_bitmap_font.h"
#include "shr_hash.h"
#include "shr_lru.h"

#define SHR_PKG_VERSION 5
#define SHR_PKG_HEADER 128
#define SHR_PKG_ENTRY 32 /* sidx and ptab entries */
#define SHR_PKG_MAX_BOXES 32
#define SHR_PKG_MAX_RECORDS (1u << 22)
#define SHR_PKG_MAX_PAGES (1u << 16)
#define SHR_PKG_MAX_PAGE_GLYPHS 4096
#define SHR_PKG_MAX_PAGE_BYTES (1u << 20)
#define SHR_PKG_PAGE_ALIGN 256 /* stored atlases in the file */
#define SHR_PKG_MAX_META ((uint64_t)128 << 20)
#define SHR_PKG_MAX_WINDOW (1u << 18)
#define SHR_PKG_REQUIRED 1u /* box flag */
#define SHR_PKG_ZSTD 8u     /* required feature: some box is zstd */
#ifdef SHR_ZSTD
#define SHR_PKG_FEATURES 15u
#else
#define SHR_PKG_FEATURES 7u
#endif
#define SHR_MAX_CELL_SIZE 1024
#define SHR_FONT_MAX_CLUSTERS (1u << 16)

/* Package roles as stored in MANIFEST; the built-in package takes slot 0. */
enum { ROLE_BUILTIN, ROLE_LATIN, ROLE_CJK, ROLE_SYMBOLS, ROLE_EMOJI, ROLE_NERD, ROLE_COUNT };
enum { PKG_UNOPENED, PKG_ABSENT, PKG_LOADING, PKG_READY, PKG_FAILED };
enum { LOAD_HEADER, LOAD_INDEX, LOAD_META };
enum { PAGE_ABSENT, PAGE_LOADING, PAGE_READY, PAGE_FAILED };
enum { GLYPH_READY, GLYPH_PENDING, GLYPH_MISSING, GLYPH_NO_MEMORY };
/* The metadata boxes, in their file order. */
enum { BOX_MANI, BOX_STRS, BOX_SRCS, BOX_INST, BOX_CMAP, BOX_SEQS, BOX_POOL, BOX_PTAB, BOX_COVR, BOX_COUNT };

/* Attempts of a package or page. A failed or refused one runs again from `at` or once a source signals
 * readiness after `ready` was read. One waiting for memory (`wake` set) runs again once the font's wake moved
 * on from it or a frame after `frame` wants glyphs. Beyond io_retry_limit failures it cools down (its glyphs
 * draw the provisional fallback) until `at` or a ready signal, and a frame after `frame` wants it again.
 * Without io_retry_ns, or after io_retry_limit cool-downs in a row, only a ready signal ends one. */
typedef struct shr__retry {
    uint64_t at, frame, wake;
    uint32_t count, cools, ready;
    bool cooling;
} shr__retry;

typedef struct shr__pkg shr__pkg;

/* A buffer pages are drawn from: a stored page's atlas in place (mapped), or memory of the page shape whose top rows
 * hold a page. A slot of the page cache moves on to the next page of its shape when its page is evicted, keeping the
 * buffer id. */
typedef struct shr__page_slot {
    shr__buf buf;
    uint64_t shape; /* shr__pkg.slot_shape of the packages whose pages it takes */
    uint64_t bytes; /* charged to the page cache; 0 outside it */
} shr__page_slot;

/* Exists while a page is wanted, loading, resident, cooling down or failed: one no frame wants any more
 * forgets its failed reads. */
typedef struct shr__page {
    shr__pkg *pkg;
    shr__page_slot *slot; /* while loading or resident */
    const uint8_t *recs;  /* resident: the glyph records, in place or `own` */
    uint8_t *own, *stage; /* the decoded records; the page box while it is read */
    uint64_t bytes;       /* of `own` and `stage` charged to the page cache */
    uint32_t index;
    uint8_t state;
    bool queued;       /* in the font's wants */
    uint64_t want;     /* the last frame that wanted or drew from it */
    uint64_t pin[2];   /* frames reading it; 0 = none */
    shr__retry retry;
    shr__lru_node lru; /* in the font's lru (unpinned) or pinned list */
} shr__page;

/* A metadata box from the index; size 0 = absent. */
typedef struct shr__box {
    uint64_t off, xxh3;
    uint32_t size, raw, flags;
} shr__box;

struct shr__pkg {
    shr_pl_res_bitmap_font *font;
    uint8_t role, state, step;
    bool mapped, step_busy, wanted;
    shr_asset_source src;
    const uint8_t *hdr; /* `header` or the mapped data */
    uint8_t header[SHR_PKG_HEADER];
    uint8_t *buf, *meta; /* the index or metadata region being read; the decoded metadata boxes */
    size_t buf_len, meta_len;
    shr__box box[BOX_COUNT];
    uint8_t order[BOX_COUNT], nbox; /* the boxes present, by offset */
    uint64_t lo, hi, end;           /* the metadata region; the end of the last indexed box */
    shr__retry retry;
    uint32_t nglyphs, ncmap, nseqs, npages;
    const uint8_t *cmap, *seqs, *pool, *ptab;
    uint8_t format;
    int16_t baseline;
    uint16_t atlas_w, atlas_h; /* the page shape */
    uint32_t stride;
    uint64_t slot_shape; /* format and page shape */
    shr__page **pages;   /* npages entries (from the metadata step), NULL until wanted */
};

typedef struct shr__cluster {
    uint64_t hash;
    uint32_t off;
    uint8_t n, kind;
    uint32_t cp;
} shr__cluster;

struct shr_pl_res_bitmap_font {
    shr__res res;
    shr__alloc al;
    void *user;
    shr_status (*open)(void *user, const char *package, shr_asset_source *out);
    const char *locale;
    uint64_t cache_bytes, retry_ns;
    uint32_t retry_limit;
    shr__pkg pkg[ROLE_COUNT];
    shr__lru_node *lru, *pinned; /* resident pages without and with pins */
    uint64_t page_bytes;
    uint64_t wake;     /* bumped when a read ends or cache space frees up */
    uint64_t cool_at;  /* earliest io_retry_ns end of a cool-down a frame ran into; 0 = none */
    uint32_t ready_seen; /* shr__ctx_asset_ready() when a cool-down last ended */
    bool cool_wait;    /* a frame ran into a cool-down, which a ready signal ends */
    bool cool_due;     /* a frame ran into a cool-down that is already over */
    uint64_t frame;    /* the latest frame that resolved glyphs */
    shr__vec wants;    /* shr__page *, wanted by a frame and not ready */
    shr__vec clusters; /* shr__cluster */
    shr__vec pool;     /* uint32_t scalars of the clusters */
    void *zdc;         /* the zstd decoder, from the first package that needs it */
    uint32_t *slots;   /* cluster index + 1, open addressing */
    size_t nslots;
    bool changed, blocked, shutting_down;
};

static inline uint64_t shr__font_now(const shr_pl_res_bitmap_font *f) { return shr__ctx_now(f->res.ctx); }

/* Logs a failure, queues SHR_EVENT_RESOURCE_FAILED and redraws provisional glyphs. */
void shr__font_report(shr_pl_res_bitmap_font *f, shr_status st, const char *why);
/* Before an attempt (open or read): later ready signals count for it. */
void shr__retry_begin(shr_pl_res_bitmap_font *f, shr__retry *r);
/* Refused (SHR_E_WOULD_BLOCK): again after io_retry_ns, or only at a ready signal without it. */
void shr__retry_refused(shr_pl_res_bitmap_font *f, shr__retry *r, uint64_t now);
/* Waits for memory: until pages are dropped or unpinned, a read ends or a later frame wants glyphs. */
void shr__retry_memory(shr_pl_res_bitmap_font *f, shr__retry *r);
bool shr__retry_due(const shr_pl_res_bitmap_font *f, const shr__retry *r, uint64_t now);
/* One more failed attempt, retried after io_retry_ns; true once io_retry_limit is exceeded. */
bool shr__retry_failed(shr_pl_res_bitmap_font *f, shr__retry *r);
void shr__retry_cool(shr_pl_res_bitmap_font *f, shr__retry *r, shr_status st, const char *why);
/* Whether a frame may want it; the first frame after it cooled down only records itself. */
bool shr__retry_cooled(shr_pl_res_bitmap_font *f, shr__retry *r, uint64_t frame);

/* Scalar sequences: `a` in a SEQPOOL, `b` in a SEQPOOL or (bytes NULL) `bc`. */
int shr__seq_cmp_pool(const uint8_t *a, size_t an, const uint8_t *b, const uint32_t *bc, size_t bn);

static inline uint64_t shr__io_tag(const shr__pkg *pkg, uint32_t page_plus_one) {
    return (uint64_t)pkg->role << 32 | page_plus_one;
}

static inline const uint8_t *shr__page_rec(const shr__page *p) { return p->pkg->ptab + SHR_PKG_ENTRY * (uint64_t)p->index; }
static inline uint32_t shr__page_length(const shr__page *p) { return shr__rd32(shr__page_rec(p) + 16); }
static inline size_t shr__page_recs(const shr__page *p) { return 16 * (size_t)shr__rd32(shr__page_rec(p) + 24); }

/* Bytes of `meta` a box takes: none when stored in a mapped package (used in place). */
static inline size_t shr__box_room(const shr__pkg *pkg, const shr__box *b) {
    return (((size_t)b->raw + 7) & ~(size_t)7) * ((pkg->mapped == 0) | (b->flags >> 4));
}

/* The streams of a page box. */
typedef struct shr__streams {
    const uint8_t *atlas, *recs;
    uint32_t atlas_n, recs_n, method;
} shr__streams;

/* package.c */
void shr__pkg_release(shr__pkg *pkg);
/* Opens a mapped package; SHR_E_NO_MEMORY: retry later. */
shr_status shr__pkg_map(shr__pkg *pkg, const char **why);
void shr__pkg_advance(shr__pkg *pkg);
bool shr__pkg_due(const shr__pkg *pkg, uint64_t now);
void shr__pkg_load_done(shr__pkg *pkg, shr_status result);
/* The streams of page box `b` (its checksum checked). */
shr_status shr__page_streams(const shr__page *p, const uint8_t *b, shr__streams *out);
/* The glyph records against the page's packed atlas. */
bool shr__page_valid(const shr__page *p, const uint8_t *atlas, const uint8_t *recs);

/* decode.c, the only part that knows zstd */
/* The font's decoder, once a package declares zstd in `features`. */
shr_status shr__zinit(shr_pl_res_bitmap_font *f, uint32_t features);
void shr__zfree(shr_pl_res_bitmap_font *f);
/* Box `b` at `x` (its checksum checked) whose header is its entry's: its payload, in place when stored and `place`,
 * else decoded into `dst`; NULL when the header differs or the payload does not decode. */
const uint8_t *shr__box_payload(shr_pl_res_bitmap_font *f, const shr__box *b, const char *type, const uint8_t *x,
                                uint8_t *dst, bool place);
/* The streams decoded into `atlas` (packed rows) and `recs`, then validated. */
bool shr__page_decode(shr_pl_res_bitmap_font *f, const shr__page *p, const shr__streams *s, uint8_t *atlas,
                      uint8_t *recs);

/* page_cache.c */
shr_status shr__pages_builtin(shr__pkg *pkg);
void shr__pages_free(shr__pkg *pkg);
void shr__pages_schedule(shr_pl_res_bitmap_font *f);
void shr__page_done(shr__page *p, shr_status result);
bool shr__page_due(const shr__page *p, uint64_t now);
void shr__font_frame_end(shr_pl_res_bitmap_font *f, uint64_t frame);
shr_status shr__font_resolve(shr_pl_res_bitmap_font *f, uint64_t id, uint64_t frame, shr__resolved *out);

/* Glyph id layout: scalar or cluster index, emoji presentation, interned cluster. */
#define SHR_ID_VALUE 0x1FFFFFull
#define SHR_ID_EMOJI (1ull << 23)
#define SHR_ID_CLUSTER (1ull << 24)

#endif
