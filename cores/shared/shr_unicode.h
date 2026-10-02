#ifndef SHIROKO_SHR_UNICODE_H
#define SHIROKO_SHR_UNICODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <shiroko/shiroko.h>

enum {
    SHR_GCB_OTHER = 0, SHR_GCB_CR, SHR_GCB_LF, SHR_GCB_CONTROL, SHR_GCB_EXTEND, SHR_GCB_ZWJ,
    SHR_GCB_RI, SHR_GCB_PREPEND, SHR_GCB_SPACINGMARK, SHR_GCB_L, SHR_GCB_V, SHR_GCB_T,
    SHR_GCB_LV, SHR_GCB_LVT
};

enum { SHR_INCB_NONE = 0, SHR_INCB_LINKER, SHR_INCB_CONSONANT, SHR_INCB_EXTEND };

/* Low 4 bits: Grapheme_Cluster_Break, bits 4-5: Indic_Conjunct_Break. */
enum {
    SHR_UP_DI = 1u << 6,
    SHR_UP_EXTPICT = 1u << 7,
    SHR_UP_EMOJI = 1u << 8,
    SHR_UP_EPRES = 1u << 9,
    SHR_UP_WIDE = 1u << 10,    /* East_Asian_Width W/F */
    SHR_UP_MARK = 1u << 11,    /* Mn, Mc, Me */
    SHR_UP_CC = 1u << 12,
    SHR_UP_CO = 1u << 13,
    SHR_UP_EMOD = 1u << 14,
    SHR_UP_NERD = 1u << 15,
    SHR_UP_CJK = 1u << 16      /* provider selection only, never width */
};

typedef struct shr__seq_ref {
    uint32_t offset;
    uint8_t length;
} shr__seq_ref;

extern const char shr__unicode_version[];
extern const uint8_t shr__text_profile_id[32];
extern const uint32_t shr__text_rules_version;
extern const uint32_t shr__cluster_max_scalars;
extern const uint32_t shr__cluster_max_bytes;
extern const uint32_t shr__tab_stop;
extern const uint16_t shr__uprop_l1[1088];
extern const uint16_t shr__uprop_l2[];
extern const uint8_t shr__uprop_data[];
extern const uint32_t shr__uprop_values[];
extern const uint32_t shr__emoji_seq_count;
extern const uint32_t shr__emoji_seq_pool[];
extern const shr__seq_ref shr__emoji_seq_index[];
extern const uint32_t shr__emoji_vs_base_count;
extern const uint32_t shr__emoji_vs_base[];

/* Values above U+10FFFF read as U+10FFFF; a sign mask, as a compare-select branches on RV32. */
static inline uint32_t shr__uprops(uint32_t cp) {
    cp ^= (cp ^ 0x10FFFFu) & (uint32_t)((0x10FFFF - (int64_t)cp) >> 32);
    uint32_t data = shr__uprop_l2[(uint32_t)shr__uprop_l1[cp >> 10] << 6 | (cp >> 4 & 63)];
    return shr__uprop_values[shr__uprop_data[data << 4 | (cp & 15)]];
}

static inline unsigned shr__gcb(uint32_t p) { return p & 0x0F; }
static inline unsigned shr__incb(uint32_t p) { return (p >> 4) & 0x03; }

