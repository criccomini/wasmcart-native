#!/usr/bin/env python3
"""pad_input_test.py — a real pad's input lands where an ABI v4 cart reads it.

Plugs two uinput Xbox 360 pads into wasmcart-run running a cart built here
that logs its whole pad array (4 x 20 bytes) as hex every frame, then
decodes it with v4's layout: u32 buttons, int16 sticks, int16 triggers
(0..32767), connected at byte 16, and pad 1 at byte 20.

Checked twice:
- plain: two pads in slots 0 and 1, every button the pad sends (Guide
  included, bit 14), triggers at full travel read 32767;
- under Couchmix (COUCHMIX_PAD_SLOTS set) with a one-player cart: Guide is
  the supervisor's and never reaches the cart, both pads' input is merged
  into slot 0, and each pad's slot goes down the heartbeat fd as an L line;
  then a pause and resume (SIGUSR1, SIGUSR2) with the buttons still held:
  they stay masked until released, and a fresh press gets through.

Linux only. Needs write access to /dev/uinput and python3-evdev; in a
container, /dev/input has to be the host's so SDL sees the new nodes.

Run:  python3 test/pad_input_test.py build/wasmcart-run
"""

import io
import json
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

from evdev import AbsInfo, UInput, ecodes as e

INFO, PADS, HEX, FB = 1024, 1280, 2048, 4096
PREFIX = b"pads "
PAD = struct.Struct("<IhhhhhhB3x")  # ABI v4 wc_pad_t
BTN_A, BTN_B, BTN_GUIDE = 1 << 0, 1 << 1, 1 << 14


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


def pad_cart(players):
    info = struct.pack("<18I", 4, 8, 8, FB, 0, 0, 0, PADS, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    types = vec([b"\x60\x02\x7f\x7f\x00", b"\x60\x00\x01\x7f", b"\x60\x00\x00"])
    imports = vec([name("env") + name("wc_log") + b"\x00" + leb(0)])
    funcs = vec([leb(1), leb(2)])  # functions 1 (wc_get_info) and 2 (wc_render)
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(1),
                   name("wc_render") + b"\x00" + leb(2)])

    # x (local 2) -> its hex digit, left on the stack
    digit = b"\x20\x02" + i32(48) + i32(87) + b"\x20\x02" + i32(10) + b"\x49\x1b\x6a"

    def nibble(shift_or_mask, off):
        return (b"\x20\x01" + shift_or_mask + b"\x21\x02"          # x = ...
                + b"\x20\x00" + i32(2) + b"\x6c"                    # addr = i * 2
                + digit + b"\x3a\x00" + leb(HEX + len(PREFIX) + off))  # i32.store8

    render = (
        b"\x02\x40\x03\x40"                                          # block, loop
        + b"\x20\x00" + i32(80) + b"\x4f\x0d\x01"                    # i >= 80: out
        + b"\x20\x00\x2d\x00" + leb(PADS) + b"\x21\x01"              # b = pads[i]
        + nibble(i32(4) + b"\x76", 0)                                # b >> 4
        + nibble(i32(15) + b"\x71", 1)                               # b & 15
        + b"\x20\x00" + i32(1) + b"\x6a\x21\x00"                     # i++
        + b"\x0c\x00\x0b\x0b"                                        # br loop, end, end
        + i32(HEX) + i32(len(PREFIX) + 160) + b"\x10\x00"            # wc_log
    )

    def body(locals_, code):
        b = locals_ + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(b"\x00", i32(INFO)), body(b"\x01\x03\x7f", render)])
    data = vec([b"\x00" + i32(a) + b"\x0b" + leb(len(d)) + d for a, d in [(INFO, info), (HEX, PREFIX)]])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(2, imports) +
            section(3, funcs) + section(5, mem) + section(7, exports) +
            section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "padtest", "abi": 4, "players": players}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


def make_pad(phys):
    """A wired Xbox 360 pad, which SDL's built-in mappings cover."""
    stick = AbsInfo(0, -32768, 32767, 16, 128, 0)
    trig = AbsInfo(0, 0, 255, 0, 0, 0)
    hat = AbsInfo(0, -1, 1, 0, 0, 0)
    caps = {
        e.EV_KEY: [e.BTN_A, e.BTN_B, e.BTN_X, e.BTN_Y, e.BTN_TL, e.BTN_TR, e.BTN_SELECT,
                   e.BTN_START, e.BTN_MODE, e.BTN_THUMBL, e.BTN_THUMBR],
        e.EV_ABS: [(e.ABS_X, stick), (e.ABS_Y, stick), (e.ABS_Z, trig), (e.ABS_RX, stick),
                   (e.ABS_RY, stick), (e.ABS_RZ, trig), (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat)],
    }
    return UInput(caps, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E,
                  version=0x110, bustype=e.BUS_USB, phys=phys)


LINE = re.compile(r"pads ([0-9a-f]{160})")


