/* The ANGLE driver against the software port: every batch is drawn by both from the same starting pixels and
 * compared, exactly for FILL/COPY/ROTATE and within MAX_BLEND per channel (in the destination's own channel
 * units) for blends. Skips (exit code 77) when no EGL display can be created. */
#define _POSIX_C_SOURCE 200809L

#include <shiroko/port_angle.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "harness.h"
#include "shr_glyph.h"

/* Metal blends RGB565 targets at reduced precision; RGBX8888 blends match exactly. */
#define MAX_BLEND(f) ((f) == SHR_FORMAT_RGB565 ? 1 : 0)

static const shr_pixel_format FMTS[2] = {SHR_FORMAT_RGB565, SHR_FORMAT_RGBX8888};

static shr_framebuffer_driver gl;

enum { K_FILL, K_DIM, K_GLYPH, K_SYNTH, K_IMAGE, K_COPY, K_ROTATE, K_GROUP, K_SCENE, KINDS };
static const char *const kind_names[KINDS] = {"fill", "fill dim", "glyph", "styled", "image", "copy", "rotate", "group", "scene"};
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

static shr_image as_image(const shr_surface *s) {
    return (shr_image){s->pixels, s->width, s->height, s->stride, s->byte_length, s->format, s->domain};
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

/* Draws the batch with the software port and with `d` (into a CPU or DEVICE surface) from the same noise. */
static int compare_with(shr_framebuffer_driver *d, const shr_draw_cmd *cmds, size_t n, shr_pixel_format f, int32_t w,
                        int32_t h, bool device, int kind) {
    size_t len = (size_t)w * (size_t)h * bpp(f);
    uint8_t *init = malloc(len), *ref = malloc(len), *out = malloc(len);
    noise(init, len);
    memcpy(ref, init, len), memcpy(out, init, len);
    shr_surface rs = packed(ref, f, w, h), ds = packed(out, f, w, h);
    shr_draw_cmd *rc = bound_to(cmds, n, &rs, true);
    ASSERT_EQ_LL(shr_software_execute(&rs, rc, n), SHR_OK);
    if (device) ds = device_on(d, f, w, h, init);
    shr_draw_cmd *dc = bound_to(cmds, n, &ds, false);
    if (pre_execute) pre_execute();
    ASSERT_EQ_LL(d->execute(d->user, &ds, dc, n, 0), SHR_OK);
    if (device) {
        ASSERT_EQ_LL(shr_angle_surface_read(d, &ds, out, (size_t)w * bpp(f)), SHR_OK);
        ASSERT_EQ_LL(shr_angle_surface_destroy(d, &ds), SHR_OK);
    }
    int diff = image_diff(f, ref, out, w, h, (size_t)w * bpp(f));
    note(kind, f, diff);
    free(rc), free(dc), free(init), free(ref), free(out);
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
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, &d), SHR_E_STATE);
    PASS();
}

SUITE(lifetime) { RUN_TEST(offscreen_backends_and_lifetime); }

/* ---- driver API ---- */

TEST driver_create_and_destroy(void) {
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, NULL), SHR_E_INVALID_ARG);
    fail_alloc f = {-1, 0};
    shr_allocator half = fail_allocator(&f);
    half.free = NULL;
    ASSERT_EQ_LL(shr_angle_driver_create(&half, 0, &d), SHR_E_INVALID_ARG);
    shr_allocator a = fail_allocator(&f);
    for (long budget = 0; budget < 2; budget++) {
        f.budget = budget;
        ASSERT_EQ_LL(shr_angle_driver_create(&a, 0, &d), SHR_E_NO_MEMORY);
        ASSERT_EQ_LL(f.live, 0);
    }
    f.budget = -1;
    ASSERT_EQ_LL(shr_angle_driver_create(&a, 1u << 20, &d), SHR_OK);
    ASSERT_EQ_LL(d.caps.domains, SHR_MEMORY_CPU | SHR_MEMORY_DEVICE);
    ASSERT(d.caps.max_width >= 2048 && d.caps.max_width == d.caps.max_height);
    ASSERT_EQ_LL(d.reset(d.user), SHR_OK);
    /* Destroy frees surfaces and cached textures still alive. */
    shr_surface s;
    ASSERT_EQ_LL(shr_angle_surface_create(&d, 4, 4, SHR_FORMAT_RGB565, &s), SHR_OK);
    uint8_t cov[4] = {255, 0, 128, 7}, px[4 * 4 * 2];
    shr_surface cpu = packed(px, SHR_FORMAT_RGB565, 4, 4);
    shr_draw_cmd c = {.kind = SHR_CMD_GLYPH, .dst = {0, 0, 2, 2}, .color = SHR_RGB(1, 2, 3),
                      .src = {cov, 2, 2, 2, 4, SHR_FORMAT_A8, 0}};
    ASSERT_EQ_LL(d.execute(d.user, &cpu, &c, 1, 0), SHR_OK);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_OK);
    ASSERT_EQ_LL(f.live, 0);
    ASSERT_EQ_LL(d.execute == NULL, 1);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&d), SHR_E_INVALID_ARG);
    ASSERT_EQ_LL(shr_angle_driver_destroy(NULL), SHR_E_INVALID_ARG);
    shr_framebuffer_driver sw;
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 0, &sw), SHR_OK);
    ASSERT_EQ_LL(shr_angle_driver_destroy(&sw), SHR_E_INVALID_ARG);
    shr_software_driver_destroy(&sw);
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
    glBindSampler(0, hostile_sampler);
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
        {.kind = SHR_CMD_GLYPH, .dst = {1, 3, 8, 8}, .color = SHR_RGB(250, 200, 100), .src = {cov, 7, 5, 7, 35, SHR_FORMAT_A8, 0}},
        {.kind = SHR_CMD_IMAGE, .dst = {9, 3, 15, 7}, .src = {rgba, 6, 4, 24, sizeof(rgba), SHR_FORMAT_RGBA8888, 0}},
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

