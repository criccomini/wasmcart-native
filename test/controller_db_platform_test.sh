#!/bin/sh
# Does the player load only this platform's controller mappings?
#
# deps/gamecontrollerdb.txt has a line per GUID per platform, and the same
# GUID can mean a different pad on another platform. Every line used to go
# through SDL_GameControllerAddMapping, which ignores the platform field, so
# the last line for a GUID won whatever platform it was for. The player logs
# how many mappings it loaded; on Linux that must be the Linux lines exactly.
#
# Run: sh test/controller_db_platform_test.sh build/wasmcart-run test/snake.wasc
set -e
BIN=$1; CART=$2
DB="$(dirname "$0")/../deps/gamecontrollerdb.txt"
[ "$(uname -s)" = Linux ] || { echo "skipped: Linux only"; exit 0; }
want=$(grep -c 'platform:Linux,' "$DB")
got=$(SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=dummy timeout 5 "$BIN" "$CART" 2>&1 |
      sed -n 's/.*loaded \([0-9]*\) controller mappings.*/\1/p' | head -1) || true
if [ "$got" = "$want" ]; then
  echo "  ok    loaded $got mappings, the Linux lines"
else
  echo "*** FAIL loaded '$got' mappings, want $want (the Linux lines only)"
  exit 1
fi
