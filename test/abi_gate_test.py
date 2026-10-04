#!/usr/bin/env python3
"""abi_gate_test.py — the player runs ABI v4 carts and refuses v1-v3 ones.

v4 moved every wc_pad_t field after buttons, so an older cart would read its
input from the wrong bytes. It must be refused with "ABI version mismatch"
and exit 1, never run or crash.

Builds tiny carts byte by byte:
- a v3 cart whose wc_info_t says 3 from the start, and whose wc_init traps:
  refused before wc_init runs, so the trap is never logged;
- a v3 cart that only sets its version in wc_init: refused after it;
- a v4 cart: runs.
Real carts can be added: a v3 one is refused, a v4 one runs.

Run:  python3 test/abi_gate_test.py build/wasmcart-run [v3.wasc] [v4.wasc]
"""

import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

INFO = 1024


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


def cart(static_version, init_code, abi):
    """One page; an 8x8 2D cart. init_code is wc_init's body."""
    info = struct.pack("<18I", static_version, 8, 8, 2048, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    types = vec([b"\x60\x00\x01\x7f", b"\x60\x00\x00"])
    funcs = vec([leb(0), leb(1), leb(1)])
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(0),
                   name("wc_render") + b"\x00" + leb(1), name("wc_init") + b"\x00" + leb(2)])

    def body(code):
        b = b"\x00" + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(b"\x41" + sleb(INFO)), body(b""), body(init_code)])
    data = vec([b"\x00\x41" + sleb(INFO) + b"\x0b" + leb(len(info)) + info])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(3, funcs) +
            section(5, mem) + section(7, exports) + section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "abitest", "abi": abi}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


TRAP = b"\x00"  # unreachable


def set_version(v):  # i32.store (INFO) v
    return b"\x41" + sleb(INFO) + b"\x41" + sleb(v) + b"\x36\x02\x00"


def run(binary, path, env, d):
    p = subprocess.run(["timeout", "-s", "TERM", "3", binary, path, "--save", os.path.join(d, "s")],
                       env=env, capture_output=True, text=True)
    return p.returncode, p.stderr


def refused(rc, log, cart_version):
    return (rc == 1 and f"ABI version mismatch: cart={cart_version}," in log
            and "failed to load" in log and "wasmcart: running " not in log)


def main():
    if len(sys.argv) not in (2, 4):
        print(__doc__)
        return 2
    binary = sys.argv[1]
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    failures = []

    def check(label, ok, log):
        print(f"  {'ok  ' if ok else 'FAIL'}  {label}")
        if not ok:
            failures.append(label)
            print("    " + "\n    ".join(log.splitlines()[-5:]))

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "c.wasc")

        open(path, "wb").write(cart(3, TRAP, 3))
        rc, log = run(binary, path, env, d)
        check(f"a v3 cart is refused before wc_init (exit {rc})",
              refused(rc, log, 3) and "wc_init error" not in log, log)

        open(path, "wb").write(cart(0, set_version(3), 3))
        rc, log = run(binary, path, env, d)
        check(f"a v3 cart that sets its version in wc_init is refused (exit {rc})",
              refused(rc, log, 3), log)

        open(path, "wb").write(cart(2, b"", 2))
        rc, log = run(binary, path, env, d)
        check(f"a v2 cart is refused (exit {rc})", refused(rc, log, 2), log)

        open(path, "wb").write(cart(5, b"", 5))
        rc, log = run(binary, path, env, d)
        check(f"a v5 cart is refused (exit {rc})", refused(rc, log, 5), log)

        open(path, "wb").write(cart(4, b"", 4))
        rc, log = run(binary, path, env, d)
        check(f"a v4 cart runs (exit {rc})",
              "wasmcart: running " in log and "ABI version mismatch" not in log, log)

        if len(sys.argv) == 4:
            v3, v4 = sys.argv[2], sys.argv[3]
            rc, log = run(binary, v3, env, d)
            check(f"{os.path.basename(v3)} (v3) is refused (exit {rc})",
                  rc == 1 and "ABI version mismatch" in log and "wasmcart: running " not in log, log)
            rc, log = run(binary, v4, env, d)
            check(f"{os.path.basename(v4)} (v4) runs (exit {rc})",
                  "wasmcart: running " in log and "ABI version mismatch" not in log, log)

    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
