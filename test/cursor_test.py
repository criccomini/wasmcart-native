#!/usr/bin/env python3
"""cursor_test.py — no mouse pointer over the player's window.

The standalone player forwards no mouse input to the cart, so a pointer over
the picture is only in the way. On a TV it shows up even with no mouse: HDMI-CEC
input devices report relative motion, libinput calls them a pointer, and the
compositor draws one.

Runs the player on a cart that fills its screen with one colour, under a
wlroots compositor, moves a virtual pointer to the middle of the picture and
screenshots it with the cursor composited in. Any pixel near the pointer that
isn't the cart's colour is a cursor. Checked fullscreen, after F11 to a window,
and after F11 back to fullscreen. First it checks the compositor draws a
pointer at all, or a pass would mean nothing.

Needs labwc (or another wlroots compositor with the virtual-pointer and
virtual-keyboard protocols), wlrctl, grim and wtype. With WAYLAND_DISPLAY unset
it starts labwc on wlroots' headless backend itself; with it set it uses that
session, and moves its pointer.

Run:  python3 test/cursor_test.py build/wasmcart-run
"""

import io
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

FILL = (0x30, 0xA0, 0x50)  # the cart's colour
BOX = 40                    # how far around the pointer to look, in pixels


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | 0x80 if n else b)
        if not n:
            return bytes(out)


def section(sid, body):
    return bytes([sid]) + leb(len(body)) + body


def vec(items):
    return leb(len(items)) + b"".join(items)


def name(s):
    return leb(len(s)) + s.encode()


def flat_cart():
    """An 8x8 v4 cart whose framebuffer is all FILL, set by a data segment."""
    info_at, fb_at = 1024, 2048
    # version, width, height, fb, audio, audio_cap, audio_write, input, save,
    # save_size, time, host_info, flags, audio_sample_rate, pointer, keys,
    # gpu_api, wheel
    info = struct.pack("<18I", 4, 8, 8, fb_at, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    pixel = bytes([FILL[2], FILL[1], FILL[0], 0])  # XRGB, little-endian
    types = vec([b"\x60\x00\x01\x7f", b"\x60\x00\x00"])
    funcs = vec([leb(0), leb(1)])
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(0),
                   name("wc_render") + b"\x00" + leb(1)])

    def body(code):
        b = b"\x00" + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(b"\x41" + leb_s(info_at)), body(b"")])  # return info_at; nothing
    segs = [(info_at, info), (fb_at, pixel * 64)]
    data = vec([b"\x00\x41" + leb_s(at) + b"\x0b" + leb(len(d)) + d for at, d in segs])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(3, funcs) +
            section(5, mem) + section(7, exports) + section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "flat", "abi": 4}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


