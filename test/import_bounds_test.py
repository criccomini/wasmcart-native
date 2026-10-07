#!/usr/bin/env python3
"""import_bounds_test.py — a cart can't point a host import outside its memory.

Builds tiny carts byte by byte (no toolchain) that call host imports (wc_log,
the asset and peer calls, the WASI calls that take pointers, and a set of GL
calls) with pointers into their own one page of memory that run past
its end, or wrap around 32 bits. Each must trap the cart the way a bad load
or store would: a RangeError naming the import, a non-zero exit, no crash,
and nothing after the bad call runs. Control carts make the same calls with
ranges that end exactly at the end of memory, and must keep running.

The GL cases need a GL context. Without one (SDL's offscreen driver has
none) the player refuses GL carts and those cases are skipped; run the test
under a compositor, e.g. headless labwc, to cover them.

Run:  python3 test/import_bounds_test.py build/wasmcart-run
"""

import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

PAGE = 65536
END = PAGE  # one page of memory: END is one past the last byte
INFO = 1024
SCRATCH = 8192  # pointer arrays, buffer names
STRS = 12288  # log text

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


def load32(addr):
    return i32(addr) + b"\x28\x02\x00"  # i32.load align=4


UNREACHABLE = b"\x00"

# field -> (module, params, results)
SIGS = {
    "wc_log": ("env", [I32] * 2, []),
    "wc_asset_size": ("env", [I32] * 2, [I32]),
    "wc_load_asset": ("env", [I32] * 4, [I32]),
    "emscripten_memcpy_js": ("env", [I32] * 3, []),
    "wc_peer_open": ("env", [I32] * 2, [I32]),
    "wc_peer_send": ("env", [I32] * 3, [I32]),
    "wc_peer_broadcast": ("env", [I32] * 2, [I32]),
    "wc_peer_name": ("env", [I32] * 3, [I32]),
    "fd_write": ("wasi_snapshot_preview1", [I32] * 4, [I32]),
    "clock_time_get": ("wasi_snapshot_preview1", [I32, I64, I32], [I32]),
    "random_get": ("wasi_snapshot_preview1", [I32] * 2, [I32]),
    "fd_read": ("wasi_snapshot_preview1", [I32] * 4, [I32]),
    "fd_fdstat_get": ("wasi_snapshot_preview1", [I32] * 2, [I32]),
    "fd_filestat_get": ("wasi_snapshot_preview1", [I32] * 2, [I32]),
    "environ_sizes_get": ("wasi_snapshot_preview1", [I32] * 2, [I32]),
    "args_sizes_get": ("wasi_snapshot_preview1", [I32] * 2, [I32]),
    "clock_res_get": ("wasi_snapshot_preview1", [I32] * 2, [I32]),
    "poll_oneoff": ("wasi_snapshot_preview1", [I32] * 4, [I32]),
    "glGenBuffers": ("gl", [I32] * 2, []),
    "glBindBuffer": ("gl", [I32] * 2, []),
    "glBufferData": ("gl", [I32] * 4, []),
    "glPixelStorei": ("gl", [I32] * 2, []),
    "glTexImage2D": ("gl", [I32] * 9, []),
    "glReadPixels": ("gl", [I32] * 7, []),
    "glCreateShader": ("gl", [I32], [I32]),
    "glShaderSource": ("gl", [I32] * 4, []),
    "glGetShaderInfoLog": ("gl", [I32] * 4, []),
    "glUniform4fv": ("gl", [I32] * 3, []),
    "glGetIntegerv": ("gl", [I32] * 2, []),
    "glVertexAttribPointer": ("gl", [I32] * 6, []),
    "glEnableVertexAttribArray": ("gl", [I32], []),
    "glDisableVertexAttribArray": ("gl", [I32], []),
    "glDrawArrays": ("gl", [I32] * 3, []),
    "glDrawElements": ("gl", [I32] * 4, []),
    "glClearBufferfv": ("gl", [I32] * 3, []),
    "glFenceSync": ("gl", [I32] * 2, [I32]),
    "glClientWaitSync": ("gl", [I32, I32, I64], [I32]),
    "glDeleteSync": ("gl", [I32], []),
}