TEST image_hashes_are_kept_per_batch(void) {
    /* More images than remembered hashes, each drawn in several rectangles between glyphs. */
    enum { N = 6, IW = 8, IH = 6 };
    uint8_t img[N][IH * IW * 4], cov[IH * IW];
    noise(img, sizeof(img)), noise(cov, sizeof(cov));
    shr_draw_cmd c[4 * N];
    for (int i = 0; i < 4 * N; i++) {
        int k = i % N, x = (i % 8) * 6, y = (i / 8) * 6;
        c[i] = (shr_draw_cmd){.kind = i % 5 == 4 ? SHR_CMD_GLYPH : SHR_CMD_IMAGE, .dst = {x, y, x + 5, y + 4},
                              .color = rnd_color(), .src = {img[k], IW, IH, IW * 4, sizeof(img[k]), SHR_FORMAT_RGBA8888, 0},
                              .src_origin = {i % 3, i % 2}};
        if (c[i].kind == SHR_CMD_GLYPH) c[i].src = (shr_image){cov, IW, IH, IW, sizeof(cov), SHR_FORMAT_A8, 0};
    }
    EACH_TARGET(f, dev) ASSERT(compare(c, 4 * N, f, 48, 24, dev, K_IMAGE) <= MAX_BLEND(f));

    /* An image read from the destination twice, rewritten in between: hashed again. */
    const int32_t w = 16, h = 8;
    uint16_t a[16 * 8], b[16 * 8];
    noise(a, sizeof(a));
    memcpy(b, a, sizeof(a));
    shr_surface sa = packed(a, SHR_FORMAT_RGB565, w, h), sb = packed(b, SHR_FORMAT_RGB565, w, h);
    shr_draw_cmd d[] = {{.kind = SHR_CMD_IMAGE, .dst = {4, 4, 8, 6}, .src = {a, 4, 2, sa.stride, 2 * sa.stride, SHR_FORMAT_RGBA8888, 0}},
                        {.kind = SHR_CMD_FILL, .dst = {0, 0, w, 3}, .color = SHR_RGB(200, 60, 7)},
                        {.kind = SHR_CMD_IMAGE, .dst = {8, 4, 12, 6}, .src = {a, 4, 2, sa.stride, 2 * sa.stride, SHR_FORMAT_RGBA8888, 0}}};
    ASSERT_EQ_LL(shr_software_execute(&sa, d, 3), SHR_OK);
    d[0].src.pixels = d[2].src.pixels = b;
    ASSERT_EQ_LL(gl.execute(gl.user, &sb, d, 3, 0), SHR_OK);
    int diff = image_diff(SHR_FORMAT_RGB565, (uint8_t *)a, (uint8_t *)b, w, h, sa.stride);
    note(K_IMAGE, SHR_FORMAT_RGB565, diff);
    ASSERT(diff <= MAX_BLEND(SHR_FORMAT_RGB565));
    PASS();
}

TEST allocation_failures(void) {
    fail_alloc f = {-1, 0};
    shr_allocator a = fail_allocator(&f);
    shr_framebuffer_driver d;
    ASSERT_EQ_LL(shr_angle_driver_create(&a, 1u << 20, &d), SHR_OK);
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

    /* Cache entry and hash table allocations failing: drawn uncached. */
    uint8_t cov[6 * 5];
    noise(cov, sizeof(cov));
    shr_draw_cmd g = {.kind = SHR_CMD_GLYPH, .dst = {1, 1, 7, 6}, .color = SHR_RGB(200, 100, 50),
                      .src = {cov, 6, 5, 6, sizeof(cov), SHR_FORMAT_A8, 0}};
    for (long budget = 1; budget <= 3; budget++) {
        ASSERT_EQ_LL(shr_angle_surface_destroy(&d, &s), SHR_OK);
        shr_angle_driver_destroy(&d);
        f.budget = -1;
        ASSERT_EQ_LL(shr_angle_driver_create(&a, 1u << 20, &d), SHR_OK);
        ASSERT_EQ_LL(shr_angle_surface_create(&d, 8, 8, SHR_FORMAT_RGB565, &s), SHR_OK);
        f.budget = budget; /* staging, then the entry, the table */
        ASSERT(compare_with(&d, &g, 1, SHR_FORMAT_RGB565, 8, 8, false, K_GLYPH) <= MAX_BLEND(SHR_FORMAT_RGB565));
    }
    f.budget = -1;
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

TEST many_quads_flush_in_batches(void) {
    enum { N = 2500 };
    shr_draw_cmd *c = calloc(N, sizeof(*c));
    for (int i = 0; i < N; i++)
        c[i] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {i % 61, i / 61, i % 61 + 1, i / 61 + 1}, .color = rnd_color()};
    EACH_TARGET(f, dev) ASSERT_EQ_LL(compare(c, N, f, 61, 41, dev, K_FILL), 0);
    free(c);
    PASS();
}

