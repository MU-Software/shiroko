#include "rec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CMD_BYTES = 64, HEADER_BYTES = 92 + 8 * 29 + 32 + 4 }; /* a header up to its package count */
#define REF_TARGET (1u << 31)
/* Fields a kind reads (shiroko_driver.h), as they are written. */
enum { F_BUF = 1, F_DST = 2, F_ORG = 4, F_COLOR = 8, F_RECT = 16, F_SRC = 32 };
static const uint8_t kind_fields[] = {
    [SHR_CMD_FILL] = F_DST | F_COLOR,
    [SHR_CMD_GLYPH] = F_BUF | F_DST | F_ORG | F_COLOR | F_RECT,
    [SHR_CMD_IMAGE] = F_BUF | F_DST | F_ORG | F_RECT,
    [SHR_CMD_COPY] = F_DST | F_ORG | F_SRC,
    [SHR_CMD_ROTATE] = F_DST | F_SRC,
    [SHR_CMD_KEEP_BEGIN] = F_BUF | F_DST,
    [SHR_CMD_KEEP_END] = 0,
    [SHR_CMD_KEEP_DRAW] = F_BUF | F_DST | F_ORG,
    [SHR_CMD_BUFFER_REGISTER] = F_BUF | F_SRC,
    [SHR_CMD_BUFFER_UPDATE] = F_BUF | F_RECT,
    [SHR_CMD_BUFFER_RELEASE] = F_BUF,
    [SHR_CMD_KEEP_RELEASE] = F_BUF,
    [SHR_CMD_LINE] = F_DST | F_ORG | F_COLOR | F_RECT,
};

static shr_rect get_rect(const uint8_t *p) { return (shr_rect){(int32_t)rec_get32(p), (int32_t)rec_get32(p + 4), (int32_t)rec_get32(p + 8), (int32_t)rec_get32(p + 12)}; }

uint32_t rec_hash(uint32_t h, const void *data, size_t bytes) {
    const uint8_t *p = data;
    for (size_t i = 0; i + 4 <= bytes; i += 4) h = (h ^ rec_get32(p + i)) * 16777619u;
    if (bytes % 4) {
        uint8_t tail[4] = {0};
        memcpy(tail, p + bytes / 4 * 4, bytes % 4);
        h = (h ^ rec_get32(tail)) * 16777619u;
    }
    return h;
}

/* ===== Writing ===== */

static void fail(rec_writer *w, shr_status st) {
    if (w->st == SHR_OK) w->st = st;
    w->on = false;
}

/* Zeroed payload; when full the frame in progress is dropped. */
uint8_t *rec_put(rec_writer *w, unsigned type, size_t len) {
    size_t n = 4 + ((len + 3) & ~(size_t)3);
    if (!w->on) return NULL;
    if (len >= 1u << 24 || n > w->cap - w->len) {
        w->full = true, w->on = false, w->len = w->frame_at;
        return NULL;
    }
    uint8_t *p = w->buf + w->len;
    memset(p, 0, n);
    rec_put32(p, type | (uint32_t)len << 8);
    w->len += n;
    return p + 4;
}

uint8_t *rec_grow(rec_writer *w, size_t at, size_t extra) {
    if (!w->on || at < w->hashed || at + 4 > w->len) return NULL;
    size_t len = rec_get32(w->buf + at) >> 8, n = 4 + ((len + extra + 3) & ~(size_t)3);
    if (at + 4 + ((len + 3) & ~(size_t)3) != w->len) return NULL;
    if (len + extra >= 1u << 24 || n > w->cap - at) {
        w->full = true, w->on = false, w->len = w->frame_at;
        return NULL;
    }
    memset(w->buf + w->len, 0, at + n - w->len);
    rec_put32(w->buf + at, (w->buf[at] & 255u) | (uint32_t)(len + extra) << 8);
    w->len = at + n;
    return w->buf + at + 4 + len;
}

static uint32_t target_of(const rec_writer *w, const void *p, uint32_t *off) {
    for (uint32_t t = 0; t < w->ntargets; t++) {
        const uint8_t *b = w->targets[t].pixels;
        if ((const uint8_t *)p >= b && (const uint8_t *)p < b + w->targets[t].byte_length)
            return *off = (uint32_t)((const uint8_t *)p - b), t;
    }
    return w->ntargets;
}

