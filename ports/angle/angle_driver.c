#include <shiroko/port_angle.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "raster.h"
#include "shr_alloc.h"
#include "shr_rect.h"

/* Surfaces and buffers stay within it, so coordinates fit int16 attributes and w * h * 4 fits 32 bits. */
#define MAX_SIZE 16384
#define UNITS 8                 /* buffer textures one draw samples, on units 0..7 */
#define SRC_UNIT UNITS          /* the COPY / ROTATE source texture */
#define LAYERS 16               /* layers of a texture shared by buffers of one shape */
#define SHARED_BYTES (4u << 20) /* ... and its largest size */
#define MIN_INST 1024
#define MAX_INST 65536

enum { TEX_R8, TEX_RGBA8, TEX_RGB565, TEX_KINDS };

static const struct {
    GLenum internal, format, type;
    uint32_t bpp;
} tex_formats[TEX_KINDS] = {{GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1},
                            {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4},
                            {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2}};

/* A4 keeps its packed bytes in R8; RGBX8888 keeps X in alpha, which the driver never writes. */
static int tex_kind(shr_pixel_format f) {
    return f == SHR_FORMAT_RGB565 ? TEX_RGB565 : f == SHR_FORMAT_A4 || f == SHR_FORMAT_A8 ? TEX_R8 : TEX_RGBA8;
}

enum { MODE_FILL, MODE_GLYPH, MODE_SYNTH, MODE_IMAGE, MODE_KEEP, MODE_COPY, MODE_CW, MODE_180, MODE_CCW };
enum { FLAG_A4 = 8 }; /* beside SHR_GLYPH_DIM, BOLD and ITALIC */

/* One quad, in destination coordinates: `clip` limits `dst`; `rect` is the buffer rect outside which coverage is 0;
 * `src` holds the source texel at dst.x0, dst.y0 (rotations: the offset added to the rotated pixel) and the slant
 * axis. */
typedef struct inst {
    int16_t dst[4], clip[4], rect[4], src[4];
    uint8_t unit, layer, mode, flags;
    uint8_t rgba[4];
} inst;

/* Layers of one GL_TEXTURE_2D_ARRAY; `used` has a bit per layer holding a buffer, `unit` is the unit the pending
 * draw samples it from (-1: none). */
typedef struct chunk {
    struct chunk *next;
    GLuint tex;
    int kind, unit;
    int32_t w, h;
    uint32_t layers, used;
} chunk;

typedef struct slot {
    chunk *c; /* NULL for a buffer without pixels */
    uint32_t layer;
} slot;

/* Keep slots of one texture kind: id k at index k - 1 of `cols` x `rows` slots of sw x sh texels in each layer of
 * `c`, whose texture the first store of the kind creates. */
typedef struct atlas {
    chunk c;
    int32_t sw, sh;
    uint32_t cols, rows;
} atlas;

/* A DEVICE surface; its address is the surface's `pixels`. */
typedef struct surf {
    struct surf *next;
    GLuint tex, fbo;
    int32_t w, h;
    shr_pixel_format format;
} surf;

typedef struct scratch {
    GLuint tex;
    int32_t w, h;
} scratch;

/* `bytes` sums the byte_length of registered buffers. `src_tex` is the texture bound to SRC_UNIT since prepare();
 * `src_pending`: pending instances sample it. */
typedef struct gl_drv {
    shr__alloc al;
    EGLContext context;
    uint64_t budget, bytes, next_id;
    shr_angle_stats stats;
    uint32_t nbuffers, nunits;
    shr_image *bufs;
    slot *slots;
    chunk *chunks, *units[UNITS];
    GLuint src_tex;
    bool src_pending;
    surf *surfs;
    GLint max;
    GLuint prog, vao, quad, vbo, fbo;
    GLint scale, origin, levels;
    scratch src[TEX_KINDS], dst[TEX_KINDS];
    uint8_t *staging;
    size_t staging_bytes, ninst, cap;
    inst *inst;
    shr__keeps keeps;
    atlas atlas[TEX_KINDS];
    GLuint keep_fbo;
} gl_drv;

/* One execute(): a CPU destination (target NULL) is drawn in a scratch texture holding `area`. */
typedef struct run {
    const shr_surface *dst;
    surf *target;
    shr_rect area;
    shr_point origin;
} run;

static const char *const vs_src =
    "#version 300 es\n"
    "uniform vec2 u_scale;\n"
    "uniform ivec2 u_origin;\n"
    "layout(location = 0) in vec2 a_corner;\n"
    "layout(location = 1) in ivec4 a_dst;\n"
    "layout(location = 2) in ivec4 a_clip;\n"
    "layout(location = 3) in ivec4 a_rect;\n"
    "layout(location = 4) in ivec4 a_src;\n"
    "layout(location = 5) in uvec4 a_info;\n"
    "layout(location = 6) in vec4 a_color;\n"
    "out vec4 v_rect;\n"
    "out vec4 v_src;\n"
    "out vec4 v_info;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "    ivec2 lo = max(a_dst.xy, a_clip.xy), hi = max(min(a_dst.zw, a_clip.zw), lo);\n"
    "    vec2 pos = vec2(a_corner.x > 0.5 ? hi.x : lo.x, a_corner.y > 0.5 ? hi.y : lo.y) - vec2(u_origin);\n"
    "    gl_Position = vec4(pos * u_scale - 1.0, 0.0, 1.0);\n"
    "    v_rect = vec4(a_rect);\n"
    "    v_src = vec4(a_info.z >= 6u ? a_src.xy : a_src.xy - a_dst.xy, a_src.z, a_info.w);\n"
    "    v_info = vec4(a_info.xyz, 0.0);\n"
    "    v_color = a_color;\n"
    "}\n";

/* Integers reach the fragment shader as smooth varyings equal at every vertex and are rounded there; the source
 * pixel follows from gl_FragCoord, so clipping a quad never moves it. Blending is source-over with the output alpha;
 * coverage is rounded like the software port's. Opaque colours are quantized here to the target's levels
 * (u_levels): GPUs may convert outputs at half precision, which would round some 8-bit values to the wrong 5/6-bit
 * level. MODE_SYNTH evaluates the shiroko_driver.h coverage formula in integers; texelFetch outside a texture is
 * undefined, so columns outside `rect` are clamped and then masked. GLSL ES 3.00 indexes sampler arrays with
 * constants only. */
