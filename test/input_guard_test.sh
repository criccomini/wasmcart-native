#!/bin/sh
# Does the standalone player stop treating keys as gamepad buttons while the
# cart is taking text?
#
# This lives in main.c, not the library, so there is no API to call -- the
# player owns its own input polling. A source check is the honest option: it
# cannot prove the runtime behaviour, but it does catch the regression, which is
# someone deleting the guard while refactoring input.
#
# The bug it guards against shipped in v0.3.0: poll_keyboard_as_pad() ran
# unconditionally, so typing a name also played the game -- "w" walked the
# player, Q/E fired the shoulders, Return pressed Start.
#
# Run: sh test/input_guard_test.sh
set -e
SRC="$(dirname "$0")/../src/main.c"
fail=0

if grep -q '!wc_host_text_input_active(host))' "$SRC" &&
   grep -A2 '!wc_host_text_input_active(host))' "$SRC" | grep -q 'poll_keyboard_as_pad'; then
  echo "  ok    keyboard-as-gamepad is suppressed during text input"
else
  echo "*** FAIL poll_keyboard_as_pad() is not guarded by wc_host_text_input_active()"
  echo "         typing in a cart's text field will also drive gameplay"
  fail=1
fi

# The guard must NOT extend to real gamepads: a controller should keep working
# while a text field is open (a d-pad character picker needs it).
if grep -B4 'poll_pads(pads);' "$SRC" | grep -q 'text_input_active'; then
  echo "*** FAIL real gamepad polling is also suppressed; it should not be"
  fail=1
else
  echo "  ok    real gamepads keep working during text input"
fi

[ "$fail" = 0 ] && echo "\nall checks passed" || echo "\nFAILED"
exit $fail
