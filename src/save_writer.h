// save_writer.h — writing saves off the game's thread.
//
// A save is a small file, but making it durable means an fsync, and on an
// SD card an fsync waits for whatever else is being written: on a Pi 5 it
// took 17 ms idle (a dropped frame) and up to 26 s while a large download
// was being written. Done between frames, that freezes the game, and a
// supervisor watching for frames calls it hung.
//
// So the game's thread only copies the save region (a snapshot, never more
// than the save size) and hands it over; one writer thread does
// save_file_write. If a newer snapshot arrives before the writer gets to the
// last one, the older one is dropped: only the newest save matters. The
// final save on exit waits for the writer (save_writer_flush) and is then
// written synchronously, so nothing can land after it.

#ifndef WC_SAVE_WRITER_H
#define WC_SAVE_WRITER_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "save_file.h"

typedef void (*save_writer_done_fn)(bool ok);

typedef struct {
    SDL_Thread* thread;
    SDL_mutex* lock;
    SDL_cond* cond;
    char path[4096];
    uint8_t* pending;      // the newest snapshot not yet taken by the writer
    uint32_t pending_size;
    bool busy;             // the writer is in save_file_write
    bool failed;           // a write failed since the game's thread last asked
    bool quit;
    save_writer_done_fn done;  // called on the writer thread after each write
} save_writer_t;

static int save_writer_main(void* arg) {
    save_writer_t* w = (save_writer_t*)arg;
    SDL_LockMutex(w->lock);
    for (;;) {
        while (!w->pending && !w->quit) SDL_CondWait(w->cond, w->lock);
        if (!w->pending) break;  // quit with nothing left to write
        uint8_t* data = w->pending;
        uint32_t size = w->pending_size;
        w->pending = NULL;
        w->busy = true;
        SDL_UnlockMutex(w->lock);
        bool ok = save_file_write(w->path, data, size) == 0;
        free(data);
        if (w->done) w->done(ok);
        SDL_LockMutex(w->lock);
        w->busy = false;
        if (!ok) w->failed = true;
        SDL_CondBroadcast(w->cond);  // wake a flush
    }
    SDL_UnlockMutex(w->lock);
    return 0;
}

static inline bool save_writer_start(save_writer_t* w, const char* path, save_writer_done_fn done) {
    memset(w, 0, sizeof(*w));
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->done = done;
    w->lock = SDL_CreateMutex();
    w->cond = SDL_CreateCond();
    if (!w->lock || !w->cond) return false;
    w->thread = SDL_CreateThread(save_writer_main, "wasmcart-save", w);
    return w->thread != NULL;
}

// Copies the snapshot; returns at once. Returns false if it couldn't (then
// the caller should write synchronously instead).
static inline bool save_writer_post(save_writer_t* w, const uint8_t* data, uint32_t size) {
    if (!w->thread) return false;
    uint8_t* copy = (uint8_t*)malloc(size);
    if (!copy) return false;
    memcpy(copy, data, size);
    SDL_LockMutex(w->lock);
    free(w->pending);  // an older snapshot nobody wrote yet: the newer one wins
    w->pending = copy;
    w->pending_size = size;
    SDL_CondBroadcast(w->cond);
    SDL_UnlockMutex(w->lock);
    return true;
}

// Waits until nothing is pending or being written.
static inline void save_writer_flush(save_writer_t* w) {
    if (!w->thread) return;
    SDL_LockMutex(w->lock);
    while (w->pending || w->busy) SDL_CondWait(w->cond, w->lock);
    SDL_UnlockMutex(w->lock);
}

// True once after any failed write, so the game's thread can try again.
static inline bool save_writer_take_failure(save_writer_t* w) {
    if (!w->thread) return false;
    SDL_LockMutex(w->lock);
    bool f = w->failed;
    w->failed = false;
    SDL_UnlockMutex(w->lock);
    return f;
}

// Writes whatever is pending, then stops the thread.
static inline void save_writer_stop(save_writer_t* w) {
    if (!w->thread) return;
    SDL_LockMutex(w->lock);
    w->quit = true;
    SDL_CondBroadcast(w->cond);
    SDL_UnlockMutex(w->lock);
    SDL_WaitThread(w->thread, NULL);
    w->thread = NULL;
    SDL_DestroyCond(w->cond);
    SDL_DestroyMutex(w->lock);
}

#endif