_Static_assert(SHR_GLYPH_SLANT / 2 == 27, "the shader's italic slope");
_Static_assert(4 * SHR_GLYPH_SYNTH_MAX <= INT16_MAX && MAX_SIZE <= INT16_MAX, "int16 attributes");
static const char *const fs_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2DArray u_buf[8];\n"
    "uniform highp sampler2D u_src;\n"
    "uniform vec3 u_levels;\n"
    "uniform ivec2 u_origin;\n"
    "in vec4 v_rect;\n"
    "in vec4 v_src;\n"
    "in vec4 v_info;\n"
    "in vec4 v_color;\n"
    "out vec4 o;\n"
    "int unit, layer, flags;\n"
    "ivec4 rect;\n"
    "vec4 texel(ivec2 t) {\n"
    "    ivec3 c = ivec3(t, layer);\n"
    "    if (unit == 0) return texelFetch(u_buf[0], c, 0);\n"
    "    if (unit == 1) return texelFetch(u_buf[1], c, 0);\n"
    "    if (unit == 2) return texelFetch(u_buf[2], c, 0);\n"
    "    if (unit == 3) return texelFetch(u_buf[3], c, 0);\n"
    "    if (unit == 4) return texelFetch(u_buf[4], c, 0);\n"
    "    if (unit == 5) return texelFetch(u_buf[5], c, 0);\n"
    "    if (unit == 6) return texelFetch(u_buf[6], c, 0);\n"
    "    return texelFetch(u_buf[7], c, 0);\n"
    "}\n"
    "int cov(int x, int y) {\n"
    "    bool a4 = (flags & 8) != 0;\n"
    "    int b = int(texel(ivec2(a4 ? x >> 1 : x, y)).r * 255.0 + 0.5);\n"
    "    return a4 ? 17 * ((x & 1) == 0 ? b >> 4 : b & 15) : b;\n"
    "}\n"
    "int masked(int x, int y) {\n"
    "    int c = cov(clamp(x, rect.x, rect.z - 1), y);\n"
    "    return x >= rect.x && x < rect.z ? c : 0;\n"
    "}\n"
    "int bolden(int l, int m, int r) { return (flags & 2) == 0 || r > m ? m : max(m, l); }\n"
    "void main() {\n"
    "    ivec4 s4 = ivec4(floor(v_src + 0.5));\n"
    "    ivec3 info = ivec3(floor(v_info.xyz + 0.5));\n"
    "    unit = info.x, layer = info.y, flags = s4.w;\n"
    "    rect = ivec4(floor(v_rect + 0.5));\n"
    "    int mode = info.z;\n"
    "    ivec2 p = ivec2(floor(gl_FragCoord.xy)) + u_origin;\n"
    "    ivec2 s = s4.xy + (mode == 6 ? ivec2(p.y, -p.x) : mode == 7 ? -p : mode == 8 ? ivec2(-p.y, p.x) : p);\n"
    "    if (mode == 0) {\n"
    "        o = v_color;\n"
    "    } else if (mode == 1) {\n"
    "        int c = cov(s.x, s.y);\n"
    "        if ((flags & 1) != 0) c = (c + 1) >> 1;\n"
    "        o = vec4(v_color.rgb, float(c) / 255.0);\n"
    "    } else if (mode == 2) {\n"
    "        int k = 0, f = 0;\n"
    "        if ((flags & 4) != 0) {\n"
    "            int t = 27 * (s4.z - 2 * (s.y - rect.y) - 1) + 8388608;\n"
    "            k = (t >> 8) - 32768, f = t & 255;\n"
    "        }\n"
    "        int q = s.x - k, i0 = masked(q - 2, s.y), i1 = masked(q - 1, s.y), i2 = masked(q, s.y);\n"
    "        int i3 = masked(q + 1, s.y);\n"
    "        int c = (bolden(i1, i2, i3) * (256 - f) + bolden(i0, i1, i2) * f + 128) >> 8;\n"
    "        if ((flags & 1) != 0) c = (c + 1) >> 1;\n"
    "        o = vec4(v_color.rgb, float(c) / 255.0);\n"
    "    } else if (mode == 3) {\n"
    "        o = texel(s);\n"
    "    } else if (mode == 4) {\n"
    "        o = vec4(texel(s).rgb, 1.0);\n"
    "    } else {\n"
    "        o = vec4(texelFetch(u_src, s, 0).rgb, 1.0);\n"
    "    }\n"
    "    if (o.a == 1.0) o.rgb = floor((floor(o.rgb * 255.0 + 0.5) * u_levels + 127.0) / 255.0) / u_levels;\n"
    "}\n";

static shr_status current(const gl_drv *g) { return eglGetCurrentContext() == g->context ? SHR_OK : SHR_E_STATE; }

static surf *find(const gl_drv *g, const void *handle) {
    surf *s = g->surfs;
    while (s && s != handle) s = s->next;
    return s;
}

static surf *device_surf(const gl_drv *g, const void *px, int32_t w, int32_t h, shr_pixel_format f) {
    surf *s = find(g, px);
    return s && s->w == w && s->h == h && s->format == f ? s : NULL;
}

/* CPU memory, or a surface of this driver with these dimensions. */
static shr_status reach(const void *user, const void *px, int32_t w, int32_t h, shr_pixel_format f,
                        shr_memory_domain dom) SHR_NONBLOCKING {
    if (dom != SHR_MEMORY_DEVICE) return dom == SHR_MEMORY_DMA ? SHR_E_UNSUPPORTED : SHR_OK;
    return device_surf(user, px, w, h, f) ? SHR_OK : SHR_E_UNSUPPORTED;
}

/* The source pixels a COPY or ROTATE reads: a ROTATE reads its whole source. */
static shr_rect read_area(const shr_draw_cmd *c) {
    shr_rect d = c->dst;
    if (c->kind == SHR_CMD_ROTATE) return (shr_rect){0, 0, c->src.width, c->src.height};
    return (shr_rect){c->src_origin.x, c->src_origin.y, c->src_origin.x + d.x1 - d.x0, c->src_origin.y + d.y1 - d.y0};
}

/* Callers pass rects within MAX_SIZE. */
static size_t texels(shr_rect r) { return (size_t)(r.x1 - r.x0) * (size_t)(r.y1 - r.y0); }

/* 2D uploads that GL cannot read in place: rows that are not whole pixels, misaligned 16-bit pixels. */
static bool staged(const void *pixels, size_t stride, shr_pixel_format f) {
    uint32_t bpp = tex_formats[tex_kind(f)].bpp;
    return stride % bpp || stride / bpp > INT32_MAX || (bpp == 2 && (uintptr_t)pixels % 2);
}

/* The software port's checks with this driver's reach; also the destination area (keep groups left out), the
 * staging bytes and the number of draws, so nothing fails once drawing started. */
