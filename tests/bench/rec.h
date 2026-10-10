/* Recordings of a scene, replayed into one part alone. Format "SHRR" version 2, little-endian: records [u8 type]
 * [u24 length][payload padded to 4 bytes], a header first and an END record last. Two kinds:
 * - commands: the batches a driver receives, recorded at its vtable. A command is 64 bytes holding only the fields its
 *   kind reads (the others zero) with pointers as (target or memory number, offset), so a recording does not depend
 *   on the machine. Buffer memory is recorded whole at REGISTER and by rows at UPDATE, which is all a driver may read
 *   (shiroko_driver.h).
 * - calls: the application's calls of the public API (rec_api.h) with the clock they ran at, replayed into the real
 *   plugins and compositor over a driver that draws nothing.
 * The recording hash is FNV-1a over the 32-bit words before END. */
#ifndef SHIROKO_BENCH_REC_H
#define SHIROKO_BENCH_REC_H

#include <shiroko/shiroko.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REC_VERSION 2
#define REC_TARGETS 4
#define REC_OPEN (-1) /* the frame index of everything before the first loop frame */
#define REC_WARM 10   /* loop frames before p50 and p95 count, as in the Tab5 example */
#define REC_COLS 6
enum { REC_KIND_CALLS = 1, REC_KIND_CMDS = 2 };
/* Record types both kinds share; the calls' own follow. */
enum { REC_HEADER = 1, REC_TARGET, REC_FRAME, REC_MEM, REC_MEMW, REC_BATCH, REC_END, REC_CALL0 = 16 };

static inline void rec_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static inline void rec_put64(uint8_t *p, uint64_t v) { rec_put32(p, (uint32_t)v), rec_put32(p + 4, (uint32_t)(v >> 32)); }
static inline uint32_t rec_get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t rec_get64(const uint8_t *p) { return rec_get32(p) | (uint64_t)rec_get32(p + 4) << 32; }
static inline void rec_put_rect(uint8_t *p, shr_rect r) {
    rec_put32(p, (uint32_t)r.x0), rec_put32(p + 4, (uint32_t)r.y0), rec_put32(p + 8, (uint32_t)r.x1);
    rec_put32(p + 12, (uint32_t)r.y1);
}
static inline float rec_ms(uint64_t ns) { return (float)ns / 1e6f; }
/* For qsort of floats. */
static inline int rec_cmp_float(const void *x, const void *y) {
    float p = *(const float *)x, q = *(const float *)y;
    return (p > q) - (p < q);
}

typedef struct rec_package {
    const char *name;
    uint64_t size;
    uint32_t hash; /* rec_hash over the file */
} rec_package;

/* What decides the commands besides the scene code. */
typedef struct rec_profile {
    const char *scene;
    const shr_context_desc *context;
    const shr_screen_desc *screen;
    uint64_t step_ns; /* the clock advance per loop frame */
    int32_t frames;   /* loop frames recorded */
    const rec_package *packages;
    uint32_t npackages;
} rec_profile;

typedef struct rec_buffer {
    const uint8_t *pixels;
    uint32_t stride, bytes, mem;
} rec_buffer;

/* Records while `on`; give `drv` to the context. Stops recording, keeping whole frames, when `buf` is full. With
 * `hash_only` (set before rec_begin) each frame is dropped once hashed, so `buf` needs room for the largest frame
 * (`peak`) only and there is no rec_end. */
typedef struct rec_writer {
    shr_framebuffer_driver inner, drv;
    shr_surface targets[REC_TARGETS];
    uint32_t ntargets;
    uint8_t *buf;
    size_t cap, len, frame_at, hashed, peak;
    rec_buffer *buffers; /* by id */
    uint32_t mems, max_batch, hash, done, limit;
    int32_t frame;
    uint32_t *sums, *hashes; /* per completed frame: screen checksum, hash of the recording so far */
    uint64_t cmds;
    bool on, full, hash_only;
    shr_status st;
} rec_writer;

/* Running FNV-1a over 32-bit little-endian words; `bytes` is a multiple of 4 or the tail counts zero-padded. */
uint32_t rec_hash(uint32_t h, const void *data, size_t bytes);
#define REC_HASH_INIT 2166136261u

/* Records into w->buf, w->cap bytes (set first). targets[0] is the output, the others bands (or a composition);
 * drawing anywhere else stops the recording. */
