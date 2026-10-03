#!/usr/bin/env python3
"""save_test.py — saves land where --save says, often, and whole.

Runs wasmcart-run on wasmcart's savecart fixture, which keeps a magic word
and a counter it bumps every frame in its save region:

  - with --save-every 1 the save appears while the cart is still running
  - SIGKILL (no chance to save on the way out) leaves a whole save behind
  - the next run picks the counter up from it and SIGTERM saves a later one
  - a cart with no save region creates no file

Needs an SDL video driver that works headless (offscreen by default).

Run:  python3 test/save_test.py build/wasmcart-run ../wasmcart/test/fixtures
"""

import os
import signal
import struct
import subprocess
import sys
import tempfile
import time

MAGIC = 0x5A5EDA7A


def run(binary, cart, save, extra=(), seconds=2.5, sig=signal.SIGTERM):
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    p = subprocess.Popen([binary, cart, "--save", save, *extra], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    time.sleep(seconds)
    seen_while_running = os.path.exists(save)
    p.send_signal(sig)
    try:
        _, err = p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        _, err = p.communicate()
    return seen_while_running, err


def read_save(path):
    data = open(path, "rb").read()
    if len(data) != 8:
        return None
    magic, counter = struct.unpack("<II", data)
    return counter if magic == MAGIC else None


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    binary, fixtures = sys.argv[1], sys.argv[2]
    savecart = os.path.join(fixtures, "savecart.wasc")
    nosave = os.path.join(fixtures, "rumble.wasc")  # declares no save region
    failures = []

    def expect(what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail else ""))
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as d:
        save = os.path.join(d, "player1", "savecart.sav")
        os.makedirs(os.path.dirname(save))

        seen, err = run(binary, savecart, save, ("--save-every", "1"), sig=signal.SIGKILL)
        expect("periodic save lands while the cart runs", seen)
        first = read_save(save) if os.path.exists(save) else None
        expect("a SIGKILL leaves a whole save", first is not None, f"counter {first}")
        expect("no temp file left", not os.path.exists(save + ".tmp"))

        _, err = run(binary, savecart, save)
        second = read_save(save) if os.path.exists(save) else None
        expect("the next run carries on from it", second is not None and first is not None
               and second > first, f"{first} -> {second}")

        other = os.path.join(d, "rumble.sav")
        run(binary, nosave, other, ("--save-every", "1"), seconds=1.5)
        expect("a cart with no save region writes nothing", not os.path.exists(other))

    if failures and err:
        print("runner output:\n  " + "\n  ".join(err.splitlines()[-10:]))
    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
