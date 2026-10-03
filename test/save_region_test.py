#!/usr/bin/env python3
"""save_region_test.py — the host checks a cart's save region before trusting it.

Builds tiny carts byte by byte (no toolchain) whose wc_info_t points the
save region at the wrong place, and runs wasmcart-run on them:

  - a region bigger than WC_MAX_SAVE_SIZE: the cart fails to load
  - a region past the end of the cart's memory: the cart fails to load
  - a region wc_init moves after the save was loaded into the old one:
    the save on disk is left alone, not overwritten with fresh state
  - a save file of the wrong size: not loaded, kept as <save>.invalid-1,
    and the cart starts fresh

Run:  python3 test/save_region_test.py build/wasmcart-run
"""

import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

INFO = 1024  # where each cart keeps its wc_info_t


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


def cart(save_ptr, save_size, moved_ptr=None):
    """A 2D cart, 8x8, one page of memory, whose save region is as given.
    With moved_ptr, wc_init rewrites save_ptr in its wc_info_t."""
    info = struct.pack("<12I", 3, 8, 8, 2048, 0, 0, 0, 0, save_ptr, save_size, 0, 0)
    info += struct.pack("<I", 0)  # flags
    types = vec([b"\x60\x00\x01\x7f", b"\x60\x00\x00"])  # () -> i32, () -> ()
    funcs = vec([leb(0), leb(1), leb(1)])  # wc_get_info, wc_render, wc_init
    mem = vec([b"\x00" + leb(1)])  # min 1 page
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(0),
                   name("wc_render") + b"\x00" + leb(1), name("wc_init") + b"\x00" + leb(2)])

    def body(code):
        b = b"\x00" + code + b"\x0b"  # no locals
        return leb(len(b)) + b

    init = b""
    if moved_ptr is not None:
        init = b"\x41" + sleb(INFO + 32) + b"\x41" + sleb(moved_ptr) + b"\x36\x02\x00"
    code = vec([body(b"\x41" + sleb(INFO)), body(b""), body(init)])
    data = vec([b"\x00\x41" + sleb(INFO) + b"\x0b" + leb(len(info)) + info])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(3, funcs) +
            section(5, mem) + section(7, exports) + section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "regiontest", "abi": 3, "entry": "cart.wasm"}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


def run(binary, cart_path, save_path, seconds=2):
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    try:
        p = subprocess.run(["timeout", "-s", "TERM", str(seconds), binary, cart_path,
                            "--save", save_path], env=env, capture_output=True, text=True, timeout=20)
        return p.returncode, p.stderr
    except subprocess.TimeoutExpired:
        return None, ""


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    failures = []

    def expect(what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail else ""))
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as d:
        def write_cart(name_, data):
            path = os.path.join(d, name_ + ".wasc")
            open(path, "wb").write(data)
            return path

        big = write_cart("big", cart(4096, 8 * 1024 * 1024))
        rc, err = run(binary, big, os.path.join(d, "big.sav"))
        expect("a region over the cap fails to load", rc not in (0, 124) and "save region too large" in err,
               f"exit {rc}")

        out = write_cart("out", cart(65536 - 16, 64))
        rc, err = run(binary, out, os.path.join(d, "out.sav"))
        expect("a region past the end of memory fails to load",
               rc not in (0, 124) and "save region out of bounds" in err, f"exit {rc}")

        ok = write_cart("ok", cart(4096, 64))
        save = os.path.join(d, "ok.sav")
        rc, err = run(binary, ok, save)
        expect("a good region loads and runs", "loaded regiontest" in err and rc in (0, 124),
               f"exit {rc}")

        moved = write_cart("moved", cart(4096, 64, moved_ptr=8192))
        save = os.path.join(d, "moved.sav")
        old = bytes(range(64))
        open(save, "wb").write(old)
        rc, err = run(binary, moved, save)
        expect("a region moved in wc_init leaves the save alone",
               "save region moved during wc_init" in err and open(save, "rb").read() == old)

        # Wrong size: a 5-byte save for a 64-byte region. It isn't loaded, it's
        # kept aside, and the cart's own (all-zero) state isn't written either,
        # since nothing on disk is its save any more.
        save = os.path.join(d, "wrong.sav")
        wrong = write_cart("wrong", cart(4096, 64))
        open(save, "wb").write(b"short")
        rc, err = run(binary, wrong, save)
        kept = os.path.join(d, "wrong.sav.invalid-1")
        expect("a save of the wrong size isn't loaded", "not loaded" in err)
        expect("and is kept as .invalid-1", os.path.exists(kept) and open(kept, "rb").read() == b"short")

    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
