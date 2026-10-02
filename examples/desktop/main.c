/* SDL3 window over the software driver (SDL_Renderer streaming texture) and the ANGLE driver (SDL's GL ES
 * context, ANGLE loaded by SDL): the render test scenes, load modes that change the screen every frame, and
 * per-frame render and present times. `--help` lists the options and keys. */
#include "render.h"

#include <SDL3/SDL.h>

#ifdef SHR_DESKTOP_ANGLE
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <shiroko/port_angle.h>
#endif

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define RING 1000
#define SPRITE 96
#define MAX_SPRITES 500

enum { SOFTWARE, ANGLE };
enum { SCROLL, CHURN, RESTYLE, BLINK, IMAGES, LOADS };
static const char *const load_names[LOADS] = {"scroll", "churn", "restyle", "blink", "images"};

typedef struct item {
    const scene *sc;
    int page;
    char name[64];
} item;

typedef struct sprite {
    shr_lyr *l;
    int32_t x, y, dx, dy;
} sprite;

typedef struct app {
    int driver;
    char label[32];
    const char *font_dir;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    SDL_GLContext gl;
    uint32_t fbo;
    shr_framebuffer_driver drv;
    bool has_drv;
    stage s;
    shr_surface out, comp;
    uint32_t flags;
    long presents;
    char log[256];
    item *items;
    size_t nitems, cur;
    int load; /* -1: the scene items[cur] */
    long n[LOADS];
    int zoom;      /* 0: scenes as large as fits, load modes at 1 */
    int vsync_opt; /* -1: on for scenes, off for load modes */
    bool vsync, redraw;
    int32_t width, height, rows, cols;
    int pw, ph;     /* drawable size the scene was opened for */
    char what[128]; /* the open scene, for its statistics */
    shr_lyr *grid;
    sprite sprites[MAX_SPRITES];
    uint64_t tick;
    uint32_t rng;
    double first, render[RING], present[RING];
    long frames;
} app;

static double ms(uint64_t ns) { return (double)ns / 1e6; }

/* ===== Output and context ===== */

static shr_status out_acquire(void *user, shr_surface *s) {
    *s = ((app *)user)->out;
    return SHR_OK;
}

static shr_status out_present(void *user, const shr_surface *s, uint64_t id) {
    (void)s, (void)id;
    ((app *)user)->presents++;
    return SHR_OK;
}

static void out_discard(void *user, const shr_surface *s) { (void)user, (void)s; }

static uint64_t scene_clock(void *user) { return ((app *)user)->s.now; }

static void on_log(void *user, shr_status st, const char *msg) {
    app *a = user;
    snprintf(a->log, sizeof(a->log), "%s (%s)", msg, shr_status_name(st));
}

static shr_status open_package(void *user, const char *name, shr_asset_source *out) {
    app *a = user;
    return stage_open_package(&a->s, a->flags, a->font_dir, name, out);
}

static shr_status surface_create(app *a, int32_t w, int32_t h, shr_pixel_format f, shr_surface *out) {
#ifdef SHR_DESKTOP_ANGLE
    if (a->driver == ANGLE) return shr_angle_surface_create(&a->drv, w, h, f, out);
#endif
    (void)a;
    size_t row;
    shr_status st = shr_format_row_bytes(f, w, &row);
    void *px = st == SHR_OK ? calloc((size_t)h, row) : NULL;
    if (!px) return st == SHR_OK ? SHR_E_NO_MEMORY : st;
    *out = (shr_surface){px, w, h, row, row * (size_t)h, f, 0, SHR_MEMORY_CPU, 0};
    return SHR_OK;
}

static void surface_destroy(app *a, shr_surface *s) {
    if (!s->pixels) return;
#ifdef SHR_DESKTOP_ANGLE
    if (a->driver == ANGLE) {
        shr_angle_surface_destroy(&a->drv, s);
        return;
    }
#endif
    (void)a;
    free(s->pixels);
    *s = (shr_surface){0};
}

static void finish(const app *a) {
#ifdef SHR_DESKTOP_ANGLE
    if (a->driver == ANGLE) glFinish();
#endif
    (void)a;
}

/* Submits (or only pumps) until nothing is due now; failed frames and unexpected resource failures fail the scene. */
static void frame(app *a, bool submit) {
    stage *s = &a->s;
    if (submit && !stage_ok(s, shr_submit(s->ctx), "submit")) return;
    shr_deadline dl = {SHR_DEADLINE_NOW, 0};
    for (int i = 0; i < 100000 && dl.kind == SHR_DEADLINE_NOW; i++) {
        shr_pump(s->ctx);
        shr_next_deadline(s->ctx, &dl);
    }
    if (dl.kind == SHR_DEADLINE_NOW) stage_ok(s, SHR_E_TIMEOUT, "frame does not settle");
    shr_event ev;
    while (shr_poll_event(s->ctx, &ev) == SHR_OK) {
        bool resource = ev.kind == SHR_EVENT_RESOURCE_FAILED || ev.kind == SHR_EVENT_OVERFLOW;
        if (ev.kind == SHR_EVENT_PRESENT_FAILED) stage_ok(s, ev.status ? ev.status : SHR_E_STATE, "frame failed");
        if (resource && !(a->flags & RESOURCE_FAILS)) stage_ok(s, ev.status ? ev.status : SHR_E_STATE, "resource failed");
    }
}