TEST glyphs_match(void) {
    const int32_t w = 29, h = 19;
    uint8_t a8[9 * 7], a4[4 * 6], a4wide[8 * 3];
    noise(a8, sizeof(a8)), noise(a4, sizeof(a4)), noise(a4wide, sizeof(a4wide));
    a8[0] = 0, a8[1] = 255, a8[2] = 1, a8[3] = 128, a8[4] = 129, a4[0] = 0x0F, a4[1] = 0xF1;
    shr_image g8 = {a8, 9, 7, 9, sizeof(a8), SHR_FORMAT_A8, 0};
    shr_image g4 = {a4, 7, 6, 4, sizeof(a4), SHR_FORMAT_A4, 0};     /* odd width, padded rows */
    shr_image g4w = {a4wide, 15, 3, 8, sizeof(a4wide), SHR_FORMAT_A4, 0};
    shr_draw_cmd c[] = {
        {.kind = SHR_CMD_GLYPH, .dst = {0, 0, 9, 7}, .color = SHR_RGB(255, 255, 255), .src = g8},
        {.kind = SHR_CMD_GLYPH, .flags = SHR_GLYPH_DIM, .dst = {10, 1, 19, 8}, .color = SHR_RGB(250, 20, 90), .src = g8},
        {.kind = SHR_CMD_GLYPH, .dst = {w - 4, h - 3, w, h}, .color = SHR_RGB(0, 0, 255), .src = g8, .src_origin = {5, 4}},
        {.kind = SHR_CMD_GLYPH, .dst = {0, 9, 7, 15}, .color = SHR_RGB(30, 255, 60), .src = g4},
        {.kind = SHR_CMD_GLYPH, .dst = {8, 9, 13, 13}, .color = SHR_RGB(200, 100, 0), .src = g4, .src_origin = {1, 1}},
        {.kind = SHR_CMD_GLYPH, .flags = SHR_GLYPH_DIM, .dst = {14, 10, 20, 15}, .color = SHR_RGB(9, 99, 199), .src = g4,
         .src_origin = {1, 0}},
        {.kind = SHR_CMD_GLYPH, .dst = {14, 16, 29, 19}, .color = SHR_RGB(128, 128, 128), .src = g4w},
        {.kind = SHR_CMD_GLYPH, .dst = {3, 3, 3, 5}, .color = SHR_RGB(1, 1, 1), .src = g8},
    };
    EACH_TARGET(f, dev) ASSERT(compare(c, 8, f, w, h, dev, K_GLYPH) <= MAX_BLEND(f));
    PASS();
}

/* Coverage bytes with runs of 0 and 255, so the BOLD gap rule meets every case. */
static void coverage_noise(uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t v = rnd();
        p[i] = (uint8_t)(v % 4 == 0 ? 0 : v % 4 == 1 ? 0xFF : v >> 8);
    }
}

/* Every BOLD/ITALIC/DIM mix of `m` with an axis above, inside and below it, each over its whole footprint and
 * again clipped with src_origin moved by (1, 1) (negative x when the footprint starts left of -1), then a group
 * drawing the first row under a smaller cache_clip. `white`: white glyphs after an opaque black FILL, so every
 * RGBX8888 pixel is the coverage itself. */
static size_t styled_batch(shr_draw_cmd *c, shr_image m, bool white, int32_t *w, int32_t *h) {
    const uint32_t B = SHR_GLYPH_BOLD, I = SHR_GLYPH_ITALIC, D = SHR_GLYPH_DIM;
    const uint32_t flags[6] = {B, I, B | I, B | D, I | D, B | I | D};
    const int32_t axes[3] = {-30, m.height, 2 * m.height + 9};
    int32_t colw = 0, x0, x1;
    for (int a = 0; a < 3; a++)
        for (int f = 0; f < 6; f++) {
            shr__glyph_footprint(m.width, m.height, flags[f], axes[a], &x0, &x1);
            colw = x1 - x0 + 1 > colw ? x1 - x0 + 1 : colw;
        }
    int32_t rowh = m.height + 1;
    *w = 6 * colw, *h = 6 * rowh;
    size_t n = 0;
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {0, 0, *w, *h}, .color = white ? 0 : rnd_color()};
    for (int clipped = 0; clipped < 2; clipped++)
        for (int a = 0; a < 3; a++)
            for (int f = 0; f < 6; f++) {
                shr__glyph_footprint(m.width, m.height, flags[f], axes[a], &x0, &x1);
                int32_t x = f * colw, y = (clipped * 3 + a) * rowh, k = clipped;
                c[n++] = (shr_draw_cmd){.kind = SHR_CMD_GLYPH, .flags = flags[f],
                                        .dst = {x + k, y + k, x + x1 - x0 - k, y + m.height},
                                        .color = white ? SHR_RGB(255, 255, 255) : rnd_color(), .src = m,
                                        .src_origin = {x0 + k, k}, .slant_axis = axes[a]};
            }
    shr_rect g = {0, 0, 3 * colw, rowh};
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_CACHE_BEGIN, .dst = g, .key = {9, 9}, .cache_clip = {2, 1, 2 * colw + 1, rowh - 1}};
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = g, .color = white ? 0 : rnd_color()};
    for (int f = 0; f < 3; f++) c[n++] = c[1 + f];
    c[n++] = (shr_draw_cmd){.kind = SHR_CMD_CACHE_END};
    return n;
}

