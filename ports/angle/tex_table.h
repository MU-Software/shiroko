#ifndef SHIROKO_TEX_TABLE_H
#define SHIROKO_TEX_TABLE_H

/* The uthash calls of angle_driver.c, included after its types. As a system header the unreachable
 * branches inside the uthash macros count as library code and stay out of coverage. */
#pragma GCC system_header

static inline tex_entry *table_find(gl_drv *g, const tex_id *id) {
    tex_entry *e;
    HASH_FIND(hh, g->table, id, sizeof(*id), e);
    return e;
}

/* false: out of memory, `e` is not in the table. */
static inline bool table_add(gl_drv *g, tex_entry *e) {
    HASH_ADD(hh, g->table, id, sizeof(e->id), e);
    return e->hh.tbl != NULL;
}

static inline void table_remove(gl_drv *g, tex_entry *e) { HASH_DELETE(hh, g->table, e); }

#endif
