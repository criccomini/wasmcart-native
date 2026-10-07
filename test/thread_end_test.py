#!/usr/bin/env python3
"""thread_end_test.py — what a threaded cart's worker threads can end, and save.

Builds carts byte by byte (no toolchain) that import a shared memory and
wasi.thread-spawn, as wasm32-wasip1-threads carts do. wc_init spawns one
thread, and the thread does what the case says.

Bad pointers. The thread writes a marker into the save region, then hands
an import a range running past the end of the cart's memory. As on the main
thread (import_bounds_test.py), that must trap with a RangeError naming the
import, before the import has done anything (fd_write prints nothing). And
a trap on any thread ends the cart: the runner stops by itself with exit 1
and writes no save, since the thread may have died halfway through it.
proc_exit on a thread ends the cart the same way.

A control thread makes the same calls with ranges ending exactly at the end
of memory, writes the marker and returns: the cart runs on, quits cleanly on
SIGTERM, and saves the marker.

A thread that never stops rewriting the save region: the runner takes the
region again while it keeps changing, says so once, and still saves.

Run:  python3 test/thread_end_test.py build/wasmcart-run
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

PAGES = 2
END = PAGES * 65536  # one past the last byte of memory
INFO, SAVE, FB = 1024, 2048, 4096
SCRATCH = 8192
TEXT = 12288
MARKER = 0x1111111111111111

I32, I64 = 0x7F, 0x7E


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
    """i32.const, taking the u32 spelling of a pointer."""
    return b"\x41" + sleb(v - (1 << 32) if v >= 1 << 31 else v)


def i64(v):
    return b"\x42" + sleb(v)


MEMARG8 = b"\x03\x00"
MEMARG4 = b"\x02\x00"

WASI = "wasi_snapshot_preview1"
# field -> (module, params, results)
SIGS = {
    "wc_log": ("env", [I32] * 2, []),
    "fd_write": (WASI, [I32] * 4, [I32]),
    "fd_read": (WASI, [I32] * 4, [I32]),
    "fd_fdstat_get": (WASI, [I32] * 2, [I32]),
    "fd_filestat_get": (WASI, [I32] * 2, [I32]),
    "environ_sizes_get": (WASI, [I32] * 2, [I32]),
    "args_sizes_get": (WASI, [I32] * 2, [I32]),
    "clock_time_get": (WASI, [I32, I64, I32], [I32]),
    "clock_res_get": (WASI, [I32] * 2, [I32]),
    "random_get": (WASI, [I32] * 2, [I32]),
    "poll_oneoff": (WASI, [I32] * 4, [I32]),
    "proc_exit": (WASI, [I32], []),
}


class ThreadCart:
    """A shared imported memory, wasi.thread-spawn, wc_init spawning one
    thread, and the thread's code."""

    def __init__(self, save=SAVE, save_size=32):
        self.imports = []
        self.data = []
        self.next_text = TEXT
        self.thread = b""
        self.save, self.save_size = save, save_size

    def call(self, field, *args):
        mod, params, results = SIGS[field]
        key = (mod, field)
        if key not in [im[:2] for im in self.imports]:
            self.imports.append((mod, field, params, results))
        idx = 1 + [im[:2] for im in self.imports].index(key)  # 0 is thread-spawn
        code = b""
        for p, a in zip(params, args):
            code += i64(a) if p == I64 else i32(a)
        code += b"\x10" + leb(idx)
        if results:
            code += b"\x1a"  # drop
        return code

    def put(self, addr, data):
        self.data.append((addr, data))

    def text(self, s):
        b = s.encode()
        addr = self.next_text
        self.next_text += len(b)
        self.put(addr, b)
        return addr, len(b)

    def log(self, s):
        return self.call("wc_log", *self.text(s))

    def wasm(self):
        # wc_info_t, ABI v4: an 8x8 framebuffer and the save region.
        info = struct.pack("<18I", 4, 8, 8, FB, 0, 0, 0, 0, self.save, self.save_size,
                           0, 0, 0, 0, 0, 0, 0, 0)
        types = []

        def typeidx(params, results):
            t = b"\x60" + vec([bytes([p]) for p in params]) + vec([bytes([r]) for r in results])
            if t not in types:
                types.append(t)
            return types.index(t)

        imports = [name("wasi") + name("thread-spawn") + b"\x00" + leb(typeidx([I32], [I32]))]
        imports += [name(m) + name(f) + b"\x00" + leb(typeidx(p, r)) for m, f, p, r in self.imports]
        # a shared memory, PAGES pages, max PAGES: flags 0x03 (has max, shared)
        imports.append(name("env") + name("memory") + b"\x02" + b"\x03" + leb(PAGES) + leb(PAGES))
        n = 1 + len(self.imports)
        t_info, t_void, t_start = typeidx([], [I32]), typeidx([], []), typeidx([I32, I32], [])
        funcs = vec([leb(t_info), leb(t_void), leb(t_void), leb(t_start)])
        exports = vec([name("wc_get_info") + b"\x00" + leb(n), name("wc_init") + b"\x00" + leb(n + 1),
                       name("wc_render") + b"\x00" + leb(n + 2),
                       name("wasi_thread_start") + b"\x00" + leb(n + 3)])

        def body(code, locals_=b"\x00"):
            b = locals_ + code + b"\x0b"
            return leb(len(b)) + b

        spawn = i32(1) + b"\x10" + leb(0) + b"\x1a"
        # the thread's function has one i32 local (2) after tid and arg
        code = vec([body(i32(INFO)), body(spawn), body(b""), body(self.thread, b"\x01\x01\x7f")])
        # Active segments: each thread's instantiation writes them again, so
        # none of them covers the save region or anything a thread changes.
        segs = [(INFO, info)] + self.data
        data = vec([b"\x00" + i32(a) + b"\x0b" + leb(len(d)) + d for a, d in segs])
        return (b"\x00asm\x01\x00\x00\x00" + section(1, vec(types)) + section(2, vec(imports)) +
                section(3, funcs) + section(7, exports) + section(10, code) + section(11, data))

    def wasc(self):
        z = io.BytesIO()
        with zipfile.ZipFile(z, "w") as f:
            f.writestr("manifest.json", json.dumps({"name": "threadend", "abi": 4}))
            f.writestr("cart.wasm", self.wasm())
        return z.getvalue()


