#!/bin/sh
# Does the player refuse a cart built for an ABI it doesn't speak?
#
# ABI v4 moved every wc_pad_t field after buttons. A v4 cart on a v3 host
# used to load and read its input from the wrong bytes, with no error. Now
# the host checks wc_info_t.version once the cart has filled it in.
#
# Run: sh test/abi_gate_test.sh build/wasmcart-run <v3 cart> <v4 cart>
#      e.g. ../wasmcart/test/fixtures/hello.wasc ../wasmcart-v4/test/fixtures/hello.wasc
BIN=$1; V3=$2; V4=$3
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_RENDER_DRIVER=${SDL_RENDER_DRIVER:-software} SDL_AUDIODRIVER=dummy
fail=0
out=$(timeout 5 "$BIN" "$V4" 2>&1); rc=$?
if echo "$out" | grep -q "ABI version mismatch" && [ $rc -ne 0 ] && [ $rc -ne 124 ]; then
  echo "  ok    a v4 cart is refused (exit $rc)"
else
  echo "*** FAIL a v4 cart was not refused (exit $rc)"; echo "$out" | tail -5; fail=1
fi
out=$(timeout -s TERM 3 "$BIN" "$V3" 2>&1)
if echo "$out" | grep -q "ABI version mismatch"; then
  echo "*** FAIL a v3 cart was refused"; fail=1
elif echo "$out" | grep -q "wasmcart: running "; then
  echo "  ok    a v3 cart loads"
else
  echo "*** FAIL the v3 cart didn't load"; echo "$out" | tail -5; fail=1
fi
[ "$fail" = 0 ] && echo "\nall checks passed" || echo "\nFAILED"
exit $fail
