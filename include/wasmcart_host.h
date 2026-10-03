// wasmcart_host.h — Native C CartHost public API
// Embed wasmcart in any application: standalone player, libretro core, etc.

#ifndef WASMCART_HOST_H
#define WASMCART_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── ABI Constants (matches abi.js) ────────────────────────────────────────

#define WC_ABI_VERSION     3
#define WC_MIN_ABI_VERSION 1

#define WC_MAX_PADS        4
#define WC_PAD_SIZE        16
#define WC_TIME_SIZE       20
#define WC_MAX_POINTERS    10
#define WC_KEYS_STATE_SIZE 32

// Button bitmask
#define WC_BUTTON_A      (1 << 0)
#define WC_BUTTON_B      (1 << 1)
#define WC_BUTTON_X      (1 << 2)
#define WC_BUTTON_Y      (1 << 3)
#define WC_BUTTON_L      (1 << 4)
#define WC_BUTTON_R      (1 << 5)
#define WC_BUTTON_START  (1 << 6)
#define WC_BUTTON_SELECT (1 << 7)
#define WC_BUTTON_UP     (1 << 8)
#define WC_BUTTON_DOWN   (1 << 9)
#define WC_BUTTON_LEFT   (1 << 10)
#define WC_BUTTON_RIGHT  (1 << 11)
#define WC_BUTTON_L3     (1 << 12)
#define WC_BUTTON_R3     (1 << 13)

// Cart info flags
#define WC_FLAG_AUDIO_F32 (1 << 0)
#define WC_FLAG_NET_PEER  (1 << 1)  // cart wants peer-connection imports
// (1 << 2) is RESERVED AND UNUSED — it was WC_FLAG_NET_DC before the WebSocket
// and data-channel families merged into one peer-connection family. Hosts must
// ignore it; carts must not set it. WC_FLAG_NET_PEER governs all networking.
#define WC_FLAG_POINTER   (1 << 3)
#define WC_FLAG_KEYBOARD  (1 << 4)

// ─── Structs ───────────────────────────────────────────────────────────────

// Matches the in-memory layout of wc_pad_t (16 bytes)
typedef struct {
    uint16_t buttons;
    int16_t  left_x;
    int16_t  left_y;
    int16_t  right_x;
    int16_t  right_y;
    uint8_t  left_trigger;
    uint8_t  right_trigger;
    uint8_t  connected;
    uint8_t  _pad;
} wc_pad_t;

// Parsed from wc_get_info() return
typedef struct {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t fb_ptr;
    uint32_t audio_ptr;
    uint32_t audio_cap;       // capacity in stereo frames
    uint32_t audio_write_ptr; // pointer to cart's write cursor (u32 in cart memory)
    uint32_t input_ptr;
    uint32_t save_ptr;
    uint32_t save_size;
    uint32_t time_ptr;
    uint32_t host_info_ptr;
    uint32_t flags;
    uint32_t audio_sample_rate;
    // v3 extended
    uint32_t pointer_ptr;
    uint32_t keys_ptr;
    uint32_t gpu_api;          // 0=2D, 1=WebGL2/GLES3, 2=WebGPU, 3=Vulkan
} wc_cart_info_t;

// GPU API values
#define WC_GPU_API_NONE    0
#define WC_GPU_API_WEBGL2  1
#define WC_GPU_API_WEBGPU  2
#define WC_GPU_API_VULKAN  3

// Manifest `controls` presentation hint (SPEC.md, Manifest > Fields): which
// parts of the standard pad the game actually reads. Advisory only — every
// button and axis is always delivered regardless. Hosts drawing on-screen
// touch controls use it to show only what the game needs.
#define WC_CTRL_DPAD    (1u << 0)
#define WC_CTRL_A       (1u << 1)
#define WC_CTRL_B       (1u << 2)
#define WC_CTRL_X       (1u << 3)
#define WC_CTRL_Y       (1u << 4)
#define WC_CTRL_L       (1u << 5)
#define WC_CTRL_R       (1u << 6)
#define WC_CTRL_START   (1u << 7)
#define WC_CTRL_SELECT  (1u << 8)
#define WC_CTRL_LSTICK  (1u << 9)
#define WC_CTRL_RSTICK  (1u << 10)
#define WC_CTRL_LTRIG   (1u << 11)
#define WC_CTRL_RTRIG   (1u << 12)
#define WC_CTRL_L3      (1u << 13)
#define WC_CTRL_R3      (1u << 14)

// The retro default set hosts SHOULD assume when the manifest omits the field
// (sticks and triggers only appear when declared).
#define WC_CTRL_DEFAULT_SET (WC_CTRL_DPAD | WC_CTRL_A | WC_CTRL_B | \
                             WC_CTRL_X | WC_CTRL_Y | WC_CTRL_START | \
                             WC_CTRL_SELECT | WC_CTRL_L | WC_CTRL_R)

