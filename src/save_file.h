// save_file.h — writing a cart's save so a crash can't eat it.
//
// The save used to be written in place: fopen("wb") truncates the file
// first, so a crash, a SIGKILL or a power cut in the middle of the write left
// a short or empty save, which loses exactly the progress the player asked
// to keep. Instead the bytes go to "<path>.tmp", which is flushed to the disk
// and then renamed over the save. A rename is atomic, so the save on disk is
// always either the old one or the new one, never half of each. The
// directory is synced too, or the rename itself can be lost.
//
// No SDL, no V8: test/save_file_test.c drives it directly.

#ifndef WC_SAVE_FILE_H
#define WC_SAVE_FILE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// Returns 0 on success. On failure the old save is untouched.
static inline int save_file_write(const char* path, const uint8_t* data, uint32_t size) {
    char tmp[4096 + 8];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size && fflush(f) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp);
        return -1;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp);
        return -1;
    }
#else
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    // Make the rename itself durable.
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash) {
        if (slash == dir) slash[1] = '\0';
        else *slash = '\0';
    } else {
        strcpy(dir, ".");
    }
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
#endif
    return 0;
}

// Reads the whole save. NULL with *out_size 0 if there is none or it can't
// be read. A "<path>.tmp" left by a crash mid-write is ignored: the save is
// whatever was last renamed into place.
static inline uint8_t* save_file_read(const char* path, uint32_t* out_size) {
    *out_size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *out_size = (uint32_t)n;
    return buf;
}

#endif
