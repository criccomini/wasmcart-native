#!/usr/bin/env python3
"""rumble_rate_test.py — a held rumble reaches the pad at a steady trickle.

Carts sustain a rumble by asking for it again every frame. The pad should get
the strength once, the same strength again every half second or so while the
cart keeps asking, and a stop when the cart stops: not a request per frame,
and not one so rarely that a pad which needs feeding goes quiet. (hid-nintendo
feeds a Switch pad for only a few hundred ms after each effect it's handed.)

Plugs in a uinput gamepad with force feedback, runs wasmcart-run on a cart
built here, presses its buttons, and records every effect the kernel hands the
pad. The cart:
  - while A is held, asks for (1.0, 1.0) for 100 ms every frame, and on release
    just stops asking, so the rumble has to end about 100 ms later;
  - while B is held, asks for (0.5, 0.25) for 5000 ms every frame, and on
    release calls wc_pad_rumble_stop, so it has to end at once.
For each hold it checks the rumble starts promptly at the right strength, stays
on the whole hold, is refreshed at least every 650 ms, reaches the pad fewer
than 3 times a second, and stops when it should.

Linux only. Needs write access to /dev/uinput and python3-evdev. In a
container, /dev/input has to be the host's (e.g. -v /dev/input:/dev/input).

With --switch the pad poses as a Switch Pro Controller over Bluetooth. A
uinput pad has no hidraw node, so the player can't write its rumble report
itself (src/switch_rumble.h) and has to fall back to SDL like any other pad.

Run:  python3 test/rumble_rate_test.py build/wasmcart-run [--switch]
"""

import io
import json
import os
import select
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

from evdev import AbsInfo, UInput, ecodes as e

HOLD_A, HOLD_B = 3.0, 1.5  # seconds
FED_EVERY = 0.65           # the longest the pad may go without its rumble during a hold
MAX_RATE = 3.0             # uploads (and plays) per second during a hold


# ── The cart ──

def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | 0x80 if n else b)
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


def i32(n):
    return b"\x41" + sleb(n)


def f32(x):
    return b"\x43" + struct.pack("<f", x)


def rumble_cart():
    info_at, fb_at, input_at, state_at = 1024, 2048, 4096, 8192
    # version, width, height, fb, audio, audio_cap, audio_write, input, save,
    # save_size, time, host_info, flags, audio_sample_rate, pointer, keys,
    # gpu_api, wheel
    info = struct.pack("<18I", 4, 8, 8, fb_at, 0, 0, 0, input_at, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    types = vec([b"\x60\x00\x01\x7f",                  # () -> i32
                 b"\x60\x00\x00",                      # () -> ()
                 b"\x60\x04\x7f\x7d\x7d\x7f\x00",      # wc_pad_rumble(i32, f32, f32, i32)
                 b"\x60\x01\x7f\x00"])                 # wc_pad_rumble_stop(i32)
    imports = vec([name("env") + name("wc_pad_rumble") + b"\x00" + leb(2),
                   name("env") + name("wc_pad_rumble_stop") + b"\x00" + leb(3)])
    funcs = vec([leb(0), leb(1)])                      # functions 2 and 3
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(2),
                   name("wc_render") + b"\x00" + leb(3)])
    buttons = i32(input_at) + b"\x28\x02\x00"          # pad 0's buttons (i32.load)
    render = (
        # A: (1.0, 1.0) for 100 ms, every frame it's held
        buttons + i32(1) + b"\x71" + b"\x04\x40"
        + i32(0) + f32(1.0) + f32(1.0) + i32(100) + b"\x10\x00"
        + b"\x0b"
        # B: (0.5, 0.25) for 5000 ms every frame it's held; stop on release
        + buttons + i32(2) + b"\x71" + b"\x04\x40"
        + i32(0) + f32(0.5) + f32(0.25) + i32(5000) + b"\x10\x00"
        + i32(state_at) + i32(1) + b"\x36\x02\x00"
        + b"\x05"
        + i32(state_at) + b"\x28\x02\x00" + b"\x04\x40"
        + i32(0) + b"\x10\x01"
        + i32(state_at) + i32(0) + b"\x36\x02\x00"
        + b"\x0b"
        + b"\x0b"
    )

    def body(code):
        b = b"\x00" + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(i32(info_at)), body(render)])
    data = vec([b"\x00" + i32(info_at) + b"\x0b" + leb(len(info)) + info])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(2, imports) +
            section(3, funcs) + section(5, mem) + section(7, exports) + section(10, code) +
            section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "rumblerate", "abi": 4}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


