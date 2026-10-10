/* Software raster with arbitrary command batches, including buffer commands, keep groups, KEEP_DRAW and KEEP_RELEASE,
 * BOLD/ITALIC glyphs drawn from buffer regions, ON_FILL ones also where no FILL lies under them, and LINE patterns.
 * Invariants:
 *   - the driver accepts a batch exactly when the rules predict it (a ROTATE's own validity comes from the stateless
 *     path); a rejected batch writes nothing; a COPY of the destination onto itself moves its pixels
 *   - an accepted batch writes only inside the `dst` of its draws outside groups
 *   - the driver, with buffer registrations and keeps lasting across batches, draws exactly what the stateless path
 *     draws from the registrations the harness keeps, each KEEP_DRAW copying what the harness drew for its group,
 *     also through a copier whose copies run only when waited for, the latest first, some refused, and which fills the
 *     columns of a keep's edge pixels around them;
 *     an accepted UPDATE changes the pixels it names (where no other registration reaches), so what the driver keeps
 *     from a buffer must follow its UPDATEs */
#include "fuzz_common.h"

#define SW 24
#define SH 20
#define SRC_BYTES 2048
#define MAX_CMDS 32
#define MAX_BUFFERS 6
#define MAX_KEEPS 4

static uint8_t init_buf[SW * SH * 4], plain[SW * SH * 4], driven[SW * SH * 4];
static uint8_t src_buf[SRC_BYTES];
static uint8_t rot_buf[SW * SH * 4];
/* The registrations as the driver should hold them: the stateless path's table. */
static shr_image model[MAX_BUFFERS];

static fuzz_keep keeps[MAX_KEEPS];

typedef struct late_op {
    void *dst;
    const void *src;
    size_t dst_stride, src_stride, bytes;
    int32_t rows;
    shr_software_trim trim;
} late_op;

static struct {
    late_op q[64];
    uint64_t started, done;
    uint32_t calls;
    bool refuse;
} late;

static uint64_t late_trim(void *user, void *dst, size_t dst_stride, const void *src, size_t src_stride, size_t bytes,
                          int32_t rows, const shr_software_trim *trim) {
    (void)user;
    if (late.refuse && ++late.calls % 3 == 0) return 0;
    FUZZ_CHECK(late.started - late.done < 64 && rows > 0 && bytes > 0);
    late.q[late.started % 64] =
        (late_op){dst, src, dst_stride, src_stride, bytes, rows, trim ? *trim : (shr_software_trim){0}};
    return ++late.started;
}

static uint64_t late_copy(void *user, void *dst, size_t dst_stride, const void *src, size_t src_stride, size_t bytes,
                          int32_t rows) {
    return late_trim(user, dst, dst_stride, src, src_stride, bytes, rows, NULL);
}

static void late_wait(void *user, uint64_t ticket) {
    (void)user;
    FUZZ_CHECK(ticket <= late.started);
    for (uint64_t t = ticket; t > late.done; t--) {
        const late_op *c = &late.q[(t - 1) % 64];
        shr_software_trim_fill(c->dst, c->dst_stride, c->bytes, c->rows, &c->trim);
        for (int32_t y = 0; y < c->rows; y++)
            memcpy((uint8_t *)c->dst + (size_t)y * c->dst_stride, (const uint8_t *)c->src + (size_t)y * c->src_stride,
                   c->bytes);
    }
    if (ticket > late.done) late.done = ticket;
}

static bool inside(shr_rect r, int32_t w, int32_t h) {
    return r.x0 >= 0 && r.y0 >= 0 && r.x0 <= r.x1 && r.y0 <= r.y1 && r.x1 <= w && r.y1 <= h;
}

static bool registered(uint32_t id) { return id >= 1 && id <= MAX_BUFFERS && model[id - 1].format; }

/* Whole pixels of the italic shift of rect row y, rounded down, and whether a fraction remains (shiroko_driver.h). */
static int32_t slant(int32_t axis, int32_t y, bool *frac) {
    int32_t t = SHR_GLYPH_SLANT / 2 * (axis - 2 * y - 1), k = t >= 0 ? t / 256 : -((255 - t) / 256);
    *frac = t != 256 * k;
    return k;
}

/* `m` limited to the `avail` bytes it may read: empty where it would reach past them. */
static shr_image_ref bounded(shr_image_ref m, size_t avail) {
    if (fuzz_span(&m) > avail) m.height = 0;
    return m;
}

