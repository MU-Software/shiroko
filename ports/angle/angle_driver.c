#include <shiroko/port_angle.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "raster.h"
#include "shr_alloc.h"
#include "shr_hash.h"
#include "shr_lru.h"
#include "shr_rect.h"

typedef struct gl_drv gl_drv;
static void *table_alloc(gl_drv *g, size_t bytes);
static void table_free(gl_drv *g, void *p, size_t bytes);

/* Keys are content hashes already, so their low bits pick the bucket. */
#define HASH_NONFATAL_OOM 1
#define HASH_FUNCTION(keyptr, keylen, hashv) ((void)(keylen), (hashv) = (unsigned)((const tex_id *)(keyptr))->hash[0])
#define uthash_malloc(bytes) table_alloc(g, (bytes))
#define uthash_free(p, bytes) table_free(g, (p), (bytes))
#include <uthash.h>

/* Surfaces and cached sources stay within it, so w * h * 4 of anything the driver allocates fits 32 bits. */
#define MAX_SIZE 16384

enum { TEX_R8, TEX_RGBA8, TEX_RGB565, TEX_KINDS };

static const struct {
    GLenum internal, format, type;
    uint32_t bpp;
} tex_formats[TEX_KINDS] = {{GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1},
                            {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4},
                            {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2}};

/* A4 is expanded to R8; RGBX8888 keeps X in alpha, which the driver never writes. */
static int tex_kind(shr_pixel_format f) {
    return f == SHR_FORMAT_RGB565 ? TEX_RGB565 : f == SHR_FORMAT_A4 || f == SHR_FORMAT_A8 ? TEX_R8 : TEX_RGBA8;
}

enum { MODE_FILL, MODE_GLYPH, MODE_GLYPH_DIM, MODE_IMAGE, MODE_COPY };

typedef struct vtx {
    float x, y, u, v;
    uint8_t rgba[4];
    uint8_t mode, pad[3];
} vtx;

#define MAX_VERTS (6 * 1024)

/* A DEVICE surface; its address is the surface's `pixels`. */
typedef struct surf {
    struct surf *next;
    GLuint tex, fbo;
    int32_t w, h;
    shr_pixel_format format;
} surf;

typedef struct tex_id {
    uint64_t hash[2];
    int32_t width, height;
    uint32_t format, zero;
} tex_id;

/* Texture of one glyph or image source. */
typedef struct tex_entry {
    UT_hash_handle hh;
    shr__lru_node lru;
    tex_id id;
    size_t bytes;
    GLuint tex;
} tex_entry;

typedef struct scratch {
    GLuint tex;
    int32_t w, h;
} scratch;

/* An image source hashed in this batch (zero padded, compared whole). */
typedef struct memo {
    const void *pixels;
    size_t stride;
    int32_t width, height;
    uint32_t format, zero;
    uint64_t hash[2];
} memo;

#define MEMOS 4

/* `used` counts cache entries, their texture bytes and the hash table. `bound` is the texture the pending
 * vertices sample. */
struct gl_drv {
    shr__alloc al;
    EGLContext context;
    uint64_t budget, used, next_id;
    shr__lru_node *lru;
    tex_entry *table;
    surf *surfs;
    GLint max;
    GLuint prog, vao, vbo, fbo, bound;
    GLint scale, levels;
    scratch src[TEX_KINDS], dst[TEX_KINDS];
    memo memos[MEMOS];
    unsigned next_memo;
    uint8_t *staging;
    size_t staging_bytes, nverts;
    vtx *verts;
};

#include "tex_table.h"

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
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "layout(location = 2) in vec4 a_color;\n"
    "layout(location = 3) in uint a_mode;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "flat out uint v_mode;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "    v_mode = a_mode;\n"
    "    gl_Position = vec4(a_pos * u_scale - 1.0, 0.0, 1.0);\n"
    "}\n";

/* Blending is source-over with the output alpha; coverage is rounded like the software port's. Opaque
 * colours are quantized here to the target's levels (u_levels): GPUs may convert outputs at half precision,
 * which would round some 8-bit values to the wrong 5/6-bit level. */