static shr_status plan(const gl_drv *g, shr__keeps *keeps, const shr_surface *dst, const shr_draw_cmd *cmds,
                       size_t count, run *x, size_t *staging, size_t *draws) {
    shr_status st = shr__raster_check(dst, cmds, count, g->bufs, g->nbuffers, keeps, reach, g);
    if (st != SHR_OK) return st;
    if (dst->width > g->max || dst->height > g->max) return SHR_E_UNSUPPORTED;
    *x = (run){dst, dst->domain == SHR_MEMORY_DEVICE ? find(g, dst->pixels) : NULL, {0, 0, 0, 0}, {0, 0}};
    bool group = false;
    for (size_t i = 0; i < count; i++) {
        const shr_draw_cmd *c = &cmds[i];
        if (c->kind == SHR_CMD_KEEP_BEGIN || c->kind == SHR_CMD_KEEP_END) {
            group = c->kind == SHR_CMD_KEEP_BEGIN;
            continue;
        }
        ++*draws;
        if (!group) x->area = shr__rect_union(x->area, c->dst);
        bool up = (c->kind == SHR_CMD_COPY || c->kind == SHR_CMD_ROTATE) && c->src.domain != SHR_MEMORY_DEVICE;
        if (up && staged(c->src.pixels, c->src.stride, (shr_pixel_format)c->src.format) &&
            4 * texels(read_area(c)) > *staging)
            *staging = 4 * texels(read_area(c));
    }
    if (!x->target && 4 * texels(x->area) > *staging) *staging = 4 * texels(x->area);
    return SHR_OK;
}

static shr_status reserve(gl_drv *g, size_t staging, size_t draws) {
    size_t n = draws < MAX_INST ? draws : MAX_INST;
    if (n > g->cap) {
        inst *p = shr__malloc(&g->al, n * sizeof(inst), SHR_ALIGNOF(inst), SHR_ALLOC_PAYLOAD);
        if (!p) return SHR_E_NO_MEMORY;
        SHR_FREE_ARRAY(&g->al, g->inst, inst, g->cap);
        g->inst = p, g->cap = n;
    }
    if (staging <= g->staging_bytes) return SHR_OK;
    uint8_t *p = shr__malloc(&g->al, staging, 16, SHR_ALLOC_PAYLOAD);
    if (!p) return SHR_E_NO_MEMORY;
    shr__free(&g->al, g->staging, g->staging_bytes, 16, SHR_ALLOC_PAYLOAD);
    g->staging = p, g->staging_bytes = staging;
    return SHR_OK;
}

/* Errors the application left behind would be taken for ours. */
static void prepare(gl_drv *g) {
    while (glGetError() != GL_NO_ERROR) {
    }
    static const GLenum off[] = {GL_DEPTH_TEST,         GL_STENCIL_TEST,         GL_CULL_FACE,
                                 GL_DITHER,             GL_RASTERIZER_DISCARD,   GL_SAMPLE_ALPHA_TO_COVERAGE,
                                 GL_SAMPLE_COVERAGE,    GL_POLYGON_OFFSET_FILL,  GL_SCISSOR_TEST};
    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++) glDisable(off[i]);
    static const GLenum zero[] = {GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,  GL_UNPACK_SKIP_PIXELS, GL_UNPACK_IMAGE_HEIGHT,
                                  GL_UNPACK_SKIP_IMAGES, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS,     GL_PACK_SKIP_PIXELS};
    for (size_t i = 0; i < sizeof(zero) / sizeof(zero[0]); i++) glPixelStorei(zero[i], 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glUseProgram(g->prog);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    for (GLuint k = 0; k <= SRC_UNIT; k++) glBindSampler(k, 0);
    glActiveTexture(GL_TEXTURE0 + SRC_UNIT);
    glBindTexture(GL_TEXTURE_2D, 0);
    g->src_tex = 0;
}

static uint64_t chunk_bytes(const chunk *c) {
    return (uint64_t)c->w * (uint64_t)c->h * tex_formats[c->kind].bpp * c->layers;
}

static void chunk_free(gl_drv *g, chunk *c) {
    chunk **p = &g->chunks;
    while (*p != c) p = &(*p)->next;
    *p = c->next;
    glDeleteTextures(1, &c->tex);
    g->stats.textures--, g->stats.texture_bytes -= chunk_bytes(c);
    SHR_DELETE(&g->al, c, chunk);
}

/* `shared`: other buffers have this shape, so the texture gets room for more. */
static chunk *chunk_new(gl_drv *g, int kind, int32_t w, int32_t h, bool shared) {
    chunk *c = SHR_NEW(&g->al, chunk);
    if (!c) return NULL;
    uint64_t n = shared ? SHARED_BYTES / ((uint64_t)w * (uint64_t)h * tex_formats[kind].bpp) : 1;
    *c = (chunk){g->chunks, 0, kind, -1, w, h, n < 1 ? 1 : n > LAYERS ? LAYERS : (uint32_t)n, 0};
    g->chunks = c;
    glGenTextures(1, &c->tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, c->tex);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, tex_formats[kind].internal, w, h, (GLsizei)c->layers);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->stats.textures++, g->stats.texture_bytes += chunk_bytes(c);
    return c;
}

/* A free layer of a texture of this shape. */
static shr_status layer_alloc(gl_drv *g, int kind, int32_t w, int32_t h, slot *out) {
    bool seen = false;
    chunk *c = g->chunks;
    for (; c; c = c->next) {
        if (c->kind != kind || c->w != w || c->h != h) continue;
        if (c->used != (1u << c->layers) - 1) break;
        seen = true;
    }
    if (!c && !(c = chunk_new(g, kind, w, h, seen))) return SHR_E_NO_MEMORY;
    uint32_t l = 0;
    while (c->used >> l & 1) l++;
    c->used |= 1u << l;
    *out = (slot){c, l};
    return SHR_OK;
}

/* Pixels `r` of `m` into its layer; A4 as whole bytes. */
static void layer_upload(const slot *s, const shr_image *m, shr_rect r) {
    if (shr__rect_empty(r)) return;
    if (m->format == SHR_FORMAT_A4) r.x0 >>= 1, r.x1 = (r.x1 + 1) >> 1;
    size_t bpp = tex_formats[s->c->kind].bpp;
    int32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    const uint8_t *p = (const uint8_t *)m->pixels + (size_t)r.y0 * m->stride + (size_t)r.x0 * bpp;
    bool rows = m->stride % bpp || m->stride / bpp > INT32_MAX;
    glBindTexture(GL_TEXTURE_2D_ARRAY, s->c->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rows ? 0 : (GLint)(m->stride / bpp));
    for (int32_t y = 0; y < h; y += rows ? 1 : h)
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, r.x0, r.y0 + y, (GLint)s->layer, w, rows ? 1 : h, 1,
                        tex_formats[s->c->kind].format, GL_UNSIGNED_BYTE, p + (size_t)y * m->stride);
}

