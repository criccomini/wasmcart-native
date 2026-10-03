// save_file_test.c — a save on disk is always a whole save (src/save_file.h).
//
// No V8 or SDL:  cc -Isrc -o save_file_test test/save_file_test.c

#include "save_file.h"
#include <stdio.h>
#include <sys/stat.h>

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); failures++; } } while (0)

static int same(const char* path, const char* want) {
    uint32_t n = 0;
    uint8_t* got = save_file_read(path, &n);
    int ok = got && n == strlen(want) && memcmp(got, want, n) == 0;
    free(got);
    return ok;
}

static int exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int main(void) {
    char dir[] = "save_file_test.XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    char path[256], tmp[256];
    snprintf(path, sizeof(path), "%s/cart.wasc.sav", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    uint32_t n = 1;
    EXPECT(save_file_read(path, &n) == NULL && n == 0, "no save yet reads as none");

    EXPECT(save_file_write(path, (const uint8_t*)"level 3", 7) == 0, "first write");
    EXPECT(same(path, "level 3"), "reads back");
    EXPECT(!exists(tmp), "no temp file left behind");

    // A crash halfway through the next write leaves a short temp file. The
    // save is still the last whole one.
    FILE* f = fopen(tmp, "wb");
    fwrite("lev", 1, 3, f);
    fclose(f);
    EXPECT(same(path, "level 3"), "a half-written temp file is ignored");

    EXPECT(save_file_write(path, (const uint8_t*)"level 4 boss", 12) == 0, "next write");
    EXPECT(same(path, "level 4 boss"), "the new save replaced the old");
    EXPECT(!exists(tmp), "the stale temp file is gone");

    // Somewhere it can't write: fails, and doesn't touch anything.
    char nowhere[256];
    snprintf(nowhere, sizeof(nowhere), "%s/missing/dir/x.sav", dir);
    EXPECT(save_file_write(nowhere, (const uint8_t*)"x", 1) != 0, "unwritable path fails");
    EXPECT(same(path, "level 4 boss"), "and the real save is untouched");

    remove(path);
    rmdir(dir);
    if (failures) return 1;
    printf("PASS: saves are replaced whole or not at all\n");
    return 0;
}
