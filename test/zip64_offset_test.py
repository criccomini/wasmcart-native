#!/usr/bin/env python3
"""zip64_offset_test.py — a cart whose entry offsets are in zip64 fields opens.

A zip64 writer may put an entry's local header offset in the zip64 extra
field, leaving 0xFFFFFFFF in the 32-bit one, while the entry's sizes stay
32-bit. Python's zipfile does that for every offset past 2 GiB - 1. miniz's
sanity check of the central directory added the 32-bit field to the
entry's size before looking at the zip64 one, so such an archive failed
to open ("failed to open ZIP") unless it was over 4 GiB + that entry's
size: a 2.8 GiB cart written by Python, or a 4.3 GiB one, never ran.

The cart here is small, written by Python's zipfile with its zip64 limit
lowered so that b.bin's offset goes in the zip64 field and its sizes
don't. wc_init loads b.bin, checks its size and first word, and logs
"asset ok" or "asset BAD". The same cart written plainly is the control.

Run:  python3 test/zip64_offset_test.py build/wasmcart-run
"""

import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

INFO, AWRITE, PADS, TIME, HOST, PTRS, KEYS = 0x400, 0x500, 0x600, 0x700, 0x740, 0x780, 0x800
AUDIO, FB, DEST = 0x2000, 0x10000, 0x20000
NAME, OK, BAD = 0x900, 0x940, 0x950
MAGIC = b"WC64"
ASSET = MAGIC + b"z" * 60  # small: its sizes fit 32 bits


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


def log(addr, text):
    return const(addr) + const(len(text)) + call(1)


def cart_wasm():
    """An ABI 4, 64x64 2D cart. Imports wc_load_asset (0) and wc_log (1);
    defines wc_get_info (2), wc_init (3), wc_render (4)."""
    i32 = 0x7F
    types = vec([b"\x60\x00\x01" + bytes([i32]), b"\x60\x00\x00",
                 b"\x60\x04" + bytes([i32] * 4) + b"\x01" + bytes([i32]), b"\x60\x02" + bytes([i32] * 2) + b"\x00"])
    imports = vec([name("env") + name("wc_load_asset") + b"\x00" + uleb(2),
                   name("env") + name("wc_log") + b"\x00" + uleb(3)])
    funcs = vec([uleb(0), uleb(1), uleb(1)])
    mem = vec([b"\x00" + uleb(4)])
    exports = vec([name("memory") + b"\x02" + uleb(0), name("wc_get_info") + b"\x00" + uleb(2),
                   name("wc_init") + b"\x00" + uleb(3), name("wc_render") + b"\x00" + uleb(4)])
    # (wc_load_asset("b.bin", DEST, 4096) == len) && (i32.load DEST == MAGIC)
    loaded = const(NAME) + const(5) + const(DEST) + const(4096) + call(0) + const(len(ASSET)) + b"\x46"
    first = const(DEST) + b"\x28\x02\x00" + const(struct.unpack("<i", MAGIC)[0]) + b"\x46"
    init = loaded + first + b"\x71" + b"\x04\x40" + log(OK, b"asset ok") + b"\x05" + log(BAD, b"asset BAD") + b"\x0b"

    def body(code):
        b = vec([]) + code + b"\x0b"
        return uleb(len(b)) + b

    code = vec([body(const(INFO)), body(init), body(b"")])
    img = bytearray(0xA00 - INFO)
    struct.pack_into("<18I", img, 0, 4, 64, 64, FB, AUDIO, 1024, AWRITE, PADS, 0, 0, TIME, HOST, 0, 48000,
                     PTRS, KEYS, 0, 0)
    for addr, s in ((NAME, b"b.bin"), (OK, b"asset ok"), (BAD, b"asset BAD")):
        img[addr - INFO:addr - INFO + len(s)] = s
    data = vec([b"\x00" + const(INFO) + b"\x0b" + uleb(len(img)) + bytes(img)])
    return (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(2, imports) + section(3, funcs) +
            section(5, mem) + section(7, exports) + section(10, code) + section(11, data))


def cart(zip64_limit):
    """The cart, with zipfile's zip64 limit set to zip64_limit while it's
    written: past it, an offset goes in the zip64 field."""
    wasm = cart_wasm()
    old = zipfile.ZIP64_LIMIT
    zipfile.ZIP64_LIMIT = zip64_limit
    try:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("manifest.json", json.dumps({"name": "zip64", "abi": 4}))
            z.writestr("cart.wasm", wasm)
            z.writestr("b.bin", ASSET)
        return buf.getvalue()
    finally:
        zipfile.ZIP64_LIMIT = old


def offset_field(data, entry):
    """The 32-bit local header offset and compressed size the central
    directory gives entry."""
    i = 0
    while (i := data.find(b"PK\x01\x02", i)) != -1:
        n = struct.unpack_from("<H", data, i + 28)[0]
        if data[i + 46:i + 46 + n] == entry.encode():
            return struct.unpack_from("<I", data, i + 42)[0], struct.unpack_from("<I", data, i + 20)[0]
        i += 4
    raise KeyError(entry)


def run(binary, path, env, d):
    p = subprocess.run(["timeout", "-s", "TERM", "3", binary, path, "--save", os.path.join(d, "s")],
                       env=env, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    failures = []

    def check(label, ok, out):
        print(f"  {'ok  ' if ok else 'FAIL'}  {label}")
        if not ok:
            failures.append(label)
            print("    " + "\n    ".join(out.splitlines()[-6:]))

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "c.wasc")
        plain = cart(zipfile.ZIP64_LIMIT)
        # Past 1000 bytes, an offset or size is zip64: cart.wasm's sizes
        # (it's under 2 KiB) and b.bin's offset, but not b.bin's sizes.
        zip64 = cart(1000)
        ofs, comp = offset_field(zip64, "b.bin")
        check(f"b.bin's offset is in its zip64 field, its size isn't ({ofs:#x}, {comp})",
              ofs == 0xFFFFFFFF and comp == len(ASSET) and offset_field(plain, "b.bin")[0] < 0xFFFFFFFF, "")

        for label, data in (("plain", plain), ("zip64 offset", zip64)):
            with open(path, "wb") as f:
                f.write(data)
            rc, out = run(binary, path, env, d)
            check(f"{label}: opens and loads b.bin (exit {rc})",
                  "asset ok" in out and "failed to open ZIP" not in out and "asset BAD" not in out, out)

    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
