// main.c — Standalone wasmcart player (SDL2 frontend for 2D framebuffer carts)
//
// Usage: wasmcart-run game.wasc
//
// This is one frontend on top of libwasmcart. The libretro core is another.
// GL carts are not yet supported in this frontend (needs EGL context setup).

#include "../include/wasmcart_host.h"
#include "egl_context.h"
#include "frame_clock.h"
#include "audio_mix.h"
#define PAD_SLOTS_MAX 4
#include "pad_slots.h"
#include "save_file.h"
#include "save_writer.h"
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>

#if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(WC_HAVE_WAYLAND_EGL)
#define WC_WAYLAND_EGL 1
#endif

#define MAX_CONTROLLERS 4

static SDL_GameController* controllers[MAX_CONTROLLERS] = {0};

// Which controller is which player. Plain first-free unless a Couchmix
// supervisor asks for sticky slots (see the supervisor hooks below).
static pad_slots_t g_slots;
static void hb_write_slot(int slot, bool connected, const char* key);
static void hb_note(char kind, const char* what);
static bool g_save_set_aside = false;

// Letterboxing is NOT done here. GL carts are scaled by wc_gl_blit_to_screen
// (gl_imports.cpp), which computes a centred destination rect from the cart's
// real blit size; 2D carts are scaled by SDL via SDL_RenderSetLogicalSize.

static void print_usage(const char* argv0) {
    fprintf(stderr, "Usage: %s <cart.wasc|cart.wasm> [options]\n", argv0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --res WxH       Window resolution (e.g. 1920x1080)\n");
    fprintf(stderr, "  --width <N>     Window width (default: 2x cart width)\n");
    fprintf(stderr, "  --height <N>    Window height (default: 2x cart height)\n");
    fprintf(stderr, "  --scale <N>     Integer scale factor (default: 2)\n");
    fprintf(stderr, "  --fullscreen    Start in fullscreen mode\n");
    fprintf(stderr, "  --fps           Show FPS counter\n");
    fprintf(stderr, "  --uncapped      Disable vsync and frame cap\n");
    fprintf(stderr, "  --save <path>   Save file (default: the cart's path + .sav)\n");
    fprintf(stderr, "  --save-every <s>  Also write the save every s seconds if it changed\n");
    fprintf(stderr, "  --no-net        Refuse every network connection, whatever the manifest grants\n");
}

// ─── Controller management ─────────────────────────────────────────────────

// A string that names this physical controller across reconnects. On Linux
// that's the input node's `uniq` (BlueZ and the HID drivers put the pad's
// address there) or, failing that, its `phys` (the USB port path). Anywhere
// else, or when neither is set, the instance ID: unique, but not stable.
static void pad_key(int device_index, char* out, size_t n) {
    out[0] = '\0';
#if defined(__linux__) && SDL_VERSION_ATLEAST(2, 24, 0)
    const char* path = SDL_JoystickPathForIndex(device_index);
    if (path && strncmp(path, "/dev/input/", 11) == 0 && !strchr(path + 11, '/')) {
        static const char* attrs[] = { "uniq", "phys" };
        for (int a = 0; a < 2 && !out[0]; a++) {
            char sys[128];
            snprintf(sys, sizeof(sys), "/sys/class/input/%s/device/%s", path + 11, attrs[a]);
            FILE* f = fopen(sys, "r");
            if (!f) continue;
            if (!fgets(out, (int)n, f)) out[0] = '\0';
            fclose(f);
            out[strcspn(out, "\r\n")] = '\0';
        }
    }
#endif
    if (!out[0])
        snprintf(out, n, "sdl-%d", (int)SDL_JoystickGetDeviceInstanceID(device_index));
}

static int slot_of(SDL_JoystickID id) {
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i] &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controllers[i])) == id)
            return i;
    return -1;
}

static void open_controller(int device_index) {
    if (!SDL_IsGameController(device_index)) return;
    // Pads already attached at startup get opened twice: once by the startup
    // loop in main(), and again from the CONTROLLERDEVICEADDED event SDL
    // queues for each of them when the game controller subsystem starts.
    // SDL_GameControllerOpen hands back the same refcounted object for a pad
    // that is already open, so without this check one pad filled two slots
    // (player 2 mirrored player 1), two pads filled all four, and a
    // disconnect freed only the first slot, leaving a "connected" ghost.
    if (slot_of(SDL_JoystickGetDeviceInstanceID(device_index)) >= 0) return;
    char key[PAD_KEY_MAX];
    pad_key(device_index, key, sizeof(key));
    int slot = pad_slots_attach(&g_slots, key);
    if (slot < 0) {
        fprintf(stderr, "wasmcart: no free slot for controller %s\n", key);
        return;
    }
    controllers[slot] = SDL_GameControllerOpen(device_index);
    if (!controllers[slot]) {
        pad_slots_detach(&g_slots, slot);
        return;
    }
    fprintf(stderr, "wasmcart: controller %d connected: %s [%s]\n",
        slot, SDL_GameControllerName(controllers[slot]), g_slots.key[slot]);
    hb_write_slot(slot, true, g_slots.key[slot]);
}

static void close_controller(SDL_JoystickID id) {
    int slot = slot_of(id);
    if (slot < 0) return;
    fprintf(stderr, "wasmcart: controller %d disconnected\n", slot);
    SDL_GameControllerClose(controllers[slot]);
    controllers[slot] = NULL;
    hb_write_slot(slot, false, g_slots.key[slot]);
    pad_slots_detach(&g_slots, slot);
}

// ─── Rumble ────────────────────────────────────────────────────────────────
//
// The cart's wc_pad_rumble imports land here. The host library has already
// clamped the magnitudes to 0..1 and capped the duration, so this only maps
// them onto SDL's 0..65535 motors. Without a backend the imports are silent
// no-ops and wc_pad_has_rumble reports 0 for every pad.

static SDL_GameController* rumble_pad(uint32_t pad_id) {
    return pad_id < MAX_CONTROLLERS ? controllers[pad_id] : NULL;
}

static int rumble_has(void* user, uint32_t pad_id) {
    (void)user;
    SDL_GameController* gc = rumble_pad(pad_id);
    if (!gc) return 0;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    return SDL_GameControllerHasRumble(gc) ? 1 : 0;
#else
    return 1;  // can't ask; SDL_GameControllerRumble fails quietly if not
#endif
}

static void rumble_play(void* user, uint32_t pad_id, float low, float high,
                        uint32_t duration_ms) {
    (void)user;
    SDL_GameController* gc = rumble_pad(pad_id);
    if (gc) SDL_GameControllerRumble(gc, (Uint16)(low * 65535.0f + 0.5f),
                                     (Uint16)(high * 65535.0f + 0.5f), duration_ms);
}

static void rumble_stop(void* user, uint32_t pad_id) {
    (void)user;
    SDL_GameController* gc = rumble_pad(pad_id);
    if (gc) SDL_GameControllerRumble(gc, 0, 0, 0);
}

static void rumble_stop_all(void) {
    for (uint32_t i = 0; i < MAX_CONTROLLERS; i++) rumble_stop(NULL, i);
}

// ─── Poll gamepads ─────────────────────────────────────────────────────────

