/* The ANGLE driver against the software port: every batch is drawn by both from the same starting pixels and
 * compared, exactly for FILL/COPY/ROTATE and within MAX_BLEND per channel (in the destination's own channel
 * units) for blends. Skips (exit code 77) when no EGL display can be created. */
#define _POSIX_C_SOURCE 200809L

#include <shiroko/port_angle.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "harness.h"
#include "raster.h"
#include "shr_glyph.h"

/* Metal blends RGB565 targets at reduced precision; RGBX8888 blends match exactly. */
#define MAX_BLEND(f) ((f) == SHR_FORMAT_RGB565 ? 1 : 0)

static const shr_pixel_format FMTS[2] = {SHR_FORMAT_RGB565, SHR_FORMAT_RGBX8888};

static shr_framebuffer_driver gl;

enum { K_FILL, K_DIM, K_GLYPH, K_SYNTH, K_IMAGE, K_LINE, K_COPY, K_ROTATE, K_SCENE, KINDS };
static const char *const kind_names[KINDS] = {"fill", "fill dim", "glyph", "styled", "image", "line", "copy", "rotate",
                                             "scene"};
static int max_diff[KINDS][2]; /* by destination format: RGB565, RGBX8888 */
static void (*pre_execute)(void); /* runs right before the driver's execute() in compare_with() */

static size_t bpp(shr_pixel_format f) { return f == SHR_FORMAT_RGB565 ? 2 : 4; }

static uint32_t rng_state = 0x2545F491u;
static uint32_t rnd(void) {
    rng_state ^= (rng_state & 0x7FFFFu) << 13, rng_state ^= rng_state >> 17, rng_state ^= (rng_state & 0x7FFFFFFu) << 5;
    return rng_state;
}

static void noise(void *p, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)rnd();
}

static shr_surface packed(void *buf, shr_pixel_format f, int32_t w, int32_t h) {
    size_t stride = (size_t)w * bpp(f);
    return (shr_surface){buf, w, h, stride, stride * (size_t)h, f, 1, SHR_MEMORY_CPU, 0};
}

static shr_image_ref as_image(const shr_surface *s) {
    return (shr_image_ref){s->pixels, s->width, s->height, (uint32_t)s->stride, (uint8_t)s->format, (uint8_t)s->domain, 0};
}

/* Buffer memory of w x h pixels in rows of `stride` bytes. */
static shr_image mem(const void *px, shr_pixel_format f, int32_t w, int32_t h, size_t stride) {
    size_t row = 0;
    shr_format_row_bytes(f, w, &row);
    return (shr_image){px, w, h, stride, h ? (size_t)(h - 1) * stride + row : 0, f, SHR_MEMORY_CPU};
}

/* Largest channel difference in the format's own units (5/6/5 bits or bytes; X ignored). */
static int pixel_diff(shr_pixel_format f, const uint8_t *a, const uint8_t *b) {
    int d = 0;
    if (f == SHR_FORMAT_RGB565) {
        uint16_t u, v;
        memcpy(&u, a, 2), memcpy(&v, b, 2);
        const int shift[3] = {11, 5, 0}, mask[3] = {31, 63, 31};
        for (int c = 0; c < 3; c++) {
            int e = abs((u >> shift[c] & mask[c]) - (v >> shift[c] & mask[c]));
            d = e > d ? e : d;
        }
    } else {
        for (int c = 0; c < 3; c++) d = abs(a[c] - b[c]) > d ? abs(a[c] - b[c]) : d;
    }
    return d;
}

static int image_diff(shr_pixel_format f, const uint8_t *a, const uint8_t *b, int32_t w, int32_t h, size_t stride) {
    int d = 0;
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            size_t o = (size_t)y * stride + (size_t)x * bpp(f);
            int e = pixel_diff(f, a + o, b + o);
            d = e > d ? e : d;
        }
    return d;
}

static void note(int kind, shr_pixel_format f, int d) {
    int *m = &max_diff[kind][f == SHR_FORMAT_RGBX8888];
    *m = d > *m ? d : *m;
}

/* ---- buffers: the table the software port reads, and the REGISTER commands that hand it to a driver ---- */

#define IDS 64
static shr_image bufs[IDS]; /* id k at [k - 1] */
static uint32_t last_id;
static bool auto_register = true; /* compare_with() registers the buffers a batch without a prologue draws from */

static uint32_t use(shr_image m) {
    last_id = last_id % IDS + 1;
    bufs[last_id - 1] = m;
    return last_id;
}

static shr_draw_cmd reg(uint32_t id) { return (shr_draw_cmd){.kind = SHR_CMD_BUFFER_REGISTER, .buffer = id, .src = img_ref(bufs[id - 1])}; }

static shr_draw_cmd buf_cmd(shr_cmd_kind kind, uint32_t id, shr_rect r) {
    return (shr_draw_cmd){.kind = (uint8_t)kind, .buffer = id, .src_rect = r};
}

/* `rect` of buffer `id` drawn at `dst`, rect pixel `origin` at its top-left. */
static shr_draw_cmd from(shr_cmd_kind kind, shr_rect dst, uint32_t id, shr_rect rect, shr_point origin, shr_color color) {
    return (shr_draw_cmd){.kind = (uint8_t)kind, .dst = dst, .color = color, .buffer = id, .src_rect = rect, .src_origin = origin};
}

/* All of `m`, as a buffer of its own. */
static shr_draw_cmd whole(shr_cmd_kind kind, shr_rect dst, shr_image m, shr_point origin, shr_color color) {
    return from(kind, dst, use(m), (shr_rect){0, 0, m.width, m.height}, origin, color);
}

/* `cmds` with a REGISTER of each buffer it draws from in front, unless it starts with buffer commands; *n grows. */
static shr_draw_cmd *with_prologue(const shr_draw_cmd *cmds, size_t *n) {
    shr_draw_cmd *out = calloc(2 * *n + 1, sizeof(*out));
    size_t k = 0;
    bool given = !auto_register || (*n && shr__buffer_cmd(cmds[0].kind));
    for (size_t i = 0; i < *n && !given; i++) {
        if (cmds[i].kind != SHR_CMD_GLYPH && cmds[i].kind != SHR_CMD_IMAGE) continue;
        bool seen = false;
        for (size_t j = 0; j < k; j++) seen |= out[j].buffer == cmds[i].buffer;
        if (!seen && cmds[i].buffer && cmds[i].buffer <= IDS) out[k++] = reg(cmds[i].buffer);
    }
    memcpy(out + k, cmds, *n * sizeof(*cmds));
    *n += k;
    return out;
}

/* Stands for the destination itself as a COPY/ROTATE source. */
static const char self_marker;
#define SELF ((const void *)&self_marker)

/* DEVICE surfaces made by device_with() and the CPU pixels the software port reads in their place. */
static struct {
    const void *handle;
    void *pixels;
} twins[8];

/* `software`: DEVICE sources are replaced by their CPU twins. */
static shr_draw_cmd *bound_to(const shr_draw_cmd *cmds, size_t n, const shr_surface *dst, bool software) {
    shr_draw_cmd *out = calloc(n ? n : 1, sizeof(*out));
    for (size_t i = 0; i < n; i++) {
        out[i] = cmds[i];
        if (out[i].src.pixels == SELF) out[i].src = as_image(dst);
        for (int t = 0; t < 8 && software && out[i].src.domain == SHR_MEMORY_DEVICE; t++)
            if (twins[t].handle == out[i].src.pixels) {
                shr_surface cpu = packed(twins[t].pixels, out[i].src.format, out[i].src.width, out[i].src.height);
                out[i].src = as_image(&cpu);
            }
    }
    return out;
}

/* A DEVICE surface of `d` holding `pixels` (packed), written by the driver's exact COPY. */
static shr_surface device_on(shr_framebuffer_driver *d, shr_pixel_format f, int32_t w, int32_t h, void *pixels) {
    shr_surface out, s = packed(pixels, f, w, h);
    ASSERT_EQ_LL(shr_angle_surface_create(d, w, h, f, &out), SHR_OK);
    shr_draw_cmd c = {.kind = SHR_CMD_COPY, .dst = {0, 0, w, h}, .src = as_image(&s)};
    ASSERT_EQ_LL(d->execute(d->user, &out, &c, 1, 0), SHR_OK);
    return out;
}

/* A source surface; CPU references read `pixels`, which must stay alive. */
static shr_surface device_with(shr_pixel_format f, int32_t w, int32_t h, void *pixels) {
    shr_surface s = device_on(&gl, f, w, h, pixels);
    int t = 0;
    while (twins[t].handle) t++;
    twins[t].handle = s.pixels, twins[t].pixels = pixels;
    return s;
}

static void device_drop(shr_surface *s) {
    for (int t = 0; t < 8; t++)
        if (twins[t].handle == s->pixels) twins[t].handle = NULL;
    ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, s), SHR_OK);
}

static uint64_t draws_of(const shr_framebuffer_driver *d) {
    shr_angle_stats st;
    ASSERT_EQ_LL(shr_angle_driver_stats(d, &st), SHR_OK);
    return st.draws;
}

static uint32_t textures_of(const shr_framebuffer_driver *d) {
    shr_angle_stats st;
    ASSERT_EQ_LL(shr_angle_driver_stats(d, &st), SHR_OK);
    return st.textures;
}

static uint64_t last_draws; /* draw calls of the last compared execute() */

/* Draws the batch with the software port and with `d` (into a CPU or DEVICE surface) from the same noise. */
static int compare_with(shr_framebuffer_driver *d, const shr_draw_cmd *cmds, size_t n, shr_pixel_format f, int32_t w,
                        int32_t h, bool device, int kind) {
    size_t len = (size_t)w * (size_t)h * bpp(f), m = n;
    uint8_t *init = malloc(len), *ref = malloc(len), *out = malloc(len);
    noise(init, len);
    memcpy(ref, init, len), memcpy(out, init, len);
    shr_surface rs = packed(ref, f, w, h), ds = packed(out, f, w, h);
    shr_draw_cmd *all = with_prologue(cmds, &m);
    shr_draw_cmd *rc = bound_to(all, m, &rs, true);
    ASSERT_EQ_LL(shr_software_execute(&rs, rc, m, bufs, IDS), SHR_OK);
    if (device) ds = device_on(d, f, w, h, init);
    shr_draw_cmd *dc = bound_to(all, m, &ds, false);
    if (pre_execute) pre_execute();
    uint64_t before = draws_of(d);
    ASSERT_EQ_LL(d->execute(d->user, &ds, dc, m, 0), SHR_OK);
    last_draws = draws_of(d) - before;
    if (device) {
        ASSERT_EQ_LL(shr_angle_surface_read(d, &ds, out, (size_t)w * bpp(f)), SHR_OK);
        ASSERT_EQ_LL(shr_angle_surface_destroy(d, &ds), SHR_OK);
    }
    int diff = image_diff(f, ref, out, w, h, (size_t)w * bpp(f));
    note(kind, f, diff);
    free(all), free(rc), free(dc), free(init), free(ref), free(out);
    return diff;
}

static int compare(const shr_draw_cmd *cmds, size_t n, shr_pixel_format f, int32_t w, int32_t h, bool device, int kind) {
    return compare_with(&gl, cmds, n, f, w, h, device, kind);
}

/* Every destination format, CPU and DEVICE. */
#define EACH_TARGET(f, dev)                \
    for (int fi_ = 0; fi_ < 2; fi_++)      \
        for (int dev = 0; dev < 2; dev++)  \
            for (shr_pixel_format f = FMTS[fi_]; f; f = 0)

static shr_color rnd_color(void) { return SHR_RGB(rnd(), rnd(), rnd()); }

/* ---- offscreen and driver lifetime (run before the shared context exists) ---- */

TEST offscreen_backends_and_lifetime(void) {
    shr_angle_offscreen *a, *b;
    ASSERT_EQ_LL(shr_angle_offscreen_create(16, 16, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_offscreen_create(0, 16, &a), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(a == NULL, 1);
    ASSERT_EQ_LL(shr_angle_offscreen_create(16, -1, &a), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_offscreen_backend(NULL) == NULL, 1);
    ASSERT_EQ_LL(shr_angle_offscreen_destroy(NULL), SHR_E_INVALID_ARG);

    const char *saved = getenv("SHIROKO_ANGLE_BACKEND");
    char keep[32] = "";
    if (saved) snprintf(keep, sizeof(keep), "%s", saved);
    /* An unknown name keeps the default order; a backend this build lacks falls back. */
    setenv("SHIROKO_ANGLE_BACKEND", "bogus", 1);
    ASSERT_EQ_LL(shr_angle_offscreen_create(8, 8, &a), SHR_OK);
    const char *first = shr_angle_offscreen_backend(a);
    static const char *const names[] = {"metal", "opengl", "vulkan", "default", "d3d11"};
    for (int i = 0; i < 5; i++) {
        setenv("SHIROKO_ANGLE_BACKEND", names[i], 1);
        ASSERT_EQ_LL(shr_angle_offscreen_create(8, 8, &b), SHR_OK);
        printf("backend: %s requested, %s obtained\n", names[i], shr_angle_offscreen_backend(b));
        if (i < 4) ASSERT_EQ_LL(shr_angle_offscreen_destroy(b), SHR_OK);
    }
    printf("backend by default: %s\n", first);
    if (saved) setenv("SHIROKO_ANGLE_BACKEND", keep, 1);
    else unsetenv("SHIROKO_ANGLE_BACKEND");

    /* Two contexts alive: b is current; destroying a keeps b usable. */
    ASSERT_EQ_LL(shr_angle_offscreen_destroy(a), SHR_OK);
    ASSERT(glGetString(GL_VERSION) != NULL);
    ASSERT_EQ_LL(shr_angle_offscreen_destroy(b), SHR_OK);

    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 0, 0, 4, &d), SHR_E_STATE);
    PASS();
}

SUITE(lifetime) { RUN_TEST(offscreen_backends_and_lifetime); }

/* ---- driver API ---- */

