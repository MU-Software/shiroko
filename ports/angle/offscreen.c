#include <shiroko/port_angle.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <stdlib.h>
#include <string.h>

struct shr_angle_offscreen {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int backend;
};

static const struct {
    const char *name;
    EGLAttrib type;
} backends[] = {{"metal", EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE},
                {"opengl", EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE},
                {"vulkan", EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE},
                {"d3d11", EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE},
                {"default", EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE}};
#define BACKENDS ((int)(sizeof(backends) / sizeof(backends[0])))

#ifdef __APPLE__
#define DEFAULT_BACKEND 0
#else
#define DEFAULT_BACKEND 1
#endif

/* ANGLE shares one display per backend and eglTerminate() is not reference counted. */
static int display_users[BACKENDS];

static void release(shr_angle_offscreen *o) {
    if (eglGetCurrentContext() == o->context) eglMakeCurrent(o->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(o->display, o->context);
    eglDestroySurface(o->display, o->surface);
    if (--display_users[o->backend] == 0) eglTerminate(o->display);
}

static bool open_backend(shr_angle_offscreen *o, int b, int32_t width, int32_t height) {
    const EGLAttrib attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, backends[b].type, EGL_NONE};
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, NULL, attrs);
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, NULL, NULL)) return false;
    static const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                          EGL_RED_SIZE,     8,               EGL_GREEN_SIZE,      8,
                                          EGL_BLUE_SIZE,    8,               EGL_ALPHA_SIZE,      8,
                                          EGL_NONE};
    static const EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    const EGLint surface_attrs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    EGLConfig config;
    EGLint n = 0;
    *o = (shr_angle_offscreen){d, EGL_NO_SURFACE, EGL_NO_CONTEXT, b};
    display_users[b]++;
    if (eglBindAPI(EGL_OPENGL_ES_API) && eglChooseConfig(d, config_attrs, &config, 1, &n) && n == 1) {
        o->surface = eglCreatePbufferSurface(d, config, surface_attrs);
        o->context = eglCreateContext(d, config, EGL_NO_CONTEXT, context_attrs);
    }
    if (o->surface != EGL_NO_SURFACE && o->context != EGL_NO_CONTEXT && eglMakeCurrent(d, o->surface, o->surface, o->context))
        return true;
    release(o);
    return false;
}

shr_status shr_angle_offscreen_create(int32_t width, int32_t height, shr_angle_offscreen **out) {
    if (!out) return SHR_E_INVALID_ARG;
    *out = NULL;
    if (width <= 0 || height <= 0) return SHR_E_INVALID_ARG;
    const char *env = getenv("SHIROKO_ANGLE_BACKEND");
    int first = DEFAULT_BACKEND;
    for (int b = 0; env && b < BACKENDS; b++)
        if (!strcmp(env, backends[b].name)) first = b;
    shr_angle_offscreen *o = malloc(sizeof(*o));
    if (!o) return SHR_E_NO_MEMORY;
    int order[BACKENDS], n = 1;
    order[0] = first;
    for (int b = 0; b < BACKENDS; b++)
        if (b != first) order[n++] = b;
    for (int k = 0; k < BACKENDS; k++) {
        if (open_backend(o, order[k], width, height)) {
            *out = o;
            return SHR_OK;
        }
    }
    free(o);
    return SHR_E_DEVICE;
}

const char *shr_angle_offscreen_backend(const shr_angle_offscreen *offscreen) {
    return offscreen ? backends[offscreen->backend].name : NULL;
}

shr_status shr_angle_offscreen_destroy(shr_angle_offscreen *offscreen) {
    if (!offscreen) return SHR_E_INVALID_ARG;
    release(offscreen);
    free(offscreen);
    return SHR_OK;
}
