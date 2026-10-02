// egl_context.c — EGL context for GL carts in standalone player

#include "egl_context.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>

#ifdef __APPLE__
#include "mac_layer.h"
#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#endif
#endif

typedef EGLDisplay (*PFNEGLGETPLATFORMDISPLAYEXTPROC_WC)(EGLenum, void*, const EGLint*);

#ifdef WC_HAVE_WAYLAND_EGL
/* After EGL/egl.h on purpose: wayland-egl-core.h defines WL_EGL_PLATFORM,
 * which would otherwise switch eglplatform.h's native types. */
#include <wayland-client.h>
#include <wayland-egl.h>
#include <string.h>
#ifndef EGL_PLATFORM_WAYLAND_EXT
#define EGL_PLATFORM_WAYLAND_EXT 0x31D8
#endif
#ifndef EGL_PRESENT_OPAQUE_EXT
#define EGL_PRESENT_OPAQUE_EXT 0x31DF
#endif
#endif

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static EGLConfig egl_config;
static bool initialized = false;
static bool window_surface = false;
static int egl_samples = 0; /* egl_set_samples: a multisampled surface (--msaa) */

void egl_set_samples(int samples) { egl_samples = samples > 0 ? samples : 0; }
int egl_get_samples(void) { return egl_samples; }

#ifdef __APPLE__
/* The original SDL handle and its resolved CALayer, plus deferred vsync:
 * ANGLE's Metal backend accepts eglSwapInterval but never syncs — the real
 * switch is CAMetalLayer.displaySyncEnabled, and that layer only exists
 * after ANGLE's first present, so the interval is applied lazily from
 * egl_swap_buffers. */
static void* mac_native_window = NULL;
static void* mac_layer = NULL;
static int mac_desired_sync = -1;
static bool mac_sync_applied = true;
#endif

#ifdef WC_HAVE_WAYLAND_EGL
/* Native Wayland. The EGL display lives on SDL's wl_display (never ours to
 * disconnect), and the player owns the wl_egl_windows: SDL only makes one for
 * an SDL_WINDOW_OPENGL window. wl_boot_win stands in for the pbuffer until the
 * real window exists; wl_win is on the player window and follows its size. */
static struct wl_display* wl_dpy = NULL;
static struct wl_egl_window* wl_boot_win = NULL;
static struct wl_egl_window* wl_win = NULL;
static int wl_w = 0, wl_h = 0;
static bool wl_opaque = false;

static EGLSurface create_wl_surface(struct wl_egl_window* win) {
    /* Opaque even if the cart leaves alpha below 1 in the framebuffer;
     * otherwise the compositor would blend the game with what's behind it. */
    static const EGLint opaque_attribs[] = { EGL_PRESENT_OPAQUE_EXT, EGL_TRUE, EGL_NONE };
    return eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)win,
        wl_opaque ? opaque_attribs : NULL);
}
#endif

static int create_context(void* wl_display, void* boot_wl_surface,
                          uint32_t width, uint32_t height) {
#ifdef __APPLE__
    (void)wl_display;
    (void)boot_wl_surface;
    /* Prefer ANGLE's Metal backend. The default display resolves to the
     * deprecated CGL backend, whose swap layer free-runs — eglSwapInterval
     * is a no-op there, so window presents can never sync to the display.
     * Falls through to the default display on builds without Metal. */
    PFNEGLGETPLATFORMDISPLAYEXTPROC_WC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC_WC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display) {
        const EGLint attribs[] = {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
            EGL_NONE
        };
        egl_display = get_platform_display(EGL_PLATFORM_ANGLE_ANGLE,
            (void*)EGL_DEFAULT_DISPLAY, attribs);
    }
    if (egl_display == EGL_NO_DISPLAY)
        egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
#ifdef WC_HAVE_WAYLAND_EGL
    if (wl_display) {
        /* Wayland objects belong to one connection, and SDL's window is on
         * SDL's. EGL_DEFAULT_DISPLAY would make Mesa open a connection of its
         * own (or pick its X11 platform, failing without DISPLAY). */
        PFNEGLGETPLATFORMDISPLAYEXTPROC_WC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC_WC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        egl_display = get_platform_display
            ? get_platform_display(EGL_PLATFORM_WAYLAND_EXT, wl_display, NULL)
            : eglGetDisplay((EGLNativeDisplayType)wl_display);
        wl_dpy = (struct wl_display*)wl_display;
    } else
#else
    (void)boot_wl_surface;