// Parsed from manifest.json
typedef struct {
    char     name[256];
    char     version[64];
    uint32_t abi;
    char     entry[256];
    /*
     * Asset root inside the archive, e.g. "app/". The JS host reads this;
     * this one used to assume "assets/" unconditionally, so a cart whose
     * manifest said otherwise could not find its own assets: the zip holds
     * "app/roms/game.prg" while the cart asks for "roms/game.prg".
     * Empty means assets sit at the archive root.
     */
    char     assets[256];
    /*
     * The size the cart AUTHOR declared. Passed to the cart as the host's
     * preferred_width/height when the user gave no --res, matching the JS
     * host: a runtime that sizes itself from preferred_width (Defold and Godot
     * both do) otherwise ignores the declaration and falls back to its own
     * default, so a 1280x720 cart opened in a 960x540 window with its picture
     * cropped. 0 means the manifest did not say.
     */
    uint32_t width;
    uint32_t height;
    uint32_t players;
    bool     pointer;
    bool     keyboard;
    uint32_t controls;       // WC_CTRL_* bitmask; valid only if controls_set
    bool     controls_set;   // manifest had a `controls` array
    // networking (v3)
    bool     websocket;
    bool     data_channel;
    char     ws_domains[8][256]; // up to 8 allowed domains
    uint32_t ws_domain_count;
    bool     has_net;            // a "net" object was present at all
} wc_manifest_t;

// Options for loading a cart
typedef struct {
    uint32_t preferred_width;
    uint32_t preferred_height;
    uint32_t host_fps;
    uint32_t audio_sample_rate;
    const uint8_t* save_data;
    uint32_t save_data_size;
    bool     defer_init;        // if true, skip _initialize/wc_init — call wc_host_finish_init() later
    // RNG seeding. A normal load (rng_seed_set == false) rolls fresh entropy
    // for the cart's wc_set_seed export, so every boot shuffles differently.
    // Set rng_seed_set for deterministic replay: the cart is seeded with
    // rng_seed exactly, same as the JS hosts' deterministic:{seed}.
    uint32_t rng_seed;
    bool     rng_seed_set;
    // The player turned networking off for this cart: wc_peer_open refuses
    // every address, whatever the manifest grants, and the cart sees -1 as it
    // would for an ungranted host. Peers the host supplies (wc_host_add_peer)
    // are the embedder's own choice and aren't affected.
    bool     deny_net;
} wc_host_options_t;

// ─── Host API ──────────────────────────────────────────────────────────────

typedef struct wc_host wc_host_t;

// Lifecycle
wc_host_t* wc_host_create(void);
void       wc_host_destroy(wc_host_t* host);

// Loading
int  wc_host_load_file(wc_host_t* host, const char* wasc_path, const wc_host_options_t* opts);
int  wc_host_load_memory(wc_host_t* host, const uint8_t* data, size_t len, const wc_host_options_t* opts);
int  wc_host_finish_init(wc_host_t* host);  // call after GL context ready if defer_init was set

// ─── Rumble (ABI v3) ───────────────────────────────────────────────────────
//
// Rumble runs the OPPOSITE way to the rest of input: the cart drives it, so it
// arrives as three host imports rather than a field in wc_pad_t. The embedder
// supplies a backend (libretro's rumble interface, SDL, ...); with none set the
// imports are silent no-ops and has_rumble reports 0, which is the honest
// answer for a keyboard-only setup.
//
// The host library clamps and caps before calling through, so a backend only
// has to translate: low/high arrive already in 0..1 and duration_ms already
// capped at WC_RUMBLE_MAX_MS.
//
// low  = low-frequency / "strong" motor      high = high-frequency / "weak"
#define WC_RUMBLE_MAX_MS 5000

typedef struct {
    // Return non-zero if THIS pad can rumble. Per-device, not per-platform:
    // an X360 pad reports rumble but no trigger rumble.
    int  (*has_rumble)(void* user, uint32_t pad_id);
    void (*rumble)(void* user, uint32_t pad_id, float low, float high,
                   uint32_t duration_ms);
    void (*stop)(void* user, uint32_t pad_id);
    void* user;
} wc_rumble_backend_t;

// Pass NULL to detach. Safe to call before or after loading a cart.
void wc_host_set_rumble_backend(wc_host_t* host, const wc_rumble_backend_t* backend);

// ─── Text input (ABI v3) ───────────────────────────────────────────────────
//
// Characters, not scancodes. A scancode is a key POSITION: Shift+2 is "@" on a
// US layout and something else on many others, and an accented character may
// take a dead-key sequence or an IME commit with no scancode of its own. The
// platform already resolved all of that, so the host forwards the result rather
// than making every cart reimplement keyboard layouts.
//
// This is the same split every engine draws -- SDL_TEXTINPUT vs SDL_KEYDOWN,
// glfwSetCharCallback vs glfwSetKeyCallback, WM_CHAR vs WM_KEYDOWN. The raw
// keyboard ABI stays for gameplay and for editing keys (backspace, arrows,
// enter), which are presses rather than characters.
//
// Feed it whatever the platform gives you: an SDL_TEXTINPUT event's `text` is
// exactly right. Text is IGNORED unless the cart has called
// wc_text_input_begin(), so it is safe to forward unconditionally.
//
// While text input is active an embedder SHOULD suppress its own key bindings:
// typing "q" into a name field must not quit, and "w" must not also walk the
// player forward. wc_host_text_input_active() reports the state; on platforms
// with an on-screen keyboard it is also the signal to raise and dismiss it.
void wc_host_push_text(wc_host_t* host, const char* utf8, uint32_t len);
int  wc_host_text_input_active(wc_host_t* host);