def leb_s(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if (n == 0 and not b & 0x40) or (n == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def shot(path):
    """The screen with the cursor composited in: (width, height, rgb bytes)."""
    subprocess.run(["grim", "-c", "-t", "ppm", path], check=True)
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:  # P6, width, height, maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    return int(fields[1]), int(fields[2]), data[pos + 1:]


def px(img, x, y):
    w, _, rgb = img
    i = (y * w + x) * 3
    return rgb[i], rgb[i + 1], rgb[i + 2]


def near(c, want, tol=48):
    return sum(abs(a - b) for a, b in zip(c, want)) <= tol


def fill_box(img):
    """Bounding box of the cart's colour, or None."""
    w, h, _ = img
    xs, ys = [], []
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            if near(px(img, x, y), FILL):
                xs.append(x)
                ys.append(y)
    if len(xs) < 100:
        return None
    return min(xs), min(ys), max(xs), max(ys)


class Pointer:
    """The virtual pointer. Moves are relative, so track where it is."""

    def __init__(self):
        self.x = self.y = 0
        self.home()

    def move(self, dx, dy):
        subprocess.run(["wlrctl", "pointer", "move", str(dx), str(dy)], check=True)

    def home(self):
        self.move(-20000, -20000)  # the compositor clamps it to the corner
        self.x = self.y = 0

    def to(self, x, y):
        self.move(x - self.x, y - self.y)
        self.x, self.y = x, y


def foreign(img, cx, cy, region):
    """Pixels within BOX of (cx, cy), inside region, that aren't the cart's."""
    x0, y0, x1, y1 = region
    n = 0
    for y in range(max(cy - BOX, y0), min(cy + BOX, y1) + 1):
        for x in range(max(cx - BOX, x0), min(cx + BOX, x1) + 1):
            if not near(px(img, x, y), FILL):
                n += 1
    return n


def wait_for(cond, seconds, path):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        img = shot(path)
        r = cond(img)
        if r:
            return img, r
        time.sleep(0.2)
    return shot(path), None


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    for tool in ("wlrctl", "grim", "wtype"):
        if not shutil.which(tool):
            print(f"*** FAIL {tool} not found")
            return 1
    tmp = tempfile.mkdtemp()
    env = dict(os.environ)
    compositor = None
    if not env.get("WAYLAND_DISPLAY"):
        env["XDG_RUNTIME_DIR"] = os.path.join(tmp, "xdg")
        os.mkdir(env["XDG_RUNTIME_DIR"], 0o700)
        env.update(WLR_BACKENDS="headless", WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1",
                   LIBGL_ALWAYS_SOFTWARE="1")
        compositor = subprocess.Popen(["labwc"], env=env, stdout=subprocess.DEVNULL,
                                      stderr=open(os.path.join(tmp, "labwc.log"), "w"))
        sock = os.path.join(env["XDG_RUNTIME_DIR"], "wayland-0")
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        env["WAYLAND_DISPLAY"] = "wayland-0"
    os.environ.update(env)  # for wlrctl, grim and wtype
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    env["SDL_VIDEODRIVER"] = "wayland"
    cart = os.path.join(tmp, "flat.wasc")
    open(cart, "wb").write(flat_cart())
    path = os.path.join(tmp, "shot.ppm")
    failures = []
    runner = None
    # A keyboard in the seat from the start. Each wtype call brings its own
    # and drops it on exit, and a seat that gains a keyboard only then can
    # deliver the key before the window has bound it.
    keyboard = subprocess.Popen(["wtype", "-s", "3600000", "-k", "F12"])
    try:
        time.sleep(0.5)
        ptr = Pointer()
        ptr.to(20, 20)
        img = shot(path)
        drawn = sum(1 for y in range(0, 60) for x in range(0, 60) if px(img, x, y) != px(img, 100, 100))
        if not drawn:
            print("*** FAIL the compositor draws no pointer, so this test can't see one")
            return 1
        print(f"  ok    the compositor draws a pointer ({drawn} pixels by the corner)")

        runner = subprocess.Popen([sys.argv[1], cart, "--fullscreen", "--res", "640x480"], env=env,
                                  stdout=subprocess.DEVNULL, stderr=open(os.path.join(tmp, "run.log"), "w"))

        def check(what, box):
            # Onto the middle of the picture, in two moves, so the pointer is
            # over the window and has just moved there.
            cx, cy = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2
            ptr.to(cx - 5, cy - 5)
            time.sleep(0.2)
            ptr.to(cx, cy)
            time.sleep(0.5)
            n = foreign(shot(path), cx, cy, box)
            if n:
                failures.append(f"{what}: {n} pointer pixels around ({cx}, {cy})")
            else:
                print(f"  ok    {what}: no pointer over the picture at ({cx}, {cy})")

        def toggle(what, prev):
            subprocess.run(["wtype", "-k", "F11"], check=True)
            _, box = wait_for(lambda img: (lambda b: b if b and b != prev else None)(fill_box(img)),
                              10, path)
            if not box:
                failures.append(f"F11 {what} didn't change the window")
                raise RuntimeError
            return box

        _, full = wait_for(fill_box, 20, path)
        if not full:
            failures.append("the cart's picture never appeared")
            raise RuntimeError
        check("fullscreen", full)
        windowed = toggle("to a window", full)
        check("after F11 to a window", windowed)
        check("after F11 back to fullscreen", toggle("back to fullscreen", windowed))
    except RuntimeError:
        pass
    finally:
        if runner:
            runner.send_signal(signal.SIGTERM)
            try:
                runner.wait(timeout=10)
            except subprocess.TimeoutExpired:
                runner.kill()
        keyboard.kill()
        keyboard.wait()
        if compositor:
            compositor.terminate()
            compositor.wait()

    if failures:
        for f in failures:
            print(f"*** FAIL {f}")
        log = os.path.join(tmp, "run.log")
        if os.path.exists(log):
            print("runner output:\n  " + "\n  ".join(open(log).read().splitlines()[-10:]))
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
