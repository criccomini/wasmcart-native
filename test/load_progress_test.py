#!/usr/bin/env python3
"""load_progress_test.py — a long wc_load_asset is chunked and reports progress.

wc_load_asset used to extract a whole asset in one call, and the cart's
thread sent nothing until it returned: about 4 s for a stored 256 MiB asset
from a Pi's SD card, cold, which a supervisor watching for frames takes for
a hang. Now it goes in chunks, and with COUCHMIX_HEARTBEAT_FD set the runner
writes "A <load> <us> <done> <total>" between them, at most every 250 ms,
plus one when a load that sent any has all its bytes in.

Each cart loads one asset at frame 20 of wc_render (mid-game, not in
wc_init): first with room for one byte less (refused: -1), then with room
for it all, and checks the return, the first word and the last. It logs
"load ok" or "load BAD". A preloaded shim (built here with cc) slows fread
to a cold card's rate, so a load of tens of MiB takes a second or two, or
stops it for good part way.

Checks:
  stored     96 MiB stored at 64 MB/s: A lines between the frames before and
             after, done rising to total, 250 ms apart, the last at total
  deflated   the same, deflated
  many       120 loads of 1 MiB in one frame: A lines go out across loads,
             their load numbers rising
  crc        a stored entry with a byte changed, and a deflated one with the
             wrong CRC: -1, "CRC mismatch", the A lines as before
  home       SIGUSR1 mid-load: the load runs on to its end, then S, then P
             and no F until SIGUSR2, then R and F
  quit       SIGTERM mid-load: the load finishes, then the save (W), exit 0
  stall      reads that stop part way: the A lines stop too, and nothing
             else goes out (the supervisor's hang)
  no fd      without COUCHMIX_HEARTBEAT_FD the load is the same, unreported
  init       the stored load in wc_init instead: A lines before the "I" line
             and the first F, done rising to total

Run:  python3 test/load_progress_test.py build/wasmcart-run    (Linux, cc)
"""

import os
import random
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

INFO, AWRITE, PADS, TIME, HOST, PTRS, KEYS = 0x400, 0x500, 0x600, 0x700, 0x740, 0x780, 0x800
SAVE, NAME, OK, BAD = 0x900, 0xA00, 0xA80, 0xAA0
AUDIO, FB, DEST = 0x2000, 0x10000, 0x100000
LOAD_FRAME = 20
MiB = 1 << 20
RATE = 64_000_000  # bytes/s: a stored asset from the Pi's card, cold

SHIM = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
/* fread at SLOWREAD_BPS for reads of 4 KiB or more; with
 * SLOWREAD_STALL_AFTER, reads of 1 MiB or more never return once that many
 * bytes have gone through them. */
