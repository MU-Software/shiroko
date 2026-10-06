#ifndef SHIROKO_SYNTH_H
#define SHIROKO_SYNTH_H

#include "raster.h"
#include "shr_alloc.h"

#define SHR__SYNTH_DEFAULT (256u << 10)
#define SHR__SYNTH_LUTS 32

/* shr__raster_lut tables by colour, DIM and format (key[0]) and bg (key[1]), found through an open-addressing `index`
 * of pool entries + 1 and all dropped when the pool is full. */
typedef struct shr__synth_luts {
    uint32_t key[SHR__SYNTH_LUTS][2];
    uint8_t index[2 * SHR__SYNTH_LUTS];
    uint32_t count;
    uint8_t px[SHR__SYNTH_LUTS][256 * 4];
} shr__synth_luts;

/* Synthesized BOLD/ITALIC coverage of buffer regions, reused until the region's buffer changes: entries packed in an
 * arena that doubles up to `cap` bytes (lowered to the size reached when growing fails) and empties when full, found
 * through an open-addressing index of the live entries' arena offsets + 1 (dropped entries keep their bytes until the
 * arena empties); ON_FILL GLYPHs draw cached coverage through `luts`. Nothing is allocated before the first styled
 * GLYPH, `luts` before the first styled ON_FILL one the cache holds. */
typedef struct shr__synth {
    uint8_t *arena;
    uint32_t *index;
    shr__synth_luts *luts;
    size_t size, used, cap;
    uint32_t slots, count;
} shr__synth;

/* Draws a checked BOLD or ITALIC GLYPH as shr__raster_draw does, from the cache where it fits. */
void shr__synth_draw(shr__synth *s, const shr__alloc *al, const shr_surface *dst, const shr_draw_cmd *c,
                     const shr_image *buffers, shr_point origin, shr_rect clip);
/* Forgets the entries of `buffer` whose src_rect meets `*rect`, or all of them for NULL. */
void shr__synth_drop(shr__synth *s, uint32_t buffer, const shr_rect *rect);
/* Frees everything; `budget` becomes the cap. */
void shr__synth_reset(shr__synth *s, const shr__alloc *al, size_t budget);

#endif
