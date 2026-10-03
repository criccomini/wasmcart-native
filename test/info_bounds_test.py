#!/usr/bin/env python3
"""info_bounds_test.py — a cart can't point the host outside its memory.

Builds tiny carts byte by byte whose wc_info_t names regions past the end of
their one page of memory (pads, framebuffer, time, the audio ring and its
cursor, and wc_info_t itself) and runs wasmcart-run on each. The host must
skip those accesses and say so, not crash.

Run:  python3 test/info_bounds_test.py build/wasmcart-run
"""

import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

PAGE = 65536
INFO = 1024


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


def cart(info_at=INFO, fb=2048, audio=0, audio_cap=0, audio_write=0, inp=0, time_=0, extra=()):
    """One page of memory; wc_get_info returns info_at. extra: (addr, bytes) data."""
    info = struct.pack("<12I", 3, 8, 8, fb, audio, audio_cap, audio_write, inp, 0, 0, time_, 0)
    info += struct.pack("<I", 0)
    types = vec([b"\x60\x00\x01\x7f", b"\x60\x00\x00"])
    funcs = vec([leb(0), leb(1)])
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(0),
                   name("wc_render") + b"\x00" + leb(1)])

    def body(code):
        b = b"\x00" + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(b"\x41" + sleb(info_at)), body(b"")])
    segs = [(INFO, info)] + list(extra)
    data = vec([b"\x00\x41" + sleb(a) + b"\x0b" + leb(len(d)) + d for a, d in segs])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(3, funcs) +
            section(5, mem) + section(7, exports) + section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "boundstest", "abi": 3}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


CASES = [
    ("pads past the end", dict(inp=PAGE - 16), "pad array"),
    ("framebuffer past the end", dict(fb=PAGE - 64), "framebuffer"),
    ("time past the end", dict(time_=PAGE - 8), "wc_time_t"),
    ("audio ring past the end", dict(audio=PAGE - 64, audio_cap=1024, audio_write=4096), "audio ring"),
    ("audio cursor past the ring", dict(audio=8192, audio_cap=256, audio_write=4096,
                                        extra=[(4096, struct.pack("<I", 999999))]), None),
    ("wc_info_t past the end", dict(info_at=PAGE - 8), "wc_info_t"),
]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    failures = []
    with tempfile.TemporaryDirectory() as d:
        for label, kw, logged in CASES:
            path = os.path.join(d, "c.wasc")
            open(path, "wb").write(cart(**kw))
            p = subprocess.run(["timeout", "-s", "TERM", "2", binary, path, "--save", os.path.join(d, "s")],
                               env=env, capture_output=True, text=True)
            crashed = p.returncode < 0 or p.returncode in (134, 139)
            ok = not crashed and (logged is None or logged in p.stderr)
            print(f"  {'ok  ' if ok else 'FAIL'}  {label}: exit {p.returncode}")
            if not ok:
                failures.append(label)
                print("    " + "\n    ".join(p.stderr.splitlines()[-4:]))
    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
