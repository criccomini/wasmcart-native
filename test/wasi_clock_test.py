#!/usr/bin/env python3
"""wasi_clock_test.py — what a cart reads from WASI's clock_time_get.

Builds a cart that reads the four WASI clocks every frame into its save
region, runs it for a second and quits it with SIGTERM, so the runner writes
the last frame's readings to the save file. Then checks:
  REALTIME (0)            the wall clock, within a minute of this machine's;
  MONOTONIC (1), and the  counted from the cart's start, so no more than the
  CPU-time clocks (2, 3)  runner ran for, whatever the machine's uptime.

Counted from boot, they broke Emscripten carts on any machine up over 35.8
minutes: their clock() is a 32-bit count of microseconds, which musl turns
to -1 from 2^31 on.

Run:  python3 test/wasi_clock_test.py build/wasmcart-run
"""

import io
import json
import os
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

INFO, SAVE, FB = 1024, 2048, 4096


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


def clock_cart():
    # wc_info_t, ABI v4: an 8x8 framebuffer and a 32-byte save region.
    info = struct.pack("<18I", 4, 8, 8, FB, 0, 0, 0, 0, SAVE, 32, 0, 0, 0, 0, 0, 0, 0, 0)
    types = vec([b"\x60\x03\x7f\x7e\x7f\x01\x7f",  # clock_time_get(i32 id, i64 precision, i32 ptr) -> i32
                 b"\x60\x00\x01\x7f",              # wc_get_info() -> i32
                 b"\x60\x00\x00"])                 # wc_render()
    imports = vec([name("wasi_snapshot_preview1") + name("clock_time_get") + b"\x00" + leb(0)])
    funcs = vec([leb(1), leb(2)])
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(1),
                   name("wc_render") + b"\x00" + leb(2)])
    render = b"".join(b"\x41" + sleb(clock) + b"\x42" + sleb(0) + b"\x41" + sleb(SAVE + 8 * clock)
                      + b"\x10" + leb(0) + b"\x1a" for clock in range(4))

    def body(code):
        b = b"\x00" + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(b"\x41" + sleb(INFO)), body(render)])
    data = vec([b"\x00\x41" + sleb(INFO) + b"\x0b" + leb(len(info)) + info])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(2, imports) +
            section(3, funcs) + section(5, mem) + section(7, exports) +
            section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "clocks", "abi": 4}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    failures = []

    def check(what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail else ""))
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as d:
        cart = os.path.join(d, "clocks.wasc")
        save = cart + ".sav"  # the runner's default
        with open(cart, "wb") as f:
            f.write(clock_cart())
        env = dict(os.environ)
        env.setdefault("SDL_VIDEODRIVER", "offscreen")
        env.setdefault("SDL_RENDER_DRIVER", "software")
        env.setdefault("SDL_AUDIODRIVER", "dummy")
        started = time.monotonic()
        p = subprocess.Popen([binary, cart], env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        time.sleep(1.5)
        p.send_signal(signal.SIGTERM)
        try:
            _, err = p.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            _, err = p.communicate()
        lived = time.monotonic() - started
        wall = time.time()
        check("the runner quit cleanly", p.returncode == 0,
              "" if p.returncode == 0 else f"exit {p.returncode}: {err[-300:]}")
        try:
            with open(save, "rb") as f:
                real, mono, proc, thread = struct.unpack("<4Q", f.read())
        except (OSError, struct.error) as e:
            check("the save holds the readings", False, str(e))
            return 1
        check("REALTIME is the wall clock", abs(real / 1e9 - wall) < 60,
              f"{real / 1e9:.1f} s vs {wall:.1f} s")
        for label, v in (("MONOTONIC", mono), ("PROCESS_CPUTIME", proc), ("THREAD_CPUTIME", thread)):
            check(f"{label} counts from the cart's start", 0 < v / 1e9 <= lived,
                  f"{v / 1e9:.3f} s, the runner ran {lived:.3f} s")
    print("PASS" if not failures else f"FAIL: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