static double rate;
static long long stall_after = -1, big_total;
static size_t (*real_fread)(void*, size_t, size_t, FILE*);
__attribute__((constructor)) static void init(void) {
    real_fread = (size_t (*)(void*, size_t, size_t, FILE*))dlsym(RTLD_NEXT, "fread");
    const char* r = getenv("SLOWREAD_BPS");
    const char* s = getenv("SLOWREAD_STALL_AFTER");
    rate = r ? atof(r) : 0;
    stall_after = s ? atoll(s) : -1;
}
size_t fread(void* p, size_t size, size_t n, FILE* f) {
    size_t bytes = size * n;
    if (bytes >= (1u << 20) && stall_after >= 0) {
        if (big_total + (long long)bytes > stall_after) for (;;) pause();
        big_total += bytes;
    }
    if (rate > 0 && bytes >= 4096) {
        double s = bytes / rate;
        struct timespec ts = {(time_t)s, (long)((s - (time_t)s) * 1e9)};
        while (nanosleep(&ts, &ts) != 0) {}
    }
    return real_fread(p, size, n, f);
}
'''


# ── a cart, by hand ─────────────────────────────────────────────────────

def uleb(n):
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


def vec(items):
    return uleb(len(items)) + b"".join(items)


def name(s):
    return uleb(len(s)) + s.encode()


def section(sid, body):
    return bytes([sid]) + uleb(len(body)) + body


def const(n):
    return b"\x41" + sleb(n if n < 1 << 31 else n - (1 << 32))


def call(i):
    return b"\x10" + uleb(i)


def load32(addr):
    return const(addr) + b"\x28\x00\x00"  # i32.load align=1 offset=0


EQ, AND, ADD, LT_U = b"\x46", b"\x71", b"\x6a", b"\x49"
IF, ELSE, END, LOOP, BR_IF = b"\x04\x40", b"\x05", b"\x0b", b"\x03\x40", b"\x0d"
GET, SET, TEE = b"\x20", b"\x21", b"\x22"
I, OKL = 0, 1  # wc_render's locals: the loop count, and all-ok so far


def cart_wasm(asset, size, count, w0, w1, at_init=False):
    """An ABI 4, 64x64 2D cart with a 16-byte save. Imports wc_load_asset (0)
    and wc_log (1); defines wc_get_info (2), wc_init (3), wc_render (4).
    At frame LOAD_FRAME, wc_render loads asset into DEST with room for one
    byte less, then count times with room for all of it, and checks the
    returns (-1, then size) and the words at DEST and DEST + size - 4.
    at_init: wc_init does that instead, and wc_render nothing."""
    i32 = 0x7F
    types = vec([b"\x60\x00\x01" + bytes([i32]), b"\x60\x00\x00",
                 b"\x60\x04" + bytes([i32] * 4) + b"\x01" + bytes([i32]),
                 b"\x60\x02" + bytes([i32] * 2) + b"\x00"])
    imports = vec([name("env") + name("wc_load_asset") + b"\x00" + uleb(2),
                   name("env") + name("wc_log") + b"\x00" + uleb(3)])
    funcs = vec([uleb(0), uleb(1), uleb(1)])
    pages = (DEST + size + 0xFFFF) // 0x10000 + 1
    mem = vec([b"\x00" + uleb(pages)])
    exports = vec([name("memory") + b"\x02" + uleb(0), name("wc_get_info") + b"\x00" + uleb(2),
                   name("wc_init") + b"\x00" + uleb(3), name("wc_render") + b"\x00" + uleb(4)])
    load = lambda room: const(NAME) + const(len(asset)) + const(DEST) + const(room) + call(0)
    loads = (load(size - 1) + const(-1) + EQ + SET + uleb(OKL) +
              const(0) + SET + uleb(I) +
              LOOP +
              GET + uleb(OKL) + load(size) + const(size) + EQ + AND + SET + uleb(OKL) +
              GET + uleb(I) + const(1) + ADD + TEE + uleb(I) + const(count) + LT_U + BR_IF + uleb(0) +
              END +
              GET + uleb(OKL) + load32(DEST) + const(w0) + EQ + AND +
              load32(DEST + size - 4) + const(w1) + EQ + AND +
              IF + const(OK) + const(7) + call(1) + ELSE + const(BAD) + const(8) + call(1) + END)
    render = load32(TIME + 16) + const(LOAD_FRAME) + EQ + IF + loads + END

    def body(code, locals_=b"\x00"):
        b = locals_ + code + END
        return uleb(len(b)) + b

    if at_init:
        code = vec([body(const(INFO)), body(loads, b"\x01\x02\x7f"), body(b"")])
    else:
        code = vec([body(const(INFO)), body(b""), body(render, b"\x01\x02\x7f")])
    img = bytearray(0xB00 - INFO)
    struct.pack_into("<18I", img, 0, 4, 64, 64, FB, AUDIO, 1024, AWRITE, PADS, SAVE, 16, TIME, HOST, 0,
                     48000, PTRS, KEYS, 0, 0)
    for addr, s in ((SAVE, b"load test save!!"), (NAME, asset.encode()), (OK, b"load ok"),
                    (BAD, b"load BAD")):
        img[addr - INFO:addr - INFO + len(s)] = s
    data = vec([b"\x00" + const(INFO) + END + uleb(len(img)) + bytes(img)])
    return (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(2, imports) + section(3, funcs) +
            section(5, mem) + section(7, exports) + section(10, code) + section(11, data))


def low_entropy(n, seed):
    """Bytes from a 16-symbol alphabet: deflate gets them to about half."""
    table = bytes((i % 16) * 7 + 33 for i in range(256))
    return random.Random(seed).randbytes(n).translate(table)


def write_cart(path, asset, data, method, count=1, at_init=False):
    w0, w1 = struct.unpack_from("<I", data, 0)[0], struct.unpack_from("<I", data, len(data) - 4)[0]
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("manifest.json", '{"name": "load progress", "abi": 4}')
        z.writestr("cart.wasm", cart_wasm(asset, len(data), count, w0, w1, at_init))
        z.writestr(zipfile.ZipInfo(asset), data, compress_type=method, compresslevel=1)
    with zipfile.ZipFile(path) as z:
        return z.getinfo(asset).compress_size


def entry_offsets(path, asset):
    """(local header, data, central directory record) offsets of asset."""
    with zipfile.ZipFile(path) as z:
        info, cd = z.getinfo(asset), z.start_dir
    raw = open(path, "rb").read()
    lh = info.header_offset
    data = lh + 30 + sum(struct.unpack_from("<HH", raw, lh + 26))
    while raw[cd:cd + 4] == b"PK\x01\x02":
        n, m, k = struct.unpack_from("<HHH", raw, cd + 28)
        if raw[cd + 46:cd + 46 + n] == asset.encode():
            return lh, data, cd
        cd += 46 + n + m + k
    raise ValueError(asset)


def patch(path, at, new):
    with open(path, "r+b") as f:
        f.seek(at)
        f.write(new)


# ── running it ──────────────────────────────────────────────────────────

class Run:
    """The runner on a cart, with a heartbeat pipe (unless hb=False), its
    lines read as they come with the time they came."""

    def __init__(self, binary, cart, d, shim, hb=True, **env):
        self.lines, self.buf = [], b""
        self.save = os.path.join(d, os.path.basename(cart) + ".sav")
        self.log_path = os.path.join(d, os.path.basename(cart) + ".log")
        e = dict(os.environ, LD_PRELOAD=shim, **{k: str(v) for k, v in env.items()})
        e.setdefault("SDL_VIDEODRIVER", "offscreen")
        e.setdefault("SDL_RENDER_DRIVER", "software")
        e.setdefault("SDL_AUDIODRIVER", "dummy")
        self.r = None
        fds = ()
        if hb:
            self.r, w = os.pipe()
            e["COUCHMIX_HEARTBEAT_FD"] = str(w)
            fds = (w,)
        self.p = subprocess.Popen([binary, cart, "--save", self.save], env=e, pass_fds=fds,
                                  stdout=subprocess.DEVNULL, stderr=open(self.log_path, "w"))
        if hb:
            os.close(w)

    def read(self, seconds, until=None):
        """Lines for up to seconds, or until until(line) is true of one."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if self.r is None:
                time.sleep(0.05)
                continue
            ready, _, _ = select.select([self.r], [], [], 0.05)
            if not ready:
                continue
            chunk = os.read(self.r, 65536)
            if not chunk:
                return False
            self.buf += chunk
            *lines, self.buf = self.buf.split(b"\n")
            for raw in lines:
                parts = raw.decode().split()
                self.lines.append((time.monotonic(), parts))
                if until and until(parts):
                    return True
        return False

    def log(self):
        with open(self.log_path) as f:
            return f.read()

    def stop(self):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait(5)
        if self.r is not None:
            os.close(self.r)


def a_lines(lines):
    return [(t, [int(x) for x in p[1:]]) for t, p in lines if p and p[0] == "A"]


def kinds(lines):
    return [p[0] for _, p in lines if p]


def check_progress(check, what, lines, total, min_lines=4):
    """The A lines of one long load, in order, among the frames."""
    a = a_lines(lines)
    seq = [v for _, v in a]  # [load, us, done, total]
    check(f"{what}: A lines while it loads", len(a) >= min_lines, len(a))
    if not a:
        return
    check(f"{what}: one load, and its total", len({v[0] for v in seq}) == 1 and
          all(v[3] == total for v in seq), seq[:3])
    check(f"{what}: done rises to total", all(x[2] < y[2] for x, y in zip(seq, seq[1:])) and
          seq[-1][2] == total, [v[2] for v in seq])
    check(f"{what}: monotonic times", all(x[1] < y[1] for x, y in zip(seq, seq[1:])))
    gaps = [(y[1] - x[1]) / 1e6 for x, y in zip(seq, seq[1:])]
    # The last one comes when the bytes are all in, whenever that is.
    check(f"{what}: 250 ms apart (a few a second)", all(g >= 0.245 for g in gaps[:-1]), gaps)
    k = kinds(lines)
    first, last = k.index("A"), len(k) - 1 - k[::-1].index("A")
    check(f"{what}: frames before, none during, frames after",
          "F" in k[:first] and "F" not in k[first:last + 1] and "F" in k[last:], k[max(0, first - 2):last + 3])
    fs = [(t, int(p[2])) for t, p in lines if p and p[0] == "F"]
    before = [us for _, us in fs if us < seq[0][1]]
    if before:
        check(f"{what}: the first 250 ms after the last frame", seq[0][1] - before[-1] >= 245_000,
              (seq[0][1] - before[-1]) / 1e6)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    failures = []

    def check(what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail != "" and not ok else ""))
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as d:
        shim = os.path.join(d, "slowread.so")
        with open(os.path.join(d, "slowread.c"), "w") as f:
            f.write(SHIM)
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-o", shim, os.path.join(d, "slowread.c"), "-ldl"],
                       check=True)

        big = 96 * MiB + 12345
        stored = os.path.join(d, "stored.wasc")
        write_cart(stored, "big.bin", random.Random(1).randbytes(big), zipfile.ZIP_STORED)
        deflated = os.path.join(d, "deflated.wasc")
        comp = write_cart(deflated, "big.bin", low_entropy(big, 2), zipfile.ZIP_DEFLATED)

        def loaded(r, seconds=10):
            """The runner's log once the cart has said how its load went."""
            end = time.monotonic() + seconds
            while time.monotonic() < end and "load ok" not in r.log() and "load BAD" not in r.log():
                r.read(0.2)
            return r.log()

        # stored, then deflated: about 1.5 s each
        for what, cart, rate in (("stored", stored, RATE), ("deflated", deflated, comp / 1.5)):
            r = Run(binary, cart, d, shim, SLOWREAD_BPS=rate)
            try:
                r.read(15, lambda p: p and p[0] == "A")
                r.read(10, lambda p: p and p[0] == "A" and int(p[3]) == int(p[4]))
                r.read(0.5)
                log = loaded(r)
                check(f"{what}: the load returned the asset (and -1 for one byte too little)",
                      "load ok" in log, log[-400:])
                check_progress(check, what, r.lines, big)
            finally:
                r.stop()

        # many: 120 loads of 1 MiB in one frame, at 64 MB/s (~2 s)
        many = os.path.join(d, "many.wasc")
        write_cart(many, "small.bin", random.Random(3).randbytes(MiB), zipfile.ZIP_STORED, count=120)
        r = Run(binary, many, d, shim, SLOWREAD_BPS=RATE)
        try:
            r.read(15, lambda p: p and p[0] == "A")
            r.read(10, lambda p: p and p[0] == "F")
            log = loaded(r)
            check("many: all 120 returned the asset", "load ok" in log, log[-400:])
            seq = [v for _, v in a_lines(r.lines)]
            check("many: A lines across the loads", len(seq) >= 5, len(seq))
            check("many: their load numbers rising, each line a whole small load",
                  all(x[0] < y[0] for x, y in zip(seq, seq[1:])) and all(v[2] == v[3] == MiB for v in seq),
                  seq[:4])
            gaps = [(y[1] - x[1]) / 1e6 for x, y in zip(seq, seq[1:])]
            check("many: 250 ms apart", all(g >= 0.245 for g in gaps), gaps)
        finally:
            r.stop()

        # crc: a stored entry with a byte changed; a deflated one with the wrong CRC
        bad_stored = os.path.join(d, "badstored.wasc")
        data = random.Random(4).randbytes(24 * MiB)
        write_cart(bad_stored, "big.bin", data, zipfile.ZIP_STORED)
        _, at, _ = entry_offsets(bad_stored, "big.bin")
        patch(bad_stored, at + len(data) // 2, bytes([data[len(data) // 2] ^ 0x5A]))
        bad_deflated = os.path.join(d, "baddeflated.wasc")
        data = low_entropy(24 * MiB, 5)
        comp = write_cart(bad_deflated, "big.bin", data, zipfile.ZIP_DEFLATED)
        lh, _, cd = entry_offsets(bad_deflated, "big.bin")
        with zipfile.ZipFile(bad_deflated) as z:
            wrong = struct.pack("<I", z.getinfo("big.bin").CRC ^ 1)
        patch(bad_deflated, lh + 14, wrong)
        patch(bad_deflated, cd + 16, wrong)
        for what, cart, rate in (("crc, stored", bad_stored, RATE), ("crc, deflated", bad_deflated, comp / 0.6)):
            r = Run(binary, cart, d, shim, SLOWREAD_BPS=rate)
            try:
                log = loaded(r, 20)
                r.read(0.3)
                check(f"{what}: -1, and the runner says why", "load BAD" in log and "CRC mismatch" in log,
                      log[-400:])
                seq = [v for _, v in a_lines(r.lines)]
                check(f"{what}: its A lines reached total all the same", seq and seq[-1][2] == seq[-1][3],
                      seq[-2:])
            finally:
                r.stop()

        # home: SIGUSR1 once the load is under way
        r = Run(binary, stored, d, shim, SLOWREAD_BPS=RATE)
        try:
            r.read(15, lambda p: p and p[0] == "A")
            r.p.send_signal(signal.SIGUSR1)
            t_home = time.monotonic()
            r.read(10, lambda p: p and p[0] == "S")
            r.read(1.0)
            ka = [p[0] for t, p in r.lines if p and t >= t_home]
            done = [int(p[3]) for t, p in r.lines if p and p[0] == "A" and t >= t_home]
            check("home: the load goes on after SIGUSR1, to its end",
                  len(done) >= 2 and done[-1] == big and all(x < y for x, y in zip(done, done[1:])), done)
            a_end = len(ka) - 1 - ka[::-1].index("A") if "A" in ka else len(ka)
            s_at = ka.index("S") if "S" in ka else len(ka)
            check("home: then S, after the load (at most its frame's F between)",
                  a_end < s_at < len(ka) and ka[a_end:s_at].count("F") <= 1, ka[-12:])
            check("home: the cart had its asset", "load ok" in r.log(), r.log()[-300:])
            check("home: P and no F while suspended", "P" in ka[s_at:] and "F" not in ka[s_at:], ka[s_at:])
            n = len(r.lines)
            r.p.send_signal(signal.SIGUSR2)
            r.read(2, lambda p: p and p[0] == "F")
            kr = kinds(r.lines[n:])
            check("home: R then F on SIGUSR2", "R" in kr and "F" in kr[kr.index("R"):], kr[:6])
        finally:
            r.stop()

        # quit: SIGTERM once the load is under way
        r = Run(binary, stored, d, shim, SLOWREAD_BPS=RATE)
        try:
            if os.path.exists(r.save):
                os.remove(r.save)
            r.read(15, lambda p: p and p[0] == "A")
            r.p.send_signal(signal.SIGTERM)
            r.read(10)
            rc = r.p.wait(10)
            ka = kinds(r.lines)
            done = [int(p[3]) for _, p in r.lines if p and p[0] == "A"]
            check("quit: the load finishes, then the save, exit 0",
                  done and done[-1] == big and "W" in ka and ka.index("W") > ka.index("A") and rc == 0
                  and os.path.exists(r.save) and open(r.save, "rb").read() == b"load test save!!",
                  (rc, ka[-6:], done[-2:]))
            check("quit: the cart had its asset", "load ok" in r.log(), r.log()[-300:])
        finally:
            r.stop()

        # stall: the reads stop after 32 MiB, and so do the lines
        r = Run(binary, stored, d, shim, SLOWREAD_BPS=RATE, SLOWREAD_STALL_AFTER=32 * MiB)
        try:
            r.read(15, lambda p: p and p[0] == "A")
            r.read(3.0)
            last = r.lines[-1][0]
            n = len(r.lines)
            r.read(3.0)
            done = [int(p[3]) for _, p in r.lines if p and p[0] == "A"]
            check("stall: A lines up to the stall, none at total", done and done[-1] <= 32 * MiB, done)
            check("stall: then nothing at all for 3 s", len(r.lines) == n and time.monotonic() - last > 3.0,
                  kinds(r.lines[n:])[:5])
            check("stall: still running (the supervisor's to end)", r.p.poll() is None)
        finally:
            r.stop()

        # no fd: the same load, nobody told
        r = Run(binary, stored, d, shim, hb=False)
        try:
            log = loaded(r)
            check("no fd: the load is the same", "load ok" in log, log[-300:])
        finally:
            r.stop()

        # init: the stored load in wc_init, before the cart's first frame
        at_init = os.path.join(d, "init.wasc")
        write_cart(at_init, "big.bin", random.Random(1).randbytes(big), zipfile.ZIP_STORED, at_init=True)
        r = Run(binary, at_init, d, shim, SLOWREAD_BPS=RATE)
        try:
            r.read(20, lambda p: p and p[0] == "F")
            log = loaded(r)
            check("init: the load in wc_init returned the asset", "load ok" in log, log[-400:])
            k = kinds(r.lines)
            seq = [v for _, v in a_lines(r.lines)]
            check("init: A lines while it loads", len(seq) >= 4, k[:8])
            check("init: all before the I line and the first F",
                  "A" in k and "I" in k and "F" in k and
                  len(k) - 1 - k[::-1].index("A") < k.index("I") < k.index("F"), k[:20])
            check("init: done rises to total", seq and all(x[2] < y[2] for x, y in zip(seq, seq[1:]))
                  and seq[-1][2] == seq[-1][3] == big, [v[2] for v in seq])
        finally:
            r.stop()

    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
