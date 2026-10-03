#!/usr/bin/env python3
"""no_net_test.py — --no-net refuses a host the manifest grants.

Builds a tiny cart byte by byte that sets WC_FLAG_NET_PEER, is granted
127.0.0.1 in its manifest, and in wc_init dials ws://127.0.0.1:<port>/ and
logs whether wc_peer_open gave it a peer. A local TCP server stands in for
the far end. The cart runs twice against the same server: without --no-net
it must get a peer and the server must see the WebSocket handshake, so the
refusal that follows can't be the server being down; with --no-net it must
get -1 and the server must see nothing.

Run:  python3 test/no_net_test.py build/wasmcart-run
"""

import io
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

INFO = 1024
ADDR = 2048
OPENED = 2304
REFUSED = 2336
FB = 4096
WC_FLAG_NET_PEER = 1 << 1


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


def i32(n):
    return b"\x41" + sleb(n)


def cart(addr):
    opened, refused = b"peer open: ok", b"peer open: refused"
    # version, width, height, fb, audio, audio_cap, audio_write, input,
    # save, save_size, time, host_info, flags
    info = struct.pack("<13I", 3, 8, 8, FB, 0, 0, 0, 0, 0, 0, 0, 0, WC_FLAG_NET_PEER)
    types = vec([b"\x60\x02\x7f\x7f\x01\x7f",   # 0: wc_peer_open(ptr, len) -> id
                 b"\x60\x02\x7f\x7f\x00",       # 1: wc_log(ptr, len)
                 b"\x60\x00\x01\x7f",           # 2: wc_get_info() -> ptr
                 b"\x60\x00\x00"])              # 3: wc_init(), wc_render()
    imports = vec([name("env") + name("wc_peer_open") + b"\x00" + leb(0),
                   name("env") + name("wc_log") + b"\x00" + leb(1)])
    funcs = vec([leb(2), leb(3), leb(3)])      # functions 2, 3 and 4
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0),
                   name("wc_get_info") + b"\x00" + leb(2),
                   name("wc_init") + b"\x00" + leb(3),
                   name("wc_render") + b"\x00" + leb(4)])

    def body(code):
        b = b"\x00" + code + b"\x0b"
        return leb(len(b)) + b

    init = (i32(ADDR) + i32(len(addr)) + b"\x10" + leb(0)       # call wc_peer_open
            + i32(0) + b"\x48"                                   # i32.lt_s
            + b"\x04\x40"                                        # if
            + i32(REFUSED) + i32(len(refused)) + b"\x10" + leb(1)
            + b"\x05"                                            # else
            + i32(OPENED) + i32(len(opened)) + b"\x10" + leb(1)
            + b"\x0b")                                           # end
    code = vec([body(i32(INFO)), body(init), body(b"")])
    segs = [(INFO, info), (ADDR, addr.encode()), (OPENED, opened), (REFUSED, refused)]
    data = vec([b"\x00" + i32(a) + b"\x0b" + leb(len(d)) + d for a, d in segs])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(2, imports) +
            section(3, funcs) + section(5, mem) + section(7, exports) +
            section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps(
            {"name": "nonettest", "abi": 3, "net": {"domains": ["127.0.0.1"]}}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


class Server:
    """Accepts connections and keeps what each sends first."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.seen = []
        self.busy = threading.Event()  # a connection is being read
        self.stop = False
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while not self.stop:
            try:
                c, _ = self.sock.accept()
            except socket.timeout:
                continue
            self.busy.set()
            c.settimeout(2)
            try:
                self.seen.append(c.recv(4096))
            except OSError:
                self.seen.append(b"")
            c.close()
            self.busy.clear()

    def take(self):
        """What arrived during the run just ended. A connection made just
        before the runner died still waits in the backlog: give the accept
        loop time to reach it, and the read time to finish, or a dial the
        switch should have stopped could go unseen."""
        time.sleep(0.5)
        while self.busy.is_set():
            time.sleep(0.05)
        seen, self.seen = self.seen, []
        return seen


def run(binary, path, extra):
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    return subprocess.run(["timeout", "-s", "TERM", "3", binary, path] + extra,
                          env=env, capture_output=True, text=True)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    srv = Server()
    failures = []

    def check(what, ok, p=None):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}")
        if not ok:
            failures.append(what)
            if p is not None:
                print("    " + "\n    ".join(p.stderr.splitlines()[-6:]))

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "c.wasc")
        open(path, "wb").write(cart(f"ws://127.0.0.1:{srv.port}/"))

        p = run(binary, path, [])
        seen = srv.take()
        check("granted: wc_peer_open gives a peer", "peer open: ok" in p.stderr, p)
        check("granted: the server sees the handshake",
              any(b"upgrade: websocket" in s.lower() for s in seen), p)

        p = run(binary, path, ["--no-net"])
        seen = srv.take()
        check("--no-net: wc_peer_open returns -1", "peer open: refused" in p.stderr, p)
        check("--no-net: the runner says why", "networking is off for this cart" in p.stderr, p)
        check("--no-net: the server sees nothing", seen == [], p)
    srv.stop = True
    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
