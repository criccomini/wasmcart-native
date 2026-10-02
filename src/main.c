// main.c — Standalone wasmcart player (SDL2 frontend for 2D framebuffer carts)
//
// Usage: wasmcart-run game.wasc
//
// This is one frontend on top of libwasmcart. The libretro core is another.
// GL carts are not yet supported in this frontend (needs EGL context setup).

#include "../include/wasmcart_host.h"
#include "egl_context.h"
#include "frame_clock.h"
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

#define MAX_CONTROLLERS 4

static SDL_GameController* controllers[MAX_CONTROLLERS] = {0};

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
}

// ─── Controller management ─────────────────────────────────────────────────

static void open_controller(int device_index) {
    if (!SDL_IsGameController(device_index)) return;
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (!controllers[i]) {
            controllers[i] = SDL_GameControllerOpen(device_index);
            if (controllers[i]) {
                fprintf(stderr, "wasmcart: controller %d connected: %s\n",
                    i, SDL_GameControllerName(controllers[i]));
            }
            return;
        }
    }
}

static void close_controller(SDL_JoystickID id) {
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (controllers[i] &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controllers[i])) == id) {
            fprintf(stderr, "wasmcart: controller %d disconnected\n", i);
            SDL_GameControllerClose(controllers[i]);
            controllers[i] = NULL;
            return;
        }
    }
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

static void queue_silence(SDL_AudioDeviceID dev, uint32_t bytes) {
    uint8_t* silence = calloc(1, bytes);
    if (!silence) return;
    SDL_QueueAudio(dev, silence, bytes);
    free(silence);
}

// ─── Main ──────────────────────────────────────────────────────────────────

