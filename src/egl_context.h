// egl_context.h — EGL context management for standalone player

#ifndef WC_EGL_CONTEXT_H
#define WC_EGL_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

// Create an EGL context with a pbuffer surface (headless, for GL carts).
// Must be called BEFORE SDL window creation.
// Returns 0 on success.
int egl_create_context(uint32_t width, uint32_t height);

// Create a window surface from an SDL window's native handle.
// Call after SDL_CreateWindow.
int egl_create_window_surface(void* native_window);

// Size of the window surface in PIXELS. On a HiDPI display (macOS Retina,
// where the layer's contentsScale is the backing scale) this is larger than
// SDL_GetWindowSize, which reports points. False if there is no window
// surface yet.
bool egl_get_drawable_size(int* w, int* h);

// Make the EGL context current (call after SDL init to re-assert).
void egl_make_current(void);

// Swap buffers (present GL frame).
void egl_swap_buffers(void);

// Swap interval: 1 = block presents on vsync, 0 = free-run. On macOS this
// drives CAMetalLayer.displaySyncEnabled (ANGLE ignores eglSwapInterval),
// applied lazily once ANGLE's first present has created its layer.
void egl_set_swap_interval(int interval);

// Destroy EGL context and surfaces.
void egl_destroy(void);

// Get a GL function pointer by name (for wc_host_set_gl_loader).
void* egl_get_proc_address(const char* name);

// Is EGL initialized?
bool egl_is_initialized(void);

#endif // WC_EGL_CONTEXT_H
