#include "shr_err.h"

const char *shr_status_name(shr_status s) {
    static const char *const names[] = {
        [SHR_OK] = "OK", [SHR_IN_PROGRESS] = "IN_PROGRESS", [SHR_E_INVALID_ARG] = "INVALID_ARG",
        [SHR_E_INVALID_UTF8] = "INVALID_UTF8", [SHR_E_CONTROL_CHAR] = "CONTROL_CHAR",
        [SHR_E_STYLE_BOUNDARY] = "STYLE_BOUNDARY", [SHR_E_UNKNOWN_STYLE] = "UNKNOWN_STYLE", [SHR_E_LIMIT] = "LIMIT",
        [SHR_E_OVERFLOW] = "OVERFLOW", [SHR_E_WRAP_NO_SPACE] = "WRAP_NO_SPACE",
        [SHR_E_CLUSTER_TOO_WIDE] = "CLUSTER_TOO_WIDE", [SHR_E_PROFILE_MISMATCH] = "PROFILE_MISMATCH",
        [SHR_E_FORMAT] = "FORMAT", [SHR_E_CHECKSUM] = "CHECKSUM", [SHR_E_IO] = "IO", [SHR_E_NO_MEMORY] = "NO_MEMORY",
        [SHR_E_WOULD_BLOCK] = "WOULD_BLOCK", [SHR_E_STATE] = "STATE", [SHR_E_NOT_FOUND] = "NOT_FOUND",
        [SHR_E_UNSUPPORTED] = "UNSUPPORTED", [SHR_E_TIMEOUT] = "TIMEOUT", [SHR_E_DEVICE] = "DEVICE"};
    _Static_assert(sizeof(names) / sizeof(names[0]) == SHR_E_DEVICE + 1, "names[] must cover every shr_status");
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "UNKNOWN";
}