static bool enc(rec_writer *w, const shr_draw_cmd *c, uint32_t mem, uint8_t *o) {
    if (!c->kind || c->kind >= sizeof(kind_fields) || (c->kind != SHR_CMD_KEEP_END && !kind_fields[c->kind])) return false;
    unsigned f = kind_fields[c->kind];
    o[0] = c->kind, o[1] = c->kind == SHR_CMD_ROTATE ? c->rotation : 0, o[2] = (uint8_t)c->flags, o[3] = (uint8_t)(c->flags >> 8);
    if (f & F_BUF) rec_put32(o + 4, c->buffer);
    if (f & F_DST) rec_put_rect(o + 8, c->dst);
    if (f & F_ORG) rec_put32(o + 24, (uint32_t)c->src_origin.x), rec_put32(o + 28, (uint32_t)c->src_origin.y);
    if (f & F_COLOR) rec_put32(o + 32, c->color);
    if (c->kind == SHR_CMD_GLYPH && c->flags & SHR_GLYPH_ON_FILL) rec_put32(o + 36, c->bg);
    if (f & F_RECT) rec_put_rect(o + 40, c->src_rect);
    if (c->kind == SHR_CMD_GLYPH && c->flags & SHR_GLYPH_ITALIC) rec_put32(o + 56, (uint32_t)c->slant_axis);
    if (!(f & F_SRC)) return true;
    uint32_t ref = mem, off = 0;
    if (c->kind != SHR_CMD_BUFFER_REGISTER) {
        uint32_t t = target_of(w, c->src.pixels, &off);
        if (t == w->ntargets) return false;
        ref = REF_TARGET | t;
    }
    rec_put32(o + 32, ref), rec_put32(o + 36, off), rec_put32(o + 40, (uint32_t)c->src.width), rec_put32(o + 44, (uint32_t)c->src.height);
    rec_put32(o + 48, c->src.stride), o[52] = c->src.format, o[53] = c->src.domain;
    return true;
}

static void mem_record(rec_writer *w, const shr_draw_cmd *c) {
    shr_image im;
    if (!c->buffer || c->buffer > w->inner.caps.max_buffers || shr_image_ref_get(&c->src, &im) != SHR_OK) {
        fail(w, SHR_E_INVALID_ARG);
        return;
    }
    rec_buffer *b = &w->buffers[c->buffer];
    *b = (rec_buffer){im.pixels, (uint32_t)im.stride, (uint32_t)im.byte_length, w->mems++};
    uint8_t *p = rec_put(w, REC_MEM, 8 + b->bytes);
    if (p) rec_put32(p, b->mem), rec_put32(p + 4, b->bytes), memcpy(p + 8, b->pixels, b->bytes);
}

static void update_record(rec_writer *w, const shr_draw_cmd *c) {
    const rec_buffer *b = c->buffer && c->buffer <= w->inner.caps.max_buffers ? &w->buffers[c->buffer] : NULL;
    if (!b || !b->pixels || c->src_rect.y0 < 0 || c->src_rect.y1 < c->src_rect.y0) {
        fail(w, SHR_E_INVALID_ARG);
        return;
    }
    uint64_t at = (uint64_t)c->src_rect.y0 * b->stride, end = (uint64_t)c->src_rect.y1 * b->stride;
    end = end < b->bytes ? end : b->bytes, at = at < end ? at : end;
    uint8_t *p = rec_put(w, REC_MEMW, 12 + (size_t)(end - at));
    if (p) rec_put32(p, b->mem), rec_put32(p + 4, (uint32_t)at), rec_put32(p + 8, (uint32_t)(end - at)), memcpy(p + 12, b->pixels + at, (size_t)(end - at));
}

static void record(rec_writer *w, const shr_surface *dst, const shr_draw_cmd *c, size_t n) {
    uint32_t off, t = target_of(w, dst->pixels, &off), mem = w->mems;
    if (t == w->ntargets || off) {
        fail(w, SHR_E_UNSUPPORTED);
        return;
    }
    for (size_t i = 0; i < n && w->on; i++)
        if (c[i].kind == SHR_CMD_BUFFER_REGISTER)
            mem_record(w, &c[i]);
        else if (c[i].kind == SHR_CMD_BUFFER_UPDATE)
            update_record(w, &c[i]);
    uint8_t *p = rec_put(w, REC_BATCH, 8 + CMD_BYTES * n);
    if (!p) return;
    rec_put32(p, t), rec_put32(p + 4, (uint32_t)n);
    for (size_t i = 0; i < n; i++)
        if (!enc(w, &c[i], c[i].kind == SHR_CMD_BUFFER_REGISTER ? mem++ : 0, p + 8 + CMD_BYTES * i)) {
            fail(w, SHR_E_UNSUPPORTED);
            return;
        }
    w->cmds += n, w->max_batch = n > w->max_batch ? (uint32_t)n : w->max_batch;
}

