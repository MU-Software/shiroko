#ifndef SHIROKO_CACHE_TABLE_H
#define SHIROKO_CACHE_TABLE_H

/* The uthash calls of software_driver.c, included after its types. As a system header the unreachable
 * branches inside the uthash macros count as library code, like the inlined xxHash, and stay out of coverage. */
#pragma GCC system_header

static inline entry *table_find(sw *s, const entry_id *id) {
    entry *e;
    HASH_FIND(hh, s->table, id, sizeof(*id), e);
    return e;
}

/* false: out of memory, `e` is not in the table. */
static inline bool table_add(sw *s, entry *e) {
    HASH_ADD(hh, s->table, id, sizeof(e->id), e);
    return e->hh.tbl != NULL;
}

static inline void table_remove(sw *s, entry *e) { HASH_DELETE(hh, s->table, e); }

#endif