TEST styled_glyphs_match(void) {
    uint8_t a8[9 * 7], a4[5 * 6], a4s[3 * 3], wide[40 * 4];
    coverage_noise(a8, sizeof(a8)), coverage_noise(a4, sizeof(a4)), coverage_noise(a4s, sizeof(a4s));
    noise(wide, sizeof(wide));
    const shr_image srcs[3] = {{a8, 9, 7, 9, sizeof(a8), SHR_FORMAT_A8, 0},
                               {a4, 7, 6, 5, sizeof(a4), SHR_FORMAT_A4, 0}, /* odd width, noisy padding */
                               {a4s, 5, 3, 3, sizeof(a4s), SHR_FORMAT_A4, 0}};
    shr_framebuffer_driver none;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, &none), SHR_OK);
    shr_draw_cmd c[48];
    int32_t w, h;
    for (int i = 0; i < 3; i++) {
        size_t n = styled_batch(c, srcs[i], true, &w, &h);
        for (int dev = 0; dev < 2; dev++) ASSERT_EQ_LL(compare(c, n, SHR_FORMAT_RGBX8888, w, h, dev, K_SYNTH), 0);
        /* Uncached: each source is uploaded whole, after a wider one left texels beyond its width. */
        memmove(c + 2, c + 1, (n - 1) * sizeof(c[0]));
        c[1] = (shr_draw_cmd){.kind = SHR_CMD_GLYPH, .dst = {0, 0, 40, 4}, .color = SHR_RGB(255, 255, 255),
                              .src = {wide, 40, 4, 40, sizeof(wide), SHR_FORMAT_A8, 0}};
        for (int dev = 0; dev < 2; dev++)
            ASSERT_EQ_LL(compare_with(&none, c, n + 1, SHR_FORMAT_RGBX8888, w, h, dev, K_SYNTH), 0);
        n = styled_batch(c, srcs[i], false, &w, &h);
        EACH_TARGET(f, dev) {
            ASSERT(compare(c, n, f, w, h, dev, K_SYNTH) <= MAX_BLEND(f));
            ASSERT(compare_with(&none, c, n, f, w, h, dev, K_SYNTH) <= MAX_BLEND(f));
        }
    }
    ASSERT_EQ_LL(shr_angle_driver_destroy(&none), SHR_OK);
    PASS();
}

/* The axis bounds, which the int16 attribute must carry exactly. */
TEST styled_glyphs_match_at_the_axis_bounds(void) {
    uint8_t a8[6 * 5];
    coverage_noise(a8, sizeof(a8));
    shr_image m = {a8, 6, 5, 6, sizeof(a8), SHR_FORMAT_A8, 0};
    for (int white = 0; white < 2; white++) {
        shr_draw_cmd c[5] = {{.kind = SHR_CMD_FILL, .dst = {0, 0, 40, 5}, .color = white ? 0 : rnd_color()}};
        for (int i = 0; i < 4; i++) {
            int32_t x0, x1, axis = i / 2 ? 4 * SHR_GLYPH_SYNTH_MAX : -4 * SHR_GLYPH_SYNTH_MAX;
            uint32_t flags = SHR_GLYPH_ITALIC | (i % 2 ? SHR_GLYPH_BOLD : 0);
            shr__glyph_footprint(6, 5, flags, axis, &x0, &x1);
            c[1 + i] = (shr_draw_cmd){.kind = SHR_CMD_GLYPH, .flags = flags, .dst = {10 * i, 0, 10 * i + x1 - x0, 5},
                                      .color = white ? SHR_RGB(255, 255, 255) : rnd_color(), .src = m,
                                      .src_origin = {x0, 0}, .slant_axis = axis};
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
    shr_image img = {rgba, IW, IH, IW * 4, sizeof(rgba), SHR_FORMAT_RGBA8888, 0};
    shr_image unaligned = {odd, IW, IH, IW * 4 + 3, sizeof(odd), SHR_FORMAT_RGBA8888, 0};
    shr_draw_cmd c[] = {
        {.kind = SHR_CMD_IMAGE, .dst = {0, 0, IW, IH}, .src = img},
        {.kind = SHR_CMD_IMAGE, .dst = {w - 6, h - 5, w, h}, .src = img, .src_origin = {7, 4}},
        {.kind = SHR_CMD_IMAGE, .dst = {14, 0, 14 + IW, IH}, .src = unaligned},
        {.kind = SHR_CMD_IMAGE, .dst = {0, 10, 5, 13}, .src = unaligned, .src_origin = {8, 6}},
        {.kind = SHR_CMD_IMAGE, .dst = {2, 2, 2 + IW, 2 + IH}, .src = img},
    };
    EACH_TARGET(f, dev) ASSERT(compare(c, 5, f, w, h, dev, K_IMAGE) <= MAX_BLEND(f));
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
        shr_surface dev = device_with(FMTS[sf], SW, SH, px);
        shr_draw_cmd c[] = {
            {.kind = SHR_CMD_COPY, .dst = {0, 0, SW, SH}, .src = as_image(&src)},
            {.kind = SHR_CMD_COPY, .dst = {w - 7, h - 5, w, h}, .src = as_image(&src), .src_origin = {13, 7}},
            {.kind = SHR_CMD_COPY, .dst = {21, 0, 31, 4}, .src = as_image(&mis), .src_origin = {3, 2}},
            {.kind = SHR_CMD_COPY, .dst = {0, 13, 9, 17}, .src = as_image(&dev), .src_origin = {11, 8}},
            {.kind = SHR_CMD_COPY, .dst = {5, 5, 5, 6}, .src = as_image(&dev)},
        };
        EACH_TARGET(f, d) ASSERT_EQ_LL(compare(c, 5, f, w, h, d, K_COPY), 0);
        device_drop(&dev);
        free(px);
    }
    PASS();
}

TEST copy_scrolls_within_a_surface(void) {
    const int32_t w = 23, h = 15;
    shr_image self = {SELF, w, h, 0, 0, 0, 0};
    shr_draw_cmd up[] = {{.kind = SHR_CMD_COPY, .dst = {0, 0, w, h - 3}, .src = self, .src_origin = {0, 3}},
                         {.kind = SHR_CMD_FILL, .dst = {0, h - 3, w, h}, .color = SHR_RGB(4, 5, 6)}};
    shr_draw_cmd down[] = {{.kind = SHR_CMD_FILL, .dst = {0, 0, w, 2}, .color = SHR_RGB(99, 5, 6)},
                           {.kind = SHR_CMD_COPY, .dst = {2, 4, w, h}, .src = self, .src_origin = {0, 0}}};
    EACH_TARGET(f, dev) {
        up[0].src.format = down[1].src.format = f;
        up[0].src.domain = down[1].src.domain = dev ? SHR_MEMORY_DEVICE : SHR_MEMORY_CPU;
        ASSERT_EQ_LL(compare(up, 2, f, w, h, dev, K_COPY), 0);
        ASSERT_EQ_LL(compare(down, 2, f, w, h, dev, K_COPY), 0);
    }
    PASS();
}

TEST cpu_sources_inside_the_destination_see_earlier_commands(void) {
    /* A glyph whose coverage bytes lie in the destination buffer, after a fill that rewrote them (RGB565: the
     * X bytes of RGBX8888 are unspecified on write). */
    const int32_t w = 16, h = 8;
    uint16_t a[16 * 8], b[16 * 8];
    noise(a, sizeof(a));
    memcpy(b, a, sizeof(a));
    shr_surface sa = packed(a, SHR_FORMAT_RGB565, w, h), sb = packed(b, SHR_FORMAT_RGB565, w, h);
    shr_draw_cmd c[] = {{.kind = SHR_CMD_FILL, .dst = {0, 0, w, 2}, .color = SHR_RGB(200, 60, 7)},
                        {.kind = SHR_CMD_GLYPH, .dst = {4, 4, 12, 6}, .color = SHR_RGB(10, 250, 70),
                         .src = {a, 8, 2, sa.stride, 2 * sa.stride, SHR_FORMAT_A8, 0}}};
    ASSERT_EQ_LL(shr_software_execute(&sa, c, 2), SHR_OK);
    c[1].src.pixels = b;
    ASSERT_EQ_LL(gl.execute(gl.user, &sb, c, 2, 0), SHR_OK);
    int d = image_diff(SHR_FORMAT_RGB565, (uint8_t *)a, (uint8_t *)b, w, h, sa.stride);
    note(K_GLYPH, SHR_FORMAT_RGB565, d);
    ASSERT(d <= MAX_BLEND(SHR_FORMAT_RGB565));
    PASS();
}

TEST rotations_map_exactly(void) {
    enum { LW = 13, LH = 7 };
    static const shr_rotation rots[3] = {SHR_ROTATE_90_CW, SHR_ROTATE_180, SHR_ROTATE_90_CCW};
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
                shr_image m = as_image(s ? &dev : &src);
                shr_draw_cmd whole = {.kind = SHR_CMD_ROTATE, .dst = {0, 0, ow, oh}, .src = m, .rotation = rots[r]};
                shr_draw_cmd part[] = {
                    {.kind = SHR_CMD_ROTATE, .dst = {1, 2, ow - 2, oh - 1}, .src = m, .rotation = rots[r]},
                    {.kind = SHR_CMD_ROTATE, .dst = {ow - 1, 0, ow, 1}, .src = m, .rotation = rots[r]},
                };
                EACH_TARGET(f, d) {
                    ASSERT_EQ_LL(compare(&whole, 1, f, ow, oh, d, K_ROTATE), 0);
                    ASSERT_EQ_LL(compare(part, 2, f, ow, oh, d, K_ROTATE), 0);
                }
            }
        }
        device_drop(&dev);
        free(px);
    }
    PASS();
}

