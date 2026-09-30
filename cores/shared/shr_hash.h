#ifndef SHIROKO_SHR_HASH_H
#define SHIROKO_SHR_HASH_H

#include <stddef.h>
#include <stdint.h>

/* xxHash, inlined: its arithmetic wraps on purpose. */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("unsigned-integer-overflow", "unsigned-shift-base"))), \
                             apply_to = function)
#endif
#define XXH_INLINE_ALL
#include <xxhash.h>
#if defined(__clang__)
#pragma clang attribute pop
#endif

static inline uint16_t shr__rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t shr__rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t shr__rd64(const uint8_t *p) { return shr__rd32(p) | (uint64_t)shr__rd32(p + 4) << 32; }

#endif
