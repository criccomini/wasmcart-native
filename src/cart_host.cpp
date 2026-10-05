// cart_host.cpp — Core wasmcart host logic using V8 WASM API (via libnode)
// Drop-in replacement for cart_host.c (wasmtime version).
// All public API (wasmcart_host.h) stays identical.

// Provide the snapshot stub that libnode.a references but doesn't include.
// Embedders don't use snapshots — return null.
// Must match the exact signature: node::SnapshotData const* (not void*)
namespace node {
struct SnapshotData;
class SnapshotBuilder {
public:
    static const SnapshotData* GetEmbeddedSnapshotData();
};
const SnapshotData* SnapshotBuilder::GetEmbeddedSnapshotData() { return nullptr; }
}

#include "node.h"
#include "v8.h"
#include "v8-wasm.h"
#include "uv.h"

extern "C" {
#include "cart_host.h"
}

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <random>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include "wc_log.h"
#include "thread_worker_js.h"
#include "../deps/miniz.h"
extern "C" FILE* _wc_log_file = NULL;
extern "C" long _wc_log_bytes = 0;
#ifdef _WIN32
#include <windows.h>
#endif

// ─── V8 state (module-level, single isolate) ──────────────────────────────

static std::unique_ptr<node::MultiIsolatePlatform> g_platform;
v8::Isolate* g_isolate = nullptr;
static node::Environment* g_env = nullptr;
std::unique_ptr<node::CommonEnvironmentSetup> g_setup;
static bool g_v8_initialized = false;

// Per-host V8 persistent handles
struct v8_host_state {
    v8::Global<v8::Object> instance;     // WebAssembly.Instance
    v8::Global<v8::Object> exports_obj;  // instance.exports
    v8::Global<v8::Function> fn_wc_get_info;
    v8::Global<v8::Function> fn_wc_init;
    v8::Global<v8::Function> fn_wc_render;
    v8::Global<v8::Function> fn_initialize;
    v8::Global<v8::Function> fn_malloc;
    v8::Global<v8::Function> fn_wc_set_seed;
    v8::Global<v8::Object> memory_obj;   // WebAssembly.Memory

    // WASI threads (wasi.thread-spawn). Empty for a cart that does not spawn.
    v8::Global<v8::Object> threads;      // { spawn, shutdown, count } from WC_THREADS_SPAWNER_JS
    v8::Global<v8::Function> thread_spawn_fn;
};

// ─── V8 helpers ──────────────────────────────────────────────────────────

static v8::Local<v8::String> v8str(const char* s) {
    return v8::String::NewFromUtf8(g_isolate, s).ToLocalChecked();
}

v8::Local<v8::Context> ctx() {
    return g_setup->context();
}

static void refresh_memory(wc_host_t* host) {
    auto state = (v8_host_state*)host->v8_state;
    auto mem = state->memory_obj.Get(g_isolate);
    // Use WasmMemoryObject::Buffer() for direct access
    auto wasm_mem = v8::WasmMemoryObject::Cast(*mem);
    auto ab = wasm_mem->Buffer();
    host->memory = (uint8_t*)ab->Data();
    host->memory_size = (uint32_t)ab->ByteLength();
}

// Seed the cart's RNG before wc_init. Without this every boot runs the cart's
// compile-time seed: same shuffle, same waves, same dice (the JS hosts had the
// identical bug). Entropy by default; opts.rng_seed_set pins it for replay.
// Caller must hold the usual V8 scopes.
static void seed_cart_rng(wc_host_t* host, const wc_host_options_t* opts) {
    auto state = (v8_host_state*)host->v8_state;
    if (state->fn_wc_set_seed.IsEmpty()) return;
    uint32_t seed;
    if (opts && opts->rng_seed_set) {
        seed = opts->rng_seed;
    } else {
        std::random_device rd;
        seed = (uint32_t)rd();
    }
    v8::Local<v8::Value> args[] = { v8::Integer::NewFromUnsigned(g_isolate, seed) };
    (void)state->fn_wc_set_seed.Get(g_isolate)->Call(ctx(), ctx()->Global(), 1, args);
    wc_log("wasmcart: wc_set_seed(%u)%s\n", seed,
        (opts && opts->rng_seed_set) ? " (pinned)" : "");
}

// ─── V8 initialization (once per process) ──────────────────────────────

static int v8_init() {
    if (g_v8_initialized) return 0;

    // Enable WASM exnref (standard exception handling) — same as --experimental-wasm-exnref
    v8::V8::SetFlagsFromString("--experimental-wasm-exnref");
    // V8 default is unlimited WASM memory pages — don't restrict it
    // Optimize WASM↔JS transitions
    v8::V8::SetFlagsFromString("--turbo-inline-js-wasm-calls");
    v8::V8::SetFlagsFromString("--turbo-optimize-inlined-js-wasm-wrappers");
    v8::V8::SetFlagsFromString("--turboshaft-wasm-in-js-inlining");

    std::vector<std::string> args = {"wasmcart-run"};
    auto result = node::InitializeOncePerProcess(args, {
        node::ProcessInitializationFlags::kNoInitializeV8,
        node::ProcessInitializationFlags::kNoInitializeNodeV8Platform
    });
    for (const std::string& err : result->errors()) {
        wc_log( "wasmcart: v8 init error: %s\n", err.c_str());
    }
    if (result->early_return() != 0) return -1;

    g_platform = node::MultiIsolatePlatform::Create(4);
    v8::V8::InitializePlatform(g_platform.get());
    v8::V8::Initialize();

    std::vector<std::string> errors;
    std::vector<std::string> exec_args;
    g_setup = node::CommonEnvironmentSetup::Create(
        g_platform.get(), &errors, args, exec_args);
    if (!g_setup) {
        for (const std::string& err : errors)
            wc_log( "wasmcart: %s\n", err.c_str());
        return -1;
    }

    g_isolate = g_setup->isolate();
    g_env = g_setup->env();
    g_v8_initialized = true;

    // Run LoadEnvironment to initialize Node.js built-ins (needed for WASI etc)
    {
        v8::Locker locker(g_isolate);
        v8::Isolate::Scope isolate_scope(g_isolate);
        v8::HandleScope handle_scope(g_isolate);
        v8::Context::Scope context_scope(ctx());
        // A real main script, not "": node's networking is reached through the
        // module system, and an empty one leaves `require` undefined.
        node::LoadEnvironment(g_env, "globalThis.__wc_require = require;");
        uv_run(g_setup->event_loop(), UV_RUN_NOWAIT);
    }

    return 0;
}

static void threads_shutdown(wc_host_t* host);

// ─── Lifecycle ─────────────────────────────────────────────────────────

extern "C" wc_host_t* wc_host_create(void) {
    wc_host_t* host = (wc_host_t*)calloc(1, sizeof(wc_host_t));
    if (!host) return NULL;

    if (v8_init() != 0) {
        free(host);
        return NULL;
    }

    host->v8_state = new v8_host_state();
    return host;
}

extern "C" void wc_host_destroy(wc_host_t* host) {
    if (!host) return;
    if (host->v8_state) threads_shutdown(host);
    wc_archive_close(host);
    if (host->v8_state) {
        delete (v8_host_state*)host->v8_state;
    }
    free(host->text_queue);
    for (uint32_t i = 0; i < host->peer_count; i++) {
        for (uint32_t k = 0; k < host->peers[i].events_len; k++)
            free(host->peers[i].events[k].data);
        free(host->peers[i].events);
    }
    free(host->peers);
    free(host);
}

// ─── Parse wc_info_t from WASM memory (unchanged from wasmtime) ─────────

// REFUSE a cart built against an incompatible ABI, rather than reading its
// structs with the wrong offsets.
//
// MUST be called only once the cart has actually populated wc_info_t. The
// first wc_get_info() happens before wc_init, and a GL cart under libretro
// defers everything until the GL context exists -- in both cases the version
// legitimately still reads 0, so checking too early refuses every cart.
//
// This host never validated the version at all: WC_ABI_VERSION existed only
// as a default for a manifest that omitted one. That was survivable while
// layouts were additive and stopped being so at v4, which moved every field
// after wc_pad_t.buttons. A v3 cart would otherwise load and read
// `connected` out of a trigger byte, reporting every pad as unplugged: a
// silent, baffling failure instead of a clear one.
static int check_abi_version(wc_host_t* host) {
    if (host->info.version < WC_MIN_ABI_VERSION || host->info.version > WC_ABI_VERSION) {
        wc_log("wasmcart: ABI version mismatch: cart=%u, host supports %d-%d. "
               "Rebuild the cart against the current wasmcart.h.\n",
               host->info.version, WC_MIN_ABI_VERSION, WC_ABI_VERSION);
        return -1;
    }
    return 0;
}

static void parse_cart_info(wc_host_t* host, uint32_t info_ptr) {
    uint8_t* mem = host->memory;
    wc_cart_info_t* info = &host->info;

    info->version        = wc_read_u32(mem, info_ptr + WC_INFO_VERSION);
    info->width          = wc_read_u32(mem, info_ptr + WC_INFO_WIDTH);
    info->height         = wc_read_u32(mem, info_ptr + WC_INFO_HEIGHT);
    info->fb_ptr         = wc_read_u32(mem, info_ptr + WC_INFO_FB_PTR);
    info->audio_ptr      = wc_read_u32(mem, info_ptr + WC_INFO_AUDIO_PTR);
    info->audio_cap      = wc_read_u32(mem, info_ptr + WC_INFO_AUDIO_CAP);
    info->audio_write_ptr = wc_read_u32(mem, info_ptr + WC_INFO_AUDIO_WRITE);
    info->input_ptr      = wc_read_u32(mem, info_ptr + WC_INFO_INPUT_PTR);
    info->save_ptr       = wc_read_u32(mem, info_ptr + WC_INFO_SAVE_PTR);
    info->save_size      = wc_read_u32(mem, info_ptr + WC_INFO_SAVE_SIZE);
    info->time_ptr       = wc_read_u32(mem, info_ptr + WC_INFO_TIME_PTR);
    info->host_info_ptr  = wc_read_u32(mem, info_ptr + WC_INFO_HOST_INFO_PTR);

    info->flags            = wc_read_u32(mem, info_ptr + WC_INFO_FLAGS);
    info->audio_sample_rate = wc_read_u32(mem, info_ptr + WC_INFO_AUDIO_RATE);
    info->pointer_ptr      = wc_read_u32(mem, info_ptr + WC_INFO_POINTER_PTR);
    info->keys_ptr         = wc_read_u32(mem, info_ptr + WC_INFO_KEYS_PTR);
    info->gpu_api          = wc_read_u32(mem, info_ptr + WC_INFO_GPU_API);
    info->wheel_ptr        = wc_read_u32(mem, info_ptr + WC_INFO_WHEEL_PTR);

    // Determine rendering mode: gpu_api is authoritative when set. For old
    // carts built before the field existed (gpu_api == 0), fall back to
    // fb_ptr — but never DOWNGRADE a cart the import pre-scan already proved
    // is GL: a hybrid cart (gl imports + fb fallback surface) was being
    // forced onto the 2D path here, which presents its fallback framebuffer
    // and silently never runs its GL renderer.
    if (info->gpu_api > 0) {
        host->uses_gl = true;
    } else if (!host->uses_gl) {
        host->uses_gl = (info->fb_ptr == 0);
    }
}

static void write_host_info(wc_host_t* host, const wc_host_options_t* opts) {
    uint32_t ptr = host->info.host_info_ptr;
    if (ptr == 0) return;
    refresh_memory(host);
    uint8_t* mem = host->memory;
    // A manifest width/height is the cart author stating the size the cart
    // wants, so pass it as the host's preference when the caller has no
    // opinion of its own. Without this a runtime that sizes itself from
    // preferred_width (Defold and Godot both do) ignores the declaration and
    // falls back to its own default: a 1280x720 cart rendered at 960x540 with
    // its picture cropped. An explicit caller preference still wins, because
    // that is the host asking rather than the cart declaring.
    uint32_t pref_w = opts && opts->preferred_width  ? opts->preferred_width  : host->manifest.width;
    uint32_t pref_h = opts && opts->preferred_height ? opts->preferred_height : host->manifest.height;
    wc_write_u32(mem, ptr + WC_HOST_INFO_PREFERRED_WIDTH, pref_w);
    wc_write_u32(mem, ptr + WC_HOST_INFO_PREFERRED_HEIGHT, pref_h);
    wc_write_u32(mem, ptr + WC_HOST_INFO_HOST_FPS, opts ? opts->host_fps : 60);
    wc_write_u32(mem, ptr + WC_HOST_INFO_AUDIO_SAMPLE_RATE, opts ? opts->audio_sample_rate : 48000);
    wc_write_u32(mem, ptr + WC_HOST_INFO_FLAGS, 0);
}

// ─── Build WASM import object ──────────────────────────────────────────

// Helper: create a V8 function from a C callback that takes wc_host_t as data
typedef void (*host_fn_cb)(const v8::FunctionCallbackInfo<v8::Value>&);

static v8::Local<v8::Function> make_fn(host_fn_cb cb) {
    return v8::Function::New(ctx(), cb).ToLocalChecked();
}

// ─── Host function callbacks (env module) ─────────────────────────────

static wc_host_t* _current_host = nullptr; // set before instantiation

static void v8_wc_log(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t ptr = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t len = args[1]->Uint32Value(ctx()).FromJust();
    wc_log( "wasmcart [cart]: %.*s\n", (int)len, (const char*)(_current_host->memory + ptr));
}