# ── The pad ──

def make_pad(switch=False):
    stick = AbsInfo(0, -32768, 32767, 16, 128, 0)
    trig = AbsInfo(0, 0, 255, 0, 0, 0)
    hat = AbsInfo(0, -1, 1, 0, 0, 0)
    caps = {
        e.EV_KEY: [e.BTN_A, e.BTN_B, e.BTN_X, e.BTN_Y, e.BTN_TL, e.BTN_TR, e.BTN_SELECT,
                   e.BTN_START, e.BTN_MODE, e.BTN_THUMBL, e.BTN_THUMBR],
        e.EV_ABS: [(e.ABS_X, stick), (e.ABS_Y, stick), (e.ABS_Z, trig), (e.ABS_RX, stick),
                   (e.ABS_RY, stick), (e.ABS_RZ, trig), (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat)],
        e.EV_FF: [e.FF_RUMBLE, e.FF_GAIN],
    }
    if switch:
        return UInput(caps, name="Pro Controller", vendor=0x057E, product=0x2009,
                      version=0x8011, bustype=e.BUS_BLUETOOTH, max_effects=16)
    return UInput(caps, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E,
                  version=0x110, bustype=e.BUS_USB, max_effects=16)


class Pad:
    """Answers the kernel's force-feedback requests and records them."""

    def __init__(self, switch=False):
        self.dev = make_pad(switch)
        self.log = []  # (monotonic, kind, ...)

    def service(self, until):
        while True:
            left = until - time.monotonic()
            if left <= 0:
                return
            r, _, _ = select.select([self.dev.fd], [], [], min(left, 0.05))
            if not r:
                continue
            for ev in self.dev.read():
                now = time.monotonic()
                if ev.type == e.EV_UINPUT and ev.code == e.UI_FF_UPLOAD:
                    up = self.dev.begin_upload(ev.value)
                    up.retval = 0
                    eff = up.effect
                    if eff.type == e.FF_RUMBLE:
                        rm = eff.u.ff_rumble_effect
                        self.log.append((now, "upload", eff.id, rm.strong_magnitude,
                                         rm.weak_magnitude, eff.ff_replay.length))
                    self.dev.end_upload(up)
                elif ev.type == e.EV_UINPUT and ev.code == e.UI_FF_ERASE:
                    er = self.dev.begin_erase(ev.value)
                    er.retval = 0
                    self.dev.end_erase(er)
                    self.log.append((now, "erase", er.effect_id))
                elif ev.type == e.EV_FF and ev.code != e.FF_GAIN:
                    self.log.append((now, "play", ev.code, ev.value))

    def button(self, code, down):
        self.dev.write(e.EV_KEY, code, 1 if down else 0)
        self.dev.syn()
        return time.monotonic()


# ── What the pad saw ──

def timeline(log):
    """[(t, strength)] at each change in what the pad is playing: (strong, weak),
    or None for nothing. An effect plays from a play request until a stop
    request, an erase, or the end of its replay length; an upload to a playing
    effect changes it in place and restarts its length, as ff-memless does."""
    effects, ends, out, cur = {}, {}, [], None  # id -> (strong, weak, length); id -> end

    def end_of(i, t):
        length = effects.get(i, (0, 0, 0))[2]
        return t + length / 1000 if length else float("inf")

    def playing(t):
        for i, end in ends.items():
            s = effects.get(i)
            if s and end > t and (s[0] or s[1]):
                return s[:2]
        return None

    def note(t):
        nonlocal cur
        now = playing(t)
        if now != cur:
            out.append((t, now))
            cur = now

    for rec in log:
        t, kind = rec[0], rec[1]
        for end in sorted(x for x in ends.values() if x <= t):
            note(end)  # a length that ran out before this request
        if kind == "upload":
            effects[rec[2]] = rec[3:6]
            if rec[2] in ends:
                ends[rec[2]] = end_of(rec[2], t)
        elif kind == "play" and rec[3]:
            ends[rec[2]] = end_of(rec[2], t)
        elif kind == "play":
            ends.pop(rec[2], None)
        elif kind == "erase":
            ends.pop(rec[2], None)
            effects.pop(rec[2], None)
        note(t)
    return out


