#!/usr/bin/env python3
"""asset_index_test.py — a threaded cart's asset index: shared, and found as the main thread finds it.

A WASI-threads cart's threads look assets up themselves (thread_worker_js.h).
The index they used was a V8 object per archive entry, built before wc_init
and structured-cloned into every worker: with 70,000 entries, hundreds of
milliseconds before the cart started and tens of MiB per worker. Now it's
one SharedArrayBuffer that every worker shares.

Lookups. The main thread loads each case in wc_init (asset_loader.c and
miniz); a spawned thread then loads the same ones. Sizes, results and bytes
must match: the manifest's asset root before the bare name before
"assets/", ASCII case folded as miniz folds it, the one entry miniz picks
of two names that differ only in case, a path cut at its first NUL and at
511 bytes (also when the root makes it longer), a name too long to ever be
found, a directory, _filelist.txt, and a missing name.

Memory. 70,000 entries and four threads, each loading one asset and then
waiting, against the same cart with one entry: with all four workers alive,
the runner's anonymous memory may differ by at most 24 MiB. The time from
start to the cart's first log line is printed for both.

Run:  python3 test/asset_index_test.py build/wasmcart-run
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

MiB = 1 << 20
PAGE = 65536
INFO, FB, STRS = 0x400, 0x800, 0x1000
FLAG, REPORTED, COUNT, QUIT = 0x3000, 0x3004, 0x3008, 0x300C
SAVE = 0x4000
WORDS = 5  # per case: main load, thread load, same bytes, main size, thread size
DEST0 = 1 * MiB


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


LOAD, STORE = b"\x28\x02\x00", b"\x36\x02\x00"
SPAWN, LOG, LOAD_ASSET, ASSET_SIZE = 0, 1, 2, 3
GET_INFO, INIT, RENDER, THREAD, EQ = 4, 5, 6, 7, 8


def call(f):
    return b"\x10" + leb(f)


def cart_wasm(cases, threads=1, main_loads=True, park=False):
    """cases: [(path bytes, max_size, dest_a, dest_b)]. wc_init logs INIT,
    loads each case into dest_a (main_loads), and spawns the threads; each
    thread loads each case into dest_b + its arg * 64 KiB and counts
    itself in, then returns, or waits forever (park). Once every thread is
    in, wc_render compares, fills in the results and logs DONE."""
    strs, data = {}, []
    nxt = [STRS]

    def s(raw):
        if raw not in strs:
            strs[raw] = (nxt[0], len(raw))
            data.append((nxt[0], raw))
            nxt[0] += len(raw) + 1
        return strs[raw]

    def res(i, w):
        return SAVE + 4 * (WORDS * i + w)

    def load(i, dest, word, size_word, per_thread=False):
        p, n = s(cases[i][0])
        d = i32(dest) + ((b"\x20\x01" + i32(PAGE) + b"\x6c\x6a") if per_thread else b"")
        return (i32(res(i, word)) + i32(p) + i32(n) + d + i32(cases[i][1]) + call(LOAD_ASSET) + STORE +
                i32(res(i, size_word)) + i32(p) + i32(n) + call(ASSET_SIZE) + STORE)

    def log(text):
        p, n = s(text.encode())
        return i32(p) + i32(n) + call(LOG)

    init = log("INIT")
    if main_loads:
        init += b"".join(load(i, c[2], 0, 3) for i, c in enumerate(cases))
    init += b"".join(i32(k) + call(SPAWN) + b"\x1a" for k in range(threads))
    thread = b"".join(load(i, c[3], 1, 4, per_thread=True) for i, c in enumerate(cases))
    thread += i32(COUNT) + i32(1) + b"\xfe\x1e\x02\x00\x1a"                  # i32.atomic.rmw.add
    if park:
        thread += i32(QUIT) + i32(0) + b"\x42\x7f" + b"\xfe\x01\x02\x00\x1a"   # memory.atomic.wait32
    render = (i32(COUNT) + b"\xfe\x10\x02\x00" + i32(threads) + b"\x49\x04\x40\x0f\x0b" +
              i32(REPORTED) + LOAD + b"\x04\x40\x0f\x0b" +
              i32(REPORTED) + i32(1) + STORE)
    if main_loads:
        for i, c in enumerate(cases):
            render += (i32(res(i, 0)) + LOAD + i32(0) + b"\x4a\x04\x40" +
                       i32(res(i, 2)) + i32(c[2]) + i32(c[3]) + i32(res(i, 0)) + LOAD + call(EQ) + STORE +
                       b"\x0b")
    render += log("DONE")
    eq = (b"\x02\x40\x03\x40" +
          b"\x20\x02\x45\x0d\x01" +
          b"\x20\x00\x2d\x00\x00\x20\x01\x2d\x00\x00\x47\x04\x40" + i32(0) + b"\x0f\x0b" +
          b"\x20\x00" + i32(1) + b"\x6a\x21\x00" +
          b"\x20\x01" + i32(1) + b"\x6a\x21\x01" +
          b"\x20\x02" + i32(1) + b"\x6b\x21\x02" +
          b"\x0c\x00\x0b\x0b" + i32(1))

    top = max([DEST0] + [max(c[2], c[3] + threads * PAGE) + max(c[1], 1) for c in cases])
    pages = top // PAGE + 2
    data.insert(0, (INFO, struct.pack("<18I", 4, 8, 8, FB, 0, 0, 0, 0, SAVE, 4 * WORDS * max(len(cases), 1),
                                      *([0] * 8))))
    types = [b"\x60\x00\x01\x7f", b"\x60\x00\x00", b"\x60\x02\x7f\x7f\x00", b"\x60\x02\x7f\x7f\x01\x7f",
             b"\x60\x04\x7f\x7f\x7f\x7f\x01\x7f", b"\x60\x01\x7f\x01\x7f", b"\x60\x03\x7f\x7f\x7f\x01\x7f"]
    imports = [name("wasi") + name("thread-spawn") + b"\x00" + leb(5),
               name("env") + name("wc_log") + b"\x00" + leb(2),
               name("env") + name("wc_load_asset") + b"\x00" + leb(4),
               name("env") + name("wc_asset_size") + b"\x00" + leb(3),
               name("env") + name("memory") + b"\x02\x03" + leb(pages) + leb(pages)]
    funcs = vec([leb(0), leb(1), leb(1), leb(2), leb(6)])
    exports = vec([name("wc_get_info") + b"\x00" + leb(GET_INFO), name("wc_init") + b"\x00" + leb(INIT),
                   name("wc_render") + b"\x00" + leb(RENDER),
                   name("wasi_thread_start") + b"\x00" + leb(THREAD)])

    def body(code):
        b = b"\x00" + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(i32(INFO)), body(init), body(render), body(thread), body(eq)])
    dsec = vec([b"\x00" + i32(a) + b"\x0b" + leb(len(d)) + d for a, d in data])
    return (b"\x00asm\x01\x00\x00\x00" + section(1, vec(types)) + section(2, vec(imports)) +
            section(3, funcs) + section(7, exports) + section(10, code) + section(11, dsec))


LONG = "L" * 507          # "app/" + LONG is 511 bytes: as long as a lookup gets
TOO_LONG = "M" * 600      # never found: the main thread cuts a path at 511


def lookups_cart(path):
    entries = [
        ("app/a.bin", b"root a!"), ("assets/a.bin", b"assets a"), ("a.bin", b"bare a"),
        ("b.bin", b"bare b"), ("assets/b.bin", b"assets b!"),
        ("assets/c.bin", b"assets c"),
        ("app/Mixed.Case.BIN", b"mixed case"),
        ("app/dup.bin", b"lower dup"), ("app/DUP.BIN", b"UPPER DUP!"),
        ("app/" + LONG, b"long name"),
        ("app/" + TOO_LONG, b"too long"),
        ("app/dir/", b""),
    ]
    # (path as the cart passes it, max size, expected main answer or None for any)
    cases = [
        (b"a.bin", 64, 7),                       # the root wins
        (b"b.bin", 64, 6),                       # bare before assets/
        (b"c.bin", 64, 8),                       # assets/
        (b"MIXED.case.bin", 64, 10),             # case folded
        (b"Dup.Bin", 64, None),                  # one of two: the one miniz picks
        (LONG.encode() + b"-and-more", 64, 9),   # root + path cut at 511 bytes
        (TOO_LONG.encode(), 64, -1),
        (b"c.bin\x00junk", 64, 8),               # cut at the NUL
        (b"dir/", 64, 0),
        (b"_filelist.txt", 4096, None),
        (b"_FILELIST.txt", 4096, -1),            # the list's name is exact
        (b"nothing.bin", 64, -1),
    ]
    laid, at = [], DEST0
    for p, mx, _ in cases:
        laid.append((p, mx, at, at + PAGE))
        at += 2 * PAGE
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("manifest.json", json.dumps({"name": "assetindex", "abi": 4, "assets": "app"}))
        z.writestr("cart.wasm", cart_wasm(laid))
        for n, body in entries:
            z.writestr(zipfile.ZipInfo(n, (2026, 10, 7, 0, 0, 0)), body)
    return cases


def many_cart(path, entries, threads):
    wasm = cart_wasm([(f"e{entries - 1:05d}.bin".encode(), 64, DEST0, DEST0)], threads=threads,
                     main_loads=False, park=True)
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("manifest.json", json.dumps({"name": "assetindex-many", "abi": 4}))
        z.writestr("cart.wasm", wasm)
        for i in range(entries):
            z.writestr(zipfile.ZipInfo(f"assets/e{i:05d}.bin", (2026, 10, 7, 0, 0, 0)), f"entry {i}\n")


def status(pid):
    out = {}
    try:
        for line in open(f"/proc/{pid}/status"):
            k, _, v = line.partition(":")
            if k in ("VmRSS", "RssAnon"):
                out[k] = int(v.split()[0]) // 1024
    except OSError:
        pass
    return out


def run(binary, path, wait=60.0):
    """Run until DONE, then SIGTERM. Returns (exit code, stderr, save or
    None, /proc status at DONE, seconds from start to the first [cart] line)."""
    save = path + ".sav"
    if os.path.exists(save):
        os.unlink(save)
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    t0 = time.monotonic()
    p = subprocess.Popen([binary, path], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    os.set_blocking(p.stderr.fileno(), False)
    buf, first, st = b"", None, {}
    deadline = t0 + wait
    while time.monotonic() < deadline:
        chunk = p.stderr.read()
        if chunk:
            buf += chunk
            if first is None and b"[cart]: INIT" in buf:
                first = time.monotonic() - t0
        if b"[cart]: DONE" in buf:
            st = status(p.pid)
            break
        if p.poll() is not None:
            break
        time.sleep(0.005)
    if p.poll() is None:
        p.send_signal(signal.SIGTERM)
    os.set_blocking(p.stderr.fileno(), True)
    try:
        buf += p.stderr.read()
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    data = open(save, "rb").read() if os.path.exists(save) else None
    if os.path.exists(save):
        os.unlink(save)
    return p.returncode, buf.decode("utf-8", "replace"), data, st, first


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
        path = os.path.join(d, "assetindex.wasc")
        cases = lookups_cart(path)
        rc, err, save, _, _ = run(binary, path)
        ok = save is not None and len(save) == 4 * WORDS * len(cases)
        check("the cart ran both sets of lookups and saved its results", ok,
              f"exit {rc}, save {len(save) if save else None} bytes: {err.strip()[-600:]}")
        if ok:
            for i, (p, mx, want) in enumerate(cases):
                main_ret, thr_ret, same, main_size, thr_size = struct.unpack_from("<5i", save, 4 * WORDS * i)
                what = repr(p if len(p) < 40 else p[:12] + b"..." + p[-12:])
                got = f"main {main_ret} (size {main_size}), thread {thr_ret} (size {thr_size}), same={same}"
                check(f"{what}: the thread finds what the main thread finds",
                      main_ret == thr_ret and main_size == thr_size and (want is None or main_ret == want), got)
                if main_ret > 0:
                    check(f"{what}: and the same bytes", same == 1, got)

        firsts, anon = {}, {}
        for label, entries in (("one", 1), ("many", 70000)):
            path = os.path.join(d, f"{label}.wasc")
            many_cart(path, entries, 4)
            rc, err, save, st, first = run(binary, path)
            os.unlink(path)
            got = struct.unpack_from("<i", save, 4) if save else None
            check(f"{entries} entries, four threads: each loaded its asset",
                  "[cart]: DONE" in err and got == (len(f"entry {entries - 1}\n"),) and "RssAnon" in st,
                  f"exit {rc}, load {got}, status {st}: {err.strip()[-400:]}")
            firsts[label] = round(first or -1, 3)
            anon[label] = st.get("RssAnon", -1)
        extra = anon["many"] - anon["one"]
        check(f"70,000 entries cost four workers {extra} MiB more than one entry (limit 24)", extra < 24,
              f"RssAnon MiB {anon}")
        print(f"  RssAnon with four workers alive, MiB: {anon}; seconds to the cart's first line: {firsts}")

    print("PASS" if not failures else f"FAIL: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