static shr_status rec_execute(void *user, const shr_surface *dst, const shr_draw_cmd *c, size_t n, shr_fence f) {
    rec_writer *w = user;
    shr_status st = w->inner.execute(w->inner.user, dst, c, n, f);
    if (w->on && (st == SHR_OK || st == SHR_IN_PROGRESS)) record(w, dst, c, n);
    return st;
}

static void rec_cancel(void *user, shr_fence f) {
    rec_writer *w = user;
    w->inner.cancel(w->inner.user, f);
}

static shr_status rec_reset(void *user) {
    rec_writer *w = user;
    return w->inner.reset(w->inner.user);
}

static void rec_sync(void *user, const void *addr, size_t bytes) {
    rec_writer *w = user;
    w->inner.sync(w->inner.user, addr, bytes);
}

static size_t end_bytes(uint32_t frames) { return 4 + 32 + 8 * (size_t)frames; }

static size_t header(uint8_t *h, const rec_profile *p, const shr_driver_caps *k, uint32_t kind) {
    const shr_context_desc *c = p->context;
    const shr_screen_desc *s = p->screen;
    shr_text_profile_info tp = {0};
    shr_pl_lyr_tilemap_profile_get(&tp);
    memcpy(h, "SHRR", 4);
    rec_put32(h + 4, REC_VERSION | kind << 16), rec_put32(h + 8, SHR_CELL_WIDTH | SHR_CELL_HEIGHT << 16);
    rec_put32(h + 12, SHR_PIXEL_FORMAT);
    strncpy((char *)h + 16, p->scene, 31);
    rec_put32(h + 48, (uint32_t)p->frames), rec_put64(h + 52, p->step_ns);
    const uint32_t sv[] = {(uint32_t)s->width, (uint32_t)s->height, s->rotation, s->output_format, s->flags, s->clear,
                           s->band_count, s->band_align};
    size_t o = 60;
    for (size_t i = 0; i < sizeof(sv) / sizeof(sv[0]); i++, o += 4) rec_put32(h + o, sv[i]);
    const uint64_t cv[] = {c->blink.interval_ns, c->blink.epoch_ns, c->blink.start_visible, 0 /* was blink restart */,
                           c->event_capacity, c->max_unreleased_frames, c->max_commands, c->max_reads,
                           c->page_cache_bytes, c->image_bytes, c->io_retry_limit, c->io_retry_ns, c->io_timeout_ns,
                           c->min_frame_interval_ns, k->domains, k->address_align, k->stride_align,
                           (uint32_t)k->max_width, (uint32_t)k->max_height, k->timeout_ns, k->max_buffers,
                           (uint32_t)k->max_buffer_width, (uint32_t)k->max_buffer_height, k->buffer_bytes,
                           k->buffer_flags, k->max_keeps, k->keep_bytes, k->max_keep_bytes, k->flags};
    for (size_t i = 0; i < sizeof(cv) / sizeof(cv[0]); i++, o += 8) rec_put64(h + o, cv[i]);
    memcpy(h + o, tp.text_profile_id, 32), o += 32;
    rec_put32(h + o, p->npackages), o += 4;
    for (uint32_t i = 0; i < p->npackages; i++, o += 44) {
        strncpy((char *)h + o, p->packages[i].name, 31);
        rec_put64(h + o + 32, p->packages[i].size), rec_put32(h + o + 40, p->packages[i].hash);
    }
    return o;
}