// Set from a signal handler, so it must be sig_atomic_t and volatile: the
// main loop polls it instead of the process dying where it stands. Without
// this, Ctrl+C (and any SIGTERM, including the one `timeout` sends) kills the
// player before the save is written, which loses exactly the progress the
// player was asked to keep.
static volatile sig_atomic_t g_should_quit = 0;
static void on_quit_signal(int sig) { (void)sig; g_should_quit = 1; }

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
static uint8_t* load_sav(const char* sav_path, uint32_t* out_size) {
    *out_size = 0;
    FILE* f = fopen(sav_path, "rb");
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

// Must run BEFORE wc_host_destroy(): the pointer returned points INTO the
// cart's linear memory, which is gone afterwards.
static void persist_sav(wc_host_t* host, const char* sav_path) {
    uint32_t size = 0;
    const uint8_t* data = wc_host_get_save_data(host, &size);
    if (!data || size == 0) return;  // cart declares no save block
    FILE* f = fopen(sav_path, "wb");
    if (!f) {
        fprintf(stderr, "wasmcart: could not write save to %s\n", sav_path);
        return;
    }
    if (fwrite(data, 1, size, f) != size) {
        fprintf(stderr, "wasmcart: short write saving to %s\n", sav_path);
    }
    fclose(f);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char* cart_path = argv[1];
    int scale = 1;
    bool fullscreen = false;
    bool show_fps = false;
    bool uncapped = false;
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
    }

    // 1. Create host
    wc_host_t* host = wc_host_create();
    if (!host) {
        fprintf(stderr, "wasmcart: failed to create host\n");
        return 1;
    }

    // 2. Create EGL context (BEFORE SDL window)
    //    Always available — the cart decides whether to use GL or framebuffer
    egl_create_context(16, 16);
    if (egl_is_initialized()) {
        wc_host_set_gl_loader(host, (wc_gl_get_proc_fn)egl_get_proc_address);
    }

    // 3. Load cart
    char sav_path[4096];
    sav_path_for(cart_path, sav_path, sizeof(sav_path));
    uint32_t sav_size = 0;
    uint8_t* sav_data = load_sav(sav_path, &sav_size);

    wc_host_options_t opts = {
        .preferred_width = pref_width,
        .preferred_height = pref_height,
        .host_fps = 60,
        .audio_sample_rate = 48000,
        .save_data = sav_data,
        .save_data_size = sav_size,
    };

    int rc = wc_host_load_file(host, cart_path, &opts);
    if (rc != 0) {
        fprintf(stderr, "wasmcart: failed to load %s\n", cart_path);
        wc_host_destroy(host);
        free(sav_data);
        return 1;
    }

    const wc_cart_info_t* info = wc_host_get_cart_info(host);
    const wc_manifest_t* manifest = wc_host_get_manifest(host);
    bool is_gl = wc_host_uses_gl(host);

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
        wc_host_destroy(host);
        free(sav_data);
        return 1;
    }

    // Load embedded gamecontroller database
    {
        #include "../deps/gamecontrollerdb.h"
        int count = 0;
        for (const char** p = _gamecontrollerdb_lines; *p; p++) {
            if (SDL_GameControllerAddMapping(*p) >= 0) count++;
        }
        fprintf(stderr, "wasmcart: loaded %d controller mappings\n", count);
    }

    // 6. Create window + renderer
    uint32_t win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    // Don't use SDL_WINDOW_OPENGL — EGL provides our GL context, not SDL
    // SDL_WINDOW_OPENGL would make SDL create a competing GLX context

    char title[300];
    snprintf(title, sizeof(title), "wasmcart - %s", manifest->name);

    SDL_Window* window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, win_flags);
    if (!window) {
        fprintf(stderr, "wasmcart: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        wc_host_destroy(host);
        free(sav_data);
        return 1;
    }

    SDL_Renderer* renderer = NULL;
    SDL_Texture* fb_tex = NULL;

    // 2D carts: destroy EGL (conflicts with SDL renderer), use SDL accelerated renderer
    // GL carts: keep EGL for direct GL rendering
    if (!is_gl) {
        egl_destroy();
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
#ifdef SDL_VIDEO_DRIVER_WAYLAND
                fprintf(stderr, "wasmcart: using Wayland EGL window surface\n");
                egl_create_window_surface((void*)wm_info.info.wl.egl_window);
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
    }

    // Set up FBO redirect at the preferred (actual rendering) resolution
    {
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
    uint32_t audio_seed_bytes = 0;  // ~50ms of silence, re-queued on resume
    uint32_t audio_rate = info->audio_sample_rate ? info->audio_sample_rate : 48000;
    bool audio_f32 = (info->flags & WC_FLAG_AUDIO_F32) != 0;

    if (info->audio_ptr && info->audio_cap) {
        SDL_AudioSpec want = {0};
        want.freq = audio_rate;
        want.format = audio_f32 ? AUDIO_F32 : AUDIO_S16;
        want.channels = 2;
        want.samples = 1024;
        want.callback = NULL; // use SDL_QueueAudio

        SDL_AudioSpec have;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio_dev) {
            // Pre-seed with ~50ms of silence to prevent initial underruns
            uint32_t seed_frames = have.freq / 20; // 50ms
            audio_seed_bytes = seed_frames * have.channels * (audio_f32 ? 4 : 2);
            queue_silence(audio_dev, audio_seed_bytes);
            SDL_PauseAudioDevice(audio_dev, 0);
            fprintf(stderr, "wasmcart: audio %uHz %s stereo\n",
                have.freq, audio_f32 ? "F32" : "S16");
        }
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
    bool running = true;
    uint32_t frame_count = 0;
    uint64_t start_ticks = SDL_GetTicks64();
    uint32_t fps_counter = 0;
    uint64_t fps_last = start_ticks;
    wc_frame_clock_t frame_clock;
    wc_frame_clock_start(&frame_clock, (double)start_ticks);
    bool window_focused = true;  // as the cart assumes at start (SPEC: Lifecycle)

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
                    if (event.key.keysym.sym == SDLK_F11) {
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
                           (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
            if (hidden) {
                if (wc_host_suspend(host)) {
                    fprintf(stderr, "wasmcart: suspended at frame %u\n", frame_count);
                    // A hidden app can be killed without ever reaching a
                    // graceful quit, so suspend is a persistence point.
                    persist_sav(host, sav_path);
                    if (audio_dev) {
                        SDL_PauseAudioDevice(audio_dev, 1);
                        SDL_ClearQueuedAudio(audio_dev);
                    }
                }
            } else if (wc_host_is_suspended(host)) {
                // Rebase first: the suspend is a gap we know about, so the
                // cart sees none of it, not even one clamped frame.
                wc_frame_clock_rebase(&frame_clock, (double)now);
                wc_host_resume(host);
                fprintf(stderr, "wasmcart: resumed at frame %u\n", frame_count);
                if (audio_dev) {
                    queue_silence(audio_dev, audio_seed_bytes);
                    SDL_PauseAudioDevice(audio_dev, 0);
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
            SDL_WaitEventTimeout(NULL, 100);
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
        if (!wc_host_text_input_active(host)) {
            poll_keyboard_as_pad(&pads[0]);
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
        if (frame_count == 0 && egl_is_initialized()) {
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
            int cur_w, cur_h;
            if (!egl_get_drawable_size(&cur_w, &cur_h))
                SDL_GetWindowSize(window, &cur_w, &cur_h);
            uint32_t rw = pref_width ? pref_width : cart_w;
            uint32_t rh = pref_height ? pref_height : cart_h;
            wc_gl_blit_to_screen(rw, rh, (uint32_t)cur_w, (uint32_t)cur_h);
            egl_swap_buffers();
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
                uint32_t bytes = num_audio_frames * (is_f32_out ? 8 : 4);
                SDL_QueueAudio(audio_dev, audio, bytes);
            }
        }

        // FPS counter
        frame_count++;
        fps_counter++;
        if (show_fps && (now - fps_last) >= 5000) {
            fprintf(stderr, "wasmcart: FPS: %.1f\n", fps_counter * 1000.0 / (now - fps_last));
            fps_counter = 0;
            fps_last = now;
        }

        // Frame timing — vsync handles it if available, otherwise manual delay
        if (!uncapped) {
            uint64_t frame_end = SDL_GetTicks64();
            uint64_t elapsed = frame_end - now;
            if (elapsed < 16) SDL_Delay(16 - (uint32_t)elapsed);
        }
    }

    // 8. Cleanup
    wc_host_exit_v8();
    fprintf(stderr, "wasmcart: shutting down\n");

    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    if (fb_tex) SDL_DestroyTexture(fb_tex);
    if (renderer) SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    egl_destroy();
    persist_sav(host, sav_path);  // before destroy: reads the cart's memory
    wc_host_destroy(host);
    free(sav_data);
    SDL_Quit();

    return 0;
}
