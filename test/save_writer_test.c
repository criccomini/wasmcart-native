// save_writer_test.c — saves written off the game's thread (src/save_writer.h).
//
//   cc -Isrc -o save_writer_test test/save_writer_test.c $(pkg-config --cflags --libs sdl2)

#include "save_writer.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); failures++; } } while (0)

static SDL_atomic_t writes, failed_writes;
static void done(bool ok) {
    SDL_AtomicAdd(ok ? &writes : &failed_writes, 1);
}

int main(void) {
    char dir[] = "save_writer_test.XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    char path[256];
    snprintf(path, sizeof(path), "%s/cart.sav", dir);

    save_writer_t w;
    EXPECT(save_writer_start(&w, path, done), "writer starts");

    // A burst of snapshots: the writer may skip some, but the newest lands.
    char buf[32];
    for (int i = 0; i < 200; i++) {
        int n = snprintf(buf, sizeof(buf), "save %03d", i);
        EXPECT(save_writer_post(&w, (const uint8_t*)buf, (uint32_t)n), "post %d", i);
    }
    save_writer_flush(&w);
    uint32_t size = 0;
    uint8_t* got = save_file_read(path, &size);
    EXPECT(got && size == 8 && memcmp(got, "save 199", 8) == 0, "the newest snapshot is on disk");
    free(got);
    int n = SDL_AtomicGet(&writes);
    EXPECT(n >= 1 && n <= 200, "between 1 and 200 writes (%d)", n);
    EXPECT(!save_writer_take_failure(&w), "no failure reported");

    // A failing write is reported once.
    save_writer_stop(&w);
    char nowhere[256];
    snprintf(nowhere, sizeof(nowhere), "%s/missing/cart.sav", dir);
    EXPECT(save_writer_start(&w, nowhere, done), "second writer starts");
    save_writer_post(&w, (const uint8_t*)"x", 1);
    save_writer_flush(&w);
    EXPECT(save_writer_take_failure(&w), "the failure is reported");
    EXPECT(!save_writer_take_failure(&w), "only once");
    EXPECT(SDL_AtomicGet(&failed_writes) == 1, "done() heard about it");

    // Stop writes what's still pending.
    save_writer_stop(&w);
    EXPECT(save_writer_start(&w, path, done), "third writer starts");
    save_writer_post(&w, (const uint8_t*)"last one", 8);
    save_writer_stop(&w);
    got = save_file_read(path, &size);
    EXPECT(got && size == 8 && memcmp(got, "last one", 8) == 0, "stop flushes the pending save");
    free(got);

    remove(path);
    rmdir(dir);
    if (failures) return 1;
    printf("PASS: saves are written off the game's thread, newest first\n");
    return 0;
}