/* ===== Load modes ===== */

static const struct {
    const char *text;
    uint32_t span;
} glyphs[] = {{"a", 1},  {"Z", 1},  {"#", 1},  {"0", 1},   {"~", 1},  {"─", 1},  {"e\xCC\x81", 1},
              {"|", 1},  {"가", 2}, {"한", 2}, {"글", 2}, {"😀", 2}, {"❤️", 2}, {"👍", 2}};
static const shr_color fgs[] = {SHR_RGB(0xF8, 0xF8, 0xF2), SHR_RGB(0xFF, 0x79, 0xC6), SHR_RGB(0x50, 0xFA, 0x7B),
                                SHR_RGB(0xF1, 0xFA, 0x8C), SHR_RGB(0x8B, 0xE9, 0xFD), SHR_RGB(0xBD, 0x93, 0xF9)};
static const shr_color bgs[] = {SHR_RGB(0x28, 0x2A, 0x36), SHR_RGB(0x44, 0x47, 0x5A), SHR_RGB(0x62, 0x72, 0xA4)};
static const uint32_t line_styles[] = {0, SHR_STYLE_BOLD, 0, SHR_STYLE_ITALIC, SHR_STYLE_UNDERLINE, 0,
                                       SHR_STYLE_BOLD | SHR_STYLE_ITALIC};

static uint32_t mix(uint32_t x) {
    x ^= x >> 16, x *= 0x7FEB352Du, x ^= x >> 15, x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}

static uint32_t next_random(app *a) {
    a->rng ^= a->rng << 13, a->rng ^= a->rng >> 17, a->rng ^= a->rng << 5;
    return a->rng;
}

/* One cell as a VT engine hands it over; returns its span. */
static int32_t paint(app *a, int32_t row, int32_t col, uint32_t h, uint32_t flags) {
    uint32_t g = h % N(glyphs), span = glyphs[g].span;
    const char *t = glyphs[g].text;
    if (col + (int32_t)span > a->cols) t = " ", span = 1;
    shr_text_style st = {fgs[(h >> 8) % N(fgs)], bgs[(h >> 12) % N(bgs)], flags | ((h >> 16) % 4 ? 0 : SHR_STYLE_BG)};
    stage_ok(&a->s, shr_pl_lyr_tilemap_set_cell(a->grid, row, col, t, strlen(t), span, st), "set_cell");
    return (int32_t)span;
}

/* Text line `line` into `row`; with SHR_STYLE_BLINK every other cell blinks. */
static void paint_row(app *a, int32_t row, uint32_t line, uint32_t flags) {
    for (int32_t c = 0; c < a->cols;) {
        uint32_t f = (row + c) & 1 ? flags & ~(uint32_t)SHR_STYLE_BLINK : flags;
        c += paint(a, row, c, mix(line * 0x9E3779B1u + (uint32_t)c), f);
    }
}

static void build_sprites(app *a) {
    static uint8_t px[SPRITE * SPRITE * 4];
    shr_pl_res_image *img[4];
    for (int k = 0; k < 4; k++) {
        for (int y = 0; y < SPRITE; y++)
            for (int x = 0; x < SPRITE; x++) {
                int dx = 2 * x - SPRITE + 1, dy = 2 * y - SPRITE + 1, d2 = dx * dx + dy * dy, r2 = SPRITE * SPRITE;
                uint8_t *p = px + (y * SPRITE + x) * 4;
                p[0] = (uint8_t)(k & 1 ? 255 - x * 2 : x * 2), p[1] = (uint8_t)(y * 2), p[2] = (uint8_t)(k & 2 ? 255 : 96);
                p[3] = (uint8_t)(d2 >= r2 ? 0 : 255 - 255 * d2 / r2);
            }
        img[k] = stage_image(&a->s, SPRITE, SPRITE, px);
    }
    int32_t sx = a->width > SPRITE ? a->width - SPRITE : 1, sy = a->height > SPRITE ? a->height - SPRITE : 1;
    for (long i = 0; i < a->n[IMAGES] && a->s.st == SHR_OK; i++) {
        sprite *p = &a->sprites[i];
        p->x = (int32_t)(next_random(a) % (uint32_t)sx), p->y = (int32_t)(next_random(a) % (uint32_t)sy);
        p->dx = (int32_t)(next_random(a) % 7) - 3, p->dy = (int32_t)(next_random(a) % 7) - 3;
        p->dx += !p->dx, p->dy += !p->dy;
        p->l = stage_layer(&a->s, 1, (shr_rect){p->x, p->y, p->x + SPRITE, p->y + SPRITE});
        if (!p->l) break;
        shr_lyr_cmd_begin(p->l);
        shr_rect src = {0, 0, SPRITE, SPRITE};
        stage_ok(&a->s, shr_lyr_cmd_image(p->l, img[i % 4], src, (shr_point){0, 0}), "cmd_image");
        stage_ok(&a->s, shr_lyr_cmd_commit(p->l), "commit");
    }
}