static void poll_pads(wc_pad_t pads[WC_MAX_PADS]) {
    memset(pads, 0, sizeof(wc_pad_t) * WC_MAX_PADS);
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        SDL_GameController* gc = controllers[i];
        if (!gc) continue;
        pads[i].connected = 1;

        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))       pads[i].buttons |= WC_BUTTON_A;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))       pads[i].buttons |= WC_BUTTON_B;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))       pads[i].buttons |= WC_BUTTON_X;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))       pads[i].buttons |= WC_BUTTON_Y;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  pads[i].buttons |= WC_BUTTON_L;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) pads[i].buttons |= WC_BUTTON_R;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))   pads[i].buttons |= WC_BUTTON_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))    pads[i].buttons |= WC_BUTTON_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))    pads[i].buttons |= WC_BUTTON_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  pads[i].buttons |= WC_BUTTON_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  pads[i].buttons |= WC_BUTTON_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) pads[i].buttons |= WC_BUTTON_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))  pads[i].buttons |= WC_BUTTON_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) pads[i].buttons |= WC_BUTTON_R3;
        /* ABI v4 buttons. SDL_GameControllerHasButton is checked because the
         * paddles and touchpad only exist on some pads, and asking for an
         * absent button is not meaningful. */
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE))    pads[i].buttons |= WC_BUTTON_GUIDE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_MISC1))    pads[i].buttons |= WC_BUTTON_MISC1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_PADDLE1))  pads[i].buttons |= WC_BUTTON_PADDLE1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_PADDLE2))  pads[i].buttons |= WC_BUTTON_PADDLE2;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_PADDLE3))  pads[i].buttons |= WC_BUTTON_PADDLE3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_PADDLE4))  pads[i].buttons |= WC_BUTTON_PADDLE4;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_TOUCHPAD)) pads[i].buttons |= WC_BUTTON_TOUCHPAD;

        /* Straight assignment, no scaling: SDL and the pad struct now use the
         * same representation for every analog axis. The triggers used to need
         * `>> 7` to fit a byte, which is the shift that was wrong by one in
         * the libretro core. */
        pads[i].left_x  = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        pads[i].left_y  = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        pads[i].right_x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
        pads[i].right_y = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
        pads[i].left_trigger  = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        pads[i].right_trigger = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    }
}

// ─── Keyboard → pad fallback (player 0) ────────────────────────────────────
//
// The keyboard acts as a virtual gamepad so a cart is playable with no
// controller attached. This runs IN ADDITION to any real pad rather than
// instead of it: making it conditional on hotplug state would mean unplugging
// a controller silently changed what keys do.
//
// The caller suppresses this while text input is active. See the call site.

static void poll_keyboard_as_pad(wc_pad_t* pad) {
    const uint8_t* kb = SDL_GetKeyboardState(NULL);

    if (kb[SDL_SCANCODE_UP]    || kb[SDL_SCANCODE_W]) pad->buttons |= WC_BUTTON_UP;
    if (kb[SDL_SCANCODE_DOWN]  || kb[SDL_SCANCODE_S]) pad->buttons |= WC_BUTTON_DOWN;
    if (kb[SDL_SCANCODE_LEFT]  || kb[SDL_SCANCODE_A]) pad->buttons |= WC_BUTTON_LEFT;
    if (kb[SDL_SCANCODE_RIGHT] || kb[SDL_SCANCODE_D]) pad->buttons |= WC_BUTTON_RIGHT;
    if (kb[SDL_SCANCODE_Z] || kb[SDL_SCANCODE_SPACE]) pad->buttons |= WC_BUTTON_A;
    if (kb[SDL_SCANCODE_X] || kb[SDL_SCANCODE_LSHIFT]) pad->buttons |= WC_BUTTON_B;
    if (kb[SDL_SCANCODE_C])     pad->buttons |= WC_BUTTON_X;
    if (kb[SDL_SCANCODE_V])     pad->buttons |= WC_BUTTON_Y;
    if (kb[SDL_SCANCODE_Q])     pad->buttons |= WC_BUTTON_L;
    if (kb[SDL_SCANCODE_E])     pad->buttons |= WC_BUTTON_R;
    if (kb[SDL_SCANCODE_RETURN]) pad->buttons |= WC_BUTTON_START;
    if (kb[SDL_SCANCODE_BACKSPACE] || kb[SDL_SCANCODE_RSHIFT]) pad->buttons |= WC_BUTTON_SELECT;

    if (pad->buttons) pad->connected = 1;
}

// The device pulls from the mixer (src/audio_mix.h) on its own thread.
static void audio_callback(void* userdata, Uint8* stream, int len) {
    audio_mix_pull((wc_audio_mix_t*)userdata, (float*)stream, (uint32_t)len / (2 * sizeof(float)));
}

// Wait for a stopped mixer to fade out and play its tail of silence, so
// closing the device doesn't cut the sound off mid-wave. Bounded, in case the
// device has stopped pulling.
static void audio_wait_stopped(SDL_AudioDeviceID dev, wc_audio_mix_t* mix) {
    uint64_t give_up = SDL_GetTicks64() + (uint64_t)(AUDIO_STOP_FADE_MS + AUDIO_TAIL_MS) + 200;
    while (SDL_GetAudioDeviceStatus(dev) == SDL_AUDIO_PLAYING) {
        SDL_LockAudioDevice(dev);
        bool stopped = audio_mix_stopped(mix);
        SDL_UnlockAudioDevice(dev);
        if (stopped || SDL_GetTicks64() >= give_up) break;
        SDL_Delay(2);
    }
}

// ─── Main ──────────────────────────────────────────────────────────────────

// Set from a signal handler, so it must be sig_atomic_t and volatile: the
// main loop polls it instead of the process dying where it stands. Without
// this, Ctrl+C (and any SIGTERM, including the one `timeout` sends) kills the
// player before the save is written, which loses exactly the progress the
// player was asked to keep.
static volatile sig_atomic_t g_should_quit = 0;
static void on_quit_signal(int sig) { (void)sig; g_should_quit = 1; }

// ─── Couchmix supervisor hooks ──────────────────────────────────────────
//
// For a process supervisor (Couchmix) that owns the screen and the Home
// button:
//
// - COUCHMIX_HEARTBEAT_FD names an inherited, write-only fd (a pipe). One
//   line per event: "F <frame> <monotonic_us>" per presented frame,
//   "S"/"R" on suspend/resume, "P" every 250ms while suspended. Writes are
//   non-blocking and dropped on EAGAIN, so a slow supervisor never stalls the
//   game. A supervisor that stops seeing them can call the cart hung.
// - SIGUSR1 asks for a suspend and SIGUSR2 for a resume. They feed the same
//   lifecycle path as a minimized window (callbacks, save, clock rebase),
//   which is the only way to suspend on native Wayland, where SDL2 can't see
//   a minimize.
// - Buttons held at a resume are masked until released, so the press that
//   chose "Resume" in the supervisor's menu never reaches the cart.
// - COUCHMIX_PAD_SLOTS, when set (even empty), makes player slots sticky and
//   seeds them: "0=<key>,1=<key>" (src/pad_slots.h). A pad that drops keeps
//   its slot and gets it back. Every slot change goes down the heartbeat fd
//   as "L <slot> <monotonic_us> <0|1> <key>". A cart that declares one
//   player (or none) gets every pad's input on slot 0, so a pad that went to
//   sleep can't lock everyone else out. The Home button (ABI v4's
//   WC_BUTTON_GUIDE) is the supervisor's, and never reaches the cart.
// - Saves are reported too: "W 0 <monotonic_us>" once a save is on disk,
//   "E 0 <monotonic_us> <reason>" when one couldn't be written
//   (write_failed) or a save that couldn't be loaded was set aside
//   (save_set_aside), and "I <abi> <save_size> <monotonic_us>" once the
//   cart is loaded. The save file stays the source of truth: lines can be
//   dropped.
// - COUCHMIX_KEYBOARD_PAD=0 stops the keyboard driving pad 0. A TV's
//   HDMI-CEC remote shows up as a keyboard.
static volatile sig_atomic_t g_ext_suspend = 0;
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
static int g_hb_fd = -1;

static void on_suspend_signal(int sig) { (void)sig; g_ext_suspend = 1; }
static void on_resume_signal(int sig) { (void)sig; g_ext_suspend = 0; }

