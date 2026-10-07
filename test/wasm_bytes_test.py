#!/usr/bin/env python3
"""wasm_bytes_test.py — cart.wasm costs its size once, not twice.

The runner extracts cart.wasm into host->wasm_bytes and compiles it; V8
copies the bytes into the compiled module and keeps them for the module's
life. The runner's own copy used to stay until the archive closed, so the
module was in memory twice for the whole game. Now it's freed as soon as
the module is compiled.

Two carts, the same but for a custom section: 96 MiB of random bytes in
one, none in the other. Each fills 256 MiB of its memory in wc_init, so
its peak comes after the load, and logs DONE from its first frame. The big
one's peak RSS must be within 1.5 times the section of the small one's:
V8's copy, and not the runner's as well. The cart also has to run as
before: the section isn't read by anything.

Run:  python3 test/wasm_bytes_test.py build/wasmcart-run
"""

import io
import json
import os
import random
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

MiB = 1 << 20
PAGE = 65536
INFO, FB, STRS = 0x400, 0x800, 0x1000
FILL_AT, FILL = 1 * MiB, 256 * MiB
PAD = 96 * MiB


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
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
    return b"\x41" + sleb(v - (1 << 32) if v >= 1 << 31 else v)


def cart(pad):
    strs = [(STRS, b"INIT done"), (STRS + 16, b"DONE")]
    pages = (FILL_AT + FILL) // PAGE + 1
    types = [b"\x60\x00\x01\x7f", b"\x60\x00\x00", b"\x60\x02\x7f\x7f\x00"]
    imports = [name("env") + name("wc_log") + b"\x00" + leb(2)]
    funcs = vec([leb(0), leb(1), leb(1)])
    mem = vec([b"\x00" + leb(pages)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(1),
                   name("wc_init") + b"\x00" + leb(2), name("wc_render") + b"\x00" + leb(3)])
    # wc_init: memory.fill(FILL_AT, 0x5a, FILL), then log
    init = i32(FILL_AT) + i32(0x5A) + i32(FILL) + b"\xfc\x0b\x00" + i32(STRS) + i32(9) + b"\x10\x00"
    # wc_render: log DONE once (a flag word at STRS + 32)
    flag = STRS + 32
    render = (i32(flag) + b"\x28\x02\x00" + b"\x04\x40\x0f\x0b" +
              i32(flag) + i32(1) + b"\x36\x02\x00" + i32(STRS + 16) + i32(4) + b"\x10\x00")

    def body(code):
        b = b"\x00" + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(i32(INFO)), body(init), body(render)])
    info = struct.pack("<18I", 4, 8, 8, FB, *([0] * 14))
    data = vec([b"\x00" + i32(a) + b"\x0b" + leb(len(d)) + d for a, d in [(INFO, info)] + strs])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, vec(types)) + section(2, vec(imports)) +
            section(3, funcs) + section(5, mem) + section(7, exports) + section(10, code) +
            section(11, data))
    if pad:
        wasm += section(0, name("wasmcart.test.pad") + random.Random(5).randbytes(pad))
    return wasm


def run(binary, path, wait=60.0):
    """Run until DONE, then SIGTERM. Returns (exit code, stderr, peak RSS MiB)."""
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    p = subprocess.Popen([binary, path, "--save", path + ".sav"], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    os.set_blocking(p.stderr.fileno(), False)
    buf = b""
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        chunk = p.stderr.read()
        if chunk:
            buf += chunk
        if b"[cart]: DONE" in buf or p.poll() is not None:
            break
        time.sleep(0.02)
    if p.poll() is None:
        p.send_signal(signal.SIGTERM)
    os.set_blocking(p.stderr.fileno(), True)
    buf += p.stderr.read()
    _, status, ru = os.wait4(p.pid, 0)
    return os.waitstatus_to_exitcode(status), buf.decode("utf-8", "replace"), ru.ru_maxrss // 1024


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    failures = []

    def check(what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail and not ok else ""))
        if not ok:
            failures.append(what)

    peaks = {}
    with tempfile.TemporaryDirectory() as d:
        for label, pad in (("small", 0), ("big", PAD)):
            path = os.path.join(d, f"{label}.wasc")
            with zipfile.ZipFile(path, "w") as z:
                z.writestr("manifest.json", json.dumps({"name": f"wasmbytes-{label}", "abi": 4}))
                z.writestr("cart.wasm", cart(pad), zipfile.ZIP_STORED)
            rc, err, peak = run(binary, path)
            ok = "[cart]: INIT done" in err and "[cart]: DONE" in err and "trapped" not in err
            check(f"the {label} cart loads and runs", ok, f"exit {rc}: {err.strip()[-400:]}")
            peaks[label] = peak
    extra = peaks["big"] - peaks["small"]
    limit = PAD * 3 // 2 // MiB
    check(f"a {PAD // MiB} MiB cart.wasm costs about its size once (+{extra} MiB, limit {limit})",
          extra < limit, f"peaks {peaks}")
    print(f"  peak RSS, MiB: {peaks}")
    print("PASS" if not failures else f"FAIL: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
