#!/bin/sh
# Linux + Mesa EGL/GLES development libraries. This standalone 8x8 software
# pbuffer test never invokes the player's EGL path or needs a display/device.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$root/vendor/gl" -I"$root/src" \
    "$root/test/gl_timer_egl_test.c" "$root/src/gl_timer.c" \
    -lEGL -lGLESv2 -ldl -o "$scratch/gl-timer-egl-test"
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
"$scratch/gl-timer-egl-test"
"$scratch/gl-timer-egl-test" --desktop