static void v8_emscripten_notify_memory_growth(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint32_t old_size = _current_host->memory_size;
    refresh_memory(_current_host);
    wc_log("wasmcart: memory grew %u → %u bytes\n", old_size, _current_host->memory_size);
}

static void v8_emscripten_memcpy_js(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t dest = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t src = args[1]->Uint32Value(ctx()).FromJust();
    uint32_t n = args[2]->Uint32Value(ctx()).FromJust();
    memmove(_current_host->memory + dest, _current_host->memory + src, n);
}

static void v8_wc_asset_size(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t path_ptr = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t path_len = args[1]->Uint32Value(ctx()).FromJust();
    char path[512];
    uint32_t len = path_len < 511 ? path_len : 511;
    memcpy(path, _current_host->memory + path_ptr, len);
    path[len] = '\0';
    int32_t size = wc_archive_asset_size(_current_host, path);
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, size));
}

static void v8_wc_load_asset(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t path_ptr = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t path_len = args[1]->Uint32Value(ctx()).FromJust();
    uint32_t dest_ptr = args[2]->Uint32Value(ctx()).FromJust();
    uint32_t max_size = args[3]->Uint32Value(ctx()).FromJust();
    char path[512];
    uint32_t len = path_len < 511 ? path_len : 511;
    memcpy(path, _current_host->memory + path_ptr, len);
    path[len] = '\0';
    if (dest_ptr + max_size > _current_host->memory_size) {
        wc_log( "wasmcart: asset %s: dest_ptr=%u + max_size=%u > memory_size=%u\n",
            path, dest_ptr, max_size, _current_host->memory_size);
        args.GetReturnValue().Set(-1);
        return;
    }
    int32_t bytes = wc_archive_load_asset(_current_host, path,
        _current_host->memory + dest_ptr, max_size);
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, bytes));
}

static void v8_noop(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // no-op stub
}

static void v8_noop_return_0(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(0);
}


// ─── Rumble imports (ABI v3) ───────────────────────────────────────────────
//
// The cart drives these, unlike the rest of input. Clamping happens HERE rather
// than in each backend: SPEC.md says out-of-range values clamp instead of being
// rejected, because a cart deriving intensity from game state (damage, distance)
// overshoots at the edges and a dropped rumble is harder to diagnose than a
// saturated one. NaN becomes 0 -- note (v != v) is the NaN test, since a plain
// comparison chain would let it through.
static float wc_clamp01(double v) {
    if (v != v) return 0.0f;            // NaN
    if (v < 0.0) return 0.0f;
    if (v > 1.0) return 1.0f;
    return (float)v;
}

static void v8_wc_pad_has_rumble(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    uint32_t pad_id = args[0]->Uint32Value(ctx()).FromJust();
    int result = 0;
    if (host && host->rumble.has_rumble && pad_id < WC_MAX_PADS)
        result = host->rumble.has_rumble(host->rumble.user, pad_id) ? 1 : 0;
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, result));
}

static void v8_wc_pad_rumble(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    uint32_t pad_id = args[0]->Uint32Value(ctx()).FromJust();
    if (!host || !host->rumble.rumble || pad_id >= WC_MAX_PADS) return;

    float low  = wc_clamp01(args[1]->NumberValue(ctx()).FromMaybe(0.0));
    float high = wc_clamp01(args[2]->NumberValue(ctx()).FromMaybe(0.0));
    double raw = args[3]->NumberValue(ctx()).FromMaybe(0.0);
    if (raw != raw || raw < 0.0) raw = 0.0;
    uint32_t dur = (raw > (double)WC_RUMBLE_MAX_MS)
                       ? WC_RUMBLE_MAX_MS : (uint32_t)raw;
    /* Zero duration is a no-op, not an infinite effect: the cap exists so a
     * cart that crashes mid-effect cannot pin the motors forever. */
    if (dur == 0) return;
    host->rumble.rumble(host->rumble.user, pad_id, low, high, dur);
}

static void v8_wc_pad_rumble_stop(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    uint32_t pad_id = args[0]->Uint32Value(ctx()).FromJust();
    if (!host || !host->rumble.stop || pad_id >= WC_MAX_PADS) return;
    host->rumble.stop(host->rumble.user, pad_id);
}

// ─── Peer connections (ABI v3) ───────────────────────────────────────────

static wc_peer_t* peer_find(wc_host_t* host, int32_t id) {
    for (uint32_t i = 0; i < host->peer_count; i++)
        if (host->peers[i].id == id) return &host->peers[i];
    return NULL;
}

static wc_peer_t* peer_alloc(wc_host_t* host) {
    if (host->peer_count == host->peer_cap) {
        uint32_t cap = host->peer_cap ? host->peer_cap * 2 : 8;
        wc_peer_t* grown = (wc_peer_t*)realloc(host->peers, cap * sizeof(wc_peer_t));
        if (!grown) return NULL;
        host->peers = grown;
        host->peer_cap = cap;
    }
    wc_peer_t* p = &host->peers[host->peer_count++];
    memset(p, 0, sizeof(*p));
    p->id = host->peer_next_id++;
    p->state = WC_PEER_CONNECTING;
    return p;
}

/* Queue an event for delivery on the next frame. Sockets fire at arbitrary
 * points in node's loop, and re-entering the cart mid-frame is not safe. */
static void peer_queue(wc_peer_t* p, int type, const uint8_t* data, uint32_t len) {
    if (p->events_len == p->events_cap) {
        uint32_t cap = p->events_cap ? p->events_cap * 2 : 8;
        wc_peer_event_t* grown =
            (wc_peer_event_t*)realloc(p->events, cap * sizeof(wc_peer_event_t));
        if (!grown) return;  // drop the event rather than lose the cart
        p->events = grown;
        p->events_cap = cap;
    }
    wc_peer_event_t* ev = &p->events[p->events_len++];
    ev->type = type;
    ev->len = len;
    ev->data = NULL;
    if (data && len) {
        ev->data = (uint8_t*)malloc(len);
        if (ev->data) memcpy(ev->data, data, len);
        else { ev->len = 0; }
    }
}

/* Does the manifest grant this address? Mirrors the JS host exactly: the cart's
 * flag AND a net grant AND a ws/wss scheme AND the hostname on the allowlist.
 * Any one missing fails closed. */
static bool peer_addr_granted(wc_host_t* host, const char* addr) {
    if (!(host->info.flags & WC_FLAG_NET_PEER)) return false;
    if (!host->manifest.has_net) return false;

    /* Only ws:// and wss:// are implemented. A LAN or serial address would be
     * gated by its own grant class, which does not exist yet. */
    const char* rest;
    if (strncmp(addr, "ws://", 5) == 0)       rest = addr + 5;
    else if (strncmp(addr, "wss://", 6) == 0) rest = addr + 6;
    else return false;

    /* Hostname ends at ':', '/' or end of string. */
    char hostname[256];
    size_t n = 0;
    while (rest[n] && rest[n] != ':' && rest[n] != '/' && n < sizeof(hostname) - 1) {
        hostname[n] = rest[n];
        n++;
    }
    hostname[n] = 0;
    if (n == 0) return false;

    if (host->manifest.ws_domain_count == 0) return false;
    for (uint32_t i = 0; i < host->manifest.ws_domain_count; i++)
        if (strcmp(host->manifest.ws_domains[i], hostname) == 0) return true;
    return false;
}

/* Read a length-delimited string out of cart memory. Not NUL-terminated on the
 * cart side, so bounds are checked against memory_size rather than trusted. */
static bool read_cart_str(wc_host_t* host, uint32_t ptr, uint32_t len,
                          char* dest, size_t dest_cap) {
    if (!host->memory || len == 0 || len >= dest_cap) return false;
    if ((uint64_t)ptr + len > host->memory_size) return false;
    memcpy(dest, host->memory + ptr, len);
    dest[len] = 0;
    return true;
}

static void v8_wc_peer_open(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, -1));  // default: refused
    if (!host) return;

    char addr[512];
    uint32_t ptr = args[0]->Uint32Value(ctx()).FromMaybe(0);
    uint32_t len = args[1]->Uint32Value(ctx()).FromMaybe(0);
    if (!read_cart_str(host, ptr, len, addr, sizeof addr)) return;

    if (!peer_addr_granted(host, addr)) {
        wc_log("wasmcart: wc_peer_open refused; the cart must set "
               "WC_FLAG_NET_PEER and the manifest must grant that host\n");
        return;
    }

    wc_peer_t* peer = peer_alloc(host);
    if (!peer) return;
    peer->transport = WC_TRANSPORT_RELIABLE | WC_TRANSPORT_ORDERED;
    snprintf(peer->name, sizeof peer->name, "%s", addr);

    /* Dial with node's own WebSocket -- the same implementation the JS host
     * uses, so a cart needs no change between hosts. The socket lives on the JS
     * side and pushes events onto a per-peer queue we drain each frame.
     *
     * The address is passed as a FUNCTION ARGUMENT, never spliced into the
     * source: a cart-supplied string must not be able to close the quote and
     * inject JS. That is the one place cart input reaches a compiler here. */
    v8::Local<v8::String> src = v8str(
        "(function(id, url) {"
        "  const q = (globalThis.__wc_peerq ||= {});"
        "  const ev = (q[id] = []);"
        "  const w = new WebSocket(url);"
        "  w.binaryType = 'arraybuffer';"
        "  w.onopen    = () => ev.push({t:0});"
        "  w.onmessage = (m) => ev.push({t:1, d:(typeof m.data === 'string')"
        "      ? new TextEncoder().encode(m.data) : new Uint8Array(m.data)});"
        "  w.onclose   = () => ev.push({t:2});"
        "  w.onerror   = () => ev.push({t:3});"
        "  (globalThis.__wc_peersock ||= {})[id] = w;"
        "})");
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(ctx(), src).ToLocal(&script)) { host->peer_count--; return; }
    v8::Local<v8::Value> fnv;
    if (!script->Run(ctx()).ToLocal(&fnv) || !fnv->IsFunction()) { host->peer_count--; return; }

    v8::Local<v8::Value> argv[2] = {
        v8::Integer::New(g_isolate, peer->id),
        v8str(addr),
    };
    v8::TryCatch tc(g_isolate);
    node::CallbackScope scope(g_env, v8::Object::New(g_isolate), {0, 0});
    if (fnv.As<v8::Function>()->Call(ctx(), ctx()->Global(), 2, argv).IsEmpty()) {
        if (tc.HasCaught()) {
            v8::String::Utf8Value e(g_isolate, tc.Exception());
            wc_log("wasmcart: wc_peer_open failed: %s\n", *e);
        }
        host->peer_count--;
        return;
    }
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, peer->id));
}

/* Call a JS helper on the socket owned by the JS side. */
static bool peer_js_call(int32_t id, const char* method,
                         v8::Local<v8::Value> arg, bool has_arg) {
    v8::Local<v8::Value> socks;
    if (!ctx()->Global()->Get(ctx(), v8str("__wc_peersock")).ToLocal(&socks)
        || !socks->IsObject()) return false;
    v8::Local<v8::Value> w;
    if (!socks.As<v8::Object>()->Get(ctx(), v8::Integer::New(g_isolate, id)).ToLocal(&w)
        || !w->IsObject()) return false;
    v8::Local<v8::Value> fn;
    if (!w.As<v8::Object>()->Get(ctx(), v8str(method)).ToLocal(&fn) || !fn->IsFunction())
        return false;
    v8::TryCatch tc(g_isolate);
    node::CallbackScope scope(g_env, v8::Object::New(g_isolate), {0, 0});
    return !fn.As<v8::Function>()
                ->Call(ctx(), w, has_arg ? 1 : 0, has_arg ? &arg : nullptr)
                .IsEmpty();
}

static void v8_wc_peer_close(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    if (!host) return;
    int32_t id = args[0]->Int32Value(ctx()).FromMaybe(-1);
    wc_peer_t* p = peer_find(host, id);
    if (!p) return;
    p->state = WC_PEER_CLOSING;
    if (p->host_supplied) {
        peer_queue(p, WC_PEER_EV_DISCONNECT, NULL, 0);
        p->state = WC_PEER_CLOSED;
    } else {
        peer_js_call(id, "close", v8::Local<v8::Value>(), false);
    }
}

static int peer_send_bytes(wc_host_t* host, wc_peer_t* p,
                           uint32_t ptr, uint32_t len) {
    if (!host->memory || len == 0) return -1;
    if ((uint64_t)ptr + len > host->memory_size) return -1;   /* untrusted */
    if (p->state != WC_PEER_OPEN) return -1;

    if (p->host_supplied) {
        if (!p->send) return -1;
        return p->send(p->user, host->memory + ptr, len) == 0 ? (int)len : -1;
    }
    /* Copy into a JS-owned buffer: the cart's memory can move under us when it
     * grows, so handing a view straight to the socket is not safe. */
    v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(g_isolate, len);
    memcpy(ab->Data(), host->memory + ptr, len);
    v8::Local<v8::Value> view = v8::Uint8Array::New(ab, 0, len);
    return peer_js_call(p->id, "send", view, true) ? (int)len : -1;
}

