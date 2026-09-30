#ifndef SHIROKO_SHR_ERR_H
#define SHIROKO_SHR_ERR_H

#include <stdint.h>

#include <shiroko/shiroko.h>

static inline shr_status shr__fail(shr_error_info *err, shr_status st, size_t offset, size_t item, const char *reason) {
    if (err) *err = (shr_error_info){st, offset, item, reason};
    return st;
}
static inline void shr__err_clear(shr_error_info *err) {
    if (err) *err = (shr_error_info){SHR_OK, SIZE_MAX, SIZE_MAX, NULL};
}

#endif