static void buf_release(gl_drv *g, uint32_t id) {
    slot *s = &g->slots[id - 1];
    g->bytes -= g->bufs[id - 1].byte_length;
    g->bufs[id - 1] = (shr_image){0};
    if (s->c && !(s->c->used &= ~(1u << s->layer))) chunk_free(g, s->c);
    *s = (slot){NULL, 0};
}

/* A layer of the old shape is reused; on failure the id names nothing. */
static shr_status buf_register(gl_drv *g, uint32_t id, const shr_image *m) {
    shr_image *b = &g->bufs[id - 1];
    slot *s = &g->slots[id - 1];
    if (m->width > g->max || m->height > g->max ||
        (g->budget && g->bytes - b->byte_length + m->byte_length > g->budget))
        return SHR_E_UNSUPPORTED;
    int kind = tex_kind(m->format);
    int32_t w = m->format == SHR_FORMAT_A4 ? (m->width + 1) / 2 : m->width, h = m->height;
    if (!s->c || s->c->kind != kind || s->c->w != w || s->c->h != h) {
        buf_release(g, id);
        shr_status st = w && h ? layer_alloc(g, kind, w, h, s) : SHR_OK;
        if (st != SHR_OK) return st;
    }
    g->bytes = g->bytes - b->byte_length + m->byte_length;
    *b = *m;
    if (s->c) layer_upload(s, m, (shr_rect){0, 0, m->width, m->height});
    return SHR_OK;
}

/* Buffer commands in order, each checked against the table the earlier ones left; `*first` is the first draw. */
static shr_status prologue(gl_drv *g, const shr_draw_cmd *cmds, size_t count, size_t *first) {
    for (*first = 0; cmds && *first < count && shr__buffer_cmd(cmds[*first].kind); ++*first) {
        const shr_draw_cmd *c = &cmds[*first];
        if (c->kind == SHR_CMD_KEEP_RELEASE) { /* the slot stays */
            if (c->buffer && c->buffer <= g->keeps.n) g->keeps.at[c->buffer - 1] = (shr__keep){0};
            continue;
        }
        shr_image m;
        shr_status st = shr__raster_buffer_check(c, g->bufs, g->nbuffers, reach, g, &m);
        if (st != SHR_OK) return st;
        if (c->kind == SHR_CMD_BUFFER_REGISTER) {
            if ((st = buf_register(g, c->buffer, &m)) != SHR_OK) return st;
        } else if (c->kind == SHR_CMD_BUFFER_UPDATE) {
            if (g->slots[c->buffer - 1].c) layer_upload(&g->slots[c->buffer - 1], &g->bufs[c->buffer - 1], c->src_rect);
        } else if (c->buffer && c->buffer <= g->nbuffers) {
            buf_release(g, c->buffer);
        }
    }
    return SHR_OK;
}

static void atlas_drop(gl_drv *g, atlas *a) {
    if (!a->c.tex) return;
    glDeleteTextures(1, &a->c.tex);
    g->stats.keep_texture_bytes -= chunk_bytes(&a->c);
    a->c.tex = 0;
}

/* Every buffer and keep, and the scratch textures: after a GL error their storage is unknown. */
static void drop_textures(gl_drv *g) {
    for (uint32_t id = 1; id <= g->nbuffers; id++) buf_release(g, id);
    for (uint32_t i = 0; g->keeps.at && i < g->keeps.n; i++) g->keeps.at[i] = (shr__keep){0};
    for (int k = 0; k < TEX_KINDS; k++) {
        atlas_drop(g, &g->atlas[k]);
        glDeleteTextures(1, &g->src[k].tex);
        glDeleteTextures(1, &g->dst[k].tex);
        g->src[k] = g->dst[k] = (scratch){0, 0, 0};
    }
    g->src_tex = 0;
}

static shr_status gl_status(gl_drv *g) {
    if (glGetError() == GL_NO_ERROR) return SHR_OK;
    drop_textures(g);
    return SHR_E_DEVICE;
}

/* Slots of `bytes` (P texels) are sw x sh texels, sh at least SHR_CELL_HEIGHT where P allows, so a row of cells
 * whose bytes fit fits as it is. They stack in as few columns as fit, so up to cols - 1 go unused. False when they
 * take more than `max_layers` layers. */
static bool atlas_init(atlas *a, int kind, uint64_t bytes, uint32_t n, int32_t max, uint32_t max_layers) {
    uint64_t p = bytes / tex_formats[kind].bpp, w = p / SHR_CELL_HEIGHT, m = (uint64_t)max;
    a->sw = (int32_t)(w < 1 ? 1 : w > m ? m : w);
    a->sh = (int32_t)(p / (uint64_t)a->sw > m ? m : p / (uint64_t)a->sw);
    uint64_t rows = m / (uint64_t)a->sh, cols = m / (uint64_t)a->sw, layers = (n + rows * cols - 1) / (rows * cols);
    if (layers == 1) cols = (n + rows - 1) / rows, rows = (n + cols - 1) / cols;
    a->cols = (uint32_t)cols, a->rows = (uint32_t)rows;
    a->c = (chunk){NULL, 0, kind, -1, a->sw * (int32_t)cols, a->sh * (int32_t)rows, (uint32_t)layers, 0};
    return layers <= max_layers;
}

/* The layer and top-left texel of the slot of keep `id`. */
static shr_point slot_at(const atlas *a, uint32_t id, uint32_t *layer) {
    uint32_t i = id - 1, per = a->cols * a->rows, j = i % per;
    *layer = i / per;
    return (shr_point){(int32_t)(j % a->cols) * a->sw, (int32_t)(j / a->cols) * a->sh};
}

/* Slots for the keeps the checked batch stores, then their records. SHR_E_NO_MEMORY, changing nothing, when a keep
 * is wider or taller than a slot or GL has no memory for the atlas. */