shr_status rec_start(rec_writer *w, const rec_profile *p, const shr_driver_caps *caps, uint32_t kind,
                     const shr_surface *targets, uint32_t ntargets) {
    uint8_t *buf = w->buf;
    size_t cap = w->cap;
    bool hash_only = w->hash_only;
    if (!buf || !p || !caps || !targets || !ntargets || ntargets > REC_TARGETS || p->frames < 0 || p->npackages > 15)
        return SHR_E_INVALID_ARG;
    *w = (rec_writer){.ntargets = ntargets, .buf = buf, .limit = (uint32_t)p->frames, .hash = REC_HASH_INIT,
                      .frame = REC_OPEN - 1, .on = true, .hash_only = hash_only};
    memcpy(w->targets, targets, ntargets * sizeof(*targets));
    w->buffers = calloc(caps->max_buffers + 1u, sizeof(*w->buffers));
    w->sums = calloc(2 * (w->limit + 1), sizeof(uint32_t));
    size_t end = end_bytes(w->limit + 1);
    if (!w->buffers || !w->sums || cap < end) return rec_free(w), SHR_E_NO_MEMORY;
    w->hashes = w->sums + w->limit + 1, w->cap = cap - end;
    uint8_t h[1024] = {0};
    size_t n = header(h, p, caps, kind);
    uint8_t *r = rec_put(w, REC_HEADER, n);
    if (r) memcpy(r, h, n);
    for (uint32_t t = 0; t < ntargets && (r = rec_put(w, REC_TARGET, 24)); t++) {
        const shr_surface *s = &targets[t];
        rec_put32(r, t), rec_put32(r + 4, (uint32_t)s->width), rec_put32(r + 8, (uint32_t)s->height), rec_put32(r + 12, (uint32_t)s->stride);
        rec_put32(r + 16, (uint32_t)s->byte_length), rec_put32(r + 20, s->format);
    }
    w->frame_at = w->len;
    return w->on ? SHR_OK : SHR_E_NO_MEMORY;
}

shr_status rec_begin(rec_writer *w, const rec_profile *p, const shr_framebuffer_driver *inner, const shr_surface *targets,
                     uint32_t ntargets) {
    if (!inner) return SHR_E_INVALID_ARG;
    shr_status st = rec_start(w, p, &inner->caps, REC_KIND_CMDS, targets, ntargets);
    w->inner = *inner, w->drv = *inner, w->drv.user = w, w->drv.execute = rec_execute;
    w->drv.cancel = inner->cancel ? rec_cancel : NULL, w->drv.reset = inner->reset ? rec_reset : NULL;
    w->drv.sync = inner->sync ? rec_sync : NULL;
    return st;
}

void rec_frame(rec_writer *w, int32_t index, uint64_t clock_ns) {
    if (index >= (int32_t)w->limit) w->on = false;
    if (!w->on) return;
    w->frame_at = w->len, w->frame = index;
    uint8_t *p = rec_put(w, REC_FRAME, 12);
    if (p) rec_put32(p, (uint32_t)index), rec_put64(p + 4, clock_ns);
}

void rec_frame_done(rec_writer *w, uint32_t sum) {
    if (!w->on) return;
    w->hash = rec_hash(w->hash, w->buf + w->hashed, w->len - w->hashed);
    w->hashed = w->frame_at = w->len;
    w->sums[w->done] = sum, w->hashes[w->done++] = w->hash;
    w->peak = w->len > w->peak ? w->len : w->peak;
    if (w->hash_only) w->len = w->hashed = w->frame_at = 0;
}

shr_status rec_end(rec_writer *w) {
    if (w->st != SHR_OK || w->hash_only) return w->st != SHR_OK ? w->st : SHR_E_STATE;
    w->len = w->frame_at, w->cap += end_bytes(w->limit + 1), w->on = true;
    uint64_t bytes = 0;
    for (size_t at = 0; at < w->len; at += 4 + ((rec_get32(w->buf + at) >> 8) + 3) / 4 * 4)
        bytes += (w->buf[at] == REC_MEM) * (uint64_t)rec_get32(w->buf + at + 8);
    uint8_t *p = rec_put(w, REC_END, 32 + 8 * (size_t)w->done);
    w->on = false;
    if (!p) return SHR_E_NO_MEMORY;
    rec_put32(p, w->done), rec_put32(p + 4, w->hash), rec_put32(p + 8, w->max_batch), rec_put32(p + 12, w->mems);
    rec_put64(p + 16, bytes), rec_put64(p + 24, w->cmds);
    for (uint32_t i = 0; i < w->done; i++) rec_put32(p + 32 + 4 * i, w->sums[i]), rec_put32(p + 32 + 4 * (w->done + i), w->hashes[i]);
    return SHR_OK;
}

void rec_free(rec_writer *w) {
    free(w->buffers), free(w->sums);
    w->buffers = NULL, w->sums = w->hashes = NULL;
}

/* ===== Reading ===== */