TEST driver_create_and_destroy(void) {
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 0, 0, 4, NULL), SHR_E_INVALID_ARG);
    fail_alloc f = {-1, 0};
    shr_allocator half = fail_allocator(&f);
    half.free = NULL;
    ASSERT_EQ_LL(shr_angle_driver_create(&half, 0, 0, 0, 4, &d), SHR_E_INVALID_ARG);
    shr_allocator a = fail_allocator(&f);
    for (long budget = 0; budget < 6; budget++) { /* the driver, its buffer and keep tables, its instances */
        f.budget = budget;
        ASSERT_EQ_LL(shr_angle_driver_create(&a, 0, 0, 0, 4, &d), SHR_E_NO_MEMORY);
        ASSERT_EQ_LL(f.live, 0);
    }
    f.budget = -1;
    ASSERT_EQ_LL(shr_angle_driver_create(&a, 1u << 20, 0, 0, 4, &d), SHR_OK);
    ASSERT_EQ_LL(d.caps.domains, SHR_MEMORY_CPU | SHR_MEMORY_DEVICE);
    ASSERT(d.caps.max_width >= 2048 && d.caps.max_width == d.caps.max_height);
    ASSERT_EQ_LL(d.caps.max_buffers, 4);
    ASSERT_EQ_LL(d.caps.max_buffer_width, d.caps.max_width);
    ASSERT_EQ_LL(d.caps.max_buffer_height, d.caps.max_height);
    ASSERT_EQ_LL(d.caps.buffer_bytes, 1u << 20);
    ASSERT_EQ_LL(d.caps.buffer_flags, SHR_BUFFER_COPIES);
    ASSERT_EQ_LL(d.caps.flags, SHR_DRIVER_CHEAP_MOVE | SHR_DRIVER_SCALE);
    ASSERT_EQ_LL(d.reset(d.user), SHR_OK);
    /* Destroy frees surfaces and buffer textures still alive. */
    shr_surface s;
    ASSERT_EQ_LL(shr_angle_surface_create(&d, 4, 4, SHR_FORMAT_RGB565, &s), SHR_OK);
    uint8_t cov[4] = {255, 0, 128, 7}, px[4 * 4 * 2];
    shr_surface cpu = packed(px, SHR_FORMAT_RGB565, 4, 4);
    shr_draw_cmd c[] = {{.kind = SHR_CMD_BUFFER_REGISTER, .buffer = 4, .src = img_ref(mem(cov, SHR_FORMAT_A8, 2, 2, 2))},
                        from(SHR_CMD_GLYPH, (shr_rect){0, 0, 2, 2}, 4, (shr_rect){0, 0, 2, 2}, (shr_point){0, 0}, SHR_RGB(1, 2, 3))};
    ASSERT_EQ_LL(d.execute(d.user, &cpu, c, 2, 0), SHR_OK);
    shr_angle_stats st;
    ASSERT_EQ_LL(shr_angle_driver_stats(&d, &st), SHR_OK);
    ASSERT(st.draws == 1 && st.instances == 1 && st.textures == 1 && st.texture_bytes == 4);
    ASSERT_EQ_LL(shr_angle_driver_stats(&d, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_driver_stats(NULL, &st), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    ASSERT_EQ_LL(d.execute == NULL, 1);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_driver_destroy(NULL), SHR_E_INVALID_ARG);
    shr_framebuffer_driver sw;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, 0, 1, &sw), SHR_OK);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&sw), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_driver_stats(&sw, &st), SHR_E_INVALID_ARG);
    shr_software_driver_destroy(&sw);
    /* No buffer ids at all. */
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 0, 0, 0, &d), SHR_OK);
    ASSERT_EQ_LL(d.execute(d.user, &cpu, c, 2, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
    PASS();
}

TEST surface_api(void) {
    shr_surface s, t;
    uint32_t tex = 0;
    ASSERT_EQ_LL(shr_angle_surface_create(&gl, 4, 4, SHR_FORMAT_RGB565, NULL), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_surface_create(NULL, 4, 4, SHR_FORMAT_RGB565, &s), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_surface_create(&gl, 0, 4, SHR_FORMAT_RGB565, &s), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_surface_create(&gl, 4, 0, SHR_FORMAT_RGB565, &s), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_surface_create(&gl, 4, 4, SHR_FORMAT_A8, &s), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_surface_create(&gl, gl.caps.max_width + 1, 4, SHR_FORMAT_RGB565, &s), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr_angle_surface_create(&gl, 4, gl.caps.max_height + 1, SHR_FORMAT_RGB565, &s), SHR_E_UNSUPPORTED);

    for (int i = 0; i < 2; i++) {
        ASSERT_EQ_LL(shr_angle_surface_create(&gl, 5, 3, FMTS[i], &s), SHR_OK);
        ASSERT_EQ_LL(shr_surface_validate(&s), SHR_OK);
        ASSERT_EQ_LL(s.domain, SHR_MEMORY_DEVICE);
        ASSERT(s.resource_id != 0 && s.pixels != NULL);
        ASSERT_EQ_LL(s.stride, 5 * bpp(FMTS[i]));
        /* Cleared to black, read with padding rows. */
        uint8_t px[3 * 32];
        memset(px, 0xAB, sizeof(px));
        ASSERT_EQ_LL(shr_angle_surface_read(&gl, &s, px, 32), SHR_OK);
        for (int y = 0; y < 3; y++) {
            for (size_t x = 0; x < 5 * bpp(FMTS[i]); x++)
                ASSERT_EQ_LL(px[y * 32 + (int)x], FMTS[i] == SHR_FORMAT_RGBX8888 && x % 4 == 3 ? 255 : 0);
            ASSERT_EQ_LL(px[y * 32 + 31], 0xAB);
        }
        ASSERT_EQ_LL(shr_angle_surface_read(&gl, &s, NULL, 32), SHR_E_INVALID_ARG);
        ASSERT_EQ_LL(shr_angle_surface_read(&gl, &s, px, 5 * bpp(FMTS[i]) - 1), SHR_E_INVALID_ARG);
        ASSERT_EQ_LL(shr_angle_surface_texture(&gl, &s, &tex), SHR_OK);
        ASSERT(glIsTexture(tex));
        ASSERT_EQ_LL(shr_angle_surface_create(&gl, 5, 3, FMTS[i], &t), SHR_OK);
        ASSERT(t.resource_id != s.resource_id);
        /* Dimensions that do not match the surface, another driver's or a CPU surface. */
        shr_surface bad = s;
        bad.width = 4;
        ASSERT_EQ_LL(shr_angle_surface_read(&gl, &bad, px, 32), SHR_E_INVALID_ARG);
        ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &bad), SHR_E_INVALID_ARG);
        bad = s, bad.format = FMTS[1 - i];
        ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &bad), SHR_E_INVALID_ARG);
        bad = s, bad.height = 1;
        ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &bad), SHR_E_INVALID_ARG);
        bad = s, bad.domain = SHR_MEMORY_CPU;
        ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &bad), SHR_E_INVALID_ARG);
        bad = s, bad.pixels = px;
        ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &bad), SHR_E_INVALID_ARG);
        ASSERT_EQ_LL(shr_angle_surface_destroy(NULL, &s), SHR_E_INVALID_ARG);
        ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, NULL), SHR_E_INVALID_ARG);
        ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &s), SHR_OK);
        ASSERT_EQ_LL(s.pixels == NULL, 1);
        ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &t), SHR_OK);
    }
    PASS();
}

TEST surface_texture_and_orientation(void) {
    shr_surface s, stale;
    uint32_t tex = 0;
    ASSERT_EQ_LL(shr_angle_surface_create(&gl, 4, 3, SHR_FORMAT_RGBX8888, &s), SHR_OK);
    shr_draw_cmd top = {.kind = SHR_CMD_FILL, .dst = {0, 0, 4, 1}, .color = SHR_RGB(255, 0, 0)};
    ASSERT_EQ_LL(gl.execute(gl.user, &s, &top, 1, 0), SHR_OK);
    ASSERT_EQ_LL(shr_angle_surface_texture(&gl, &s, &tex), SHR_OK);
    /* Texel row 0 (GL's first row) is the top row. */
    GLuint fbo;
    uint8_t px[3][16];
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glReadPixels(0, 0, 4, 3, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    ASSERT_EQ_LL(px[0][0], 255);
    ASSERT_EQ_LL(px[0][12], 255);
    ASSERT_EQ_LL(px[2][0], 0);

    ASSERT_EQ_LL(shr_angle_surface_texture(NULL, &s, &tex), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_surface_texture(&gl, NULL, &tex), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_surface_texture(&gl, &s, NULL), SHR_E_INVALID_ARG);
    shr_surface cpu = s;
    cpu.domain = SHR_MEMORY_CPU;
    ASSERT_EQ_LL(shr_angle_surface_texture(&gl, &cpu, &tex), SHR_E_INVALID_ARG);
    /* A copy kept after destroy names nothing. */
    stale = s;
    ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &s), SHR_OK);
    ASSERT_EQ_LL(shr_angle_surface_texture(&gl, &stale, &tex), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_surface_read(&gl, &stale, px, 16), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &stale), SHR_E_INVALID_ARG);
    shr_draw_cmd from_stale = {.kind = SHR_CMD_COPY, .dst = {0, 0, 4, 3}, .src = as_image(&stale)};
    uint8_t out[4 * 3 * 4];
    shr_surface o = packed(out, SHR_FORMAT_RGBX8888, 4, 3);
    ASSERT_EQ_LL(gl.execute(gl.user, &o, &from_stale, 1, 0), SHR_E_UNSUPPORTED);
    PASS();
}

TEST calls_need_the_driver_context(void) {
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    EGLContext ctx = eglGetCurrentContext();
    shr_surface s, t;
    ASSERT_EQ_LL(shr_angle_surface_create(&gl, 4, 4, SHR_FORMAT_RGB565, &s), SHR_OK);
    uint16_t px[16], before[16];
    shr_surface cpu = packed(px, SHR_FORMAT_RGB565, 4, 4);
    shr_draw_cmd fill = {.kind = SHR_CMD_FILL, .dst = {0, 0, 4, 4}, .color = SHR_RGB(9, 99, 199)};
    uint32_t tex;
    shr_angle_stats st;
    for (int k = 0; k < 2; k++) { /* none current, then another context */
        shr_angle_offscreen *other = NULL;
        if (k == 0) ASSERT(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        else ASSERT_EQ_LL(shr_angle_offscreen_create(8, 8, &other), SHR_OK);
        noise(px, sizeof(px));
        memcpy(before, px, sizeof(px));
        ASSERT_EQ_LL(gl.execute(gl.user, &cpu, &fill, 1, 0), SHR_E_STATE);
        ASSERT_EQ_LL(memcmp(px, before, sizeof(px)), 0);
        ASSERT_EQ_LL(gl.execute(gl.user, &s, &fill, 1, 0), SHR_E_STATE);
        ASSERT_EQ_LL(shr_angle_surface_create(&gl, 4, 4, SHR_FORMAT_RGB565, &t), SHR_E_STATE);
        ASSERT_EQ_LL(shr_angle_surface_read(&gl, &s, px, 8), SHR_E_STATE);
        ASSERT_EQ_LL(shr_angle_surface_texture(&gl, &s, &tex), SHR_E_STATE);
        ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &s), SHR_E_STATE);
        ASSERT_EQ_LL(shr_angle_driver_destroy(&gl), SHR_E_STATE);
        ASSERT_EQ_LL(shr_angle_driver_stats(&gl, &st), SHR_OK);
        if (other) ASSERT_EQ_LL(shr_angle_offscreen_destroy(other), SHR_OK);
        ASSERT(eglMakeCurrent(dpy, draw, read, ctx));
    }
    ASSERT_EQ_LL(gl.execute(gl.user, &s, &fill, 1, 0), SHR_OK);
    ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &s), SHR_OK);
    PASS();
}

/* State an application may leave behind, set right before the compared execute(). */
static GLuint hostile_prog, hostile_vao, hostile_bufs[2], hostile_sampler;

static void hostile_state(void) {
    glUseProgram(hostile_prog);
    glBindVertexArray(hostile_vao);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, hostile_bufs[0]);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, hostile_bufs[1]);
    glBlendFunc(GL_ONE, GL_ZERO);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT_AND_BACK);
    glEnable(GL_DITHER);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
    for (GLuint k = 0; k < 9; k++) glBindSampler(k, hostile_sampler);
    glActiveTexture(GL_TEXTURE5);
}

TEST hostile_application_state_is_reset(void) {
    static const char *const src[2] = {"#version 300 es\nvoid main() { gl_Position = vec4(0.0); }\n",
                                       "#version 300 es\nprecision mediump float;\nout vec4 o;\n"
                                       "void main() { o = vec4(1.0); }\n"};
    hostile_prog = glCreateProgram();
    for (int i = 0; i < 2; i++) {
        GLuint sh = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        glShaderSource(sh, 1, &src[i], NULL);
        glCompileShader(sh);
        glAttachShader(hostile_prog, sh);
        glDeleteShader(sh);
    }
    glLinkProgram(hostile_prog);
    GLint linked = 0;
    glGetProgramiv(hostile_prog, GL_LINK_STATUS, &linked);
    ASSERT(linked);
    glGenVertexArrays(1, &hostile_vao);
    glGenBuffers(2, hostile_bufs);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(i ? GL_PIXEL_PACK_BUFFER : GL_PIXEL_UNPACK_BUFFER, hostile_bufs[i]);
        glBufferData(i ? GL_PIXEL_PACK_BUFFER : GL_PIXEL_UNPACK_BUFFER, 1 << 16, NULL, GL_STREAM_DRAW);
    }
    glGenSamplers(1, &hostile_sampler);
    glSamplerParameteri(hostile_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(hostile_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    uint8_t cov[7 * 5], rgba[6 * 4 * 4], px[20 * 9 * 2];
    noise(cov, sizeof(cov)), noise(rgba, sizeof(rgba)), noise(px, sizeof(px));
    shr_surface src565 = packed(px, SHR_FORMAT_RGB565, 20, 9);
    shr_draw_cmd c[] = {
        {.kind = SHR_CMD_FILL, .dst = {0, 0, 24, 2}, .color = SHR_RGB(10, 20, 30)},
        whole(SHR_CMD_GLYPH, (shr_rect){1, 3, 8, 8}, mem(cov, SHR_FORMAT_A8, 7, 5, 7), (shr_point){0, 0}, SHR_RGB(250, 200, 100)),
        whole(SHR_CMD_IMAGE, (shr_rect){9, 3, 15, 7}, mem(rgba, SHR_FORMAT_RGBA8888, 6, 4, 24), (shr_point){0, 0}, 0),
        {.kind = SHR_CMD_COPY, .dst = {16, 2, 24, 10}, .src = as_image(&src565), .src_origin = {3, 1}},
    };
    pre_execute = hostile_state;
    EACH_TARGET(f, dev) ASSERT(compare(c, 4, f, 24, 12, dev, K_IMAGE) <= MAX_BLEND(f));
    pre_execute = NULL;
    glDeleteProgram(hostile_prog);
    glDeleteVertexArrays(1, &hostile_vao);
    glDeleteBuffers(2, hostile_bufs);
    glDeleteSamplers(1, &hostile_sampler);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    PASS();
}

TEST allocation_failures(void) {
    fail_alloc f = {-1, 0};
    shr_allocator a = fail_allocator(&f);
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(&a, 1u << 20, 0, 0, IDS, &d), SHR_OK);
    shr_surface s;
    f.budget = 0;
    ASSERT_EQ_LL(shr_angle_surface_create(&d, 8, 8, SHR_FORMAT_RGB565, &s), SHR_E_NO_MEMORY);
    f.budget = 1;
    ASSERT_EQ_LL(shr_angle_surface_create(&d, 8, 8, SHR_FORMAT_RGB565, &s), SHR_OK);
    uint16_t px[64];
    f.budget = 0; /* staging for the read */
    ASSERT_EQ_LL(shr_angle_surface_read(&d, &s, px, 16), SHR_E_NO_MEMORY);

    /* Staging for a CPU destination: nothing is drawn. */
    uint16_t cpu[64], before[64];
    noise(cpu, sizeof(cpu));
    memcpy(before, cpu, sizeof(cpu));
    shr_surface cs = packed(cpu, SHR_FORMAT_RGB565, 8, 8);
    shr_draw_cmd fill = {.kind = SHR_CMD_FILL, .dst = {0, 0, 8, 8}, .color = SHR_RGB(9, 9, 9)};
    ASSERT_EQ_LL(d.execute(d.user, &cs, &fill, 1, 0), SHR_E_NO_MEMORY);
    ASSERT_EQ_LL(memcmp(cpu, before, sizeof(cpu)), 0);

    /* More draws than instances fit: the larger array fails before anything is drawn. */
    enum { N = 1100 };
    shr_draw_cmd *many = calloc(N, sizeof(*many));
    for (int i = 0; i < N; i++) many[i] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {i % 8, 0, i % 8 + 1, 1}};
    f.budget = 0;
    ASSERT_EQ_LL(d.execute(d.user, &s, many, N, 0), SHR_E_NO_MEMORY);
    f.budget = -1;
    ASSERT_EQ_LL(compare_with(&d, many, N, SHR_FORMAT_RGB565, 8, 8, true, K_FILL), 0);
    free(many);

    /* The texture of a REGISTER: the id names nothing, and registers once memory is back. */
    uint8_t cov[6 * 5];
    noise(cov, sizeof(cov));
    shr_draw_cmd g = whole(SHR_CMD_GLYPH, (shr_rect){1, 1, 7, 6}, mem(cov, SHR_FORMAT_A8, 6, 5, 6), (shr_point){0, 0},
                           SHR_RGB(200, 100, 50));
    shr_draw_cmd r[2] = {reg(g.buffer), g};
    f.budget = 0;
    ASSERT_EQ_LL(d.execute(d.user, &cs, r, 2, 0), SHR_E_NO_MEMORY);
    ASSERT_EQ_LL(memcmp(cpu, before, sizeof(cpu)), 0);
    ASSERT_EQ_LL(d.execute(d.user, &cs, &g, 1, 0), SHR_E_INVALID_ARG);
    f.budget = -1;
    ASSERT(compare_with(&d, &g, 1, SHR_FORMAT_RGB565, 8, 8, false, K_GLYPH) <= MAX_BLEND(SHR_FORMAT_RGB565));
    ASSERT_EQ_LL(shr_angle_surface_destroy(&d, &s), SHR_OK);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    PASS();
}

/* ---- commands against the software port ---- */

TEST fill_matches(void) {
    const int32_t w = 37, h = 23;
    shr_draw_cmd c[] = {
        {.kind = SHR_CMD_FILL, .dst = {0, 0, w, h}, .color = SHR_RGB(10, 200, 30)},
        {.kind = SHR_CMD_FILL, .dst = {w - 1, 0, w, h}, .color = SHR_RGB(255, 255, 255)},
        {.kind = SHR_CMD_FILL, .dst = {0, h - 1, w, h}, .color = SHR_RGB(1, 2, 3)},
        {.kind = SHR_CMD_FILL, .dst = {3, 5, 4, 6}, .color = SHR_RGB(250, 0, 125)},
        {.kind = SHR_CMD_FILL, .dst = {5, 5, 5, 9}, .color = SHR_RGB(0, 0, 0)},
        {.kind = SHR_CMD_FILL, .dst = {7, 3, 30, 4}, .color = 0x00123456},
    };
    shr_draw_cmd dim[] = {
        {.kind = SHR_CMD_FILL, .flags = SHR_GLYPH_DIM, .dst = {0, 0, w, h}, .color = SHR_RGB(10, 200, 30)},
        {.kind = SHR_CMD_FILL, .flags = SHR_GLYPH_DIM, .dst = {w - 5, h - 4, w, h}, .color = SHR_RGB(255, 0, 255)},
        {.kind = SHR_CMD_FILL, .flags = SHR_GLYPH_DIM, .dst = {2, 2, 9, 3}, .color = SHR_RGB(0, 255, 0)},
    };
    EACH_TARGET(f, dev) {
        ASSERT_EQ_LL(compare(c, 6, f, w, h, dev, K_FILL), 0);
        ASSERT(compare(dim, 3, f, w, h, dev, K_DIM) <= MAX_BLEND(f));
        ASSERT(compare(&c[3], 1, f, w, h, dev, K_FILL) == 0); /* one pixel: a small uploaded area */
    }
    PASS();
}