static shr_status keep_alloc(gl_drv *g, shr_pixel_format format) {
    atlas *a = &g->atlas[tex_kind(format)];
    for (uint32_t k = 0; k < g->keeps.nstores; k++) {
        const shr__keep *e = &g->keeps.at[g->keeps.stores[k] - 1];
        if (e->store_w > a->sw || e->store_h > a->sh) return SHR_E_NO_MEMORY;
    }
    if (g->keeps.nstores && !a->c.tex) {
        shr_status st = gl_status(g); /* errors before it are not the atlas's */
        if (st != SHR_OK) return st;
        glGenTextures(1, &a->c.tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, a->c.tex);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, tex_formats[a->c.kind].internal, a->c.w, a->c.h, (GLsizei)a->c.layers);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g->stats.keep_texture_bytes += chunk_bytes(&a->c);
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) {
            atlas_drop(g, a);
            if (e != GL_OUT_OF_MEMORY) drop_textures(g);
            return e == GL_OUT_OF_MEMORY ? SHR_E_NO_MEMORY : SHR_E_DEVICE;
        }
    }
    for (uint32_t k = 0; k < g->keeps.nstores; k++) {
        shr__keep *e = &g->keeps.at[g->keeps.stores[k] - 1];
        e->width = e->store_w, e->height = e->store_h, e->format = format;
        e->size = (size_t)e->width * (size_t)e->height * shr__px_bytes(format);
    }
    return SHR_OK;
}

static void flush(gl_drv *g) {
    if (!g->ninst) return;
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g->ninst * sizeof(inst)), g->inst, GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)g->ninst);
    g->stats.draws++, g->stats.instances += g->ninst;
    g->ninst = 0;
    while (g->nunits) g->units[--g->nunits]->unit = -1;
    g->src_pending = false;
}

/* The unit the pending draw samples `c` from; a ninth texture draws what is pending first. */
static uint8_t unit_of(gl_drv *g, chunk *c) {
    if (c->unit >= 0) return (uint8_t)c->unit;
    if (g->nunits == UNITS) flush(g);
    int k = (int)g->nunits++;
    g->units[k] = c, c->unit = k;
    glActiveTexture(GL_TEXTURE0 + (GLenum)k);
    glBindTexture(GL_TEXTURE_2D_ARRAY, c->tex);
    glActiveTexture(GL_TEXTURE0 + SRC_UNIT);
    return (uint8_t)k;
}

/* Binds `t` as the source texture; pending instances sampling another go first. */
static void src_bind(gl_drv *g, GLuint t) {
    if (t == g->src_tex) return;
    if (g->src_pending) flush(g);
    glBindTexture(GL_TEXTURE_2D, t);
    g->src_tex = t;
}

/* Bound explicitly: the name may be one just deleted that `src_tex` still holds. */
static GLuint new_texture(gl_drv *g, int kind, int32_t w, int32_t h) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    g->src_tex = t;
    glTexStorage2D(GL_TEXTURE_2D, 1, tex_formats[kind].internal, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

/* Binds a texture of at least w x h whose contents change, so pending instances sampling it go first. */
static GLuint scratch_bind(gl_drv *g, scratch *s, int kind, int32_t w, int32_t h) {
    if (g->src_pending) flush(g);
    if (w > s->w || h > s->h) {
        glDeleteTextures(1, &s->tex);
        s->w = w > s->w ? w : s->w, s->h = h > s->h ? h : s->h;
        s->tex = new_texture(g, kind, s->w, s->h);
    }
    src_bind(g, s->tex);
    return s->tex;
}

/* Rect `r` of `m` to (0, 0) of the bound 2D texture. */
static void upload(gl_drv *g, const void *pixels, size_t stride, shr_pixel_format f, shr_rect r) {
    int kind = tex_kind(f);
    size_t bpp = tex_formats[kind].bpp;
    int32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    const uint8_t *base = pixels;
    const void *data = base + (size_t)r.y0 * stride + (size_t)r.x0 * bpp;
    GLint row = 0;
    if (staged(pixels, stride, f)) {
        for (int32_t y = 0; y < h; y++)
            memcpy(g->staging + (size_t)y * (size_t)w * bpp, base + (size_t)(r.y0 + y) * stride + (size_t)r.x0 * bpp,
                   (size_t)w * bpp);
        data = g->staging;
    } else {
        row = (GLint)(stride / bpp);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, tex_formats[kind].format, tex_formats[kind].type, data);
}

/* w x h pixels at (0, 0) of the bound framebuffer into rows of `out` in `f`. */
static void read_back(gl_drv *g, int32_t w, int32_t h, shr_pixel_format f, uint8_t *out, size_t stride) {
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g->staging);
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *q = g->staging + (size_t)y * (size_t)w * 4;
        uint8_t *p = out + (size_t)y * stride;
        if (f != SHR_FORMAT_RGB565) {
            memcpy(p, q, (size_t)w * 4);
            continue;
        }
        for (int32_t x = 0; x < w; x++, q += 4, p += 2) {
            uint16_t v = (uint16_t)(shr__quantize(q[0], 31) << 11 | shr__quantize(q[1], 63) << 5 | shr__quantize(q[2], 31));
            memcpy(p, &v, 2);
        }
    }
}

/* Writes what was drawn so far into a CPU destination. */
static void sync(gl_drv *g, const run *x) {
    flush(g);
    const shr_surface *d = x->dst;
    read_back(g, x->area.x1 - x->area.x0, x->area.y1 - x->area.y0, d->format,
              (uint8_t *)d->pixels + (size_t)x->area.y0 * d->stride + (size_t)x->area.x0 * shr__px_bytes(d->format),
              d->stride);
}

/* Binds the texture a COPY or ROTATE reads; returns the source pixel at its texel (0, 0). */
static shr_point source(gl_drv *g, const run *x, const shr_draw_cmd *c) {
    const shr_image_ref *m = &c->src;
    shr_rect s = read_area(c);
    int kind = tex_kind((shr_pixel_format)m->format);
    if (m->domain == SHR_MEMORY_DEVICE) {
        const surf *f = find(g, m->pixels);
        if (f != x->target) {
            src_bind(g, f->tex);
            return (shr_point){0, 0};
        }
        /* Reading the surface being drawn is a feedback loop: copy the part read, as drawn so far. */
        flush(g);
        scratch_bind(g, &g->src[kind], kind, s.x1 - s.x0, s.y1 - s.y0);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.x0, s.y0, s.x1 - s.x0, s.y1 - s.y0);
    } else {
        if (!x->target && shr__raster_reads_dst(x->dst, m)) sync(g, x);
        scratch_bind(g, &g->src[kind], kind, s.x1 - s.x0, s.y1 - s.y0);
        upload(g, m->pixels, m->stride, (shr_pixel_format)m->format, s);
    }
    return (shr_point){s.x0, s.y0};
}

static void put_rect(int16_t out[4], shr_rect r) {
    out[0] = (int16_t)r.x0, out[1] = (int16_t)r.y0, out[2] = (int16_t)r.x1, out[3] = (int16_t)r.y1;
}