static void v8_wc_peer_send(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, -1));
    if (!host) return;
    wc_peer_t* p = peer_find(host, args[0]->Int32Value(ctx()).FromMaybe(-1));
    if (!p) return;
    int r = peer_send_bytes(host, p,
                            args[1]->Uint32Value(ctx()).FromMaybe(0),
                            args[2]->Uint32Value(ctx()).FromMaybe(0));
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, r));
}

static void v8_wc_peer_broadcast(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, 0));
    if (!host) return;
    uint32_t ptr = args[0]->Uint32Value(ctx()).FromMaybe(0);
    uint32_t len = args[1]->Uint32Value(ctx()).FromMaybe(0);
    int sent = 0;
    for (uint32_t i = 0; i < host->peer_count; i++)
        if (peer_send_bytes(host, &host->peers[i], ptr, len) > 0) sent++;
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, sent));
}

static void v8_wc_peer_state(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    wc_peer_t* p = host ? peer_find(host, args[0]->Int32Value(ctx()).FromMaybe(-1)) : NULL;
    args.GetReturnValue().Set(
        v8::Integer::New(g_isolate, p ? p->state : WC_PEER_CLOSED));
}

static void v8_wc_peer_count(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    args.GetReturnValue().Set(
        v8::Integer::New(g_isolate, host ? (int)host->peer_count : 0));
}

static void v8_wc_peer_id(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    uint32_t idx = args[0]->Uint32Value(ctx()).FromMaybe(0);
    args.GetReturnValue().Set(v8::Integer::New(g_isolate,
        (host && idx < host->peer_count) ? host->peers[idx].id : -1));
}

static void v8_wc_peer_name(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, 0));
    if (!host || !host->memory) return;
    wc_peer_t* p = peer_find(host, args[0]->Int32Value(ctx()).FromMaybe(-1));
    if (!p) return;
    uint32_t dest = args[1]->Uint32Value(ctx()).FromMaybe(0);
    uint32_t cap  = args[2]->Uint32Value(ctx()).FromMaybe(0);
    if (cap == 0 || (uint64_t)dest + cap > host->memory_size) return;

    /* Truncate the TEXT and always NUL-terminate. Writing cap bytes and losing
     * the terminator would hand the cart an unterminated string -- the same
     * defect the JS host had and fixed. */
    size_t n = strlen(p->name);
    if (n > cap - 1) n = cap - 1;
    memcpy(host->memory + dest, p->name, n);
    host->memory[dest + n] = 0;
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, (int)n));
}

static void v8_wc_peer_transport(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    wc_peer_t* p = host ? peer_find(host, args[0]->Int32Value(ctx()).FromMaybe(-1)) : NULL;
    args.GetReturnValue().Set(
        v8::Integer::New(g_isolate, p ? (int)p->transport : WC_TRANSPORT_UNKNOWN));
}

// ─── Text input (ABI v3) ─────────────────────────────────────────────────
// Three imports the cart drives, mirroring SDL_StartTextInput/StopTextInput.
// Always provided, so a cart using text input is never a cart that fails to
// load; with no platform feeding wc_host_push_text they simply never fire.

static void v8_wc_text_input_begin(const v8::FunctionCallbackInfo<v8::Value>& args) {
    (void)args;
    if (_current_host) _current_host->text_active = true;
}

static void v8_wc_text_input_end(const v8::FunctionCallbackInfo<v8::Value>& args) {
    (void)args;
    wc_host_t* host = _current_host;
    if (!host) return;
    host->text_active = false;
    // Drop anything queued but undelivered: text typed before the cart stopped
    // listening must not surface in the next field it opens.
    host->text_queue_len = 0;
}

static void v8_wc_text_input_active(const v8::FunctionCallbackInfo<v8::Value>& args) {
    wc_host_t* host = _current_host;
    args.GetReturnValue().Set(
        v8::Integer::New(g_isolate, (host && host->text_active) ? 1 : 0));
}

// Build the env import object
static v8::Local<v8::Object> build_env_imports() {
    auto env = v8::Object::New(g_isolate);
    env->Set(ctx(), v8str("wc_log"), make_fn(v8_wc_log)).Check();
    env->Set(ctx(), v8str("wc_asset_size"), make_fn(v8_wc_asset_size)).Check();
    env->Set(ctx(), v8str("wc_load_asset"), make_fn(v8_wc_load_asset)).Check();
    env->Set(ctx(), v8str("emscripten_notify_memory_growth"), make_fn(v8_emscripten_notify_memory_growth)).Check();
    env->Set(ctx(), v8str("emscripten_memcpy_js"), make_fn(v8_emscripten_memcpy_js)).Check();
    env->Set(ctx(), v8str("emscripten_stack_init"), make_fn(v8_noop)).Check();
    env->Set(ctx(), v8str("__cxa_atexit"), make_fn(v8_noop_return_0)).Check();
    // Rumble: always provided, so a cart that rumbles is never a cart that
    // fails to load. With no backend wired they are silent no-ops.
    env->Set(ctx(), v8str("wc_pad_has_rumble"), make_fn(v8_wc_pad_has_rumble)).Check();
    env->Set(ctx(), v8str("wc_pad_rumble"), make_fn(v8_wc_pad_rumble)).Check();
    env->Set(ctx(), v8str("wc_pad_rumble_stop"), make_fn(v8_wc_pad_rumble_stop)).Check();
    // Text input: likewise always provided.
    env->Set(ctx(), v8str("wc_text_input_begin"), make_fn(v8_wc_text_input_begin)).Check();
    env->Set(ctx(), v8str("wc_text_input_end"), make_fn(v8_wc_text_input_end)).Check();
    env->Set(ctx(), v8str("wc_text_input_active"), make_fn(v8_wc_text_input_active)).Check();
    // Peer connections: always provided, so a networked cart links even where
    // nothing can grant it access. wc_peer_open then refuses with -1, which the
    // cart can see and act on -- unlike a missing import, which is a LinkError.
    env->Set(ctx(), v8str("wc_peer_open"), make_fn(v8_wc_peer_open)).Check();
    env->Set(ctx(), v8str("wc_peer_close"), make_fn(v8_wc_peer_close)).Check();
    env->Set(ctx(), v8str("wc_peer_send"), make_fn(v8_wc_peer_send)).Check();
    env->Set(ctx(), v8str("wc_peer_broadcast"), make_fn(v8_wc_peer_broadcast)).Check();
    env->Set(ctx(), v8str("wc_peer_state"), make_fn(v8_wc_peer_state)).Check();
    env->Set(ctx(), v8str("wc_peer_count"), make_fn(v8_wc_peer_count)).Check();
    env->Set(ctx(), v8str("wc_peer_id"), make_fn(v8_wc_peer_id)).Check();
    env->Set(ctx(), v8str("wc_peer_name"), make_fn(v8_wc_peer_name)).Check();
    env->Set(ctx(), v8str("wc_peer_transport"), make_fn(v8_wc_peer_transport)).Check();
    return env;
}

// ─── GL import callbacks will be registered by gl_imports.c ──────────
// gl_imports.c calls wc_gl_imports_init which uses the wasmtime linker.
// For V8, we need a different registration path. For now, we'll build
// the gl import object here and have gl_imports provide the callbacks.

// Helpers for gl_imports.cpp to access V8 state
extern "C" v8::Global<v8::Function>* wc_get_malloc_fn(wc_host_t* host) {
    auto state = (v8_host_state*)host->v8_state;
    return &state->fn_malloc;
}

extern "C" void wc_refresh_memory(wc_host_t* host) {
    refresh_memory(host);
}

// Forward declaration — gl_imports.cpp provides this
extern "C" void wc_gl_build_v8_imports(v8::Isolate* isolate, v8::Local<v8::Context> context,
    v8::Local<v8::Object> gl_obj, v8::Local<v8::Object> env_obj, wc_host_t* host);

// ─── WASI stub imports ──────────────────────────────────────────────────

static void v8_fd_write(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // fd_write(fd, iovs_ptr, iovs_len, nwritten_ptr) -> errno
    refresh_memory(_current_host);
    uint32_t fd = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t iovs_ptr = args[1]->Uint32Value(ctx()).FromJust();
    uint32_t iovs_len = args[2]->Uint32Value(ctx()).FromJust();
    uint32_t nwritten_ptr = args[3]->Uint32Value(ctx()).FromJust();

    uint8_t* mem = _current_host->memory;
    uint32_t total = 0;
    for (uint32_t i = 0; i < iovs_len; i++) {
        uint32_t buf_ptr = *(uint32_t*)(mem + iovs_ptr + i * 8);
        uint32_t buf_len = *(uint32_t*)(mem + iovs_ptr + i * 8 + 4);
        if (fd == 1) fwrite(mem + buf_ptr, 1, buf_len, stdout);
        else if (fd == 2) fwrite(mem + buf_ptr, 1, buf_len, stderr);
        total += buf_len;
    }
    *(uint32_t*)(mem + nwritten_ptr) = total;
    args.GetReturnValue().Set(0); // success
}

// WASI errno values used below.
#define WC_WASI_EBADF   8
#define WC_WASI_EINVAL  28
#define WC_WASI_ENOTSUP 58

// True when [ptr, ptr+len) lies inside the cart's memory. Cart pointers are
// untrusted; an out-of-range one must not become a host write.
static bool cart_range_ok(uint32_t ptr, uint64_t len) {
    return _current_host && _current_host->memory &&
           (uint64_t)ptr + len <= _current_host->memory_size;
}

static void v8_fd_read(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // No stdin: report end of file (nread = 0) rather than leave nread unset.
    refresh_memory(_current_host);
    uint32_t nread_ptr = args[3]->Uint32Value(ctx()).FromMaybe(0);
    if (cart_range_ok(nread_ptr, 4)) wc_write_u32(_current_host->memory, nread_ptr, 0);
    args.GetReturnValue().Set(0);
}

static void v8_fd_close(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(0);
}

static void v8_fd_seek(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(0);
}

// stdin/stdout/stderr are character devices; there are no other descriptors.
static void v8_fd_fdstat_get(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t fd = args[0]->Uint32Value(ctx()).FromMaybe(0);
    uint32_t ptr = args[1]->Uint32Value(ctx()).FromMaybe(0);
    if (fd > 2) { args.GetReturnValue().Set(WC_WASI_EBADF); return; }
    if (!cart_range_ok(ptr, 24)) { args.GetReturnValue().Set(WC_WASI_EINVAL); return; }
    uint8_t* m = _current_host->memory + ptr;
    memset(m, 0, 24);
    m[0] = 2;                               // filetype: character device
    m[2] = fd == 0 ? 0 : 1;                 // fdflags: append for stdout/stderr
    memset(m + 8, 0xff, 16);                // rights base + inheriting
    args.GetReturnValue().Set(0);
}

static void v8_fd_fdstat_set_flags(const v8::FunctionCallbackInfo<v8::Value>& args) {
    uint32_t fd = args[0]->Uint32Value(ctx()).FromMaybe(0);
    args.GetReturnValue().Set(fd > 2 ? WC_WASI_EBADF : 0);
}

static void v8_fd_filestat_get(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t fd = args[0]->Uint32Value(ctx()).FromMaybe(0);
    uint32_t ptr = args[1]->Uint32Value(ctx()).FromMaybe(0);
    if (fd > 2) { args.GetReturnValue().Set(WC_WASI_EBADF); return; }
    if (!cart_range_ok(ptr, 64)) { args.GetReturnValue().Set(WC_WASI_EINVAL); return; }
    memset(_current_host->memory + ptr, 0, 64);
    _current_host->memory[ptr + 16] = 2;    // filetype: character device
    args.GetReturnValue().Set(0);
}

// No preopened directories, so no filesystem: the preopen scan in wasi-libc
// stops at the first EBADF. Returning success here (the old stub) made that
// scan walk every descriptor number with an uninitialised prestat.
static void v8_return_ebadf(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(WC_WASI_EBADF);
}

// environ_sizes_get / args_sizes_get: both counts are zero, and are WRITTEN --
// libc reads them straight back to size its arrays.
static void v8_sizes_get_zero(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t count_ptr = args[0]->Uint32Value(ctx()).FromMaybe(0);
    uint32_t size_ptr = args[1]->Uint32Value(ctx()).FromMaybe(0);
    if (cart_range_ok(count_ptr, 4)) wc_write_u32(_current_host->memory, count_ptr, 0);
    if (cart_range_ok(size_ptr, 4)) wc_write_u32(_current_host->memory, size_ptr, 0);
    args.GetReturnValue().Set(0);
}

static void v8_proc_exit(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // proc_exit never returns. Returning into the cart lands on the
    // unreachable libc places after it, so stop the call here instead, with a
    // message that says what actually happened.
    int32_t code = args[0]->Int32Value(ctx()).FromMaybe(0);
    char msg[64];
    snprintf(msg, sizeof msg, "cart called proc_exit(%d)", code);
    g_isolate->ThrowException(v8::Exception::Error(v8str(msg)));
}

// One clock for every thread: uv_hrtime is what a worker's
// process.hrtime.bigint() reads, so a deadline computed on one thread means
// the same instant on another (pthread_cond_timedwait depends on that).
static uint64_t wasi_now_ns(void) {
    return uv_hrtime();
}

static void v8_clock_time_get(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // clock_time_get(id, precision, timestamp_ptr) -> errno
    refresh_memory(_current_host);
    uint32_t ts_ptr = args[2]->Uint32Value(ctx()).FromJust();
    uint64_t nanos = wasi_now_ns();
    if (!cart_range_ok(ts_ptr, 8)) { args.GetReturnValue().Set(WC_WASI_EINVAL); return; }
    memcpy(_current_host->memory + ts_ptr, &nanos, 8);
    args.GetReturnValue().Set(0);
}

