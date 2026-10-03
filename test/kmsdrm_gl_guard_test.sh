#!/bin/sh
# Does the standalone player's KMSDRM GL path keep SDL's rules?
#
# This lives in main.c and needs a DRM device to run, which CI doesn't have,
# so a source check is the honest option: it can't prove the runtime
# behaviour, but it catches the regressions that are easy to make while
# refactoring, each of which SDL turns into a black screen, a modeset or a
# context bound to nothing.
#
# Run: sh test/kmsdrm_gl_guard_test.sh
set -e
SRC="$(dirname "$0")/../src/main.c"
fail=0
ok()  { echo "  ok    $1"; }
bad() { echo "*** FAIL $1"; fail=1; }
first() { grep -n "$1" | head -1 | cut -d: -f1; }

# Driver names are matched case-insensitively by SDL itself; SDL2 reports
# "KMSDRM", SDL3 "kmsdrm".
if grep -E '(^|[^_a-zA-Z])strcmp\(.*[Kk][Mm][Ss][Dd][Rr][Mm]' "$SRC" > /dev/null; then
  bad "the KMSDRM driver name is compared case-sensitively"
else
  ok "the KMSDRM driver name is compared case-insensitively"
fi

# The GL library and EGL config are fixed when SDL creates the window, so the
# attributes come first; a FULLSCREEN flag would rebuild SDL's surfaces.
boot=$(sed -n '/^static bool kms_boot/,/^}/p' "$SRC")
attr=$(echo "$boot" | first 'SDL_GL_SetAttribute')
win=$(echo "$boot" | first 'SDL_CreateWindow')
if [ -n "$attr" ] && [ -n "$win" ] && [ "$attr" -lt "$win" ]; then
  ok "GL attributes are set before the KMSDRM window exists"
else
  bad "SDL_GL_SetAttribute must come before SDL_CreateWindow in kms_boot"
fi
if echo "$boot" | grep -A2 'SDL_CreateWindow' | grep -q 'FULLSCREEN'; then
  bad "the KMSDRM window must not be created FULLSCREEN"
else
  ok "the KMSDRM window is not created FULLSCREEN"
fi

# GL carts run their init while loading: the loader and a current context
# have to exist first. The loader is set before the load and makes the
# context on its first lookup (create_gl_context, kms_boot); on KMSDRM it
# hands out SDL_GL_GetProcAddress.
loader=$(first 'wc_host_set_gl_loader(host, (wc_gl_get_proc_fn)lazy_gl_proc)' < "$SRC")
load=$(first 'wc_host_load_file(' < "$SRC")
lazy=$(sed -n '/^static void\* lazy_gl_proc/,/^}/p' "$SRC")
kproc=$(sed -n '/^static void\* kms_gl_proc/,/^}/p' "$SRC")
create=$(sed -n '/^static void create_gl_context/,/^}/p' "$SRC")
if [ -n "$loader" ] && [ -n "$load" ] && [ "$loader" -lt "$load" ] &&
   echo "$lazy" | grep -q 'kms_gl_proc(name)' &&
   echo "$kproc" | grep -q 'SDL_GL_GetProcAddress(name)' &&
   echo "$create" | grep -q 'kms_boot()'; then
  ok "SDL's GL loader is the one the cart gets on KMSDRM, set before it loads"
else
  bad "the loader set before wc_host_load_file must make SDL's context (kms_boot) and hand out SDL_GL_GetProcAddress"
fi

# A new SDL context starts at swap interval 0.
if sed -n '/is_gl && kms_ctx/,$p' "$SRC" | grep -q 'SDL_GL_SetSwapInterval'; then
  ok "the KMSDRM context gets its swap interval"
else
  bad "SDL_GL_SetSwapInterval must follow the KMSDRM GL branch"
fi

# The default renderer would rebuild the window (EGL, GBM and DRM master).
hint=$(first 'SDL_HINT_RENDER_DRIVER' < "$SRC")
rend=$(first 'SDL_CreateRenderer(' < "$SRC")
if [ -n "$hint" ] && [ -n "$rend" ] && [ "$hint" -lt "$rend" ]; then
  ok "2D carts ask for the GLES2 renderer before creating one"
else
  bad "SDL_HINT_RENDER_DRIVER must be set before SDL_CreateRenderer"
fi

# Destroying the last KMSDRM window unloads SDL's EGL under the context.
cleanup=$(sed -n '/8\. Cleanup/,$p' "$SRC")
del=$(echo "$cleanup" | first 'SDL_GL_DeleteContext(kms_ctx)')
dw=$(echo "$cleanup" | first 'SDL_DestroyWindow(window);')
if [ -n "$del" ] && [ -n "$dw" ] && [ "$del" -lt "$dw" ]; then
  ok "SDL's context is deleted before the window at cleanup"
else
  bad "SDL_GL_DeleteContext(kms_ctx) must come before SDL_DestroyWindow(window)"
fi

[ "$fail" = 0 ] && echo "\nall checks passed" || echo "\nFAILED"
exit $fail
