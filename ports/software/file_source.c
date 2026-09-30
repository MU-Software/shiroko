#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 700

#include <shiroko/port_software.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) == 8, "64-bit file offsets");

static shr_status file_read(void *user, uint64_t offset, uint32_t length, void *dst, uint64_t request) {
    (void)request;
    if (offset > (uint64_t)INT64_MAX - length) return SHR_E_IO;
    for (uint32_t done = 0; done < length;) {
        ssize_t n = -1; /* primed as if interrupted: EINTR retries */
        for (int e = EINTR; n < 0 && e == EINTR; e = errno)
            n = pread((int)(intptr_t)user, (char *)dst + done, length - done, (off_t)(offset + done));
        if (n <= 0) return SHR_E_IO;
        done += (uint32_t)n;
    }
    return SHR_OK;
}

static void file_close(void *user) { close((int)(intptr_t)user); }

shr_status shr_asset_source_file(const char *path, shr_asset_source *out) {
    if (!path || !out) return SHR_E_INVALID_ARG;
    int fd = -1, e = EINTR; /* primed as if interrupted: EINTR retries */
    for (; fd < 0 && e == EINTR; e = errno) fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return e == ENOENT || e == ENOTDIR ? SHR_E_NOT_FOUND : SHR_E_IO;
    struct stat st = {0}; /* a failed fstat leaves st_mode 0: not a regular file */
    fstat(fd, &st);
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return SHR_E_IO;
    }
    shr_asset_source_init(out);
    out->user = (void *)(intptr_t)fd, out->size = (uint64_t)st.st_size, out->read = file_read, out->close = file_close;
    return SHR_OK;
}
