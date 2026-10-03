#!/usr/bin/env python3
"""audio_flow_test.py — a cart's audio keeps playing past its first ring.

wasmcart's SDKs let the audio write cursor run free (they only increment it
and index the ring with % cap). A host check that treated a cursor past the
ring's size as bad silenced every SDK-built cart after its first ring. This
runs a cart that plays a steady tone through SDL's disk audio driver for a
few seconds and checks the last second isn't silent.

Run:  python3 test/audio_flow_test.py build/wasmcart-run <cart with a steady tone>
      (Couchmix's probe cart is one: spike/carts/out/probe.wasc)
"""

import array
import os
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    with tempfile.TemporaryDirectory() as d:
        raw = os.path.join(d, "out.raw")
        env = dict(os.environ, SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=raw)
        env.setdefault("SDL_VIDEODRIVER", "offscreen")
        env.setdefault("SDL_RENDER_DRIVER", "software")
        subprocess.run(["timeout", "-s", "TERM", "4", sys.argv[1], sys.argv[2], "--save",
                        os.path.join(d, "s")], env=env, capture_output=True)
        data = open(raw, "rb").read() if os.path.exists(raw) else b""
    samples = array.array("f", data[: len(data) // 4 * 4])  # F32 stereo
    rate = 48000 * 2
    last = samples[-rate:] if len(samples) > 2 * rate else array.array("f")
    loud = max((abs(x) for x in last), default=0.0)
    if len(samples) <= 2 * rate:
        print(f"*** FAIL only {len(samples) / rate:.2f} s of audio came out")
        return 1
    if loud < 0.01:
        print(f"*** FAIL the last second of {len(samples) / rate:.1f} s is silent (peak {loud:.4f})")
        return 1
    print(f"PASS: still playing after {len(samples) / rate:.1f} s (last second's peak {loud:.2f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