TEST many_quads_take_few_draws(void) {
    enum { N = 2500, M = 65536 + 100 };
    shr_draw_cmd *c = calloc(M, sizeof(*c));
    for (int i = 0; i < N; i++)
        c[i] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {i % 61, i / 61, i % 61 + 1, i / 61 + 1}, .color = rnd_color()};
    EACH_TARGET(f, dev) {
        ASSERT_EQ_LL(compare(c, N, f, 61, 41, dev, K_FILL), 0);
        ASSERT_EQ_LL(last_draws, 1);
    }
    /* More quads than one draw holds. */
    for (int i = 0; i < M; i++)
        c[i] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {i % 256, i / 256, i % 256 + 1, i / 256 + 1}, .color = rnd_color()};
    ASSERT_EQ_LL(compare(c, M, SHR_FORMAT_RGBX8888, 256, 257, true, K_FILL), 0);
    ASSERT_EQ_LL(last_draws, 2);
    free(c);
    PASS();
}

TEST glyphs_match(void) {
    const int32_t w = 29, h = 19;
    uint8_t a8[9 * 7], a4[4 * 6], a4wide[8 * 3];
    noise(a8, sizeof(a8)), noise(a4, sizeof(a4)), noise(a4wide, sizeof(a4wide));
    a8[0] = 0, a8[1] = 255, a8[2] = 1, a8[3] = 128, a8[4] = 129, a4[0] = 0x0F, a4[1] = 0xF1;
    shr_image g8 = mem(a8, SHR_FORMAT_A8, 9, 7, 9);
    shr_image g4 = mem(a4, SHR_FORMAT_A4, 7, 6, 4); /* odd width, padded rows */
    shr_image g4w = mem(a4wide, SHR_FORMAT_A4, 15, 3, 8);
    const shr_point o = {0, 0};
    shr_draw_cmd c[] = {
        whole(SHR_CMD_GLYPH, (shr_rect){0, 0, 9, 7}, g8, o, SHR_RGB(255, 255, 255)),
        whole(SHR_CMD_GLYPH, (shr_rect){10, 1, 19, 8}, g8, o, SHR_RGB(250, 20, 90)),
        whole(SHR_CMD_GLYPH, (shr_rect){w - 4, h - 3, w, h}, g8, (shr_point){5, 4}, SHR_RGB(0, 0, 255)),
        whole(SHR_CMD_GLYPH, (shr_rect){0, 9, 7, 15}, g4, o, SHR_RGB(30, 255, 60)),
        whole(SHR_CMD_GLYPH, (shr_rect){8, 9, 13, 13}, g4, (shr_point){1, 1}, SHR_RGB(200, 100, 0)),
        whole(SHR_CMD_GLYPH, (shr_rect){14, 10, 20, 15}, g4, (shr_point){1, 0}, SHR_RGB(9, 99, 199)),
        whole(SHR_CMD_GLYPH, (shr_rect){14, 16, 29, 19}, g4w, o, SHR_RGB(128, 128, 128)),
        whole(SHR_CMD_GLYPH, (shr_rect){3, 3, 3, 5}, g8, o, SHR_RGB(1, 1, 1)),
    };
    c[1].flags = c[5].flags = SHR_GLYPH_DIM;
    EACH_TARGET(f, dev) ASSERT(compare(c, 8, f, w, h, dev, K_GLYPH) <= MAX_BLEND(f));
    PASS();
}

/* A4 from R8 bytes: rects at even and odd columns of one atlas, odd widths, nibbles of neighbours either side. */
TEST a4_nibbles_from_r8_bytes(void) {
    enum { AW = 23, AH = 9, STRIDE = 13 };
    uint8_t atlas[AH * STRIDE];
    noise(atlas, sizeof(atlas));
    uint32_t id = use(mem(atlas, SHR_FORMAT_A4, AW, AH, STRIDE));
    static const shr_rect rects[] = {{0, 0, 23, 9}, {2, 1, 7, 8}, {4, 0, 5, 9}, {6, 3, 23, 6}, {22, 0, 23, 9}, {10, 2, 10, 2}};
    shr_draw_cmd c[1 + 2 * 6] = {{.kind = SHR_CMD_FILL, .dst = {0, 0, 60, 30}, .color = 0}};
    size_t n = 1;
    for (int i = 0; i < 6; i++) {
        shr_rect r = rects[i];
        int32_t x = (i % 3) * 20 + 1, y = (i / 3) * 12 + 1, w = r.x1 - r.x0, h = r.y1 - r.y0;
        c[n++] = from(SHR_CMD_GLYPH, (shr_rect){x, y, x + w, y + h}, id, r, (shr_point){0, 0}, SHR_RGB(255, 255, 255));
        if (w > 1) /* odd origins inside the rect */
            c[n++] = from(SHR_CMD_GLYPH, (shr_rect){x, y + h, x + w - 1, y + h + 1}, id, r, (shr_point){1, h - 1}, SHR_RGB(255, 255, 255));
    }
    for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare(c, n, SHR_FORMAT_RGBX8888, 60, 30, dev, K_GLYPH), 0);
    for (size_t i = 1; i < n; i++) c[i].color = rnd_color(), c[i].flags = i % 2 ? SHR_GLYPH_DIM : 0;
    EACH_TARGET(f, dev) ASSERT(compare(c, n, f, 60, 30, dev, K_GLYPH) <= MAX_BLEND(f));
    PASS();
}

/* Coverage bytes with runs of 0 and 255, so the BOLD gap rule meets every case. */
static void coverage_noise(uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t v = rnd();
        p[i] = (uint8_t)(v % 4 == 0 ? 0 : v % 4 == 1 ? 0xFF : v >> 8);
    }
}

/* Every BOLD/ITALIC/DIM mix of `rect` of buffer `id` with an axis above, inside and below it, each over its whole
 * footprint and again clipped with src_origin moved by (1, 1) (negative x when the footprint starts left of -1).
 * `white`: white glyphs after an opaque black FILL, so every RGBX8888 pixel is the coverage itself. */
static size_t styled_batch(shr_draw_cmd *c, uint32_t id, shr_rect rect, bool white, int32_t *w, int32_t *h) {
    const uint32_t B = SHR_GLYPH_BOLD, I = SHR_GLYPH_ITALIC, D = SHR_GLYPH_DIM;
    const uint32_t flags[6] = {B, I, B | I, B | D, I | D, B | I | D};
    int32_t rw = rect.x1 - rect.x0, rh = rect.y1 - rect.y0;
    const int32_t axes[3] = {-30, rh, 2 * rh + 9};
    int32_t colw = 0, x0, x1;
    for (int a = 0; a < 3; a++)
        for (int f = 0; f < 6; f++) {
            shr__glyph_footprint(rw, rh, flags[f], axes[a], &x0, &x1);
            colw = x1 - x0 + 1 > colw ? x1 - x0 + 1 : colw;
        }
    int32_t rowh = rh + 1;
    *w = 6 * colw, *h = 6 * rowh;
    size_t n = 0;
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {0, 0, *w, *h}, .color = white ? 0 : rnd_color()};
    for (int clipped = 0; clipped < 2; clipped++)
        for (int a = 0; a < 3; a++)
            for (int f = 0; f < 6; f++) {
                shr__glyph_footprint(rw, rh, flags[f], axes[a], &x0, &x1);
                int32_t x = f * colw, y = (clipped * 3 + a) * rowh, k = clipped;
                c[n] = from(SHR_CMD_GLYPH, (shr_rect){x + k, y + k, x + x1 - x0 - k, y + rh}, id, rect, (shr_point){x0 + k, k},
                            white ? SHR_RGB(255, 255, 255) : rnd_color());
                c[n].flags = (uint16_t)flags[f], c[n++].slant_axis = axes[a];
            }
    return n;
}

TEST styled_glyphs_match(void) {
    uint8_t a8[9 * 7], a4[5 * 6], a4s[3 * 3];
    coverage_noise(a8, sizeof(a8)), coverage_noise(a4, sizeof(a4)), coverage_noise(a4s, sizeof(a4s));
    const shr_image srcs[3] = {mem(a8, SHR_FORMAT_A8, 9, 7, 9), mem(a4, SHR_FORMAT_A4, 7, 6, 5), /* odd width, noisy padding */
                               mem(a4s, SHR_FORMAT_A4, 5, 3, 3)};
    shr_draw_cmd c[48];
    int32_t w, h;
    for (int i = 0; i < 3; i++) {
        uint32_t id = use(srcs[i]);
        shr_rect all = {0, 0, srcs[i].width, srcs[i].height};
        size_t n = styled_batch(c, id, all, true, &w, &h);
        for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare(c, n, SHR_FORMAT_RGBX8888, w, h, dev, K_SYNTH), 0);
        n = styled_batch(c, id, all, false, &w, &h);
        EACH_TARGET(f, dev) ASSERT(compare(c, n, f, w, h, dev, K_SYNTH) <= MAX_BLEND(f));
    }
    PASS();
}

/* Styled glyphs of rects inside an atlas whose neighbours have coverage, at its edges and inside: the footprint
 * reads 0 outside the rect, never the neighbours. */
TEST styled_glyphs_at_atlas_edges(void) {
    enum { AW = 30, AH = 20 };
    uint8_t a8[AH * AW], a4[AH * (AW / 2)];
    coverage_noise(a8, sizeof(a8)), coverage_noise(a4, sizeof(a4));
    const shr_image atlases[2] = {mem(a8, SHR_FORMAT_A8, AW, AH, AW), mem(a4, SHR_FORMAT_A4, AW, AH, AW / 2)};
    static const shr_rect rects[] = {{0, 0, 6, 5}, {AW - 8, 0, AW, 6}, {0, AH - 4, 5, AH}, {AW - 6, AH - 5, AW, AH},
                                     {10, 6, 17, 13}, {0, 0, AW, AH}};
    shr_draw_cmd c[48];
    int32_t w, h;
    for (int a = 0; a < 2; a++) {
        uint32_t id = use(atlases[a]);
        for (size_t r = 0; r < sizeof(rects) / sizeof(rects[0]); r++) {
            size_t n = styled_batch(c, id, rects[r], true, &w, &h);
            for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare(c, n, SHR_FORMAT_RGBX8888, w, h, dev, K_SYNTH), 0);
            n = styled_batch(c, id, rects[r], false, &w, &h);
            for (int dev = 0; dev < 2; dev++) ASSERT(compare(c, n, SHR_FORMAT_RGB565, w, h, dev, K_SYNTH) <= 1);
        }
    }
    PASS();
}

/* The axis bounds, which the int16 attribute must carry exactly. */
TEST styled_glyphs_match_at_the_axis_bounds(void) {
    uint8_t a8[6 * 5];
    coverage_noise(a8, sizeof(a8));
    uint32_t id = use(mem(a8, SHR_FORMAT_A8, 6, 5, 6));
    for (int white = 0; white < 2; white++) {
        shr_draw_cmd c[5] = {{.kind = SHR_CMD_FILL, .dst = {0, 0, 40, 5}, .color = white ? 0 : rnd_color()}};
        for (int i = 0; i < 4; i++) {
            int32_t x0, x1, axis = i / 2 ? 4 * SHR_GLYPH_SYNTH_MAX : -4 * SHR_GLYPH_SYNTH_MAX;
            uint32_t flags = SHR_GLYPH_ITALIC | (i % 2 ? SHR_GLYPH_BOLD : 0);
            shr__glyph_footprint(6, 5, flags, axis, &x0, &x1);
            c[1 + i] = from(SHR_CMD_GLYPH, (shr_rect){10 * i, 0, 10 * i + x1 - x0, 5}, id, (shr_rect){0, 0, 6, 5},
                            (shr_point){x0, 0}, white ? SHR_RGB(255, 255, 255) : rnd_color());
            c[1 + i].flags = (uint16_t)flags, c[1 + i].slant_axis = axis;
        }
        if (white)
            for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare(c, 5, SHR_FORMAT_RGBX8888, 40, 5, dev, K_SYNTH), 0);
        else
            EACH_TARGET(f, dev) ASSERT(compare(c, 5, f, 40, 5, dev, K_SYNTH) <= MAX_BLEND(f));
    }
    PASS();
}

TEST images_match(void) {
    const int32_t w = 33, h = 21;
    enum { IW = 13, IH = 9 };
    uint8_t rgba[IH][IW * 4], odd[IH * (IW * 4 + 3)];
    noise(rgba, sizeof(rgba));
    for (int y = 0; y < IH; y++)
        for (int x = 0; x < IW; x++) {
            static const uint8_t alphas[] = {0, 128, 255};
            if ((x + y) % 2) rgba[y][x * 4 + 3] = alphas[(x + y) % 3];
        }
    for (int y = 0; y < IH; y++) memcpy(odd + y * (IW * 4 + 3), rgba[y], IW * 4);
    shr_image img = mem(rgba, SHR_FORMAT_RGBA8888, IW, IH, IW * 4);
    shr_image unaligned = mem(odd, SHR_FORMAT_RGBA8888, IW, IH, IW * 4 + 3); /* uploaded row by row */
    uint32_t ui = use(unaligned);
    shr_draw_cmd c[] = {
        whole(SHR_CMD_IMAGE, (shr_rect){0, 0, IW, IH}, img, (shr_point){0, 0}, 0),
        whole(SHR_CMD_IMAGE, (shr_rect){w - 6, h - 5, w, h}, img, (shr_point){7, 4}, 0),
        from(SHR_CMD_IMAGE, (shr_rect){14, 0, 14 + IW, IH}, ui, (shr_rect){0, 0, IW, IH}, (shr_point){0, 0}, 0),
        from(SHR_CMD_IMAGE, (shr_rect){0, 10, 5, 13}, ui, (shr_rect){0, 0, IW, IH}, (shr_point){8, 6}, 0),
        from(SHR_CMD_IMAGE, (shr_rect){20, 12, 25, 16}, ui, (shr_rect){3, 2, 11, 8}, (shr_point){2, 1}, 0),
        whole(SHR_CMD_IMAGE, (shr_rect){2, 2, 2 + IW, 2 + IH}, img, (shr_point){0, 0}, 0),
    };
    EACH_TARGET(f, dev) ASSERT(compare(c, 6, f, w, h, dev, K_IMAGE) <= MAX_BLEND(f));
    PASS();
}

static shr_draw_cmd line(shr_rect dst, uint32_t shape, int32_t w, int32_t h, shr_point origin, shr_color color) {
    return (shr_draw_cmd){.kind = SHR_CMD_LINE, .flags = (uint16_t)(shape << SHR_LINE_SHAPE_SHIFT), .dst = dst,
                          .src_origin = origin, .color = color, .src_rect = {0, 0, w, h}};
}

/* Every shape, DIM, phases and band rows cut by `dst`, cells of other sizes in turn (the kept cells change between
 * draws), pieces at the edges. */
TEST lines_match(void) {
    const int32_t w = 71, h = 29;
    static const int32_t cells[][2] = {{8, 3}, {8, 1}, {7, 2}, {13, 5}, {32, 8}, {1, 1}, {8, 3}};
    shr_draw_cmd c[5 * 7 * 2 + 4];
    size_t n = 0;
    for (uint32_t shape = SHR_LINE_SINGLE; shape <= SHR_LINE_DASHED; shape++)
        for (int i = 0; i < 7; i++)
            for (int dim = 0; dim < 2; dim++) {
                int32_t cw = cells[i][0], ch = cells[i][1], oy = ch > 2 && dim, y = (int32_t)(n % 11) * 2;
                int32_t x0 = (int32_t)(rnd() % 9), x1 = w - (int32_t)(rnd() % 9);
                c[n] = line((shr_rect){x0, y, x1, y + ch - oy}, shape, cw, ch, (shr_point){(int32_t)(rnd() % (uint32_t)cw), oy},
                            rnd_color());
                c[n++].flags |= (uint16_t)dim;
            }
    c[n++] = line((shr_rect){w - 1, h - 3, w, h}, SHR_LINE_CURLY, 8, 3, (shr_point){7, 0}, rnd_color());
    c[n++] = line((shr_rect){0, 0, 1, 1}, SHR_LINE_DOTTED, 8, 3, (shr_point){0, 2}, rnd_color());
    c[n++] = line((shr_rect){5, 5, 5, 8}, SHR_LINE_CURLY, 8, 3, (shr_point){0, 0}, rnd_color());
    c[n++] = line((shr_rect){5, 5, 9, 5}, SHR_LINE_DOTTED, 8, 3, (shr_point){0, 0}, rnd_color());
    EACH_TARGET(f, dev) ASSERT(compare(c, n, f, w, h, dev, K_LINE) <= MAX_BLEND(f));
    PASS();
}

/* SCALED IMAGEs: up, down, a yazi piece; whole and clipped; source rects at the buffer's edges and inside it. */
TEST scaled_images_match(void) {
    enum { IW = 23, IH = 17 };
    static uint8_t rgba[IH][IW * 4];
    noise(rgba, sizeof(rgba));
    for (int y = 0; y < IH; y++)
        for (int x = 0; x < IW; x++)
            if ((x * 3 + y) % 4 == 0) rgba[y][x * 4 + 3] = (uint8_t)(x * 11);
    uint32_t id = use(mem(rgba, SHR_FORMAT_RGBA8888, IW, IH, IW * 4));
    static const struct {
        shr_rect src;
        int32_t dw, dh;
        shr_rect dst;
        shr_point at;
    } k[] = {
        {{0, 0, IW, IH}, 41, 30, {0, 0, 41, 30}, {0, 0}},    {{0, 0, IW, IH}, 13, 9, {42, 0, 55, 9}, {42, 0}},
        {{3, 2, 11, 17}, 8, 16, {42, 10, 50, 26}, {42, 10}}, {{5, 4, 20, 15}, 37, 27, {50, 12, 64, 30}, {40, 5}},
        {{0, 0, IW, IH}, 17, 40, {0, 31, 9, 40}, {-8, 0}},   {{1, 0, 22, 16}, 9, 5, {56, 0, 64, 5}, {55, 0}},
    };
    shr_draw_cmd c[6];
    for (int i = 0; i < 6; i++) {
        c[i] = from(SHR_CMD_IMAGE, k[i].dst, id, k[i].src, (shr_point){k[i].dst.x0 - k[i].at.x, k[i].dst.y0 - k[i].at.y}, 0);
        c[i].flags = SHR_IMAGE_SCALED, c[i].scale_w = k[i].dw, c[i].scale_h = k[i].dh;
    }
    EACH_TARGET(f, dev) ASSERT(compare(c, 6, f, 64, 40, dev, K_IMAGE) <= MAX_BLEND(f));
    PASS();
}

