// cart_host.h — Internal cart host types and functions

#ifndef WC_CART_HOST_INTERNAL_H
#define WC_CART_HOST_INTERNAL_H

#include "../include/wasmcart_host.h"
#include "abi.h"

// ─── Internal host struct ──────────────────────────────────────────────────

// One queued peer event, drained into the cart's wc_peer_on_* exports once per
// frame. Queued rather than delivered inline because a socket callback can fire
// at any point in node's loop, and re-entering the cart mid-frame is not safe.
typedef struct {
    int      type;      // WC_PEER_EV_*
    uint8_t* data;      // message payload (owned), NULL for non-message events
    uint32_t len;
} wc_peer_event_t;

#define WC_PEER_EV_CONNECT    0
#define WC_PEER_EV_MESSAGE    1
#define WC_PEER_EV_DISCONNECT 2
#define WC_PEER_EV_ERROR      3

typedef struct {
    int32_t  id;
    char     name[128];     // display only, NOT a handle, never trusted
    int      state;         // WC_PEER_CONNECTING / OPEN / CLOSING / CLOSED
    uint32_t transport;     // WC_TRANSPORT_* bitmask
    bool     host_supplied; // came from wc_host_add_peer, not wc_peer_open
    void*    js_ws;         // v8::Global<v8::Object>* for a dialled WebSocket
    void*    user;          // embedder pointer for a host-supplied peer
    wc_peer_send_fn send;   // host-supplied peers only

    wc_peer_event_t* events;
    uint32_t events_len;
    uint32_t events_cap;
} wc_peer_t;

struct wc_host {
    // V8 state (opaque, managed by cart_host.cpp)
    void* v8_state;

    // WASM memory (refreshed after grow)
    uint8_t* memory;
    uint32_t memory_size;

    // Raw WASM bytes (kept for module compilation)
    uint8_t* wasm_bytes;
    size_t   wasm_bytes_len;

    // Function export handles (opaque, point into v8_state)
    void* fn_wc_get_info;
    void* fn_wc_init;
    void* fn_wc_render;

    // Cart info (parsed from wc_get_info return)
    wc_cart_info_t info;
    wc_manifest_t manifest;

    // Cached "_filelist.txt" body; built on first request, freed on close.
    char*    file_list;
    uint32_t file_list_len;

    // GL state
    bool uses_gl;
    wc_gl_get_proc_fn gl_loader;

    // Audio ring buffer state
    uint32_t audio_read_cursor;

    // Error state
    bool trapped;
    uint32_t oob_logged;  // which out-of-memory regions have been reported (region_ok)
    bool init_deferred;  // _initialize/wc_init not yet called
    wc_host_options_t deferred_opts;  // saved for finish_init

    // .wasc archive (kept open for wc_load_asset)
    void* archive;  // mz_zip_archive*

    // Rumble backend supplied by the embedder (libretro, SDL, ...).
    // has_rumble == NULL means "no backend": the imports become silent no-ops
    // and wc_pad_has_rumble reports 0.
    wc_rumble_backend_t rumble;

    // Text input (ABI v3). Characters, not scancodes -- the platform has
    // already applied layout, shift, dead keys and IME composition, which is
    // work a cart must never redo. Same split every engine uses: SDL_TEXTINPUT
    // vs SDL_KEYDOWN, glfwSetCharCallback vs glfwSetKeyCallback, WM_CHAR vs
    // WM_KEYDOWN.
    //
    // Off until the cart calls wc_text_input_begin(), so a cart that never asks
    // cannot be surprised by text and an embedder may forward platform text
    // unconditionally. Queued strings are dropped on end(), so what was typed
    // into one field cannot resurface in the next.
    bool  text_active;
    char* text_queue;        // NUL-separated UTF-8 strings
    size_t text_queue_len;
    size_t text_queue_cap;

    // Scroll wheel (ABI v3.1), in 1/120 notch units. Accumulated across
    // however many events arrive during a frame and zeroed after each
    // wc_render, so the cart reads one delta per frame and never clears
    // anything. A device with no wheel leaves these at zero forever.
    int32_t wheel_dx;
    int32_t wheel_dy;

    // Peer connections (ABI v3). Two kinds live in one table on purpose: a
    // connection the CART dialled (wc_peer_open) and a peer the HOST handed it
    // (wc_host_add_peer) are the same object to the cart, which is the whole
    // point of hiding transport behind wc_peer_*.
    //
    // The security asymmetry is deliberate and matches the JS host: dialling
    // out is allowlisted, because the cart names a destination the packager may
    // not have anticipated. A host-supplied peer needs no grant -- the host
    // already chose it.
    wc_peer_t* peers;
    uint32_t   peer_count;
    uint32_t   peer_cap;
    int32_t    peer_next_id;

    // Grown-on-demand staging page for host->cart payloads when the cart
    // exports no malloc. Grown past everything the cart owns so it cannot
    // alias cart data. 0 = not yet claimed.
    uint32_t scratch_base;
    bool     scratch_warned;
};

// ─── Asset loading ─────────────────────────────────────────────────────

int  wc_archive_open(wc_host_t* host, const char* path);
int  wc_archive_open_memory(wc_host_t* host, const uint8_t* data, size_t len);
void wc_archive_close(wc_host_t* host);

int32_t wc_archive_load_asset(wc_host_t* host, const char* path, uint8_t* dest, uint32_t max_size);
int32_t wc_archive_asset_size(wc_host_t* host, const char* path);

int wc_parse_manifest(wc_host_t* host, const char* json, size_t len);

// ─── GL imports ────────────────────────────────────────────────────────

void wc_gl_imports_init(wc_host_t* host);

// ─── WASI stubs ────────────────────────────────────────────────────────
// (handled in cart_host.cpp for V8 version)

// ─── Env imports ───────────────────────────────────────────────────────
// (handled in cart_host.cpp for V8 version)

#endif // WC_CART_HOST_INTERNAL_H