GL_ARRAY_BUFFER, GL_PIXEL_UNPACK_BUFFER = 0x8892, 0x88EC
GL_STATIC_DRAW, GL_STREAM_DRAW = 0x88E4, 0x88E0
GL_TEXTURE_2D, GL_RGB, GL_RGBA, GL_UNSIGNED_BYTE = 0x0DE1, 0x1907, 0x1908, 0x1401
GL_UNSIGNED_SHORT, GL_FLOAT, GL_POINTS = 0x1403, 0x1406, 0x0000
GL_UNPACK_ROW_LENGTH, GL_VIEWPORT, GL_MAX_TEXTURE_SIZE = 0x0CF2, 0x0BA2, 0x0D33
GL_VERTEX_SHADER, GL_COLOR, GL_SYNC_GPU_COMMANDS_COMPLETE = 0x8B31, 0x1800, 0x9117


class Cart:
    """One page of memory, wc_get_info/wc_init/wc_render, and whatever
    imports the code calls."""

    def __init__(self):
        self.imports = []
        self.data = []
        self.next_str = STRS
        self.init = b""
        self.render = b""
        self.gl = False

    def call(self, field, *args, keep=False):
        mod, params, results = SIGS[field]
        self.gl |= mod == "gl"
        key = (mod, field)
        if key not in [im[:2] for im in self.imports]:
            self.imports.append((mod, field, params, results))
        idx = [im[:2] for im in self.imports].index(key)
        code = b""
        for p, a in zip(params, args):
            if isinstance(a, bytes):
                code += a
            elif p == I64:
                code += b"\x42" + sleb(a)
            else:
                code += i32(a)
        code += b"\x10" + leb(idx)
        if results and not keep:
            code += b"\x1a"  # drop
        return code

    def put(self, addr, data):
        self.data.append((addr, data))

    def log(self, text):
        b = text.encode()
        addr = self.next_str
        self.next_str += len(b)
        self.put(addr, b)
        return self.call("wc_log", addr, len(b))

    def log_if(self, value, text):
        """Log text if the i32 on the stack equals value."""
        return i32(value) + b"\x46\x04\x40" + self.log(text) + b"\x0b"

    def wasm(self):
        info = struct.pack("<12I", 4, 8, 8, 0 if self.gl else 2048, 0, 0, 0, 0, 0, 0, 0, 0)
        info += struct.pack("<I", 0)
        types = []

        def typeidx(params, results):
            t = b"\x60" + vec([bytes([p]) for p in params]) + vec([bytes([r]) for r in results])
            if t not in types:
                types.append(t)
            return types.index(t)

        imports = [name(m) + name(f) + b"\x00" + leb(typeidx(p, r)) for m, f, p, r in self.imports]
        t_info, t_void = typeidx([], [I32]), typeidx([], [])
        n = len(self.imports)
        funcs = vec([leb(t_info), leb(t_void), leb(t_void)])
        mem = vec([b"\x00" + leb(1)])
        exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(n),
                       name("wc_init") + b"\x00" + leb(n + 1), name("wc_render") + b"\x00" + leb(n + 2)])

        def body(code):
            b = b"\x00" + code + b"\x0b"
            return leb(len(b)) + b

        code = vec([body(i32(INFO)), body(self.init), body(self.render)])
        segs = [(INFO, info)] + self.data
        data = vec([b"\x00" + i32(a) + b"\x0b" + leb(len(d)) + d for a, d in segs])
        return (b"\x00asm\x01\x00\x00\x00" + section(1, vec(types)) + section(2, vec(imports)) +
                section(3, funcs) + section(5, mem) + section(7, exports) + section(10, code) +
                section(11, data))

    def wasc(self):
        z = io.BytesIO()
        with zipfile.ZipFile(z, "w") as f:
            f.writestr("manifest.json", json.dumps({"name": "importtest", "abi": 4}))
            f.writestr("cart.wasm", self.wasm())
            f.writestr("assets/a.bin", bytes(range(64)))
        return z.getvalue()


