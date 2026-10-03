#!/usr/bin/env python3
"""pad_hotplug_test.py — each physical pad takes exactly one slot.

Plugs uinput gamepads into a running wasmcart-run and reads its stderr: two
pads present at startup must take slots 0 and 1 and nothing else; unplugging
the first must free slot 0 without leaving a second slot behind; a new pad
must land in slot 0 again.

The bug this guards against: pads present at startup were opened twice, once
by the startup loop and once from SDL's queued CONTROLLERDEVICEADDED event, so
one pad filled two slots and player 2 mirrored player 1.

Linux only. Needs write access to /dev/uinput and python3-evdev. In a
container, /dev/input has to be the host's (e.g. -v /dev/input:/dev/input)
so SDL sees the new nodes.

Run:  python3 test/pad_hotplug_test.py build/wasmcart-run test/snake.wasc
      (SDL_VIDEODRIVER=dummy by default; set it to override)
"""

import os
import re
import subprocess
import sys
import threading
import time

from evdev import AbsInfo, UInput, ecodes as e

LINE = re.compile(r"wasmcart: controller (\d+) (connected|disconnected)")


def make_pad(name):
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
                  version=0x110, bustype=e.BUS_USB, phys=name)


class Runner:
    def __init__(self, binary, cart):
        env = dict(os.environ)
        env.setdefault("SDL_VIDEODRIVER", "dummy")
        env.setdefault("SDL_AUDIODRIVER", "dummy")
        # Without a udev daemon (a container) SDL still enumerates through
        # libudev but never hears about new devices. Make it watch /dev/input.
        if not os.path.exists("/run/udev/control"):
            env.setdefault("SDL_JOYSTICK_DISABLE_UDEV", "1")
        self.proc = subprocess.Popen([binary, cart], env=env, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, text=True)
        self.events = []  # (slot, "connected" | "disconnected")
        self.log = []
        self.lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stderr:
            self.log.append(line.rstrip())
            m = LINE.search(line)
            if m:
                with self.lock:
                    self.events.append((int(m.group(1)), m.group(2)))

    def take(self, settle=2.0):
        """Wait for things to settle, then return and clear the events seen."""
        time.sleep(settle)
        with self.lock:
            out, self.events = self.events, []
        return out

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    failures = []

    def expect(what, got, want):
        ok = sorted(got) == sorted(want)
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}: {got}" + ("" if ok else f", want {want}"))
        if not ok:
            failures.append(what)

    a, b = make_pad("pad-test/a"), make_pad("pad-test/b")
    time.sleep(1.0)  # let the nodes appear before the runner scans
    r = Runner(sys.argv[1], sys.argv[2])
    try:
        expect("two pads at startup", r.take(4.0), [(0, "connected"), (1, "connected")])
        a.close()
        expect("first pad unplugged", r.take(), [(0, "disconnected")])
        c = make_pad("pad-test/c")
        expect("new pad plugged in", r.take(), [(0, "connected")])
        c.close()
        b.close()
        expect("both unplugged", r.take(), [(0, "disconnected"), (1, "disconnected")])
    finally:
        r.stop()
    if r.proc.returncode not in (0, -15) and not any("running" in l for l in r.log):
        print("runner output:\n  " + "\n  ".join(r.log[-20:]))
        failures.append("runner did not start")
    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
