#include <shiroko/port_software.h>

#include "raster.h"
#include "shr_alloc.h"
#include "shr_lru.h"
#include "shr_rect.h"

typedef struct sw sw;
static void *table_alloc(sw *s, size_t bytes);
static void table_free(sw *s, void *p, size_t bytes);

/* Keys are content hashes already, so their low bits pick the bucket. */
#define HASH_NONFATAL_OOM 1
#define HASH_FUNCTION(keyptr, keylen, hashv) ((void)(keylen), (hashv) = (unsigned)((const entry_id *)(keyptr))->key[0])
#define uthash_malloc(bytes) table_alloc(s, (bytes))
#define uthash_free(p, bytes) table_free(s, (p), (bytes))
#include <uthash.h>

typedef struct entry_id {
    uint64_t key[2];
    int32_t width, height;
    uint32_t format, zero;
} entry_id;

/* Cached pixels of one CACHE_BEGIN group. */
typedef struct entry {
    UT_hash_handle hh;
    shr__lru_node lru;
    entry_id id;
    size_t bytes;
    uint8_t *pixels;
} entry;

/* `used` counts entries, their pixels and the hash table. */
struct sw {
    shr__alloc al;
    uint64_t budget, used;
    shr__lru_node *lru;
    entry *table;
    shr_image *buffers;
    uint32_t nbuffers;
};

#include "cache_table.h"

shr_status shr_software_execute(const shr_surface *dst, const shr_draw_cmd *cmds, size_t count, const shr_image *buffers,
                                uint32_t nbuffers) SHR_NONBLOCKING {
    shr_status st = shr__raster_check(dst, cmds, count, buffers, nbuffers, NULL, NULL);
    if (st != SHR_OK) return st;
    shr_rect all = {0, 0, dst->width, dst->height}, clip = all;
    for (size_t i = 0; i < count; i++) {
        if (cmds[i].kind == SHR_CMD_CACHE_BEGIN) clip = cmds[i].cache_clip;
        else if (cmds[i].kind == SHR_CMD_CACHE_END) clip = all;
        else if (!shr__buffer_cmd(cmds[i].kind)) shr__raster_draw(dst, &cmds[i], buffers, (shr_point){0, 0}, clip);
    }
    return SHR_OK;
}

static void *table_alloc(sw *s, size_t bytes) {
    void *p = shr__malloc(&s->al, bytes, SHR_ALIGNOF(void *), SHR_ALLOC_DESCRIPTOR);
    if (p) s->used += bytes;
    return p;
}

static void table_free(sw *s, void *p, size_t bytes) {
    shr__free(&s->al, p, bytes, SHR_ALIGNOF(void *), SHR_ALLOC_DESCRIPTOR);
    s->used -= bytes;
}

static void entry_discard(sw *s, entry *e) {
    shr__free(&s->al, e->pixels, e->bytes, 16, SHR_ALLOC_PAYLOAD);
    SHR_DELETE(&s->al, e, entry);
}

static void entry_free(sw *s, entry *e) {
    table_remove(s, e);
    shr__lru_remove(&s->lru, &e->lru);
    s->used -= sizeof(entry) + e->bytes;
    entry_discard(s, e);
}

/* What uthash allocates with the first entry. */
#define TABLE_BYTES (sizeof(UT_hash_table) + HASH_INITIAL_NUM_BUCKETS * sizeof(UT_hash_bucket))

/* A miss renders the group into its own buffer (not zeroed: groups are opaque) and stores it; an entry the
 * table could not take has hh.tbl == NULL. NULL = draw directly: no memory, or a group over half of the
 * budget beside the table. */
static entry *render(sw *s, const shr_surface *dst, const shr_draw_cmd *group, size_t n, const entry_id *id) {
    int32_t w = id->width, h = id->height;
    size_t stride = (size_t)w * shr__px_bytes(dst->format), bytes = stride * (size_t)h;
    uint64_t room = s->budget > TABLE_BYTES ? s->budget - TABLE_BYTES : 0;
    if (sizeof(entry) + (uint64_t)bytes > room / 2) return NULL;
    entry *e = SHR_NEW(&s->al, entry);
    uint8_t *pixels = e ? shr__malloc(&s->al, bytes, 16, SHR_ALLOC_PAYLOAD) : NULL;
    if (!pixels) {
        SHR_DELETE(&s->al, e, entry);
        return NULL;
    }
    e->id = *id, e->bytes = bytes, e->pixels = pixels;
    shr_surface buf = {.pixels = pixels, .width = w, .height = h, .stride = stride, .byte_length = bytes,
                       .format = dst->format, .domain = SHR_MEMORY_CPU};
    shr_point origin = {group->dst.x0, group->dst.y0};
    for (size_t i = 1; i < n; i++) shr__raster_draw(&buf, &group[i], s->buffers, origin, (shr_rect){0, 0, w, h});
    if (table_add(s, e)) {
        shr__lru_push(&s->lru, &e->lru);
        s->used += sizeof(entry) + bytes;
    }
    return e;
}

