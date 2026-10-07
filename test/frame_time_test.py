#!/usr/bin/env python3
"""frame_time_test.py — the delta_ms a cart is handed, with and without --fixed-step.

Builds a cart byte by byte (no toolchain) that stalls 400 ms inside its
10th wc_render (poll_oneoff) and keeps the largest, smallest and latest
delta_ms of wc_time_t, the latest time_ms and frame, in its save region,
which the runner writes on SIGTERM. Then checks:
  - on the wall clock, the stall reaches the cart as one delta of
    WC_MAX_DELTA_MS (250 ms), not 400;
  - with --fixed-step 1000 every frame's delta is exactly 1000 ms, stall or
    not: the harness's step is the cart's time, unclamped, and
    time_ms = frame * step.

Run:  python3 test/frame_time_test.py build/wasmcart-run
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

INFO, TIME, CNT, SAVE, FB, SUB, EV = 1024, 1536, 1600, 2048, 4096, 8192, 8256
STALL_NS = 400_000_000
MAX_DELTA_MS = 250.0  # WC_MAX_DELTA_MS


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


def i32(v):
    return b"\x41" + sleb(v)


M4, M8 = b"\x02\x00", b"\x03\x00"
F64_LOAD, F64_STORE, F64_MIN, F64_MAX = b"\x2b", b"\x39", b"\xa4", b"\xa5"
DELTA = i32(TIME + 8) + F64_LOAD + M8


def cart():
    # wc_info_t, ABI v4: an 8x8 framebuffer, a 40-byte save region, time_ptr.
    info = struct.pack("<18I", 4, 8, 8, FB, 0, 0, 0, 0, SAVE, 40, TIME, 0, 0, 0, 0, 0, 0, 0)
    # a relative MONOTONIC clock subscription of STALL_NS
    sub = bytearray(48)
    struct.pack_into("<I", sub, 16, 1)
    struct.pack_into("<Q", sub, 24, STALL_NS)
    types = vec([b"\x60\x04\x7f\x7f\x7f\x7f\x01\x7f",  # 0 poll_oneoff
                 b"\x60\x00\x01\x7f",                  # 1 wc_get_info
                 b"\x60\x00\x00"])                     # 2 wc_init, wc_render
    imports = vec([name("wasi_snapshot_preview1") + name("poll_oneoff") + b"\x00" + leb(0)])
    funcs = vec([leb(1), leb(2), leb(2)])
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(1),
                   name("wc_init") + b"\x00" + leb(2), name("wc_render") + b"\x00" + leb(3)])
    render = (
        i32(CNT) + i32(CNT) + b"\x28" + M4 + i32(1) + b"\x6a" + b"\x36" + M4 +           # cnt++
        i32(CNT) + b"\x28" + M4 + i32(10) + b"\x46" + b"\x04\x40" +                      # if cnt == 10
        i32(SUB) + i32(EV) + i32(1) + i32(EV + 32) + b"\x10" + leb(0) + b"\x1a" +        #   stall
        b"\x0b" +
        i32(SAVE) + i32(SAVE) + F64_LOAD + M8 + DELTA + F64_MAX + F64_STORE + M8 +       # max
        i32(SAVE + 8) + DELTA + F64_STORE + M8 +                                         # latest
        i32(SAVE + 16) + i32(TIME) + F64_LOAD + M8 + F64_STORE + M8 +                    # time_ms
        i32(SAVE + 24) + i32(SAVE + 24) + F64_LOAD + M8 + DELTA + F64_MIN + F64_STORE + M8 +  # min
        i32(SAVE + 32) + i32(TIME + 16) + b"\x28" + M4 + b"\x36" + M4)                   # frame

    def body(code):
        b = b"\x00" + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(i32(INFO)), body(b""), body(render)])
    segs = [(INFO, info), (SUB, bytes(sub)), (SAVE + 24, struct.pack("<d", 1e9))]
    data = vec([b"\x00" + i32(a) + b"\x0b" + leb(len(d)) + d for a, d in segs])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(2, imports) +
            section(3, funcs) + section(5, mem) + section(7, exports) + section(10, code) +
            section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "frametime", "abi": 4}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


def run(binary, path, args):
    save = path + ".sav"  # the runner's default
    if os.path.exists(save):
        os.unlink(save)
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    p = subprocess.Popen([binary, path, *args], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    time.sleep(2.0)
    p.send_signal(signal.SIGTERM)
    try:
        _, err = p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        _, err = p.communicate()
    try:
        with open(save, "rb") as f:
            hi, last, time_ms, lo, frame = struct.unpack("<4dI", f.read()[:36])
    except (OSError, struct.error):
        return p.returncode, err, None
    return p.returncode, err, (hi, last, time_ms, lo, frame)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    failures = []

    def check(what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail else ""))
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "frametime.wasc")
        with open(path, "wb") as f:
            f.write(cart())

        rc, err, r = run(binary, path, [])
        check("the wall-clock run quit cleanly with its save", rc == 0 and r is not None,
              f"exit {rc}: {err[-300:]}" if rc or r is None else "")
        if r:
            hi, last, time_ms, lo, frame = r
            check("a 400 ms stall reaches the cart as one delta of 250 ms", hi == MAX_DELTA_MS,
                  f"largest delta {hi:.3f} ms, over {frame} frames")
            check("and no delta is zero", lo > 0, f"smallest {lo:.3f} ms")

        rc, err, r = run(binary, path, ["--fixed-step", "1000"])
        check("the --fixed-step run quit cleanly with its save", rc == 0 and r is not None,
              f"exit {rc}: {err[-300:]}" if rc or r is None else "")
        if r:
            hi, last, time_ms, lo, frame = r
            check("--fixed-step 1000 hands every frame 1000 ms, the stall's too",
                  hi == lo == last == 1000.0 and frame > 10,
                  f"deltas {lo}-{hi} ms, latest {last}, {frame} frames")
            check("and time_ms is frame * 1000", time_ms == frame * 1000.0,
                  f"time_ms {time_ms} at frame {frame}")
    print("PASS" if not failures else f"FAIL: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
