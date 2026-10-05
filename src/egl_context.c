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
typedef EGLDisplay (*PFNEGLGETPLATFORMDISPLAYEXTPROC_WC)(EGLenum, void*, const EGLint*);
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

int egl_create_context(uint32_t width, uint32_t height) {
#ifdef __APPLE__
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
    // falling back to the plain config if the display has none)
    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | EGL_WINDOW_BIT,
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
            config_attribs[17] = 0;
            config_attribs[19] = 0;
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

    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        fprintf(stderr, "wasmcart: eglMakeCurrent failed\n");
        return -1;
    }

    initialized = true;

    fprintf(stderr, "wasmcart: GL: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "wasmcart: GL version: %s\n", glGetString(GL_VERSION));

    return 0;
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

void egl_destroy(void) {
    if (!initialized) return;
#ifdef __APPLE__
    mac_native_window = NULL;
    mac_layer = NULL;
    mac_desired_sync = -1;
    mac_sync_applied = true;
#endif
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_surface);
    if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
    if (egl_display != EGL_NO_DISPLAY) eglTerminate(egl_display);
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