TEST copies_convert_exactly(void) {
    const int32_t w = 31, h = 17;
    enum { SW = 20, SH = 12 };
    for (int sf = 0; sf < 2; sf++) {
        size_t len = SW * SH * bpp(FMTS[sf]);
        uint8_t *px = malloc(len + 2);
        noise(px, len + 2);
        shr_surface src = packed(px, FMTS[sf], SW, SH);
        shr_surface mis = packed(px + 1, FMTS[sf], SW, SH); /* odd address: staged for RGB565 */
        shr_surface odd = packed(px, FMTS[sf], SW - 1, SH);
        odd.stride = odd.stride + 1, odd.byte_length = len; /* rows that are no whole pixels: staged */
        shr_surface dev = device_with(FMTS[sf], SW, SH, px);
        shr_draw_cmd c[] = {
            {.kind = SHR_CMD_COPY, .dst = {0, 0, SW, SH}, .src = as_image(&src)},
            {.kind = SHR_CMD_COPY, .dst = {w - 7, h - 5, w, h}, .src = as_image(&src), .src_origin = {13, 7}},
            {.kind = SHR_CMD_COPY, .dst = {21, 0, 31, 4}, .src = as_image(&mis), .src_origin = {3, 2}},
            {.kind = SHR_CMD_COPY, .dst = {21, 5, 29, 8}, .src = as_image(&odd), .src_origin = {2, 1}},
            {.kind = SHR_CMD_COPY, .dst = {0, 13, 9, 17}, .src = as_image(&dev), .src_origin = {11, 8}},
            {.kind = SHR_CMD_COPY, .dst = {10, 13, 19, 17}, .src = as_image(&dev), .src_origin = {1, 0}},
            {.kind = SHR_CMD_COPY, .dst = {5, 5, 5, 6}, .src = as_image(&dev)},
        };
        EACH_TARGET(f, d) ASSERT_EQ_LL(compare(c, 7, f, w, h, d, K_COPY), 0);
        device_drop(&dev);
        free(px);
    }
    /* One row: the stride is never used, even when it is no GL row length. */
    uint8_t row[16 * 4];
    noise(row, sizeof(row));
    shr_draw_cmd one = {.kind = SHR_CMD_COPY, .dst = {2, 1, 18, 2}, .src = {row, 16, 1, 0xFFFFFFFEu, SHR_FORMAT_RGBX8888, 0, 0}};
    EACH_TARGET(f, d) ASSERT_EQ_LL(compare(&one, 1, f, 20, 3, d, K_COPY), 0);
    PASS();
}

TEST copy_scrolls_within_a_surface(void) {
    const int32_t w = 23, h = 15;
    shr_image_ref self = {SELF, w, h, 0, 0, 0, 0};
    shr_draw_cmd up[] = {{.kind = SHR_CMD_COPY, .dst = {0, 0, w, h - 3}, .src = self, .src_origin = {0, 3}},
                         {.kind = SHR_CMD_FILL, .dst = {0, h - 3, w, h}, .color = SHR_RGB(4, 5, 6)}};
    shr_draw_cmd down[] = {{.kind = SHR_CMD_FILL, .dst = {0, 0, w, 2}, .color = SHR_RGB(99, 5, 6)},
                           {.kind = SHR_CMD_COPY, .dst = {2, 4, w, h}, .src = self, .src_origin = {0, 0}}};
    EACH_TARGET(f, dev) {
        up[0].src.format = down[1].src.format = (uint8_t)f;
        up[0].src.domain = down[1].src.domain = dev ? SHR_MEMORY_DEVICE : SHR_MEMORY_CPU;
        ASSERT_EQ_LL(compare(up, 2, f, w, h, dev, K_COPY), 0);
        ASSERT_EQ_LL(compare(down, 2, f, w, h, dev, K_COPY), 0);
    }
    PASS();
}

TEST rotations_map_exactly(void) {
    enum { LW = 13, LH = 7 };
    static const uint8_t rots[3] = {SHR_ROTATE_90_CW, SHR_ROTATE_180, SHR_ROTATE_90_CCW};
    for (int sf = 0; sf < 2; sf++) {
        size_t len = LW * LH * bpp(FMTS[sf]);
        uint8_t *px = malloc(len);
        noise(px, len);
        shr_surface src = packed(px, FMTS[sf], LW, LH);
        shr_surface dev = device_with(FMTS[sf], LW, LH, px);
        for (int r = 0; r < 3; r++) {
            bool quarter = rots[r] != SHR_ROTATE_180;
            int32_t ow = quarter ? LH : LW, oh = quarter ? LW : LH;
            for (int s = 0; s < 2; s++) {
                shr_image_ref m = as_image(s ? &dev : &src);
                shr_draw_cmd whole_rot = {.kind = SHR_CMD_ROTATE, .dst = {0, 0, ow, oh}, .src = m, .rotation = rots[r]};
                /* Parts of a CPU source, rotated away from the output's corner. */
                shr_image_ref sub = m, dot = m;
                sub.pixels = px + (2 * LW + 1) * bpp(FMTS[sf]), sub.width = LW - 3, sub.height = LH - 3;
                dot.pixels = px + (LH * LW - 1) * bpp(FMTS[sf]), dot.width = dot.height = 1;
                shr_draw_cmd part[] = {
                    {.kind = SHR_CMD_ROTATE, .dst = {1, 2, 1 + (quarter ? LH : LW) - 3, 2 + (quarter ? LW : LH) - 3},
                     .src = sub, .rotation = rots[r]},
                    {.kind = SHR_CMD_ROTATE, .dst = {ow - 1, 0, ow, 1}, .src = dot, .rotation = rots[r]},
                };
                EACH_TARGET(f, d) {
                    ASSERT_EQ_LL(compare(&whole_rot, 1, f, ow, oh, d, K_ROTATE), 0);
                    if (!s) ASSERT_EQ_LL(compare(part, 2, f, ow, oh, d, K_ROTATE), 0);
                }
            }
        }
        device_drop(&dev);
        free(px);
    }
    PASS();
}

TEST scratch_textures_grow_in_either_direction(void) {
    shr_framebuffer_driver fresh;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 0, 0, IDS, &fresh), SHR_OK);
    static const shr_rect areas[] = {{0, 0, 20, 2}, {0, 0, 2, 20}, {0, 0, 30, 5}};
    for (int i = 0; i < 3; i++) {
        shr_draw_cmd c = {.kind = SHR_CMD_FILL, .dst = areas[i], .color = rnd_color()};
        ASSERT_EQ_LL(compare_with(&fresh, &c, 1, SHR_FORMAT_RGB565, 32, 24, false, K_FILL), 0);
    }
    ASSERT_EQ_LL(shr_angle_driver_destroy(&fresh), SHR_OK);
    PASS();
}

TEST errors_left_by_the_application_are_not_ours(void) {
    glEnable(0x1234);
    ASSERT(glGetError() != GL_NO_ERROR);
    glEnable(0x1234);
    shr_draw_cmd c = {.kind = SHR_CMD_FILL, .dst = {0, 0, 3, 3}, .color = SHR_RGB(1, 2, 3)};
    EACH_TARGET(f, dev) ASSERT_EQ_LL(compare(&c, 1, f, 3, 3, dev, K_FILL), 0);
    /* Batches drawing nothing touch no GL state. */
    uint16_t px[4];
    shr_surface s = packed(px, SHR_FORMAT_RGB565, 2, 2);
    ASSERT_EQ_LL(gl.execute(gl.user, &s, NULL, 0, 0), SHR_OK);
    c.dst = (shr_rect){1, 1, 1, 2};
    ASSERT_EQ_LL(gl.execute(gl.user, &s, &c, 1, 0), SHR_OK);
    PASS();
}

/* ---- buffers ---- */

/* `rect` of buffer `id` drawn white at column x: whole, dim, and clipped with an offset origin. */
static size_t probe(shr_draw_cmd *c, uint32_t id, shr_rect rect, int32_t x) {
    int32_t w = rect.x1 - rect.x0, h = rect.y1 - rect.y0;
    const shr_color white = SHR_RGB(255, 255, 255);
    c[0] = from(SHR_CMD_GLYPH, (shr_rect){x, 0, x + w, h}, id, rect, (shr_point){0, 0}, white);
    c[1] = from(SHR_CMD_GLYPH, (shr_rect){x, h, x + w, 2 * h}, id, rect, (shr_point){0, 0}, white);
    c[1].flags = SHR_GLYPH_DIM;
    c[2] = from(SHR_CMD_GLYPH, (shr_rect){x, 2 * h, x + w - 1, 3 * h - 2}, id, rect, (shr_point){1, 2}, white);
    return 3;
}

TEST buffers_register_update_replace_release(void) {
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 0, 0, IDS, &d), SHR_OK);
    enum { W = 15, H = 12 };
    uint8_t a8[H * W], old8[H * W], a4[H * 8], old4[H * 8];
    coverage_noise(a8, sizeof(a8)), coverage_noise(a4, sizeof(a4));
    uint32_t i8 = use(mem(a8, SHR_FORMAT_A8, W, H, W)), i4 = use(mem(a4, SHR_FORMAT_A4, W, H, 8));
    shr_draw_cmd c[8] = {{.kind = SHR_CMD_FILL, .dst = {0, 0, 64, 40}, .color = 0}};
    shr_rect all = {0, 0, W, H};
    size_t n = 1 + probe(c + 1, i8, all, 0);
    n += probe(c + n, i4, all, 16);
    for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare_with(&d, c, n, SHR_FORMAT_RGBX8888, 64, 40, dev, K_GLYPH), 0);
    ASSERT_EQ_LL(textures_of(&d), 2);

    /* Written without UPDATE: the driver draws its copy, as the software port draws the old pixels. */
    memcpy(old8, a8, sizeof(a8)), memcpy(old4, a4, sizeof(a4));
    shr_rect r8 = {3, 2, 11, 9}, r4 = {3, 1, 11, 7}; /* odd A4 columns: their bytes are shared with neighbours */
    for (int32_t y = r8.y0; y < r8.y1; y++)
        for (int32_t x = r8.x0; x < r8.x1; x++) a8[y * W + x] = (uint8_t)rnd();
    for (int32_t y = r4.y0; y < r4.y1; y++)
        for (int32_t x = r4.x0; x < r4.x1; x++) {
            uint8_t *b = &a4[y * 8 + x / 2], v = (uint8_t)(rnd() & 15);
            *b = (uint8_t)(x % 2 ? (*b & 0xF0) | v : (*b & 0x0F) | v << 4);
        }
    auto_register = false;
    bufs[i8 - 1].pixels = old8, bufs[i4 - 1].pixels = old4;
    for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare_with(&d, c, n, SHR_FORMAT_RGBX8888, 64, 40, dev, K_GLYPH), 0);
    /* UPDATE the written rects (and an empty one): the new pixels. */
    bufs[i8 - 1].pixels = a8, bufs[i4 - 1].pixels = a4;
    shr_draw_cmd u[16] = {buf_cmd(SHR_CMD_BUFFER_UPDATE, i8, r8), buf_cmd(SHR_CMD_BUFFER_UPDATE, i4, r4),
                          buf_cmd(SHR_CMD_BUFFER_UPDATE, i4, (shr_rect){5, 5, 5, 5})};
    memcpy(u + 3, c, n * sizeof(c[0]));
    for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare_with(&d, u, n + 3, SHR_FORMAT_RGBX8888, 64, 40, dev, K_GLYPH), 0);

    /* REGISTER again: another memory of the same shape keeps the layer, another shape takes a new texture. */
    uint8_t b8[H * W];
    coverage_noise(b8, sizeof(b8));
    bufs[i8 - 1] = mem(b8, SHR_FORMAT_A8, W, H, W);
    u[0] = reg(i8);
    memcpy(u + 1, c, n * sizeof(c[0]));
    for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare_with(&d, u, n + 1, SHR_FORMAT_RGBX8888, 64, 40, dev, K_GLYPH), 0);
    ASSERT_EQ_LL(textures_of(&d), 2);
    bufs[i8 - 1] = mem(b8, SHR_FORMAT_A8, W - 2, H - 1, W);
    u[0] = reg(i8), u[1] = c[0];
    n = 1 + probe(u + 2, i8, (shr_rect){0, 0, W - 2, H - 1}, 0);
    for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare_with(&d, u, n + 1, SHR_FORMAT_RGBX8888, 64, 40, dev, K_GLYPH), 0);
    ASSERT_EQ_LL(textures_of(&d), 2); /* the old texture went with its last layer */
    uint8_t rgba[(H - 1) * (W - 2) * 4];
    noise(rgba, sizeof(rgba));
    const shr_image shapes[3] = {mem(b8, SHR_FORMAT_A8, W - 2, H - 2, W), mem(rgba, SHR_FORMAT_RGBA8888, W - 2, H - 1, (W - 2) * 4),
                                 mem(b8, SHR_FORMAT_A8, W - 2, 0, W)};
    for (int k = 0; k < 3; k++) { /* another height, format, no rows */
        bufs[i8 - 1] = shapes[k];
        u[0] = reg(i8), u[1] = c[0];
        shr_rect rr = {0, 0, shapes[k].width, shapes[k].height};
        u[2] = from(k == 1 ? SHR_CMD_IMAGE : SHR_CMD_GLYPH, (shr_rect){0, 0, rr.x1, rr.y1}, i8, rr, (shr_point){0, 0}, SHR_RGB(255, 255, 255));
        for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare_with(&d, u, 3, SHR_FORMAT_RGBX8888, 64, 40, dev, K_GLYPH), 0);
        ASSERT_EQ_LL(textures_of(&d), k < 2 ? 2 : 1);
    }

    /* RELEASE: drawing from the id fails; ids that name nothing, 0 and those beyond the table are no-ops. */
    uint8_t px[4 * 4 * 4];
    shr_surface s = packed(px, SHR_FORMAT_RGBX8888, 4, 4);
    shr_draw_cmd rel[] = {buf_cmd(SHR_CMD_BUFFER_RELEASE, i8, (shr_rect){0}), buf_cmd(SHR_CMD_BUFFER_RELEASE, i8, (shr_rect){0}),
                          buf_cmd(SHR_CMD_BUFFER_RELEASE, 0, (shr_rect){0}), buf_cmd(SHR_CMD_BUFFER_RELEASE, IDS + 1, (shr_rect){0})};
    ASSERT_EQ_LL(d.execute(d.user, &s, rel, 4, 0), SHR_OK);
    ASSERT_EQ_LL(textures_of(&d), 1);
    shr_draw_cmd g = from(SHR_CMD_GLYPH, (shr_rect){0, 0, 2, 2}, i8, (shr_rect){0, 0, 2, 2}, (shr_point){0, 0}, 0);
    ASSERT_EQ_LL(d.execute(d.user, &s, &g, 1, 0), SHR_E_INVALID_ARG);
    shr_draw_cmd up = buf_cmd(SHR_CMD_BUFFER_UPDATE, i8, (shr_rect){0, 0, 1, 1});
    ASSERT_EQ_LL(d.execute(d.user, &s, &up, 1, 0), SHR_E_INVALID_ARG);
    up = buf_cmd(SHR_CMD_BUFFER_UPDATE, i4, (shr_rect){0, 0, W + 1, 1});
    ASSERT_EQ_LL(d.execute(d.user, &s, &up, 1, 0), SHR_E_INVALID_ARG);

    /* A buffer without pixels: registered, updated (nothing) and drawn from (nothing, also styled). */
    shr_draw_cmd none[] = {{.kind = SHR_CMD_BUFFER_REGISTER, .buffer = 7, .src = {NULL, 0, 4, 0, SHR_FORMAT_A8, 0, 0}},
                           buf_cmd(SHR_CMD_BUFFER_UPDATE, 7, (shr_rect){0, 0, 0, 4}),
                           from(SHR_CMD_GLYPH, (shr_rect){0, 0, 1, 4}, 7, (shr_rect){0, 0, 0, 4}, (shr_point){0, 0}, SHR_RGB(255, 0, 0))};
    none[2].flags = SHR_GLYPH_BOLD;
    memset(px, 0x5A, sizeof(px));
    ASSERT_EQ_LL(d.execute(d.user, &s, none, 3, 0), SHR_OK);
    for (size_t i = 0; i < sizeof(px); i++) ASSERT_EQ_LL(px[i], 0x5A);
    ASSERT_EQ_LL(textures_of(&d), 1);
    auto_register = true;
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
    PASS();
}