# ─── Carts that must trap ────────────────────────────────────────────────────
# Each makes one bad call from wc_init (or wc_render), then logs "after trap",
# which must never appear.

def trap(make, in_render=False):
    c = Cart()
    code = make(c) + c.log("after trap")
    if in_render:
        c.render = code
    else:
        c.init = code
    return c


def caught():
    """The cart catches the RangeError (try_table/catch_all). Wasm can't catch
    a real trap, so the host must still stop it."""
    c = Cart()
    bad = c.call("wc_log", END - 3, 4)
    c.init = b"\x02\x40" + b"\x1f\x40\x01\x02\x00" + bad + b"\x0b\x0b" + c.log("caught it")
    return c


def iov(c, buf, n):
    c.put(SCRATCH, struct.pack("<2I", buf, n))
    return SCRATCH


def shader_source_unterminated(c):
    c.put(SCRATCH, struct.pack("<I", END - 4))
    c.put(END - 4, b"abcd")  # no NUL before the end of memory
    return c.call("glShaderSource", c.call("glCreateShader", GL_VERTEX_SHADER, keep=True), 1, SCRATCH, 0)


def client_array_overrun(c):
    # Two vertices of four floats end exactly at END; drawing three reads past it.
    return (c.call("glVertexAttribPointer", 0, 4, GL_FLOAT, 0, 0, END - 32) +
            c.call("glEnableVertexAttribArray", 0) +
            c.call("glDrawArrays", GL_POINTS, 0, 3))


