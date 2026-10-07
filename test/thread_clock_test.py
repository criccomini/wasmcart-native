#!/usr/bin/env python3
"""thread_clock_test.py — a threaded cart's clocks, off the main thread.

Builds carts byte by byte (no toolchain) that import a shared memory and
wasi.thread-spawn, as wasm32-wasip1-threads carts do.

The first spawns a thread from wc_init that reads REALTIME and MONOTONIC
into the save region, then sleeps 100 ms through poll_oneoff with an
ABSOLUTE MONOTONIC deadline it computed from clock_time_get, and records
how long that took. The main thread does the same 100 ms absolute sleep in
its first wc_render. After a second and a half the runner is stopped with
SIGTERM, so it writes the save region to the save file. Then checks:
  - the thread's REALTIME is the wall clock, and its MONOTONIC counts from
    the cart's start, as the main thread's do (wasi_clock_test.py), not
    from the machine's boot;
  - an absolute deadline sleeps until it, on a thread and on the main
    thread: it is measured on the clock the cart read it from.

The second has the main thread (every frame) and a thread (every 20 ms)
write REALTIME to the save region while an LD_PRELOAD shim steps the wall
clock a day forward partway through, as a network time sync does on a Pi
with no clock battery; MONOTONIC doesn't move. Both threads must follow the
step. Linux only (LD_PRELOAD); needs a C compiler ($CC, or cc).

Bad pointers and traps on a thread: thread_end_test.py.

Run:  python3 test/thread_clock_test.py build/wasmcart-run
"""

import io
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

INFO, SAVE, FB = 1024, 2048, 4096
W_SCRATCH, M_SCRATCH = 8192, 12288  # each: t0 (8), t1 (8), sub (48), event (32), nevents (4)
DONE = 2560                          # the main thread's one-shot flag
SLEEP_NS = 100_000_000


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
    return b"\x41" + sleb(v - (1 << 32) if v >= 1 << 31 else v)


def i64(v):
    return b"\x42" + sleb(v)


MEMARG8 = b"\x03\x00"  # align 8, offset 0
MEMARG4 = b"\x02\x00"
CLOCK, POLL, SPAWN, LOG = 0, 1, 2, 3  # imported functions


def clock_into(clock, addr):
    return i32(clock) + i64(0) + i32(addr) + b"\x10" + leb(CLOCK) + b"\x1a"


def abs_sleep(scratch, out):
    """t0 = MONOTONIC; poll_oneoff until t0 + SLEEP_NS (absolute); out = MONOTONIC - t0."""
    t0, t1, sub, ev, nev = scratch, scratch + 8, scratch + 16, scratch + 64, scratch + 96
    code = clock_into(1, t0)
    code += i32(sub + 8) + i32(0) + b"\x36" + MEMARG4         # tag 0: clock (and padding)
    code += i32(sub + 16) + i32(1) + b"\x36" + MEMARG4        # clock id: MONOTONIC
    code += (i32(sub + 24) + i32(t0) + b"\x29" + MEMARG8 + i64(SLEEP_NS) + b"\x7c"
             + b"\x37" + MEMARG8)                             # timeout = t0 + 100 ms
    code += i32(sub + 40) + i32(1) + b"\x36" + MEMARG4        # flags: abstime
    code += i32(sub) + i32(ev) + i32(1) + i32(nev) + b"\x10" + leb(POLL) + b"\x1a"
    code += clock_into(1, t1)
    code += (i32(out) + i32(t1) + b"\x29" + MEMARG8 + i32(t0) + b"\x29" + MEMARG8 + b"\x7d"
             + b"\x37" + MEMARG8)                             # out = t1 - t0
    return code