static void load_build(app *a) {
    shr_color bg = SHR_RGB(0x1E, 0x1F, 0x29);
    a->rows = a->height / CH, a->cols = a->width / CW, a->rng = 0x9E3779B9u;
    a->grid = stage_grid(&a->s, 0, (shr_rect){0, 0, a->width, a->height}, &bg);
    for (int32_t r = 0; r < a->rows && a->grid; r++) paint_row(a, r, (uint32_t)r, a->load == BLINK ? SHR_STYLE_BLINK : 0);
    if (a->load == IMAGES) build_sprites(a);
}

/* Changes the screen for the next frame; false when the frame comes from the clock alone. */
static bool load_step(app *a) {
    a->tick++;
    switch (a->load) {
    case SCROLL:
        for (int32_t r = 0; r < a->rows; r++) {
            uint32_t line = (uint32_t)(r + (int32_t)a->tick);
            paint_row(a, r, line, line_styles[line % N(line_styles)]);
        }
        break;
    case CHURN:
        for (long i = 0; i < a->n[CHURN]; i++) {
            uint32_t h = next_random(a);
            paint(a, (int32_t)(h % (uint32_t)a->rows), (int32_t)((h >> 8) % (uint32_t)a->cols), mix(h), 0);
        }
        break;
    case RESTYLE:
        for (int32_t r = 0; r < a->rows; r++)
            paint_row(a, r, (uint32_t)r, a->tick & 1 ? SHR_STYLE_BOLD | SHR_STYLE_ITALIC | SHR_STYLE_UNDERLINE : 0);
        break;
    case BLINK:
        a->s.now += BLINK_NS;
        return false;
    default:
        for (long i = 0; i < a->n[IMAGES] && a->sprites[i].l; i++) {
            sprite *p = &a->sprites[i];
            if (p->x + p->dx < 0 || p->x + p->dx > a->width - SPRITE) p->dx = -p->dx;
            if (p->y + p->dy < 0 || p->y + p->dy > a->height - SPRITE) p->dy = -p->dy;
            p->x += p->dx, p->y += p->dy;
            stage_ok(&a->s, shr_lyr_set_rect(p->l, (shr_rect){p->x, p->y, p->x + SPRITE, p->y + SPRITE}), "set_rect");
        }
    }
    return true;
}

/* ===== Presentation ===== */

static void set_vsync(app *a) {
    a->vsync = a->vsync_opt >= 0 ? a->vsync_opt : a->load < 0;
    bool ok = a->driver == SOFTWARE ? SDL_SetRenderVSync(a->ren, a->vsync) : SDL_GL_SetSwapInterval(a->vsync);
    if (!ok) printf("vsync %s: %s\n", a->vsync ? "on" : "off", SDL_GetError());
}

/* Integer zoom and the top-left corner of the picture in a w x h drawable. */
static int place(const app *a, int w, int h, int *x, int *y) {
    int ow = SDL_max(1, a->out.width), oh = SDL_max(1, a->out.height), z = a->zoom;
    if (!z) z = a->load >= 0 ? 1 : SDL_max(1, SDL_min(w / ow, h / oh));
    *x = SDL_max(0, (w - ow * z) / 2), *y = SDL_max(0, (h - oh * z) / 2);
    return z;
}