bool rec_next(const uint8_t *rec, size_t len, size_t *at, unsigned *type, const uint8_t **p, size_t *n) {
    if (len - *at < 4) return false;
    uint32_t h = rec_get32(rec + *at);
    size_t step = 4 + ((h >> 8) + 3) / 4 * 4;
    if (step > len - *at) return false;
    *type = h & 255, *p = rec + *at + 4, *n = h >> 8, *at += step;
    return true;
}

shr_status rec_scan(const uint8_t *rec, size_t len, rec_info *out) {
    size_t at = 0, n;
    unsigned type;
    const uint8_t *p;
    *out = (rec_info){0};
    if (!rec_next(rec, len, &at, &type, &p, &n) || type != REC_HEADER || n < 60 || memcmp(p, "SHRR", 4)) return SHR_E_FORMAT;
    out->kind = rec_get32(p + 4) >> 16;
    if ((rec_get32(p + 4) & 0xFFFF) != REC_VERSION || (out->kind != REC_KIND_CMDS && out->kind != REC_KIND_CALLS) ||
        rec_get32(p + 8) != (SHR_CELL_WIDTH | SHR_CELL_HEIGHT << 16) || rec_get32(p + 12) != SHR_PIXEL_FORMAT)
        return SHR_E_PROFILE_MISMATCH;
    memcpy(out->scene, p + 16, 31);
    for (size_t end = at; rec_next(rec, len, &at, &type, &p, &n); end = at) {
        if (type != REC_END) continue;
        if (n < 32 || rec_get32(p) > (n - 32) / 8) return SHR_E_FORMAT;
        out->frames = rec_get32(p), out->hash = rec_get32(p + 4), out->max_batch = rec_get32(p + 8), out->mems = rec_get32(p + 12);
        out->mem_bytes = rec_get64(p + 16), out->cmds = rec_get64(p + 24), out->sums = p + 32;
        return rec_hash(REC_HASH_INIT, rec, end) == out->hash ? SHR_OK : SHR_E_CHECKSUM;
    }
    return SHR_E_FORMAT;
}

uint32_t rec_sum(const rec_info *in, uint32_t k) { return k < in->frames ? rec_get32(in->sums + 4 * (size_t)k) : 0; }

uint32_t rec_frame_hash(const rec_info *in, uint32_t k) {
    return k < in->frames ? rec_get32(in->sums + 4 * ((size_t)in->frames + k)) : 0;
}

shr_status rec_header_read(const uint8_t *rec, size_t len, rec_header *out) {
    size_t at = 0, n;
    unsigned type;
    const uint8_t *p;
    rec_info in;
    shr_status st = rec_scan(rec, len, &in);
    if (st != SHR_OK) return st;
    rec_next(rec, len, &at, &type, &p, &n);
    if (n < HEADER_BYTES) return SHR_E_FORMAT;
    memset(out, 0, sizeof(*out));
    memcpy(out->scene, in.scene, sizeof(out->scene));
    out->kind = in.kind, out->frames = (int32_t)rec_get32(p + 48), out->step_ns = rec_get64(p + 52);
    shr_screen_desc *s = &out->screen;
    shr_screen_desc_init(s);
    s->width = (int32_t)rec_get32(p + 60), s->height = (int32_t)rec_get32(p + 64), s->rotation = (shr_rotation)rec_get32(p + 68);
    s->output_format = (shr_pixel_format)rec_get32(p + 72), s->flags = rec_get32(p + 76), s->clear = rec_get32(p + 80);
    s->band_count = rec_get32(p + 84), s->band_align = rec_get32(p + 88);
    uint64_t v[29];
    for (size_t i = 0; i < 29; i++) v[i] = rec_get64(p + 92 + 8 * i);
    shr_context_desc *c = &out->context;
    shr_driver_caps *k = &out->caps;
    shr_context_desc_init(c);
    c->blink = (shr_blink_profile){v[0], v[1], v[2] != 0}; /* v[3] (a removed blink restart) is ignored */
    c->event_capacity = (uint32_t)v[4], c->max_unreleased_frames = (uint32_t)v[5], c->max_commands = (uint32_t)v[6];
    c->max_reads = (uint32_t)v[7], c->page_cache_bytes = v[8], c->image_bytes = v[9], c->io_retry_limit = (uint32_t)v[10];
    c->io_retry_ns = v[11], c->io_timeout_ns = v[12], c->min_frame_interval_ns = v[13];
    *k = (shr_driver_caps){0};
    k->domains = (uint32_t)v[14], k->address_align = (uint32_t)v[15], k->stride_align = (uint32_t)v[16];
    k->max_width = (int32_t)v[17], k->max_height = (int32_t)v[18], k->timeout_ns = v[19], k->max_buffers = (uint32_t)v[20];
    k->max_buffer_width = (int32_t)v[21], k->max_buffer_height = (int32_t)v[22], k->buffer_bytes = v[23];
    k->buffer_flags = (uint32_t)v[24], k->max_keeps = (uint32_t)v[25], k->keep_bytes = v[26], k->max_keep_bytes = v[27];
    k->flags = (uint32_t)v[28];
    size_t o = HEADER_BYTES - 4;
    out->npackages = rec_get32(p + o), o += 4;
    if (out->npackages > 15 || n < o + 44 * (size_t)out->npackages) return SHR_E_FORMAT;
    for (uint32_t i = 0; i < out->npackages; i++, o += 44) {
        memcpy(out->names[i], p + o, 31);
        out->packages[i] = (rec_package){out->names[i], rec_get64(p + o + 32), rec_get32(p + o + 40)};
    }
    return SHR_OK;
}