def check_hold(what, log, pressed, released, want, stop_within, failures):
    ups = [r for r in log if r[1] == "upload" and pressed <= r[0] <= released]
    plays = [r for r in log if r[1] == "play" and r[3] and pressed <= r[0] <= released]
    tl = timeline(log)
    on = [t for t, s in tl if s and pressed <= t <= released]
    held = released - pressed
    if not on:
        failures.append(f"{what}: the pad never rumbled")
        return
    start = on[0] - pressed
    first = next(s for t, s in tl if t == on[0])
    print(f"  {what}: on {start * 1000:.0f} ms after the press at {first}; "
          f"{len(ups)} uploads and {len(plays)} plays in {held:.1f} s")
    print(f"  {what}: uploads (strong, weak): {[r[3:5] for r in ups]}")
    if start > 0.15:
        failures.append(f"{what}: started {start * 1000:.0f} ms after the press")
    if any(abs(a - b) > 2 for a, b in zip(first, want)):
        failures.append(f"{what}: strength {first}, want about {want}")
    off = [t for t, s in tl if s is None and on[0] < t <= released]
    if off:
        failures.append(f"{what}: stopped {(off[0] - pressed) * 1000:.0f} ms into the hold")
    for kind, n in (("uploads", len(ups)), ("plays", len(plays))):
        if n / held >= MAX_RATE:
            failures.append(f"{what}: {n} {kind} in {held:.1f} s, {n / held:.1f} a second")
    fed = [r[0] for r in plays] + [released]
    gap = max(b - a for a, b in zip(fed, fed[1:])) if len(fed) > 1 else held
    print(f"  {what}: longest the pad went without its rumble: {gap * 1000:.0f} ms")
    if gap > FED_EVERY:
        failures.append(f"{what}: {gap * 1000:.0f} ms without a refresh during the hold")
    ends = [t for t, s in tl if s is None and t > released]
    if not ends:
        failures.append(f"{what}: never stopped after the release")
        return
    late = ends[0] - released
    print(f"  {what}: off {late * 1000:.0f} ms after the release")
    lo, hi = stop_within
    if not lo <= late <= hi:
        failures.append(f"{what}: stopped {late * 1000:.0f} ms after the release, "
                        f"want {lo * 1000:.0f}-{hi * 1000:.0f} ms")


def main():
    args = [a for a in sys.argv[1:] if a != "--switch"]
    switch = "--switch" in sys.argv[1:]
    if len(args) != 1:
        print(__doc__)
        return 2
    pad = Pad(switch)
    time.sleep(1.0)
    with tempfile.TemporaryDirectory() as d:
        cart = os.path.join(d, "rumblerate.wasc")
        open(cart, "wb").write(rumble_cart())
        env = dict(os.environ)
        env.setdefault("SDL_VIDEODRIVER", "dummy")
        env.setdefault("SDL_AUDIODRIVER", "dummy")
        if not os.path.exists("/run/udev/control"):
            env.setdefault("SDL_JOYSTICK_DISABLE_UDEV", "1")  # see pad_hotplug_test.py
        proc = subprocess.Popen([args[0], cart], env=env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, text=True)
        err = []
        connected = threading.Event()

        def read_err():
            for line in proc.stderr:
                err.append(line.rstrip())
                if "controller 0 connected" in line:
                    connected.set()

        threading.Thread(target=read_err, daemon=True).start()
        end = time.monotonic() + 15
        while not connected.is_set() and time.monotonic() < end:
            pad.service(time.monotonic() + 0.1)
        failures = []
        if not connected.is_set():
            failures.append("the player never opened the pad")
        else:
            pad.service(time.monotonic() + 0.5)
            a_down = pad.button(e.BTN_A, True)
            pad.service(a_down + HOLD_A)
            a_up = pad.button(e.BTN_A, False)
            pad.service(a_up + 1.0)
            b_down = pad.button(e.BTN_B, True)
            pad.service(b_down + HOLD_B)
            b_up = pad.button(e.BTN_B, False)
            pad.service(b_up + 1.0)
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    pad.dev.close()

    if not failures:
        # A: the cart's last 100 ms request runs out (plus up to two frames).
        check_hold("A", pad.log, a_down, a_up, (65535, 65535), (0.06, 0.25), failures)
        # B: wc_pad_rumble_stop, at once.
        check_hold("B", pad.log, b_down, b_up, (32768, 16384), (0.0, 0.1), failures)
    if switch and any("rumbles through" in line for line in err):
        failures.append("the player found a hidraw node for a uinput pad")
    if failures:
        for f in failures:
            print(f"  FAIL  {f}")
        print("runner output:\n  " + "\n  ".join(err[-15:]))
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