TEST groups_write_only_their_cache_clip(void) {
    const int32_t w = 40, h = 24;
    uint8_t cov[10 * 6];
    noise(cov, sizeof(cov));
    shr_image g = {cov, 10, 6, 10, sizeof(cov), SHR_FORMAT_A8, 0};
    shr_draw_cmd c[] = {
        {.kind = SHR_CMD_FILL, .dst = {0, 0, w, h}, .color = SHR_RGB(20, 20, 20)},
        {.kind = SHR_CMD_CACHE_BEGIN, .dst = {2, 3, 30, 20}, .key = {1, 2}, .cache_clip = {5, 4, 21, 11}},
        {.kind = SHR_CMD_FILL, .dst = {2, 3, 30, 20}, .color = SHR_RGB(200, 210, 220)},
        {.kind = SHR_CMD_GLYPH, .dst = {4, 4, 14, 10}, .color = SHR_RGB(255, 0, 0), .src = g},
        {.kind = SHR_CMD_FILL, .flags = SHR_GLYPH_DIM, .dst = {18, 8, 30, 20}, .color = SHR_RGB(0, 0, 255)},
        {.kind = SHR_CMD_CACHE_END},
        {.kind = SHR_CMD_CACHE_BEGIN, .dst = {30, 0, 40, 10}, .key = {3, 4}, .cache_clip = {32, 2, 32, 9}},
        {.kind = SHR_CMD_FILL, .dst = {30, 0, 40, 10}, .color = SHR_RGB(255, 255, 0)},
        {.kind = SHR_CMD_CACHE_END},
        {.kind = SHR_CMD_FILL, .dst = {0, 20, 40, 24}, .color = SHR_RGB(0, 128, 0)},
    };
    EACH_TARGET(f, dev) {
        ASSERT(compare(c, 10, f, w, h, dev, K_GROUP) <= MAX_BLEND(f));
        ASSERT(compare(&c[1], 5, f, w, h, dev, K_GROUP) <= MAX_BLEND(f)); /* the group alone: area = cache_clip */
        ASSERT_EQ_LL(compare(&c[6], 3, f, w, h, dev, K_GROUP), 0);     /* only an empty clip: nothing drawn */
    }
    PASS();
}