shr_status rec_begin(rec_writer *w, const rec_profile *p, const shr_framebuffer_driver *inner, const shr_surface *targets,
                     uint32_t ntargets);
/* Frame `index` starts at `clock_ns` (REC_OPEN first, then 0, 1, ...); recording stops at index p->frames. */
void rec_frame(rec_writer *w, int32_t index, uint64_t clock_ns);
/* The frame is on the screen, whose checksum is `sum`. */
void rec_frame_done(rec_writer *w, uint32_t sum);
/* Writes END; the recording is buf[0, len). */
shr_status rec_end(rec_writer *w);
void rec_free(rec_writer *w);

/* For the call recorder (rec_api.c): rec_start starts a recording of `kind` without wrapping a driver; rec_put makes
 * room for a record of `len` payload bytes, zeroed (NULL when not recording or full); rec_grow adds `extra` payload
 * bytes to the record at `at` while it is the last one and not hashed yet (NULL otherwise). */
shr_status rec_start(rec_writer *w, const rec_profile *p, const shr_driver_caps *caps, uint32_t kind,
                     const shr_surface *targets, uint32_t ntargets);
uint8_t *rec_put(rec_writer *w, unsigned type, size_t len);
uint8_t *rec_grow(rec_writer *w, size_t at, size_t extra);
/* The record at `at` of rec[0, len): its type and payload; false at the end or when malformed. */
bool rec_next(const uint8_t *rec, size_t len, size_t *at, unsigned *type, const uint8_t **p, size_t *n);
/* What a recording's header holds, back as descriptors (the host's pointers and callbacks unset); `names` keeps
 * the package names. */
typedef struct rec_header {
    char scene[32];
    uint32_t kind;
    int32_t frames;
    uint64_t step_ns;
    shr_context_desc context;
    shr_screen_desc screen;
    shr_driver_caps caps;
    rec_package packages[15];
    char names[15][32];
    uint32_t npackages;
} rec_header;
shr_status rec_header_read(const uint8_t *rec, size_t len, rec_header *out);

typedef struct rec_info {
    char scene[32];
    uint32_t kind;
    uint32_t frames; /* recorded, REC_OPEN included */
    uint32_t mems, max_batch, hash; /* calls: objects, styles */
    uint64_t mem_bytes, cmds;       /* calls: 0, calls */
    const uint8_t *sums; /* frames u32 words, the screen checksum after each frame, then as many recording hashes */
} rec_info;

shr_status rec_scan(const uint8_t *rec, size_t len, rec_info *out);
/* rec_sum: the screen checksum after recorded frame k (0: REC_OPEN, k: loop frame k - 1); rec_frame_hash: the
 * recording's hash up to there. Both 0 for k past the frames. */
uint32_t rec_sum(const rec_info *in, uint32_t k);
uint32_t rec_frame_hash(const rec_info *in, uint32_t k);

typedef struct rec_host {
    const shr_framebuffer_driver *drv;
    const shr_surface *targets; /* as recorded */
    uint32_t ntargets;
    shr_draw_cmd *cmds; /* at least max_batch */
    void *(*alloc)(void *user, size_t bytes); /* buffer memory, 128-byte aligned */
    void (*free)(void *user, void *p);
    uint64_t (*now_ns)(void *user);
    uint64_t (*waited_ns)(void *user); /* time blocked on the device so far; NULL: none */
    void (*finish)(void *user);        /* timed: the device finishes the frame */
    void (*settle)(void *user);        /* not timed: after finish (cache write-back) */
    uint32_t (*checksum)(void *user);  /* not timed */
    void *user;
} rec_host;

/* Per frame, the columns of a part in ms, the first the frame's (MAX is taken from it). A driver's: frame (execute()
 * and finish), busy (frame less the device waits), lead (execute() of the batches into the output: buffers, keeps
 * released, moves), raster (of the batches into the bands), rotate (of the ROTATE batches), wait (the device waits). */
typedef struct rec_times {
    float v[REC_COLS];
    uint32_t cmds, sum; /* sum: the screen checksum after the frame, when verifying */
} rec_times;
extern const char *const rec_driver_cols[REC_COLS];

/* Replays into h->drv: t[0] the REC_OPEN frame, t[1..] loop frames. `verify` compares the screen after every frame
 * (*bad: the first frame that differs, else -2); the last checksum goes to *sum. */