static void hb_open(void) {
    const char* s = getenv("COUCHMIX_HEARTBEAT_FD");
    if (!s || !*s) return;
    int fd = atoi(s);
    if (fd < 0 || fcntl(fd, F_GETFD) < 0) {
        fprintf(stderr, "wasmcart: COUCHMIX_HEARTBEAT_FD=%s is not open\n", s);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    signal(SIGPIPE, SIG_IGN);
    g_hb_fd = fd;
}

static void hb_write(char kind, uint32_t frame) {
    if (g_hb_fd < 0) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[64];
    int n = snprintf(line, sizeof(line), "%c %u %llu\n", kind, frame,
        (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull);
    if (write(g_hb_fd, line, (size_t)n) < 0 && errno != EAGAIN && errno != EINTR) {
        close(g_hb_fd);
        g_hb_fd = -1;  // the supervisor went away; keep playing
    }
}

static unsigned long long hb_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
}

static void hb_line(const char* line, int n) {
    if (g_hb_fd < 0 || n <= 0) return;
    if (write(g_hb_fd, line, (size_t)n) < 0 && errno != EAGAIN && errno != EINTR) {
        close(g_hb_fd);
        g_hb_fd = -1;
    }
}

static void hb_note(char kind, const char* what) {
    char line[128];
    int n = what ? snprintf(line, sizeof(line), "%c 0 %llu %s\n", kind, hb_now_us(), what)
                 : snprintf(line, sizeof(line), "%c 0 %llu\n", kind, hb_now_us());
    hb_line(line, n);
}

static void hb_info(uint32_t abi, uint32_t save_size) {
    char line[96];
    hb_line(line, snprintf(line, sizeof(line), "I %u %u %llu\n", abi, save_size, hb_now_us()));
}

static void hb_write_slot(int slot, bool connected, const char* key) {
    if (g_hb_fd < 0) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[PAD_KEY_MAX + 64];
    int n = snprintf(line, sizeof(line), "L %d %llu %d %s\n", slot,
        (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull,
        connected ? 1 : 0, key);
    if (write(g_hb_fd, line, (size_t)n) < 0 && errno != EAGAIN && errno != EINTR) {
        close(g_hb_fd);
        g_hb_fd = -1;
    }
}

static void supervisor_hooks_init(void) {
    signal(SIGUSR1, on_suspend_signal);
    signal(SIGUSR2, on_resume_signal);
    hb_open();
}
#else
static void hb_write(char kind, uint32_t frame) { (void)kind; (void)frame; }
static void hb_write_slot(int slot, bool connected, const char* key) {
    (void)slot; (void)connected; (void)key;
}
static void hb_note(char kind, const char* what) { (void)kind; (void)what; }
static void hb_info(uint32_t abi, uint32_t save_size) { (void)abi; (void)save_size; }
static void supervisor_hooks_init(void) {}
#endif

// Couchmix's pad policy: sticky slots, and one-player carts hear every pad.
static bool g_couchmix_pads = false;
static bool g_keyboard_pad = true;

static void pad_policy_init(void) {
    const char* seed = getenv("COUCHMIX_PAD_SLOTS");
    g_couchmix_pads = seed != NULL;
    pad_slots_init(&g_slots, g_couchmix_pads, seed);
    const char* kb = getenv("COUCHMIX_KEYBOARD_PAD");
    g_keyboard_pad = !(kb && strcmp(kb, "0") == 0);
}

static int16_t louder(int16_t a, int16_t b) { return abs(b) > abs(a) ? b : a; }

// Fold every connected pad into slot 0: buttons OR'd, each stick axis from
// whichever pad pushes it furthest, each trigger at its highest.
static void merge_into_slot0(wc_pad_t pads[WC_MAX_PADS]) {
    wc_pad_t m;
    memset(&m, 0, sizeof(m));
    for (int i = 0; i < WC_MAX_PADS; i++) {
        if (!pads[i].connected) continue;
        m.connected = 1;
        m.buttons |= pads[i].buttons;
        m.left_x = louder(m.left_x, pads[i].left_x);
        m.left_y = louder(m.left_y, pads[i].left_y);
        m.right_x = louder(m.right_x, pads[i].right_x);
        m.right_y = louder(m.right_y, pads[i].right_y);
        if (pads[i].left_trigger > m.left_trigger) m.left_trigger = pads[i].left_trigger;
        if (pads[i].right_trigger > m.right_trigger) m.right_trigger = pads[i].right_trigger;
    }
    memset(pads, 0, sizeof(wc_pad_t) * WC_MAX_PADS);
    pads[0] = m;
}

// ─── Save data ──────────────────────────────────────────────────────────
//
// A cart's save block is a region of its own linear memory that the host is
// responsible for writing out at exit and restoring at load. Without this the
// cart's save API appears to work for the length of one run and loses
// everything on the next, which reads as "saving is broken" rather than as a
// missing host step.
//
// Path convention matches the JS players (src/save.js): a local cart saves
// alongside itself with ".sav" appended, so a cart moved between players finds
// the same file.
static void sav_path_for(const char* cart_path, char* out, size_t out_size) {
    snprintf(out, out_size, "%s.sav", cart_path);
}

// Returns a malloc'd buffer the caller frees, or NULL when there is no save
// yet (the ordinary first run, not an error).
static bool g_sav_too_big = false;

static uint8_t* load_sav(const char* sav_path, uint32_t* out_size) {
    // A save over the cap can't belong to any region the host would accept,
    // so don't read it in (it could be any size): it gets set aside below.
    struct stat st;
    if (stat(sav_path, &st) == 0 && st.st_size > (off_t)WC_MAX_SAVE_SIZE) {
        *out_size = 0;
        g_sav_too_big = true;
        return NULL;
    }
    return save_file_read(sav_path, out_size);
}

// Must run BEFORE wc_host_destroy(): the pointer returned points INTO the
// cart's linear memory, which is gone afterwards.
static uint8_t* g_last_saved = NULL;  // the last save written or handed over
static uint32_t g_last_saved_size = 0;
static save_writer_t g_writer;           // writes periodic saves off the game's thread
static bool g_save_retry = false;        // a write failed: send the next one regardless

// Called after every save write, on whichever thread did it. The heartbeat
// lines are single non-blocking writes, so the writer thread can send them.
static void save_done(bool ok) {
    if (!ok) fprintf(stderr, "wasmcart: could not write a save\n");
    hb_note(ok ? 'W' : 'E', ok ? NULL : "write_failed");
}

// now: write it here and now, after anything the writer has in flight (the
// final save on exit). Otherwise hand a snapshot to the writer thread.
static void persist_sav(wc_host_t* host, const char* sav_path, bool now) {
    // After a trap the cart may have died halfway through updating its
    // save; the last save written while it was healthy is the better one.
    if (wc_host_has_trapped(host)) return;
    uint32_t size = 0;
    const uint8_t* data = wc_host_get_save_data(host, &size);
    if (!data || size == 0) return;  // cart declares no save block
    if (save_writer_take_failure(&g_writer)) g_save_retry = true;
    if (!g_save_retry && g_last_saved && g_last_saved_size == size &&
        memcmp(g_last_saved, data, size) == 0)
        return;  // nothing changed since the last write: spare the SD card
    // An all-zero region from a cart that never saved isn't a save: writing
    // it would create a .sav just by running the cart. With a save already
    // on disk, zeros mean the player cleared it, and that must be written or
    // the old save comes back next time (SPEC.md, "Saving is host-managed").
    if (!g_last_saved) {
        uint32_t i = 0;
        while (i < size && data[i] == 0) i++;
        if (i == size) return;
    }
    if (now || !save_writer_post(&g_writer, data, size)) {
        save_writer_flush(&g_writer);
        bool ok = save_file_write(sav_path, data, size) == 0;
        save_done(ok);
        if (!ok) return;
    }
    g_save_retry = false;
    uint8_t* copy = (uint8_t*)realloc(g_last_saved, size);
    if (copy) {
        memcpy(copy, data, size);
        g_last_saved = copy;
        g_last_saved_size = size;
    }
}

// ─── Native Wayland ───────────────────────────────────────────────────────
//
// GL carts run their init inside wc_host_load_file, so a context has to be
// current before the cart loads, and on Wayland it has to be on SDL's own
// wl_display: Wayland objects can't cross connections, and the window will be
// SDL's. SDL2 only hands its wl_display out through a window, so when SDL
// picks Wayland a hidden 16x16 window comes first. Its wl_display carries the
// EGL display and its wl_surface the boot surface that stands in for the
// pbuffer. With any other video driver SDL video is shut down again and the
// pbuffer path below runs exactly as before.
#ifdef WC_WAYLAND_EGL
static SDL_Window* boot_window = NULL;

static void* wayland_boot_window(void** wl_surface) {
    *wl_surface = NULL;
    if (!getenv("WAYLAND_DISPLAY") && !getenv("WAYLAND_SOCKET")) return NULL;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;
    void* display = NULL;
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && strcmp(driver, "wayland") == 0) {
        boot_window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            16, 16, SDL_WINDOW_HIDDEN);
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        if (boot_window && SDL_GetWindowWMInfo(boot_window, &wm) &&
            wm.subsystem == SDL_SYSWM_WAYLAND) {
            display = wm.info.wl.display;
            *wl_surface = wm.info.wl.surface;
        } else if (boot_window) {
            SDL_DestroyWindow(boot_window);
            boot_window = NULL;
        }
    }
    if (!display) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return display;
}