TEST texture_cache_hits_misses_and_evictions(void) {
    /* A budget for two 16x16 A8 textures beside the table: repeated and new glyphs hit, miss and evict. */
    shr_framebuffer_driver small;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 1500, &small), SHR_OK);
    enum { N = 6 };
    uint8_t cov[N][16 * 16];
    for (int i = 0; i < N; i++) noise(cov[i], sizeof(cov[i]));
    shr_draw_cmd c[3 * N];
    for (int i = 0; i < 3 * N; i++) {
        int k = i < N ? i : i < 2 * N ? i % 2 : N - 1 - i % N;
        c[i] = (shr_draw_cmd){.kind = SHR_CMD_GLYPH, .dst = {(i % 6) * 8, (i / 6) * 8, (i % 6) * 8 + 16, (i / 6) * 8 + 16},
                              .color = rnd_color(), .src = {cov[k], 16, 16, 16, 256, SHR_FORMAT_A8, 0}};
    }
    for (int round = 0; round < 2; round++)
        EACH_TARGET(f, dev) ASSERT(compare_with(&small, c, 3 * N, f, 56, 40, dev, K_GLYPH) <= MAX_BLEND(f));
    ASSERT_EQ_LL(shr_angle_driver_destroy(&small), SHR_OK);

    /* No cache, and a source wider than a texture: uploaded each time. */
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, &small), SHR_OK);
    EACH_TARGET(f, dev) ASSERT(compare_with(&small, c, N, f, 56, 40, dev, K_GLYPH) <= MAX_BLEND(f));
    ASSERT_EQ_LL(shr_angle_driver_destroy(&small), SHR_OK);
    int32_t wide = gl.caps.max_width + 1;
    uint8_t *row = malloc((size_t)wide);
    noise(row, (size_t)wide);
    shr_draw_cmd big[] = {
        {.kind = SHR_CMD_GLYPH, .dst = {3, 2, 20, 3}, .color = SHR_RGB(250, 250, 0),
         .src = {row, wide, 1, (size_t)wide, (size_t)wide, SHR_FORMAT_A8, 0}, .src_origin = {wide - 17, 0}},
        {.kind = SHR_CMD_GLYPH, .dst = {0, 0, 1, 4}, .color = SHR_RGB(0, 250, 250),
         .src = {row, 1, wide, 1, (size_t)wide, SHR_FORMAT_A8, 0}, .src_origin = {0, wide - 4}},
        /* One row: the stride is never used, even when it is no GL row length. */
        {.kind = SHR_CMD_GLYPH, .dst = {4, 3, 20, 4}, .color = SHR_RGB(250, 0, 250),
         .src = {row, 16, 1, (size_t)1 << 31, 16, SHR_FORMAT_A8, 0}},
    };
    EACH_TARGET(f, dev) ASSERT(compare(big, 3, f, 24, 4, dev, K_GLYPH) <= MAX_BLEND(f));
    free(row);
    PASS();
}

