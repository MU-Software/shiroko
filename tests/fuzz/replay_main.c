/* Corpus replay for builds without libFuzzer: runs every file (or every file
 * in every directory) given on the command line through the fuzz target, so
 * the seed and regression corpora run in ctest under every sanitizer. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    size_t got = buf ? fread(buf, 1, (size_t)(n > 0 ? n : 0), f) : 0;
    fclose(f);
    if (!buf || got != (size_t)(n > 0 ? n : 0)) {
        free(buf);
        return 1;
    }
    LLVMFuzzerTestOneInput(buf, got);
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    int files = 0, errors = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            fprintf(stderr, "missing %s\n", argv[i]);
            return 1;
        }
        if (!S_ISDIR(st.st_mode)) {
            errors += run_file(argv[i]);
            files++;
            continue;
        }
        DIR *d = opendir(argv[i]);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", argv[i], e->d_name);
            errors += run_file(path);
            files++;
        }
        if (d) closedir(d);
    }
    printf("replayed %d inputs\n", files);
    return errors || files == 0;
}
