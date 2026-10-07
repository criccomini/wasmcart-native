#!/bin/sh
# Does the standalone player keep its native-Wayland EGL objects on SDL's
# terms?
#
# This lives in main.c and egl_context.c, and CI never runs a Wayland session,
# so a source check is the honest option: it can't prove the runtime
# behaviour, but it does catch the two regressions that are easy to make while
# refactoring.
#
# - SDL2 only fills info.wl.egl_window for an SDL_WINDOW_OPENGL window, and
#   the player's window isn't one, so that field is always NULL here. Handing
#   it to eglCreateWindowSurface is how GL carts on Wayland used to fail.
# - Our EGLSurface and wl_egl_window sit on SDL's wl_surface, and the
#   EGLDisplay on SDL's wl_display. Destroying the SDL window (or quitting SDL)
#   first leaves Mesa holding objects on a dead connection.
# - In direct present the cart draws straight onto that surface, so the
#   wl_egl_window takes the window's size before the cart's frame, not only
#   at the swap after it (a resized window would get one frame drawn at the
#   old size), and before direct mode is chosen against that size.
#
# Run: sh test/wayland_egl_guard_test.sh
set -e
SRC="$(dirname "$0")/../src/main.c"
fail=0

if grep -q 'wl\.egl_window' "$SRC"; then
  echo "*** FAIL main.c uses info.wl.egl_window, which is NULL without SDL_WINDOW_OPENGL"
  fail=1
else
  echo "  ok    the Wayland window surface is built on SDL's wl_surface"
fi

cleanup=$(sed -n '/8\. Cleanup/,$p' "$SRC")
egl_line=$(echo "$cleanup" | grep -n 'egl_destroy();' | head -1 | cut -d: -f1)
win_line=$(echo "$cleanup" | grep -n 'SDL_DestroyWindow(window);' | head -1 | cut -d: -f1)
if [ -n "$egl_line" ] && [ -n "$win_line" ] && [ "$egl_line" -lt "$win_line" ]; then
  echo "  ok    EGL is torn down before the SDL window"
else
  echo "*** FAIL egl_destroy() must come before SDL_DestroyWindow() at cleanup"
  fail=1
fi

loop=$(sed -n '/Main loop/,/8\. Cleanup/p' "$SRC")
resize_line=$(echo "$loop" | grep -n 'egl_resize_window_surface(' | head -1 | cut -d: -f1)
direct_line=$(echo "$loop" | grep -n 'wc_gl_set_direct(want);' | head -1 | cut -d: -f1)
frame_line=$(echo "$loop" | grep -n 'wc_host_run_frame(host);' | head -1 | cut -d: -f1)
if [ -n "$resize_line" ] && [ -n "$direct_line" ] && [ -n "$frame_line" ] &&
   [ "$resize_line" -lt "$direct_line" ] && [ "$direct_line" -lt "$frame_line" ]; then
  echo "  ok    the Wayland surface is resized before direct mode is chosen and the cart draws"
else
  echo "*** FAIL egl_resize_window_surface() must come before wc_gl_set_direct() and wc_host_run_frame() in the main loop"
  fail=1
fi

[ "$fail" = 0 ] && echo "\nall checks passed" || echo "\nFAILED"
exit $fail