static inline int shr__seq_cmp(const uint32_t *a, size_t an, const uint32_t *b, size_t bn) {
    for (size_t i = 0; i < an && i < bn; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return an == bn ? 0 : (an < bn ? -1 : 1);
}

/* Recognised multi-scalar emoji sequences, including qualification aliases. */
static inline bool shr__emoji_sequence(const uint32_t *cps, size_t n) {
    uint32_t lo = 0, hi = shr__emoji_seq_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const shr__seq_ref *r = &shr__emoji_seq_index[mid];
        int c = shr__seq_cmp(&shr__emoji_seq_pool[r->offset], r->length, cps, n);
        if (c == 0) return true;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

static inline bool shr__emoji_vs_registered(uint32_t cp) {
    uint32_t lo = 0, hi = shr__emoji_vs_base_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (shr__emoji_vs_base[mid] == cp) return true;
        if (shr__emoji_vs_base[mid] < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

#define SHR_VS15 0xFE0Eu
#define SHR_VS16 0xFE0Fu

typedef enum shr__glyph_kind {
    SHR_GLYPH_NONE = 0,
    SHR_GLYPH_SCALAR,   /* one visible base; selectors hidden */
    SHR_GLYPH_SEQUENCE, /* exact sequence lookup, else replacement */
    SHR_GLYPH_REPLACEMENT
} shr__glyph_kind;

typedef struct shr__cluster_class {
    int32_t cells;
    uint32_t flags;     /* SHR_CLUSTER_INVISIBLE, _REPLACEMENT, _EMOJI */
    uint8_t glyph_kind;
    uint32_t glyph_cp;  /* the base of SHR_GLYPH_SCALAR */
} shr__cluster_class;

/* Width rules in order: default-ignorable only 0; marks only 1 replacement;
 * recognised emoji 2; Nerd/private use 1; EAW W/F 2 else 1; otherwise the
 * widest base. Marks and hidden selectors never add width. */
static inline void shr__classify(const uint32_t *cps, size_t n, shr__cluster_class *out) {
    if (n == 1 && cps[0] - 0x20u < 0x5Fu) { /* printable ASCII: one cell, its own glyph */
        *out = (shr__cluster_class){1, 0, SHR_GLYPH_SCALAR, cps[0]};
        return;
    }
    *out = (shr__cluster_class){0};
    size_t bases = 0, visible = 0;
    uint32_t first_base = 0, first_props = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = shr__uprops(cps[i]);
        if (p & SHR_UP_DI) continue;
        visible++;
        if (!(p & SHR_UP_MARK) && bases++ == 0) first_base = cps[i], first_props = p;
    }
    if (visible == 0) {
        out->flags = SHR_CLUSTER_INVISIBLE;
        return;
    }
    if (bases == 0) {
        *out = (shr__cluster_class){1, SHR_CLUSTER_REPLACEMENT, SHR_GLYPH_REPLACEMENT, 0};
        return;
    }
    bool selected = n >= 2 && shr__emoji_vs_registered(cps[0]); /* base, selector, then only marks or ignorables */
    for (size_t i = 2; selected && i < n; i++) selected = (shr__uprops(cps[i]) & (SHR_UP_MARK | SHR_UP_DI)) != 0;
    bool emoji = n == 1 ? (first_props & SHR_UP_EPRES) != 0
                        : (selected && cps[1] == SHR_VS16) || shr__emoji_sequence(cps, n);
    if (emoji) {
        bool single = bases == 1 && n <= 2;
        *out = (shr__cluster_class){2, SHR_CLUSTER_EMOJI, single ? SHR_GLYPH_SCALAR : SHR_GLYPH_SEQUENCE,
                                    single ? first_base : 0};
        return;
    }
    bool text_forced = selected && cps[1] == SHR_VS15;
    int32_t w = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = shr__uprops(cps[i]);
        if (p & (SHR_UP_DI | SHR_UP_MARK)) continue;
        bool epres = !text_forced && (p & SHR_UP_EPRES);
        int32_t bw = epres ? 2 : (p & (SHR_UP_CO | SHR_UP_NERD)) ? 1 : (p & SHR_UP_WIDE) ? 2 : 1;
        if (bases == 1 && epres) out->flags |= SHR_CLUSTER_EMOJI;
        if (bw > w) w = bw;
    }
    out->cells = w;
    out->glyph_kind = bases == 1 && visible == 1 ? SHR_GLYPH_SCALAR : SHR_GLYPH_SEQUENCE;
    out->glyph_cp = out->glyph_kind == SHR_GLYPH_SCALAR ? first_base : 0;
}

#endif