TRAPS = [
    ("wc_log past the end", "wc_log", lambda c: c.call("wc_log", END - 3, 4)),
    ("wc_log wrapping 32 bits", "wc_log", lambda c: c.call("wc_log", 0xFFFFFFF0, 0x20)),
    ("wc_log from wc_render", "wc_log", lambda c: c.call("wc_log", END - 3, 4), True),
    ("emscripten_memcpy_js source", "emscripten_memcpy_js",
     lambda c: c.call("emscripten_memcpy_js", 0, END - 3, 4)),
    ("emscripten_memcpy_js dest", "emscripten_memcpy_js",
     lambda c: c.call("emscripten_memcpy_js", END - 3, 0, 4)),
    ("wc_asset_size path", "wc_asset_size", lambda c: c.call("wc_asset_size", END - 3, 5)),
    ("wc_load_asset path", "wc_load_asset", lambda c: c.call("wc_load_asset", END - 3, 5, 0, 64)),
    # dest + max_size wraps to 0x100, which the old 32-bit check let through.
    ("wc_load_asset dest wrapping 32 bits", "wc_load_asset",
     lambda c: (c.put(STRS - 8, b"a.bin"), c.call("wc_load_asset", STRS - 8, 5, 0xFFFFFF00, 0x200))[1]),
    ("fd_write iovecs", "fd_write", lambda c: c.call("fd_write", 2, END - 4, 1, 0)),
    ("fd_write buffer", "fd_write", lambda c: c.call("fd_write", 2, iov(c, END - 2, 4), 1, 0)),
    ("fd_write nwritten", "fd_write", lambda c: c.call("fd_write", 2, iov(c, STRS, 0), 1, END - 2)),
    ("clock_time_get", "clock_time_get", lambda c: c.call("clock_time_get", 0, 0, END - 4)),
    ("random_get", "random_get", lambda c: c.call("random_get", END - 3, 4)),
    ("fd_read nread", "fd_read", lambda c: c.call("fd_read", 0, 0, 0, END - 3)),
    ("fd_fdstat_get", "fd_fdstat_get", lambda c: c.call("fd_fdstat_get", 1, END - 23)),
    ("fd_filestat_get", "fd_filestat_get", lambda c: c.call("fd_filestat_get", 1, END - 63)),
    ("environ_sizes_get", "environ/args_sizes_get", lambda c: c.call("environ_sizes_get", 0, END - 3)),
    ("args_sizes_get", "environ/args_sizes_get", lambda c: c.call("args_sizes_get", END - 3, 0)),
    ("clock_res_get", "clock_res_get", lambda c: c.call("clock_res_get", 0, END - 7)),
    ("poll_oneoff subscriptions", "poll_oneoff",
     lambda c: c.call("poll_oneoff", END - 47, SCRATCH, 1, SCRATCH + 64)),
    ("poll_oneoff events", "poll_oneoff", lambda c: c.call("poll_oneoff", SCRATCH, END - 31, 1, SCRATCH + 64)),
    ("poll_oneoff nevents", "poll_oneoff", lambda c: c.call("poll_oneoff", SCRATCH, SCRATCH + 64, 1, END - 3)),
    ("wc_peer_open", "wc_peer_open", lambda c: c.call("wc_peer_open", END - 3, 4)),
    ("wc_peer_send", "wc_peer_send", lambda c: c.call("wc_peer_send", 0, END - 3, 4)),
    ("wc_peer_broadcast", "wc_peer_broadcast", lambda c: c.call("wc_peer_broadcast", END - 3, 4)),
    ("wc_peer_name", "wc_peer_name", lambda c: c.call("wc_peer_name", 0, END - 3, 4)),
    # GL
    ("glBufferData past the end", "glBufferData",
     lambda c: c.call("glBufferData", GL_ARRAY_BUFFER, 16, END - 8, GL_STATIC_DRAW)),
    ("glBufferData wrapping 32 bits", "glBufferData",
     lambda c: c.call("glBufferData", GL_ARRAY_BUFFER, 0x20, 0xFFFFFFF0, GL_STATIC_DRAW)),
    ("glGenBuffers", "glGenBuffers", lambda c: c.call("glGenBuffers", 4, END - 12)),
    ("glTexImage2D RGBA", "glTexImage2D",
     lambda c: c.call("glTexImage2D", GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, END - 63)),
    # 3x2 RGB at the default alignment of 4: rows 12 bytes apart, the last
    # one unpadded, so 21 bytes. 20 is one short.
    ("glTexImage2D RGB row padding", "glTexImage2D",
     lambda c: c.call("glTexImage2D", GL_TEXTURE_2D, 0, GL_RGB, 3, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, END - 20)),
    # A row length of 1000 pixels puts the second row 4000 bytes on.
    ("glTexImage2D UNPACK_ROW_LENGTH", "glTexImage2D",
     lambda c: c.call("glPixelStorei", GL_UNPACK_ROW_LENGTH, 1000) +
     c.call("glTexImage2D", GL_TEXTURE_2D, 0, GL_RGBA, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, END - 100)),
    ("glReadPixels", "glReadPixels",
     lambda c: c.call("glReadPixels", 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, END - 63)),
    ("glShaderSource unterminated string", "glShaderSource", shader_source_unterminated),
    ("glGetShaderInfoLog", "glGetShaderInfoLog",
     lambda c: c.call("glGetShaderInfoLog", c.call("glCreateShader", GL_VERTEX_SHADER, keep=True), 64, 0, END - 32)),
    ("glUniform4fv", "glUniform4fv", lambda c: c.call("glUniform4fv", 0xFFFFFFFF, 2, END - 16)),
    ("glGetIntegerv GL_VIEWPORT", "glGetIntegerv", lambda c: c.call("glGetIntegerv", GL_VIEWPORT, END - 12)),
    ("glClearBufferfv GL_COLOR", "glClearBufferfv", lambda c: c.call("glClearBufferfv", GL_COLOR, 0, END - 12)),
    ("glDrawElements client indices", "glDrawElements",
     lambda c: c.call("glDrawElements", GL_POINTS, 8, GL_UNSIGNED_SHORT, END - 8)),
    ("glDrawArrays client array", "glDrawArrays", client_array_overrun),
]