/* Same-shape buffers share array textures: the first one alone, then 16 layers; freed layers are taken again. */
TEST texture_arrays_are_shared_and_reused(void) {
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 0, 0, IDS, &d), SHR_OK);
    enum { W = 8, H = 16, N = 20 };
    static uint8_t px[N][H * W / 2];
    for (int i = 0; i < N; i++) coverage_noise(px[i], sizeof(px[i]));
    shr_draw_cmd c[2 * N + 1];
    size_t n = 0;
    for (uint32_t id = 1; id <= N; id++) {
        bufs[id - 1] = mem(px[id - 1], SHR_FORMAT_A4, W, H, W / 2);
        c[n++] = reg(id);
    }
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {0, 0, N * W, H}, .color = 0};
    for (uint32_t id = 1; id <= N; id++)
        c[n++] = from(SHR_CMD_GLYPH, (shr_rect){(int32_t)(id - 1) * W, 0, (int32_t)id * W, H}, id, (shr_rect){0, 0, W, H},
                      (shr_point){0, 0}, SHR_RGB(255, 255, 255));
    shr_angle_stats st;
    for (int dev = 0; dev < 2; dev++) {
        ASSERT_EQ_LL(compare_with(&d, c, n, SHR_FORMAT_RGBX8888, N * W, H, dev, K_GLYPH), 0);
        ASSERT_EQ_LL(last_draws, 1);
    }
    ASSERT_EQ_LL(shr_angle_driver_stats(&d, &st), SHR_OK);
    ASSERT_EQ_LL(st.textures, 3); /* 1 + 16 + 3 of 16 */
    ASSERT_EQ_LL(st.texture_bytes, 33 * (W / 2 * H));
    /* Released layers are reused; a texture goes with its last layer. */
    uint8_t out[4 * 4 * 4];
    shr_surface s = packed(out, SHR_FORMAT_RGBX8888, 4, 4);
    shr_draw_cmd rel[N];
    for (uint32_t id = 2; id <= 6; id++) rel[id - 2] = buf_cmd(SHR_CMD_BUFFER_RELEASE, id, (shr_rect){0});
    ASSERT_EQ_LL(d.execute(d.user, &s, rel, 5, 0), SHR_OK);
    for (uint32_t id = 2; id <= 6; id++) rel[id - 2] = reg(id);
    ASSERT_EQ_LL(d.execute(d.user, &s, rel, 5, 0), SHR_OK);
    ASSERT_EQ_LL(textures_of(&d), 3);
    for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare_with(&d, c + N, n - N, SHR_FORMAT_RGBX8888, N * W, H, dev, K_GLYPH), 0);
    for (uint32_t id = 1; id <= N; id++) rel[id - 1] = buf_cmd(SHR_CMD_BUFFER_RELEASE, id, (shr_rect){0});
    ASSERT_EQ_LL(d.execute(d.user, &s, rel, 1, 0), SHR_OK);
    ASSERT_EQ_LL(textures_of(&d), 2);
    ASSERT_EQ_LL(d.execute(d.user, &s, rel + 1, N - 1, 0), SHR_OK);
    ASSERT_EQ_LL(shr_angle_driver_stats(&d, &st), SHR_OK);
    ASSERT(st.textures == 0 && st.texture_bytes == 0);

    /* Large shapes share textures of fewer layers, at least one. */
    enum { BW = 1024, GW = 2100 };
    uint8_t *big = calloc(BW * BW, 3), *huge = calloc(GW * GW, 2);
    for (uint32_t id = 1; id <= 5; id++) {
        bufs[id - 1] = id <= 3 ? mem(big + (id - 1) * BW * BW, SHR_FORMAT_A8, BW, BW, BW)
                               : mem(huge + (id - 4) * GW * GW, SHR_FORMAT_A8, GW, GW, GW);
        c[id - 1] = reg(id);
    }
    ASSERT_EQ_LL(d.execute(d.user, &s, c, 5, 0), SHR_OK);
    ASSERT_EQ_LL(shr_angle_driver_stats(&d, &st), SHR_OK);
    ASSERT_EQ_LL(st.textures, 4); /* 1 + 4 layers, 1 + 1 */
    ASSERT_EQ_LL(st.texture_bytes, 5 * BW * BW + 2 * GW * GW);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
    free(big), free(huge);
    PASS();
}

/* A ninth texture in one batch draws what is pending first. */
TEST more_than_eight_textures_flush(void) {
    enum { N = 10 };
    static uint8_t px[N][20 * 12];
    shr_draw_cmd c[1 + 2 * N] = {{.kind = SHR_CMD_FILL, .dst = {0, 0, 100, 40}, .color = 0}};
    uint32_t ids[N];
    for (int i = 0; i < N; i++) {
        coverage_noise(px[i], sizeof(px[i]));
        ids[i] = use(mem(px[i], SHR_FORMAT_A8, 11 + i, 12, 20)); /* a shape each */
    }
    for (int i = 0; i < 2 * N; i++) {
        int k = i % N, x = (i % 10) * 10, y = (i / 10) * 13;
        c[1 + i] = from(SHR_CMD_GLYPH, (shr_rect){x, y, x + 10, y + 12}, ids[k], (shr_rect){0, 0, 11 + k, 12},
                        (shr_point){i / N, 0}, SHR_RGB(255, 255, 255));
    }
    for (int dev = 0; dev < 2; dev++) {
        ASSERT_EQ_LL(compare(c, 1 + 2 * N, SHR_FORMAT_RGBX8888, 100, 40, dev, K_GLYPH), 0);
        ASSERT_EQ_LL(last_draws, 3); /* 8 textures, 8 more, 4 */
    }
    for (int i = 1; i <= 2 * N; i++) c[i].color = rnd_color();
    EACH_TARGET(f, dev) ASSERT(compare(c, 1 + 2 * N, f, 100, 40, dev, K_GLYPH) <= MAX_BLEND(f));
    PASS();
}

/* The driver obeys REGISTER and RELEASE within its budget; the compositor decides what to evict. */
TEST budget_limits_registration(void) {
    enum { B = 3 * 16 * 16 };
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, B, 0, 0, IDS, &d), SHR_OK);
    ASSERT_EQ_LL(d.caps.buffer_bytes, B);
    static uint8_t px[5][16 * 16];
    for (int i = 0; i < 5; i++) noise(px[i], sizeof(px[i]));
    uint8_t out[4 * 4 * 4];
    shr_surface s = packed(out, SHR_FORMAT_RGBX8888, 4, 4);
    for (uint32_t id = 1; id <= 4; id++) bufs[id - 1] = mem(px[id - 1], SHR_FORMAT_A8, 16, 16, 16);
    shr_draw_cmd c[4] = {reg(1), reg(2), reg(3), reg(4)};
    ASSERT_EQ_LL(d.execute(d.user, &s, c, 4, 0), SHR_E_UNSUPPORTED);
    shr_draw_cmd evict[2] = {buf_cmd(SHR_CMD_BUFFER_RELEASE, 2, (shr_rect){0}), reg(4)};
    ASSERT_EQ_LL(d.execute(d.user, &s, evict, 2, 0), SHR_OK);
    /* Replacing counts the old size out first; growing past the budget fails. */
    ASSERT_EQ_LL(d.execute(d.user, &s, &c[2], 1, 0), SHR_OK);
    bufs[2] = mem(px[2], SHR_FORMAT_A8, 16, 17, 16);
    shr_draw_cmd grow = reg(3);
    ASSERT_EQ_LL(d.execute(d.user, &s, &grow, 1, 0), SHR_E_UNSUPPORTED);
    bufs[2] = mem(px[2], SHR_FORMAT_A8, 16, 16, 16);
    shr_draw_cmd g[] = {from(SHR_CMD_GLYPH, (shr_rect){0, 0, 16, 16}, 1, (shr_rect){0, 0, 16, 16}, (shr_point){0, 0}, SHR_RGB(255, 255, 255)),
                        from(SHR_CMD_GLYPH, (shr_rect){8, 8, 24, 24}, 4, (shr_rect){0, 0, 16, 16}, (shr_point){0, 0}, SHR_RGB(0, 255, 255)),
                        from(SHR_CMD_GLYPH, (shr_rect){16, 0, 32, 16}, 3, (shr_rect){0, 0, 16, 16}, (shr_point){0, 0}, SHR_RGB(255, 0, 255))};
    auto_register = false;
    EACH_TARGET(f, dev) ASSERT(compare_with(&d, g, 3, f, 32, 24, dev, K_GLYPH) <= MAX_BLEND(f));
    auto_register = true;
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);

    /* Unlimited budget; buffers beyond the texture size or outside the domains. */
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 0, 0, IDS, &d), SHR_OK);
    ASSERT_EQ_LL(d.execute(d.user, &s, c, 4, 0), SHR_OK);
    int32_t wide = d.caps.max_buffer_width + 1;
    uint8_t *row = calloc((size_t)wide, 1);
    shr_draw_cmd big = {.kind = SHR_CMD_BUFFER_REGISTER, .buffer = 5, .src = img_ref(mem(row, SHR_FORMAT_A8, wide, 1, (size_t)wide))};
    ASSERT_EQ_LL(d.execute(d.user, &s, &big, 1, 0), SHR_E_UNSUPPORTED);
    big.src = img_ref(mem(row, SHR_FORMAT_A8, 1, wide, 1));
    ASSERT_EQ_LL(d.execute(d.user, &s, &big, 1, 0), SHR_E_UNSUPPORTED);
    big.src = img_ref(mem(row, SHR_FORMAT_A8, 16, 1, 16)), big.src.domain = SHR_MEMORY_DMA;
    ASSERT_EQ_LL(d.execute(d.user, &s, &big, 1, 0), SHR_E_UNSUPPORTED);
    /* One row of a stride no GL row length holds. */
    big.src = (shr_image_ref){row, 16, 1, 0xFFFFFFFFu, SHR_FORMAT_A8, 0, 0};
    ASSERT_EQ_LL(shr_image_ref_get(&big.src, &bufs[4]), SHR_OK);
    ASSERT_EQ_LL(d.execute(d.user, &s, &big, 1, 0), SHR_OK);
    shr_draw_cmd one = from(SHR_CMD_GLYPH, (shr_rect){0, 0, 16, 1}, 5, (shr_rect){0, 0, 16, 1}, (shr_point){0, 0}, SHR_RGB(255, 255, 255));
    auto_register = false;
    for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare_with(&d, &one, 1, SHR_FORMAT_RGBX8888, 16, 2, dev, K_GLYPH), 0);
    auto_register = true;
    free(row);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
    PASS();
}

/* After an error the compositor registers again; that works whatever the error left registered. */
TEST errors_then_registering_again(void) {
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 0, 0, IDS, &d), SHR_OK);
    uint8_t cov[9 * 6], px[16 * 8 * 2], before[sizeof(px)];
    coverage_noise(cov, sizeof(cov));
    noise(px, sizeof(px));
    memcpy(before, px, sizeof(px));
    shr_surface s = packed(px, SHR_FORMAT_RGB565, 16, 8);
    uint32_t id = use(mem(cov, SHR_FORMAT_A8, 9, 6, 9));
    shr_draw_cmd good = from(SHR_CMD_GLYPH, (shr_rect){1, 1, 10, 7}, id, (shr_rect){0, 0, 9, 6}, (shr_point){0, 0}, SHR_RGB(255, 99, 0));
    shr_draw_cmd bad = good;
    bad.src_rect.x1 = 10;
    shr_draw_cmd fill = {.kind = SHR_CMD_FILL, .dst = {0, 0, 4, 4}};
    shr_image rgb = mem(px, SHR_FORMAT_RGB565, 4, 4, 32), short_stride = mem(cov, SHR_FORMAT_A8, 9, 6, 8);
    const shr_draw_cmd batches[][2] = {
        {reg(id), bad},
        {{.kind = SHR_CMD_BUFFER_REGISTER, .buffer = 0, .src = img_ref(bufs[id - 1])}, good},
        {{.kind = SHR_CMD_BUFFER_REGISTER, .buffer = IDS + 1, .src = img_ref(bufs[id - 1])}, good},
        {{.kind = SHR_CMD_BUFFER_REGISTER, .buffer = id, .src = img_ref(short_stride)}, good},
        {{.kind = SHR_CMD_BUFFER_REGISTER, .buffer = id, .src = img_ref(rgb)}, fill}, /* no SHR_DRIVER_IMAGE_565 */
        {fill, reg(id)},
        {buf_cmd(SHR_CMD_BUFFER_UPDATE, IDS - 1, (shr_rect){0, 0, 1, 1}), good},
    };
    const shr_status want[] = {SHR_E_INVALID_ARG, SHR_E_INVALID_ARG, SHR_E_INVALID_ARG, SHR_E_INVALID_ARG,
                               SHR_E_UNSUPPORTED, SHR_E_INVALID_ARG, SHR_E_INVALID_ARG};
    for (size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        ASSERT_EQ_LL(d.execute(d.user, &s, batches[k], 2, 0), want[k]);
        ASSERT_EQ_LL(memcmp(px, before, sizeof(px)), 0);
        EACH_TARGET(f, dev) ASSERT(compare_with(&d, &good, 1, f, 16, 8, dev, K_GLYPH) <= MAX_BLEND(f));
    }
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
    PASS();
}

/* A full screen of glyphs from two atlases is one draw. */
TEST full_screen_text_is_a_few_draws(void) {
    enum { W = 1024, H = 768, CW = 8, CH = 16 };
    static uint8_t a4[256 * 128], a8[128 * 128];
    coverage_noise(a4, sizeof(a4)), coverage_noise(a8, sizeof(a8));
    uint32_t i4 = use(mem(a4, SHR_FORMAT_A4, 512, 128, 256)), i8 = use(mem(a8, SHR_FORMAT_A8, 128, 128, 128));
    size_t n = 0, cells = (W / CW) * (H / CH);
    shr_draw_cmd *c = calloc(2 * cells + H / CH, sizeof(*c));
    for (int32_t y = 0; y < H / CH; y++) {
        c[n++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {0, y * CH, W, y * CH + CH}, .color = rnd_color()};
        for (int32_t x = 0; x < W / CW; x++) {
            uint32_t g = rnd(), id = g % 5 ? i4 : i8;
            int32_t cols = id == i4 ? 64 : 16, gx = (int32_t)(g >> 8) % cols * CW, gy = (int32_t)(g >> 16) % 8 * CH;
            shr_rect dst = {x * CW, y * CH, x * CW + CW, y * CH + CH};
            c[n++] = from(SHR_CMD_GLYPH, dst, id, (shr_rect){gx, gy, gx + CW, gy + CH}, (shr_point){0, 0}, rnd_color());
            if (g % 7 == 0) c[n - 1].flags = SHR_GLYPH_BOLD;
        }
    }
    EACH_TARGET(f, dev) {
        ASSERT(compare(c, n, f, W, H, dev, K_GLYPH) <= MAX_BLEND(f));
        ASSERT_EQ_LL(last_draws, 1);
    }
    free(c);
    PASS();
}

/* ---- validation: the software driver's statuses ---- */

static shr_framebuffer_driver swd;

static void same_status(const shr_surface *dst, const shr_draw_cmd *c, size_t n, shr_status expected) {
    size_t len = dst->byte_length, m = n;
    uint8_t *before = malloc(len ? len : 1);
    memcpy(before, dst->pixels, len);
    shr_draw_cmd *all = c ? with_prologue(c, &m) : NULL;
    ASSERT_EQ_LL(swd.execute(swd.user, dst, all, m, 0), expected);
    ASSERT_EQ_LL(gl.execute(gl.user, dst, all, m, 0), expected);
    ASSERT_EQ_LL(memcmp(before, dst->pixels, len), 0);
    free(all), free(before);
}

static void same_one(const shr_surface *dst, shr_draw_cmd c, shr_status expected) { same_status(dst, &c, 1, expected); }