static bool in_span(const shr_image *m, const uint8_t *p) {
    return m->format && p >= (const uint8_t *)m->pixels && p < (const uint8_t *)m->pixels + m->byte_length;
}

/* Changes the bytes holding only pixels of `rect` of buffer `id` that no other registration, at the batch's start
 * (`start`) or now, reaches. */
static void touch(uint32_t id, shr_rect rect, const shr_image *start) {
    const shr_image *m = &model[id - 1];
    bool a4 = m->format == SHR_FORMAT_A4;
    int32_t px = m->format == SHR_FORMAT_RGBA8888 ? 4 : 1, b0 = a4 ? (rect.x0 + 1) / 2 : rect.x0 * px,
            b1 = a4 ? rect.x1 / 2 : rect.x1 * px;
    for (int32_t y = rect.y0; y < rect.y1; y++)
        for (int32_t bx = b0; bx < b1; bx++) {
            const uint8_t *p = (const uint8_t *)m->pixels + (size_t)y * m->stride + (size_t)bx;
            bool shared = false;
            for (uint32_t i = 0; i < MAX_BUFFERS; i++) shared |= i + 1 != id && (in_span(&start[i], p) || in_span(&model[i], p));
            if (!shared) src_buf[p - src_buf] ^= (uint8_t)(y * 31 + bx) | 1;
        }
}

/* Brings the driver's registrations to the model's and makes it hold no keep, as the compositor does after a failed
 * batch. */
static void resync(shr_framebuffer_driver *drv, const shr_surface *dst, bool forget) {
    shr_draw_cmd c[MAX_BUFFERS + MAX_KEEPS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_BUFFERS; i++)
        c[n++] = (shr_draw_cmd){.kind = model[i].format ? SHR_CMD_BUFFER_REGISTER : SHR_CMD_BUFFER_RELEASE,
                                .buffer = i + 1,
                                .src = {model[i].pixels, model[i].width, model[i].height, (uint32_t)model[i].stride,
                                        (uint8_t)model[i].format, (uint8_t)model[i].domain, 0}};
    for (uint32_t i = 0; forget && i < MAX_KEEPS; i++) {
        c[n++] = (shr_draw_cmd){.kind = SHR_CMD_KEEP_RELEASE, .buffer = i + 1};
        keeps[i].held = false;
    }
    FUZZ_CHECK(drv->execute(drv->user, dst, c, n, 1) == SHR_OK);
    late_wait(NULL, late.started);
}