class Runner:
    def __init__(self, binary, cart, couchmix):
        env = dict(os.environ)
        env.setdefault("SDL_VIDEODRIVER", "dummy")
        env.setdefault("SDL_AUDIODRIVER", "dummy")
        env["COUCHMIX_KEYBOARD_PAD"] = "0"
        env.pop("COUCHMIX_PAD_SLOTS", None)
        self.hb, fds = None, ()
        if couchmix:
            env["COUCHMIX_PAD_SLOTS"] = ""
            r, w = os.pipe()
            env["COUCHMIX_HEARTBEAT_FD"] = str(w)
            self.hb, fds = os.fdopen(r), (w,)
        if not os.path.exists("/run/udev/control"):
            env.setdefault("SDL_JOYSTICK_DISABLE_UDEV", "1")
        self.proc = subprocess.Popen([binary, cart], env=env, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, text=True, pass_fds=fds)
        for fd in fds:
            os.close(fd)
        self.slots = []  # (slot, connected) from L lines
        if self.hb:
            threading.Thread(target=self._read_hb, daemon=True).start()
        self.last = None
        self.connected = 0
        self.tail = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stderr:
            m = LINE.search(line)
            if m:
                self.last = bytes.fromhex(m.group(1))
                continue
            if "controller" in line and "connected" in line and "disconnected" not in line:
                self.connected += 1
            self.tail = (self.tail + [line.rstrip()])[-20:]

    def _read_hb(self):
        for line in self.hb:
            parts = line.split()
            if len(parts) == 5 and parts[0] == "L":
                self.slots.append((int(parts[1]), parts[3] == "1"))

    def pads(self):
        if self.last is None:
            return None
        return [PAD.unpack_from(self.last, 20 * i) for i in range(4)]

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def press(a, b):
    a.write(e.EV_KEY, e.BTN_A, 1)
    a.write(e.EV_KEY, e.BTN_MODE, 1)
    a.write(e.EV_ABS, e.ABS_Z, 255)       # left trigger, full travel
    a.write(e.EV_ABS, e.ABS_RZ, 128)      # right trigger, about half
    a.write(e.EV_ABS, e.ABS_X, 32767)     # left stick right
    a.write(e.EV_ABS, e.ABS_RY, -32768)   # right stick up
    a.syn()
    b.write(e.EV_KEY, e.BTN_B, 1)
    b.syn()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    failures = []

    def check(what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail and not ok else ""))
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as d:
        for couchmix, players in ((False, 2), (True, 1)):
            mode = "under Couchmix, one player" if couchmix else "plain, two players"
            print(f"{mode}:")
            path = os.path.join(d, f"pads{players}.wasc")
            open(path, "wb").write(pad_cart(players))
            a, b = make_pad("pad-input/a"), make_pad("pad-input/b")
            time.sleep(1.0)
            r = Runner(sys.argv[1], path, couchmix)
            try:
                deadline = time.time() + 8
                while time.time() < deadline and (r.connected < 2 or r.last is None):
                    time.sleep(0.1)
                if r.connected < 2 or r.last is None:
                    check("runner up with two pads", False, "\n    " + "\n    ".join(r.tail))
                    continue
                press(a, b)
                time.sleep(1.0)
                p = r.pads()
                (b0, lx, ly, rx, ry, lt, rt, c0), p1 = p[0], p[1]
                if not couchmix:
                    check("pad 0 connected, at byte 16", c0 == 1, p[0])
                    check("pad 0: A and Guide (bit 14) in u32 buttons", b0 == BTN_A | BTN_GUIDE, hex(b0))
                    check("pad 0: left trigger at full travel is 32767", lt == 32767, lt)
                    check("pad 0: right trigger at half travel", 12000 < rt < 21000, rt)
                    check("pad 0: sticks", lx > 32000 and ry < -32000 and abs(ly) < 1000 and abs(rx) < 1000,
                          (lx, ly, rx, ry))
                    check("pad 1 at byte 20: B, connected", p1[0] == BTN_B and p1[7] == 1, p1)
                    check("pads 2 and 3 empty", p[2][7] == 0 and p[3][7] == 0, p[2:])
                else:
                    check("slot 0: A and B merged, Guide withheld", b0 == BTN_A | BTN_B, hex(b0))
                    check("slot 0: triggers and sticks merged", lt == 32767 and 12000 < rt < 21000
                          and lx > 32000 and ry < -32000, (lx, ly, rx, ry, lt, rt))
                    check("slots 1-3 empty", all(q[7] == 0 and q[0] == 0 for q in p[1:]), p[1:])
                    check("L lines: slots 0 and 1 connected", sorted(r.slots) == [(0, True), (1, True)],
                          r.slots)
                    r.proc.send_signal(signal.SIGUSR1)
                    time.sleep(0.5)
                    r.proc.send_signal(signal.SIGUSR2)
                    time.sleep(0.5)
                    b0 = r.pads()[0][0]
                    check("after a resume, buttons still held are masked", b0 == 0, hex(b0))
                    a.write(e.EV_KEY, e.BTN_A, 0)
                    a.syn()
                    time.sleep(0.3)
                    a.write(e.EV_KEY, e.BTN_A, 1)
                    a.syn()
                    time.sleep(0.5)
                    b0 = r.pads()[0][0]
                    check("a fresh press gets through; B, still held, doesn't", b0 == BTN_A, hex(b0))
            finally:
                r.stop()
                a.close()
                b.close()
                time.sleep(0.5)
    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
