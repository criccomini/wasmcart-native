#!/usr/bin/env python3
"""worker_asset_test.py — a cart thread's assets: the main thread's answers, at the asset's size.

A WASI-threads cart's spawned threads load assets in thread_worker_js.h,
not asset_loader.c. That path read the whole compressed entry into one
buffer, inflated it into a second, copied that into the cart's memory, and
never checked the CRC: a 600 MiB asset from a thread cost up to 3.7 times
its size, past a 1 GiB game's limit. Now it reads and inflates straight
into the cart's memory, a chunk at a time, and checks the CRC.

Answers. The cart's main thread loads each case in wc_init, through
miniz; then a spawned thread loads the same cases into other buffers. Both
also ask each case's size. Every answer must match, and wherever a load
succeeded the two buffers must hold the same bytes. The cases: stored,
deflated over many reads, a long run inflated from a few bytes, an empty
file, a dest one byte short, a wrong CRC (stored and deflated), a deflated
stream that ends short of its size or runs past it, a corrupted stream,
bzip2, a missing name, and _filelist.txt. The results land in the save
region; the test quits the cart with SIGTERM and reads the save.

Memory. The thread alone loads one big asset (256 MiB by default), stored
and then deflated, and the runner's peak RSS is compared with the same
cart loading a tiny one. The difference must stay under 1.25 times the
asset: the asset itself plus slack, where the old path took two to three
times it.

Run:  python3 test/worker_asset_test.py build/wasmcart-run [--mib N]
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
import zlib

MiB = 1 << 20
PAGE = 65536
INFO, FB, STRS = 0x400, 0x800, 0x1000
FLAG, REPORTED = 0x3000, 0x3004
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
SPAWN, LOG, LOAD_ASSET, ASSET_SIZE = 0, 1, 2, 3          # imported functions
GET_INFO, INIT, RENDER, THREAD, EQ = 4, 5, 6, 7, 8        # defined ones


def call(f):
    return b"\x10" + leb(f)


def cart_wasm(cases, main_loads=True):
    """cases: [(path, max_size, dest_a, dest_b)]. The main thread (if
    main_loads) loads each into dest_a in wc_init and spawns the thread;
    the thread loads each into dest_b and raises FLAG; wc_render then
    compares, fills the results in, and logs DONE."""
    strs, data = {}, []
    nxt = [STRS]

    def s(text):
        if text not in strs:
            b = text.encode()
            strs[text] = (nxt[0], len(b))
            data.append((nxt[0], b))
            nxt[0] += len(b) + 1
        return strs[text]

    def res(i, w):
        return SAVE + 4 * (WORDS * i + w)

    def load(i, dest, word, size_word):
        p, n = s(cases[i][0])
        return (i32(res(i, word)) + i32(p) + i32(n) + i32(dest) + i32(cases[i][1]) + call(LOAD_ASSET) + STORE +
                i32(res(i, size_word)) + i32(p) + i32(n) + call(ASSET_SIZE) + STORE)

    init = b"".join(load(i, c[2], 0, 3) for i, c in enumerate(cases)) if main_loads else b""
    init += i32(0) + call(SPAWN) + b"\x1a"
    thread = b"".join(load(i, c[3], 1, 4) for i, c in enumerate(cases))
    thread += i32(FLAG) + i32(1) + b"\xfe\x17\x02\x00"                       # i32.atomic.store
    render = (i32(FLAG) + b"\xfe\x10\x02\x00" + b"\x45\x04\x40\x0f\x0b" +     # not yet: return
              i32(REPORTED) + LOAD + b"\x04\x40\x0f\x0b" +                    # once
              i32(REPORTED) + i32(1) + STORE)
    if main_loads:
        for i, c in enumerate(cases):
            render += (i32(res(i, 0)) + LOAD + i32(0) + b"\x4a\x04\x40" +    # main load > 0
                       i32(res(i, 2)) + i32(c[2]) + i32(c[3]) + i32(res(i, 0)) + LOAD + call(EQ) + STORE +
                       b"\x0b")
    p, n = s("DONE")
    render += i32(p) + i32(n) + call(LOG)
    # eq(a, b, n): 1 when n bytes at a and b match
    eq = (b"\x02\x40\x03\x40" +
          b"\x20\x02\x45\x0d\x01" +
          b"\x20\x00\x2d\x00\x00\x20\x01\x2d\x00\x00\x47\x04\x40" + i32(0) + b"\x0f\x0b" +
          b"\x20\x00" + i32(1) + b"\x6a\x21\x00" +
          b"\x20\x01" + i32(1) + b"\x6a\x21\x01" +
          b"\x20\x02" + i32(1) + b"\x6b\x21\x02" +
          b"\x0c\x00\x0b\x0b" + i32(1))

    top = max([DEST0] + [max(c[2], c[3]) + max(c[1], 1) for c in cases])
    pages = top // PAGE + 2
    data.insert(0, (INFO, struct.pack("<18I", 4, 8, 8, FB, 0, 0, 0, 0, SAVE, 4 * WORDS * len(cases),
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


def central_entries(buf):
    """name -> (central header offset, local header offset), for a plain
    (non-zip64) archive."""
    eocd = buf.rindex(b"PK\x05\x06")
    count, _, cd_ofs = struct.unpack_from("<HII", buf, eocd + 10)
    out, pos = {}, cd_ofs
    for _ in range(count):
        n, m, k = struct.unpack_from("<HHH", buf, pos + 28)
        local = struct.unpack_from("<I", buf, pos + 42)[0]
        out[buf[pos + 46:pos + 46 + n].decode()] = (pos, local)
        pos += 46 + n + m + k
    return out


def patch(buf, entries, name, crc=None, usize=None):
    cd, local = entries[name]
    if crc is not None:
        struct.pack_into("<I", buf, cd + 16, crc)
        struct.pack_into("<I", buf, local + 14, crc)
    if usize is not None:
        struct.pack_into("<I", buf, cd + 24, usize)
        struct.pack_into("<I", buf, local + 22, usize)


def letters(n, seed):
    """Text-like bytes: deflate gets them to about two thirds."""
    r = random.Random(seed)
    words = [bytes(r.choice(b"etaoinshrdlucmfwyp") for _ in range(r.randint(2, 9))) for _ in range(4000)]
    out = bytearray()
    while len(out) < n:
        out += r.choice(words) + b" "
    return bytes(out[:n])


def answers_cart(path):
    r = random.Random(7)
    stored = r.randbytes(3 * MiB)
    deflated = letters(6 * MiB, 8)       # several 1 MiB reads of input
    runs = b"wasmcart" * (3 * MiB)       # 24 MiB from a few KiB
    small = letters(64 * 1024, 9)
    # (path, max_size, expected main answer or None to take any)
    cases = [
        ("stored.bin", len(stored), len(stored)),
        ("deflated.bin", len(deflated), len(deflated)),
        ("runs.bin", len(runs), len(runs)),
        ("empty.bin", 16, 0),
        ("stored.bin", len(stored) - 1, -1),          # dest one byte short
        ("crc_stored.bin", len(small), -1),
        ("crc_deflated.bin", len(small), -1),
        ("short.bin", len(small) + 1, -1),            # says one byte more than it has
        ("long.bin", len(small) - 1, -1),             # says one byte less
        ("corrupt.bin", MiB, -1),
        ("bzip.bin", len(small), -1),
        ("missing.bin", 16, -1),
        ("_filelist.txt", 4096, None),
    ]
    laid, at = [], DEST0
    for p, mx, _ in cases:
        span = (max(mx, 1) + PAGE - 1) // PAGE * PAGE
        laid.append((p, mx, at, at + span))
        at += 2 * span

    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "workerasset", "abi": 4}))
        f.writestr("cart.wasm", cart_wasm(laid))
        f.writestr("assets/stored.bin", stored, zipfile.ZIP_STORED)
        f.writestr("assets/deflated.bin", deflated, zipfile.ZIP_DEFLATED)
        f.writestr("assets/runs.bin", runs, zipfile.ZIP_DEFLATED)
        f.writestr("assets/empty.bin", b"", zipfile.ZIP_STORED)
        f.writestr("assets/crc_stored.bin", small, zipfile.ZIP_STORED)
        f.writestr("assets/crc_deflated.bin", small, zipfile.ZIP_DEFLATED)
        f.writestr("assets/short.bin", small, zipfile.ZIP_DEFLATED)
        f.writestr("assets/long.bin", small, zipfile.ZIP_DEFLATED)
        f.writestr("assets/corrupt.bin", deflated[:MiB], zipfile.ZIP_DEFLATED)
        f.writestr("assets/bzip.bin", small, zipfile.ZIP_BZIP2)
    # Damage some entries in place: no offset moves.
    buf = bytearray(z.getvalue())
    ents = central_entries(buf)
    patch(buf, ents, "assets/crc_stored.bin", crc=zlib.crc32(small) ^ 1)
    patch(buf, ents, "assets/crc_deflated.bin", crc=zlib.crc32(small) ^ 1)
    patch(buf, ents, "assets/short.bin", usize=len(small) + 1)
    patch(buf, ents, "assets/long.bin", usize=len(small) - 1)
    cd, local = ents["assets/corrupt.bin"]
    n, m = struct.unpack_from("<HH", buf, local + 26)
    mid = local + 30 + n + m + struct.unpack_from("<I", buf, cd + 20)[0] // 2
    for k in range(64):
        buf[mid + k] ^= 0x5A
    with open(path, "wb") as f:
        f.write(buf)
    return cases


def env():
    e = dict(os.environ)
    e.setdefault("SDL_VIDEODRIVER", "offscreen")
    e.setdefault("SDL_RENDER_DRIVER", "software")
    e.setdefault("SDL_AUDIODRIVER", "dummy")
    return e


def run(binary, path, wait=60.0):
    """Run until the cart logs DONE, then SIGTERM it (it saves on the way
    out). Returns (exit code, stderr, save bytes or None, peak RSS in KiB)."""
    save = path + ".sav"
    if os.path.exists(save):
        os.unlink(save)
    p = subprocess.Popen([binary, path], env=env(), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    lines = []
    deadline = time.monotonic() + wait
    os.set_blocking(p.stderr.fileno(), False)
    buf = b""
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
    rc = os.waitstatus_to_exitcode(status)
    data = open(save, "rb").read() if os.path.exists(save) else None
    if os.path.exists(save):
        os.unlink(save)
    return rc, buf.decode("utf-8", "replace"), data, ru.ru_maxrss


def big_cart(path, mib, method):
    size = mib * MiB
    dest = 64 * MiB
    wasm = cart_wasm([("big.bin", size, dest, dest)], main_loads=False)
    with zipfile.ZipFile(path, "w", allowZip64=True) as z:
        z.writestr("manifest.json", json.dumps({"name": "workerbig", "abi": 4}))
        z.writestr("cart.wasm", wasm)
        zi = zipfile.ZipInfo("assets/big.bin", (2026, 10, 7, 0, 0, 0))
        zi.compress_type = method
        if method == zipfile.ZIP_DEFLATED:
            zi.compress_level = 1
        block = letters(8 * MiB, 3) if method == zipfile.ZIP_DEFLATED else None
        r = random.Random(4)
        with z.open(zi, "w", force_zip64=True) as f:
            done = 0
            while done < size:
                n = min(8 * MiB, size - done)
                f.write(block[:n] if block else r.randbytes(n))
                done += n
    return os.path.getsize(path)


def main():
    args = sys.argv[1:]
    mib = 256
    if "--mib" in args:
        i = args.index("--mib")
        mib = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        print(__doc__)
        return 2
    binary = os.path.abspath(args[0])
    failures = []

    def check(what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail and not ok else ""))
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "workerasset.wasc")
        cases = answers_cart(path)
        rc, err, save, _ = run(binary, path)
        ok = save is not None and len(save) == 4 * WORDS * len(cases)
        check("the cart ran both sets of loads and saved its results", ok,
              f"exit {rc}, save {len(save) if save else None} bytes: {err.strip()[-600:]}")
        if ok:
            for i, (p, mx, want) in enumerate(cases):
                main_ret, thr_ret, same, main_size, thr_size = struct.unpack_from("<5i", save, 4 * WORDS * i)
                what = f"{p} into {mx} bytes"
                got = f"main {main_ret} (size {main_size}), thread {thr_ret} (size {thr_size}), same={same}"
                check(f"{what}: the thread's answers are the main thread's",
                      main_ret == thr_ret and main_size == thr_size and (want is None or main_ret == want), got)
                if main_ret > 0:
                    check(f"{what}: and the same bytes", same == 1, got)
        check("a thread's failed load says why",
              "[cart t1]: asset crc_stored.bin: CRC mismatch" in err and
              "[cart t1]: asset long.bin: more data than its size" in err, err.strip()[-600:])

        peaks = {}
        for label, method in (("tiny", None), ("stored", zipfile.ZIP_STORED), ("deflated", zipfile.ZIP_DEFLATED)):
            big = os.path.join(d, f"{label}.wasc")
            if method is None:
                wasm = cart_wasm([("big.bin", 1024, 64 * MiB, 64 * MiB)], main_loads=False)
                with zipfile.ZipFile(big, "w") as z:
                    z.writestr("manifest.json", json.dumps({"name": "workertiny", "abi": 4}))
                    z.writestr("cart.wasm", wasm)
                    z.writestr("assets/big.bin", b"t" * 1024)
                size = 1024
            else:
                big_cart(big, mib, method)
                size = mib * MiB
            rc, err, save, peak = run(binary, big)
            os.unlink(big)
            got = struct.unpack_from("<i", save, 4) if save else None
            check(f"{label}: the thread loaded it", got == (size,), f"exit {rc}, load {got}: {err.strip()[-400:]}")
            peaks[label] = peak // 1024
        for label in ("stored", "deflated"):
            extra = peaks[label] - peaks["tiny"]
            check(f"a {mib} MiB {label} asset from a thread costs about its size "
                  f"(+{extra} MiB over a tiny one, limit {mib * 5 // 4})", extra < mib * 5 // 4,
                  f"peaks {peaks}")
        print(f"  peak RSS, MiB: {peaks}")

    print("PASS" if not failures else f"FAIL: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
