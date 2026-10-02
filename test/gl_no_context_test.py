#!/usr/bin/env python3
"""gl_no_context_test.py — a GL cart with no GL context is refused, not run.

With no display EGL fails to initialize and no GL entry point is resolved.
A GL cart that touches GL while loading, here in its wasm start function,
jumped through a null pointer and crashed the player. It must instead fail
the load with "is a GL cart but no GL context is available" and exit 1. A
2D cart must still run.

Run:  python3 test/gl_no_context_test.py build/wasmcart-run   (no DISPLAY/WAYLAND_DISPLAY)
"""

import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def section(sid, body):
    return bytes([sid]) + leb(len(body)) + body


def vec(items):
    return leb(len(items)) + b"".join(items)


def name(s):
    return leb(len(s)) + s.encode()


def cart(gl):
    """8x8. gl: imports gl.glCreateProgram and calls it from the start function."""
    info = struct.pack("<18I", 4, 8, 8, 0 if gl else 2048, 0, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 1 if gl else 0, 0)
    types = [b"\x60\x00\x01\x7f", b"\x60\x00\x00"]
    imports = [name("gl") + name("glCreateProgram") + b"\x00" + leb(0)] if gl else []
    n = len(imports)
    funcs = vec([leb(0), leb(1), leb(1)])  # wc_get_info, wc_render, start
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(n),
                   name("wc_render") + b"\x00" + leb(n + 1)])

    def body(code):
        b = b"\x00" + code + b"\x0b"
        return leb(len(b)) + b

    start_code = b"\x10\x00\x1a" if gl else b""  # call glCreateProgram, drop
    code = vec([body(b"\x41" + leb(1024)), body(b""), body(start_code)])
    data = vec([b"\x00\x41" + leb(1024) + b"\x0b" + leb(len(info)) + info])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, vec(types)) +
            (section(2, vec(imports)) if imports else b"") + section(3, funcs) +
            section(5, mem) + section(7, exports) + section(8, leb(n + 2)) +
            section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "glstart" if gl else "twod", "abi": 4}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    env = dict(os.environ)
    for k in ("DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER"):
        env.pop(k, None)
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    failures = []
    with tempfile.TemporaryDirectory() as d:
        for gl in (True, False):
            path = os.path.join(d, "c.wasc")
            open(path, "wb").write(cart(gl))
            p = subprocess.run(["timeout", "-s", "TERM", "3", sys.argv[1], path],
                               env=env, capture_output=True, text=True)
            if gl:
                ok = p.returncode == 1 and "no GL context is available" in p.stderr
                label = f"a GL cart touching GL in its start function is refused: exit {p.returncode}"
            else:
                ok = p.returncode == 124 and "running twod" in p.stderr
                label = f"a 2D cart still runs: exit {p.returncode}"
            print(f"  {'ok  ' if ok else 'FAIL'}  {label}")
            if not ok:
                failures.append(label)
                print("    " + "\n    ".join(p.stderr.splitlines()[-5:]))
    print("\nall checks passed" if not failures else "\nFAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