static void present(app *a) {
    int w = 0, h = 0, x, y, z;
    if (a->driver == SOFTWARE) {
        SDL_UpdateTexture(a->tex, NULL, a->out.pixels, (int)a->out.stride);
        SDL_GetRenderOutputSize(a->ren, &w, &h);
        z = place(a, w, h, &x, &y);
        SDL_SetRenderDrawColor(a->ren, 0x10, 0x10, 0x14, 0xFF);
        SDL_RenderClear(a->ren);
        SDL_FRect dst = {(float)x, (float)y, (float)(a->out.width * z), (float)(a->out.height * z)};
        SDL_RenderTexture(a->ren, a->tex, NULL, &dst);
        SDL_RenderPresent(a->ren);
        return;
    }
#ifdef SHR_DESKTOP_ANGLE
    /* The EGL surface follows a resize a frame late; flip with its own height. */
    EGLint ew = 0, eh = 0;
    eglQuerySurface(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW), EGL_WIDTH, &ew);
    eglQuerySurface(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW), EGL_HEIGHT, &eh);
    w = ew, h = eh;
    z = place(a, w, h, &x, &y);
    uint32_t tex = 0;
    stage_ok(&a->s, shr_angle_surface_texture(&a->drv, &a->out, &tex), "surface_texture");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, a->fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, w, h);
    glClearColor(0x10 / 255.0f, 0x10 / 255.0f, 0x14 / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* Surface row 0 is the top, default framebuffer row 0 the bottom. */
    glBlitFramebuffer(0, 0, a->out.width, a->out.height, x, h - y, x + a->out.width * z, h - y - a->out.height * z,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (glGetError() != GL_NO_ERROR) stage_ok(&a->s, SHR_E_DEVICE, "present blit");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    SDL_GL_SwapWindow(a->win);
#endif
}

static void close_driver(app *a) {
    if (a->has_drv) {
#ifdef SHR_DESKTOP_ANGLE
        if (a->driver == ANGLE) {
            glDeleteFramebuffers(1, &a->fbo);
            shr_angle_driver_destroy(&a->drv);
        }
#endif
        if (a->driver == SOFTWARE) shr_software_driver_destroy(&a->drv);
        a->has_drv = false;
    }
    if (a->gl) SDL_GL_DestroyContext(a->gl);
    if (a->ren) SDL_DestroyRenderer(a->ren);
    if (a->win) SDL_DestroyWindow(a->win);
    a->gl = NULL, a->ren = NULL, a->win = NULL;
}

/* A new window for `driver`: SDL_Renderer and a GL context cannot share one. */
static shr_status open_driver(app *a, int driver, int w, int h) {
#ifndef SHR_DESKTOP_ANGLE
    if (driver == ANGLE) {
        printf("ANGLE mode needs SHIROKO_PORT_ANGLE with a dynamic ANGLE (preset desktop)\n");
        return SHR_E_UNSUPPORTED;
    }
#endif
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (driver == ANGLE ? SDL_WINDOW_OPENGL : 0);
    a->driver = driver;
    a->win = SDL_CreateWindow("shiroko", w, h, flags);
    if (!a->win) {
        printf("window: %s\n", SDL_GetError());
        return SHR_E_DEVICE;
    }
    shr_status st = SHR_E_DEVICE;
    if (driver == SOFTWARE) {
        int pw, ph, rw, rh;
        SDL_GetWindowSizeInPixels(a->win, &pw, &ph);
        /* Room for the rows of two screens with slack: rows toggled between two states still hit. */
        uint64_t cache = 3ull * (uint64_t)pw * (uint64_t)ph * (SHR_PIXEL_FORMAT == SHR_FORMAT_RGB565 ? 2 : 4);
        a->ren = SDL_CreateRenderer(a->win, NULL);
        st = a->ren ? shr_software_driver_create(NULL, cache, 256, &a->drv) : SHR_E_DEVICE;
        if (st == SHR_OK) {
            SDL_GetRenderOutputSize(a->ren, &rw, &rh);
            printf("software driver, presented by SDL_Renderer %s (output %dx%d px, window %dx%d px, cache %.1f MiB)\n",
                   SDL_GetRendererName(a->ren), rw, rh, pw, ph, (double)cache / (1 << 20));
            snprintf(a->label, sizeof(a->label), "software");
        }
    }
#ifdef SHR_DESKTOP_ANGLE
    else {
        a->gl = SDL_GL_CreateContext(a->win);
        st = a->gl && SDL_GL_MakeCurrent(a->win, a->gl) ? shr_angle_driver_create(NULL, 0, 256, &a->drv) : SHR_E_DEVICE;
        if (st == SHR_OK) {
            const char *r = (const char *)glGetString(GL_RENDERER);
            snprintf(a->label, sizeof(a->label), "angle/%s",
                     strstr(r, "Metal") ? "metal" : strstr(r, "Vulkan") ? "vulkan" : strstr(r, "OpenGL") ? "opengl" : "?");
            /* The driver binds to eglGetCurrentContext(): it must be the context SDL created. */
            printf("ANGLE driver on %s (EGL context %p, SDL context %p)\n", r, (void *)eglGetCurrentContext(),
                   (void *)a->gl);
            glGenFramebuffers(1, &a->fbo);
        }
    }
#endif
    if (st != SHR_OK) {
        printf("%s driver: %s (%s)\n", driver == ANGLE ? "ANGLE" : "software", shr_status_name(st), SDL_GetError());
        close_driver(a);
        return st;
    }
    a->has_drv = true;
    return SHR_OK;
}

/* ===== Scenes ===== */

static const char *scene_name(const app *a) { return a->load < 0 ? a->items[a->cur].name : load_names[a->load]; }

static void scene_close(app *a) {
    stage *s = &a->s;
    for (size_t i = 0; i < s->nlayers; i++) shr_lyr_destroy(s->layers[i]);
    for (size_t i = 0; i < s->nimages; i++) shr_pl_res_image_release(s->images[i]);
    s->nlayers = s->nimages = 0;
    if (s->ctx) {
        shr_begin_shutdown(s->ctx);
        bool done = false;
        for (int i = 0; i < 64 && !done; i++) {
            shr_pump(s->ctx);
            if (s->font && shr_pl_res_bitmap_font_destroy(s->font) == SHR_OK) s->font = NULL;
            done = !s->font && shr_destroy(s->ctx) == SHR_OK;
        }
        if (!done) printf("%s: context did not shut down\n", scene_name(a));
        s->ctx = NULL;
    }
    surface_destroy(a, &a->comp);
    surface_destroy(a, &a->out);
    if (a->tex) SDL_DestroyTexture(a->tex);
    a->tex = NULL;
    memset(a->sprites, 0, sizeof(a->sprites));
}

/* Builds the scene and draws its first frame (the update step included), timed like test_render. */
static shr_status scene_open(app *a) {
    stage *s = &a->s;
    memset(s, 0, sizeof(*s));
    a->presents = a->frames = 0, a->tick = 0, a->first = 0, a->log[0] = 0;
    const scene *sc = a->load < 0 ? a->items[a->cur].sc : NULL;
    int pw = 0, ph = 0, z = a->zoom ? a->zoom : 1;
    SDL_GetWindowSizeInPixels(a->win, &pw, &ph);
    a->pw = pw, a->ph = ph;
    a->width = sc ? sc->w : SDL_max(1, pw / z / CW) * CW, a->height = sc ? sc->h : SDL_max(1, ph / z / CH) * CH;
    shr_rotation rot = sc ? sc->rotation : SHR_ROTATE_NONE;
    shr_pixel_format of = sc && sc->output_format ? sc->output_format : SHR_PIXEL_FORMAT;
    bool quarter = rot == SHR_ROTATE_90_CW || rot == SHR_ROTATE_90_CCW;
    bool composing = rot || of != SHR_PIXEL_FORMAT || (sc && (sc->screen_flags & SHR_SCREEN_COMPOSITION));
    a->flags = sc ? sc->flags : 0;
    s->page = sc ? a->items[a->cur].page : 0, s->now = sc ? sc->now_ns : 0, s->hold = (a->flags & FONTS_ASYNC) != 0;
    set_vsync(a);
    int32_t ow = quarter ? a->height : a->width, oh = quarter ? a->width : a->height;
    int n = snprintf(a->what, sizeof(a->what), "%s", scene_name(a));
    if (a->load == CHURN || a->load == IMAGES)
        n += snprintf(a->what + n, sizeof(a->what) - (size_t)n, " (N %ld)", a->n[a->load]);
    n += snprintf(a->what + n, sizeof(a->what) - (size_t)n, ": %dx%d px", ow, oh);
    if (!sc) snprintf(a->what + n, sizeof(a->what) - (size_t)n, " (%dx%d cells)", a->width / CW, a->height / CH);
    if (!stage_ok(s, surface_create(a, ow, oh, of, &a->out), "output") ||
        (composing && !stage_ok(s, surface_create(a, a->width, a->height, SHR_PIXEL_FORMAT, &a->comp), "composition")))
        return s->st;
    if (a->driver == SOFTWARE) {
        a->tex = SDL_CreateTexture(a->ren, of == SHR_FORMAT_RGB565 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_RGBX32,
                                   SDL_TEXTUREACCESS_STREAMING, ow, oh);
        if (!a->tex || !SDL_SetTextureScaleMode(a->tex, SDL_SCALEMODE_NEAREST))
            return stage_ok(s, SHR_E_DEVICE, "texture"), s->st;
    }
    shr_output output;
    shr_output_init(&output);
    output.user = a, output.acquire = out_acquire, output.present = out_present, output.discard = out_discard;
    output.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | (a->flags & PRESERVE_NONE ? 0 : SHR_OUTPUT_PRESERVES_CONTENT);
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.user = a, cd.now_ns = scene_clock, cd.log = on_log;
    cd.driver = &a->drv, cd.output = &output;
    cd.blink = (shr_blink_profile){BLINK_NS, 0, true, SHR_BLINK_RESTART_NONE};
    cd.max_commands = 1u << 22, cd.page_cache_bytes = 64u << 20, cd.image_bytes = 64u << 20;
    cd.io_retry_ns = cd.io_timeout_ns = 0;
    if (!stage_ok(s, shr_create(&cd, &s->ctx), "create")) return s->st;
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = a->width, sd.height = a->height, sd.rotation = rot, sd.output_format = of;
    sd.flags = sc ? sc->screen_flags : 0, sd.composition = composing ? &a->comp : NULL;
    sd.clear = SHR_RGB(0x1E, 0x1F, 0x29);
    if (!stage_ok(s, shr_screen_configure(s->ctx, &sd), "screen_configure")) return s->st;
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.user = a, fd.open = open_package, fd.locale = "ko";
    if (!stage_ok(s, shr_pl_res_bitmap_font_create(s->ctx, &fd, &s->font), "font_create")) return s->st;
    if (sc)
        sc->build(s);
    else
        load_build(a);
    if (s->st != SHR_OK) return s->st;
    uint64_t t0 = SDL_GetTicksNS();
    frame(a, true);
    if (sc && sc->update && s->st == SHR_OK && sc->update(s) == SHR_OK) frame(a, true);
    finish(a);
    a->first = ms(SDL_GetTicksNS() - t0);
    if (s->st == SHR_OK) present(a);
    return s->st;
}

/* ===== Timing ===== */

static int cmp_double(const void *x, const void *y) {
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

/* p50, p95, p99 and max of the last `window` entries. */
static void percentiles(const double *ring, long frames, long window, double out[4]) {
    static double v[RING];
    size_t n = (size_t)SDL_min(frames, window);
    for (size_t i = 0; i < n; i++) v[i] = ring[(size_t)(frames - 1 - (long)i) % RING];
    SDL_qsort(v, n, sizeof(double), cmp_double);
    static const size_t q[3] = {50, 95, 99};
    for (int k = 0; k < 3; k++) out[k] = n ? v[(n * q[k] + 99) / 100 - 1] : 0;
    out[3] = n ? v[n - 1] : 0;
}

static void print_stats(const app *a) {
    printf("[%s] %s, vsync %s%s; first %.3f ms, %ld frames, %ld presented\n", a->label, a->what, a->vsync ? "on" : "off",
           a->load < 0 && !a->redraw ? ", no redraw" : "", a->first, a->frames, a->presents);
    const char *names[2] = {"render ", "present"};
    const double *rings[2] = {a->render, a->present};
    for (int k = 0; k < 2; k++) {
        double p[2][4];
        percentiles(rings[k], a->frames, 100, p[0]);
        percentiles(rings[k], a->frames, 1000, p[1]);
        printf("  %s ms p50/p95/p99/max  last 100: %.3f/%.3f/%.3f/%.3f  last 1000: %.3f/%.3f/%.3f/%.3f\n", names[k],
               p[0][0], p[0][1], p[0][2], p[0][3], p[1][0], p[1][1], p[1][2], p[1][3]);
    }
    fflush(stdout);
}

static void update_title(const app *a) {
    double r[4], p[4];
    percentiles(a->render, a->frames, 100, r);
    percentiles(a->present, a->frames, 100, p);
    char title[256];
    if (a->s.st != SHR_OK)
        snprintf(title, sizeof(title), "shiroko %s · %s · failed: %s", a->label, scene_name(a), a->s.what);
    else
        snprintf(title, sizeof(title), "shiroko %s · %s · %dx%d · first %.2f · render p50 %.2f p99 %.2f · present p50 %.2f ms%s",
                 a->label, scene_name(a), a->out.width, a->out.height, a->first, r[0], r[2], p[0],
                 a->vsync ? " · vsync" : "");
    SDL_SetWindowTitle(a->win, title);
}

/* Renders, presents and records one frame. */
static void tick(app *a) {
    stage *s = &a->s;
    uint64_t t0 = SDL_GetTicksNS();
    bool submit = a->load >= 0 ? load_step(a) : a->redraw && stage_ok(s, shr_request_redraw(s->ctx), "request_redraw");
    frame(a, submit);
    finish(a);
    uint64_t t1 = SDL_GetTicksNS();
    present(a);
    uint64_t t2 = SDL_GetTicksNS();
    a->render[a->frames % RING] = ms(t1 - t0), a->present[a->frames % RING] = ms(t2 - t1);
    a->frames++;
}

/* Ends the statistics of the current settings. */
static void segment(app *a) {
    if (a->frames) print_stats(a);
    a->frames = a->presents = 0;
}

static void reopen(app *a) {
    segment(a);
    scene_close(a);
    if (scene_open(a) != SHR_OK) printf("%s: %s%s%s\n", scene_name(a), a->s.what, a->log[0] ? "; last log: " : "", a->log);
}

static void toggle(app *a) {
    int w, h, x, y;
    SDL_GetWindowSize(a->win, &w, &h);
    SDL_GetWindowPosition(a->win, &x, &y);
    segment(a);
    scene_close(a);
    int was = a->driver;
    close_driver(a);
    if (open_driver(a, !was, w, h) != SHR_OK && open_driver(a, was, w, h) != SHR_OK) exit(EXIT_FAILURE);
    SDL_SetWindowPosition(a->win, x, y);
    reopen(a);
}

/* ===== Main ===== */

static void items_build(app *a) {
    size_t cap = 0;
    for (size_t i = 0; i < scene_count; i++) {
        int pages = scenes[i].pages ? scenes[i].pages() : 1;
        for (int p = 1; p <= pages; p++) {
            if (a->nitems == cap) {
                item *grown = realloc(a->items, (cap = cap ? cap * 2 : 128) * sizeof(item));
                if (!grown) abort();
                a->items = grown;
            }
            item *it = &a->items[a->nitems++];
            *it = (item){.sc = &scenes[i], .page = p};
            snprintf(it->name, sizeof(it->name), scenes[i].pages ? "%s-%d" : "%s", scenes[i].name, p);
        }
    }
}

#ifdef SHR_DESKTOP_ANGLE
/* Before SDL_Init: SDL loads the ANGLE libraries the executable links (one ANGLE instance, so the driver sees
 * SDL's context) and ANGLE takes its backend from ANGLE_DEFAULT_PLATFORM. */
static void angle_setup(void) {
    static const char *const names[][2] = {{"metal", "metal"}, {"opengl", "gl"}, {"vulkan", "vulkan"}, {"d3d11", "d3d11"}};
    const char *b = SDL_getenv_unsafe("SHIROKO_ANGLE_BACKEND");
#ifdef __APPLE__
    if (!b) b = "metal"; /* ANGLE's own default there is OpenGL */
#endif
    for (size_t i = 0; b && i < N(names); i++)
        if (!strcmp(b, names[i][0])) SDL_setenv_unsafe("ANGLE_DEFAULT_PLATFORM", names[i][1], 1);
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_SetHint(SDL_HINT_EGL_LIBRARY, ANGLE_EGL_LIBRARY);
    SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, ANGLE_GLES_LIBRARY);
}
#endif

static const char usage[] =
    "usage: shiroko_desktop [--driver software|angle] [--scene NAME | --load scroll|churn|restyle|blink|images]\n"
    "         [--n N] [--zoom Z] [--vsync on|off] [--redraw on|off] [--size WxH] [--fonts DIR]\n"
    "         [--frames N [--quit]] [--toggle-every K] [--print N] [--list]\n"
    "  --frames N: print the statistics after N frames (--quit: then exit); --toggle-every K: switch drivers every\n"
    "  K frames; --print N: statistics every N frames of a scene; --n: cells per frame (churn) or image layers.\n"
    "keys: Tab driver, Left/Right scene, 1-5 load mode, Up/Down N x2 /2, +/- zoom (0 = fit), V vsync,\n"
    "  R redraw scenes every frame, Space statistics, Esc quit\n"
    "Render time: the scene's changes, submit and pump until the frame is presented, plus glFinish (ANGLE).\n"
    "Present time: texture upload and SDL_RenderPresent, or blit and SDL_GL_SwapWindow.\n";

static bool on(const char *v) { return !strcmp(v, "on") || !strcmp(v, "1"); }

static bool key(app *a, SDL_Keycode k) {
    long lim = a->load == IMAGES ? MAX_SPRITES : (long)a->rows * a->cols;
    switch (k) {
    case SDLK_ESCAPE:
    case SDLK_Q: return false;
    case SDLK_TAB: toggle(a); break;
    case SDLK_LEFT:
    case SDLK_RIGHT:
        segment(a);
        if (a->load < 0) a->cur = (a->cur + (k == SDLK_RIGHT ? 1 : a->nitems - 1)) % a->nitems;
        a->load = -1;
        reopen(a);
        break;
    case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: case SDLK_5:
        segment(a);
        a->load = (int)(k - SDLK_1);
        reopen(a);
        break;
    case SDLK_UP:
    case SDLK_DOWN:
        if (a->load != CHURN && a->load != IMAGES) break;
        segment(a);
        a->n[a->load] = SDL_clamp(k == SDLK_UP ? a->n[a->load] * 2 : a->n[a->load] / 2, 1, lim);
        if (a->load == IMAGES) reopen(a);
        printf("%s N = %ld\n", load_names[a->load], a->n[a->load]);
        break;
    case SDLK_EQUALS:
    case SDLK_KP_PLUS:
    case SDLK_MINUS:
    case SDLK_KP_MINUS: {
        int w, h, x, y;
        SDL_GetWindowSizeInPixels(a->win, &w, &h);
        int z = place(a, w, h, &x, &y);
        segment(a);
        a->zoom = k == SDLK_EQUALS || k == SDLK_KP_PLUS ? z + 1 : a->zoom ? a->zoom - 1 : SDL_max(0, z - 1);
        printf("zoom %d\n", a->zoom);
        if (a->load >= 0) reopen(a);
        break;
    }
    case SDLK_V:
        segment(a);
        a->vsync_opt = !a->vsync;
        set_vsync(a);
        break;
    case SDLK_R:
        segment(a);
        a->redraw = !a->redraw;
        break;
    case SDLK_SPACE: print_stats(a); break;
    default: break;
    }
    return true;
}

int main(int argc, char **argv) {
    static app a;
    a.font_dir = SHR_FONT_DIR, a.load = -1, a.vsync_opt = -1, a.redraw = true;
    a.n[CHURN] = 1000, a.n[IMAGES] = 16;
    int driver = SOFTWARE, ww = 1280, wh = 720;
    long frames = 0, toggle_every = 0, print_every = 1000;
    bool quit = false, list = false;
    const char *scene_arg = NULL;
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        bool used = v != NULL;
        if (!strcmp(o, "--quit")) quit = true, used = false;
        else if (!strcmp(o, "--list")) list = true, used = false;
        else if (!v) used = false, o = "--help";
        else if (!strcmp(o, "--driver")) driver = !strcmp(v, "angle") ? ANGLE : SOFTWARE;
        else if (!strcmp(o, "--scene")) scene_arg = v;
        else if (!strcmp(o, "--load")) {
            for (int k = 0; k < LOADS; k++)
                if (!strcmp(v, load_names[k])) a.load = k;
            if (a.load < 0) o = "--help";
        } else if (!strcmp(o, "--n")) a.n[CHURN] = a.n[IMAGES] = SDL_clamp(strtol(v, NULL, 10), 1, 1L << 24);
        else if (!strcmp(o, "--zoom")) a.zoom = SDL_max(0, (int)strtol(v, NULL, 10));
        else if (!strcmp(o, "--vsync")) a.vsync_opt = on(v);
        else if (!strcmp(o, "--redraw")) a.redraw = on(v);
        else if (!strcmp(o, "--size")) sscanf(v, "%dx%d", &ww, &wh);
        else if (!strcmp(o, "--fonts")) a.font_dir = v;
        else if (!strcmp(o, "--frames")) frames = strtol(v, NULL, 10);
        else if (!strcmp(o, "--toggle-every")) toggle_every = strtol(v, NULL, 10);
        else if (!strcmp(o, "--print")) print_every = strtol(v, NULL, 10);
        else o = "--help";
        if (!strcmp(o, "--help")) {
            fputs(usage, stdout);
            return EXIT_FAILURE;
        }
        i += used;
    }
    a.n[IMAGES] = SDL_min(a.n[IMAGES], MAX_SPRITES);
    items_build(&a);
    for (size_t i = 0; i < a.nitems && (scene_arg || list); i++) {
        if (list) printf("%s\n", a.items[i].name);
        if (scene_arg && !strcmp(scene_arg, a.items[i].name)) a.cur = i, scene_arg = NULL;
    }
    if (list) return EXIT_SUCCESS;
    if (scene_arg) {
        printf("unknown scene %s (--list shows them)\n", scene_arg);
        return EXIT_FAILURE;
    }
#ifdef SHR_DESKTOP_ANGLE
    angle_setup();
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("SDL_Init: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
#ifdef SHR_DESKTOP_ANGLE
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#endif
    if (open_driver(&a, driver, ww, wh) != SHR_OK) return EXIT_FAILURE;
    if (frames == 0) fputs(usage, stdout);
    reopen(&a);
    int rc = EXIT_SUCCESS;
    long total = 0;
    uint64_t title_at = 0;
    bool reported = false;
    for (bool running = true; running;) {
        SDL_Event e;
        while (running && SDL_PollEvent(&e)) {
            bool ours = e.window.windowID == SDL_GetWindowID(a.win);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ours)) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) running = key(&a, e.key.key), reported = false;
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && ours && a.load >= 0 &&
                (e.window.data1 != a.pw || e.window.data2 != a.ph))
                reopen(&a);
        }
        if (!running) break;
        if (a.s.st != SHR_OK) {
            if (!reported) printf("%s failed: %s\n", scene_name(&a), a.s.what), update_title(&a);
            reported = true;
            if (quit) {
                rc = EXIT_FAILURE;
                break;
            }
            SDL_Delay(16);
            continue;
        }
        tick(&a);
        total++;
        if (print_every > 0 && a.frames % print_every == 0 && total != frames) print_stats(&a);
        if (SDL_GetTicksNS() - title_at > 250000000u) update_title(&a), title_at = SDL_GetTicksNS();
        if (frames && total == frames) {
            print_stats(&a);
            if (quit) break;
        }
        if (toggle_every > 0 && total % toggle_every == 0) toggle(&a);
    }
    if (a.s.st != SHR_OK) rc = EXIT_FAILURE;
    scene_close(&a);
    close_driver(&a);
    SDL_Quit();
    free(a.items);
    return rc;
}