// After egl_destroy() or the switch to the real window: the boot surface's
// wl_egl_window sits on this window's wl_surface.
static void drop_boot_window(void) {
    if (boot_window) SDL_DestroyWindow(boot_window);
    boot_window = NULL;
}
#endif

// ─── KMSDRM (no compositor) ───────────────────────────────────────────────
//
// On SDL's KMSDRM backend the player can't put its own EGL surface on the
// screen: SysWM exposes only the DRM fd and gbm_device, not SDL's gbm_surface,
// and SDL does the page flips in its own GLES swap. So there SDL owns the GL
// context (ES 3.0), and its SDL_GL_GetProcAddress is the cart's GL loader.
// Because GL carts run their init inside wc_host_load_file, the window and
// context have to exist before the cart loads.
//
// One window, for the whole run, at the console's mode. On KMSDRM a window's
// size picks the video mode (a small boot window would switch the screen to
// its lowest mode), and creating or destroying windows moves or unbinds SDL's
// current context and resets the CRTC. It is never resized or made
// fullscreen either; both rebuild SDL's surfaces.
//
// Opt-in with SDL_VIDEODRIVER=kmsdrm, so every other run (including headless
// CI, which loads the cart before SDL starts) is unchanged.
static SDL_Window* kms_window = NULL;
static SDL_GLContext kms_ctx = NULL;

// The context goes before its window: destroying the last KMSDRM window
// unloads SDL's EGL.
static void kms_shutdown(void) {
    if (kms_ctx) SDL_GL_DeleteContext(kms_ctx);
    if (kms_window) SDL_DestroyWindow(kms_window);
    kms_ctx = NULL;
    kms_window = NULL;
}

#ifdef __linux__
#define WC_KMSDRM_GL 1

// SDL_VIDEODRIVER may be a comma-separated list; any entry naming kmsdrm counts.
static bool kms_requested(void) {
    const char* list = getenv("SDL_VIDEODRIVER");
    if (!list) list = getenv("SDL_VIDEO_DRIVER");  // SDL3's name, read by sdl2-compat
    while (list && *list) {
        size_t n = strcspn(list, ",");
        if (n == 6 && SDL_strncasecmp(list, "kmsdrm", 6) == 0) return true;
        list += n;
        if (*list == ',') list++;
    }
    return false;
}

typedef const GLubyte* (*wc_get_string_fn)(GLenum);
typedef void (*wc_rect_fn)(GLint, GLint, GLsizei, GLsizei);

static bool kms_boot(void) {
    if (!kms_requested()) return false;
    // SDL's video init mutes the console keyboard, and only SDL_Quit (or
    // SDL's own SIGINT/SIGTERM handler, which node's pre-empts) unmutes it.
    // Install the player's quit handler now rather than at the main loop, so
    // a signal while the cart loads becomes an orderly exit after the load
    // instead of leaving the console without a keyboard.
    signal(SIGINT, on_quit_signal);
    signal(SIGTERM, on_quit_signal);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "wasmcart: KMSDRM GL unavailable: %s\n", SDL_GetError());
        return false;
    }
    // SDL2 names it "KMSDRM". With a list like "x11,kmsdrm" SDL may have
    // picked another driver.
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || SDL_strcasecmp(driver, "KMSDRM") != 0) {
        fprintf(stderr, "wasmcart: SDL picked %s, not KMSDRM\n", driver ? driver : "no video driver");
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    // After video init (which resets them) and before the window, where SDL
    // fixes the GL library and EGL config: the same ES 3.0, RGBA8, D24S8 as
    // egl_context.c.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_DisplayMode dm;
    wc_get_string_fn get_string = NULL;
    wc_rect_fn viewport = NULL, scissor = NULL;
    int major = 0;
    if (SDL_GetDesktopDisplayMode(0, &dm) != 0 || dm.w <= 0 || dm.h <= 0) goto fail;
    // Desktop size keeps the console's mode. No FULLSCREEN flag: on KMSDRM
    // every window covers the screen anyway, and the flag rebuilds surfaces.
    kms_window = SDL_CreateWindow("wasmcart", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        dm.w, dm.h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!kms_window) goto fail;
    kms_ctx = SDL_GL_CreateContext(kms_window);  // also makes it current
    if (!kms_ctx) goto fail;

    // Through the same loader the cart gets.
    get_string = (wc_get_string_fn)SDL_GL_GetProcAddress("glGetString");
    viewport = (wc_rect_fn)SDL_GL_GetProcAddress("glViewport");
    scissor = (wc_rect_fn)SDL_GL_GetProcAddress("glScissor");
    // Insist on what was asked for: a cart must not run on an ES 2 context.
    if (!get_string || !viewport || !scissor ||
        sscanf((const char*)get_string(GL_VERSION), "OpenGL ES %d", &major) != 1 || major < 3) {
        SDL_SetError("no OpenGL ES 3 context");
        goto fail;
    }
    // Cart init sees the same 16x16 viewport and scissor box as the pbuffer
    // other platforms boot on.
    viewport(0, 0, 16, 16);
    scissor(0, 0, 16, 16);
    SDL_ShowCursor(SDL_DISABLE);  // nothing here forwards the mouse

    SDL_version v;
    SDL_GetVersion(&v);
    fprintf(stderr, "wasmcart: KMSDRM GL: %s, %s, %dx%d@%dHz (SDL %d.%d.%d %s)\n",
        get_string(GL_RENDERER), get_string(GL_VERSION), dm.w, dm.h, dm.refresh_rate,
        v.major, v.minor, v.patch, SDL_GetRevision());
    return true;

fail:
    fprintf(stderr, "wasmcart: KMSDRM GL unavailable: %s\n", SDL_GetError());
    kms_shutdown();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return false;
}
#endif

static void window_pixel_size(SDL_Window* window, int* w, int* h) {
#if SDL_VERSION_ATLEAST(2, 26, 0)
    SDL_GetWindowSizeInPixels(window, w, h);
#else
    SDL_GetWindowSize(window, w, h);  // the same without SDL_WINDOW_ALLOW_HIGHDPI
#endif
}

// A save the host refused to load (wrong size) would be overwritten by the
// cart's fresh one at the next save. Keep it as <save>.invalid-<n> instead.
static void set_aside_sav(const char* sav_path) {
    char dest[4096 + 32];
    for (int n = 1; n < 1000; n++) {
        snprintf(dest, sizeof(dest), "%s.invalid-%d", sav_path, n);
        FILE* f = fopen(dest, "rb");
        if (f) { fclose(f); continue; }
        if (rename(sav_path, dest) == 0) {
            fprintf(stderr, "wasmcart: kept the save it couldn't load as %s\n", dest);
            g_save_set_aside = true;
        }
        return;
    }
}

static int run_player(int argc, char* argv[]);

// The player's own teardown (the save, audio, GL, SDL_Quit) is done by the
// time run_player returns. What exit() does after that is node's: static
// destructors dispose the isolate and shut the platform down, which joins
// the platform's delayed-task thread. That thread runs a libuv loop until
// its last timer is closed, and the shutdown races with V8 posting one:
// disposing the heap hands its pages to V8's memory pool, whose release
// task, on a worker thread, posts itself again 8 s out. When that lands
// after the platform has closed the other timers, the join, and so the
// exit, waits the 8 s, and a supervisor that gives up first sees a game
// that wouldn't quit. Nothing that teardown does outlives the process, so
// it is skipped.
int main(int argc, char* argv[]) {
    int code = run_player(argc, argv);
    fflush(NULL);
    _Exit(code);
}