typedef struct rec_mem {
    uint8_t *p;
    uint32_t n;
} rec_mem;

/* False when the command's source is not inside a target or a recorded memory. */
static bool dec(const uint8_t *o, shr_draw_cmd *c, const rec_host *h, const rec_mem *mem, uint32_t mems) {
    memset(c, 0, sizeof(*c));
    c->kind = o[0], c->rotation = o[1], c->flags = (uint16_t)(o[2] | o[3] << 8), c->buffer = rec_get32(o + 4);
    c->dst = get_rect(o + 8), c->src_origin = (shr_point){(int32_t)rec_get32(o + 24), (int32_t)rec_get32(o + 28)};
    c->slant_axis = (int32_t)rec_get32(o + 56);
    if (!c->kind || c->kind >= sizeof(kind_fields) || !(kind_fields[c->kind] & F_SRC)) {
        c->color = rec_get32(o + 32), c->bg = rec_get32(o + 36), c->src_rect = get_rect(o + 40);
        return true;
    }
    uint32_t ref = rec_get32(o + 32), off = rec_get32(o + 36), t = ref & ~REF_TARGET;
    bool target = ref & REF_TARGET;
    const uint8_t *base = target ? (t < h->ntargets ? h->targets[t].pixels : NULL) : (ref < mems ? mem[ref].p : NULL);
    size_t size = !base ? 0 : target ? h->targets[t].byte_length : mem[ref].n;
    if (!base || off > size) return false;
    c->src = (shr_image_ref){base + off, (int32_t)rec_get32(o + 40), (int32_t)rec_get32(o + 44), rec_get32(o + 48), o[52], o[53], 0};
    shr_image m;
    return shr_image_ref_get(&c->src, &m) == SHR_OK && m.byte_length <= size - off;
}

/* A REC_MEMW writes inside a recorded memory. */
static bool memw_fits(const uint8_t *p, size_t n, const rec_mem *mem, uint32_t mems) {
    if (n < 12 || rec_get32(p) >= mems || !mem[rec_get32(p)].p) return false;
    uint32_t size = mem[rec_get32(p)].n, off = rec_get32(p + 4), len = rec_get32(p + 8);
    return len <= n - 12 && off <= size && len <= size - off;
}

static uint64_t waited(const rec_host *h) { return h->waited_ns ? h->waited_ns(h->user) : 0; }

static void frame_end(const rec_host *h, rec_times *t, uint64_t ns[5], bool verify, const uint8_t *sums, int32_t k, int32_t *bad) {
    uint64_t t0 = h->now_ns(h->user), w0 = waited(h);
    if (h->finish) h->finish(h->user);
    ns[0] += h->now_ns(h->user) - t0, ns[4] += waited(h) - w0;
    if (h->settle) h->settle(h->user);
    const float v[REC_COLS] = {rec_ms(ns[0]), rec_ms(ns[0] - ns[4]), rec_ms(ns[1]),
                               rec_ms(ns[2]), rec_ms(ns[3]),         rec_ms(ns[4])};
    memcpy(t->v, v, sizeof(v));
    if (verify && (t->sum = h->checksum(h->user)) != rec_get32(sums + 4 * (size_t)k) && *bad == -2) *bad = k - 1;
}