static void v8_clock_res_get(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t ptr = args[1]->Uint32Value(ctx()).FromMaybe(0);
    uint64_t res = 1000;  // 1us
    if (!cart_range_ok(ptr, 8)) { args.GetReturnValue().Set(WC_WASI_EINVAL); return; }
    memcpy(_current_host->memory + ptr, &res, 8);
    args.GetReturnValue().Set(0);
}

static void v8_sched_yield(const v8::FunctionCallbackInfo<v8::Value>& args) {
    std::this_thread::yield();
    args.GetReturnValue().Set(0);
}

// poll_oneoff(in, out, nsubscriptions, nevents_ptr) -> errno
// Clock subscriptions sleep until the earliest one is due (this is how
// wasi-libc implements nanosleep/usleep). fd subscriptions are reported at once
// with ENOTSUP: there is nothing to wait on.
static void v8_poll_oneoff(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t in_ptr = args[0]->Uint32Value(ctx()).FromMaybe(0);
    uint32_t out_ptr = args[1]->Uint32Value(ctx()).FromMaybe(0);
    uint32_t nsubs = args[2]->Uint32Value(ctx()).FromMaybe(0);
    uint32_t nevents_ptr = args[3]->Uint32Value(ctx()).FromMaybe(0);
    if (nsubs == 0) { args.GetReturnValue().Set(WC_WASI_EINVAL); return; }
    if (!cart_range_ok(in_ptr, (uint64_t)nsubs * 48) || !cart_range_ok(out_ptr, (uint64_t)nsubs * 32) ||
        !cart_range_ok(nevents_ptr, 4)) {
        args.GetReturnValue().Set(WC_WASI_EINVAL);
        return;
    }
    uint64_t now = wasi_now_ns();
    bool have_clock = false;
    uint64_t wait_ns = 0;
    for (uint32_t i = 0; i < nsubs; i++) {
        const uint8_t* sub = _current_host->memory + in_ptr + i * 48;
        if (sub[8] != 0) continue;           // not a clock subscription
        uint64_t timeout; uint16_t flags;
        memcpy(&timeout, sub + 24, 8);
        memcpy(&flags, sub + 40, 2);
        if (flags & 1) timeout = timeout > now ? timeout - now : 0;   // absolute
        if (!have_clock || timeout < wait_ns) wait_ns = timeout;
        have_clock = true;
    }
    if (have_clock && wait_ns > 0)
        std::this_thread::sleep_for(std::chrono::nanoseconds(wait_ns));
    refresh_memory(_current_host);           // another thread may have grown memory
    for (uint32_t i = 0; i < nsubs; i++) {
        const uint8_t* sub = _current_host->memory + in_ptr + i * 48;
        uint8_t* ev = _current_host->memory + out_ptr + i * 32;
        uint8_t type = sub[8];
        memset(ev, 0, 32);
        memcpy(ev, sub, 8);                   // userdata
        uint16_t err = type == 0 ? 0 : WC_WASI_ENOTSUP;
        memcpy(ev + 8, &err, 2);
        ev[10] = type;
    }
    wc_write_u32(_current_host->memory, nevents_ptr, nsubs);
    args.GetReturnValue().Set(0);
}

static void v8_random_get(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t buf_ptr = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t buf_len = args[1]->Uint32Value(ctx()).FromJust();
    uint8_t* dest = _current_host->memory + buf_ptr;
    for (uint32_t i = 0; i < buf_len; i++) dest[i] = rand() & 0xff;
    args.GetReturnValue().Set(0);
}

static v8::Local<v8::Object> build_wasi_imports() {
    auto wasi = v8::Object::New(g_isolate);
    wasi->Set(ctx(), v8str("fd_write"), make_fn(v8_fd_write)).Check();
    wasi->Set(ctx(), v8str("fd_close"), make_fn(v8_fd_close)).Check();
    wasi->Set(ctx(), v8str("fd_seek"), make_fn(v8_fd_seek)).Check();
    wasi->Set(ctx(), v8str("fd_read"), make_fn(v8_fd_read)).Check();
    wasi->Set(ctx(), v8str("fd_fdstat_get"), make_fn(v8_fd_fdstat_get)).Check();
    wasi->Set(ctx(), v8str("fd_fdstat_set_flags"), make_fn(v8_fd_fdstat_set_flags)).Check();
    wasi->Set(ctx(), v8str("fd_filestat_get"), make_fn(v8_fd_filestat_get)).Check();
    wasi->Set(ctx(), v8str("fd_prestat_get"), make_fn(v8_return_ebadf)).Check();
    wasi->Set(ctx(), v8str("fd_prestat_dir_name"), make_fn(v8_return_ebadf)).Check();
    wasi->Set(ctx(), v8str("path_open"), make_fn(v8_return_ebadf)).Check();
    wasi->Set(ctx(), v8str("path_filestat_get"), make_fn(v8_return_ebadf)).Check();
    wasi->Set(ctx(), v8str("proc_exit"), make_fn(v8_proc_exit)).Check();
    wasi->Set(ctx(), v8str("clock_time_get"), make_fn(v8_clock_time_get)).Check();
    wasi->Set(ctx(), v8str("clock_res_get"), make_fn(v8_clock_res_get)).Check();
    wasi->Set(ctx(), v8str("random_get"), make_fn(v8_random_get)).Check();
    wasi->Set(ctx(), v8str("sched_yield"), make_fn(v8_sched_yield)).Check();
    wasi->Set(ctx(), v8str("poll_oneoff"), make_fn(v8_poll_oneoff)).Check();
    wasi->Set(ctx(), v8str("environ_get"), make_fn(v8_noop_return_0)).Check();
    wasi->Set(ctx(), v8str("environ_sizes_get"), make_fn(v8_sizes_get_zero)).Check();
    wasi->Set(ctx(), v8str("args_get"), make_fn(v8_noop_return_0)).Check();
    wasi->Set(ctx(), v8str("args_sizes_get"), make_fn(v8_sizes_get_zero)).Check();
    return wasi;
}

// ─── WASI threads ───────────────────────────────────────────────────────

// Limits of the module's imported memory, read from the import section. The
// JS API cannot tell us these (WebAssembly.Module.imports gives no type), and
// a shared memory import only links against a memory with the same initial,
// maximum and shared flag -- the old 1-page stub was a LinkError.
struct wasm_memory_import {
    std::string module, name;
    uint64_t initial = 0, maximum = 0;
    bool has_max = false, shared = false, is64 = false;
};

static bool read_leb_u64(const uint8_t* b, size_t len, size_t* pos, uint64_t* out) {
    uint64_t result = 0;
    unsigned shift = 0;
    while (*pos < len && shift < 64) {
        uint8_t byte = b[(*pos)++];
        result |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) { *out = result; return true; }
        shift += 7;
    }
    return false;
}

static bool parse_memory_import(const uint8_t* b, size_t len, wasm_memory_import* out) {
    size_t pos = 8;  // magic + version
    while (pos < len) {
        uint8_t id = b[pos++];
        uint64_t size;
        if (!read_leb_u64(b, len, &pos, &size) || pos + size > len) return false;
        size_t end = pos + (size_t)size;
        if (id != 2) { pos = end; continue; }        // only the import section
        uint64_t count;
        if (!read_leb_u64(b, len, &pos, &count)) return false;
        for (uint64_t i = 0; i < count && pos < end; i++) {
            uint64_t n;
            if (!read_leb_u64(b, len, &pos, &n) || pos + n > end) return false;
            std::string mod((const char*)b + pos, (size_t)n); pos += (size_t)n;
            if (!read_leb_u64(b, len, &pos, &n) || pos + n > end) return false;
            std::string name((const char*)b + pos, (size_t)n); pos += (size_t)n;
            if (pos >= end) return false;
            uint8_t kind = b[pos++];
            uint64_t tmp;
            switch (kind) {
            case 0x00:  // function: type index
                if (!read_leb_u64(b, len, &pos, &tmp)) return false;
                break;
            case 0x01: {  // table: reftype, limits
                pos++;
                if (pos >= end) return false;
                uint8_t f = b[pos++];
                if (!read_leb_u64(b, len, &pos, &tmp)) return false;
                if ((f & 1) && !read_leb_u64(b, len, &pos, &tmp)) return false;
                break;
            }
            case 0x02: {  // memory: limits
                if (pos >= end) return false;
                uint8_t f = b[pos++];
                out->module = mod;
                out->name = name;
                out->has_max = f & 1;
                out->shared = (f & 2) != 0;
                out->is64 = (f & 4) != 0;
                if (!read_leb_u64(b, len, &pos, &out->initial)) return false;
                if (out->has_max && !read_leb_u64(b, len, &pos, &out->maximum)) return false;
                return true;
            }
            case 0x03:  // global: valtype, mutability
                pos += 2;
                break;
            case 0x04:  // tag: attribute, type index
                pos++;
                if (!read_leb_u64(b, len, &pos, &tmp)) return false;
                break;
            default:
                return false;
            }
        }
        return false;  // import section without a memory import
    }
    return false;
}

static void v8_thread_spawn(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(-1);
    if (!_current_host) return;
    auto state = (v8_host_state*)_current_host->v8_state;
    if (state->thread_spawn_fn.IsEmpty()) return;
    v8::Local<v8::Value> argv[1] = { args[0] };
    v8::TryCatch tc(g_isolate);
    v8::Local<v8::Value> r;
    if (!state->thread_spawn_fn.Get(g_isolate)->Call(ctx(), ctx()->Global(), 1, argv).ToLocal(&r)) {
        if (tc.HasCaught()) {
            v8::String::Utf8Value e(g_isolate, tc.Exception());
            wc_log("wasmcart: thread-spawn failed: %s\n", *e);
        }
        return;
    }
    args.GetReturnValue().Set(r->Int32Value(ctx()).FromMaybe(-1));
}

// What a worker needs to read assets on its own: the .wasc path and where each
// entry sits in it. Workers cannot call into this C code (they are separate
// isolates), and the main thread may be parked in a futex wait, so they read
// the archive themselves with the same lookup rules as asset_loader.c.
static v8::Local<v8::Object> build_thread_cfg(wc_host_t* host, const char* wasc_path) {
    auto cfg = v8::Object::New(g_isolate);
    auto index = v8::Object::New(g_isolate);
    mz_zip_archive* zip = (mz_zip_archive*)host->archive;
    if (zip) {
        mz_uint n = mz_zip_reader_get_num_files(zip);
        for (mz_uint i = 0; i < n; i++) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(zip, i, &st) || st.m_is_directory) continue;
            auto e = v8::Object::New(g_isolate);
            e->Set(ctx(), v8str("ofs"), v8::Number::New(g_isolate, (double)st.m_local_header_ofs)).Check();
            e->Set(ctx(), v8str("csize"), v8::Number::New(g_isolate, (double)st.m_comp_size)).Check();
            e->Set(ctx(), v8str("usize"), v8::Number::New(g_isolate, (double)st.m_uncomp_size)).Check();
            e->Set(ctx(), v8str("method"), v8::Integer::New(g_isolate, st.m_method)).Check();
            index->Set(ctx(), v8str(st.m_filename), e).Check();
        }
    }
    char path_buf[4096];
    const char* abs_path = wasc_path;
#ifndef _WIN32
    if (realpath(wasc_path, path_buf)) abs_path = path_buf;
#else
    if (_fullpath(path_buf, wasc_path, sizeof path_buf)) abs_path = path_buf;
#endif
    cfg->Set(ctx(), v8str("wascPath"), v8str(abs_path)).Check();
    cfg->Set(ctx(), v8str("zipIndex"), zip ? index.As<v8::Value>() : v8::Null(g_isolate).As<v8::Value>()).Check();
    cfg->Set(ctx(), v8str("assetsRoot"), v8str(host->manifest.assets)).Check();
    // _filelist.txt: same body the main thread serves.
    int32_t flen = wc_archive_asset_size(host, "_filelist.txt");
    if (flen >= 0) {
        std::vector<uint8_t> buf((size_t)flen + 1);
        int32_t got = wc_archive_load_asset(host, "_filelist.txt", buf.data(), (uint32_t)flen);
        cfg->Set(ctx(), v8str("fileList"),
            v8::String::NewFromUtf8(g_isolate, (const char*)buf.data(), v8::NewStringType::kNormal,
                got > 0 ? got : 0).ToLocalChecked()).Check();
    } else {
        cfg->Set(ctx(), v8str("fileList"), v8::Null(g_isolate)).Check();
    }
    return cfg;
}