TEST invalid_batches_match_the_software_port(void) {
    enum { W = 16, H = 8 };
    static uint16_t px[W * H], other[W * H];
    shr_surface d = packed(px, SHR_FORMAT_RGB565, W, H);
    shr_surface o = packed(other, SHR_FORMAT_RGB565, W, H);
    uint8_t cov[16], rgba[16];
    shr_image a8 = mem(cov, SHR_FORMAT_A8, 4, 4, 4), img = mem(rgba, SHR_FORMAT_RGBA8888, 2, 2, 8);
    shr_image_ref src = as_image(&o), self = as_image(&d);
    shr_rect r = {0, 0, 4, 4};
    const shr_point o0 = {0, 0};
    const shr_draw_cmd fill = {.kind = SHR_CMD_FILL, .dst = r}, end = {.kind = SHR_CMD_KEEP_END};
    const shr_draw_cmd begin = {.kind = SHR_CMD_KEEP_BEGIN, .dst = {0, 0, 8, 8}, .buffer = 1};

    same_status(&d, NULL, 1, SHR_E_INVALID_ARG);
    shr_surface bad = d;
    bad.stride = 1;
    ASSERT_EQ_LL(gl.execute(gl.user, &bad, &fill, 1, 0), SHR_E_INVALID_ARG);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {0, 0, W + 1, 1}}, 1, SHR_E_INVALID_ARG);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {3, 0, 2, 1}}, 1, SHR_E_INVALID_ARG);
    same_status(&d, &(shr_draw_cmd){.kind = 99, .dst = r}, 1, SHR_E_INVALID_ARG);
    /* Neither keeps anything: keep commands are invalid, KEEP_RELEASE has no effect. */
    same_status(&d, (shr_draw_cmd[]){fill, end}, 2, SHR_E_INVALID_ARG);
    same_status(&d, (shr_draw_cmd[]){begin, fill, end}, 3, SHR_E_INVALID_ARG);
    same_one(&d, (shr_draw_cmd){.kind = SHR_CMD_KEEP_DRAW, .dst = r, .buffer = 1}, SHR_E_INVALID_ARG);
    same_status(&d, (shr_draw_cmd[]){{.kind = SHR_CMD_KEEP_RELEASE, .buffer = 1}, {.kind = SHR_CMD_KEEP_RELEASE}}, 2, SHR_OK);
    same_status(&d, (shr_draw_cmd[]){fill, {.kind = SHR_CMD_KEEP_RELEASE, .buffer = 1}}, 2, SHR_E_INVALID_ARG);

    /* Buffers: formats, rects, the even A4 x0, ids. */
    uint32_t i8 = use(a8), ii = use(img), i4 = use(mem(cov, SHR_FORMAT_A4, 8, 4, 4));
    same_one(&d, from(SHR_CMD_GLYPH, r, ii, (shr_rect){0, 0, 2, 2}, o0, 0), SHR_E_UNSUPPORTED);
    same_one(&d, from(SHR_CMD_IMAGE, r, i8, (shr_rect){0, 0, 4, 4}, o0, 0), SHR_E_UNSUPPORTED);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_COPY, .dst = r, .src = img_ref(a8)}, 1, SHR_E_UNSUPPORTED);
    same_one(&d, from(SHR_CMD_GLYPH, r, i8, (shr_rect){0, 0, 4, 4}, (shr_point){1, 0}, 0), SHR_E_INVALID_ARG);
    same_one(&d, from(SHR_CMD_GLYPH, r, i8, (shr_rect){0, 0, 4, 4}, (shr_point){0, 1}, 0), SHR_E_INVALID_ARG);
    same_one(&d, from(SHR_CMD_GLYPH, r, i8, (shr_rect){0, 0, 4, 4}, (shr_point){-1, 0}, 0), SHR_E_INVALID_ARG);
    same_one(&d, from(SHR_CMD_GLYPH, r, i8, (shr_rect){0, 0, 5, 4}, o0, 0), SHR_E_INVALID_ARG);
    same_one(&d, from(SHR_CMD_IMAGE, (shr_rect){0, 0, 2, 2}, ii, (shr_rect){0, 0, 2, 2}, (shr_point){0, -1}, 0), SHR_E_INVALID_ARG);
    same_one(&d, from(SHR_CMD_GLYPH, (shr_rect){0, 0, 3, 4}, i4, (shr_rect){1, 0, 4, 4}, o0, 0),
                SHR_E_INVALID_ARG);
    same_one(&d, from(SHR_CMD_GLYPH, (shr_rect){0, 0, 3, 4}, i4, (shr_rect){2, 0, 5, 4}, o0, 0), SHR_OK);
    same_one(&d, from(SHR_CMD_GLYPH, r, 0, (shr_rect){0, 0, 4, 4}, o0, 0), SHR_E_INVALID_ARG);
    same_one(&d, from(SHR_CMD_GLYPH, r, IDS + 1, (shr_rect){0, 0, 4, 4}, o0, 0), SHR_E_INVALID_ARG);
    same_one(&d, line(r, SHR_LINE_DASHED + 1, 8, 4, o0, 0), SHR_E_INVALID_ARG);
    same_one(&d, line(r, SHR_LINE_CURLY, SHR_LINE_MAX_PERIOD + 1, 4, o0, 0), SHR_E_INVALID_ARG);
    same_one(&d, line(r, SHR_LINE_DOTTED, 8, 3, o0, 0), SHR_E_INVALID_ARG); /* four rows of a three-row cell */
    same_one(&d, line(r, SHR_LINE_SINGLE, 4, 4, (shr_point){4, 0}, 0), SHR_E_INVALID_ARG);
    same_one(&d, line((shr_rect){0, 0, 17, 1}, SHR_LINE_SINGLE, 4, 1, o0, 0), SHR_E_INVALID_ARG);
    same_one(&d, line(r, SHR_LINE_CURLY, 8, 4, (shr_point){7, 0}, 0), SHR_OK);
    same_status(&d, (shr_draw_cmd[]){fill, buf_cmd(SHR_CMD_BUFFER_RELEASE, 1, (shr_rect){0})}, 2, SHR_E_INVALID_ARG);
    shr_image_ref dev_src = src;
    dev_src.domain = SHR_MEMORY_DEVICE; /* not a surface of the driver */
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_COPY, .dst = r, .src = dev_src}, 1, SHR_E_UNSUPPORTED);

    /* COPY overlapping its destination with another stride or format. */
    shr_image_ref shifted = self;
    shifted.height = 6, shifted.stride = (uint32_t)d.stride + 2;
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_COPY, .dst = {0, 2, 6, 6}, .src = shifted}, 1, SHR_E_UNSUPPORTED);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_COPY, .dst = {2, 2, 2, 6}, .src = shifted}, 1, SHR_OK);

    shr_draw_cmd rot = {.kind = SHR_CMD_ROTATE, .dst = {0, 0, W, H}, .src = src, .rotation = SHR_ROTATE_180};
    same_status(&d, &rot, 1, SHR_OK);
    for (int k = 0; k < 7; k++) {
        shr_draw_cmd c = rot;
        shr_status want = k == 6 ? SHR_OK : k < 3 ? SHR_E_INVALID_ARG : SHR_E_UNSUPPORTED;
        if (k == 0) c.rotation = SHR_ROTATE_NONE;
        if (k == 1) c.rotation = (shr_rotation)7;
        if (k == 2) c.rotation = SHR_ROTATE_90_CW; /* a 16x8 source needs an 8x16 output */
        if (k == 3) c.src = self;
        if (k == 4) c.src = img_ref(mem(cov, SHR_FORMAT_A8, 16, 1, 16));
        if (k == 5) c.src = dev_src;
        if (k == 6) c.src.width = c.src.height = 4, c.dst = r; /* part of the source */
        same_status(&d, &c, 1, want);
    }
    shr_draw_cmd rot_bad = rot;
    rot_bad.src.stride = 2;
    same_status(&d, &rot_bad, 1, SHR_E_INVALID_ARG);
    rot_bad = rot, rot_bad.src.height = H - 1;
    same_status(&d, &rot_bad, 1, SHR_E_INVALID_ARG);
    static const shr_rect outside[] = {{-1, 0, 2, 2}, {0, -1, 2, 2}, {0, 3, 2, 2}, {0, 0, 2, H + 1}};
    for (int k = 0; k < 4; k++) same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = outside[k]}, 1, SHR_E_INVALID_ARG);

    /* Styled glyphs (of zero coverage, so accepted ones leave the pixels): footprint, sizes, axis bounds. */
    static uint8_t blank[1025];
    const uint32_t B = SHR_GLYPH_BOLD, I = SHR_GLYPH_ITALIC;
    uint32_t b4 = use(mem(blank, SHR_FORMAT_A8, 4, 4, 4)), none = use((shr_image){NULL, 0, 4, 0, 0, SHR_FORMAT_A8, 0});
    uint32_t wide = use(mem(blank, SHR_FORMAT_A8, 1025, 1, 1025)), tall = use(mem(blank, SHR_FORMAT_A8, 1, 1025, 1));
    int32_t x0, x1;
    shr__glyph_footprint(4, 4, B | I, 3, &x0, &x1);
    shr_draw_cmd ok = from(SHR_CMD_GLYPH, (shr_rect){0, 0, x1 - x0, 4}, b4, (shr_rect){0, 0, 4, 4}, (shr_point){x0, 0}, 0);
    ok.flags = B | I, ok.slant_axis = 3;
    same_status(&d, &ok, 1, SHR_OK);
    const shr_draw_cmd paint = {.kind = SHR_CMD_FILL, .dst = r, .color = SHR_RGB(200, 9, 9)};
    const struct {
        int32_t ox, oy, w, h;
        uint32_t flags;
        int32_t axis;
        uint32_t id;
        shr_rect rect;
        shr_status want;
    } st[] = {
        {x0 - 1, 0, x1 - x0, 4, B | I, 3, b4, {0, 0, 4, 4}, SHR_E_INVALID_ARG},
        {x0, 0, x1 - x0 + 1, 4, B | I, 3, b4, {0, 0, 4, 4}, SHR_E_INVALID_ARG},
        {x0, 1, x1 - x0, 4, B | I, 3, b4, {0, 0, 4, 4}, SHR_E_INVALID_ARG},
        {0, 0, 1, 1, B, 3, wide, {0, 0, 1025, 1}, SHR_E_INVALID_ARG},
        {0, 0, 1, 1, B, 3, tall, {0, 0, 1, 1025}, SHR_E_INVALID_ARG},
        {0, 0, 1, 1, B, 3, wide, {1, 0, 2, 1}, SHR_OK},
        {0, 0, 5, 4, B, INT32_MIN, b4, {0, 0, 4, 4}, SHR_OK},
        {0, 0, 1, 4, B, 3, none, {0, 0, 0, 4}, SHR_OK},
        {-1, 0, 1, 4, I, 3, none, {0, 0, 0, 4}, SHR_OK},
        {0, 0, 1, 2, B, 3, b4, {1, 1, 1, 3}, SHR_OK},
    };
    for (size_t k = 0; k < sizeof(st) / sizeof(st[0]); k++) {
        shr_draw_cmd c = from(SHR_CMD_GLYPH, (shr_rect){0, 0, st[k].w, st[k].h}, st[k].id, st[k].rect,
                              (shr_point){st[k].ox, st[k].oy}, 0);
        c.flags = (uint16_t)st[k].flags, c.slant_axis = st[k].axis;
        same_status(&d, (shr_draw_cmd[]){st[k].want == SHR_OK ? fill : paint, c}, 2, st[k].want);
    }
    for (int32_t axis = -4097; axis <= 4097; axis += 2 * 4097) { /* the footprint holds the origin: only the bound */
        shr__glyph_footprint(4, 4, I, axis, &x0, &x1);
        shr_draw_cmd c = from(SHR_CMD_GLYPH, (shr_rect){0, 0, x1 - x0, 4}, b4, (shr_rect){0, 0, 4, 4}, (shr_point){x0, 0}, 0);
        c.flags = I, c.slant_axis = axis;
        same_status(&d, (shr_draw_cmd[]){paint, c}, 2, SHR_E_INVALID_ARG);
    }
    shr_draw_cmd bad_img = from(SHR_CMD_IMAGE, (shr_rect){0, 0, 2, 2}, ii, (shr_rect){0, 0, 2, 2}, (shr_point){-1, 0}, 0);
    bad_img.flags = B | I;
    same_status(&d, &bad_img, 1, SHR_E_INVALID_ARG);
    PASS();
}

TEST device_and_domain_rules(void) {
    enum { W = 16, H = 8 };
    static uint16_t px[W * H];
    shr_surface dv, cpu = packed(px, SHR_FORMAT_RGB565, W, H);
    ASSERT_EQ_LL(shr_angle_surface_create(&gl, W, H, SHR_FORMAT_RGB565, &dv), SHR_OK);
    shr_draw_cmd fill = {.kind = SHR_CMD_FILL, .dst = {0, 0, 4, 4}};
    shr_surface fake = dv;
    fake.pixels = px;
    ASSERT_EQ_LL(gl.execute(gl.user, &fake, &fill, 1, 0), SHR_E_UNSUPPORTED);
    fake = dv, fake.width = W - 1;
    ASSERT_EQ_LL(gl.execute(gl.user, &fake, &fill, 1, 0), SHR_E_UNSUPPORTED);
    shr_surface dma = cpu;
    dma.domain = SHR_MEMORY_DMA; /* outside the caps */
    ASSERT_EQ_LL(gl.execute(gl.user, &dma, &fill, 1, 0), SHR_E_UNSUPPORTED);
    shr_draw_cmd from_dma = {.kind = SHR_CMD_COPY, .dst = {0, 0, 4, 4}, .src = as_image(&dma)};
    ASSERT_EQ_LL(gl.execute(gl.user, &cpu, &from_dma, 1, 0), SHR_E_UNSUPPORTED);
    /* A buffer cannot be a surface. */
    shr_draw_cmd dev_buf = {.kind = SHR_CMD_BUFFER_REGISTER, .buffer = 1, .src = {dv.pixels, W, H, 0, SHR_FORMAT_A8, SHR_MEMORY_DEVICE, 0}};
    ASSERT_EQ_LL(gl.execute(gl.user, &cpu, &dev_buf, 1, 0), SHR_E_UNSUPPORTED);
    int32_t wide = gl.caps.max_width + 1;
    uint16_t *row = calloc((size_t)wide, 2);
    shr_surface huge = packed(row, SHR_FORMAT_RGB565, wide, 1);
    ASSERT_EQ_LL(gl.execute(gl.user, &huge, &fill, 0, 0), SHR_E_UNSUPPORTED);
    huge = packed(row, SHR_FORMAT_RGB565, 1, wide);
    ASSERT_EQ_LL(gl.execute(gl.user, &huge, &fill, 0, 0), SHR_E_UNSUPPORTED);
    free(row);

    shr_draw_cmd rot = {.kind = SHR_CMD_ROTATE, .dst = {0, 0, W, H}, .src = as_image(&dv), .rotation = SHR_ROTATE_180};
    ASSERT_EQ_LL(gl.execute(gl.user, &dv, &rot, 1, 0), SHR_E_UNSUPPORTED);
    rot.src = as_image(&cpu);
    ASSERT_EQ_LL(gl.execute(gl.user, &dv, &rot, 1, 0), SHR_OK);
    shr_image_ref wrong = as_image(&dv);
    wrong.height = H - 1;
    shr_draw_cmd copy = {.kind = SHR_CMD_COPY, .dst = {0, 0, 4, 4}, .src = wrong};
    ASSERT_EQ_LL(gl.execute(gl.user, &cpu, &copy, 1, 0), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &dv), SHR_OK);
    PASS();
}

/* ---- keeps ---- */

#define KH SHR_CELL_HEIGHT

/* The slot bytes of a row of w RGBX8888 pixels. */
static uint64_t row_slot(int32_t w) { return ((uint64_t)w * KH * 4 + 127) / 128 * 128; }

static shr_angle_stats stats_of(const shr_framebuffer_driver *d) {
    shr_angle_stats st;
    ASSERT_EQ_LL(shr_angle_driver_stats(d, &st), SHR_OK);
    return st;
}

/* The software driver drawing into CPU memory and the ANGLE driver into CPU memory or a DEVICE surface, with the
 * same keeps and starting pixels; both keep their state across batches. */
typedef struct pair {
    shr_framebuffer_driver gl, sw;
    shr_pixel_format f;
    int32_t w, h;
    bool device;
    uint8_t *ref, *out;
    shr_surface rs, ds;
} pair;

static void pair_open(pair *p, uint64_t keep_bytes, uint32_t keeps, shr_pixel_format f, int32_t w, int32_t h,
                      bool device) {
    size_t len = (size_t)w * (size_t)h * bpp(f);
    *p = (pair){.f = f, .w = w, .h = h, .device = device, .ref = malloc(len), .out = malloc(len)};
    noise(p->ref, len);
    memcpy(p->out, p->ref, len);
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, keep_bytes, keeps, IDS, &p->gl), SHR_OK);
    ASSERT_EQ_LL(shr_software_driver_create(NULL, keep_bytes, keeps, IDS, &p->sw), SHR_OK);
    p->rs = packed(p->ref, f, w, h);
    p->ds = device ? device_on(&p->gl, f, w, h, p->out) : packed(p->out, f, w, h);
}

/* Both run the batch, with a REGISTER of each buffer it draws from in front. */
static void pair_run(pair *p, const shr_draw_cmd *cmds, size_t n, shr_status want) {
    size_t m = n;
    shr_draw_cmd *all = with_prologue(cmds, &m);
    ASSERT_EQ_LL(p->sw.execute(p->sw.user, &p->rs, all, m, 0), want);
    ASSERT_EQ_LL(p->gl.execute(p->gl.user, &p->ds, all, m, 0), want);
    free(all);
}

static int pair_diff(pair *p) {
    if (p->device) ASSERT_EQ_LL(shr_angle_surface_read(&p->gl, &p->ds, p->out, (size_t)p->w * bpp(p->f)), SHR_OK);
    return image_diff(p->f, p->ref, p->out, p->w, p->h, (size_t)p->w * bpp(p->f));
}

static void pair_close(pair *p) {
    if (p->device) ASSERT_EQ_LL(shr_angle_surface_destroy(&p->gl, &p->ds), SHR_OK);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&p->gl), SHR_OK);
    ASSERT_EQ_LL(shr_software_driver_destroy(&p->sw), SHR_OK);
    free(p->ref), free(p->out);
}

static shr_draw_cmd keep_draw(uint32_t id, shr_rect dst, shr_point origin) {
    return (shr_draw_cmd){.kind = SHR_CMD_KEEP_DRAW, .dst = dst, .buffer = id, .src_origin = origin};
}

/* Keep group `id` at `k` (at least 30 x 7): an opaque fill, then a glyph, a bold glyph, an image, a copy and a dim
 * fill blended over it. `i8`: 9 x 6 A8, `ii`: 7 x 5 RGBA8888. */