shr_status rec_play(const uint8_t *rec, size_t len, const rec_host *h, bool verify, rec_times *t, uint32_t *sum,
                    int32_t *bad) {
    rec_info in;
    shr_status st = rec_scan(rec, len, &in);
    if (st != SHR_OK || in.kind != REC_KIND_CMDS) return st != SHR_OK ? st : SHR_E_PROFILE_MISMATCH;
    rec_mem *mem = calloc(in.mems + 1u, sizeof(*mem));
    if (!mem) return SHR_E_NO_MEMORY;
    size_t at = 0, n;
    unsigned type;
    const uint8_t *p;
    while (st == SHR_OK && rec_next(rec, len, &at, &type, &p, &n)) {
        if ((type == REC_TARGET && n < 24) || (type == REC_MEM && (n < 8 || rec_get32(p) >= in.mems ||
                                                                   mem[rec_get32(p)].p || rec_get32(p + 4) > n - 8))) {
            st = SHR_E_FORMAT;
        } else if (type == REC_TARGET) {
            const shr_surface *g = rec_get32(p) < h->ntargets ? &h->targets[rec_get32(p)] : NULL;
            if (!g || g->width != (int32_t)rec_get32(p + 4) || g->height != (int32_t)rec_get32(p + 8) ||
                g->stride != rec_get32(p + 12) || g->format != rec_get32(p + 20))
                st = SHR_E_PROFILE_MISMATCH;
        } else if (type == REC_MEM) {
            rec_mem *m = &mem[rec_get32(p)];
            if (!(m->p = h->alloc(h->user, m->n = rec_get32(p + 4)))) st = SHR_E_NO_MEMORY;
        }
    }
    *bad = -2;
    uint64_t ns[5] = {0}, fence = 0;
    int32_t k = -1;
    for (at = 0; st == SHR_OK && rec_next(rec, len, &at, &type, &p, &n);) {
        if ((type == REC_FRAME || type == REC_END) && k >= 0) frame_end(h, &t[k], ns, verify, in.sums, k, bad);
        if (type == REC_END) break;
        if ((type == REC_FRAME && (uint32_t)(k + 1) >= in.frames) || (type == REC_MEMW && !memw_fits(p, n, mem, in.mems))) {
            st = SHR_E_FORMAT;
            break;
        }
        if (type == REC_FRAME) k++, memset(ns, 0, sizeof(ns)), t[k] = (rec_times){0};
        if (type == REC_MEM) memcpy(mem[rec_get32(p)].p, p + 8, rec_get32(p + 4));
        if (type == REC_MEMW) memcpy(mem[rec_get32(p)].p + rec_get32(p + 4), p + 12, rec_get32(p + 8));
        if (type != REC_BATCH) continue;
        uint32_t tg = n < 8 ? h->ntargets : rec_get32(p), cnt = n < 8 ? 0 : rec_get32(p + 4);
        if (k < 0 || tg >= h->ntargets || cnt > in.max_batch || cnt > (n - 8) / CMD_BYTES) {
            st = SHR_E_FORMAT;
            break;
        }
        for (uint32_t i = 0; i < cnt; i++)
            if (!dec(p + 8 + CMD_BYTES * i, &h->cmds[i], h, mem, in.mems)) st = SHR_E_FORMAT;
        if (st != SHR_OK) break;
        uint64_t t0 = h->now_ns(h->user), w0 = waited(h);
        st = h->drv->execute(h->drv->user, &h->targets[tg], h->cmds, cnt, ++fence);
        uint64_t dt = h->now_ns(h->user) - t0;
        ns[0] += dt, ns[4] += waited(h) - w0;
        ns[h->cmds[0].kind == SHR_CMD_ROTATE ? 3 : tg ? 2 : 1] += dt;
        t[k].cmds += cnt;
    }
    for (uint32_t i = 0; i < in.mems; i++)
        if (mem[i].p) h->free(h->user, mem[i].p);
    free(mem);
    if (st == SHR_OK) *sum = h->checksum(h->user);
    return st;
}

/* ===== Lines ===== */

const char *const rec_driver_cols[REC_COLS] = {"frame", "busy", "lead", "raster", "rotate", "wait"};

