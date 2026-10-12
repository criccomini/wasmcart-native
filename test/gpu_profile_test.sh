#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Isrc -Ivendor/gl \
    src/gpu_profile.c test/gpu_profile_test.c -lm -o "$tmp/gpu-profile-test"
"$tmp/gpu-profile-test"
