// pad_slots.h — which physical controller is which player, without SDL.
//
// A slot holds a controller's key: a string that stays the same across
// reconnects (on Linux, the Bluetooth address from the input node's `uniq`,
// or the USB port path). A pad that arrives takes the slot that already holds
// its key, or else the lowest slot holding no key.
//
// Two modes:
//   - Plain (the default): a pad that leaves frees its slot, so the next pad
//     to arrive takes it. That's "first free" and matches the player before.
//   - Sticky (Couchmix): a pad that leaves keeps its slot, reading as
//     disconnected, and gets it back when it returns. A new pad skips held
//     slots. The table can be seeded with keys, so a supervisor that numbered
//     the players before the game started hands its numbering over.
//
// Seed format: "0=<key>,1=<key>" -- slot=key pairs separated by commas.

#ifndef WC_PAD_SLOTS_H
#define WC_PAD_SLOTS_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef PAD_SLOTS_MAX
#define PAD_SLOTS_MAX 4
#endif
#define PAD_KEY_MAX 64

typedef struct {
    char key[PAD_SLOTS_MAX][PAD_KEY_MAX];  // "" = nobody holds this slot
    bool connected[PAD_SLOTS_MAX];
    bool sticky;
} pad_slots_t;

// Keys travel in an environment variable and in heartbeat lines, so keep
// them to one printable word with no separators.
static inline void pad_key_clean(char* key) {
    for (char* c = key; *c; c++) {
        if (*c <= ' ' || *c == ',' || *c == '=' || *c == 127) *c = '_';
        else if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    }
}

static inline void pad_slots_init(pad_slots_t* t, bool sticky, const char* seed) {
    memset(t, 0, sizeof(*t));
    t->sticky = sticky;
    if (!seed) return;
    const char* p = seed;
    while (*p) {
        const char* end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        const char* eq = memchr(p, '=', len);
        if (eq && eq > p) {
            char* num_end;
            long slot = strtol(p, &num_end, 10);
            size_t klen = len - (size_t)(eq + 1 - p);
            if (num_end == eq && slot >= 0 && slot < PAD_SLOTS_MAX &&
                klen > 0 && klen < PAD_KEY_MAX) {
                memcpy(t->key[slot], eq + 1, klen);
                t->key[slot][klen] = '\0';
                pad_key_clean(t->key[slot]);
            }
        }
        if (!end) break;
        p = end + 1;
    }
}

// A pad with this key arrived. Returns its slot, or -1 if every slot is held.
static inline int pad_slots_attach(pad_slots_t* t, const char* key) {
    char k[PAD_KEY_MAX];
    strncpy(k, key, PAD_KEY_MAX - 1);
    k[PAD_KEY_MAX - 1] = '\0';
    pad_key_clean(k);
    int slot = -1;
    for (int i = 0; i < PAD_SLOTS_MAX && slot < 0; i++)
        if (k[0] && strcmp(t->key[i], k) == 0 && !t->connected[i]) slot = i;
    for (int i = 0; i < PAD_SLOTS_MAX && slot < 0; i++)
        if (!t->key[i][0] && !t->connected[i]) slot = i;
    if (slot < 0) return -1;
    strcpy(t->key[slot], k);
    t->connected[slot] = true;
    return slot;
}

// The pad in this slot left.
static inline void pad_slots_detach(pad_slots_t* t, int slot) {
    if (slot < 0 || slot >= PAD_SLOTS_MAX) return;
    t->connected[slot] = false;
    if (!t->sticky) t->key[slot][0] = '\0';
}

#endif