#endif
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#endif
    if (egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "wasmcart: eglGetDisplay failed\n");
        return -1;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        fprintf(stderr, "wasmcart: eglInitialize failed\n");
        return -1;
    }
    fprintf(stderr, "wasmcart: EGL %d.%d\n", major, minor);

    // Request GLES3 context (multisampled when egl_set_samples asked for it,
    // falling back to the plain config if the display has none). Mesa's
    // Wayland platform has no pbuffer configs, so there the boot surface is a
    // window too.
    EGLint surface_type = wl_display ? EGL_WINDOW_BIT : (EGL_PBUFFER_BIT | EGL_WINDOW_BIT);
    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, surface_type,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, egl_samples > 0 ? 1 : 0,
        EGL_SAMPLES, egl_samples > 0 ? egl_samples : 0,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs) || num_configs == 0) {
        if (egl_samples > 0) {
            fprintf(stderr, "wasmcart: no %d-sample EGL config, using a plain one\n", egl_samples);
            // By name rather than position, so the list above can change.
            for (int i = 0; config_attribs[i] != EGL_NONE; i += 2)
                if (config_attribs[i] == EGL_SAMPLE_BUFFERS || config_attribs[i] == EGL_SAMPLES)
                    config_attribs[i + 1] = 0;
        }
        if (!eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs) || num_configs == 0) {
            fprintf(stderr, "wasmcart: eglChooseConfig failed\n");
            return -1;
        }
    }

    EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };

    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "wasmcart: eglCreateContext failed (0x%x)\n", eglGetError());
        return -1;
    }

#ifdef WC_HAVE_WAYLAND_EGL
    if (wl_display) {
        // Boot surface: a width x height wl_egl_window on a hidden window, so
        // the cart's init sees the same complete default framebuffer the
        // pbuffer gives elsewhere. Nothing is ever swapped to it, so nothing
        // reaches the compositor, and what the cart draws there is discarded
        // just as the pbuffer's contents were.
        const char* exts = eglQueryString(egl_display, EGL_EXTENSIONS);
        wl_opaque = exts && strstr(exts, "EGL_EXT_present_opaque");
        wl_boot_win = boot_wl_surface
            ? wl_egl_window_create((struct wl_surface*)boot_wl_surface, (int)width, (int)height)
            : NULL;
        egl_surface = wl_boot_win ? create_wl_surface(wl_boot_win) : EGL_NO_SURFACE;
        if (egl_surface == EGL_NO_SURFACE) {
            fprintf(stderr, "wasmcart: Wayland boot surface failed (0x%x)\n", eglGetError());
            return -1;
        }
    } else
#endif
    {
        // Create a pbuffer surface (will be replaced by window surface later if available)
        EGLint pbuffer_attribs[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_NONE
        };

        egl_surface = eglCreatePbufferSurface(egl_display, egl_config, pbuffer_attribs);
        if (egl_surface == EGL_NO_SURFACE) {
            fprintf(stderr, "wasmcart: eglCreatePbufferSurface failed (0x%x)\n", eglGetError());
            return -1;
        }
    }

    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        fprintf(stderr, "wasmcart: eglMakeCurrent failed\n");
        return -1;
    }

    initialized = true;

    fprintf(stderr, "wasmcart: GL: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "wasmcart: GL version: %s\n", glGetString(GL_VERSION));

    return 0;
}

int egl_create_context(uint32_t width, uint32_t height) {
    return create_context(NULL, NULL, width, height);
}

#ifdef WC_HAVE_WAYLAND_EGL
int egl_create_wayland_context(void* wl_display, void* boot_wl_surface,
                               uint32_t width, uint32_t height) {
    return create_context(wl_display, boot_wl_surface, width, height);
}

int egl_create_wayland_window_surface(void* wl_surface, int width, int height) {
    if (!initialized || !wl_dpy || !wl_surface || wl_win || width <= 0 || height <= 0) {
        fprintf(stderr, "wasmcart: no EGL context on SDL's Wayland display\n");
        return -1;
    }
    struct wl_egl_window* win = wl_egl_window_create((struct wl_surface*)wl_surface, width, height);
    EGLSurface s = win ? create_wl_surface(win) : EGL_NO_SURFACE;
    if (s == EGL_NO_SURFACE || !eglMakeCurrent(egl_display, s, s, egl_context)) {
        fprintf(stderr, "wasmcart: Wayland window surface failed (0x%x)\n", eglGetError());
        if (s != EGL_NO_SURFACE) eglDestroySurface(egl_display, s);
        if (win) wl_egl_window_destroy(win);
        return -1;  // the boot surface stays current
    }
    // Only now, with the new surface current, retire the boot surface; its
    // wl_egl_window goes after the EGLSurface that used it.
    eglDestroySurface(egl_display, egl_surface);
    wl_egl_window_destroy(wl_boot_win);
    wl_boot_win = NULL;
    egl_surface = s;
    wl_win = win;
    wl_w = width;
    wl_h = height;
    window_surface = true;
    return 0;
}
#endif