def marker():
    return i32(SAVE) + i64(MARKER) + b"\x37" + MEMARG8


def clock_sub(c):
    """A relative MONOTONIC subscription of 0 ns at SCRATCH, as data."""
    sub = bytearray(48)
    sub[8] = 0                               # tag: clock
    struct.pack_into("<I", sub, 16, 1)       # MONOTONIC
    c.put(SCRATCH, bytes(sub))
    return SCRATCH


def fd_write_bad_nwritten(c):
    buf, n = c.text("PRINTED BEFORE THE TRAP\n")
    c.put(SCRATCH + 64, struct.pack("<2I", buf, n))
    return c.call("fd_write", 1, SCRATCH + 64, 1, END - 2)


BAD = [
    ("wc_log", lambda c: c.call("wc_log", END - 3, 4)),
    ("fd_write", fd_write_bad_nwritten),
    ("fd_read", lambda c: c.call("fd_read", 0, SCRATCH + 64, 0, END - 2)),
    ("fd_fdstat_get", lambda c: c.call("fd_fdstat_get", 1, END - 10)),
    ("fd_filestat_get", lambda c: c.call("fd_filestat_get", 1, END - 10)),
    ("environ_sizes_get", lambda c: c.call("environ_sizes_get", SCRATCH + 64, END - 2)),
    ("args_sizes_get", lambda c: c.call("args_sizes_get", END - 2, SCRATCH + 64)),
    ("clock_time_get", lambda c: c.call("clock_time_get", 1, 0, END - 4)),
    ("clock_res_get", lambda c: c.call("clock_res_get", 1, END - 4)),
    ("random_get", lambda c: c.call("random_get", END - 8, 16)),
    ("poll_oneoff", lambda c: c.call("poll_oneoff", clock_sub(c), END - 10, 1, SCRATCH + 128)),
]


def bad_cart(make):
    c = ThreadCart()
    c.thread = marker() + make(c) + c.log("after the bad call")
    return c


def proc_exit_cart():
    c = ThreadCart()
    c.thread = marker() + c.call("proc_exit", 0) + c.log("after the bad call")
    return c