# ─── Carts that must keep running ────────────────────────────────────────────
# The same calls with ranges ending exactly at END. Each cart logs a line per
# step, then traps on purpose in wc_render (unreachable) so it exits by
# itself; that trap must not be a RangeError.

def control_env():
    c = Cart()
    c.put(END - 5, b"a.bin")
    c.init = (c.call("wc_log", END - 5, 5) +
              c.call("emscripten_memcpy_js", END - 16, END - 5, 5) +
              c.call("wc_asset_size", END - 5, 5, keep=True) + c.log_if(64, "ok wc_asset_size") +
              c.call("wc_load_asset", END - 5, 5, END - 128, 64, keep=True) + c.log_if(64, "ok wc_load_asset") +
              c.call("wc_peer_open", END - 5, 5) +
              c.call("wc_peer_send", 0, END - 5, 5) +
              c.call("wc_peer_broadcast", END - 5, 5) +
              c.call("wc_peer_name", 0, END - 32, 32) +
              c.log("ok env"))
    c.render = UNREACHABLE
    return c, ["wasmcart [cart]: a.bin", "ok wc_asset_size", "ok wc_load_asset", "ok env"]


def control_wasi():
    c = Cart()
    c.put(STRS + 512, b"fd_write ok\n")
    c.init = (c.call("fd_write", 2, iov(c, STRS + 512, 12), 1, END - 4) +
              c.call("clock_time_get", 0, 0, END - 8) +
              c.call("random_get", END - 16, 16) +
              c.call("fd_read", 0, 0, 0, END - 4) +
              c.call("fd_fdstat_get", 1, END - 24) +
              c.call("fd_filestat_get", 1, END - 64) +
              c.call("environ_sizes_get", END - 8, END - 4) +
              c.call("args_sizes_get", END - 8, END - 4) +
              c.call("clock_res_get", 0, END - 8) +
              # one clock subscription, all zeros: a relative timeout of 0
              c.call("poll_oneoff", SCRATCH + 128, END - 32, 1, END - 36) +
              c.log("ok wasi"))
    c.render = UNREACHABLE
    return c, ["fd_write ok", "ok wasi"]


def control_gl():
    c = Cart()
    c.put(END - 8, struct.pack("<4H", 0, 1, 1, 0))  # indices, inside the last vertex
    c.init = (
        # client vertex array: two vertices ending at END, drawn directly and
        # through client-side indices at END - 8
        c.call("glVertexAttribPointer", 0, 4, GL_FLOAT, 0, 0, END - 32) +
        c.call("glEnableVertexAttribArray", 0) +
        c.call("glDrawArrays", GL_POINTS, 0, 2) +
        c.call("glDrawElements", GL_POINTS, 4, GL_UNSIGNED_SHORT, END - 8) +
        c.call("glDisableVertexAttribArray", 0) +
        c.log("ok client arrays") +
        # reads ending at END
        c.call("glGenBuffers", 1, SCRATCH) +
        c.call("glBindBuffer", GL_ARRAY_BUFFER, load32(SCRATCH)) +
        c.call("glBufferData", GL_ARRAY_BUFFER, 16, END - 16, GL_STATIC_DRAW) +
        c.call("glTexImage2D", GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, END - 64) +
        c.call("glTexImage2D", GL_TEXTURE_2D, 0, GL_RGB, 3, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, END - 21) +
        c.call("glUniform4fv", 0xFFFFFFFF, 1, END - 16) +
        c.log("ok reads") +
        # with a pixel unpack buffer bound the pointer is an offset into it,
        # not cart memory: GL rejects this one, the host mustn't trap
        c.call("glGenBuffers", 1, SCRATCH + 4) +
        c.call("glBindBuffer", GL_PIXEL_UNPACK_BUFFER, load32(SCRATCH + 4)) +
        c.call("glBufferData", GL_PIXEL_UNPACK_BUFFER, 64, 0, GL_STREAM_DRAW) +
        c.call("glTexImage2D", GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0xFFFFFF00) +
        c.call("glBindBuffer", GL_PIXEL_UNPACK_BUFFER, 0) +
        c.log("ok unpack buffer") +
        # writes ending at END
        c.call("glGenBuffers", 1, END - 4) +
        c.call("glGetIntegerv", GL_MAX_TEXTURE_SIZE, END - 4) +
        c.call("glGetIntegerv", GL_VIEWPORT, END - 16) +
        c.call("glReadPixels", 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, END - 64) +
        c.log("ok writes") +
        # a sync is a handle, not a pointer, and the timeout is an i64
        c.call("glClientWaitSync", c.call("glFenceSync", GL_SYNC_GPU_COMMANDS_COMPLETE, 0, keep=True), 0, 0) +
        c.call("glDeleteSync", 0x41414141) +
        c.log("ok sync"))
    c.render = UNREACHABLE
    return c, ["ok client arrays", "ok reads", "ok unpack buffer", "ok writes", "ok sync"]