/* Validates a leading buffer command the way the driver does and applies it to the model. */
static bool apply(const shr_draw_cmd *c) {
    uint32_t id = c->buffer;
    bool in_range = id >= 1 && id <= MAX_BUFFERS;
    if (c->kind == SHR_CMD_KEEP_RELEASE) {
        if (id >= 1 && id <= MAX_KEEPS) keeps[id - 1].held = false;
        return true;
    }
    if (c->kind == SHR_CMD_BUFFER_RELEASE) {
        if (in_range) model[id - 1] = (shr_image){0};
        return true;
    }
    if (c->kind == SHR_CMD_BUFFER_UPDATE)
        return registered(id) && inside(c->src_rect, model[id - 1].width, model[id - 1].height);
    shr_image m;
    bool ok = in_range && shr_image_ref_get(&c->src, &m) == SHR_OK && m.domain != SHR_MEMORY_DEVICE &&
              (m.format == SHR_FORMAT_A4 || m.format == SHR_FORMAT_A8 || m.format == SHR_FORMAT_RGBA8888);
    if (ok) model[id - 1] = m;
    return ok;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_reader r = {data, size};
    uint8_t cfg = fr_u8(&r);
    shr_pixel_format dfmt = (cfg & 1) ? SHR_FORMAT_RGB565 : SHR_FORMAT_RGBX8888;
    size_t bpp = dfmt == SHR_FORMAT_RGB565 ? 2 : 4;
    int32_t w = 1 + fr_u8(&r) % SW, h = 1 + fr_u8(&r) % SH;
    size_t stride = (size_t)w * bpp + (fr_u8(&r) % 3) * bpp;
    if (stride * (size_t)h > sizeof(init_buf)) stride = (size_t)w * bpp;
    /* Keeps of their own size, or in slots of 128, 512 and 256 Ki bytes. */
    static const uint64_t budgets[4] = {0, 4 * 128 + 100, 4 * 512, 1u << 20}, slots[4] = {0, 128, 512, 1u << 18};
    uint64_t limit = budgets[(cfg >> 1) & 3], slot = slots[(cfg >> 1) & 3];
    shr_framebuffer_driver drv;
    FUZZ_CHECK(shr_software_driver_create(NULL, limit, MAX_KEEPS, MAX_BUFFERS, &drv) == SHR_OK);
    FUZZ_CHECK(drv.caps.max_buffers == MAX_BUFFERS && drv.caps.max_keeps == MAX_KEEPS && drv.caps.keep_bytes == limit &&
               drv.caps.max_keep_bytes == slot);
    late.started = late.done = late.calls = 0, late.refuse = cfg & 16;
    shr_software_copier lc = {.copy = late_copy, .wait = late_wait, .copy_trim = cfg & 64 ? late_trim : NULL,
                              .align = cfg & 128 ? 8 : 0};
    if (cfg & 8) FUZZ_CHECK(shr_software_driver_set_copier(&drv, &lc) == SHR_OK);
    memset(model, 0, sizeof(model));
    for (int i = 0; i < MAX_KEEPS; i++) keeps[i].held = false;
    for (size_t i = 0; i < sizeof(src_buf); i++) src_buf[i] = (uint8_t)(i * 13u + 1u);
    for (size_t i = 0; i < sizeof(rot_buf); i++) rot_buf[i] = (uint8_t)(i * 5u + 3u);
    uint8_t seed = fr_u8(&r);
    for (size_t i = 0; i < sizeof(init_buf); i++) init_buf[i] = (uint8_t)(i * 7u + seed);
    shr_surface dst = {plain, w, h, stride, stride * (size_t)h, dfmt, 0, SHR_MEMORY_CPU, 0};

    while (r.n > 0) {
        shr_draw_cmd cmds[MAX_CMDS];
        bool self[MAX_CMDS] = {0}; /* COPYs reading the destination */
        size_t n = 0, group = SIZE_MAX;
        bool expect_ok = true, prologue_ok = true, drawn = false;
        uint8_t state[MAX_KEEPS] = {0}; /* in this batch: 1 stored, 2 drawn */
        shr_image start[MAX_BUFFERS];
        memcpy(start, model, sizeof(start));
        int32_t sw[MAX_KEEPS], sh[MAX_KEEPS];
        for (uint8_t count = 1 + fr_u8(&r) % MAX_CMDS; n < count && r.n > 0;) {
            shr_draw_cmd *c = &cmds[n];
            fuzz_blank(c, seed);
            uint8_t kind = fr_u8(&r) % 14;
            if (kind == 13) { /* LINE: shapes 0..7, cells up to 40 x 10 and origins across their bounds */
                drawn = true;
                c->kind = SHR_CMD_LINE;
                uint8_t f = fr_u8(&r);
                c->flags = (uint16_t)((f & 1) | (f >> 1 & 7) << SHR_LINE_SHAPE_SHIFT | (f == 0xFF ? 2 : 0));
                c->dst = fr_rect(&r);
                c->color = SHR_RGB(fr_u8(&r), fr_u8(&r), fr_u8(&r));
                int32_t lw = fr_u8(&r) % 41, lh = fr_u8(&r) % 11;
                c->src_rect = (shr_rect){fr_u8(&r) == 0xFF, 0, lw, lh};
                c->src_origin = (shr_point){fr_i8(&r) % 48, fr_i8(&r) % 12};
                shr_rect lim = group != SIZE_MAX ? cmds[group].dst : (shr_rect){0, 0, w, h};
                bool in = (group != SIZE_MAX || inside(c->dst, w, h)) && c->dst.x0 <= c->dst.x1 && c->dst.y0 <= c->dst.y1 &&
                          c->dst.x0 >= lim.x0 && c->dst.y0 >= lim.y0 && c->dst.x1 <= lim.x1 && c->dst.y1 <= lim.y1;
                expect_ok &= in && f != 0xFF && (f >> 1 & 7) <= SHR_LINE_DASHED && !c->src_rect.x0 && lw >= 1 &&
                             lw <= SHR_LINE_MAX_PERIOD && lh >= 1 && lh <= SHR_LINE_MAX_BAND && c->src_origin.x >= 0 &&
                             c->src_origin.x < lw && c->src_origin.y >= 0 && c->src_origin.y + (c->dst.y1 - c->dst.y0) <= lh;
                n++;
                continue;
            }
            if (kind >= 9) { /* REGISTER, UPDATE, RELEASE of ids 0..MAX_BUFFERS + 1, KEEP_RELEASE of 0..MAX_KEEPS + 1 */
                c->kind = kind == 12 ? SHR_CMD_KEEP_RELEASE : (uint8_t)(SHR_CMD_BUFFER_REGISTER + kind - 9);
                c->flags = 0;
                c->buffer = fr_u8(&r) % (kind == 12 ? MAX_KEEPS + 2 : MAX_BUFFERS + 2);
                if (c->kind == SHR_CMD_BUFFER_REGISTER) {
                    static const shr_pixel_format fmts[] = {SHR_FORMAT_A4, SHR_FORMAT_A8, SHR_FORMAT_RGBA8888,
                                                            SHR_FORMAT_RGB565, SHR_FORMAT_RGBX8888};
                    uint8_t fb = fr_u8(&r);
                    int32_t bw = fr_i8(&r), bh = fr_i8(&r);
                    size_t off = (size_t)fr_u8(&r) * 8;
                    uint32_t bstride = fr_u8(&r);
                    size_t len = fr_u16(&r) % (SRC_BYTES + 1);
                    if (len > SRC_BYTES - off) len = SRC_BYTES - off; /* never beyond the real buffer */
                    c->src = bounded((shr_image_ref){src_buf + off, bw, bh, bstride, (uint8_t)fmts[(fb & 0x3F) % 5],
                                                     fb & 0x80 ? SHR_MEMORY_DEVICE : fb & 0x40 ? SHR_MEMORY_DMA : SHR_MEMORY_CPU, 0},
                                     len);
                } else if (c->kind == SHR_CMD_BUFFER_UPDATE) {
                    c->src_rect = fr_rect(&r);
                }
                if (drawn) expect_ok = false;
                else if (prologue_ok && (prologue_ok = apply(c)) && c->kind == SHR_CMD_BUFFER_UPDATE)
                    touch(c->buffer, c->src_rect, start);
                n++;
                continue;
            }
            drawn = true;
            if (kind == 6) { /* a keep group that starts by covering its area, as the contract requires */
                if (n + 2 > count) break;
                uint32_t id = fr_u8(&r) % (MAX_KEEPS + 2);
                shr_rect d = fr_rect(&r);
                c->kind = SHR_CMD_KEEP_BEGIN, c->flags = 0, c->dst = d, c->buffer = id;
                fuzz_blank(&c[1], seed);
                c[1].kind = SHR_CMD_FILL, c[1].flags = 0, c[1].dst = d, c[1].color = SHR_RGB(fr_u8(&r), 9, 99);
                bool ok = group == SIZE_MAX && id >= 1 && id <= MAX_KEEPS && d.x0 <= d.x1 && d.y0 <= d.y1 && !state[id - 1] &&
                          (!slot || (uint64_t)(d.x1 - d.x0) * (uint64_t)(d.y1 - d.y0) * bpp <= slot);
                if (ok) state[id - 1] = 1, sw[id - 1] = d.x1 - d.x0, sh[id - 1] = d.y1 - d.y0;
                expect_ok &= ok;
                group = n;
                n += 2;
                continue;
            }
            if (kind == 7) {
                c->kind = SHR_CMD_KEEP_END, c->flags = 0;
                expect_ok &= group != SIZE_MAX;
                group = SIZE_MAX;
                n++;
                continue;
            }
            c->kind = kind; /* 0 is invalid on purpose */
            c->flags = fr_u8(&r) & 15;
            c->dst = fr_rect(&r);
            shr_color color = SHR_RGB(fr_u8(&r), fr_u8(&r), fr_u8(&r));
            if (kind == SHR_CMD_FILL || kind == SHR_CMD_GLYPH) c->color = color;
            shr_rect lim = group != SIZE_MAX ? cmds[group].dst : (shr_rect){0, 0, w, h};
            bool in = (group != SIZE_MAX || inside(c->dst, w, h)) && c->dst.x0 <= c->dst.x1 && c->dst.y0 <= c->dst.y1 &&
                      c->dst.x0 >= lim.x0 && c->dst.y0 >= lim.y0 && c->dst.x1 <= lim.x1 && c->dst.y1 <= lim.y1;
            int64_t dw = c->dst.x1 - c->dst.x0, dh = c->dst.y1 - c->dst.y0;
            if (kind == 8) { /* KEEP_DRAW of ids 0..MAX_KEEPS + 1 */
                c->kind = SHR_CMD_KEEP_DRAW, c->flags = 0;
                c->buffer = fr_u8(&r) % (MAX_KEEPS + 2);
                c->src_origin = (shr_point){fr_i8(&r), fr_i8(&r)};
                uint32_t k = c->buffer ? c->buffer - 1 : MAX_KEEPS;
                bool known = k < MAX_KEEPS && (state[k] == 1 || keeps[k].held);
                int32_t kw = known ? (state[k] == 1 ? sw[k] : keeps[k].w) : 0, kh = known ? (state[k] == 1 ? sh[k] : keeps[k].h) : 0;
                if (known && !state[k]) state[k] = 2;
                expect_ok &= group == SIZE_MAX && in && known && c->src_origin.x >= 0 && c->src_origin.y >= 0 &&
                             c->src_origin.x + dw <= kw && c->src_origin.y + dh <= kh;
                n++;
                continue;
            }
            if (c->kind == SHR_CMD_GLYPH || c->kind == SHR_CMD_IMAGE) {
                c->buffer = fr_u8(&r) % (MAX_BUFFERS + 2);
                c->src_rect = fr_rect(&r);
                c->src_origin = (shr_point){fr_i8(&r), fr_i8(&r)};
                if (c->kind == SHR_CMD_GLYPH) { /* past +-100 in steps of 40 across the +-4096 bound, then the extremes */
                    int32_t a = fr_i8(&r);
                    c->slant_axis = a == 127 ? INT32_MAX : a <= -127 ? INT32_MIN : a > 100 || a < -100 ? 40 * a : a;
                    c->bg = 0;
                }
                shr_image b = registered(c->buffer) ? model[c->buffer - 1] : (shr_image){0};
                shr_rect s = c->src_rect;
                int32_t rw = s.x1 - s.x0, rh = s.y1 - s.y0;
                bool fmt_ok = c->kind == SHR_CMD_IMAGE ? b.format == SHR_FORMAT_RGBA8888
                                                       : b.format == SHR_FORMAT_A4 || b.format == SHR_FORMAT_A8;
                bool rect_ok = inside(s, b.width, b.height) && (b.format != SHR_FORMAT_A4 || s.x0 % 2 == 0);
                /* Columns a BOLD or ITALIC GLYPH may take (rect sizes, at most 127, never exceed SHR_GLYPH_SYNTH_MAX). */
                uint32_t syn = c->kind == SHR_CMD_GLYPH ? c->flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC) : 0;
                bool italic = syn & SHR_GLYPH_ITALIC, frac = false, unused;
                bool axis_ok = !italic || (c->slant_axis >= -4 * SHR_GLYPH_SYNTH_MAX && c->slant_axis <= 4 * SHR_GLYPH_SYNTH_MAX);
                int64_t x0 = 0, x1 = rw;
                if (syn && axis_ok) {
                    int32_t k0 = italic ? slant(c->slant_axis, 0, &frac) : 0;
                    x0 = italic ? slant(c->slant_axis, rh - 1, &unused) : 0;
                    x1 = rw + (syn & SHR_GLYPH_BOLD ? 1 : 0) + k0 + frac;
                }
                expect_ok &= in && b.format && fmt_ok && rect_ok && axis_ok && c->src_origin.x >= x0 &&
                             c->src_origin.y >= 0 && c->src_origin.x + dw <= x1 && c->src_origin.y + dh <= rh;
                n++;
                continue;
            }
            static const shr_pixel_format fmts[] = {SHR_FORMAT_A4, SHR_FORMAT_A8, SHR_FORMAT_RGBA8888,
                                                    SHR_FORMAT_RGB565, SHR_FORMAT_RGBX8888};
            uint8_t fb = fr_u8(&r);
            shr_pixel_format sf = fmts[fb % 5];
            int32_t srw = fr_i8(&r), srh = fr_i8(&r);
            uint32_t sstride = fr_u8(&r);
            size_t slen = fr_u16(&r) % (SRC_BYTES + 1); /* never beyond the real buffer */
            c->src = bounded((shr_image_ref){src_buf, srw, srh, sstride, (uint8_t)sf, SHR_MEMORY_CPU, 0}, slen);
            srh = c->src.height;
            self[n] = c->kind == SHR_CMD_COPY && fb >= 250;
            if (self[n]) {
                srw = w, srh = h, sf = dfmt;
                c->src = (shr_image_ref){NULL, w, h, (uint32_t)stride, (uint8_t)dfmt, SHR_MEMORY_CPU, 0};
                c->flags = fb & 1 ? SHR_COPY_REST_UNDEFINED : 0;
            }
            shr_point origin = {fr_i8(&r), fr_i8(&r)};
            if (c->kind != SHR_CMD_ROTATE) c->src_origin = origin;
            if (c->kind == SHR_CMD_ROTATE) {
                c->rotation = fr_u8(&r) % 4;
                bool quarter = c->rotation == SHR_ROTATE_90_CW || c->rotation == SHR_ROTATE_90_CCW;
                int32_t rw = (int32_t)(quarter ? dh : dw), rh = (int32_t)(quarter ? dw : dh);
                if (rw > 0 && rh > 0 && (size_t)rw * (size_t)rh * bpp <= sizeof(rot_buf))
                    c->src = (shr_image_ref){rot_buf, rw, rh, (uint32_t)((size_t)rw * bpp), (uint8_t)dfmt, SHR_MEMORY_CPU, 0};
            }
            shr_image sm;
            bool src_ok = (self[n] || shr_image_ref_get(&c->src, &sm) == SHR_OK) && origin.x >= 0 && origin.y >= 0 &&
                          origin.x + dw <= srw && origin.y + dh <= srh;
            bool screen = sf == SHR_FORMAT_RGB565 || sf == SHR_FORMAT_RGBX8888;
            if (c->kind == SHR_CMD_FILL) expect_ok &= in;
            else if (c->kind == SHR_CMD_COPY) expect_ok &= in && src_ok && screen && (!self[n] || group == SIZE_MAX);
            else if (c->kind == SHR_CMD_ROTATE) expect_ok &= group == SIZE_MAX && shr_software_execute(&dst, c, 1, model, MAX_BUFFERS) == SHR_OK;
            else expect_ok = false;
            n++;
        }
        expect_ok &= group == SIZE_MAX;

        /* The reference: draws outside groups by the stateless path, groups into the harness's keeps. */
        memcpy(plain, init_buf, sizeof(plain));
        dst.pixels = plain;
        for (size_t i = 0; i < n; i++)
            if (self[i]) cmds[i].src.pixels = plain;
        bool ok = expect_ok && prologue_ok;
        if (ok) FUZZ_CHECK(fuzz_keep_draw(&dst, NULL, cmds, n, model, MAX_BUFFERS, keeps, MAX_KEEPS));
        /* Nothing outside the written areas changed (stride padding and bytes past the surface included). */
        for (size_t off = 0; ok && off < sizeof(plain); off++) {
            if (plain[off] == init_buf[off]) continue;
            FUZZ_CHECK(off < dst.byte_length && off % stride < (size_t)w * bpp);
            int32_t x = (int32_t)(off % stride / bpp), y = (int32_t)(off / stride);
            bool hit = false;
            for (size_t i = 0, in_group = 0; i < n && !hit; i++) {
                if (cmds[i].kind == SHR_CMD_KEEP_BEGIN || cmds[i].kind == SHR_CMD_KEEP_END) in_group = cmds[i].kind == SHR_CMD_KEEP_BEGIN;
                else if (!in_group && (cmds[i].kind < SHR_CMD_BUFFER_REGISTER || cmds[i].kind == SHR_CMD_LINE))
                    hit = x >= cmds[i].dst.x0 && x < cmds[i].dst.x1 && y >= cmds[i].dst.y0 && y < cmds[i].dst.y1;
            }
            FUZZ_CHECK(hit);
        }
        memcpy(driven, init_buf, sizeof(driven));
        dst.pixels = driven;
        for (size_t i = 0; i < n; i++)
            if (self[i]) cmds[i].src.pixels = driven;
        shr_status got = drv.execute(drv.user, &dst, cmds, n, 1);
        late_wait(NULL, late.started);
        FUZZ_CHECK((got == SHR_OK) == ok);
        FUZZ_CHECK(memcmp(driven, ok ? plain : init_buf, sizeof(driven)) == 0);
        if (!ok || (cfg & 32)) resync(&drv, &dst, !ok);
    }
    FUZZ_CHECK(shr_software_driver_destroy(&drv) == SHR_OK && late.done == late.started);
    return 0;
}