TEST scratch_textures_grow_in_either_direction(void) {
    shr_framebuffer_driver fresh;
    ASSERT_EQ_LL(shr_angle_driver_create(NULL, 0, &fresh), SHR_OK);
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

/* ---- validation: the software port's statuses ---- */

static void same_status(const shr_surface *dst, const shr_draw_cmd *c, size_t n, shr_status expected) {
    size_t len = dst->byte_length;
    uint8_t *before = malloc(len ? len : 1);
    memcpy(before, dst->pixels, len);
    ASSERT_EQ_LL(shr_software_execute(dst, c, n), expected);
    ASSERT_EQ_LL(gl.execute(gl.user, dst, c, n, 0), expected);
    ASSERT_EQ_LL(memcmp(before, dst->pixels, len), 0);
    free(before);
}

TEST invalid_batches_match_the_software_port(void) {
    enum { W = 16, H = 8 };
    static uint16_t px[W * H], other[W * H];
    shr_surface d = packed(px, SHR_FORMAT_RGB565, W, H);
    shr_surface o = packed(other, SHR_FORMAT_RGB565, W, H);
    uint8_t cov[16], rgba[16];
    shr_image a8 = {cov, 4, 4, 4, 16, SHR_FORMAT_A8, 0}, img = {rgba, 2, 2, 8, 16, SHR_FORMAT_RGBA8888, 0};
    shr_image src = as_image(&o), self = as_image(&d);
    shr_rect r = {0, 0, 4, 4};
    const shr_draw_cmd fill = {.kind = SHR_CMD_FILL, .dst = r}, end = {.kind = SHR_CMD_CACHE_END};
    const shr_draw_cmd begin = {.kind = SHR_CMD_CACHE_BEGIN, .dst = {0, 0, 8, 8}, .cache_clip = {0, 0, 8, 8}};

    same_status(&d, NULL, 1, SHR_E_INVALID_ARG);
    shr_surface bad = d;
    bad.stride = 1;
    ASSERT_EQ_LL(gl.execute(gl.user, &bad, &fill, 1, 0), SHR_E_INVALID_ARG);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {0, 0, W + 1, 1}}, 1, SHR_E_INVALID_ARG);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_FILL, .dst = {3, 0, 2, 1}}, 1, SHR_E_INVALID_ARG);
    same_status(&d, &(shr_draw_cmd){.kind = 99, .dst = r}, 1, SHR_E_INVALID_ARG);
    same_status(&d, (shr_draw_cmd[]){fill, end}, 2, SHR_E_INVALID_ARG);
    same_status(&d, (shr_draw_cmd[]){begin, begin, end, end}, 4, SHR_E_INVALID_ARG);
    same_status(&d, (shr_draw_cmd[]){begin, fill}, 2, SHR_E_INVALID_ARG);
    shr_draw_cmd b2 = begin;
    b2.dst.x1 = W + 1;
    same_status(&d, (shr_draw_cmd[]){b2, end}, 2, SHR_E_INVALID_ARG);
    b2 = begin, b2.cache_clip.x1 = 9;
    same_status(&d, (shr_draw_cmd[]){b2, end}, 2, SHR_E_INVALID_ARG);
    same_status(&d, (shr_draw_cmd[]){begin, {.kind = SHR_CMD_FILL, .dst = {0, 0, 9, 1}}, end}, 3, SHR_E_INVALID_ARG);
    same_status(&d, (shr_draw_cmd[]){begin, {.kind = SHR_CMD_COPY, .dst = r, .src = self, .src_origin = {8, 0}}, end}, 3,
                SHR_E_INVALID_ARG);
    same_status(&d, (shr_draw_cmd[]){begin, {.kind = SHR_CMD_ROTATE, .dst = r, .src = src, .rotation = SHR_ROTATE_180}, end},
                3, SHR_E_INVALID_ARG);

    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_GLYPH, .dst = r, .src = img}, 1, SHR_E_UNSUPPORTED);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_IMAGE, .dst = r, .src = a8}, 1, SHR_E_UNSUPPORTED);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_COPY, .dst = r, .src = a8}, 1, SHR_E_UNSUPPORTED);
    shr_image short_stride = a8;
    short_stride.stride = 3;
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_GLYPH, .dst = r, .src = short_stride}, 1, SHR_E_INVALID_ARG);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_GLYPH, .dst = r, .src = a8, .src_origin = {1, 0}}, 1, SHR_E_INVALID_ARG);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_GLYPH, .dst = r, .src = a8, .src_origin = {0, 1}}, 1, SHR_E_INVALID_ARG);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_GLYPH, .dst = r, .src = a8, .src_origin = {-1, 0}}, 1, SHR_E_INVALID_ARG);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_IMAGE, .dst = {0, 0, 2, 2}, .src = img, .src_origin = {0, -1}}, 1,
                SHR_E_INVALID_ARG);
    shr_image dev_src = src;
    dev_src.domain = SHR_MEMORY_DEVICE; /* not a surface of the driver */
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_COPY, .dst = r, .src = dev_src}, 1, SHR_E_UNSUPPORTED);

    /* COPY overlapping its destination with another stride or format. */
    shr_image shifted = self;
    shifted.height = 6, shifted.stride = d.stride + 2;
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_COPY, .dst = {0, 2, 6, 6}, .src = shifted}, 1, SHR_E_UNSUPPORTED);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_COPY, .dst = {2, 2, 2, 6}, .src = shifted}, 1, SHR_OK);

    shr_draw_cmd rot = {.kind = SHR_CMD_ROTATE, .dst = r, .src = src, .rotation = SHR_ROTATE_180};
    same_status(&d, &rot, 1, SHR_OK);
    for (int k = 0; k < 7; k++) {
        shr_draw_cmd c = rot;
        shr_status want = k == 6 ? SHR_OK : k < 3 ? SHR_E_INVALID_ARG : SHR_E_UNSUPPORTED;
        if (k == 0) c.rotation = SHR_ROTATE_NONE;
        if (k == 1) c.rotation = (shr_rotation)7;
        if (k == 2) c.rotation = SHR_ROTATE_90_CW; /* a 16x8 source needs an 8x16 output */
        if (k == 3) c.src = self;
        if (k == 4) c.src = (shr_image){cov, 16, 1, 16, 16, SHR_FORMAT_A8, 0};
        if (k == 5) c.src = dev_src;
        if (k == 6) c.src = self, c.dst = (shr_rect){2, 2, 2, 2};
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
    shr_image b4 = {blank, 4, 4, 4, 16, SHR_FORMAT_A8, 0}, none = {NULL, 0, 4, 0, 0, SHR_FORMAT_A8, 0};
    int32_t x0, x1;
    shr__glyph_footprint(4, 4, B | I, 3, &x0, &x1);
    same_status(&d, &(shr_draw_cmd){.kind = SHR_CMD_GLYPH, .flags = B | I, .dst = {0, 0, x1 - x0, 4}, .src = b4,
                                    .src_origin = {x0, 0}, .slant_axis = 3}, 1, SHR_OK);
    const shr_draw_cmd paint = {.kind = SHR_CMD_FILL, .dst = r, .color = SHR_RGB(200, 9, 9)};
    const struct {
        int32_t ox, oy, w, h;
        uint32_t flags;
        int32_t axis;
        shr_image src;
        shr_status want;
    } st[] = {
        {x0 - 1, 0, x1 - x0, 4, B | I, 3, b4, SHR_E_INVALID_ARG},
        {x0, 0, x1 - x0 + 1, 4, B | I, 3, b4, SHR_E_INVALID_ARG},
        {x0, 1, x1 - x0, 4, B | I, 3, b4, SHR_E_INVALID_ARG},
        {0, 0, 1, 1, B, 3, {blank, 1025, 1, 1025, 1025, SHR_FORMAT_A8, 0}, SHR_E_INVALID_ARG},
        {0, 0, 1, 1, B, 3, {blank, 1, 1025, 1, 1025, SHR_FORMAT_A8, 0}, SHR_E_INVALID_ARG},
        {0, 0, 5, 4, B, INT32_MIN, b4, SHR_OK},
        {0, 0, 1, 4, B, 3, none, SHR_OK},
        {-1, 0, 1, 4, I, 3, none, SHR_OK},
    };
    for (size_t k = 0; k < sizeof(st) / sizeof(st[0]); k++) {
        shr_draw_cmd c = {.kind = SHR_CMD_GLYPH, .flags = st[k].flags, .dst = {0, 0, st[k].w, st[k].h}, .src = st[k].src,
                          .src_origin = {st[k].ox, st[k].oy}, .slant_axis = st[k].axis};
        same_status(&d, (shr_draw_cmd[]){st[k].want == SHR_OK ? fill : paint, c}, 2, st[k].want);
    }
    for (int32_t axis = -4097; axis <= 4097; axis += 2 * 4097) { /* the footprint holds the origin: only the bound */
        shr__glyph_footprint(4, 4, I, axis, &x0, &x1);
        shr_draw_cmd c = {.kind = SHR_CMD_GLYPH, .flags = I, .dst = {0, 0, x1 - x0, 4}, .src = b4,
                          .src_origin = {x0, 0}, .slant_axis = axis};
        same_status(&d, (shr_draw_cmd[]){paint, c}, 2, SHR_E_INVALID_ARG);
    }
    shr_draw_cmd bad_img = {.kind = SHR_CMD_IMAGE, .flags = B | I, .dst = {0, 0, 2, 2}, .src = img, .src_origin = {-1, 0}};
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
    int32_t wide = gl.caps.max_width + 1;
    uint16_t *row = calloc((size_t)wide, 2);
    shr_surface huge = packed(row, SHR_FORMAT_RGB565, wide, 1);
    ASSERT_EQ_LL(gl.execute(gl.user, &huge, &fill, 0, 0), SHR_E_UNSUPPORTED);
    huge = packed(row, SHR_FORMAT_RGB565, 1, wide);
    ASSERT_EQ_LL(gl.execute(gl.user, &huge, &fill, 0, 0), SHR_E_UNSUPPORTED);
    free(row);

    shr_image self = as_image(&dv);
    shr_draw_cmd rot = {.kind = SHR_CMD_ROTATE, .dst = {0, 0, 4, 4}, .src = self, .rotation = SHR_ROTATE_180};
    ASSERT_EQ_LL(gl.execute(gl.user, &dv, &rot, 1, 0), SHR_E_UNSUPPORTED);
    rot.dst = (shr_rect){1, 1, 1, 1};
    ASSERT_EQ_LL(gl.execute(gl.user, &dv, &rot, 1, 0), SHR_OK);
    shr_draw_cmd group[] = {{.kind = SHR_CMD_CACHE_BEGIN, .dst = {0, 0, 8, 8}, .cache_clip = {0, 0, 8, 8}},
                            {.kind = SHR_CMD_COPY, .dst = {0, 0, 4, 4}, .src = self, .src_origin = {8, 0}},
                            {.kind = SHR_CMD_CACHE_END}};
    ASSERT_EQ_LL(gl.execute(gl.user, &dv, group, 3, 0), SHR_E_INVALID_ARG);
    /* A group may read another surface. */
    group[1].src = as_image(&cpu);
    ASSERT_EQ_LL(gl.execute(gl.user, &dv, group, 3, 0), SHR_OK);
    shr_image wrong = self;
    wrong.height = H - 1;
    shr_draw_cmd copy = {.kind = SHR_CMD_COPY, .dst = {0, 0, 4, 4}, .src = wrong};
    ASSERT_EQ_LL(gl.execute(gl.user, &cpu, &copy, 1, 0), SHR_E_UNSUPPORTED);
    ASSERT_EQ_LL(shr_angle_surface_destroy(&gl, &dv), SHR_OK);
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
    shr_text_style dim = {SHR_RGB(0x50, 0xFA, 0x7B), SHR_RGB(0x44, 0x47, 0x5A), SHR_STYLE_DIM | SHR_STYLE_BG | SHR_STYLE_UNDERLINE};
    ASSERT_EQ_LL(shr_pl_lyr_tilemap_set_text(text, 2, 1, "dim bg", 6, dim, NULL, 0, 0, &err), SHR_OK);

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
    ASSERT_EQ_LL(shr_software_driver_create(NULL, 1u << 20, &sw), SHR_OK);
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
    RUN_TEST(image_hashes_are_kept_per_batch);
    RUN_TEST(allocation_failures);
    RUN_TEST(fill_matches);
    RUN_TEST(many_quads_flush_in_batches);
    RUN_TEST(glyphs_match);
    RUN_TEST(styled_glyphs_match);
    RUN_TEST(styled_glyphs_match_at_the_axis_bounds);
    RUN_TEST(images_match);
    RUN_TEST(copies_convert_exactly);
    RUN_TEST(copy_scrolls_within_a_surface);
    RUN_TEST(cpu_sources_inside_the_destination_see_earlier_commands);
    RUN_TEST(rotations_map_exactly);
    RUN_TEST(groups_write_only_their_cache_clip);
    RUN_TEST(texture_cache_hits_misses_and_evictions);
    RUN_TEST(scratch_textures_grow_in_either_direction);
    RUN_TEST(errors_left_by_the_application_are_not_ours);
    RUN_TEST(invalid_batches_match_the_software_port);
    RUN_TEST(device_and_domain_rules);
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
    if (shr_angle_offscreen_create(64, 64, &off) != SHR_OK || shr_angle_driver_create(NULL, 1u << 20, &gl) != SHR_OK)
        return EXIT_FAILURE;
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    RUN_SUITE(driver);
    shr_angle_driver_destroy(&gl);
    shr_angle_offscreen_destroy(off);
    printf("max difference to the software port (channel units of the destination): RGB565 RGBX8888\n");
    for (int k = 0; k < KINDS; k++) printf("  %-9s %d %d\n", kind_names[k], max_diff[k][0], max_diff[k][1]);
    GREATEST_MAIN_END();
}