// Stop every cart thread. Called from wc_host_destroy, outside the frame loop's
// persistent V8 scopes. A thread parked in memory.atomic.wait wakes for
// terminate() (V8 interrupts futex waits); the loop is pumped so the exits are
// observed before the environment goes away.
static void threads_shutdown(wc_host_t* host) {
    auto state = (v8_host_state*)host->v8_state;
    if (!state || state->threads.IsEmpty()) return;
    v8::Locker locker(g_isolate);
    v8::Isolate::Scope isolate_scope(g_isolate);
    v8::HandleScope handle_scope(g_isolate);
    v8::Context::Scope context_scope(ctx());
    auto threads = state->threads.Get(g_isolate);
    auto call = [&](const char* name) -> int32_t {
        v8::Local<v8::Value> fn;
        if (!threads->Get(ctx(), v8str(name)).ToLocal(&fn) || !fn->IsFunction()) return 0;
        v8::TryCatch tc(g_isolate);
        v8::Local<v8::Value> r;
        if (!fn.As<v8::Function>()->Call(ctx(), threads, 0, nullptr).ToLocal(&r)) return 0;
        return r->Int32Value(ctx()).FromMaybe(0);
    };
    call("shutdown");
    uint64_t deadline = uv_hrtime() + 2000000000ULL;  // 2s
    while (call("count") > 0 && uv_hrtime() < deadline) {
        {
            node::CallbackScope scope(g_env, v8::Object::New(g_isolate), {0, 0});
        }
        uv_run(g_setup->event_loop(), UV_RUN_NOWAIT);
        g_isolate->PerformMicrotaskCheckpoint();
        uv_sleep(5);
    }
    int32_t left = call("count");
    if (left > 0) wc_log("wasmcart: %d cart thread(s) did not stop within 2s\n", left);
    state->thread_spawn_fn.Reset();
    state->threads.Reset();
}

// ─── Load cart ─────────────────────────────────────────────────────────

extern "C" int wc_host_load_file(wc_host_t* host, const char* wasc_path, const wc_host_options_t* opts) {
    v8::Locker locker(g_isolate);
    v8::Isolate::Scope isolate_scope(g_isolate);
    v8::HandleScope handle_scope(g_isolate);
    v8::Context::Scope context_scope(ctx());

    _current_host = host;
    auto state = (v8_host_state*)host->v8_state;

    // 1. Open archive and read wasm bytes
    int rc = wc_archive_open(host, wasc_path);
    if (rc != 0) {
        wc_log( "wasmcart: failed to open %s\n", wasc_path);
        return rc;
    }

    // 2. Compile WASM module — V8 Liftoff baseline is near-instant
    v8::TryCatch try_catch(g_isolate);

    // Compile WASM module — V8 Liftoff baseline compiles instantly
    v8::MemorySpan<const uint8_t> wire_bytes(host->wasm_bytes, host->wasm_bytes_len);
    auto maybe_module = v8::WasmModuleObject::Compile(g_isolate, wire_bytes);
    if (maybe_module.IsEmpty()) {
        if (try_catch.HasCaught()) {
            v8::String::Utf8Value err(g_isolate, try_catch.Exception());
            wc_log( "wasmcart: compile error: %s\n", *err);
        }
        return -1;
    }
    auto wasm_module = maybe_module.ToLocalChecked();

    // Need WebAssembly.Instance constructor for instantiation (V8 has no C++ instantiate API)
    auto global = ctx()->Global();
    auto wasm_ns = global->Get(ctx(), v8str("WebAssembly")).ToLocalChecked().As<v8::Object>();
    auto wasm_instance_ctor = wasm_ns->Get(ctx(), v8str("Instance")).ToLocalChecked().As<v8::Function>();

    // 3. Build import object
    auto imports = v8::Object::New(g_isolate);
    auto env_imports = build_env_imports();
    auto wasi_imports = build_wasi_imports();

    imports->Set(ctx(), v8str("env"), env_imports).Check();
    imports->Set(ctx(), v8str("wasi_snapshot_preview1"), wasi_imports).Check();
    imports->Set(ctx(), v8str("wasi_unstable"), wasi_imports).Check();

    // GL imports
    auto gl_imports_obj = v8::Object::New(g_isolate);
    wc_gl_build_v8_imports(g_isolate, ctx(), gl_imports_obj, env_imports, host);
    imports->Set(ctx(), v8str("gl"), gl_imports_obj).Check();

    // 3b. Imported memory and WASI threads.
    //
    // A module that IMPORTS its memory (every wasm32-wasip1-threads cart does,
    // shared) needs one built to its own declared limits; the generic stub
    // below made a 1-page memory, which fails to link. Threads need that
    // memory shared, plus a real wasi.thread-spawn. Mirrors CartHost.js.
    v8::Local<v8::Object> imported_memory;
    {
        auto wasm_module_ns = wasm_ns->Get(ctx(), v8str("Module")).ToLocalChecked().As<v8::Object>();
        auto list = [&](const char* which) {
            auto fn = wasm_module_ns->Get(ctx(), v8str(which)).ToLocalChecked().As<v8::Function>();
            v8::Local<v8::Value> a[] = { wasm_module };
            return fn->Call(ctx(), wasm_ns, 1, a).ToLocalChecked().As<v8::Array>();
        };
        auto has = [&](v8::Local<v8::Array> arr, const char* mod, const char* name, const char* kind) {
            for (uint32_t i = 0; i < arr->Length(); i++) {
                auto e = arr->Get(ctx(), i).ToLocalChecked().As<v8::Object>();
                if (mod) {
                    v8::String::Utf8Value m(g_isolate, e->Get(ctx(), v8str("module")).ToLocalChecked());
                    if (strcmp(*m, mod) != 0) continue;
                }
                v8::String::Utf8Value n(g_isolate, e->Get(ctx(), v8str("name")).ToLocalChecked());
                v8::String::Utf8Value k(g_isolate, e->Get(ctx(), v8str("kind")).ToLocalChecked());
                if (strcmp(*n, name) == 0 && strcmp(*k, kind) == 0) return true;
            }
            return false;
        };
        bool spawn_import = has(list("imports"), "wasi", "thread-spawn", "function");
        bool start_export = has(list("exports"), nullptr, "wasi_thread_start", "function");
        if (spawn_import != start_export) {
            wc_log("wasmcart: cart %s but does not %s. Both are required for WASI threads.\n",
                spawn_import ? "imports wasi.thread-spawn" : "exports wasi_thread_start",
                spawn_import ? "export wasi_thread_start" : "import wasi.thread-spawn");
            return -1;
        }
        host->threaded = spawn_import;

        wasm_memory_import mi;
        if (parse_memory_import(host->wasm_bytes, host->wasm_bytes_len, &mi)) {
            if (mi.is64) {
                wc_log("wasmcart: cart imports a memory64 memory, which is not supported\n");
                return -1;
            }
            if (host->threaded && !mi.shared) {
                wc_log("wasmcart: threaded cart must import a SHARED memory (link with --shared-memory)\n");
                return -1;
            }
            auto mem_ctor = wasm_ns->Get(ctx(), v8str("Memory")).ToLocalChecked().As<v8::Function>();
            auto desc = v8::Object::New(g_isolate);
            desc->Set(ctx(), v8str("initial"), v8::Number::New(g_isolate, (double)mi.initial)).Check();
            if (mi.has_max)
                desc->Set(ctx(), v8str("maximum"), v8::Number::New(g_isolate, (double)mi.maximum)).Check();
            if (mi.shared)
                desc->Set(ctx(), v8str("shared"), v8::True(g_isolate)).Check();
            v8::Local<v8::Value> m_args[] = { desc };
            v8::Local<v8::Object> mem;
            if (!mem_ctor->NewInstance(ctx(), 1, m_args).ToLocal(&mem)) {
                if (try_catch.HasCaught()) {
                    v8::String::Utf8Value err(g_isolate, try_catch.Exception());
                    wc_log("wasmcart: cannot create the cart's imported memory: %s\n", *err);
                }
                return -1;
            }
            auto mod_str = v8str(mi.module.c_str());
            auto mod_val = imports->Get(ctx(), mod_str).ToLocalChecked();
            v8::Local<v8::Object> mod_obj;
            if (mod_val->IsObject()) mod_obj = mod_val.As<v8::Object>();
            else { mod_obj = v8::Object::New(g_isolate); imports->Set(ctx(), mod_str, mod_obj).Check(); }
            mod_obj->Set(ctx(), v8str(mi.name.c_str()), mem).Check();
            imported_memory = mem;
            wc_log("wasmcart: imported memory %s.%s: %llu pages%s%s%s\n",
                mi.module.c_str(), mi.name.c_str(), (unsigned long long)mi.initial,
                mi.has_max ? ", max " : "",
                mi.has_max ? std::to_string(mi.maximum).c_str() : "",
                mi.shared ? ", shared" : "");
        } else if (host->threaded) {
            wc_log("wasmcart: threaded cart must import a shared memory (link with --import-memory --shared-memory)\n");
            return -1;
        }

        if (host->threaded) {
            v8::Local<v8::Script> sc;
            v8::Local<v8::Value> factory;
            if (!v8::Script::Compile(ctx(), v8str(WC_THREADS_SPAWNER_JS)).ToLocal(&sc) ||
                !sc->Run(ctx()).ToLocal(&factory) || !factory->IsFunction()) {
                wc_log("wasmcart: failed to set up WASI threads\n");
                return -1;
            }
            v8::Local<v8::Value> f_args[] = {
                wasm_module, imported_memory, build_thread_cfg(host, wasc_path), v8str(WC_THREAD_WORKER_JS),
            };
            v8::Local<v8::Value> threads;
            if (!factory.As<v8::Function>()->Call(ctx(), ctx()->Global(), 4, f_args).ToLocal(&threads) ||
                !threads->IsObject()) {
                if (try_catch.HasCaught()) {
                    v8::String::Utf8Value err(g_isolate, try_catch.Exception());
                    wc_log("wasmcart: failed to set up WASI threads: %s\n", *err);
                }
                return -1;
            }
            auto spawn = threads.As<v8::Object>()->Get(ctx(), v8str("spawn")).ToLocalChecked().As<v8::Function>();
            state->threads.Reset(g_isolate, threads.As<v8::Object>());
            state->thread_spawn_fn.Reset(g_isolate, spawn);
            auto wasi_ns = v8::Object::New(g_isolate);
            wasi_ns->Set(ctx(), v8str("thread-spawn"), make_fn(v8_thread_spawn)).Check();
            imports->Set(ctx(), v8str("wasi"), wasi_ns).Check();
            wc_log("wasmcart: threaded cart (wasi.thread-spawn); threads run as node workers\n");
        }
    }

    // 4. Auto-stub missing imports
    // V8 doesn't have wasmtime's "define_unknown_imports_as_default_values" — we must provide every import.
    // Use WebAssembly.Module.imports(module) to find what's needed, stub anything missing.
    {
        auto wasm_module_ns = wasm_ns->Get(ctx(), v8str("Module")).ToLocalChecked().As<v8::Function>();
        auto imports_fn = wasm_module_ns->Get(ctx(), v8str("imports")).ToLocalChecked().As<v8::Function>();
        v8::Local<v8::Value> imp_args[] = { wasm_module };
        auto imp_arr = imports_fn->Call(ctx(), wasm_ns, 1, imp_args).ToLocalChecked().As<v8::Array>();

        for (uint32_t i = 0; i < imp_arr->Length(); i++) {
            auto entry = imp_arr->Get(ctx(), i).ToLocalChecked().As<v8::Object>();
            v8::String::Utf8Value mod(g_isolate, entry->Get(ctx(), v8str("module")).ToLocalChecked());
            v8::String::Utf8Value name(g_isolate, entry->Get(ctx(), v8str("name")).ToLocalChecked());
            v8::String::Utf8Value kind(g_isolate, entry->Get(ctx(), v8str("kind")).ToLocalChecked());

            // Ensure module namespace exists in imports
            auto mod_str = v8str(*mod);
            auto mod_val = imports->Get(ctx(), mod_str).ToLocalChecked();
            v8::Local<v8::Object> mod_obj;
            if (mod_val->IsObject()) {
                mod_obj = mod_val.As<v8::Object>();
            } else {
                mod_obj = v8::Object::New(g_isolate);
                imports->Set(ctx(), mod_str, mod_obj).Check();
            }

            // Check if already provided
            auto name_str = v8str(*name);
            auto existing = mod_obj->Get(ctx(), name_str).ToLocalChecked();
            if (!existing->IsUndefined()) continue;

            // Auto-stub based on kind.
            //
            // A stubbed FUNCTION returns 0, and for a query like
            // glGetActiveUniformBlockiv(GL_UNIFORM_BLOCK_DATA_SIZE) that zero
            // reads as a real answer: the engine sizes a uniform block from it,
            // uploads garbage, and every vertex collapses to a point with no GL
            // error and no failed draw call. The screen just stays empty. So
            // say which imports were stubbed - a silent stub is the difference
            // between a five-minute fix and a multi-day hunt.
            if (strcmp(*kind, "function") == 0) {
                wc_log("wasmcart: stubbed missing import %s.%s (returns 0)\n", *mod, *name);
                mod_obj->Set(ctx(), name_str, make_fn(v8_noop_return_0)).Check();
            } else if (strcmp(*kind, "global") == 0) {
                // Create a WebAssembly.Global with value 0
                auto global_ctor = wasm_ns->Get(ctx(), v8str("Global")).ToLocalChecked().As<v8::Function>();
                auto desc = v8::Object::New(g_isolate);
                desc->Set(ctx(), v8str("value"), v8str("i32")).Check();
                desc->Set(ctx(), v8str("mutable"), v8::Boolean::New(g_isolate, true)).Check();
                v8::Local<v8::Value> g_args[] = { desc, v8::Integer::New(g_isolate, 0) };
                auto wasm_global = global_ctor->NewInstance(ctx(), 2, g_args).ToLocalChecked();
                mod_obj->Set(ctx(), name_str, wasm_global).Check();
            } else if (strcmp(*kind, "table") == 0) {
                auto table_ctor = wasm_ns->Get(ctx(), v8str("Table")).ToLocalChecked().As<v8::Function>();
                auto desc = v8::Object::New(g_isolate);
                desc->Set(ctx(), v8str("element"), v8str("anyfunc")).Check();
                desc->Set(ctx(), v8str("initial"), v8::Integer::New(g_isolate, 0)).Check();
                v8::Local<v8::Value> t_args[] = { desc };
                auto wasm_table = table_ctor->NewInstance(ctx(), 1, t_args).ToLocalChecked();
                mod_obj->Set(ctx(), name_str, wasm_table).Check();
            } else if (strcmp(*kind, "memory") == 0) {
                auto mem_ctor = wasm_ns->Get(ctx(), v8str("Memory")).ToLocalChecked().As<v8::Function>();
                auto desc = v8::Object::New(g_isolate);
                desc->Set(ctx(), v8str("initial"), v8::Integer::New(g_isolate, 1)).Check();
                v8::Local<v8::Value> m_args[] = { desc };
                auto wasm_mem = mem_ctor->NewInstance(ctx(), 1, m_args).ToLocalChecked();
                mod_obj->Set(ctx(), name_str, wasm_mem).Check();
            }
        }
    }

    // 5. Instantiate: new WebAssembly.Instance(module, imports)
    v8::Local<v8::Value> instance_args[] = { wasm_module, imports };
    auto instance_result = wasm_instance_ctor->NewInstance(ctx(), 2, instance_args);
    if (instance_result.IsEmpty()) {
        if (try_catch.HasCaught()) {
            v8::String::Utf8Value err(g_isolate, try_catch.Exception());
            wc_log( "wasmcart: instantiate error: %s\n", *err);
        }
        return -1;
    }
    auto instance = instance_result.ToLocalChecked();
    state->instance.Reset(g_isolate, instance);

    // 5. Get exports
    auto exports = instance->Get(ctx(), v8str("exports")).ToLocalChecked().As<v8::Object>();
    state->exports_obj.Reset(g_isolate, exports);

    // Memory: the exported one, else the one we built for the import (a
    // threaded cart need not re-export the memory it imports).
    auto mem_val = exports->Get(ctx(), v8str("memory")).ToLocalChecked();
    if (mem_val->IsUndefined() && !imported_memory.IsEmpty()) mem_val = imported_memory;
    if (!mem_val->IsUndefined()) {
        state->memory_obj.Reset(g_isolate, mem_val.As<v8::Object>());
        refresh_memory(host);
        wc_log("wasmcart: initial WASM memory: %u bytes (%u MB)\n",
            host->memory_size, host->memory_size / (1024*1024));
    }

    // Functions
    auto get_fn = [&](const char* name) -> v8::Local<v8::Function> {
        auto val = exports->Get(ctx(), v8str(name)).ToLocalChecked();
        if (val->IsFunction()) return val.As<v8::Function>();
        return v8::Local<v8::Function>();
    };

    auto fn_get_info = get_fn("wc_get_info");
    auto fn_init = get_fn("wc_init");
    auto fn_render = get_fn("wc_render");
    auto fn_initialize = get_fn("_initialize");
    auto fn_malloc = get_fn("malloc");
    auto fn_set_seed = get_fn("wc_set_seed");

    if (fn_get_info.IsEmpty() || fn_render.IsEmpty()) {
        wc_log( "wasmcart: missing required exports (wc_get_info, wc_render)\n");
        return -1;
    }

    state->fn_wc_get_info.Reset(g_isolate, fn_get_info);
    state->fn_wc_render.Reset(g_isolate, fn_render);
    if (!fn_init.IsEmpty()) state->fn_wc_init.Reset(g_isolate, fn_init);
    if (!fn_initialize.IsEmpty()) state->fn_initialize.Reset(g_isolate, fn_initialize);
    if (!fn_malloc.IsEmpty()) state->fn_malloc.Reset(g_isolate, fn_malloc);
    if (!fn_set_seed.IsEmpty()) state->fn_wc_set_seed.Reset(g_isolate, fn_set_seed);

    // Store fn pointers for gl_imports to use
    host->fn_wc_get_info = &state->fn_wc_get_info;
    host->fn_wc_init = fn_init.IsEmpty() ? nullptr : &state->fn_wc_init;
    host->fn_wc_render = &state->fn_wc_render;

    // Pre-scan: detect GL usage from imports (needed before init for libretro HW render setup)
    // Check if any import is from "gl" module
    {
        auto wasm_module_ns = wasm_ns->Get(ctx(), v8str("Module")).ToLocalChecked().As<v8::Function>();
        auto imports_fn = wasm_module_ns->Get(ctx(), v8str("imports")).ToLocalChecked().As<v8::Function>();
        v8::Local<v8::Value> imp_args[] = { wasm_module };
        auto imp_arr = imports_fn->Call(ctx(), wasm_ns, 1, imp_args).ToLocalChecked().As<v8::Array>();
        for (uint32_t i = 0; i < imp_arr->Length(); i++) {
            auto entry = imp_arr->Get(ctx(), i).ToLocalChecked().As<v8::Object>();
            v8::String::Utf8Value mod(g_isolate, entry->Get(ctx(), v8str("module")).ToLocalChecked());
            v8::String::Utf8Value name(g_isolate, entry->Get(ctx(), v8str("name")).ToLocalChecked());
            v8::String::Utf8Value kind(g_isolate, entry->Get(ctx(), v8str("kind")).ToLocalChecked());
            // Pre-scan for GL imports (needed before wc_get_info for deferred init)
            // This is a fallback — gpu_api field in wc_info_t is authoritative after init
            if (strcmp(*mod, "gl") == 0 ||
                (strcmp(*mod, "env") == 0 && strcmp(*kind, "function") == 0 &&
                 (*name)[0] == 'g' && (*name)[1] == 'l' && (*name)[2] >= 'A' && (*name)[2] <= 'Z')) {
                host->uses_gl = true;
                break;
            }
        }
    }

    // 6-10. Init sequence (_initialize, wc_get_info, wc_init)
    // Can be deferred for GL carts that need the GL context first (libretro)
    if (opts && opts->defer_init) {
        host->init_deferred = true;
        host->info.width = (opts->preferred_width > 0) ? opts->preferred_width : 640;
        host->info.height = (opts->preferred_height > 0) ? opts->preferred_height : 480;
        // Store opts for write_host_info during finish_init
        if (opts) host->deferred_opts = *opts;
    } else {
        // Normal path: run full init now
        if (!fn_initialize.IsEmpty()) {
            wc_log( "wasmcart: calling _initialize\n");
            auto result = fn_initialize->Call(ctx(), ctx()->Global(), 0, nullptr);
            if (result.IsEmpty() && try_catch.HasCaught()) {
                v8::String::Utf8Value err(g_isolate, try_catch.Exception());
                wc_log( "wasmcart: _initialize error: %s\n", *err);
            }
            refresh_memory(host);
        }

        {
            auto result = fn_get_info->Call(ctx(), ctx()->Global(), 0, nullptr);
            if (!result.IsEmpty()) {
                refresh_memory(host);
                uint32_t info_ptr = result.ToLocalChecked()->Uint32Value(ctx()).FromJust();
                parse_cart_info(host, info_ptr);
            }
        }

        write_host_info(host, opts);

        if (opts && opts->save_data && host->info.save_ptr && opts->save_data_size > 0) {
            uint32_t copy_size = opts->save_data_size < host->info.save_size ?
                opts->save_data_size : host->info.save_size;
            memcpy(host->memory + host->info.save_ptr, opts->save_data, copy_size);
        }

        seed_cart_rng(host, opts);

        if (!fn_init.IsEmpty()) {
            auto result = fn_init->Call(ctx(), ctx()->Global(), 0, nullptr);
            if (result.IsEmpty() && try_catch.HasCaught()) {
                v8::String::Utf8Value err(g_isolate, try_catch.Exception());
                wc_log( "wasmcart: wc_init error: %s\n", *err);
                auto stack = try_catch.StackTrace(ctx());
                if (!stack.IsEmpty()) {
                    v8::String::Utf8Value st(g_isolate, stack.ToLocalChecked());
                    wc_log( "wasmcart: stack: %s\n", *st);
                }
            }
            refresh_memory(host);
        }

        // Re-read cart info
        {
            auto result = fn_get_info->Call(ctx(), ctx()->Global(), 0, nullptr);
            if (!result.IsEmpty()) {
                refresh_memory(host);
                parse_cart_info(host, result.ToLocalChecked()->Uint32Value(ctx()).FromJust());
            }
        }
    } // end else (non-deferred init)

    // A deferred cart has not run yet, so its version is not knowable here;
    // wc_host_finish_init() checks it once the cart has filled wc_info_t in.
    if (!host->init_deferred && check_abi_version(host) != 0) return -1;

    wc_log( "wasmcart: loaded %s (%ux%u, %s, ABI v%u)\n",
        host->manifest.name,
        host->info.width, host->info.height,
        host->uses_gl ? "GL" : "2D",
        host->info.version);

    return 0;
}