rec_stats rec_print(int rep, const char *scene, const char *part, const char *const *cols, uint32_t ncols,
                    const rec_times *t, uint32_t n, uint32_t sum, bool ok, uint32_t hash, bool frames) {
    uint32_t w = n > REC_WARM ? REC_WARM : 0, m = n - w, max_f = 0, busy_f = 0, b = ncols > 1 && !strcmp(cols[1], "busy");
    float *v = malloc((m ? m : 1) * sizeof(float));
    uint64_t cmds = 0;
    for (uint32_t f = 0; f < n; f++) {
        max_f = t[1 + f].v[0] > t[1 + max_f].v[0] ? f : max_f;
        busy_f = t[1 + f].v[b] > t[1 + busy_f].v[b] ? f : busy_f;
        cmds += t[1 + f].cmds;
    }
    rec_stats s = {0};
    printf("  B rep=%d rec=%s part=%s frames=%u ms p50/p95/max:", rep, scene, part, (unsigned)n);
    for (uint32_t k = 0; k < ncols && v && m; k++) {
        for (uint32_t f = 0; f < m; f++) v[f] = t[1 + w + f].v[k];
        qsort(v, m, sizeof(float), rec_cmp_float);
        float p50 = v[m / 2], p95 = v[(m * 95 + 99) / 100 - 1];
        printf(" %s %.3f/%.3f/%.3f", cols[k], (double)p50, (double)p95, (double)v[m - 1]);
        if (!k) s.p50 = p50, s.p95 = p95;
    }
    free(v);
    s.max = n ? t[1 + max_f].v[0] : 0, s.busy_max = n ? t[1 + busy_f].v[b] : 0;
    printf(" first %.3f max %.3f max-frame %u", (double)t[0].v[0], (double)s.max, (unsigned)max_f);
    if (b) printf(" busy-max %.3f busy-max-frame %u", (double)s.busy_max, (unsigned)busy_f);
    printf(" cmds %llu fb %08lx %s rec %08lx\n", (unsigned long long)(n ? cmds / n : 0), (unsigned long)sum,
           ok ? "ok" : "DIFFER", (unsigned long)hash);
    for (uint32_t f = 0; frames && f < n; f++) {
        printf("  BF rec=%s part=%s f=%u ms", scene, part, (unsigned)f);
        for (uint32_t k = 0; k < ncols; k++) printf(" %s %.3f", cols[k], (double)t[1 + f].v[k]);
        printf(" cmds %u\n", (unsigned)t[1 + f].cmds);
    }
    return s;
}

void rec_print_pass(const rec_writer *w, const rec_info *in, uint32_t pass) {
    printf("  RP rec=%s frames=%u/%u pass %08lx bytes %.1f MiB mems %u (%.0f KiB) max-batch %u cmds %llu%s\n", in->scene,
           (unsigned)in->frames - 1, (unsigned)w->limit, (unsigned long)pass, (double)w->len / 1048576, (unsigned)in->mems,
           (double)in->mem_bytes / 1024, (unsigned)in->max_batch, (unsigned long long)in->cmds, w->full ? " FULL" : "");
    printf("  RH rec=%s", in->scene);
    for (uint32_t i = 0; i < w->done; i++) printf(" %08lx", (unsigned long)w->hashes[i]);
    printf("\n");
}

void rec_print_diff(const rec_info *in, const char *part, const rec_times *t) {
    int shown = 0;
    for (uint32_t k = 0; k < in->frames; k++) {
        if (t[k].sum == rec_sum(in, k)) continue;
        if (!shown++) printf("  RD rec=%s part=%s frame record/replay:", in->scene, part);
        if (shown <= 8) printf(" %d %08lx/%08lx", (int)k - 1, (unsigned long)rec_sum(in, k), (unsigned long)t[k].sum);
    }
    if (shown) printf(" (%d frames)\n", shown);
}

void rec_print_summary(const char *scene, const char *part, const rec_stats *s, int reps) {
    static const char *const names[4] = {"p50", "p95", "max", "busy-max"};
    float v[4][64], spread[4];
    reps = reps > 64 ? 64 : reps;
    if (reps < 1) return;
    for (int r = 0; r < reps; r++) v[0][r] = s[r].p50, v[1][r] = s[r].p95, v[2][r] = s[r].max, v[3][r] = s[r].busy_max;
    printf("  BS rec=%s part=%s reps=%d ms median/largest:", scene, part, reps);
    for (int k = 0; k < 4; k++) {
        qsort(v[k], (size_t)reps, sizeof(float), rec_cmp_float);
        spread[k] = v[k][reps - 1] - v[k][0];
        printf(" %s %.3f/%.3f", names[k], (double)v[k][reps / 2], (double)v[k][reps - 1]);
    }
    printf(" spread p50 %.3f max %.3f busy-max %.3f\n", (double)spread[0], (double)spread[2], (double)spread[3]);
}