shr_status rec_play(const uint8_t *rec, size_t len, const rec_host *h, bool verify, rec_times *t, uint32_t *sum,
                    int32_t *bad);

typedef struct rec_stats {
    float p50, p95, max, busy_max;
} rec_stats;

/* One B line for a replay rep over loop frames t[1..n] (p50 and p95 from REC_WARM on, max over all of them) with
 * `ncols` columns named `cols` and the BF lines when `frames`; busy-max where the second column is "busy". */
rec_stats rec_print(int rep, const char *scene, const char *part, const char *const *cols, uint32_t ncols,
                    const rec_times *t, uint32_t n, uint32_t sum, bool ok, uint32_t hash, bool frames);
/* The RP line of a record pass whose screen checksum at its end was `pass`, and the RH line: the recording hash
 * after each frame, to find where two recordings part. */
void rec_print_pass(const rec_writer *w, const rec_info *in, uint32_t pass);
/* With a verifying replay's `t`: an RD line listing the frames whose screen differs from the recording (up to 8). */
void rec_print_diff(const rec_info *in, const char *part, const rec_times *t);
/* The BS line over `reps` reps. */
void rec_print_summary(const char *scene, const char *part, const rec_stats *s, int reps);

/* ===== Call recordings (rec_api.c) ===== */

typedef struct rec_style_slot rec_style_slot;

/* Records the calls rec_api.h redirects while rec_api_rec points at it and `w` is on, with the clock the context
 * reads (from shr_create's desc) whenever it changed. Frames as for commands: rec_frame, rec_frame_done on `w`. */
typedef struct rec_calls {
    rec_writer w; /* set w.buf and w.cap first */
    uint64_t (*clock)(void *user);
    void *clock_user;
    uint64_t now;
    const void **objs; /* layers, images and fonts by id - 1; NULL once destroyed */
    uint32_t nobjs, cap, last;
    rec_style_slot *styles;
    uint32_t nstyles, slots;
    size_t cells_at; /* the set_cell record the next one may join */
    uint32_t cells_layer;
} rec_calls;

extern rec_calls *rec_api_rec;

/* Starts recording into c (rec_api_rec = c) before the context is created; `caps` are the driver's. */
shr_status rec_calls_begin(rec_calls *c, const rec_profile *p, const shr_driver_caps *caps, const shr_surface *targets,
                           uint32_t ntargets);
/* Writes END and stops (rec_api_rec = NULL); the recording is w.buf[0, w.len). */
shr_status rec_calls_end(rec_calls *c);
void rec_calls_free(rec_calls *c);

typedef struct rec_calls_host {
    const shr_framebuffer_driver *drv; /* caps as recorded; its execute() is replaced by one drawing nothing */
    const shr_surface *targets;        /* the output and the bands, as recorded */
    uint32_t ntargets;
    const shr_allocator *allocator;    /* NULL: malloc */
    shr_status (*open)(void *user, const char *package, shr_asset_source *out);
    uint64_t (*now_ns)(void *user);
    void (*idle)(void *user); /* not timed, between frames; NULL: none */
    void *user;
    void *scratch; /* calls decoded ahead of their timed run, at least 2 KiB */
    size_t scratch_bytes;
} rec_calls_host;

/* Columns per frame: compositor: all calls (frame), the calls but submit, pump and polls (api), submit, pump and
 * polls (build); api: those calls by kind, cells = set_cell, set_text and clear; layer: layers and their commands. */
enum { REC_COMPOSITOR_COLS = 4, REC_API_COLS = 6 };
extern const char *const rec_compositor_cols[REC_COLS], *const rec_api_cols[REC_COLS];

/* Replays a call recording into new plugins and a new context, the driver drawing nothing: comp[0] and api[0] the
 * REC_OPEN frame, then the loop frames (comp[k].cmds: the commands the driver took, api[k].cmds: the calls). With
 * `verify` (w.buf, w.cap and hash_only set) the commands are recorded into it as the record pass did, for their hash
 * (rec_free it after). The context is destroyed at the end. */
shr_status rec_calls_play(const uint8_t *rec, size_t len, const rec_calls_host *h, rec_writer *verify, rec_times *comp,
                          rec_times *api);

#endif