/* group[0] is CACHE_BEGIN, group[n] its CACHE_END. */
static void draw_group(sw *s, const shr_surface *dst, const shr_draw_cmd *group, size_t n) {
    shr_rect clip = group->cache_clip;
    if (shr__rect_empty(clip)) return;
    entry_id id = {{group->key[0], group->key[1]}, group->dst.x1 - group->dst.x0, group->dst.y1 - group->dst.y0,
                   dst->format, 0};
    entry *e = table_find(s, &id);
    if (e) {
        shr__lru_remove(&s->lru, &e->lru);
        shr__lru_push(&s->lru, &e->lru);
    } else {
        e = render(s, dst, group, n, &id);
    }
    if (!e) {
        for (size_t i = 1; i < n; i++) shr__raster_draw(dst, &group[i], s->buffers, (shr_point){0, 0}, clip);
        return;
    }
    size_t bpp = shr__px_bytes(dst->format), stride = (size_t)id.width * bpp, row = (size_t)(clip.x1 - clip.x0) * bpp;
    for (int32_t y = clip.y0; y < clip.y1; y++)
        memcpy((uint8_t *)dst->pixels + (size_t)y * dst->stride + (size_t)clip.x0 * bpp,
               e->pixels + (size_t)(y - group->dst.y0) * stride + (size_t)(clip.x0 - group->dst.x0) * bpp, row);
    if (!e->hh.tbl) entry_discard(s, e);
    /* Evicting only now keeps the cache whole when the new entry could not be made. */
    while (s->used > s->budget) entry_free(s, SHR_CONTAINER(shr__lru_oldest(s->lru), entry, lru));
}

/* Buffer commands take effect before the batch is checked: after an error the registrations are unspecified. */
static shr_status sw_execute(void *user, const shr_surface *dst, const shr_draw_cmd *cmds, size_t count,
                             shr_fence fence) {
    (void)fence;
    sw *s = user;
    size_t i = 0;
    for (; cmds && i < count && shr__buffer_cmd(cmds[i].kind); i++) {
        const shr_draw_cmd *c = &cmds[i];
        shr_status st = shr__raster_buffer_check(c, s->buffers, s->nbuffers, NULL, NULL);
        if (st != SHR_OK) return st;
        if (c->kind == SHR_CMD_BUFFER_REGISTER)
            s->buffers[c->buffer - 1] = c->src;
        else if (c->kind == SHR_CMD_BUFFER_RELEASE && c->buffer && c->buffer <= s->nbuffers)
            s->buffers[c->buffer - 1] = (shr_image){0};
    }
    shr_status st = shr__raster_check(dst, cmds, count, s->buffers, s->nbuffers, NULL, NULL);
    if (st != SHR_OK) return st;
    shr_rect all = {0, 0, dst->width, dst->height};
    for (; i < count; i++) {
        if (cmds[i].kind != SHR_CMD_CACHE_BEGIN) {
            shr__raster_draw(dst, &cmds[i], s->buffers, (shr_point){0, 0}, all);
            continue;
        }
        size_t n = 1;
        while (cmds[i + n].kind != SHR_CMD_CACHE_END) n++;
        draw_group(user, dst, &cmds[i], n);
        i += n;
    }
    return SHR_OK;
}

static shr_status sw_reset(void *user) {
    (void)user;
    return SHR_OK;
}

shr_status shr_software_driver_create(const shr_allocator *allocator, uint64_t cache_bytes, uint32_t max_buffers,
                                      shr_framebuffer_driver *out) {
    if (!out) return SHR_E_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    shr__alloc al;
    if (!shr__alloc_init(&al, allocator)) return SHR_E_INVALID_ARG;
    sw *s = SHR_NEW(&al, sw);
    shr_image *buffers = s ? SHR_NEW_ARRAY(&al, shr_image, max_buffers) : NULL;
    if (!buffers) {
        SHR_DELETE(&al, s, sw);
        return SHR_E_NO_MEMORY;
    }
    *s = (sw){.al = al, .budget = cache_bytes, .buffers = buffers, .nbuffers = max_buffers};
    shr_framebuffer_driver_init(out);
    out->user = s;
    out->caps.domains = SHR_MEMORY_CPU | SHR_MEMORY_DMA;
    out->caps.max_buffers = max_buffers;
    out->execute = sw_execute;
    out->reset = sw_reset;
    return SHR_OK;
}

shr_status shr_software_driver_destroy(shr_framebuffer_driver *driver) {
    if (!driver || driver->execute != sw_execute || !driver->user) return SHR_E_INVALID_ARG;
    sw *s = driver->user;
    while (s->lru) entry_free(s, SHR_CONTAINER(s->lru, entry, lru));
    shr__alloc al = s->al;
    SHR_FREE_ARRAY(&al, s->buffers, shr_image, s->nbuffers);
    SHR_DELETE(&al, s, sw);
    memset(driver, 0, sizeof(*driver));
    return SHR_OK;
}
