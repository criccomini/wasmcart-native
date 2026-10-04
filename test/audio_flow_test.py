#!/usr/bin/env python3
"""audio_flow_test.py — a cart's audio keeps playing past its first ring.

wasmcart's SDKs let the audio write cursor run free (they only increment it
and index the ring with % cap). A host check that treated a cursor past the
ring's size as bad silenced every SDK-built cart after its first ring. This
runs a cart that plays a steady tone through SDL's disk audio driver for a
few seconds and checks the last second isn't silent.

With no cart given it builds one: a v4 cart writing a square wave, 800
frames per wc_render, cursor incremented and never wrapped, into a ring of
4096 frames, so the cursor passes the ring's size in under half a second.

Run:  python3 test/audio_flow_test.py build/wasmcart-run [<cart with a steady tone>]
"""

import array
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

INFO, FB, CURSOR, RING, CAP = 1024, 2048, 4096, 8192, 4096
WC_FLAG_AUDIO_F32 = 1


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def sleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if (n == 0 and not b & 0x40) or (n == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def section(sid, body):
    return bytes([sid]) + leb(len(body)) + body


def vec(items):
    return leb(len(items)) + b"".join(items)


def name(s):
    return leb(len(s)) + s.encode()


def i32(n):
    return b"\x41" + sleb(n)


def f32(x):
    return b"\x43" + struct.pack("<f", x)


def tone_cart():
    # version, width, height, fb, audio, audio_cap, audio_write, input, save,
    # save_size, time, host_info, flags, audio_sample_rate, pointer, keys,
    # gpu_api, wheel
    info = struct.pack("<18I", 4, 8, 8, FB, RING, CAP, CURSOR, 0, 0, 0, 0, 0,
                       WC_FLAG_AUDIO_F32, 48000, 0, 0, 0, 0)
    types = vec([b"\x60\x00\x01\x7f", b"\x60\x00\x00"])
    funcs = vec([leb(0), leb(1)])
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(0),
                   name("wc_render") + b"\x00" + leb(1)])
    load_cursor = i32(CURSOR) + b"\x28\x02\x00"  # i32.load
    # locals: 0 = n (i32), 1 = addr (i32), 2 = sample (f32)
    render = (
        b"\x02\x40\x03\x40"                                    # block, loop
        + b"\x20\x00" + i32(800) + b"\x4f\x0d\x01"             # n >= 800: br_if out
        + i32(RING) + load_cursor + i32(CAP) + b"\x70"         # RING + cursor % CAP
        + i32(8) + b"\x6c\x6a\x21\x01"                         # * 8, +, addr =
        + f32(0.25) + f32(-0.25) + load_cursor + i32(64) + b"\x71\x1b\x21\x02"  # square
        + b"\x20\x01\x20\x02\x38\x02\x00"                      # left
        + b"\x20\x01\x20\x02\x38\x02\x04"                      # right
        + i32(CURSOR) + load_cursor + i32(1) + b"\x6a\x36\x02\x00"  # cursor++
        + b"\x20\x00" + i32(1) + b"\x6a\x21\x00"               # n++
        + b"\x0c\x00\x0b\x0b"                                  # br loop, end, end
    )

    def body(locals_, code):
        b = locals_ + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(b"\x00", i32(INFO)), body(b"\x02\x02\x7f\x01\x7d", render)])
    data = vec([b"\x00" + i32(INFO) + b"\x0b" + leb(len(info)) + info])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(3, funcs) +
            section(5, mem) + section(7, exports) + section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "tonetest", "abi": 4}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    with tempfile.TemporaryDirectory() as d:
        cart = sys.argv[2] if len(sys.argv) == 3 else os.path.join(d, "tone.wasc")
        if len(sys.argv) == 2:
            open(cart, "wb").write(tone_cart())
        raw = os.path.join(d, "out.raw")
        env = dict(os.environ, SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=raw)
        env.setdefault("SDL_VIDEODRIVER", "offscreen")
        env.setdefault("SDL_RENDER_DRIVER", "software")
        p = subprocess.run(["timeout", "-s", "TERM", "4", sys.argv[1], cart, "--save",
                            os.path.join(d, "s")], env=env, capture_output=True, text=True)
        data = open(raw, "rb").read() if os.path.exists(raw) else b""
    samples = array.array("f", data[: len(data) // 4 * 4])  # F32 stereo
    rate = 48000 * 2
    last = samples[-rate:] if len(samples) > 2 * rate else array.array("f")
    loud = max((abs(x) for x in last), default=0.0)
    if len(samples) <= 2 * rate:
        print(f"*** FAIL only {len(samples) / rate:.2f} s of audio came out")
        print("    " + "\n    ".join(p.stderr.splitlines()[-5:]))
        return 1
    if loud < 0.01:
        print(f"*** FAIL the last second of {len(samples) / rate:.1f} s is silent (peak {loud:.4f})")
        return 1
    print(f"PASS: still playing after {len(samples) / rate:.1f} s (last second's peak {loud:.2f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