/* Instances carry the part of `dst` inside `clip` and sources moved along with its corner. */
static void draw(gl_drv *g, const run *x, const shr_draw_cmd *c, shr_rect clip) {
    shr_rect d = shr__rect_intersect(c->dst, clip);
    if (shr__rect_empty(d)) return;
    if (g->ninst == g->cap) flush(g);
    inst v = {.rgba = {0, 0, 0, 255}};
    shr_point m = {d.x0 - c->dst.x0, d.y0 - c->dst.y0};
    put_rect(v.dst, d);
    put_rect(v.clip, clip);
    if (c->kind == SHR_CMD_FILL || c->kind == SHR_CMD_GLYPH)
        v.rgba[0] = (uint8_t)(c->color >> 16), v.rgba[1] = (uint8_t)(c->color >> 8), v.rgba[2] = (uint8_t)c->color;
    if (c->kind == SHR_CMD_FILL) {
        v.mode = MODE_FILL;
        if (c->flags & SHR_GLYPH_DIM) v.rgba[3] = 128;
    } else if (c->kind == SHR_CMD_GLYPH || c->kind == SHR_CMD_IMAGE) {
        /* An empty rect has no coverage, also where a styled glyph's footprint reaches. */
        shr_rect r = c->src_rect;
        if (shr__rect_empty(r)) return;
        const slot *s = &g->slots[c->buffer - 1];
        put_rect(v.rect, r);
        /* Checked: W, H <= SHR_GLYPH_SYNTH_MAX and an ITALIC axis within 4 * SHR_GLYPH_SYNTH_MAX. */
        v.src[0] = (int16_t)(r.x0 + c->src_origin.x + m.x), v.src[1] = (int16_t)(r.y0 + c->src_origin.y + m.y);
        v.src[2] = (int16_t)(c->flags & SHR_GLYPH_ITALIC ? c->slant_axis : 0);
        v.unit = unit_of(g, s->c), v.layer = (uint8_t)s->layer;
        v.mode = c->kind == SHR_CMD_IMAGE ? MODE_IMAGE : c->flags & (SHR_GLYPH_BOLD | SHR_GLYPH_ITALIC) ? MODE_SYNTH : MODE_GLYPH;
        v.flags = (uint8_t)((c->flags & 7) | (g->bufs[c->buffer - 1].format == SHR_FORMAT_A4 ? FLAG_A4 : 0));
    } else if (c->kind == SHR_CMD_KEEP_DRAW) {
        atlas *a = &g->atlas[tex_kind(x->dst->format)];
        uint32_t layer;
        shr_point p = slot_at(a, c->buffer, &layer);
        v.src[0] = (int16_t)(p.x + c->src_origin.x + m.x), v.src[1] = (int16_t)(p.y + c->src_origin.y + m.y);
        v.unit = unit_of(g, &a->c), v.layer = (uint8_t)layer, v.mode = MODE_KEEP;
        g->stats.keep_draws++;
    } else {
        shr_point o = source(g, x, c), b;
        int32_t w = c->src.width - 1 - o.x, h = c->src.height - 1 - o.y;
        v.mode = MODE_COPY;
        if (c->kind == SHR_CMD_ROTATE) {
            bool cw = c->rotation == SHR_ROTATE_90_CW, ccw = c->rotation == SHR_ROTATE_90_CCW;
            v.mode = cw ? MODE_CW : ccw ? MODE_CCW : MODE_180;
            shr_point t = {c->dst.x0, c->dst.y0}; /* the source maps onto `dst` from its corner */
            b = cw ? (shr_point){-o.x - t.y, h + t.x} : ccw ? (shr_point){w + t.y, -o.y - t.x} : (shr_point){w + t.x, h + t.y};
        } else {
            b = (shr_point){c->src_origin.x - o.x + m.x, c->src_origin.y - o.y + m.y};
        }
        v.src[0] = (int16_t)b.x, v.src[1] = (int16_t)b.y;
        g->src_pending = true;
    }
    g->inst[g->ninst++] = v;
}

static void viewport(gl_drv *g, int32_t w, int32_t h, shr_point origin, shr_pixel_format f) {
    glViewport(0, 0, w, h);
    glUniform2f(g->scale, 2.0f / (float)w, 2.0f / (float)h);
    glUniform2i(g->origin, origin.x, origin.y);
    bool rgb565 = f == SHR_FORMAT_RGB565;
    glUniform3f(g->levels, rgb565 ? 31.0f : 255.0f, rgb565 ? 63.0f : 255.0f, rgb565 ? 31.0f : 255.0f);
}

/* Binds the destination's framebuffer; a CPU destination's area is uploaded into a scratch texture. */
static void target(gl_drv *g, run *x) {
    int32_t w = x->dst->width, h = x->dst->height;
    if (x->target) {
        glBindFramebuffer(GL_FRAMEBUFFER, x->target->fbo);
    } else {
        int kind = tex_kind(x->dst->format);
        scratch *s = &g->dst[kind];
        GLuint t = scratch_bind(g, s, kind, x->area.x1 - x->area.x0, x->area.y1 - x->area.y0);
        const shr_surface *d = x->dst;
        upload(g, d->pixels, d->stride, d->format, x->area);
        src_bind(g, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        x->origin = (shr_point){x->area.x0, x->area.y0};
        w = s->w, h = s->h;
    }
    viewport(g, w, h, x->origin, x->dst->format);
}

/* Draws the group at cmds[at] into its slot, its commands moved there; returns the index of its KEEP_END. `*bound` is
 * the layer attached plus 1. */
static size_t keep_store(gl_drv *g, const run *x, const shr_draw_cmd *cmds, size_t at, uint32_t *bound) {
    const atlas *a = &g->atlas[tex_kind(x->dst->format)];
    shr_rect k = cmds[at].dst;
    uint32_t layer;
    shr_point p = slot_at(a, cmds[at].buffer, &layer);
    if (*bound != layer + 1) {
        flush(g);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, a->c.tex, 0, (GLint)layer);
        *bound = layer + 1;
    }
    g->stats.keep_stores++;
    shr_point o = {k.x0 - p.x, k.y0 - p.y};
    shr_rect clip = {p.x, p.y, p.x + k.x1 - k.x0, p.y + k.y1 - k.y0};
    size_t i = at + 1;
    for (; cmds[i].kind != SHR_CMD_KEEP_END; i++) {
        shr_draw_cmd c = cmds[i];
        c.dst = (shr_rect){c.dst.x0 - o.x, c.dst.y0 - o.y, c.dst.x1 - o.x, c.dst.y1 - o.y};
        draw(g, x, &c, clip);
    }
    return i;
}