CONTROLS = [("env imports in bounds", control_env), ("WASI imports in bounds", control_wasi),
            ("GL imports in bounds", control_gl)]


def run(binary, cart, d):
    path = os.path.join(d, "c.wasc")
    with open(path, "wb") as f:
        f.write(cart.wasc())
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    p = subprocess.run(["timeout", "-s", "TERM", "5", binary, path, "--save", os.path.join(d, "s")],
                       env=env, capture_output=True, text=True)
    return p.returncode, p.stderr


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    failures, skipped = [], 0
    with tempfile.TemporaryDirectory() as d:
        for case in TRAPS:
            label, imp, make = case[:3]
            cart = trap(make, *case[3:])
            rc, err = run(binary, cart, d)
            if cart.gl and "no GL context" in err:
                print(f"  skip  {label}: no GL context")
                skipped += 1
                continue
            crashed = rc < 0 or rc in (124, 134, 139)
            ok = (not crashed and rc != 0 and f"RangeError: {imp}:" in err and
                  "after trap" not in err)
            print(f"  {'ok  ' if ok else 'FAIL'}  {label}: exit {rc}")
            if not ok:
                failures.append(label)
                print("    " + "\n    ".join(err.splitlines()[-6:]))

        cart = caught()
        rc, err = run(binary, cart, d)
        ok = (rc == 1 and "RangeError: wc_log:" in err and "caught it" in err and
              "cart trapped, exiting" in err)
        print(f"  {'ok  ' if ok else 'FAIL'}  the cart catches the RangeError and is stopped anyway: exit {rc}")
        if not ok:
            failures.append("caught")
            print("    " + "\n    ".join(err.splitlines()[-6:]))

        for label, make in CONTROLS:
            cart, markers = make()
            rc, err = run(binary, cart, d)
            if cart.gl and "no GL context" in err:
                print(f"  skip  {label}: no GL context")
                skipped += 1
                continue
            missing = [m for m in markers if m not in err]
            crashed = rc < 0 or rc in (124, 134, 139)
            ok = not crashed and not missing and "RangeError" not in err and "unreachable" in err
            print(f"  {'ok  ' if ok else 'FAIL'}  {label}: exit {rc}")
            if not ok:
                failures.append(label)
                if missing:
                    print(f"    missing: {missing}")
                print("    " + "\n    ".join(err.splitlines()[-6:]))

    if failures:
        print(f"\nFAILED: {', '.join(failures)}")
        return 1
    print("\nall checks passed" + (f" ({skipped} GL cases skipped: no GL context)" if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
