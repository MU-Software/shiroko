#include "compositor.h"

enum { IO_FREE = 0, IO_SUBMITTED, IO_DONE };

static uint64_t io_request(uint64_t state, uint32_t slot) { return (state >> 8) << 16 | slot; }

static shr_status final_status(shr_status st) {
    return (unsigned)st >= 64 || st == SHR_IN_PROGRESS || st == SHR_E_WOULD_BLOCK ? SHR_E_IO : st;
}

shr_status shr__ctx_read(shr_context *ctx, shr__res *res, const shr_asset_source *src, uint64_t offset,
                         uint32_t length, void *dst, uint64_t tag) {
    if (!ctx || !res || !src || !src->read || src->data || (!dst && length)) return SHR_E_INVALID_ARG;
    if (shr__ctx_refused(ctx)) return SHR_E_STATE;
    uint32_t i = 0;
    while (i < ctx->nio && (atomic_load(&ctx->io[i].state) & 3) != IO_FREE) i++;
    if (i == ctx->nio) return SHR_E_LIMIT;
    shr__io *io = &ctx->io[i];
    uint64_t gen = ++ctx->io_gen & 0xFFFFFFFFFFFull;
    io->res = res, io->key = src, io->src = *src, io->tag = tag, io->length = length;
    io->deadline = ctx->desc.io_timeout_ns && src->cancel ? shr__sat_add(shr__ctx_now(ctx), ctx->desc.io_timeout_ns) : 0;
    atomic_store(&io->state, gen << 8 | IO_SUBMITTED);
    uint64_t request = gen << 16 | i;
    shr__ctx_trace(ctx, SHR_TRACE_IO_BEGIN, request, length, 0);
    shr_status st;
    SHR_HOST(ctx, st = src->read(src->user, offset, length, dst, request));
    if (st == SHR_IN_PROGRESS) return SHR_OK;
    if (st == SHR_E_WOULD_BLOCK) {
        io->deadline = 0;
        atomic_store(&io->state, IO_FREE);
        return st;
    }
    atomic_store(&io->state, gen << 8 | (uint64_t)final_status(st) << 2 | IO_DONE);
    return SHR_OK;
}

/* Ends a submitted read as failed; the source's cancel() guarantees `dst` is no longer written. */
static void io_cancel(shr_context *ctx, uint32_t i, shr_status why) {
    shr__io *io = &ctx->io[i];
    if ((atomic_load(&io->state) & 3) == IO_FREE || !io->src.cancel) return;
    /* A completion and a cancellation race on setting IO_DONE; only the owner thread frees slots. */
    uint64_t s = atomic_fetch_or(&io->state, IO_DONE);
    if (s & IO_DONE) return;
    atomic_store(&io->state, (s >> 8) << 8 | (uint64_t)why << 2 | IO_DONE);
    SHR_HOST(ctx, io->src.cancel(io->src.user, io_request(s, i)));
}

shr_status shr_asset_complete(shr_context *ctx, uint64_t request, shr_status result) {
    if (!ctx) return SHR_E_INVALID_ARG;
    if ((request & 0xFFFF) >= ctx->nio) return SHR_E_NOT_FOUND;
    shr__io *io = &ctx->io[request & 0xFFFF];
    uint64_t expect = (request >> 16) << 8 | IO_SUBMITTED;
    uint64_t done = (request >> 16) << 8 | (uint64_t)final_status(result) << 2 | IO_DONE;
    return atomic_compare_exchange_strong(&io->state, &expect, done) ? SHR_OK : SHR_E_NOT_FOUND;
}

bool shr__ctx_read_cancel(shr_context *ctx, const shr_asset_source *src) {
    bool writing = false;
    for (uint32_t i = 0; i < ctx->nio; i++) {
        if (ctx->io[i].key != src || (atomic_load(&ctx->io[i].state) & 3) == IO_FREE) continue;
        io_cancel(ctx, i, SHR_E_IO);
        writing |= (atomic_load(&ctx->io[i].state) & 3) == IO_SUBMITTED;
    }
    return writing;
}

void shr__io_cancel_all(shr_context *ctx) {
    for (uint32_t i = 0; i < ctx->nio; i++) io_cancel(ctx, i, SHR_E_IO);
}

bool shr__io_pump(shr_context *ctx) {
    bool progress = false;
    uint64_t now = shr__ctx_now(ctx);
    for (uint32_t i = 0; i < ctx->nio; i++) {
        shr__io *io = &ctx->io[i];
        if (io->deadline && now >= io->deadline) io_cancel(ctx, i, SHR_E_TIMEOUT);
        uint64_t s = atomic_load(&io->state);
        if ((s & 3) != IO_DONE) continue;
        shr__res *res = io->res;
        shr_status result = (shr_status)(s >> 2 & 63);
        io->deadline = 0;
        atomic_store(&io->state, IO_FREE);
        shr__ctx_trace(ctx, SHR_TRACE_IO_END, io_request(s, i), io->length, (uint64_t)result);
        if (res->ops->io_done) res->ops->io_done(res, io->tag, result);
        progress = true;
    }
    return progress;
}

bool shr__io_busy(const shr_context *ctx, const shr__res *res) {
    for (uint32_t i = 0; i < ctx->nio; i++)
        if ((atomic_load(&ctx->io[i].state) & 3) != IO_FREE && (!res || ctx->io[i].res == res)) return true;
    return false;
}

bool shr__io_ready(const shr_context *ctx) {
    uint64_t now = shr__ctx_now(ctx);
    for (uint32_t i = 0; i < ctx->nio; i++) {
        uint64_t s = atomic_load(&ctx->io[i].state) & 3;
        if (s == IO_DONE || (s == IO_SUBMITTED && ctx->io[i].deadline && now >= ctx->io[i].deadline)) return true;
    }
    return false;
}

uint64_t shr__io_deadline(const shr_context *ctx) {
    uint64_t at = 0;
    for (uint32_t i = 0; i < ctx->nio; i++)
        if ((atomic_load(&ctx->io[i].state) & 3) == IO_SUBMITTED && ctx->io[i].deadline &&
            (!at || ctx->io[i].deadline < at))
            at = ctx->io[i].deadline;
    return at;
}