/* Keep groups first, then the destination's commands. */
static void render(gl_drv *g, run *x, const shr_draw_cmd *cmds, size_t count) {
    if (g->keeps.nstores) {
        const atlas *a = &g->atlas[tex_kind(x->dst->format)];
        uint32_t bound = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, g->keep_fbo);
        viewport(g, a->c.w, a->c.h, (shr_point){0, 0}, x->dst->format);
        for (size_t i = 0; i < count; i++)
            if (cmds[i].kind == SHR_CMD_KEEP_BEGIN) i = keep_store(g, x, cmds, i, &bound);
        flush(g);
    }
    if (!shr__rect_empty(x->area)) {
        target(g, x);
        shr_rect all = {0, 0, x->dst->width, x->dst->height};
        for (size_t i = 0; i < count; i++) {
            if (cmds[i].kind != SHR_CMD_KEEP_BEGIN)
                draw(g, x, &cmds[i], all);
            else
                while (cmds[i].kind != SHR_CMD_KEEP_END) i++;
        }
        flush(g);
        if (!x->target) sync(g, x);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static shr_status gl_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t count,
                             shr_fence fence) {
    (void)fence;
    gl_drv *g = user;
    shr_status st = current(g);
    if (st != SHR_OK) return st;
    bool gl = cmds && count && shr__buffer_cmd(cmds[0].kind);
    if (gl) prepare(g);
    size_t first, staging = 0, draws = 0;
    run x = {0};
    st = prologue(g, cmds, count, &first);
    shr__keeps *keeps = g->keeps.n ? &g->keeps : NULL;
    if (st == SHR_OK) st = plan(g, keeps, dst, cmds ? cmds + first : NULL, count - first, &x, &staging, &draws);
    if (st == SHR_OK && (!shr__rect_empty(x.area) || g->keeps.nstores) && (st = reserve(g, staging, draws)) == SHR_OK) {
        if (!gl) prepare(g);
        gl = true;
        if ((st = keep_alloc(g, dst->format)) == SHR_OK) render(g, &x, cmds + first, count - first);
    }
    shr_status done = gl ? gl_status(g) : SHR_OK;
    return done != SHR_OK ? done : st;
}

static shr_status gl_reset(void *user) {
    (void)user;
    return SHR_OK;
}

static gl_drv *drv(const shr_framebuffer_driver *d) { return d && d->execute == gl_execute ? d->user : NULL; }

static void surf_free(gl_drv *g, surf *s) {
    surf **p = &g->surfs;
    while (*p != s) p = &(*p)->next;
    *p = s->next;
    glDeleteFramebuffers(1, &s->fbo);
    glDeleteTextures(1, &s->tex);
    SHR_DELETE(&g->al, s, surf);
}

static void teardown(gl_drv *g) {
    if (g->slots) drop_textures(g);
    while (g->surfs) surf_free(g, g->surfs);
    glDeleteFramebuffers(1, &g->fbo);
    glDeleteFramebuffers(1, &g->keep_fbo);
    glDeleteBuffers(1, &g->vbo);
    glDeleteBuffers(1, &g->quad);
    glDeleteVertexArrays(1, &g->vao);
    glDeleteProgram(g->prog);
    shr__free(&g->al, g->staging, g->staging_bytes, 16, SHR_ALLOC_PAYLOAD);
    SHR_FREE_ARRAY(&g->al, g->inst, inst, g->cap);
    SHR_FREE_ARRAY(&g->al, g->slots, slot, g->nbuffers);
    SHR_FREE_ARRAY(&g->al, g->bufs, shr_image, g->nbuffers);
    SHR_FREE_ARRAY(&g->al, g->keeps.at, shr__keep, g->keeps.n);
    SHR_FREE_ARRAY(&g->al, g->keeps.stores, uint32_t, g->keeps.n);
    shr__alloc al = g->al;
    SHR_DELETE(&al, g, gl_drv);
}

static void shader(GLuint prog, GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glAttachShader(prog, s);
    glDeleteShader(s);
}

/* Four components of each instance at `offset`: integers, or normalized. */
static void attrib(GLuint i, GLenum type, bool normalized, size_t offset) {
    glEnableVertexAttribArray(i);
    if (normalized)
        glVertexAttribPointer(i, 4, type, GL_TRUE, (GLsizei)sizeof(inst), (const void *)offset);
    else
        glVertexAttribIPointer(i, 4, type, (GLsizei)sizeof(inst), (const void *)offset);
    glVertexAttribDivisor(i, 1);
}

static void setup(gl_drv *g) {
    g->prog = glCreateProgram();
    shader(g->prog, GL_VERTEX_SHADER, vs_src);
    shader(g->prog, GL_FRAGMENT_SHADER, fs_src);
    glLinkProgram(g->prog);
    glUseProgram(g->prog);
    g->scale = glGetUniformLocation(g->prog, "u_scale");
    g->origin = glGetUniformLocation(g->prog, "u_origin");
    g->levels = glGetUniformLocation(g->prog, "u_levels");
    static const GLint units[UNITS] = {0, 1, 2, 3, 4, 5, 6, 7};
    glUniform1iv(glGetUniformLocation(g->prog, "u_buf"), UNITS, units);
    glUniform1i(glGetUniformLocation(g->prog, "u_src"), SRC_UNIT);
    glGenVertexArrays(1, &g->vao);
    glBindVertexArray(g->vao);
    static const uint8_t corners[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &g->quad);
    glBindBuffer(GL_ARRAY_BUFFER, g->quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_UNSIGNED_BYTE, GL_FALSE, 2, NULL);
    glGenBuffers(1, &g->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    attrib(1, GL_SHORT, false, offsetof(inst, dst));
    attrib(2, GL_SHORT, false, offsetof(inst, clip));
    attrib(3, GL_SHORT, false, offsetof(inst, rect));
    attrib(4, GL_SHORT, false, offsetof(inst, src));
    attrib(5, GL_UNSIGNED_BYTE, false, offsetof(inst, unit));
    attrib(6, GL_UNSIGNED_BYTE, true, offsetof(inst, rgba));
    glGenFramebuffers(1, &g->fbo);
    glGenFramebuffers(1, &g->keep_fbo);
    /* ANGLE's viewport and renderbuffer limits are not below its texture size; min(that, MAX_SIZE). */
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g->max);
    g->max -= (g->max - MAX_SIZE) * (g->max > MAX_SIZE);
}