def thread_cart(stepping=False):
    """The clocks cart, or with stepping the REALTIME-step one."""
    # wc_info_t, ABI v4: an 8x8 framebuffer and a 32-byte save region:
    # thread REALTIME, thread MONOTONIC, thread sleep, main sleep (ns, u64 each).
    # Stepping: main REALTIME, thread REALTIME.
    info = struct.pack("<18I", 4, 8, 8, FB, 0, 0, 0, 0, SAVE, 32, 0, 0, 0, 0, 0, 0, 0, 0)
    types = vec([b"\x60\x03\x7f\x7e\x7f\x01\x7f",      # 0 clock_time_get(i32, i64, i32) -> i32
                 b"\x60\x04\x7f\x7f\x7f\x7f\x01\x7f",  # 1 poll_oneoff(i32 x4) -> i32
                 b"\x60\x01\x7f\x01\x7f",              # 2 thread-spawn(i32) -> i32
                 b"\x60\x02\x7f\x7f\x00",              # 3 wc_log(i32, i32); wasi_thread_start(i32, i32)
                 b"\x60\x00\x01\x7f",                  # 4 wc_get_info() -> i32
                 b"\x60\x00\x00"])                     # 5 wc_init(), wc_render()
    imports = vec([
        name("wasi_snapshot_preview1") + name("clock_time_get") + b"\x00" + leb(0),
        name("wasi_snapshot_preview1") + name("poll_oneoff") + b"\x00" + leb(1),
        name("wasi") + name("thread-spawn") + b"\x00" + leb(2),
        name("env") + name("wc_log") + b"\x00" + leb(3),
        # a shared memory, 2 pages, max 2: flags 0x03 (has max, shared)
        name("env") + name("memory") + b"\x02" + b"\x03" + leb(2) + leb(2),
    ])
    # 4 wc_get_info, 5 wc_init, 6 wc_render, 7 wasi_thread_start
    funcs = vec([leb(4), leb(5), leb(5), leb(3)])
    exports = vec([name("wc_get_info") + b"\x00" + leb(4), name("wc_init") + b"\x00" + leb(5),
                   name("wc_render") + b"\x00" + leb(6),
                   name("wasi_thread_start") + b"\x00" + leb(7)])

    def body(code, locals_=b"\x00"):
        b = locals_ + code + b"\x0b"
        return leb(len(b)) + b

    get_info = i32(INFO)
    init = i32(1) + b"\x10" + leb(SPAWN) + b"\x1a"
    if stepping:
        render = clock_into(0, SAVE)
        # loop { REALTIME -> SAVE + 8; sleep 20 ms (relative) }
        sub, ev, nev = W_SCRATCH + 16, W_SCRATCH + 64, W_SCRATCH + 96
        start = (b"\x03\x40" + clock_into(0, SAVE + 8) +
                 i32(sub + 8) + i32(0) + b"\x36" + MEMARG4 +
                 i32(sub + 16) + i32(1) + b"\x36" + MEMARG4 +
                 i32(sub + 24) + i64(20_000_000) + b"\x37" + MEMARG8 +
                 i32(sub + 40) + i32(0) + b"\x36" + MEMARG4 +
                 i32(sub) + i32(ev) + i32(1) + i32(nev) + b"\x10" + leb(POLL) + b"\x1a" +
                 b"\x0c\x00" + b"\x0b")
    else:
        # wc_render: the first time only, the absolute sleep on the main thread
        render = (i32(DONE) + b"\x28" + MEMARG4 + b"\x45" + b"\x04\x40" +   # if (!done)
                  i32(DONE) + i32(1) + b"\x36" + MEMARG4 +
                  abs_sleep(M_SCRATCH, SAVE + 24) + b"\x0b")
        # wasi_thread_start(tid, arg): the clocks, then the absolute sleep
        start = (clock_into(0, SAVE) + clock_into(1, SAVE + 8) +
                 abs_sleep(W_SCRATCH, SAVE + 16))
    code = vec([body(get_info), body(init), body(render), body(start)])
    # Active: each thread's instantiation writes the same bytes again, which
    # changes nothing (a toolchain would make it passive and init it once).
    data = vec([b"\x00" + i32(INFO) + b"\x0b" + leb(len(info)) + info])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(2, imports) +
            section(3, funcs) + section(7, exports) + section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "threadclocks", "abi": 4}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


STEP_SHIM = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* The wall clock jumps a day forward once $STEP_FILE exists. MONOTONIC
   doesn't, as when a network time sync steps the clock. */