void egl_resize_window_surface(int width, int height) {
#ifdef WC_HAVE_WAYLAND_EGL
    // X11, Cocoa and Win32 surfaces follow their window; a wl_egl_window is
    // a size we have to set. SDL would, but only for one it made itself.
    if (wl_win && width > 0 && height > 0 && (width != wl_w || height != wl_h)) {
        wl_egl_window_resize(wl_win, width, height, 0, 0);
        wl_w = width;
        wl_h = height;
    }
#else
    (void)width;
    (void)height;
#endif
}

int egl_create_window_surface(void* native_window) {
    if (!initialized) return -1;

#ifdef __APPLE__
    /* SDL hands over an NSWindow*; ANGLE validates a CALayer*. Resolve it
     * (and remember both, for per-swap scale re-sync). */
    mac_native_window = native_window;
    native_window = wc_mac_layer_for_native_window(native_window);
    if (!native_window) {
        fprintf(stderr, "wasmcart: macOS native window is not an NSWindow/NSView/CALayer\n");
        return -1;
    }
    mac_layer = native_window;
#endif

    // Destroy the pbuffer surface
    if (egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display, egl_surface);
    }

    egl_surface = eglCreateWindowSurface(egl_display, egl_config,
        (EGLNativeWindowType)native_window, NULL);
    if (egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "wasmcart: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
        return -1;
    }

    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    window_surface = true;
    return 0;
}

bool egl_get_drawable_size(int* w, int* h) {
    if (!initialized || !window_surface) return false;
#ifdef WC_HAVE_WAYLAND_EGL
    // Mesa's eglQuerySurface keeps reporting the old size until it fetches
    // the next back buffer, which is a frame late after a resize.
    if (wl_win) {
        *w = wl_w;
        *h = wl_h;
        return true;
    }
#endif
    EGLint sw = 0, sh = 0;
    if (!eglQuerySurface(egl_display, egl_surface, EGL_WIDTH, &sw) ||
        !eglQuerySurface(egl_display, egl_surface, EGL_HEIGHT, &sh) ||
        sw <= 0 || sh <= 0) return false;
    *w = sw;
    *h = sh;
    return true;
}

void egl_make_current(void) {
    if (initialized) {
        eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    }
}

void egl_swap_buffers(void) {
    if (initialized) {
        eglSwapBuffers(egl_display, egl_surface);
#ifdef __APPLE__
        if (mac_layer) {
            /* Cross-monitor drags change the backing scale under us. */
            wc_mac_sync_backing_scale(mac_native_window, mac_layer);
            /* ANGLE creates its CAMetalLayer on the first present; a swap
             * interval requested before then had nothing to apply to. */
            if (!mac_sync_applied) {
                mac_sync_applied = wc_mac_set_display_sync(mac_layer, mac_desired_sync >= 1);
            }
        }
#endif
    }
}

void egl_set_swap_interval(int interval) {
    if (!initialized) return;
    eglSwapInterval(egl_display, interval);
#ifdef __APPLE__
    /* ANGLE Metal ignores eglSwapInterval; displaySyncEnabled is the real
     * switch. Apply now if ANGLE already made its layer, else let
     * egl_swap_buffers retry once it exists. */
    if (mac_layer) {
        mac_desired_sync = interval;
        mac_sync_applied = wc_mac_set_display_sync(mac_layer, interval >= 1);
    }
#endif
}

// Safe to call more than once, and after a partial egl_create_context:
// on Wayland nothing Mesa made on SDL's wl_display may outlive SDL_Quit.
void egl_destroy(void) {
#ifdef __APPLE__
    mac_native_window = NULL;
    mac_layer = NULL;
    mac_desired_sync = -1;
    mac_sync_applied = true;
#endif
    if (egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_surface);
        if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
        // On Wayland this is SDL's wl_display; Mesa only disconnects one it
        // opened itself.
        eglTerminate(egl_display);
    }
#ifdef WC_HAVE_WAYLAND_EGL
    // The wl_egl_windows go after the EGLSurfaces that used them.
    if (wl_win) wl_egl_window_destroy(wl_win);
    if (wl_boot_win) wl_egl_window_destroy(wl_boot_win);
    wl_win = NULL;
    wl_boot_win = NULL;
    wl_dpy = NULL;
    wl_w = wl_h = 0;
    wl_opaque = false;
#endif
    egl_display = EGL_NO_DISPLAY;
    egl_context = EGL_NO_CONTEXT;
    egl_surface = EGL_NO_SURFACE;
    initialized = false;
    window_surface = false;
}

void* egl_get_proc_address(const char* name) {
    return (void*)eglGetProcAddress(name);
}

bool egl_is_initialized(void) {
    return initialized;
}

EGLDisplay egl_get_display(void) {
    return egl_display;
}
