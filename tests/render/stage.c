/* Scene building helpers (render.h), shared by test_render and examples/desktop. */
#include "render.h"

bool stage_ok(stage *s, shr_status st, const char *what) {
    if (st == SHR_OK) return true;
    if (s->st == SHR_OK) {
        s->st = st;
        snprintf(s->what, sizeof(s->what), "%s: %s", what, shr_status_name(st));
    }
    return false;
}

bool stage_expect(stage *s, shr_status got, shr_status want, const char *what) {
    if (got == want) return true;
    if (s->st == SHR_OK) {
        s->st = got == SHR_OK ? SHR_E_STATE : got;
        snprintf(s->what, sizeof(s->what), "%s: %s, expected %s", what, shr_status_name(got), shr_status_name(want));
    }
    return false;
}

shr_lyr *stage_layer(stage *s, int32_t z, shr_rect r) {
    shr_lyr *l = NULL;
    if (s->st != SHR_OK) return NULL;
    if (s->nlayers == sizeof(s->layers) / sizeof(s->layers[0])) {
        stage_ok(s, SHR_E_LIMIT, "too many layers");
        return NULL;
    }
    if (stage_ok(s, shr_lyr_create(s->ctx, z, r, &l), "lyr_create")) s->layers[s->nlayers++] = l;
    return l;
}

shr_lyr *stage_grid(stage *s, int32_t z, shr_rect r, const shr_color *background) {
    shr_lyr *l = stage_layer(s, z, r);
    if (l && !stage_ok(s, shr_pl_lyr_tilemap_resize(l, s->font, (r.y1 - r.y0) / CH, (r.x1 - r.x0) / CW, background),
                       "tilemap_resize"))
        return NULL;
    return l;
}

shr_pl_res_image *stage_image(stage *s, int32_t w, int32_t h, const uint8_t *rgba) {
    shr_pl_res_image *img = NULL;
    if (s->st != SHR_OK) return NULL;
    if (s->nimages == sizeof(s->images) / sizeof(s->images[0])) {
        stage_ok(s, SHR_E_LIMIT, "too many images");
        return NULL;
    }
    if (stage_ok(s, shr_pl_res_image_create(s->ctx, w, h, rgba, (size_t)w * 4, &img), "image_create"))
        s->images[s->nimages++] = img;
    return img;
}

void stage_text(stage *s, shr_lyr *l, int32_t row, int32_t col, const char *utf8, shr_text_style style, uint32_t flags) {
    if (s->st != SHR_OK) return;
    shr_error_info err = {0};
    shr_status st = shr_pl_lyr_tilemap_set_text(l, row, col, utf8, strlen(utf8), style, NULL, 0, flags, &err);
    if (st != SHR_OK) {
        char what[160];
        snprintf(what, sizeof(what), "set_text \"%.40s\" (%s at byte %zu)", utf8, err.reason ? err.reason : "",
                 err.byte_offset);
        stage_ok(s, st, what);
    }
}

void stage_cell(stage *s, shr_lyr *l, int32_t row, int32_t col, const char *utf8, uint32_t span, shr_text_style style) {
    if (s->st != SHR_OK) return;
    char what[96];
    snprintf(what, sizeof(what), "set_cell %d,%d \"%.40s\"", row, col, utf8);
    stage_ok(s, shr_pl_lyr_tilemap_set_cell(l, row, col, utf8, strlen(utf8), span, style), what);
}

void stage_fill(stage *s, shr_lyr *l, shr_rect r, shr_color c) {
    if (s->st == SHR_OK) stage_ok(s, shr_lyr_cmd_fill(l, r, c), "cmd_fill");
}

/* Reads of FONTS_ASYNC packages: held while `hold`, completed by stage_release_reads(). */
typedef struct async_source {
    stage *s;
    shr_asset_source inner;
} async_source;

static shr_status async_read(void *user, uint64_t offset, uint32_t length, void *dst, uint64_t request) {
    async_source *a = user;
    stage *s = a->s;
    if (!s->hold) return a->inner.read(a->inner.user, offset, length, dst, request);
    if (s->nheld == sizeof(s->held) / sizeof(s->held[0])) return SHR_E_WOULD_BLOCK;
    s->held[s->nheld++] = (held_read){&a->inner, offset, length, dst, request};
    return SHR_IN_PROGRESS;
}

static void async_cancel(void *user, uint64_t request) {
    stage *s = ((async_source *)user)->s;
    for (size_t i = s->nheld; i > 0; i--)
        if (s->held[i - 1].request == request) s->held[i - 1] = s->held[--s->nheld];
}

static void async_close(void *user) {
    async_source *a = user;
    if (a->inner.close) a->inner.close(a->inner.user);
    free(a);
}

shr_status stage_open_package(stage *s, uint32_t flags, const char *font_dir, const char *name, shr_asset_source *out) {
    if ((flags & FONTS_NONE) || ((flags & FONTS_LATIN) && strcmp(name, "shiroko-latin.shrf"))) return SHR_E_NOT_FOUND;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", font_dir, name);
    if (!(flags & FONTS_ASYNC)) return shr_asset_source_file(path, out);
    async_source *a = calloc(1, sizeof(*a));
    if (!a) return SHR_E_NO_MEMORY;
    shr_status st = shr_asset_source_file(path, &a->inner);
    if (st != SHR_OK) {
        free(a);
        return st;
    }
    a->s = s;
    shr_asset_source_init(out);
    out->user = a, out->size = a->inner.size;
    out->read = async_read, out->cancel = async_cancel, out->close = async_close;
    return SHR_OK;
}

shr_status stage_release_reads(stage *s) {
    s->hold = false;
    for (size_t i = 0; i < s->nheld; i++) {
        held_read *h = &s->held[i];
        shr_status st = h->inner->read(h->inner->user, h->offset, h->length, h->dst, h->request);
        if (!stage_ok(s, shr_asset_complete(s->ctx, h->request, st), "asset_complete")) break;
    }
    s->nheld = 0;
    return s->st;
}