static size_t keep_group(shr_draw_cmd *c, uint32_t id, shr_rect k, uint32_t i8, uint32_t ii, shr_image_ref src) {
    int32_t x = k.x0, y = k.y0;
    size_t n = 0;
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_KEEP_BEGIN, .dst = k, .buffer = id};
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = k, .color = rnd_color()};
    c[n++] = from(SHR_CMD_GLYPH, (shr_rect){x + 1, y + 1, x + 10, y + 7}, i8, (shr_rect){0, 0, 9, 6}, (shr_point){0, 0},
                  rnd_color());
    c[n] = from(SHR_CMD_GLYPH, (shr_rect){x + 9, y, x + 19, y + 6}, i8, (shr_rect){0, 0, 9, 6}, (shr_point){0, 0},
                rnd_color());
    c[n++].flags = SHR_GLYPH_BOLD;
    c[n++] = from(SHR_CMD_IMAGE, (shr_rect){x + 14, y + 2, x + 21, y + 7}, ii, (shr_rect){0, 0, 7, 5},
                  (shr_point){0, 0}, 0);
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_COPY, .dst = {x + 22, y + 1, x + 28, y + 4}, .src = src,
                            .src_origin = {1, 1}};
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .flags = SHR_GLYPH_DIM, .dst = {x + 3, y, x + 20, y + 3},
                            .color = rnd_color()};
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_KEEP_END};
    return n;
}

TEST keep_caps_and_slot_textures(void) {
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 0, 2, IDS, &d), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 255, 2, IDS, &d), SHR_E_INVALID_ARG); /* slots of 0 bytes */
    /* Slots of a whole layer: 256 layers at most, the layers an instance can name. Nothing is allocated yet. */
    uint64_t layer = 4ull * (uint64_t)gl.caps.max_width * (uint64_t)gl.caps.max_width;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 257 * layer, 257, IDS, &d), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 256 * layer, 256, IDS, &d), SHR_OK);
    ASSERT_EQ_LL(stats_of(&d).keep_texture_bytes, 0);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);

    enum { N = 6, KW = 48 };
    uint64_t bytes = N * row_slot(KW) + 100;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, bytes, N, IDS, &d), SHR_OK);
    ASSERT(d.caps.max_keeps == N && d.caps.keep_bytes == bytes && d.caps.max_keep_bytes == row_slot(KW));
    ASSERT_EQ_LL(stats_of(&d).keep_texture_bytes, 0);
    /* Each destination format takes one texture for every slot at its first store, within keep_bytes. */
    uint64_t total = 0;
    for (int fi = 0; fi < 2; fi++) {
        uint8_t px[KW * KH * 4];
        shr_surface s = packed(px, FMTS[fi], KW, KH);
        shr_draw_cmd c[] = {{.kind = SHR_CMD_KEEP_BEGIN, .dst = {0, 0, KW, KH}, .buffer = N},
                            {.kind = SHR_CMD_FILL, .dst = {0, 0, KW, KH}},
                            {.kind = SHR_CMD_KEEP_END}};
        for (int k = 0; k < 2; k++) ASSERT_EQ_LL(d.execute(d.user, &s, c, 3, 0), SHR_OK);
        uint64_t now = stats_of(&d).keep_texture_bytes;
        ASSERT(now - total >= N * (uint64_t)KW * KH * bpp(FMTS[fi]) && now - total <= bytes);
        total = now;
    }
    ASSERT_EQ_LL(stats_of(&d).keep_stores, 4);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
    PASS();
}

TEST keeps_match_the_software_port(void) {
    enum { W = 48, H = 40, KW = 32 };
    uint8_t cov[9 * 6], rgba[7 * 5 * 4], cpu[8 * 8 * 4];
    coverage_noise(cov, sizeof(cov));
    noise(rgba, sizeof(rgba)), noise(cpu, sizeof(cpu));
    uint32_t i8 = use(mem(cov, SHR_FORMAT_A8, 9, 6, 9)), ii = use(mem(rgba, SHR_FORMAT_RGBA8888, 7, 5, 28));
    EACH_TARGET(f, dev) {
        shr_surface src = packed(cpu, FMTS[!fi_], 8, 8);
        pair p;
        pair_open(&p, 3 * row_slot(KW), 3, f, W, H, dev);
        shr_draw_cmd c[32];
        size_t n = keep_group(c, 1, (shr_rect){-20, 30, KW - 20, 30 + KH}, i8, ii, as_image(&src)); /* partly outside */
        c[n++] = keep_draw(1, (shr_rect){2, 2, 2 + KW, 2 + KH}, (shr_point){0, 0});
        c[n++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {0, 20, W, 24}, .color = rnd_color()};
        shr_rect far = {1 << 23, -(1 << 23), (1 << 23) + KW, KH - (1 << 23)};
        n += keep_group(c + n, 2, far, i8, ii, as_image(&src));
        c[n++] = keep_draw(2, (shr_rect){10, 22, 30, 20 + KH}, (shr_point){5, 2});
        c[n++] = keep_draw(1, (shr_rect){30, 1, 48, 6}, (shr_point){3, 2});
        pair_run(&p, c, n, SHR_OK);
        ASSERT(pair_diff(&p) <= MAX_BLEND(f));
        shr_angle_stats st = stats_of(&p.gl);
        ASSERT(st.keep_stores == 2 && st.keep_draws == 3);
        /* Drawn by a later batch. */
        shr_draw_cmd later[] = {{.kind = SHR_CMD_FILL, .dst = {0, 0, W, H}, .color = rnd_color()},
                                keep_draw(2, (shr_rect){16, 16, 16 + KW, 16 + KH}, (shr_point){0, 0}),
                                keep_draw(1, (shr_rect){0, H - KH, KW, H}, (shr_point){0, 0})};
        pair_run(&p, later, 3, SHR_OK);
        ASSERT(pair_diff(&p) <= MAX_BLEND(f));
        /* Released, an id holds nothing; stored again, it holds the new keep. */
        shr_draw_cmd gone[] = {{.kind = SHR_CMD_KEEP_RELEASE, .buffer = 2}, later[1]};
        pair_run(&p, gone, 2, SHR_E_INVALID_ARG);
        n = keep_group(c, 2, (shr_rect){0, 0, KW - 2, KH}, i8, ii, as_image(&src));
        c[n++] = keep_draw(2, (shr_rect){W - KW + 2, 0, W, KH}, (shr_point){0, 0});
        pair_run(&p, c, n, SHR_OK);
        ASSERT(pair_diff(&p) <= MAX_BLEND(f));
        ASSERT_EQ_LL(stats_of(&p.gl).keep_texture_bytes, st.keep_texture_bytes); /* slots stay */
        pair_close(&p);
    }
    PASS();
}

/* LINEs in a keep group, the drivers keeping their cells across batches. */
TEST lines_in_keep_groups_match(void) {
    enum { W = 48, H = 16 };
    EACH_TARGET(f, dev) {
        pair p;
        pair_open(&p, 1 << 16, 1, f, W, H, dev);
        for (int dim = 0; dim < 2; dim++) {
            shr_rect k = {3, 4, 40, 12};
            shr_draw_cmd g[] = {{.kind = SHR_CMD_KEEP_BEGIN, .dst = k, .buffer = 1},
                                {.kind = SHR_CMD_FILL, .dst = k, .color = rnd_color()},
                                line((shr_rect){3, 9, 40, 12}, SHR_LINE_CURLY, 8, 3, (shr_point){3, 0}, rnd_color()),
                                line((shr_rect){4, 4, 39, 7}, SHR_LINE_DOTTED, 8, 3, (shr_point){5, 0}, rnd_color()),
                                line((shr_rect){4, 7, 39, 8}, SHR_LINE_DASHED, 8, 1, (shr_point){1, 0}, rnd_color()),
                                {.kind = SHR_CMD_KEEP_END},
                                keep_draw(1, (shr_rect){5, 2, 42, 10}, (shr_point){0, 0})};
            g[2].flags |= (uint16_t)dim;
            pair_run(&p, g, 7, SHR_OK);
            ASSERT(pair_diff(&p) <= MAX_BLEND(f));
        }
        pair_close(&p);
    }
    PASS();
}

/* A keep drawn is what its group draws directly, bit for bit, also from a later batch; keep draws and glyphs share
 * a draw call. */
TEST keep_draws_are_exact(void) {
    enum { W = 40, H = 2 * KH, KW = 32 };
    uint8_t cov[9 * 6], rgba[7 * 5 * 4], cpu[8 * 8 * 4];
    coverage_noise(cov, sizeof(cov));
    noise(rgba, sizeof(rgba)), noise(cpu, sizeof(cpu));
    uint32_t i8 = use(mem(cov, SHR_FORMAT_A8, 9, 6, 9)), ii = use(mem(rgba, SHR_FORMAT_RGBA8888, 7, 5, 28));
    EACH_TARGET(f, dev) {
        shr_framebuffer_driver d;
        ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 2 * row_slot(KW), 2, IDS, &d), SHR_OK);
        size_t len = (size_t)W * H * bpp(f);
        uint8_t *init = malloc(len), *a = malloc(len), *b = malloc(len);
        noise(init, len);
        memcpy(a, init, len), memcpy(b, init, len);
        shr_surface sa = dev ? device_on(&d, f, W, H, init) : packed(a, f, W, H);
        shr_surface sb = dev ? device_on(&d, f, W, H, init) : packed(b, f, W, H);
        shr_surface src = packed(cpu, f, 8, 8);
        shr_rect at[2] = {{3, 1, 3 + KW, 1 + KH}, {W - KW, H - KH, W, H}};
        shr_draw_cmd c[16], direct[16];
        size_t n = keep_group(c, 1, (shr_rect){-100, 7, KW - 100, 7 + KH}, i8, ii, as_image(&src));
        for (int k = 0; k < 2; k++) { /* the second keep draw comes from the first batch's keep */
            size_t nd = 0, nk = k ? 1 : n + 1;
            for (size_t i = 1; i + 1 < n; i++, nd++) {
                shr_rect r = c[i].dst;
                int32_t dx = at[k].x0 + 100, dy = at[k].y0 - 7;
                direct[nd] = c[i], direct[nd].dst = (shr_rect){r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy};
            }
            c[n] = keep_draw(1, at[k], (shr_point){0, 0});
            shr_draw_cmd *dc = with_prologue(direct, &nd), *kc = with_prologue(k ? c + n : c, &nk);
            ASSERT_EQ_LL(d.execute(d.user, &sa, dc, nd, 0), SHR_OK);
            ASSERT_EQ_LL(d.execute(d.user, &sb, kc, nk, 0), SHR_OK);
            free(dc), free(kc);
        }
        if (dev) {
            ASSERT_EQ_LL(shr_angle_surface_read(&d, &sa, a, (size_t)W * bpp(f)), SHR_OK);
            ASSERT_EQ_LL(shr_angle_surface_read(&d, &sb, b, (size_t)W * bpp(f)), SHR_OK);
        }
        ASSERT_EQ_LL(image_diff(f, a, b, W, H, (size_t)W * bpp(f)), 0);
        shr_draw_cmd mixed[] = {
            keep_draw(1, (shr_rect){0, 0, 8, KH}, (shr_point){0, 0}),
            from(SHR_CMD_GLYPH, (shr_rect){9, 1, 18, 7}, i8, (shr_rect){0, 0, 9, 6}, (shr_point){0, 0}, 0),
            keep_draw(1, (shr_rect){20, 0, 28, KH}, (shr_point){8, 0})};
        uint64_t draws = draws_of(&d);
        ASSERT_EQ_LL(d.execute(d.user, &sb, mixed, 3, 0), SHR_OK);
        ASSERT_EQ_LL(draws_of(&d) - draws, 1);
        if (dev) shr_angle_surface_destroy(&d, &sa), shr_angle_surface_destroy(&d, &sb);
        ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
        free(init), free(a), free(b);
    }
    PASS();
}

/* Slots by id across columns of the slot texture; limits; shapes a slot cannot hold. */
TEST keep_slots_by_id(void) {
    enum { N = 2050, PER_ROW = 64, KW = 2 };
    const int32_t w = PER_ROW * KW, h = (N + PER_ROW - 1) / PER_ROW * KH;
    const uint64_t slot = row_slot(KW);
    shr_draw_cmd *c = calloc(4 * N, sizeof(*c));
    for (int fi = 0; fi < 2; fi++) {
        shr_pixel_format f = FMTS[fi];
        pair p;
        pair_open(&p, N * slot, N, f, w, h, true);
        size_t n = 0;
        for (uint32_t id = 1; id <= N; id++) {
            c[n++] = (shr_draw_cmd){.kind = SHR_CMD_KEEP_BEGIN, .dst = {0, 0, KW, KH}, .buffer = id};
            shr_color unique = SHR_RGB(id << 3, id >> 5 << 2, id >> 11 << 3); /* also in RGB565 */
            c[n++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {0, 0, KW, KH}, .color = unique};
            c[n++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {1, KH - 1, 2, KH}, .color = ~unique};
            c[n++] = (shr_draw_cmd){.kind = SHR_CMD_KEEP_END};
        }
        pair_run(&p, c, n, SHR_OK);
        uint64_t held = stats_of(&p.gl).keep_texture_bytes; /* three columns, the last one not full */
        ASSERT(held >= N * slot && held < (N + 3) * slot);
        n = 0;
        for (uint32_t id = 1; id <= N; id++) {
            int32_t x = (int32_t)(id - 1) % PER_ROW * KW, y = (int32_t)(id - 1) / PER_ROW * KH;
            c[n++] = keep_draw(id, (shr_rect){x, y, x + KW, y + KH}, (shr_point){0, 0});
        }
        pair_run(&p, c, n, SHR_OK);
        ASSERT_EQ_LL(pair_diff(&p), 0);

        int32_t texels = (int32_t)(slot / bpp(f)), sw = texels / KH, sh = texels / sw;
        shr_draw_cmd fill = {.kind = SHR_CMD_FILL, .dst = {0, 0, 1, 1}, .color = rnd_color()};
        shr_draw_cmd big[] = {{.kind = SHR_CMD_KEEP_BEGIN, .dst = {0, 0, sw + 1, KH}, .buffer = 3},
                              {.kind = SHR_CMD_FILL, .dst = {0, 0, sw + 1, KH}},
                              {.kind = SHR_CMD_KEEP_END}};
        pair_run(&p, big, 3, SHR_E_INVALID_ARG); /* more bytes than a slot */
        /* As many bytes but wider or taller than a slot: out of memory, and nothing changes. */
        shr_rect odd[2] = {{0, 0, sw + 1, 1}, {0, 0, 1, sh + 1}};
        for (int k = 0; k < 2; k++) {
            shr_draw_cmd b[] = {{.kind = SHR_CMD_KEEP_BEGIN, .dst = {0, 0, KW, KH}, .buffer = 7},
                                {.kind = SHR_CMD_FILL, .dst = {0, 0, KW, KH}, .color = rnd_color()},
                                {.kind = SHR_CMD_KEEP_END},
                                {.kind = SHR_CMD_KEEP_BEGIN, .dst = odd[k], .buffer = 5},
                                {.kind = SHR_CMD_FILL, .dst = odd[k]},
                                {.kind = SHR_CMD_KEEP_END},
                                fill};
            ASSERT_EQ_LL(p.gl.execute(p.gl.user, &p.ds, b, 7, 0), SHR_E_NO_MEMORY);
        }
        ASSERT_EQ_LL(pair_diff(&p), 0);
        shr_draw_cmd again[] = {keep_draw(7, (shr_rect){0, 0, KW, KH}, (shr_point){0, 0}),
                                keep_draw(5, (shr_rect){KW, 0, 2 * KW, KH}, (shr_point){0, 0})};
        pair_run(&p, again, 2, SHR_OK);
        ASSERT_EQ_LL(pair_diff(&p), 0);
        /* Released, then stored smaller. */
        shr_draw_cmd gone[] = {{.kind = SHR_CMD_KEEP_RELEASE, .buffer = 9}, again[0]};
        gone[1].buffer = 9;
        pair_run(&p, gone, 2, SHR_E_INVALID_ARG);
        shr_draw_cmd small[] = {{.kind = SHR_CMD_KEEP_BEGIN, .dst = {5, 5, 6, 5 + KH}, .buffer = 9},
                                {.kind = SHR_CMD_FILL, .dst = {5, 5, 6, 5 + KH}, .color = rnd_color()},
                                {.kind = SHR_CMD_KEEP_END},
                                keep_draw(9, (shr_rect){3, 0, 4, KH}, (shr_point){0, 0})};
        pair_run(&p, small, 4, SHR_OK);
        ASSERT_EQ_LL(pair_diff(&p), 0);
        ASSERT_EQ_LL(stats_of(&p.gl).keep_texture_bytes, held);
        pair_close(&p);
    }
    free(c);
    PASS();
}