extern "C" int wc_host_load_memory(wc_host_t* host, const uint8_t* data, size_t len, const wc_host_options_t* opts) {
    int rc = wc_archive_open_memory(host, data, len);
    if (rc != 0) return rc;
    // TODO: shared init
    return -1;
}

// ─── Deferred init (for libretro GL carts — call after GL context ready) ──

extern "C" int wc_host_finish_init(wc_host_t* host) {
    if (!host || !host->init_deferred) return 0;

    v8::Locker locker(g_isolate);
    v8::Isolate::Scope isolate_scope(g_isolate);
    v8::HandleScope handle_scope(g_isolate);
    v8::Context::Scope context_scope(ctx());

    _current_host = host;
    auto state = (v8_host_state*)host->v8_state;
    v8::TryCatch try_catch(g_isolate);

    // Call _initialize
    if (!state->fn_initialize.IsEmpty()) {
        wc_log( "wasmcart: calling _initialize (deferred)\n");
        auto result = state->fn_initialize.Get(g_isolate)->Call(ctx(), ctx()->Global(), 0, nullptr);
        if (result.IsEmpty() && try_catch.HasCaught()) {
            v8::String::Utf8Value err(g_isolate, try_catch.Exception());
            wc_log( "wasmcart: _initialize error: %s\n", *err);
            return -1;
        }
        refresh_memory(host);
    }

    // Call wc_get_info
    {
        auto result = state->fn_wc_get_info.Get(g_isolate)->Call(ctx(), ctx()->Global(), 0, nullptr);
        if (!result.IsEmpty()) {
            refresh_memory(host);
            parse_cart_info(host, result.ToLocalChecked()->Uint32Value(ctx()).FromJust());
        }
    }

    // The cart has run now, so this is the first point on the deferred path
    // where its ABI version is knowable. Refuse before wc_init rather than
    // after, so an incompatible cart never gets to touch the structs.
    if (check_abi_version(host) != 0) return -1;

    // Write host info (preferred dimensions, etc.) before wc_init
    write_host_info(host, &host->deferred_opts);

    seed_cart_rng(host, &host->deferred_opts);

    // Call wc_init
    if (!state->fn_wc_init.IsEmpty()) {
        auto result = state->fn_wc_init.Get(g_isolate)->Call(ctx(), ctx()->Global(), 0, nullptr);
        if (result.IsEmpty() && try_catch.HasCaught()) {
            v8::String::Utf8Value err(g_isolate, try_catch.Exception());
            wc_log("wasmcart: wc_init error: %s\n", *err);
            auto stack = try_catch.StackTrace(ctx());
            if (!stack.IsEmpty()) {
                v8::String::Utf8Value st(g_isolate, stack.ToLocalChecked());
                wc_log("wasmcart: stack: %s\n", *st);
            }
            return -1;
        }
        refresh_memory(host);
    }

    // Re-read info
    {
        auto result = state->fn_wc_get_info.Get(g_isolate)->Call(ctx(), ctx()->Global(), 0, nullptr);
        if (!result.IsEmpty()) {
            refresh_memory(host);
            parse_cart_info(host, result.ToLocalChecked()->Uint32Value(ctx()).FromJust());
        }
    }

    host->init_deferred = false;

    wc_log( "wasmcart: deferred init complete (%ux%u, %s)\n",
        host->info.width, host->info.height,
        host->uses_gl ? "GL" : "2D");

    return 0;
}

// ─── Input (unchanged — just writes to host->memory) ──────────────────

extern "C" void wc_host_set_rumble_backend(wc_host_t* host,
                                           const wc_rumble_backend_t* backend) {
    if (!host) return;
    if (backend) {
        host->rumble = *backend;
    } else {
        /* Detach: zeroing has_rumble is what makes the imports no-ops. */
        memset(&host->rumble, 0, sizeof(host->rumble));
    }
}

extern "C" void wc_host_set_pads(wc_host_t* host, const wc_pad_t pads[WC_MAX_PADS]) {
    if (!host->memory || !host->info.input_ptr) return;
    memcpy(host->memory + host->info.input_ptr, pads, sizeof(wc_pad_t) * WC_MAX_PADS);
}

extern "C" void wc_host_set_keyboard(wc_host_t* host, const uint8_t keys[WC_KEYS_STATE_SIZE]) {
    if (!host->memory || !host->info.keys_ptr) return;
    memcpy(host->memory + host->info.keys_ptr, keys, WC_KEYS_STATE_SIZE);
}