static long step(void) {
    const char* f = getenv("STEP_FILE");
    return f && access(f, F_OK) == 0 ? 86400 : 0;
}
int clock_gettime(clockid_t id, struct timespec* ts) {
    static int (*real)(clockid_t, struct timespec*);
    if (!real) real = (int (*)(clockid_t, struct timespec*))dlsym(RTLD_NEXT, "clock_gettime");
    int r = real(id, ts);
    if (r == 0 && (id == CLOCK_REALTIME || id == CLOCK_REALTIME_COARSE)) ts->tv_sec += step();
    return r;
}
int gettimeofday(struct timeval* tv, void* tz) {
    struct timespec ts;
    (void)tz;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (tv) { tv->tv_sec = ts.tv_sec; tv->tv_usec = ts.tv_nsec / 1000; }
    return 0;
}
time_t time(time_t* t) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (t) *t = ts.tv_sec;
    return ts.tv_sec;
}
int timespec_get(struct timespec* ts, int base) {
    if (base != TIME_UTC) return 0;
    clock_gettime(CLOCK_REALTIME, ts);
    return TIME_UTC;
}
"""


def run(binary, cart, before_term, env_extra=None, between=None):
    env = dict(os.environ, **(env_extra or {}))
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    started = time.monotonic()
    p = subprocess.Popen([binary, cart], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    time.sleep(before_term)
    if between:
        between()
        time.sleep(1.0)
    p.send_signal(signal.SIGTERM)
    try:
        _, err = p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        _, err = p.communicate()
    return p.returncode, err, time.monotonic() - started


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

    def read_save(path, n):
        try:
            with open(path, "rb") as f:
                return struct.unpack(f"<{n}Q", f.read()[:8 * n])
        except (OSError, struct.error) as e:
            check("the save holds the readings", False, str(e))
            return None

    with tempfile.TemporaryDirectory() as d:
        cart = os.path.join(d, "threadclocks.wasc")
        save = cart + ".sav"  # the runner's default
        with open(cart, "wb") as f:
            f.write(thread_cart())
        rc, err, lived = run(binary, cart, 1.5)
        wall = time.time()
        check("the cart ran threaded", "threads run as node workers" in err,
              "" if "threads run as node workers" in err else err[-400:])
        check("the runner quit cleanly", rc == 0, "" if rc == 0 else f"exit {rc}: {err[-400:]}")
        readings = read_save(save, 4)
        if readings:
            real, mono, t_sleep, m_sleep = readings
            check("a thread's REALTIME is the wall clock", abs(real / 1e9 - wall) < 60,
                  f"{real / 1e9:.1f} s vs {wall:.1f} s")
            check("a thread's MONOTONIC counts from the cart's start", 0 < mono / 1e9 <= lived,
                  f"{mono / 1e9:.3f} s, the runner ran {lived:.3f} s")
            for where, v in (("a thread", t_sleep), ("the main thread", m_sleep)):
                check(f"an absolute MONOTONIC deadline sleeps until it on {where}",
                      0.09 <= v / 1e9 < 1.0, f"{v / 1e6:.1f} ms for a 100 ms deadline")

        # The wall clock stepped under a running cart.
        src, shim = os.path.join(d, "step.c"), os.path.join(d, "step.so")
        with open(src, "w") as f:
            f.write(STEP_SHIM)
        cc = os.environ.get("CC") or shutil.which("cc") or "gcc"
        subprocess.run([cc, "-shared", "-fPIC", "-o", shim, src, "-ldl"], check=True)
        cart = os.path.join(d, "realtimestep.wasc")
        save = cart + ".sav"
        with open(cart, "wb") as f:
            f.write(thread_cart(stepping=True))
        flag = os.path.join(d, "step")
        rc, err, _ = run(binary, cart, 1.5, {"LD_PRELOAD": shim, "STEP_FILE": flag},
                         between=lambda: open(flag, "w").close())
        wall = time.time() + 86400
        check("the runner quit cleanly after the step", rc == 0,
              "" if rc == 0 else f"exit {rc}: {err[-400:]}")
        readings = read_save(save, 2)
        if readings:
            main_rt, thread_rt = (v / 1e9 for v in readings)
            check("the main thread's REALTIME follows a step of the wall clock",
                  abs(main_rt - wall) < 60, f"{main_rt - wall:+.1f} s from the stepped clock")
            check("a thread's REALTIME follows it too, and agrees with the main thread's",
                  abs(thread_rt - main_rt) < 1.0, f"thread minus main {thread_rt - main_rt:+.3f} s")
    print("PASS" if not failures else f"FAIL: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
