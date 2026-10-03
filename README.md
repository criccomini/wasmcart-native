# wasmcart-native

Standalone [wasmcart](https://github.com/wasmcart/wasmcart) player. Runs `.wasc` game
carts with [V8](https://v8.dev) for instant WASM startup (via
[libnode](https://github.com/wasmcart/build-libnode)), [SDL2](https://www.libsdl.org)
for display/input/audio, and EGL/GLES3 for GPU carts.

One binary. No runtime dependencies beyond system GL. Drop it on any Linux, macOS, or Windows machine and play.

## Usage

```
wasmcart-run game.wasc [options]

Options:
  --res WxH       Window resolution (e.g. --res 1920x1080)
  --scale N       Integer scale factor for 2D carts (default: 1)
  --fullscreen    Start in fullscreen mode
  --fps           Show FPS counter
  --uncapped      Disable vsync and frame cap
```

## What It Runs

Every `.wasc` cart that runs in the browser or Node.js also runs here. Same WASM, same GL calls, same audio, same input.

| Cart type | Rendering | Examples |
|-----------|-----------|----------|
| 2D framebuffer | SDL2 accelerated renderer, letterboxed | Snake, Doom, ccleste, pygame carts |
| GL (GLES3) | EGL + direct GL, FBO redirect, letterboxed | OpenArena, [GZDoom](https://zdoom.org), [Neverball](https://neverball.org), ETR |
| [Godot](https://godotengine.org) 4.x | GL + GLES3 Compatibility renderer | Warlords, RoboBlast, Kenney Platformer |

## Performance

V8's Liftoff baseline compiler starts WASM instantly — no compilation delay, even for 52MB Godot carts (356ms load time).

| Cart | FPS (uncapped, 1080p) |
|------|---------------|
| Snake (320x240 2D) | 4,900 |
| [Three.js](https://threejs.org) (WebGL2) | 2,470 |
| OpenArena ([ioquake3](https://github.com/ioquake/ioq3) GL) | 830 |
| Adventure AI ([Skia](https://skia.org) Ganesh GPU) | 716 |
| Warlords (Godot GL) | 430 |

## Build

### Prerequisites

- CMake 3.16+
- C/C++ compiler (gcc/clang)
- [SDL2](https://www.libsdl.org) dev headers (`sudo apt install libsdl2-dev`)
- EGL + GLES dev headers (`sudo apt install libegl-dev libgles-dev`)
- Wayland client + wayland-egl dev headers (`sudo apt install libwayland-dev`),
  for GL carts on native Wayland. Optional: without them that path compiles
  out and GL carts on a Wayland session need `SDL_VIDEODRIVER=x11`.

### Build from source

```bash
# Download pre-built libnode
mkdir -p deps/libnode
curl -sL https://github.com/wasmcart/build-libnode/releases/download/v26.3.0-jsg9/libnode-linux-x86_64.tar.gz \
  | tar xz -C deps/libnode

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run
./wasmcart-run /path/to/game.wasc
```

### Pre-built binaries

Download from [Releases](https://github.com/wasmcart/wasmcart-native/releases) —
Linux (x86_64/aarch64), macOS (x86_64/aarch64) and Windows x86_64.

On Linux and macOS, if the binary arrives without its execute bit:

```bash
chmod +x wasmcart-run
```

The release `.tar.gz` preserves the mode. What does not is the **ZIP that
GitHub Actions wraps around build artifacts** — downloading from the *Actions*
run page (rather than the Releases page) gives you a zip of the tarball, and
that layer drops the Unix mode. Copying between filesystems can do the same.

Worth knowing because the failure is misleading: the file is a perfectly valid
ELF, so `Permission denied` reads like a missing dependency or a broken build
rather than a file mode. `ls -l wasmcart-run` settles it in one command —
`-rw-rw-r--` means chmod, not a rebuild.

Building from source is unaffected; the linker sets the bit.

The ZIP reader is [miniz](https://github.com/richgel999/miniz) and manifest parsing
uses [cJSON](https://github.com/DaveGamble/cJSON), both vendored under `deps/`.

This same host core is shared via git submodule with
[wasmcart-libretro](https://github.com/wasmcart/wasmcart-libretro) and with the
standalone Android player, so the GL bridge, asset loader, and ABI handling
stay in lockstep across all three embeddings.

## Architecture

```
wasmcart-run (75MB, statically linked)
│
├── V8 engine (libnode.a)    — WASM compile + execute (instant via Liftoff JIT)
├── SDL2 (libSDL2.a)         — Window, gamepad, keyboard, audio
├── EGL + GLES3              — GL context for GPU carts
│
├── cart_host.cpp             — wasmcart ABI: load .wasc, manage V8, run frames
├── gl_imports.cpp            — ~209 GL functions registered as V8 callbacks
├── asset_loader.c            — .wasc ZIP reading (miniz) + manifest parsing (cJSON)
├── egl_context.c             — EGL context, boot surface (pbuffer; a hidden window on Wayland) + window surface
└── main.c                    — SDL2 event loop, input, display, audio queue
```

## Input

- **Gamepad**: SDL2 GameController API with 2,182 built-in controller mappings
- **Keyboard**: Arrow keys / WASD / Z / X mapped to gamepad for pad-only carts
- **4 players**: Up to 4 controllers supported simultaneously
- **Hot-plug**: Controllers can be connected/disconnected during play
- **Rumble**: `wc_pad_rumble` routed to SDL2's haptics, with the ABI's clamping
  applied once in the host library so every embedder behaves the same
- **Text input**: `wc_on_text` fed from `SDL_TEXTINPUT`, so a cart receives
  characters the OS already composed -- layout, shift, dead keys and IME -- and
  never has to reimplement a keyboard layout. `SDL_StartTextInput` is mirrored
  from the cart's own state, which is also the on-screen keyboard signal on
  platforms that have one

While a cart is taking text, the keyboard STOPS acting as a virtual gamepad --
otherwise typing a name also plays the game ("w" walks the player, Q/E fire the
shoulders, Return presses Start). Real gamepads keep working throughout, so a
cart can drive a d-pad character picker while a field is open.

The keyboard-as-gamepad fallback is not conditional on whether a controller is
plugged in. Making it so would mean unplugging a pad mid-game silently changed
what the keys did.

## RNG Seeding

A normal load seeds the cart's `wc_set_seed` export (if it has one) with fresh
entropy, so every boot deals a different shuffle. Embedders pin it for
deterministic replay via `wc_host_options_t`:

```c
wc_host_options_t opts = { .rng_seed = 1234, .rng_seed_set = true };
```

This mirrors the JS hosts' `deterministic:{seed}` exactly: unpinned differs
every run, pinned reproduces bit-for-bit. `test/seed_test.c` asserts both
directions against the `detrng` fixture.

## Resolution

The host passes preferred resolution to the cart via `--res`. The cart decides its actual rendering resolution. The host scales the output to fit the window, preserving aspect ratio with letterboxing. Without `--res`, the window matches the cart's native resolution.

## Networking

Carts reach the network through the `wc_peer_*` family, gated two ways: the cart
sets `WC_FLAG_NET_PEER` to ask, and the packager grants specific hosts in the
manifest. Neither alone is enough, and with no grant the host fails closed.

Two ways a peer reaches a cart, with deliberately different security:

| | Who dials | Grant needed |
|---|---|---|
| `wc_peer_open()` | the **cart** | `WC_FLAG_NET_PEER` **and** a manifest `net.domains` entry for that host |
| `wc_host_add_peer()` | the **host** | none -- the host already chose it |

The asymmetry is the point. Dialling out is allowlisted because the cart names a
destination the packager may not have anticipated. A peer the host established
needs no cart-side grant; requiring an embedder to write a manifest key
permitting its own action would be ceremony.

Dial-out is handled entirely inside the host using node's own WebSocket -- the
same implementation the Node host uses, so a cart that talks to a server in the
browser talks to it here without change, and an embedder does nothing. For a
host-supplied peer the embedder provides a send callback, feeds inbound bytes
with `wc_host_peer_recv()`, and reports a drop with `wc_host_remove_peer()`.

Payloads are staged into the cart's own `malloc` where it exports one, and
otherwise into a page grown onto the END of linear memory. Never into spare
existing memory: that is how the JS host once silently overwrote a small cart's
statics.

Async work (connections, messages, timers) advances once per frame from
`wc_host_run_frame()`. An embedder driving async work outside a frame loop --
waiting for a connection before starting the cart, say -- can call
`wc_host_pump()` directly.

> Embedding note: node's event loop needs three things pumped together, and
> missing any one breaks a different part of node in ways that look unrelated.
> `process.nextTick` is drained by `node::CallbackScope`, NOT by `uv_run`, and
> `net.Socket.connect()` defers its dial through nextTick -- so without it
> sockets sit in `connecting` forever with no error and no syscall ever issued,
> while timers, `setImmediate`, `fs` and even `dns.lookup` all keep working.
> `test/net_test.c` pins this.

## Rendering Modes

Determined by the cart's `gpu_api` field:

| gpu_api | Mode | Display path |
|---------|------|-------------|
| 0 | 2D framebuffer | SDL2 accelerated renderer + letterboxing |
| 1 | WebGL2 / GLES3 | EGL window surface + FBO redirect + letterboxing |

## Development Notes

### Tests

Standalone C programs against the built library, not a framework. Each takes a
cart and asserts on what the cart observed, so a passing run means the ABI
worked end to end rather than that a mock was called.

```bash
# build the library first, then:
gcc -O0 -o net_test  test/net_test.c  -Iinclude -Isrc \
    build/libwasmcart.a deps/libnode/libnode.a -lstdc++ -lm -lpthread -ldl

node ../wasmcart/test/wsserver.mjs --port 8796 &   # from the wasmcart repo
./net_test 8796         # require, nextTick, net.connect, WebSocket round-trip
./rumble_test ../wasmcart/test/fixtures/rumble.wasc
./text_test  test/textauto.wasc 5452 5456 5460 5472
sh test/input_guard_test.sh   # keyboard is not also a gamepad while typing
sh test/wayland_egl_guard_test.sh   # Wayland EGL stays on SDL's surface, torn down first
./peer_test 8796 <granted.wasc> <ungranted.wasc>   # wc_peer_* end to end
./seed_test ../wasmcart/test/fixtures/detrng.wasc  # entropy differs, pinned reproduces
```

`text_test` takes the cart's debug-field offsets as arguments because they move
whenever the fixture is recompiled -- linking `malloc` alone shifted them 16
bytes. Read them with wasmcart's `readDebugState()`.

Two traps worth knowing before writing another one:

- `wc_host_enter_v8()` must be called **after** `wc_host_create()`; create() is
  what initialises V8. Calling it first segfaults before `main()` prints
  anything, which reads like a host bug rather than a harness bug.
- A static library does not relink automatically. Rebuilding `libwasmcart.a`
  and re-running a stale test binary produces confident wrong answers.

### GL Import Bridge (gl_imports.cpp)

~209 GLES3 functions registered as V8 FunctionCallbacks (the count is logged
at startup: `GL imports registered (N functions, ...)`). Key non-obvious behaviors:

**FBO Redirect**: All `glBindFramebuffer(GL_FRAMEBUFFER, 0)` calls are intercepted and redirected to a capture FBO. This lets the host blit the cart's output to the screen with letterboxing. `glClear` on the redirect FBO is suppressed after a cart blit to prevent wiping captured content.

**GL Version Filtering**: `glGetString(GL_VERSION)` returns `"OpenGL ES 3.0 wasmcart"` regardless of actual driver. Extensions are filtered to a WebGL2-compatible subset (7 extensions). This prevents Skia/Ganesh from probing for ES 3.1+ functions that aren't in the WASM import table. 28 ES 3.1+ functions are registered as no-op stubs for safety.

**`glGetInternalformativ`**: Required for Skia Ganesh GPU rendering. Ganesh queries max MSAA samples — without this function, `maxSamples=0` → render target creation fails → software fallback at ~60 FPS instead of 700+ FPS GPU.

**Signed Blit Coordinates**: `glBlitFramebuffer` source rect dimensions must be computed with signed math. Ganesh uses Y-inverted blits (srcY0 > srcY1). Storing the height as `uint32_t` causes wrap-around to ~4 billion → corrupted blit → black screen.

**Client-Side Vertex Arrays**: gl4es carts pass WASM memory offsets as "pointers" to `glVertexAttribPointer` with no VBO bound. The bridge tracks `GL_ARRAY_BUFFER` binding state and uploads client-side data to temp VBOs at draw time.

### Performance: Native vs Node.js Host

The Node.js host ([retroemu](https://github.com/monteslu/retroemu) +
[native-gles](https://github.com/monteslu/native-gles)) currently beats the native
host on some GL carts (~860 vs ~716 FPS for Skia Ganesh). Both use V8 for WASM and the same GPU for GL. The gap is in the GL call overhead:

- **Node.js host**: GL calls go through N-API (native-gles addon) — one function pointer call per GL function, minimal marshaling.
- **Native host**: GL calls go through V8 FunctionCallbacks — each call enters V8's callback machinery, extracts args from `v8::FunctionCallbackInfo`, converts types. This overhead multiplies across ~73 GL calls per frame (Ganesh) or ~193 GL calls in complex scenes.

**Potential fix**: Register GL imports as [fast API calls](https://v8.dev/docs/embed) (`v8::CFunction`) instead of regular FunctionCallbacks. V8's fast API path bypasses the full callback machinery for simple functions with known signatures — could eliminate most of the per-call overhead.

### Wayland vs X11

SDL2 prefers X11 (through Xwayland) whenever `DISPLAY` is set, so on most
Wayland desktops the player runs on X11 unless you ask for
`SDL_VIDEODRIVER=wayland`. sdl2-compat (SDL2's API over SDL3, which some
distros ship as SDL2) is different: SDL3 prefers Wayland on compositors that
offer `wp_fifo_v1`, so there native Wayland is the default. Both work for 2D
and GL carts; the window-surface code checks `wm_info.subsystem` at runtime,
not with a compile-time `#ifdef`.

On native Wayland, EGL has to live on SDL's own `wl_display`: Wayland objects
can't cross connections, and `EGL_DEFAULT_DISPLAY` would make Mesa open its
own (or pick its X11 platform, which fails without `DISPLAY`). SDL2 only hands
its `wl_display` out through a window, and GL carts run their init while
loading, before the real window exists, so the player first creates a hidden
16x16 window. The EGL display goes on its connection, and a `wl_egl_window` on
its surface stands in for the pbuffer that Mesa's Wayland platform doesn't
have; cart init sees the same 16x16 default framebuffer as elsewhere. The real
window then gets a `wl_egl_window` the player owns and resizes to follow the
window, since SDL only makes one for an `SDL_WINDOW_OPENGL` window. This needs
`libwayland-dev` at build time. X11 keeps `EGL_DEFAULT_DISPLAY`, since X window
IDs work across connections.

Presents on Wayland are paced by the player, not by `eglSwapInterval(1)`: Mesa
waits for the compositor's frame callback with no timeout, and compositors
stop sending them to a covered or minimized window, which would freeze the
loop, quit signals included. Like SDL's own Wayland GL path, the player swaps
with interval 0 and waits for the callback itself, for at most 50ms.

### Platform-Specific

- **macOS / Windows**: Require [ANGLE](https://github.com/google/angle) for GLES3 (no native GLES). Set `ANGLE_DIR` in cmake.
- **macOS**: Link `-framework Security -framework SystemConfiguration` (libnode TLS).
- **Windows**: MSVC needs `/std:c++20 /Zc:__cplusplus` (CXX only) + static CRT (`/MT`).
- **Windows**: `clock_gettime` → `QueryPerformanceCounter` (`#ifdef _WIN32`).

## License

MIT