static const char *const fs_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform highp sampler2D u_tex;\n"
    "uniform vec3 u_levels;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "flat in uint v_mode;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    if (v_mode == 0u) {\n"
    "        o = v_color;\n"
    "    } else {\n"
    "        vec4 t = texelFetch(u_tex, ivec2(v_uv), 0);\n"
    "        float c = floor(t.r * 255.0 + 0.5);\n"
    "        if (v_mode == 2u) c = floor((c + 1.0) * 0.5);\n"
    "        o = v_mode == 3u ? t : v_mode == 4u ? vec4(t.rgb, 1.0) : vec4(v_color.rgb, c / 255.0);\n"
    "    }\n"
    "    if (o.a == 1.0) o.rgb = floor((floor(o.rgb * 255.0 + 0.5) * u_levels + 127.0) / 255.0) / u_levels;\n"
    "}\n";

static void *table_alloc(gl_drv *g, size_t bytes) {
    void *p = shr__malloc(&g->al, bytes, SHR_ALIGNOF(void *), SHR_ALLOC_DESCRIPTOR);
    if (p) g->used += bytes;
    return p;
}

static void table_free(gl_drv *g, void *p, size_t bytes) {
    shr__free(&g->al, p, bytes, SHR_ALIGNOF(void *), SHR_ALLOC_DESCRIPTOR);
    g->used -= bytes;
}

/* What uthash allocates with the first entry. */
#define TABLE_BYTES (sizeof(UT_hash_table) + HASH_INITIAL_NUM_BUCKETS * sizeof(UT_hash_bucket))

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

/* The source texel corner that destination corner (x, y) maps to. */
static shr_point src_point(const shr_draw_cmd *c, int32_t x, int32_t y) {
    int32_t w = c->src.width, h = c->src.height;
    if (c->kind != SHR_CMD_ROTATE) return (shr_point){c->src_origin.x + x - c->dst.x0, c->src_origin.y + y - c->dst.y0};
    switch (c->rotation) {
    case SHR_ROTATE_90_CW: return (shr_point){y, h - x};
    case SHR_ROTATE_90_CCW: return (shr_point){w - y, x};
    default: return (shr_point){w - x, h - y};
    }
}

static shr_rect src_rect(const shr_draw_cmd *c) {
    shr_point a = src_point(c, c->dst.x0, c->dst.y0), b = src_point(c, c->dst.x1, c->dst.y1);
    return (shr_rect){a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y};
}

/* Callers pass rects within MAX_SIZE. */
static size_t texels(shr_rect r) { return (size_t)(r.x1 - r.x0) * (size_t)(r.y1 - r.y0); }

/* Glyph and image sources are kept whole in the cache when they fit MAX_SIZE and half of the budget. */
static bool cacheable(const gl_drv *g, const shr_draw_cmd *c) {
    const shr_image *m = &c->src;
    uint64_t room = g->budget > TABLE_BYTES ? g->budget - TABLE_BYTES : 0;
    return (c->kind == SHR_CMD_GLYPH || c->kind == SHR_CMD_IMAGE) && m->width <= g->max && m->height <= g->max &&
           sizeof(tex_entry) + (uint64_t)m->width * (uint64_t)m->height * tex_formats[tex_kind(m->format)].bpp <= room / 2;
}

/* Uploads that GL cannot read in place: A4, rows that are not whole pixels, misaligned 16-bit pixels. */
static bool staged(const shr_image *m) {
    uint32_t bpp = tex_formats[tex_kind(m->format)].bpp;
    return m->format == SHR_FORMAT_A4 || m->stride % bpp || m->stride / bpp > INT32_MAX ||
           (bpp == 2 && (uintptr_t)m->pixels % 2);
}

static size_t src_staging(const gl_drv *g, const shr_draw_cmd *c) {
    if (c->src.domain == SHR_MEMORY_DEVICE || !staged(&c->src)) return 0;
    return 4 * (cacheable(g, c) ? texels((shr_rect){0, 0, c->src.width, c->src.height}) : texels(src_rect(c)));
}

/* The software port's checks with this driver's reach; also the destination area and the staging bytes the
 * batch needs, so nothing fails once drawing started. */
static shr_status plan(const gl_drv *g, const shr_surface *dst, const shr_draw_cmd *cmds, size_t count, run *x,
                       size_t *staging) {
    shr_status st = shr__raster_check(dst, cmds, count, reach, g);
    if (st != SHR_OK) return st;
    if (dst->width > g->max || dst->height > g->max) return SHR_E_UNSUPPORTED;
    *x = (run){dst, dst->domain == SHR_MEMORY_DEVICE ? find(g, dst->pixels) : NULL, {0, 0, 0, 0}, {0, 0}};
    *staging = 0;
    bool group = false;
    for (size_t i = 0; i < count; i++) {
        const shr_draw_cmd *c = &cmds[i];
        if (c->kind == SHR_CMD_CACHE_BEGIN) {
            group = true;
            x->area = shr__rect_union(x->area, c->cache_clip);
            continue;
        }
        if (c->kind == SHR_CMD_CACHE_END) {
            group = false;
            continue;
        }
        if (!group) x->area = shr__rect_union(x->area, c->dst);
        if (c->kind != SHR_CMD_FILL && src_staging(g, c) > *staging) *staging = src_staging(g, c);
    }
    if (!x->target && 4 * texels(x->area) > *staging) *staging = 4 * texels(x->area);
    return SHR_OK;
}

static shr_status reserve(gl_drv *g, size_t bytes) {
    if (bytes <= g->staging_bytes) return SHR_OK;
    uint8_t *p = shr__malloc(&g->al, bytes, 16, SHR_ALLOC_PAYLOAD);
    if (!p) return SHR_E_NO_MEMORY;
    shr__free(&g->al, g->staging, g->staging_bytes, 16, SHR_ALLOC_PAYLOAD);
    g->staging = p, g->staging_bytes = bytes;
    return SHR_OK;
}

/* Errors the application left behind would be taken for ours. */
static void prepare(gl_drv *g) {
    while (glGetError() != GL_NO_ERROR) {
    }
    static const GLenum off[] = {GL_DEPTH_TEST,      GL_STENCIL_TEST,          GL_CULL_FACE,       GL_DITHER,
                                 GL_RASTERIZER_DISCARD, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE, GL_POLYGON_OFFSET_FILL};
    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++) glDisable(off[i]);
    static const GLenum zero[] = {GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,  GL_UNPACK_SKIP_PIXELS, GL_UNPACK_IMAGE_HEIGHT,
                                  GL_UNPACK_SKIP_IMAGES, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS,     GL_PACK_SKIP_PIXELS};
    for (size_t i = 0; i < sizeof(zero) / sizeof(zero[0]); i++) glPixelStorei(zero[i], 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glEnable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glUseProgram(g->prog);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    g->bound = 0;
}

static void entry_free(gl_drv *g, tex_entry *e) {
    table_remove(g, e);
    shr__lru_remove(&g->lru, &e->lru);
    g->used -= sizeof(tex_entry) + e->bytes;
    glDeleteTextures(1, &e->tex);
    g->bound = 0;
    SHR_DELETE(&g->al, e, tex_entry);
}

/* Cached and scratch textures: after a GL error their storage is unknown. */
static void drop_textures(gl_drv *g) {
    while (g->lru) entry_free(g, SHR_CONTAINER(g->lru, tex_entry, lru));
    for (int k = 0; k < TEX_KINDS; k++) {
        glDeleteTextures(1, &g->src[k].tex);
        glDeleteTextures(1, &g->dst[k].tex);
        g->src[k] = g->dst[k] = (scratch){0, 0, 0};
    }
}

static shr_status gl_status(gl_drv *g) {
    if (glGetError() == GL_NO_ERROR) return SHR_OK;
    drop_textures(g);
    return SHR_E_DEVICE;
}

static void flush(gl_drv *g) {
    if (!g->nverts) return;
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g->nverts * sizeof(vtx)), g->verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g->nverts);
    g->nverts = 0;
}