extern "C" void wc_host_set_pointer(wc_host_t* host, int index, int16_t x, int16_t y, uint8_t buttons, uint8_t active) {
    if (!host->memory || !host->info.pointer_ptr || index < 0 || index >= WC_MAX_POINTERS) return;
    uint32_t offset = host->info.pointer_ptr + (index * 8);
    *(int16_t*)(host->memory + offset + 0) = x;
    *(int16_t*)(host->memory + offset + 2) = y;
    host->memory[offset + 4] = buttons;
    host->memory[offset + 5] = active;
    host->memory[offset + 6] = 0;
    host->memory[offset + 7] = 0;
}

// Wheel (ABI v3.1). ACCUMULATES: an embedder calls this once per event and
// the host hands the cart the frame's total, which is what makes a trackpad
// flick one delta instead of a number that depends on how long the frame
// took. Zeroed after each frame in run_frame, so the cart only ever reads.
extern "C" void wc_host_add_wheel(wc_host_t* host, int32_t dx, int32_t dy) {
    if (!host->memory || !host->info.wheel_ptr) return;
    host->wheel_dx += dx;
    host->wheel_dy += dy;
}

extern "C" void wc_host_set_time(wc_host_t* host, double time_ms, double delta_ms, uint32_t frame) {
    if (!host->memory || !host->info.time_ptr) return;
    uint32_t ptr = host->info.time_ptr;
    wc_write_f64(host->memory, ptr + WC_TIME_TIME_MS, time_ms);
    wc_write_f64(host->memory, ptr + WC_TIME_DELTA_MS, delta_ms);
    wc_write_u32(host->memory, ptr + WC_TIME_FRAME, frame);
}

// ─── Run frame ─────────────────────────────────────────────────────────

static v8::Locker* g_persistent_locker = nullptr;
static v8::Isolate::Scope* g_persistent_isolate_scope = nullptr;

extern "C" void wc_host_enter_v8(void) {
    g_persistent_locker = new v8::Locker(g_isolate);
    g_persistent_isolate_scope = new v8::Isolate::Scope(g_isolate);
}

extern "C" void wc_host_exit_v8(void) {
    delete g_persistent_isolate_scope; g_persistent_isolate_scope = nullptr;
    delete g_persistent_locker; g_persistent_locker = nullptr;
}

// ─── Text input delivery ─────────────────────────────────────────────────

/* Copy bytes into the cart's own heap so an export can be handed a pointer.
 * Uses the cart's malloc: writing into spare linear memory is how the JS host
 * once corrupted small carts, and there is no safe address to guess at.
 * Deliberately does not free -- a cart's malloc may have no matching free
 * exported, and leaking a few bytes per message beats calling one that is not
 * there. Carts that care copy out and reuse a static buffer. */
static bool stage_bytes(wc_host_t* host, const uint8_t* data, uint32_t len,
                        uint32_t* out_ptr) {
    if (!len) return false;
    auto state = (v8_host_state*)host->v8_state;

    /* Preferred: the cart's own allocator. */
    if (!state->fn_malloc.IsEmpty()) {
        v8::Local<v8::Value> arg = v8::Integer::NewFromUnsigned(g_isolate, len);
        v8::Local<v8::Value> ret;
        if (state->fn_malloc.Get(g_isolate)
                ->Call(ctx(), ctx()->Global(), 1, &arg).ToLocal(&ret)) {
            uint32_t ptr = ret->Uint32Value(ctx()).FromMaybe(0);
            if (ptr) {
                refresh_memory(host);
                if (host->memory && (uint64_t)ptr + len <= host->memory_size) {
                    memcpy(host->memory + ptr, data, len);
                    *out_ptr = ptr;
                    return true;
                }
            }
        }
    }

    /* No allocator -- plenty of hand-written carts export none. Grow a scratch
     * page onto the END of linear memory instead. It has to be grown rather
     * than carved out of existing memory: reusing the tail is how the JS host
     * once silently overwrote a small cart's statics. */
    if (host->scratch_base == 0) {
        auto mem = state->memory_obj.Get(g_isolate);
        v8::Local<v8::Value> grow_v;
        if (!mem->Get(ctx(), v8str("grow")).ToLocal(&grow_v) || !grow_v->IsFunction())
            return false;
        v8::Local<v8::Value> pages = v8::Integer::New(g_isolate, 1);
        v8::Local<v8::Value> prev;
        v8::TryCatch tc(g_isolate);
        if (!grow_v.As<v8::Function>()->Call(ctx(), mem, 1, &pages).ToLocal(&prev)) {
            /* Cart pinned its maximum. Dropping the message is recoverable;
             * writing somewhere unproven is not. */
            if (!host->scratch_warned) {
                host->scratch_warned = true;
                wc_log("wasmcart: cannot stage a %u-byte payload -- the cart "
                       "exports no malloc and its memory cannot grow. Messages "
                       "will be dropped.\n", len);
            }
            return false;
        }
        host->scratch_base = prev->Uint32Value(ctx()).FromMaybe(0) * 65536u;
        refresh_memory(host);
    }
    if (len > 65536u) return false;
    if (!host->memory || (uint64_t)host->scratch_base + len > host->memory_size)
        return false;
    memcpy(host->memory + host->scratch_base, data, len);
    *out_ptr = host->scratch_base;
    return true;
}

// ─── Peer event delivery ─────────────────────────────────────────────────

/* Move events the JS side queued for dialled sockets into the C peer records.
 * Done as a separate step so the cart is only ever entered from deliver_peers()
 * at a known point in the frame. */
static void drain_js_peer_events(wc_host_t* host) {
    if (host->peer_count == 0) return;
    v8::Local<v8::Value> qv;
    if (!ctx()->Global()->Get(ctx(), v8str("__wc_peerq")).ToLocal(&qv) || !qv->IsObject())
        return;
    v8::Local<v8::Object> q = qv.As<v8::Object>();

    for (uint32_t i = 0; i < host->peer_count; i++) {
        wc_peer_t* p = &host->peers[i];
        if (p->host_supplied) continue;
        v8::Local<v8::Value> av;
        if (!q->Get(ctx(), v8::Integer::New(g_isolate, p->id)).ToLocal(&av)
            || !av->IsArray()) continue;
        v8::Local<v8::Array> arr = av.As<v8::Array>();
        uint32_t n = arr->Length();
        for (uint32_t k = 0; k < n; k++) {
            v8::Local<v8::Value> ev;
            if (!arr->Get(ctx(), k).ToLocal(&ev) || !ev->IsObject()) continue;
            v8::Local<v8::Object> o = ev.As<v8::Object>();
            v8::Local<v8::Value> tv;
            if (!o->Get(ctx(), v8str("t")).ToLocal(&tv)) continue;
            int t = tv->Int32Value(ctx()).FromMaybe(-1);
            if (t == 1) {
                v8::Local<v8::Value> dv;
                if (o->Get(ctx(), v8str("d")).ToLocal(&dv) && dv->IsUint8Array()) {
                    v8::Local<v8::Uint8Array> u8 = dv.As<v8::Uint8Array>();
                    size_t len = u8->ByteLength();
                    uint8_t* tmp = (uint8_t*)malloc(len ? len : 1);
                    if (tmp) {
                        u8->CopyContents(tmp, len);
                        peer_queue(p, WC_PEER_EV_MESSAGE, tmp, (uint32_t)len);
                        free(tmp);
                    }
                }
            } else if (t >= 0) {
                peer_queue(p, t, NULL, 0);
            }
        }
        if (n) {
            /* Emptying by length keeps the same array the socket closes over. */
            v8::Local<v8::Value> setlen;
            if (arr->Set(ctx(), v8str("length"), v8::Integer::New(g_isolate, 0)).IsNothing())
                (void)setlen;
        }
    }
}

/* Hand queued events to the cart's wc_peer_on_* exports. */
static void deliver_peers(wc_host_t* host) {
    if (host->peer_count == 0) return;
    auto state = (v8_host_state*)host->v8_state;
    auto exports = state->exports_obj.Get(g_isolate);

    auto get_fn = [&](const char* name) -> v8::Local<v8::Function> {
        v8::Local<v8::Value> v;
        if (exports->Get(ctx(), v8str(name)).ToLocal(&v) && v->IsFunction())
            return v.As<v8::Function>();
        return v8::Local<v8::Function>();
    };
    auto on_connect    = get_fn("wc_peer_on_connect");
    auto on_message    = get_fn("wc_peer_on_message");
    auto on_disconnect = get_fn("wc_peer_on_disconnect");
    auto on_error      = get_fn("wc_peer_on_error");

    for (uint32_t i = 0; i < host->peer_count; i++) {
        wc_peer_t* p = &host->peers[i];
        for (uint32_t k = 0; k < p->events_len; k++) {
            wc_peer_event_t* ev = &p->events[k];
            v8::TryCatch tc(g_isolate);
            if (ev->type == WC_PEER_EV_CONNECT) {
                p->state = WC_PEER_OPEN;
                if (!on_connect.IsEmpty()) {
                    /* name is passed through the cart's own memory, so it needs
                     * somewhere to land. Reuse the scratch path used elsewhere. */
                    v8::Local<v8::Value> argv[3] = {
                        v8::Integer::New(g_isolate, p->id),
                        v8::Integer::NewFromUnsigned(g_isolate, 0),
                        v8::Integer::NewFromUnsigned(g_isolate, 0),
                    };
                    (void)on_connect->Call(ctx(), ctx()->Global(), 3, argv);
                }
            } else if (ev->type == WC_PEER_EV_MESSAGE) {
                if (!on_message.IsEmpty() && ev->data && ev->len) {
                    uint32_t ptr = 0;
                    if (stage_bytes(host, ev->data, ev->len, &ptr)) {
                        v8::Local<v8::Value> argv[3] = {
                            v8::Integer::New(g_isolate, p->id),
                            v8::Integer::NewFromUnsigned(g_isolate, ptr),
                            v8::Integer::NewFromUnsigned(g_isolate, ev->len),
                        };
                        (void)on_message->Call(ctx(), ctx()->Global(), 3, argv);
                    }
                }
            } else if (ev->type == WC_PEER_EV_DISCONNECT) {
                p->state = WC_PEER_CLOSED;
                if (!on_disconnect.IsEmpty()) {
                    v8::Local<v8::Value> a = v8::Integer::New(g_isolate, p->id);
                    (void)on_disconnect->Call(ctx(), ctx()->Global(), 1, &a);
                }
            } else if (ev->type == WC_PEER_EV_ERROR) {
                if (!on_error.IsEmpty()) {
                    v8::Local<v8::Value> a = v8::Integer::New(g_isolate, p->id);
                    (void)on_error->Call(ctx(), ctx()->Global(), 1, &a);
                }
            }
            if (tc.HasCaught()) {
                v8::String::Utf8Value e(g_isolate, tc.Exception());
                wc_log("wasmcart: cart's wc_peer_on_* threw: %s\n", *e);
            }
            free(ev->data);
        }
        p->events_len = 0;
    }
}

extern "C" int32_t wc_host_add_peer(wc_host_t* host, const char* name,
                                   wc_peer_send_fn send, void* user,
                                   uint32_t transport) {
    if (!host) return -1;
    wc_peer_t* p = peer_alloc(host);
    if (!p) return -1;
    p->host_supplied = true;
    p->send = send;
    p->user = user;
    p->transport = transport;
    p->state = WC_PEER_OPEN;   /* the host would not register a dead peer */
    snprintf(p->name, sizeof p->name, "%s", name ? name : "");
    peer_queue(p, WC_PEER_EV_CONNECT, NULL, 0);
    return p->id;
}

extern "C" void wc_host_peer_recv(wc_host_t* host, int32_t peer_id,
                                  const uint8_t* data, uint32_t len) {
    if (!host) return;
    wc_peer_t* p = peer_find(host, peer_id);
    if (p) peer_queue(p, WC_PEER_EV_MESSAGE, data, len);
}

extern "C" void wc_host_remove_peer(wc_host_t* host, int32_t peer_id) {
    if (!host) return;
    wc_peer_t* p = peer_find(host, peer_id);
    if (p) { p->state = WC_PEER_CLOSED; peer_queue(p, WC_PEER_EV_DISCONNECT, NULL, 0); }
}

extern "C" void wc_host_push_text(wc_host_t* host, const char* utf8, uint32_t len) {
    // Ignored unless the cart asked for text. That is what lets an embedder
    // forward every platform text event unconditionally without a cart that
    // never wanted text ever seeing one.
    if (!host || !host->text_active || !utf8 || len == 0) return;

    size_t need = host->text_queue_len + len + 1; // +1 for the separator
    if (need > host->text_queue_cap) {
        size_t cap = host->text_queue_cap ? host->text_queue_cap * 2 : 256;
        while (cap < need) cap *= 2;
        char* grown = (char*)realloc(host->text_queue, cap);
        if (!grown) return;  // out of memory: drop the keystroke, keep the cart
        host->text_queue = grown;
        host->text_queue_cap = cap;
    }
    memcpy(host->text_queue + host->text_queue_len, utf8, len);
    host->text_queue_len += len;
    host->text_queue[host->text_queue_len++] = '\0'; // separator
}

extern "C" int wc_host_text_input_active(wc_host_t* host) {
    return (host && host->text_active) ? 1 : 0;
}