def control_cart():
    """Every call above with its range ending exactly at END."""
    c = ThreadCart()
    buf, n = c.text("CONTROL WROTE THIS\n")
    c.put(END - 12, struct.pack("<2I", buf, n))  # the iovec, nwritten after it
    c.thread = (c.call("fd_write", 1, END - 12, 1, END - 4) +
                c.call("wc_log", END - 4, 4) +
                c.call("fd_read", 0, SCRATCH + 64, 0, END - 4) +
                c.call("fd_fdstat_get", 1, END - 24) +
                c.call("fd_filestat_get", 1, END - 64) +
                c.call("environ_sizes_get", END - 8, END - 4) +
                c.call("args_sizes_get", END - 8, END - 4) +
                c.call("clock_time_get", 1, 0, END - 8) +
                c.call("clock_res_get", 1, END - 8) +
                c.call("random_get", END - 16, 16) +
                c.call("poll_oneoff", clock_sub(c), END - 36, 1, END - 4) +
                marker() + c.log("control done"))
    return c


CHURN_SAVE, CHURN_SIZE = 32768, 16384


def churn_cart():
    """loop { the save region's first and last words = i; i++ }, forever.
    The region is big enough that copying it and comparing the copy take
    a few microseconds, so the thread changes it between the two every time."""
    c = ThreadCart(CHURN_SAVE, CHURN_SIZE)
    stores = b"".join(i32(a) + b"\x20\x02" + b"\x36" + MEMARG4
                      for a in (CHURN_SAVE, CHURN_SAVE + CHURN_SIZE - 4))
    c.thread = (b"\x03\x40" + stores +
                b"\x20\x02" + i32(1) + b"\x6a" + b"\x21\x02" +   # i++
                b"\x0c\x00" + b"\x0b")
    return c


def run(binary, wasc, d, args=(), quit_after=None, wait=8.0):
    """Run the cart; SIGTERM it after quit_after seconds, or wait for it to
    stop by itself. Returns (exit code or None if it had to be killed,
    stdout, stderr, whether the save file exists, its bytes)."""
    path = os.path.join(d, "threadend.wasc")
    save = path + ".sav"  # the runner's default
    for p in (path, save):
        if os.path.exists(p):
            os.unlink(p)
    with open(path, "wb") as f:
        f.write(wasc)
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    p = subprocess.Popen([binary, path, *args], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                         errors="replace")
    try:
        if quit_after is not None:
            time.sleep(quit_after)
            p.send_signal(signal.SIGTERM)
        out, err = p.communicate(timeout=wait)
        rc = p.returncode
    except subprocess.TimeoutExpired:
        p.send_signal(signal.SIGTERM)  # it ran on: quit it, so a save shows
        try:
            out, err = p.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            out, err = p.communicate()
        rc = None
    data = None
    if os.path.exists(save):
        with open(save, "rb") as f:
            data = f.read()
    return rc, out, err, data


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

    with tempfile.TemporaryDirectory() as d:
        cases = [(imp, bad_cart(make)) for imp, make in BAD]
        cases.append(("proc_exit", proc_exit_cart()))
        for imp, cart in cases:
            rc, out, err, save = run(binary, cart.wasc(), d)
            if imp == "proc_exit":
                said = "called proc_exit(0)" in err
                why = "the thread's proc_exit"
            else:
                said = any("thread trapped" in l and f"RangeError: {imp}:" in l for l in err.splitlines())
                why = f"a bad pointer to {imp} on a thread"
            detail = f"exit {rc}, save {'written' if save else 'none'}: {err.strip()[-300:]}"
            check(f"{why} ends the cart", said and "which ends the cart" in err and rc == 1, detail)
            check(f"  and nothing after it runs, and no save", save is None and "after the bad call" not in err,
                  detail)
            if imp == "fd_write":
                check("  and fd_write printed nothing first", "PRINTED BEFORE THE TRAP" not in out, out[-200:])

        rc, out, err, save = run(binary, control_cart().wasc(), d, quit_after=2.0)
        ok = (rc == 0 and "control done" in err and "CONTROL WROTE THIS" in out and
              "trapped" not in err and save is not None and save[:8] == struct.pack("<Q", MARKER))
        check("ranges ending exactly at the end of memory are fine on a thread", ok,
              f"exit {rc}, save {save[:8].hex() if save else None}: {err.strip()[-300:]}")

        rc, out, err, save = run(binary, churn_cart().wasc(), d, args=("--save-every", "1"),
                                 quit_after=2.5)
        warned = err.count("the save region kept changing")
        check("a save region a thread never stops rewriting is still saved, with one warning",
              rc == 0 and warned == 1 and save is not None and len(save) == CHURN_SIZE,
              f"exit {rc}, {warned} warnings, save {len(save) if save else None} bytes: {err.strip()[-300:]}")
    print("PASS" if not failures else f"FAIL: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