static void bind(gl_drv *g, GLuint t) {
    if (t == g->bound) return;
    flush(g);
    glBindTexture(GL_TEXTURE_2D, t);
    g->bound = t;
}

/* Bound explicitly: the name may be one just deleted that `bound` still holds. */
static GLuint new_texture(gl_drv *g, int kind, int32_t w, int32_t h) {
    GLuint t;
    flush(g);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    g->bound = t;
    glTexStorage2D(GL_TEXTURE_2D, 1, tex_formats[kind].internal, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

/* Binds a texture of at least w x h; its contents change, so pending draws go first. */
static GLuint scratch_bind(gl_drv *g, scratch *s, int kind, int32_t w, int32_t h) {
    flush(g);
    if (w > s->w || h > s->h) {
        glDeleteTextures(1, &s->tex);
        s->w = w > s->w ? w : s->w, s->h = h > s->h ? h : s->h;
        s->tex = new_texture(g, kind, s->w, s->h);
    }
    bind(g, s->tex);
    return s->tex;
}

/* Rect `r` of `m` to (0, 0) of the bound texture. */
static void upload(gl_drv *g, const shr_image *m, shr_rect r) {
    int kind = tex_kind(m->format);
    size_t bpp = tex_formats[kind].bpp;
    int32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    const uint8_t *base = m->pixels;
    const void *data = base + (size_t)r.y0 * m->stride + (size_t)r.x0 * bpp;
    GLint row = 0;
    if (staged(m)) {
        for (int32_t y = 0; y < h; y++) {
            const uint8_t *p = base + (size_t)(r.y0 + y) * m->stride;
            uint8_t *d = g->staging + (size_t)y * (size_t)w * bpp;
            if (m->format != SHR_FORMAT_A4) {
                memcpy(d, p + (size_t)r.x0 * bpp, (size_t)w * bpp);
                continue;
            }
            for (int32_t x = 0; x < w; x++) {
                int32_t sx = r.x0 + x;
                d[x] = (uint8_t)(17 * (sx % 2 ? p[sx / 2] & 0x0F : p[sx / 2] >> 4));
            }
        }
        data = g->staging;
    } else {
        row = (GLint)(m->stride / bpp);
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

/* Writes what was drawn so far into a CPU destination; sources in it may change, so hashes are forgotten. */
static void sync(gl_drv *g, const run *x) {
    flush(g);
    const shr_surface *d = x->dst;
    read_back(g, x->area.x1 - x->area.x0, x->area.y1 - x->area.y0, d->format,
              (uint8_t *)d->pixels + (size_t)x->area.y0 * d->stride + (size_t)x->area.x0 * shr__px_bytes(d->format),
              d->stride);
    memset(g->memos, 0, sizeof(g->memos));
}

/* An image is drawn once per damage rectangle, so image hashes are kept for the batch. */
static void source_hash(gl_drv *g, const shr_draw_cmd *c, uint64_t out[2]) {
    const shr_image *m = &c->src;
    memo key = {m->pixels, m->stride, m->width, m->height, (uint32_t)m->format, 0, {0, 0}};
    for (int i = 0; i < MEMOS; i++) {
        if (memcmp(&key, &g->memos[i], offsetof(memo, hash))) continue;
        out[0] = g->memos[i].hash[0], out[1] = g->memos[i].hash[1];
        return;
    }
    size_t row = 0;
    shr_format_row_bytes(m->format, m->width, &row);
    XXH3_state_t st;
    XXH3_128bits_reset(&st);
    for (int32_t y = 0; y < m->height; y++) XXH3_128bits_update(&st, (const uint8_t *)m->pixels + (size_t)y * m->stride, row);
    XXH128_hash_t h = XXH3_128bits_digest(&st);
    out[0] = h.low64, out[1] = h.high64;
    if (c->kind != SHR_CMD_IMAGE) return;
    key.hash[0] = out[0], key.hash[1] = out[1];
    g->memos[g->next_memo++ % MEMOS] = key;
}

/* The cached texture of a whole source; 0 when it cannot be stored. */
static GLuint cached_texture(gl_drv *g, const shr_draw_cmd *c) {
    const shr_image *m = &c->src;
    tex_id id = {{0, 0}, m->width, m->height, (uint32_t)m->format, 0};
    source_hash(g, c, id.hash);
    tex_entry *e = table_find(g, &id);
    if (e) {
        shr__lru_remove(&g->lru, &e->lru);
        shr__lru_push(&g->lru, &e->lru);
        return e->tex;
    }
    /* Evicted textures may be what the pending vertices sample. */
    flush(g);
    size_t bytes = texels((shr_rect){0, 0, m->width, m->height}) * tex_formats[tex_kind(m->format)].bpp;
    while (g->used + sizeof(tex_entry) + bytes > g->budget)
        entry_free(g, SHR_CONTAINER(shr__lru_oldest(g->lru), tex_entry, lru));
    e = SHR_NEW(&g->al, tex_entry);
    if (!e) return 0;
    e->id = id;
    if (!table_add(g, e)) {
        SHR_DELETE(&g->al, e, tex_entry);
        return 0;
    }
    e->bytes = bytes;
    e->tex = new_texture(g, tex_kind(m->format), m->width, m->height);
    upload(g, m, (shr_rect){0, 0, m->width, m->height});
    shr__lru_push(&g->lru, &e->lru);
    g->used += sizeof(tex_entry) + bytes;
    return e->tex;
}

/* The texture a command reads and the source texel at its (0, 0). */
static GLuint source(gl_drv *g, const run *x, const shr_draw_cmd *c, shr_point *off) {
    const shr_image *m = &c->src;
    shr_rect s = src_rect(c);
    int kind = tex_kind(m->format);
    if (m->domain == SHR_MEMORY_DEVICE) {
        const surf *f = find(g, m->pixels);
        if (f != x->target) return f->tex;
        /* Reading the surface being drawn is a feedback loop: copy the part read first. */
        GLuint t = scratch_bind(g, &g->src[kind], kind, s.x1 - s.x0, s.y1 - s.y0);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.x0, s.y0, s.x1 - s.x0, s.y1 - s.y0);
        *off = (shr_point){s.x0, s.y0};
        return t;
    }
    if (!x->target && shr__raster_reads_dst(x->dst, m)) sync(g, x);
    GLuint t = cacheable(g, c) ? cached_texture(g, c) : 0;
    if (t) return t;
    t = scratch_bind(g, &g->src[kind], kind, s.x1 - s.x0, s.y1 - s.y0);
    upload(g, m, s);
    *off = (shr_point){s.x0, s.y0};
    return t;
}

static void quad(gl_drv *g, const run *x, const shr_draw_cmd *c, shr_point off, uint8_t alpha, uint8_t mode) {
    if (g->nverts + 6 > MAX_VERTS) flush(g);
    shr_rect r = c->dst;
    const int32_t xs[4] = {r.x0, r.x1, r.x0, r.x1}, ys[4] = {r.y0, r.y0, r.y1, r.y1};
    static const int order[6] = {0, 1, 2, 1, 3, 2};
    vtx v[4];
    for (int i = 0; i < 4; i++) {
        shr_point uv = src_point(c, xs[i], ys[i]);
        v[i] = (vtx){(float)(xs[i] - x->origin.x), (float)(ys[i] - x->origin.y), (float)(uv.x - off.x),
                     (float)(uv.y - off.y),
                     {(uint8_t)(c->color >> 16), (uint8_t)(c->color >> 8), (uint8_t)c->color, alpha}, mode, {0}};
    }
    for (int i = 0; i < 6; i++) g->verts[g->nverts++] = v[order[i]];
}

static void draw(gl_drv *g, const run *x, const shr_draw_cmd *c) {
    if (shr__rect_empty(c->dst)) return;
    shr_point off = {0, 0};
    if (c->kind == SHR_CMD_FILL) {
        quad(g, x, c, off, c->flags & SHR_GLYPH_DIM ? 128 : 255, MODE_FILL);
        return;
    }
    bind(g, source(g, x, c, &off));
    int mode = c->kind == SHR_CMD_GLYPH ? (c->flags & SHR_GLYPH_DIM ? MODE_GLYPH_DIM : MODE_GLYPH)
               : c->kind == SHR_CMD_IMAGE ? MODE_IMAGE
                                          : MODE_COPY;
    quad(g, x, c, off, 255, (uint8_t)mode);
}

static void clip(gl_drv *g, const run *x, shr_rect r) {
    flush(g);
    glScissor(r.x0 - x->origin.x, r.y0 - x->origin.y, r.x1 - r.x0, r.y1 - r.y0);
}

/* Binds the destination's framebuffer; a CPU destination's area is uploaded into a scratch texture. */
static void target(gl_drv *g, run *x) {
    int32_t w = x->dst->width, h = x->dst->height;
    if (x->target) {
        glBindFramebuffer(GL_FRAMEBUFFER, x->target->fbo);
    } else {
        int kind = tex_kind(x->dst->format);
        scratch *s = &g->dst[kind];
        scratch_bind(g, s, kind, x->area.x1 - x->area.x0, x->area.y1 - x->area.y0);
        const shr_surface *d = x->dst;
        upload(g, &(shr_image){d->pixels, d->width, d->height, d->stride, d->byte_length, d->format, d->domain}, x->area);
        bind(g, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
        x->origin = (shr_point){x->area.x0, x->area.y0};
        w = s->w, h = s->h;
    }
    glViewport(0, 0, w, h);
    glUniform2f(g->scale, 2.0f / (float)w, 2.0f / (float)h);
    bool rgb565 = x->dst->format == SHR_FORMAT_RGB565;
    glUniform3f(g->levels, rgb565 ? 31.0f : 255.0f, rgb565 ? 63.0f : 255.0f, rgb565 ? 31.0f : 255.0f);
    clip(g, x, (shr_rect){0, 0, x->dst->width, x->dst->height});
}

static shr_status gl_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t count,
                             shr_fence fence) {
    (void)fence;
    gl_drv *g = user;
    run x;
    size_t staging;
    shr_status st = current(g);
    if (st != SHR_OK || (st = plan(g, dst, cmds, count, &x, &staging)) != SHR_OK || shr__rect_empty(x.area)) return st;
    if ((st = reserve(g, staging)) != SHR_OK) return st;
    prepare(g);
    memset(g->memos, 0, sizeof(g->memos));
    target(g, &x);
    shr_rect all = {0, 0, dst->width, dst->height};
    for (size_t i = 0; i < count; i++) {
        const shr_draw_cmd *c = &cmds[i];
        if (c->kind == SHR_CMD_CACHE_BEGIN) {
            if (shr__rect_empty(c->cache_clip))
                while (cmds[i + 1].kind != SHR_CMD_CACHE_END) i++;
            else
                clip(g, &x, c->cache_clip);
        } else if (c->kind == SHR_CMD_CACHE_END) {
            clip(g, &x, all);
        } else {
            draw(g, &x, c);
        }
    }
    flush(g);
    if (!x.target) sync(g, &x);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return gl_status(g);
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
    drop_textures(g);
    while (g->surfs) surf_free(g, g->surfs);
    glDeleteFramebuffers(1, &g->fbo);
    glDeleteBuffers(1, &g->vbo);
    glDeleteVertexArrays(1, &g->vao);
    glDeleteProgram(g->prog);
    shr__free(&g->al, g->staging, g->staging_bytes, 16, SHR_ALLOC_PAYLOAD);
    SHR_FREE_ARRAY(&g->al, g->verts, vtx, MAX_VERTS);
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

shr_status shr_angle_driver_create(const shr_allocator *allocator, uint64_t texture_cache_bytes,
                                   shr_framebuffer_driver *out) {
    if (!out) return SHR_E_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    shr__alloc al;
    if (!shr__alloc_init(&al, allocator)) return SHR_E_INVALID_ARG;
    EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return SHR_E_STATE;
    gl_drv *g = SHR_NEW(&al, gl_drv);
    vtx *verts = g ? SHR_NEW_ARRAY(&al, vtx, MAX_VERTS) : NULL;
    if (!verts) {
        SHR_DELETE(&al, g, gl_drv);
        return SHR_E_NO_MEMORY;
    }
    *g = (gl_drv){.al = al, .context = context, .budget = texture_cache_bytes, .verts = verts};
    prepare(g);
    g->prog = glCreateProgram();
    shader(g->prog, GL_VERTEX_SHADER, vs_src);
    shader(g->prog, GL_FRAGMENT_SHADER, fs_src);
    glLinkProgram(g->prog);
    GLint linked = 0;
    glGetProgramiv(g->prog, GL_LINK_STATUS, &linked);
    g->scale = glGetUniformLocation(g->prog, "u_scale");
    g->levels = glGetUniformLocation(g->prog, "u_levels");
    glGenVertexArrays(1, &g->vao);
    glBindVertexArray(g->vao);
    glGenBuffers(1, &g->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    for (GLuint i = 0; i < 4; i++) glEnableVertexAttribArray(i);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(vtx), (const void *)offsetof(vtx, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(vtx), (const void *)offsetof(vtx, u));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)sizeof(vtx), (const void *)offsetof(vtx, rgba));
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_BYTE, (GLsizei)sizeof(vtx), (const void *)offsetof(vtx, mode));
    glGenFramebuffers(1, &g->fbo);
    /* ANGLE's viewport and renderbuffer limits are not below its texture size; min(that, MAX_SIZE). */
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g->max);
    g->max -= (g->max - MAX_SIZE) * (g->max > MAX_SIZE);
    if (!linked || gl_status(g) != SHR_OK) {
        teardown(g);
        return SHR_E_DEVICE;
    }
    shr_framebuffer_driver_init(out);
    out->user = g;
    out->caps.domains = SHR_MEMORY_CPU | SHR_MEMORY_DEVICE;
    out->caps.max_width = out->caps.max_height = g->max;
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
    glDisable(GL_SCISSOR_TEST);
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
    if ((st = reserve(g, 4 * texels((shr_rect){0, 0, s->w, s->h}))) != SHR_OK) return st;
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