// Input — call before wc_host_run_frame()
void wc_host_set_pads(wc_host_t* host, const wc_pad_t pads[WC_MAX_PADS]);
void wc_host_set_keyboard(wc_host_t* host, const uint8_t keys[WC_KEYS_STATE_SIZE]);
void wc_host_set_pointer(wc_host_t* host, int index, int16_t x, int16_t y, uint8_t buttons, uint8_t active);
void wc_host_set_time(wc_host_t* host, double time_ms, double delta_ms, uint32_t frame);

// ─── Peer connections (ABI v3) ─────────────────────────────────────────────
//
// One networking primitive: a connection to a peer. The cart opens one, sends
// bytes, receives bytes, and is told when it opens or closes. Transport stays
// opaque -- a peer is the same object to the cart whether it arrived over a
// WebSocket, a LAN socket or a serial cable.
//
// Two ways a peer appears, with deliberately different security:
//
//   wc_peer_open()      the CART dials. Requires BOTH the cart's
//                       WC_FLAG_NET_PEER and a manifest net grant covering the
//                       address. Fails closed. Handled entirely inside the
//                       host using node's WebSocket -- an embedder does nothing.
//
//   wc_host_add_peer()  the HOST hands the cart a peer it established itself.
//                       No manifest grant needed: the host already made that
//                       decision, and requiring it to also write a manifest key
//                       permitting its own action would be ceremony.
//
// A host-supplied peer needs a send callback, because only the embedder knows
// how to put bytes on its transport. Feed inbound bytes back with
// wc_host_peer_recv(), and report a drop with wc_host_remove_peer().

#define WC_PEER_CONNECTING 0
#define WC_PEER_OPEN       1
#define WC_PEER_CLOSING    2
#define WC_PEER_CLOSED     3

#define WC_TRANSPORT_UNKNOWN     0x00
#define WC_TRANSPORT_RELIABLE    0x01
#define WC_TRANSPORT_ORDERED     0x02
#define WC_TRANSPORT_LOW_LATENCY 0x04

// Return 0 on success, non-zero to signal the send failed.
typedef int (*wc_peer_send_fn)(void* user, const uint8_t* data, uint32_t len);

// Register a peer the host established. `name` is display-only and is never
// treated as a handle. Returns the peer id, or -1 if the table is full.
int32_t wc_host_add_peer(wc_host_t* host, const char* name,
                         wc_peer_send_fn send, void* user, uint32_t transport);

// Deliver bytes that arrived on a host-supplied peer.
void wc_host_peer_recv(wc_host_t* host, int32_t peer_id,
                       const uint8_t* data, uint32_t len);

// Report that a host-supplied peer went away. The cart sees
// wc_peer_on_disconnect on the next frame.
void wc_host_remove_peer(wc_host_t* host, int32_t peer_id);

// Advance node's event loop without running a frame. wc_host_run_frame() does
// this for you; call it directly only when driving async work outside the
// frame loop (waiting on a connection before the cart starts, say).
void wc_host_pump(wc_host_t* host);

// Run one frame — calls wc_render() on the cart
void wc_host_run_frame(wc_host_t* host);

// V8 locking — hold locker persistently for hosts that call from the same thread
void wc_host_enter_v8(void);
void wc_host_exit_v8(void);

// Readback — call after wc_host_run_frame()
const uint8_t*  wc_host_get_framebuffer(wc_host_t* host, uint32_t* width, uint32_t* height);
const void*     wc_host_get_audio(wc_host_t* host, uint32_t* num_frames, bool* is_f32);
uint8_t*        wc_host_get_save_data(wc_host_t* host, uint32_t* size);

// GL carts
bool wc_host_uses_gl(wc_host_t* host);
// Set a function that resolves GL function names to pointers.
// For standalone: use eglGetProcAddress. For libretro: use retro_hw_get_proc_address.
typedef void* (*wc_gl_get_proc_fn)(const char* name);
void wc_host_set_gl_loader(wc_host_t* host, wc_gl_get_proc_fn loader);

// Error state
bool wc_host_has_trapped(wc_host_t* host);

// Info
const wc_cart_info_t* wc_host_get_cart_info(wc_host_t* host);
const wc_manifest_t*  wc_host_get_manifest(wc_host_t* host);

// Direct memory access (for save states, etc.)
void*  wc_host_get_memory(wc_host_t* host, uint32_t* size);

#ifdef __cplusplus
}
#endif

#endif // WASMCART_HOST_H