static int run_player(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char* cart_path = argv[1];
    int scale = 1;
    bool fullscreen = false;
    bool show_fps = false;
    bool uncapped = false;
    const char* save_override = NULL;
    uint32_t save_every_s = 0;
    bool no_net = false;
    uint32_t pref_width = 0;
    uint32_t pref_height = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--res") == 0 && i + 1 < argc) {
            char* res = argv[++i];
            char* x = strchr(res, 'x');
            if (x) { pref_width = atoi(res); pref_height = atoi(x + 1); }
        }
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
            pref_width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
            pref_height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            scale = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fullscreen") == 0)
            fullscreen = true;
        else if (strcmp(argv[i], "--fps") == 0)
            show_fps = true;
        else if (strcmp(argv[i], "--uncapped") == 0)
            uncapped = true;
        else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc)
            save_override = argv[++i];
        else if (strcmp(argv[i], "--save-every") == 0 && i + 1 < argc)
            save_every_s = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-net") == 0)
            no_net = true;
    }

    // 1. Create host
    wc_host_t* host = wc_host_create();
    if (!host) {
        fprintf(stderr, "wasmcart: failed to create host\n");
        return 1;
    }
    {
        wc_rumble_backend_t rumble = { rumble_has, rumble_play, rumble_stop, NULL };
        wc_host_set_rumble_backend(host, &rumble);
    }

    // 2. Create EGL context (BEFORE SDL window)
    //    Always available — the cart decides whether to use GL or framebuffer.
    //    On native Wayland it goes on SDL's wl_display (see above).
#ifdef WC_WAYLAND_EGL
    void* boot_wl_surface = NULL;
    void* sdl_wl_display = wayland_boot_window(&boot_wl_surface);
    if (sdl_wl_display) {
        egl_create_wayland_context(sdl_wl_display, boot_wl_surface, 16, 16);
        if (!egl_is_initialized()) {
            fprintf(stderr, "wasmcart: no EGL on SDL's Wayland display; "
                            "GL carts need SDL_VIDEODRIVER=x11\n");
            egl_destroy();
            drop_boot_window();
        }
    } else
#endif
#ifdef WC_KMSDRM_GL
    if (kms_boot()) {
        wc_host_set_gl_loader(host, (wc_gl_get_proc_fn)SDL_GL_GetProcAddress);
    } else
#endif
    {
        egl_create_context(16, 16);
    }
    if (egl_is_initialized()) {
        wc_host_set_gl_loader(host, (wc_gl_get_proc_fn)egl_get_proc_address);
    }

    // 3. Load cart
    char sav_path[4096];
    if (save_override)
        snprintf(sav_path, sizeof(sav_path), "%s", save_override);
    else
        sav_path_for(cart_path, sav_path, sizeof(sav_path));
    uint32_t sav_size = 0;
    uint8_t* sav_data = load_sav(sav_path, &sav_size);
    if (!save_writer_start(&g_writer, sav_path, save_done))
        fprintf(stderr, "wasmcart: no save writer thread; saves happen between frames\n");
    // The save as loaded is what's on disk: an exit without changes then
    // doesn't rewrite it.
    if (sav_data) {
        g_last_saved = (uint8_t*)malloc(sav_size);
        if (g_last_saved) {
            memcpy(g_last_saved, sav_data, sav_size);
            g_last_saved_size = sav_size;
        }
    }

    wc_host_options_t opts = {
        .preferred_width = pref_width,
        .preferred_height = pref_height,
        .host_fps = 60,
        .audio_sample_rate = 48000,
        .save_data = sav_data,
        .save_data_size = sav_size,
        .deny_net = no_net,
    };
    if (no_net) fprintf(stderr, "wasmcart: networking off (--no-net)\n");

    int rc = wc_host_load_file(host, cart_path, &opts);
    if (rc != 0) {
        fprintf(stderr, "wasmcart: failed to load %s\n", cart_path);
        egl_destroy();
#ifdef WC_WAYLAND_EGL
        drop_boot_window();
        SDL_Quit();
#endif
        if (kms_window) {
            kms_shutdown();
            SDL_Quit();
        }
        wc_host_destroy(host);
        free(sav_data);
        return 1;
    }
    if (kms_window && g_should_quit) {
        // A quit arrived while the cart loaded (see kms_boot).
        fprintf(stderr, "wasmcart: quit while loading\n");
        kms_shutdown();
        SDL_Quit();
        wc_host_destroy(host);
        free(sav_data);
        return 0;
    }

    if (wc_host_save_rejected(host) || g_sav_too_big) {
        set_aside_sav(sav_path);
        // What's on disk now is nothing: the never-saved guard applies again,
        // so a cart that never saves doesn't get an all-zero file.
        free(g_last_saved);
        g_last_saved = NULL;
        g_last_saved_size = 0;
    }

    const wc_cart_info_t* info = wc_host_get_cart_info(host);
    const wc_manifest_t* manifest = wc_host_get_manifest(host);
    bool is_gl = wc_host_uses_gl(host);

    // A GL cart cannot run without a GL context, and its first GL call through
    // an unresolved proc is a NULL jump. Say why instead of segfaulting. On
    // KMSDRM the context is SDL's, not EGL's.
    if (is_gl && !egl_is_initialized() && !kms_ctx) {
        fprintf(stderr, "wasmcart: %s is a GL cart but EGL failed to initialize "
            "(no usable display?); 2D carts still run without it\n", cart_path);
        wc_host_destroy(host);
        free(sav_data);
        return 1;
    }

    uint32_t cart_w = info->width;
    uint32_t cart_h = info->height;
    uint32_t win_w, win_h;
    if (is_gl) {
        // GL carts: use preferred dimensions if specified, otherwise cart defaults
        win_w = pref_width ? pref_width : cart_w;
        win_h = pref_height ? pref_height : cart_h;
    } else {
        // 2D carts get scaled up for display
        win_w = pref_width ? pref_width : cart_w * scale;
        win_h = pref_height ? pref_height : cart_h * scale;
    }

    // 5. Init SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "wasmcart: SDL_Init failed: %s\n", SDL_GetError());
        egl_destroy();
#ifdef WC_WAYLAND_EGL
        drop_boot_window();
        SDL_Quit();