// Hand each queued string to the cart's wc_on_text, staged through the cart's
// own malloc. Called with a HandleScope already open.
static void deliver_text(wc_host_t* host) {
    if (host->text_queue_len == 0) return;

    auto state = (v8_host_state*)host->v8_state;
    auto exports = state->exports_obj.Get(g_isolate);
    auto key = v8str("wc_on_text");
    v8::Local<v8::Value> val;
    if (!exports->Get(ctx(), key).ToLocal(&val) || !val->IsFunction()) {
        // A cart may enable text input without exporting the handler (it might
        // only want a mobile keyboard raised). Drop rather than grow forever.
        host->text_queue_len = 0;
        return;
    }
    auto fn = val.As<v8::Function>();

    if (state->fn_malloc.IsEmpty()) {
        // No allocator: there is nowhere safe to stage the bytes. Writing into
        // spare linear memory is how the JS host corrupted small carts, so drop
        // instead and say why, once.
        static bool warned = false;
        if (!warned) {
            warned = true;
            wc_log("wasmcart: cart exports wc_on_text but no malloc; text cannot "
                   "be staged and will be dropped.\n");
        }
        host->text_queue_len = 0;
        return;
    }
    auto malloc_fn = state->fn_malloc.Get(g_isolate);

    size_t pos = 0;
    while (pos < host->text_queue_len) {
        const char* str = host->text_queue + pos;
        size_t slen = strlen(str);
        pos += slen + 1;
        if (slen == 0) continue;

        v8::Local<v8::Value> mlen = v8::Integer::NewFromUnsigned(g_isolate, (uint32_t)slen);
        v8::Local<v8::Value> mret;
        if (!malloc_fn->Call(ctx(), ctx()->Global(), 1, &mlen).ToLocal(&mret)) continue;
        uint32_t ptr = mret->Uint32Value(ctx()).FromMaybe(0);
        if (ptr == 0) continue;

        refresh_memory(host);
        if (!host->memory || ptr + slen > host->memory_size) continue;
        memcpy(host->memory + ptr, str, slen);

        v8::Local<v8::Value> argv[2] = {
            v8::Integer::NewFromUnsigned(g_isolate, ptr),
            v8::Integer::NewFromUnsigned(g_isolate, (uint32_t)slen),
        };
        v8::TryCatch tc(g_isolate);
        v8::MaybeLocal<v8::Value> unused = fn->Call(ctx(), ctx()->Global(), 2, argv);
        (void)unused;
        if (tc.HasCaught()) {
            v8::String::Utf8Value err(g_isolate, tc.Exception());
            wc_log("wasmcart: cart's wc_on_text() threw: %s\n", *err);
        }
        // Deliberately not freeing: the cart's malloc may have no matching free
        // exported, and leaking a few bytes per keystroke beats calling a
        // free() that does not exist. Carts that care can reuse a static buffer.
    }
    host->text_queue_len = 0;
}

// Drive node's event loop for one slice.
//
// Three things have to happen, and missing any one of them breaks a different
// subset of node in ways that look nothing like each other:
//
//   node::CallbackScope  drains the process.nextTick queue on scope exit.
//     Without it nextTick NEVER runs. That is the one that cost the most to
//     find: net.Socket.connect() defers its dial through nextTick, so a socket
//     sat in `connecting` forever, no error, no close, and strace showed the
//     process never made a single network syscall. Timers, setImmediate,
//     microtasks, fs callbacks and even dns.lookup all worked, and the raw
//     tcp_wrap binding connected fine -- so everything pointed away from the
//     actual cause.
//   uv_run               advances libuv: timers, sockets, threadpool results.
//   PerformMicrotaskCheckpoint  settles promises (undici is promise-heavy).
//
// Called every frame, so a cart's networking progresses at frame cadence.
static void pump_node(wc_host_t* host) {
    (void)host;
    {
        node::CallbackScope scope(g_env, v8::Object::New(g_isolate), {0, 0});
    }
    uv_run(g_setup->event_loop(), UV_RUN_NOWAIT);
    g_isolate->PerformMicrotaskCheckpoint();
}

/* Test hook: run `code`, then pump until globalThis[flag] is true.
 * Exported for test/net_test.c; not part of the public header. */
/* Test hook: call a cart export by name with up to 3 uint32 args, returning its
 * int32 result. Exported for the peer test; not part of the public header. */
extern "C" int32_t wc_test_call_export(wc_host_t* host, const char* name,
                                       uint32_t a, uint32_t b, uint32_t c, int argc) {
    if (!host) return -1;
    v8::HandleScope hs(g_isolate);
    v8::Context::Scope cs(ctx());
    auto state = (v8_host_state*)host->v8_state;
    auto exports = state->exports_obj.Get(g_isolate);
    v8::Local<v8::Value> fv;
    if (!exports->Get(ctx(), v8str(name)).ToLocal(&fv) || !fv->IsFunction()) return -1;
    v8::Local<v8::Value> argv[3] = {
        v8::Integer::NewFromUnsigned(g_isolate, a),
        v8::Integer::NewFromUnsigned(g_isolate, b),
        v8::Integer::NewFromUnsigned(g_isolate, c),
    };
    v8::TryCatch tc(g_isolate);
    node::CallbackScope scope(g_env, v8::Object::New(g_isolate), {0, 0});
    v8::Local<v8::Value> r;
    if (!fv.As<v8::Function>()->Call(ctx(), ctx()->Global(), argc, argv).ToLocal(&r)) {
        if (tc.HasCaught()) { v8::String::Utf8Value e(g_isolate, tc.Exception());
            wc_log("test export %s threw: %s\n", name, *e); }
        return -1;
    }
    return r->Int32Value(ctx()).FromMaybe(-1);
}

/* Write bytes straight into cart memory at an absolute offset (test only). */
extern "C" int wc_test_poke(wc_host_t* host, uint32_t off, const void* data, uint32_t len) {
    if (!host || !host->memory) return -1;
    if ((uint64_t)off + len > host->memory_size) return -1;
    memcpy(host->memory + off, data, len);
    return 0;
}

extern "C" int wc_test_eval_flag(const char* code, const char* flag, int iters) {
    v8::HandleScope hs(g_isolate);
    v8::Context::Scope cs(ctx());
    v8::Local<v8::Script> sc;
    if (!v8::Script::Compile(ctx(), v8str(code)).ToLocal(&sc)) return -1;
    {
        // Deliberately NOT a node::CallbackScope. One here would drain the
        // nextTick queue itself, which is enough to complete a fast local
        // connection -- so the test would pass even with the scope removed
        // from pump_node(), i.e. it would not test the fix at all. Every
        // nextTick drain has to come from the pump.
        v8::TryCatch tc(g_isolate);
        if (sc->Run(ctx()).IsEmpty()) {
            if (tc.HasCaught()) {
                v8::String::Utf8Value e(g_isolate, tc.Exception());
                wc_log("test eval threw: %s\n", *e);
            }
            return -2;
        }
    }
    for (int i = 0; i < iters; i++) {
        pump_node(NULL);
        v8::Local<v8::Value> v;
        if (ctx()->Global()->Get(ctx(), v8str(flag)).ToLocal(&v) && v->IsTrue()) return 1;
        uv_sleep(10);  // uv, not nanosleep: this builds on Windows too
    }
    return 0;
}

extern "C" void wc_host_pump(wc_host_t* host) {
    if (!host) return;
    v8::HandleScope hs(g_isolate);
    v8::Context::Scope cs(ctx());
    pump_node(host);
}

// Write the frame's accumulated wheel delta where the cart reads it. Separate
// from wc_host_add_wheel so events can arrive at any time and the cart still
// sees a single coherent value for the frame.
static void deliver_wheel(wc_host_t* host) {
    if (!host->memory || !host->info.wheel_ptr) return;
    wc_write_u32(host->memory, host->info.wheel_ptr + WC_WHEEL_DX, (uint32_t) host->wheel_dx);
    wc_write_u32(host->memory, host->info.wheel_ptr + WC_WHEEL_DY, (uint32_t) host->wheel_dy);
}

extern "C" void wc_host_run_frame(wc_host_t* host) {
    if (!host->fn_wc_render || host->trapped) return;

    // Locker + Isolate::Scope held persistently. Only HandleScope + Context::Scope per frame.
    v8::HandleScope handle_scope(g_isolate);
    v8::Context::Scope context_scope(ctx());

    auto state = (v8_host_state*)host->v8_state;
    v8::TryCatch try_catch(g_isolate);

    pump_node(host);          // let node's async work progress before the frame
    drain_js_peer_events(host); // move socket events into the peer records
    deliver_peers(host);        // then into the cart, at a known point
    deliver_text(host);         // before render, like every other input
    deliver_wheel(host);        // frame total in, zeroed again after render

    auto result = state->fn_wc_render.Get(g_isolate)->Call(
        ctx(), ctx()->Global(), 0, nullptr);

    if (result.IsEmpty() && try_catch.HasCaught()) {
        v8::String::Utf8Value err(g_isolate, try_catch.Exception());
        wc_log( "wasmcart: wc_render trapped: %s\n", *err);
        host->trapped = true;
        return;
    }

    refresh_memory(host);

    // Clear AFTER the frame, not before: the cart has now read the total, and
    // leaving it set would scroll forever off a single flick.
    if (host->memory && host->info.wheel_ptr) {
        wc_write_u32(host->memory, host->info.wheel_ptr + WC_WHEEL_DX, 0);
        wc_write_u32(host->memory, host->info.wheel_ptr + WC_WHEEL_DY, 0);
    }
    host->wheel_dx = 0;
    host->wheel_dy = 0;
}

// ─── Readback (unchanged — pure C, reads host->memory) ────────────────

extern "C" const uint8_t* wc_host_get_framebuffer(wc_host_t* host, uint32_t* width, uint32_t* height) {
    if (!host->memory || !host->info.fb_ptr) {
        *width = 0; *height = 0;
        return NULL;
    }
    *width = host->info.width;
    *height = host->info.height;
    return host->memory + host->info.fb_ptr;
}

static uint8_t* _audio_copy_buf = NULL;
static uint32_t _audio_copy_cap = 0;

extern "C" const void* wc_host_get_audio(wc_host_t* host, uint32_t* num_frames, bool* is_f32) {
    if (!host->memory || !host->info.audio_ptr || !host->info.audio_cap) {
        *num_frames = 0; *is_f32 = false;
        return NULL;
    }

    *is_f32 = (host->info.flags & WC_FLAG_AUDIO_F32) != 0;

    uint32_t write_cursor = wc_read_u32(host->memory, host->info.audio_write_ptr);
    uint32_t read_cursor = host->audio_read_cursor;
    uint32_t cap = host->info.audio_cap;

    if (write_cursor == read_cursor) { *num_frames = 0; return NULL; }

    uint32_t available;
    if (write_cursor >= read_cursor)
        available = write_cursor - read_cursor;
    else
        available = cap - read_cursor + write_cursor;

    if (available == 0) { *num_frames = 0; return NULL; }

    uint32_t needed = available * 2;

    if (*is_f32) {
        uint32_t needed_bytes = needed * sizeof(float);
        if (_audio_copy_cap < needed_bytes) {
            _audio_copy_buf = (uint8_t*)realloc(_audio_copy_buf, needed_bytes);
            _audio_copy_cap = needed_bytes;
        }
        float* out = (float*)_audio_copy_buf;
        const float* ring_f32 = (const float*)(host->memory + host->info.audio_ptr);
        for (uint32_t i = 0; i < available; i++) {
            uint32_t ring_idx = ((read_cursor + i) % cap) * 2;
            out[i * 2]     = ring_f32[ring_idx];
            out[i * 2 + 1] = ring_f32[ring_idx + 1];
        }
    } else {
        uint32_t needed_bytes = needed * sizeof(int16_t);
        if (_audio_copy_cap < needed_bytes) {
            _audio_copy_buf = (uint8_t*)realloc(_audio_copy_buf, needed_bytes);
            _audio_copy_cap = needed_bytes;
        }
        int16_t* out = (int16_t*)_audio_copy_buf;
        const int16_t* ring_i16 = (const int16_t*)(host->memory + host->info.audio_ptr);
        for (uint32_t i = 0; i < available; i++) {
            uint32_t ring_idx = ((read_cursor + i) % cap) * 2;
            out[i * 2]     = ring_i16[ring_idx];
            out[i * 2 + 1] = ring_i16[ring_idx + 1];
        }
    }

    host->audio_read_cursor = write_cursor;
    *num_frames = available;
    return _audio_copy_buf;
}

extern "C" uint8_t* wc_host_get_save_data(wc_host_t* host, uint32_t* size) {
    if (!host->memory || !host->info.save_ptr || !host->info.save_size) {
        *size = 0; return NULL;
    }
    *size = host->info.save_size;
    return host->memory + host->info.save_ptr;
}

// ─── GL / Info (unchanged) ──────────────────────────────────────────────

extern "C" bool wc_host_uses_gl(wc_host_t* host) { return host->uses_gl; }
extern "C" bool wc_host_has_trapped(wc_host_t* host) { return host->trapped; }
extern "C" void wc_host_set_gl_loader(wc_host_t* host, wc_gl_get_proc_fn loader) { host->gl_loader = loader; }
extern "C" const wc_cart_info_t* wc_host_get_cart_info(wc_host_t* host) { return &host->info; }
extern "C" const wc_manifest_t* wc_host_get_manifest(wc_host_t* host) { return &host->manifest; }
extern "C" void* wc_host_get_memory(wc_host_t* host, uint32_t* size) {
    if (size) *size = host->memory_size;
    return host->memory;
}
