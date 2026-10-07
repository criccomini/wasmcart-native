#!/usr/bin/env python3
"""asset_size_test.py — an asset too big for the ABI's int32 sizes has none.

wc_asset_size and wc_load_asset give sizes as int32. An entry of 2 GiB or
more used to come out of wc_asset_size cast: negative for 2.5 GiB, and a
small, wrong size for 4 GiB + 300 MiB (300 MiB). Now both calls say -1
for it, as for an asset that isn't there, and a small asset beside it
still loads.

The cart holds huge.bin, 2.5 GiB of deflated zeros (a few MB in the
archive), and b.bin. wc_init asks huge.bin's size, loads it with room for
4 KiB, loads b.bin, and logs "sizes ok" or "sizes BAD". Making the cart
deflates 2.5 GiB: a few seconds.

Run:  python3 test/asset_size_test.py build/wasmcart-run
"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

INFO, AWRITE, PADS, TIME, HOST, PTRS, KEYS = 0x400, 0x500, 0x600, 0x700, 0x740, 0x780, 0x800
AUDIO, FB, DEST = 0x2000, 0x10000, 0x20000
HUGE, SMALL, OK, BAD = 0x900, 0x910, 0x940, 0x950
HUGE_SIZE = 5 << 29  # 2.5 GiB
ASSET = b"small asset"


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
    return b"\x41" + sleb(n)


def call(i):
    return b"\x10" + uleb(i)


EQ, AND = b"\x46", b"\x71"


def cart_wasm():
    """An ABI 4, 64x64 2D cart. Imports wc_asset_size (0), wc_load_asset
    (1) and wc_log (2); defines wc_get_info (3), wc_init (4), wc_render (5)."""
    i32 = 0x7F
    types = vec([b"\x60\x00\x01" + bytes([i32]), b"\x60\x00\x00",
                 b"\x60\x04" + bytes([i32] * 4) + b"\x01" + bytes([i32]),
                 b"\x60\x02" + bytes([i32] * 2) + b"\x00",
                 b"\x60\x02" + bytes([i32] * 2) + b"\x01" + bytes([i32])])
    imports = vec([name("env") + name("wc_asset_size") + b"\x00" + uleb(4),
                   name("env") + name("wc_load_asset") + b"\x00" + uleb(2),
                   name("env") + name("wc_log") + b"\x00" + uleb(3)])
    funcs = vec([uleb(0), uleb(1), uleb(1)])
    mem = vec([b"\x00" + uleb(4)])
    exports = vec([name("memory") + b"\x02" + uleb(0), name("wc_get_info") + b"\x00" + uleb(3),
                   name("wc_init") + b"\x00" + uleb(4), name("wc_render") + b"\x00" + uleb(5)])
    size = const(HUGE) + const(8) + call(0) + const(-1) + EQ
    load = const(HUGE) + const(8) + const(DEST) + const(4096) + call(1) + const(-1) + EQ
    small = const(SMALL) + const(5) + const(DEST) + const(4096) + call(1) + const(len(ASSET)) + EQ
    init = (size + load + AND + small + AND + b"\x04\x40" + const(OK) + const(8) + call(2) + b"\x05" +
            const(BAD) + const(9) + call(2) + b"\x0b")

    def body(code):
        b = vec([]) + code + b"\x0b"
        return uleb(len(b)) + b

    code = vec([body(const(INFO)), body(init), body(b"")])
    img = bytearray(0xA00 - INFO)
    struct.pack_into("<18I", img, 0, 4, 64, 64, FB, AUDIO, 1024, AWRITE, PADS, 0, 0, TIME, HOST, 0, 48000,
                     PTRS, KEYS, 0, 0)
    for addr, s in ((HUGE, b"huge.bin"), (SMALL, b"b.bin"), (OK, b"sizes ok"), (BAD, b"sizes BAD")):
        img[addr - INFO:addr - INFO + len(s)] = s
    data = vec([b"\x00" + const(INFO) + b"\x0b" + uleb(len(img)) + bytes(img)])
    return (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(2, imports) + section(3, funcs) +
            section(5, mem) + section(7, exports) + section(10, code) + section(11, data))


def write_cart(path):
    zero = bytes(64 << 20)
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("manifest.json", json.dumps({"name": "sizes", "abi": 4}))
        z.writestr("cart.wasm", cart_wasm())
        zi = zipfile.ZipInfo("huge.bin")
        zi.compress_type = zipfile.ZIP_DEFLATED
        with z.open(zi, "w", force_zip64=True) as f:
            left = HUGE_SIZE
            while left:
                f.write(zero[:min(left, len(zero))])
                left -= min(left, len(zero))
        z.writestr("b.bin", ASSET)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "c.wasc")
        write_cart(path)
        with zipfile.ZipFile(path) as z:
            assert z.getinfo("huge.bin").file_size == HUGE_SIZE
        p = subprocess.run(["timeout", "-s", "TERM", "5", binary, path, "--save", os.path.join(d, "s")],
                           env=env, capture_output=True, text=True)
        out = p.stdout + p.stderr
    ok = "sizes ok" in out and "sizes BAD" not in out
    print(f"  {'ok  ' if ok else 'FAIL'}  a 2.5 GiB asset has no size and doesn't load; a small one does")
    if not ok:
        print("    " + "\n    ".join(out.splitlines()[-6:]))
    print("\nall checks passed" if ok else "\nFAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