shr_status shr_angle_driver_create(const shr_allocator *allocator, uint64_t texture_cache_bytes, uint64_t keep_bytes,
                                   uint32_t max_keeps, uint32_t max_buffers, shr_framebuffer_driver *out) {
    if (!out) return SHR_E_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    uint64_t keep_slot = max_keeps ? keep_bytes / max_keeps / 128 * 128 : 0;
    shr__alloc al;
    if (!shr__alloc_init(&al, allocator) || (max_keeps && !keep_slot)) return SHR_E_INVALID_ARG;
    EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return SHR_E_STATE;
    gl_drv *g = SHR_NEW(&al, gl_drv);
    if (!g) return SHR_E_NO_MEMORY;
    *g = (gl_drv){.al = al, .context = context, .budget = texture_cache_bytes, .nbuffers = max_buffers,
                  .keeps = {.n = max_keeps, .slot = keep_slot}};
    g->bufs = SHR_NEW_ARRAY(&al, shr_image, max_buffers);
    g->slots = g->bufs ? SHR_NEW_ARRAY(&al, slot, max_buffers) : NULL;
    g->keeps.at = g->slots ? SHR_NEW_ARRAY(&al, shr__keep, max_keeps) : NULL;
    g->keeps.stores = g->keeps.at ? SHR_NEW_ARRAY(&al, uint32_t, max_keeps) : NULL;
    g->inst = g->keeps.stores ? shr__malloc(&al, MIN_INST * sizeof(inst), SHR_ALIGNOF(inst), SHR_ALLOC_PAYLOAD) : NULL;
    if (!g->inst) {
        teardown(g);
        return SHR_E_NO_MEMORY;
    }
    g->cap = MIN_INST;
    prepare(g);
    setup(g);
    GLint linked = 0, layers = 0;
    glGetProgramiv(g->prog, GL_LINK_STATUS, &linked);
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &layers);
    if (!linked || gl_status(g) != SHR_OK) {
        teardown(g);
        return SHR_E_DEVICE;
    }
    uint32_t max_layers = layers < 256 ? (uint32_t)layers : 256; /* inst.layer */
    for (int k = TEX_RGBA8; k < TEX_KINDS; k++) /* the destination kinds */
        if (max_keeps && !atlas_init(&g->atlas[k], k, keep_slot, max_keeps, g->max, max_layers)) {
            teardown(g);
            return SHR_E_UNSUPPORTED;
        }
    shr_framebuffer_driver_init(out);
    out->user = g;
    out->caps.domains = SHR_MEMORY_CPU | SHR_MEMORY_DEVICE;
    out->caps.max_width = out->caps.max_height = g->max;
    out->caps.max_buffers = max_buffers;
    out->caps.max_buffer_width = out->caps.max_buffer_height = g->max;
    out->caps.buffer_bytes = texture_cache_bytes;
    out->caps.buffer_flags = SHR_BUFFER_COPIES;
    out->caps.max_keeps = max_keeps;
    out->caps.keep_bytes = keep_bytes;
    out->caps.max_keep_bytes = keep_slot;
    out->caps.flags = SHR_DRIVER_CHEAP_MOVE;
    out->execute = gl_execute;
    out->reset = gl_reset;
    return SHR_OK;
}

shr_status shr_angle_driver_destroy(shr_framebuffer_driver *driver) {
    gl_drv *g = drv(driver);
    if (!g) return SHR_E_INVALID_ARG;
    shr_status st = current(g);
    if (st != SHR_OK) return st;
    teardown(g);
    memset(driver, 0, sizeof(*driver));
    return SHR_OK;
}

shr_status shr_angle_driver_stats(const shr_framebuffer_driver *driver, shr_angle_stats *out) {
    gl_drv *g = drv(driver);
    if (!g || !out) return SHR_E_INVALID_ARG;
    *out = g->stats;
    return SHR_OK;
}

shr_status shr_angle_surface_create(shr_framebuffer_driver *driver, int32_t width, int32_t height,
                                    shr_pixel_format format, shr_surface *out) {
    if (!out) return SHR_E_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    gl_drv *g = drv(driver);
    if (!g || width <= 0 || height <= 0 || (format != SHR_FORMAT_RGB565 && format != SHR_FORMAT_RGBX8888))
        return SHR_E_INVALID_ARG;
    shr_status st = current(g);
    if (st != SHR_OK) return st;
    if (width > g->max || height > g->max) return SHR_E_UNSUPPORTED;
    surf *s = SHR_NEW(&g->al, surf);
    if (!s) return SHR_E_NO_MEMORY;
    *s = (surf){g->surfs, 0, 0, width, height, format};
    g->surfs = s;
    prepare(g);
    s->tex = new_texture(g, tex_kind(format), width, height);
    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (gl_status(g) != SHR_OK) {
        surf_free(g, s);
        return SHR_E_DEVICE;
    }
    size_t row = (size_t)width * shr__px_bytes(format);
    *out = (shr_surface){s, width, height, row, row * (size_t)height, format, 0, SHR_MEMORY_DEVICE, ++g->next_id};
    return SHR_OK;
}

/* The surface of `driver` that `s` describes; SHR_E_STATE with another EGL context current. */
static shr_status own_surface(const shr_framebuffer_driver *driver, const shr_surface *s, gl_drv **g, surf **out) {
    *g = drv(driver);
    *out = *g && s && s->domain == SHR_MEMORY_DEVICE ? device_surf(*g, s->pixels, s->width, s->height, s->format) : NULL;
    return !*out ? SHR_E_INVALID_ARG : current(*g);
}

shr_status shr_angle_surface_destroy(shr_framebuffer_driver *driver, shr_surface *surface) {
    gl_drv *g;
    surf *s;
    shr_status st = own_surface(driver, surface, &g, &s);
    if (st != SHR_OK) return st;
    surf_free(g, s);
    memset(surface, 0, sizeof(*surface));
    return SHR_OK;
}

shr_status shr_angle_surface_read(shr_framebuffer_driver *driver, const shr_surface *surface, void *pixels,
                                  size_t stride) {
    gl_drv *g;
    surf *s;
    shr_status st = own_surface(driver, surface, &g, &s);
    if (st != SHR_OK) return st;
    if (!pixels || stride < (size_t)s->w * shr__px_bytes(s->format)) return SHR_E_INVALID_ARG;
    if ((st = reserve(g, 4 * texels((shr_rect){0, 0, s->w, s->h}), 0)) != SHR_OK) return st;
    prepare(g);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    read_back(g, s->w, s->h, s->format, pixels, stride);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return gl_status(g);
}

shr_status shr_angle_surface_texture(shr_framebuffer_driver *driver, const shr_surface *surface, uint32_t *texture) {
    gl_drv *g;
    surf *s;
    if (!texture) return SHR_E_INVALID_ARG;
    shr_status st = own_surface(driver, surface, &g, &s);
    if (st != SHR_OK) return st;
    *texture = s->tex;
    return SHR_OK;
}
