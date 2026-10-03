#!/usr/bin/env python3
"""rumble_sdl_test.py — a cart's wc_pad_rumble reaches a real pad.

Plugs in a uinput gamepad that supports force feedback, runs wasmcart-run on
wasmcart's rumble fixture, and answers the kernel's force-feedback uploads the
way a driver would. The fixture asks for a 0.5/0.25 rumble on pad 0 in its
first frame, so the pad must receive an FF_RUMBLE effect with those strengths
and a request to play it.

rumble_test.c checks the host library's clamping with a recording backend;
this checks the standalone player wires a backend up at all.

Linux only. Needs write access to /dev/uinput and python3-evdev. In a
container, /dev/input has to be the host's (e.g. -v /dev/input:/dev/input).

Run:  python3 test/rumble_sdl_test.py build/wasmcart-run ../wasmcart/test/fixtures/rumble.wasc
"""

import os
import select
import subprocess
import sys
import time

from evdev import AbsInfo, UInput, ecodes as e


def make_pad():
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
    return UInput(caps, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E,
                  version=0x110, bustype=e.BUS_USB, max_effects=16)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    pad = make_pad()
    time.sleep(1.0)
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    if not os.path.exists("/run/udev/control"):
        env.setdefault("SDL_JOYSTICK_DISABLE_UDEV", "1")  # see pad_hotplug_test.py
    proc = subprocess.Popen([sys.argv[1], sys.argv[2]], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)

    uploads, plays = [], []
    deadline = time.monotonic() + 6.0
    while time.monotonic() < deadline:
        r, _, _ = select.select([pad.fd], [], [], 0.1)
        if not r:
            continue
        for ev in pad.read():
            if ev.type == e.EV_UINPUT and ev.code == e.UI_FF_UPLOAD:
                up = pad.begin_upload(ev.value)
                up.retval = 0
                eff = up.effect
                if eff.type == e.FF_RUMBLE:
                    rm = eff.u.ff_rumble_effect
                    uploads.append((rm.strong_magnitude, rm.weak_magnitude, eff.ff_replay.length))
                pad.end_upload(up)
            elif ev.type == e.EV_UINPUT and ev.code == e.UI_FF_ERASE:
                er = pad.begin_erase(ev.value)
                er.retval = 0
                pad.end_erase(er)
            elif ev.type == e.EV_FF and ev.code != e.FF_GAIN:
                plays.append(ev.value)

    proc.terminate()
    try:
        _, err = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        _, err = proc.communicate()
    pad.close()

    failures = []
    # SDL scales 0..1 to 0..65535; the fixture's first effect is 0.5 / 0.25.
    want = (32768, 16384)
    first = uploads[0] if uploads else None
    if not first or abs(first[0] - want[0]) > 2 or abs(first[1] - want[1]) > 2:
        failures.append(f"first rumble effect {first}, want strong/weak about {want}")
    if 1 not in plays:
        failures.append(f"no play request reached the pad (EV_FF values seen: {plays})")
    print(f"  uploads (strong, weak, length): {uploads[:4]}")
    print(f"  play requests: {plays[:8]}")
    if failures:
        for f in failures:
            print(f"  FAIL  {f}")
        print("runner output:\n  " + "\n  ".join(err.splitlines()[-15:]))
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