/* A GL error empties every keep and drops the slot texture; the next store makes it again. */
TEST gl_errors_empty_the_keeps(void) {
    enum { KW = 16 };
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    EGLContext ctx = eglGetCurrentContext();
    shr_angle_offscreen *own; /* a context of its own: the textures deleted below are its */
    ASSERT_EQ_LL(shr_angle_offscreen_create(8, 8, &own), SHR_OK);
    uint16_t px[KW * KH];
    shr_surface s = packed(px, SHR_FORMAT_RGB565, KW, KH);
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, 2 * row_slot(KW), 2, IDS, &d), SHR_OK);
    shr_draw_cmd c[] = {{.kind = SHR_CMD_KEEP_BEGIN, .dst = {0, 0, KW, KH}, .buffer = 1},
                        {.kind = SHR_CMD_FILL, .dst = {0, 0, KW, KH}, .color = SHR_RGB(10, 20, 30)},
                        {.kind = SHR_CMD_KEEP_END},
                        keep_draw(1, (shr_rect){0, 0, KW, KH}, (shr_point){0, 0})};
    ASSERT_EQ_LL(d.execute(d.user, &s, c, 4, 0), SHR_OK);
    uint64_t bytes = stats_of(&d).keep_texture_bytes;
    ASSERT(bytes > 0);
    GLuint names[64];
    for (GLuint i = 0; i < 64; i++) names[i] = i + 1;
    glDeleteTextures(64, names); /* the slot texture among them: attaching it fails */
    c[0].buffer = 2;
    ASSERT_EQ_LL(d.execute(d.user, &s, c, 3, 0), SHR_E_DEVICE);
    ASSERT_EQ_LL(stats_of(&d).keep_texture_bytes, 0);
    ASSERT_EQ_LL(d.execute(d.user, &s, c + 3, 1, 0), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(d.execute(d.user, &s, c, 4, 0), SHR_E_INVALID_ARG); /* id 2 stored, id 1 empty */
    c[3].buffer = 2;
    ASSERT_EQ_LL(d.execute(d.user, &s, c, 4, 0), SHR_OK);
    ASSERT_EQ_LL(stats_of(&d).keep_texture_bytes, bytes);
    uint16_t want;
    memcpy(&want, &px[KW * KH - 1], 2);
    ASSERT_EQ_LL(want, (10 * 31 + 127) / 255 << 11 | (20 * 63 + 127) / 255 << 5 | (30 * 31 + 127) / 255);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
    ASSERT_EQ_LL(shr_angle_offscreen_destroy(own), SHR_OK);
    ASSERT(eglMakeCurrent(dpy, draw, read, ctx));
    PASS();
}

/* ---- end to end: a context presenting into ANGLE surfaces against the software driver ---- */

enum { SW_ = 96, SH_ = 64 };

typedef struct outputs {
    shr_surface bufs[2];
    bool busy[2];
    int shown, presents;
} outputs;

static int o_index(const outputs *o, const shr_surface *s) { return s->pixels == o->bufs[0].pixels ? 0 : 1; }

static shr_status o_acquire(void *user, shr_surface *s) {
    outputs *o = user;
    int i = o->busy[0] ? 1 : 0;
    if (o->busy[i]) return SHR_E_WOULD_BLOCK;
    o->busy[i] = true;
    *s = o->bufs[i];
    return SHR_OK;
}

static shr_status o_present(void *user, const shr_surface *s, uint64_t id) {
    (void)id;
    outputs *o = user;
    o->shown = o_index(o, s);
    o->busy[o->shown] = false;
    o->presents++;
    return SHR_OK;
}

static void o_discard(void *user, const shr_surface *s) {
    outputs *o = user;
    o->busy[o_index(o, s)] = false;
}

static size_t scene(shr_context *ctx, shr_pl_res_bitmap_font *font, shr_lyr **ls, shr_pl_res_image **img) {
    size_t n = 0;
    shr_lyr *shapes, *text, *pic;
    ASSERT_EQ_LL(shr_lyr_create(ctx, 0, (shr_rect){0, 0, SW_, SH_}, &shapes), SHR_OK);
    ls[n++] = shapes;
    shr_lyr_cmd_begin(shapes);
    shr_lyr_cmd_fill(shapes, (shr_rect){0, 0, SW_, SHR_CELL_HEIGHT}, SHR_RGB(0x33, 0x37, 0x48));
    shr_lyr_cmd_fill(shapes, (shr_rect){50, 20, 90, 60}, SHR_RGB(0x28, 0x2A, 0x36));
    ASSERT_EQ_LL(shr_lyr_cmd_commit(shapes), SHR_OK);

    ASSERT_EQ_LL(shr_lyr_create(ctx, 1, (shr_rect){0, 0, SW_, 3 * SHR_CELL_HEIGHT}, &text), SHR_OK);
    ls[n++] = text;
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_resize(text, font, 3, SW_ / SHR_CELL_WIDTH, NULL), SHR_OK);
    const char *t = "Shiroko \xEB\xA0\x8C\xEB\x8D\x94 \xE2\x9C\x93 A\xF0\x9F\x98\x80";
    shr_error_info err;
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_text(text, 0, 0, t, strlen(t), (shr_text_style){SHR_RGB(0xF8, 0xF8, 0xF2), 0,
                                             SHR_STYLE_BOLD}, NULL, 0, SHR_TEXT_WRAP, &err),
                 SHR_OK);
    shr_text_style dim = {(SHR_RGB(0x50, 0xFA, 0x7B) & 0xFFFFFFu) | 0x80000000u, SHR_RGB(0x44, 0x47, 0x5A), 0};
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_text(text, 2, 1, "dim bg", 6, dim, NULL, 0, 0, &err), SHR_OK);
    const shr_text_line under = {1, 6, SHR_LINE_UNDER, SHR_LINE_SINGLE, 0, dim.fg};
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_lines(text, 2, &under, 1, &err), SHR_OK);
    const shr_text_line lines[] = {{0, 3, SHR_LINE_UNDER, SHR_LINE_CURLY, 0, SHR_RGB(0xFF, 0x55, 0x55)},
                                   {3, 3, SHR_LINE_UNDER, SHR_LINE_DOTTED, 0, SHR_RGB(0xF1, 0xFA, 0x8C)},
                                   {6, 3, SHR_LINE_UNDER, SHR_LINE_DASHED, 0, (SHR_RGB(0xFF, 0x79, 0xC6) & 0xFFFFFFu) | 0x80000000u},
                                   {9, 3, SHR_LINE_UNDER, SHR_LINE_DOUBLE, 0, SHR_RGB(0x8B, 0xE9, 0xFD)},
                                   {0, 12, SHR_LINE_OVER, SHR_LINE_SINGLE, 0, SHR_RGB(0xBD, 0x93, 0xF9)},
                                   {2, 6, SHR_LINE_STRIKE, SHR_LINE_CURLY, 0, SHR_RGB(0xF8, 0xF8, 0xF2)}};
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_lines(text, 1, lines, sizeof(lines) / sizeof(lines[0]), &err), SHR_OK);

    uint8_t rgba[24 * 24 * 4];
    for (int y = 0; y < 24; y++)
        for (int x = 0; x < 24; x++) {
            uint8_t *p = rgba + (y * 24 + x) * 4;
            p[0] = (uint8_t)(x * 10), p[1] = (uint8_t)(y * 10), p[2] = 0xC0;
            p[3] = (uint8_t)(x < 8 ? 255 : x < 16 ? 128 : (23 - x) * 30);
        }
    ASSERT_EQ_LL(shr_pl_res_image_create(ctx, 24, 24, rgba, 24 * 4, img), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_create(ctx, 2, (shr_rect){40, 24, SW_, SH_}, &pic), SHR_OK);
    ls[n++] = pic;
    shr_lyr_cmd_begin(pic);
    ASSERT_EQ_LL(shr_lyr_cmd_image(pic, *img, (shr_rect){0, 0, 24, 24}, (shr_point){0, 0}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_image(pic, *img, (shr_rect){4, 4, 24, 24}, (shr_point){30, 12}), SHR_OK);
    ASSERT_EQ_LL(shr_lyr_cmd_commit(pic), SHR_OK);
    return n;
}

typedef struct scene_cfg {
    shr_rotation rotation;
    shr_pixel_format output_format;
    bool device_composition, device_outputs;
} scene_cfg;

/* The presented frame, packed. */
static uint8_t *render(shr_framebuffer_driver *d, scene_cfg cfg) {
    bool quarter = cfg.rotation == SHR_ROTATE_90_CW || cfg.rotation == SHR_ROTATE_90_CCW;
    int32_t ow = quarter ? SH_ : SW_, oh = quarter ? SW_ : SH_;
    shr_pixel_format of = cfg.output_format ? cfg.output_format : SHR_PIXEL_FORMAT;
    size_t len = (size_t)ow * (size_t)oh * bpp(of);
    outputs o = {0};
    uint8_t *cpu[2] = {calloc(1, len), calloc(1, len)}, *frame = malloc(len);
    for (int i = 0; i < 2; i++) {
        if (cfg.device_outputs) ASSERT_EQ_LL(shr_angle_surface_create(&gl, ow, oh, of, &o.bufs[i]), SHR_OK);
        else o.bufs[i] = packed(cpu[i], of, ow, oh);
    }
    shr_surface comp = {0};
    if (cfg.device_composition) ASSERT_EQ_LL(shr_angle_surface_create(&gl, SW_, SH_, SHR_PIXEL_FORMAT, &comp), SHR_OK);
    shr_output output;
    shr_output_init(&output);
    output.user = &o, output.flags = SHR_OUTPUT_RELEASE_ON_PRESENT;
    output.acquire = o_acquire, output.present = o_present, output.discard = o_discard;
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.driver = d, cd.output = &output, cd.page_cache_bytes = 64u << 20;
    cd.io_retry_ns = cd.io_timeout_ns = 0;
    shr_context *ctx;
    ASSERT_EQ_LL(shr_create(&cd, &ctx), SHR_OK);
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = SW_, sd.height = SH_, sd.rotation = cfg.rotation, sd.output_format = cfg.output_format;
    sd.composition = cfg.device_composition ? &comp : NULL;
    sd.clear = SHR_RGB(0x1E, 0x1F, 0x29);
    ASSERT_EQ_LL(shr_screen_configure(ctx, &sd), SHR_OK);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = font_dir_open, fd.locale = "ko";
    shr_pl_res_bitmap_font *font;
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_create(ctx, &fd, &font), SHR_OK);
    shr_lyr *ls[4];
    shr_pl_res_image *img;
    size_t n = scene(ctx, font, ls, &img);
    ASSERT_EQ_LL(shr_submit(ctx), SHR_OK);
    settle(ctx);
    shr_event ev;
    while (shr_poll_event(ctx, &ev) == SHR_OK)
        ASSERT_EQ_LL(ev.kind == SHR_EVENT_PRESENT_FAILED || ev.kind == SHR_EVENT_RESOURCE_FAILED, 0);
    ASSERT_EQ_LL(o.presents > 0, 1);
    if (cfg.device_outputs) ASSERT_EQ_LL(shr_angle_surface_read(&gl, &o.bufs[o.shown], frame, (size_t)ow * bpp(of)), SHR_OK);
    else memcpy(frame, cpu[o.shown], len);
    for (size_t i = 0; i < n; i++) shr_lyr_destroy(ls[i]);
    shr_pl_res_image_release(img);
    ASSERT_EQ_LL(shr_pl_res_bitmap_font_destroy(font), SHR_OK);
    shr_begin_shutdown(ctx);
    for (int i = 0; i < 16 && shr_destroy(ctx) == SHR_E_WOULD_BLOCK; i++) shr_pump(ctx);
    for (int i = 0; i < 2; i++) {
        if (cfg.device_outputs) shr_angle_surface_destroy(&gl, &o.bufs[i]);
        free(cpu[i]);
    }
    if (cfg.device_composition) shr_angle_surface_destroy(&gl, &comp);
    return frame;
}

/* The other format's pixels in SHR_PIXEL_FORMAT, converted like the software port (lossless both ways for
 * what it wrote). Frees `px`. */
static uint8_t *to_screen_format(uint8_t *px, int32_t n) {
    uint8_t *out = calloc((size_t)n, 4);
    for (int32_t i = 0; i < n; i++) {
        if (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565) {
            const uint8_t *p = px + 4 * i;
            uint16_t v = (uint16_t)(((p[0] * 31 + 127) / 255) << 11 | ((p[1] * 63 + 127) / 255) << 5 | (p[2] * 31 + 127) / 255);
            memcpy(out + 2 * i, &v, 2);
        } else {
            uint16_t v;
            memcpy(&v, px + 2 * i, 2);
            uint8_t *p = out + 4 * i;
            p[0] = (uint8_t)(((v >> 11) * 255 + 15) / 31), p[1] = (uint8_t)(((v >> 5 & 63) * 255 + 31) / 63);
            p[2] = (uint8_t)(((v & 31) * 255 + 15) / 31);
        }
    }
    free(px);
    return out;
}

TEST scene_matches_the_software_driver(void) {
    const shr_pixel_format other = SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? SHR_FORMAT_RGBX8888 : SHR_FORMAT_RGB565;
    const scene_cfg cfgs[] = {
        {SHR_ROTATE_NONE, 0, false, true},           /* drawn straight into the output surface */
        {SHR_ROTATE_NONE, 0, false, false},          /* CPU outputs */
        {SHR_ROTATE_90_CW, 0, false, true},          /* CPU composition rotated into a surface */
        {SHR_ROTATE_90_CCW, 0, true, true},          /* surface rotated into a surface */
        {SHR_ROTATE_180, 0, true, false},            /* surface rotated into CPU memory */
        {SHR_ROTATE_NONE, other, false, true},       /* CPU composition converted */
        {SHR_ROTATE_NONE, other, true, true},        /* surface converted */
    };
    shr_framebuffer_driver sw;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1u << 20, 256, 256, &sw), SHR_OK);
    for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i++) {
        scene_cfg ref_cfg = cfgs[i];
        ref_cfg.device_composition = ref_cfg.device_outputs = false;
        uint8_t *ref = render(&sw, ref_cfg), *got = render(&gl, cfgs[i]);
        bool quarter = cfgs[i].rotation == SHR_ROTATE_90_CW || cfgs[i].rotation == SHR_ROTATE_90_CCW;
        shr_pixel_format of = cfgs[i].output_format ? cfgs[i].output_format : SHR_PIXEL_FORMAT;
        int32_t ow = quarter ? SH_ : SW_, oh = quarter ? SW_ : SH_;
        if (of != SHR_PIXEL_FORMAT) { /* differences count in the composition's units */
            ref = to_screen_format(ref, ow * oh), got = to_screen_format(got, ow * oh);
            of = SHR_PIXEL_FORMAT;
        }
        int d = image_diff(of, ref, got, ow, oh, (size_t)ow * bpp(of));
        note(K_SCENE, of, d);
        free(ref), free(got);
        ASSERT(d <= MAX_BLEND(of));
    }
    shr_software_driver_destroy(&sw);
    PASS();
}

SUITE(driver) {
    RUN_TEST(driver_create_and_destroy);
    RUN_TEST(surface_api);
    RUN_TEST(surface_texture_and_orientation);
    RUN_TEST(calls_need_the_driver_context);
    RUN_TEST(hostile_application_state_is_reset);
    RUN_TEST(allocation_failures);
    RUN_TEST(fill_matches);
    RUN_TEST(many_quads_take_few_draws);
    RUN_TEST(glyphs_match);
    RUN_TEST(a4_nibbles_from_r8_bytes);
    RUN_TEST(styled_glyphs_match);
    RUN_TEST(styled_glyphs_at_atlas_edges);
    RUN_TEST(styled_glyphs_match_at_the_axis_bounds);
    RUN_TEST(images_match);
    RUN_TEST(scaled_images_match);
    RUN_TEST(lines_match);
    RUN_TEST(copies_convert_exactly);
    RUN_TEST(copy_scrolls_within_a_surface);
    RUN_TEST(rotations_map_exactly);
    RUN_TEST(scratch_textures_grow_in_either_direction);
    RUN_TEST(errors_left_by_the_application_are_not_ours);
    RUN_TEST(buffers_register_update_replace_release);
    RUN_TEST(texture_arrays_are_shared_and_reused);
    RUN_TEST(more_than_eight_textures_flush);
    RUN_TEST(budget_limits_registration);
    RUN_TEST(errors_then_registering_again);
    RUN_TEST(full_screen_text_is_a_few_draws);
    RUN_TEST(invalid_batches_match_the_software_port);
    RUN_TEST(device_and_domain_rules);
    RUN_TEST(keep_caps_and_slot_textures);
    RUN_TEST(keeps_match_the_software_port);
    RUN_TEST(lines_in_keep_groups_match);
    RUN_TEST(keep_draws_are_exact);
    RUN_TEST(keep_slots_by_id);
    RUN_TEST(gl_errors_empty_the_keeps);
    RUN_TEST(scene_matches_the_software_driver);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    shr_angle_offscreen *probe;
    if (shr_angle_offscreen_create(8, 8, &probe) != SHR_OK) {
        printf("no EGL display: skipped\n");
        return 77;
    }
    shr_angle_offscreen_destroy(probe);
    RUN_SUITE(lifetime);
    shr_angle_offscreen *off;
    if (shr_angle_offscreen_create(64, 64, &off) != SHR_OK ||
        shr_angle_driver_create(NULL, 64u << 20, 0, 0, 256, &gl) != SHR_OK ||
        shr_software_driver_create(NULL, 0, 0, IDS, &swd) != SHR_OK)
        return EXIT_FAILURE;
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    RUN_SUITE(driver);
    shr_software_driver_destroy(&swd);
    shr_angle_driver_destroy(&gl);
    shr_angle_offscreen_destroy(off);
    printf("max difference to the software port (channel units of the destination): RGB565 RGBX8888\n");
    for (int k = 0; k < KINDS; k++) printf("  %-9s %d %d\n", kind_names[k], max_diff[k][0], max_diff[k][1]);
    GREATEST_MAIN_END();
}