#endif
        if (kms_window) {
            kms_shutdown();
            SDL_Quit();
        }
        wc_host_destroy(host);
        free(sav_data);
        return 1;
    }

    // Load embedded gamecontroller database
    {
        #include "../deps/gamecontrollerdb.h"
        // The file covers every platform, and a line's GUID can mean a
        // different device elsewhere. SDL_GameControllerAddMapping doesn't
        // look at the platform field (only SDL's file loader does), so skip
        // other platforms' lines here, or an iOS or Windows line can replace
        // the right mapping for a pad on this one.
        const char* platform = SDL_GetPlatform();
        size_t platform_len = strlen(platform);
        int count = 0;
        for (const char** p = _gamecontrollerdb_lines; *p; p++) {
            const char* tag = strstr(*p, "platform:");
            if (tag && (strncmp(tag + 9, platform, platform_len) != 0 ||
                        (tag[9 + platform_len] != ',' && tag[9 + platform_len] != '\0')))
                continue;
            if (SDL_GameControllerAddMapping(*p) >= 0) count++;
        }
        fprintf(stderr, "wasmcart: loaded %d controller mappings\n", count);
    }

    // 6. Create window + renderer
    uint32_t win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    // Don't use SDL_WINDOW_OPENGL — EGL provides our GL context, not SDL
    // SDL_WINDOW_OPENGL would make SDL create a competing GLX context, and on
    // Wayland would make SDL wrap the window in an EGLSurface of its own.

    char title[300];
    snprintf(title, sizeof(title), "wasmcart - %s", manifest->name);

    SDL_Window* window;
    if (kms_window) {
        // The KMSDRM boot window, at the console's mode for the whole run.
        window = kms_window;
        SDL_SetWindowTitle(window, title);
        fprintf(stderr, "wasmcart: KMSDRM keeps the console's mode; --res sets only "
                        "the GL render size\n");
    } else {
        window = SDL_CreateWindow(title,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            win_w, win_h, win_flags);
    }
    if (!window) {
        fprintf(stderr, "wasmcart: SDL_CreateWindow failed: %s\n", SDL_GetError());
        egl_destroy();
#ifdef WC_WAYLAND_EGL
        drop_boot_window();
#endif
        SDL_Quit();
        wc_host_destroy(host);
        free(sav_data);
        return 1;
    }

    // No pointer over the picture. This player hands the cart no mouse input,
    // so a pointer would only be in the way, and on a TV one appears with no
    // mouse at all: HDMI-CEC inputs report motion, and the compositor draws a
    // pointer for them. SDL applies this whenever the pointer enters the
    // window, so it holds across fullscreen toggles.
    SDL_ShowCursor(SDL_DISABLE);

    SDL_Renderer* renderer = NULL;
    SDL_Texture* fb_tex = NULL;

    // 2D carts: destroy EGL (conflicts with SDL renderer), use SDL accelerated renderer
    // GL carts: keep EGL for direct GL rendering
    if (!is_gl) {
        egl_destroy();
#ifdef WC_WAYLAND_EGL
        drop_boot_window();
#endif
        if (kms_ctx) {
            // SDL's GLES2 renderer takes this ES window as it is; the default
            // "opengl" one would rebuild it (EGL and GBM torn down, DRM master
            // dropped and taken again). A hinted driver turns batching off,
            // so turn it back on. Normal priority: SDL_RENDER_DRIVER wins.
            SDL_GL_DeleteContext(kms_ctx);
            kms_ctx = NULL;
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
            SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
        }
        uint32_t render_flags = SDL_RENDERER_ACCELERATED;
        if (!uncapped) render_flags |= SDL_RENDERER_PRESENTVSYNC;
        renderer = SDL_CreateRenderer(window, -1, render_flags);
        if (renderer) {
            fb_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, cart_w, cart_h);
            SDL_RenderSetLogicalSize(renderer, cart_w, cart_h);
            fprintf(stderr, "wasmcart: rendering %ux%u via SDL renderer%s\n",
                cart_w, cart_h, uncapped ? " (uncapped)" : "");
        } else {
            fprintf(stderr, "wasmcart: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        }
    }

    // GL carts: create EGL window surface
    if (is_gl && egl_is_initialized()) {
        SDL_SysWMinfo wm_info;
        SDL_VERSION(&wm_info.version);
        if (SDL_GetWindowWMInfo(window, &wm_info)) {
            // Runtime check — SDL2 may use X11 or Wayland regardless of compile flags
            if (wm_info.subsystem == SDL_SYSWM_WAYLAND) {
#ifdef WC_WAYLAND_EGL
                // SDL only makes a wl_egl_window for an SDL_WINDOW_OPENGL
                // window (and then owns it), so wrap its wl_surface ourselves.
                int pw, ph;
                window_pixel_size(window, &pw, &ph);
                if (egl_create_wayland_window_surface(wm_info.info.wl.surface, pw, ph) != 0) {
                    fprintf(stderr, "wasmcart: could not create a Wayland EGL window surface for %s\n",
                        manifest->name);
                    egl_destroy();
                    drop_boot_window();
                    SDL_DestroyWindow(window);
                    SDL_Quit();
                    wc_host_destroy(host);
                    free(sav_data);
                    return 1;
                }
                fprintf(stderr, "wasmcart: using Wayland EGL window surface\n");
                drop_boot_window();
#elif defined(SDL_VIDEO_DRIVER_WAYLAND)
                fprintf(stderr, "wasmcart: built without wayland-egl; "
                                "GL carts need SDL_VIDEODRIVER=x11 on Wayland\n");
#endif
            } else if (wm_info.subsystem == SDL_SYSWM_X11) {
#ifdef SDL_VIDEO_DRIVER_X11
                fprintf(stderr, "wasmcart: using X11 EGL window surface\n");
                egl_create_window_surface((void*)(uintptr_t)wm_info.info.x11.window);
#endif
            } else if (wm_info.subsystem == SDL_SYSWM_COCOA) {
#ifdef SDL_VIDEO_DRIVER_COCOA
                // NSWindow*; egl_create_window_surface resolves it to the
                // contentView's CALayer for ANGLE's Metal backend.
                fprintf(stderr, "wasmcart: using Cocoa EGL window surface\n");
                egl_create_window_surface((void*)wm_info.info.cocoa.window);
#endif
            }
            egl_make_current();
            /* Vsync unless uncapped: unsynced timer-paced presents land at
             * random phases of the refresh — microstutter at a nominally
             * perfect frame rate. egl_set_swap_interval also handles the
             * macOS Metal path, where eglSwapInterval alone is a no-op. */
            egl_set_swap_interval(uncapped ? 0 : 1);
            fprintf(stderr, "wasmcart: swap interval %d\n", uncapped ? 0 : 1);
            // Set up FBO redirect for GL carts
            {
                extern void wc_gl_setup_redirect(uint32_t width, uint32_t height);
                uint32_t redir_w = pref_width ? pref_width : cart_w;
                uint32_t redir_h = pref_height ? pref_height : cart_h;
                wc_gl_setup_redirect(redir_w, redir_h);
            }
            int actual_w, actual_h;
            SDL_GetWindowSize(window, &actual_w, &actual_h);
            fprintf(stderr, "wasmcart: rendering to %dx%d window via EGL (%s%s)\n",
                actual_w, actual_h, is_gl ? "GL cart" : "2D cart",
                uncapped ? ", uncapped" : "");
        }
    } else if (is_gl && kms_ctx) {
        // Set the interval explicitly: SDL's default for a new context
        // differs between versions. The context is already current.
        if (SDL_GL_SetSwapInterval(uncapped ? 0 : 1) != 0)
            fprintf(stderr, "wasmcart: SDL_GL_SetSwapInterval: %s\n", SDL_GetError());
        extern void wc_gl_setup_redirect(uint32_t width, uint32_t height);
        uint32_t redir_w = pref_width ? pref_width : cart_w;
        uint32_t redir_h = pref_height ? pref_height : cart_h;
        wc_gl_setup_redirect(redir_w, redir_h);
        int dw, dh;
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        fprintf(stderr, "wasmcart: rendering to %dx%d via SDL's KMSDRM GL (swap interval %d)\n",
            dw, dh, uncapped ? 0 : 1);
    }

    // Set up FBO redirect at the preferred (actual rendering) resolution.
    // Only with a live context, which by now means a GL cart: a 2D cart has
    // already traded EGL for the SDL renderer, and when EGL failed to
    // initialize no GL loader was ever set, so this would call through null
    // GL pointers.
    if (egl_is_initialized()) {
        extern void wc_gl_setup_redirect(uint32_t width, uint32_t height);
        uint32_t redir_w = pref_width ? pref_width : cart_w;
        uint32_t redir_h = pref_height ? pref_height : cart_h;
        wc_gl_setup_redirect(redir_w, redir_h);
    }

    // For 2D carts: create a GL texture to blit the framebuffer
    GLuint blit_tex = 0;
    GLuint blit_program = 0;
    GLuint blit_vao = 0;
    if (!is_gl && egl_is_initialized()) {
        // Create texture for framebuffer upload
        glGenTextures(1, &blit_tex);
        glBindTexture(GL_TEXTURE_2D, blit_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cart_w, cart_h, 0,
            GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);

        // Simple blit shader
        const char* vs_src =
            "#version 300 es\n"
            "out vec2 uv;\n"
            "void main() {\n"
            "  float x = float((gl_VertexID & 1) << 2) - 1.0;\n"
            "  float y = float((gl_VertexID & 2) << 1) - 1.0;\n"
            "  uv = vec2((x + 1.0) * 0.5, 1.0 - (y + 1.0) * 0.5);\n"
            "  gl_Position = vec4(x, y, 0.0, 1.0);\n"
            "}\n";
        const char* fs_src =
            "#version 300 es\n"
            "precision mediump float;\n"
            "in vec2 uv;\n"
            "out vec4 fragColor;\n"
            "uniform sampler2D tex;\n"
            "void main() { fragColor = texture(tex, uv); }\n";

        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vs_src, NULL);
        glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fs_src, NULL);
        glCompileShader(fs);
        blit_program = glCreateProgram();
        glAttachShader(blit_program, vs);
        glAttachShader(blit_program, fs);
        glLinkProgram(blit_program);
        glDeleteShader(vs);
        glDeleteShader(fs);

        glGenVertexArrays(1, &blit_vao);
    }

    // 5. Open audio device
    SDL_AudioDeviceID audio_dev = 0;
    wc_audio_mix_t audio_mix = {0};
    uint32_t audio_rate = info->audio_sample_rate ? info->audio_sample_rate : 48000;
    bool audio_f32 = (info->flags & WC_FLAG_AUDIO_F32) != 0;

    if (info->audio_ptr && info->audio_cap) {
        // Float stereo through a callback, whatever the cart writes: the
        // mixer converts S16 carts on the way in, and owning the samples is
        // what lets it fade instead of cutting (see src/audio_mix.h).
        SDL_AudioSpec want = {0};
        want.freq = audio_rate;
        want.format = AUDIO_F32;
        want.channels = 2;
        want.samples = 1024;
        want.callback = audio_callback;
        want.userdata = &audio_mix;

        SDL_AudioSpec have;
        if (audio_mix_init(&audio_mix, audio_rate, audio_rate / 2))
            audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio_dev) {
            SDL_PauseAudioDevice(audio_dev, 0);
            fprintf(stderr, "wasmcart: audio %uHz %s stereo\n",
                have.freq, audio_f32 ? "F32" : "S16");
        }
    }

    // The supervisor hooks go first: the heartbeat fd has to be open to
    // report the slots of the pads opened just below.
    pad_policy_init();
    supervisor_hooks_init();
    {
        uint32_t save_size = 0;
        wc_host_get_save_data(host, &save_size);
        hb_info(info->version, save_size);
        if (g_save_set_aside) hb_note('E', "save_set_aside");
    }

    // 6. Open any already-connected controllers
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        open_controller(i);
    }

    fprintf(stderr, "wasmcart: running %s (%ux%u)\n", manifest->name, cart_w, cart_h);

    // 7. Main loop — enter V8 scopes once (avoids per-frame lock overhead)
    extern void wc_host_enter_v8(void);
    extern void wc_host_exit_v8(void);
    wc_host_enter_v8();
    signal(SIGINT, on_quit_signal);
    signal(SIGTERM, on_quit_signal);
    uint64_t last_alive_ticks = 0;
    uint32_t guard_mask[WC_MAX_PADS] = {0};
    bool running = true;
    int exit_code = 0;
    uint32_t frame_count = 0;
    uint64_t start_ticks = SDL_GetTicks64();
    uint32_t fps_counter = 0;
    uint64_t fps_last = start_ticks;
    wc_frame_clock_t frame_clock;
    wc_frame_clock_start(&frame_clock, (double)start_ticks);
    bool window_focused = true;  // as the cart assumes at start (SPEC: Lifecycle)
    uint64_t last_save_ticks = start_ticks;

    while (running) {
        uint64_t now = SDL_GetTicks64();

        // SDL only emits SDL_TEXTINPUT between StartTextInput and
        // StopTextInput, so mirror whatever the cart asked for. This is also
        // what raises and dismisses the on-screen keyboard on mobile.
        {
            static bool text_started = false;
            bool want = wc_host_text_input_active(host) != 0;
            if (want != text_started) {
                if (want) SDL_StartTextInput(); else SDL_StopTextInput();
                text_started = want;
            }
        }

        // Events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;
                case SDL_TEXTINPUT:
                    // Characters, already resolved by the OS: layout, shift,
                    // dead keys and IME composition are all applied before this
                    // arrives. Forwarded unconditionally -- the host ignores it
                    // unless the cart called wc_text_input_begin().
                    wc_host_push_text(host, event.text.text,
                                      (uint32_t)strlen(event.text.text));
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    open_controller(event.cdevice.which);
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    close_controller(event.cdevice.which);
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                        window_focused = true;
                    else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                        window_focused = false;
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
                    // Not on KMSDRM: there fullscreen rebuilds SDL's surfaces
                    // and resets the screen for a frame, for nothing.
                    if (event.key.keysym.sym == SDLK_F11 && !kms_window) {
                        uint32_t flags = SDL_GetWindowFlags(window);
                        SDL_SetWindowFullscreen(window,
                            (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    }
                    break;
            }
        }

        // Lifecycle (SPEC: Lifecycle). A minimized or hidden window suspends
        // the cart; losing focus only tells it so, and it keeps rendering.
        // Suspension is read from the window flags after the drain rather
        // than from individual events, because backends disagree about which
        // events a minimize and restore send (SDL2 on macOS restores with
        // SHOWN, sdl2-compat with RESTORED) while all of them keep the flags
        // right. Focus is tracked from focus events instead of polled, since
        // a seat with only gamepads never gives a window keyboard focus.
        {
            bool hidden = (SDL_GetWindowFlags(window) &
                           (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0 ||
                          g_ext_suspend;
            if (hidden) {
                if (wc_host_suspend(host)) {
                    hb_write('S', frame_count);
                    // The cart can't stop a rumble it started while it's
                    // suspended, and a paused game shouldn't buzz.
                    rumble_stop_all();
                    fprintf(stderr, "wasmcart: suspended at frame %u\n", frame_count);
                    // A hidden app can be killed without ever reaching a
                    // graceful quit, so suspend is a persistence point.
                    // On the writer thread: an fsync can take seconds on an
                    // SD card, and a paused game must keep its heartbeat.
                    persist_sav(host, sav_path, false);
                    if (audio_dev) {  // fades out, then drops what's left
                        SDL_LockAudioDevice(audio_dev);
                        audio_mix_set_paused(&audio_mix, true);
                        SDL_UnlockAudioDevice(audio_dev);
                    }
                }
            } else if (wc_host_is_suspended(host)) {
                // Rebase first: the suspend is a gap we know about, so the
                // cart sees none of it, not even one clamped frame.
                wc_frame_clock_rebase(&frame_clock, (double)now);
                wc_host_resume(host);
                hb_write('R', frame_count);
                // Mask what is held right now until it is released.
                wc_pad_t held[WC_MAX_PADS];
                poll_pads(held);
                for (int i = 0; i < WC_MAX_PADS; i++) guard_mask[i] = held[i].buttons;
                fprintf(stderr, "wasmcart: resumed at frame %u\n", frame_count);
                if (audio_dev) {  // fades in once the cart has written enough
                    SDL_LockAudioDevice(audio_dev);
                    audio_mix_set_paused(&audio_mix, false);
                    SDL_UnlockAudioDevice(audio_dev);
                }
                fps_counter = 0;
                fps_last = now;
            }
            if (!wc_host_is_suspended(host)) {
                if (window_focused) wc_host_focus(host);
                else wc_host_blur(host);
            }
        }
        if (wc_host_is_suspended(host)) {
            if (!running || g_should_quit) break;
            // No render, present or audio while suspended. Keep node's event
            // loop turning for the cart's connections, and sleep until the
            // next window event instead of spinning.
            wc_host_pump(host);
            if (now - last_alive_ticks >= 250) {
                hb_write('P', frame_count);
                last_alive_ticks = now;
            }
            // A resume signal doesn't wake SDL's wait, so poll faster while
            // the supervisor holds the cart suspended.
            SDL_WaitEventTimeout(NULL, g_ext_suspend ? 16 : 100);
            continue;
        }

        // Don't set viewport before wc_render — the cart manages its own GL state

        // Input
        wc_pad_t pads[WC_MAX_PADS];
        poll_pads(pads);
        // Not while the cart is taking text. The keyboard doubles as a virtual
        // gamepad here, so without this guard typing a name also plays the
        // game: "w" walks the player, Q/E fire the shoulders, Return presses
        // Start. Gamepads are unaffected -- a controller keeps working while a
        // text field is open, which is what you want for a d-pad character
        // picker.
        if (g_keyboard_pad && !wc_host_text_input_active(host)) {
            poll_keyboard_as_pad(&pads[0]);
        }
        for (int i = 0; i < WC_MAX_PADS; i++) {
            if (!guard_mask[i]) continue;
            guard_mask[i] &= pads[i].buttons;  // released buttons leave the mask
            pads[i].buttons &= ~guard_mask[i];
        }
        if (g_couchmix_pads) {
            // Home opens Couchmix's menu, so it isn't the cart's button too.
            for (int i = 0; i < WC_MAX_PADS; i++) pads[i].buttons &= ~(uint32_t)WC_BUTTON_GUIDE;
            if (manifest->players <= 1) merge_into_slot0(pads);
        }
        wc_host_set_pads(host, pads);

        // Time: delta clamped to WC_MAX_DELTA_MS, time_ms kept consistent
        double time_ms, delta_ms;
        wc_frame_clock_tick(&frame_clock, (double)now, &time_ms, &delta_ms);
        wc_host_set_time(host, time_ms, delta_ms, frame_count);

        // Run frame
        wc_host_run_frame(host);

        // After first frame: cart may have resized (Godot reads host_info and reconfigures)
        // Resize redirect FBO to match actual render dimensions
        if (frame_count == 0 && (egl_is_initialized() || kms_ctx)) {
            const wc_cart_info_t* new_info = wc_host_get_cart_info(host);
            if (new_info->width != cart_w || new_info->height != cart_h) {
                cart_w = new_info->width;
                cart_h = new_info->height;
                uint32_t redir_w = pref_width > cart_w ? pref_width : cart_w;
                uint32_t redir_h = pref_height > cart_h ? pref_height : cart_h;
                extern void wc_gl_setup_redirect(uint32_t width, uint32_t height);
                wc_gl_setup_redirect(redir_w, redir_h);
                fprintf(stderr, "wasmcart: resized redirect FBO to %ux%u (cart=%ux%u)\n",
                    redir_w, redir_h, cart_w, cart_h);
            }
        }

        if (wc_host_has_trapped(host)) {
            fprintf(stderr, "wasmcart: cart trapped, exiting\n");
            exit_code = 1;  // a trap is a failure, not a quit
            running = false;
            break;
        }

        if (g_should_quit) {
            running = false;
            break;
        }

        // Present
        if (egl_is_initialized()) {
            // GL carts: blit redirect FBO to screen, then swap
            extern void wc_gl_blit_to_screen(uint32_t cart_w, uint32_t cart_h, uint32_t win_w, uint32_t win_h);
            // The letterbox rect and viewport are in surface PIXELS. On Retina
            // the surface is backing-scale times the window's point size, so
            // SDL_GetWindowSize would put the picture in a corner quarter.
            // A Wayland surface also has to be told the window's new size
            // (F11, a late fullscreen configure, a compositor resize).
            {
                int pw, ph;
                window_pixel_size(window, &pw, &ph);
                egl_resize_window_surface(pw, ph);
            }
            int cur_w, cur_h;
            if (!egl_get_drawable_size(&cur_w, &cur_h))
                SDL_GetWindowSize(window, &cur_w, &cur_h);
            uint32_t rw = pref_width ? pref_width : cart_w;
            uint32_t rh = pref_height ? pref_height : cart_h;
            wc_gl_blit_to_screen(rw, rh, (uint32_t)cur_w, (uint32_t)cur_h);
            egl_swap_buffers();
        } else if (kms_ctx) {
            // GL carts on KMSDRM: the same blit, SDL's swap (and page flip).
            extern void wc_gl_blit_to_screen(uint32_t cart_w, uint32_t cart_h, uint32_t win_w, uint32_t win_h);
            int dw, dh;
            SDL_GL_GetDrawableSize(window, &dw, &dh);
            uint32_t rw = pref_width ? pref_width : cart_w;
            uint32_t rh = pref_height ? pref_height : cart_h;
            wc_gl_blit_to_screen(rw, rh, (uint32_t)dw, (uint32_t)dh);
            SDL_GL_SwapWindow(window);
        } else if (renderer) {
            // 2D carts: SDL accelerated renderer
            uint32_t w, h;
            const uint8_t* fb = wc_host_get_framebuffer(host, &w, &h);
            if (fb && w > 0 && h > 0) {
                SDL_UpdateTexture(fb_tex, NULL, fb, w * 4);
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, fb_tex, NULL, NULL);
                SDL_RenderPresent(renderer);
            }
        }

        // Queue audio
        if (audio_dev) {
            uint32_t num_audio_frames;
            bool is_f32_out;
            const void* audio = wc_host_get_audio(host, &num_audio_frames, &is_f32_out);
            if (num_audio_frames > 0) {
                SDL_LockAudioDevice(audio_dev);
                if (is_f32_out) audio_mix_push_f32(&audio_mix, (const float*)audio, num_audio_frames);
                else audio_mix_push_s16(&audio_mix, (const int16_t*)audio, num_audio_frames);
                SDL_UnlockAudioDevice(audio_dev);
            }
        }

        hb_write('F', frame_count);

        // FPS counter
        frame_count++;
        fps_counter++;
        if (show_fps && (now - fps_last) >= 5000) {
            fprintf(stderr, "wasmcart: FPS: %.1f\n", fps_counter * 1000.0 / (now - fps_last));
            fps_counter = 0;
            fps_last = now;
        }

        // Periodic saves: a crash or power cut then costs at most this long
        // of progress. Unchanged saves aren't rewritten.
        if (save_every_s && now - last_save_ticks >= (uint64_t)save_every_s * 1000) {
            last_save_ticks = now;
            persist_sav(host, sav_path, false);
        }

        // Frame timing — vsync handles it if available, otherwise manual delay
        if (!uncapped) {
            uint64_t frame_end = SDL_GetTicks64();
            uint64_t elapsed = frame_end - now;
            if (elapsed < 16) SDL_Delay(16 - (uint32_t)elapsed);
        }
    }

    // 8. Cleanup
    // Start the sound fading out now, so the fade plays while the save below
    // is written: the save doesn't wait for it, and the cart's last audio is
    // still there to fade rather than run dry.
    if (audio_dev) {
        SDL_LockAudioDevice(audio_dev);
        audio_mix_stop(&audio_mix);
        SDL_UnlockAudioDevice(audio_dev);
    }
    // A long rumble would otherwise run out its duration after the window
    // closes (SDL only stops it when the pad is closed, in SDL_Quit).
    rumble_stop_all();
    wc_host_exit_v8();
    fprintf(stderr, "wasmcart: shutting down\n");
    // Save first: a crash in the GL, audio or SDL teardown below, or a
    // supervisor that loses patience and kills us, mustn't cost the save.
    persist_sav(host, sav_path, true);
    save_writer_stop(&g_writer);

    if (audio_dev) {
        audio_wait_stopped(audio_dev, &audio_mix);
        SDL_CloseAudioDevice(audio_dev);
    }
    audio_mix_free(&audio_mix);
    if (fb_tex) SDL_DestroyTexture(fb_tex);
    if (renderer) SDL_DestroyRenderer(renderer);
    // EGL before the window: on Wayland our surface sits on SDL's wl_surface
    // and the display on SDL's wl_display, and ANGLE likewise wants its
    // surface released while the native window still exists.
    egl_destroy();
    // On KMSDRM SDL's context goes before the last window (which is `window`).
    if (kms_ctx) SDL_GL_DeleteContext(kms_ctx);
    kms_ctx = NULL;
    kms_window = NULL;
    SDL_DestroyWindow(window);
#ifdef WC_WAYLAND_EGL
    drop_boot_window();
#endif
    wc_host_destroy(host);
    free(sav_data);
    SDL_Quit();

    return exit_code;
}
