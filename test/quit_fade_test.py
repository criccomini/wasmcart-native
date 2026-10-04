#!/usr/bin/env python3
"""quit_fade_test.py — quitting fades the sound out instead of cutting it.

Runs a cart that plays a steady 480 Hz sine through SDL's disk audio driver,
which writes the device's stream to a file sample for sample, ends it the
ways Couchmix ends a game, and looks at the last 200 ms of what was played.
The cart writes by the clock, 150 ms ahead, so the runner has more than the
fade's worth queued at any frame rate:

  quit        SIGTERM while it plays: Quit or Restart with the game in
              front, a shutdown, a stopped session
  menu_quit   SIGUSR1, then SIGTERM: Quit or Restart from the Home menu
  force_quit  SIGUSR1, then SIGKILL: Force quit from the Home menu

Each must end without a step: no sample-to-sample jump bigger than the sine
itself makes, counting the jump from the last sample to the silence after
the stream ends, and a silent last block. A quit while playing must fade
over tens of milliseconds, not cut, and not just decay in 5 ms as an
underrun does. The cart changes its save every frame, and the save must be
on disk sooner after SIGTERM than a 50 ms fade could have finished.

Run:  python3 test/quit_fade_test.py build/wasmcart-run [--keep <dir>]
      (--keep writes each case's stream there as a WAV)
"""

import array
import io
import json
import math
import os
import select
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
import zipfile

RATE = 48000
AMP = 0.25
PERIOD = 100                  # samples: 480 Hz
INFO, FB, CURSOR, SAVE, TIME, TABLE, RING, CAP = 1024, 2048, 4096, 4352, 4480, 4608, 8192, 4096
SAVE_SIZE = 16
# The cart writes by the clock, LEAD ahead of it, so the runner's ring holds
# about that much whatever the frame rate: more than the stop's fade, which
# fits what's queued when less is. A fixed amount a frame didn't: at the
# 50 fps an offscreen sim container manages, 900 a frame ran the ring dry
# over and over, and a quit then found too little queued for its fade.
LEAD = RATE * 150 // 1000     # frames
MAX_PER_RENDER = 3000         # under the cart's ring (CAP), for the first frames and a slow one
WC_FLAG_AUDIO_F32 = 1

TONE_STEP = AMP * 2 * math.pi / PERIOD   # the most the sine moves in one sample
STEP_LIMIT = TONE_STEP * 1.5
TONE_RMS = AMP / math.sqrt(2)
FADE_MIN_MS = 30.0
SAVE_LIMIT_MS = 50.0


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


def sine_cart():
    # version, width, height, fb, audio, audio_cap, audio_write, input, save,
    # save_size, time, host_info, flags, audio_sample_rate, pointer, keys,
    # gpu_api, wheel
    info = struct.pack("<18I", 4, 8, 8, FB, RING, CAP, CURSOR, 0, SAVE, SAVE_SIZE, TIME, 0,
                       WC_FLAG_AUDIO_F32, RATE, 0, 0, 0, 0)
    table = struct.pack(f"<{PERIOD}f", *(AMP * math.sin(2 * math.pi * i / PERIOD)
                                         for i in range(PERIOD)))
    types = vec([b"\x60\x00\x01\x7f", b"\x60\x00\x00"])
    funcs = vec([leb(0), leb(1)])
    mem = vec([b"\x00" + leb(1)])
    exports = vec([name("memory") + b"\x02" + leb(0), name("wc_get_info") + b"\x00" + leb(0),
                   name("wc_render") + b"\x00" + leb(1)])
    load_cursor = i32(CURSOR) + b"\x28\x02\x00"  # i32.load
    # locals: 0 = n (i32), 1 = addr (i32), 2 = target (i32), 3 = sample (f32)
    render = (
        i32(TIME) + b"\x2b\x03\x00"                                 # time_ms (f64.load)
        + b"\x44" + struct.pack("<d", RATE / 1000) + b"\xa2\xab"     # * frames per ms, i32.trunc_f64_u
        + i32(LEAD) + b"\x6a\x21\x02"                                # + LEAD, target =
        + b"\x02\x40\x03\x40"                                        # block, loop
        + b"\x20\x00" + i32(MAX_PER_RENDER) + b"\x4f\x0d\x01"        # n >= MAX_PER_RENDER: out
        + load_cursor + b"\x20\x02\x4f\x0d\x01"                       # cursor >= target: out
        + i32(RING) + load_cursor + i32(CAP) + b"\x70"               # RING + cursor % CAP
        + i32(8) + b"\x6c\x6a\x21\x01"                               # * 8, +, addr =
        + i32(TABLE) + load_cursor + i32(PERIOD) + b"\x70"           # TABLE + cursor % PERIOD
        + i32(4) + b"\x6c\x6a\x2a\x02\x00\x21\x03"                   # * 4, +, f32.load, sample =
        + b"\x20\x01\x20\x03\x38\x02\x00"                            # left
        + b"\x20\x01\x20\x03\x38\x02\x04"                            # right
        + i32(CURSOR) + load_cursor + i32(1) + b"\x6a\x36\x02\x00"   # cursor++
        + b"\x20\x00" + i32(1) + b"\x6a\x21\x00"                     # n++
        + b"\x0c\x00\x0b\x0b"                                        # br loop, end, end
        + i32(SAVE) + i32(SAVE) + b"\x28\x02\x00" + i32(1) + b"\x6a\x36\x02\x00"  # save[0]++
    )

    def body(locals_, code):
        b = locals_ + code + b"\x0b"
        return leb(len(b)) + b

    code = vec([body(b"\x00", i32(INFO)), body(b"\x02\x03\x7f\x01\x7d", render)])
    data = vec([b"\x00" + i32(INFO) + b"\x0b" + leb(len(info)) + info,
                b"\x00" + i32(TABLE) + b"\x0b" + leb(len(table)) + table])
    wasm = (b"\x00asm\x01\x00\x00\x00" + section(1, types) + section(3, funcs) +
            section(5, mem) + section(7, exports) + section(10, code) + section(11, data))
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("manifest.json", json.dumps({"name": "quitfade", "abi": 4}))
        f.writestr("cart.wasm", wasm)
    return z.getvalue()


def heartbeat_reader(fd, lines):
    buf = b""
    while True:
        r, _, _ = select.select([fd], [], [], 0.5)
        if not r:
            continue
        chunk = os.read(fd, 65536)
        if not chunk:
            return
        buf += chunk
        *done, buf = buf.split(b"\n")
        lines += [l.decode().split() for l in done]


def run_case(binary, cart, d, case):
    raw = os.path.join(d, case + ".raw")
    save = os.path.join(d, case + ".sav")
    r, w = os.pipe()
    env = dict(os.environ, SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=raw,
               COUCHMIX_HEARTBEAT_FD=str(w))
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    p = subprocess.Popen([binary, cart, "--save", save], env=env, pass_fds=(w,),
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    os.close(w)
    lines = []
    reader = threading.Thread(target=heartbeat_reader, args=(r, lines), daemon=True)
    reader.start()
    give_up = time.monotonic() + 15
    while not any(l and l[0] == "F" for l in lines) and time.monotonic() < give_up:
        time.sleep(0.02)
    time.sleep(1.2)  # from the first frame
    if case != "quit":
        p.send_signal(signal.SIGUSR1)
        time.sleep(0.5)
    t_end = time.monotonic()
    p.send_signal(signal.SIGKILL if case == "force_quit" else signal.SIGTERM)
    try:
        _, err = p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        _, err = p.communicate()
    t_exit = time.monotonic()
    reader.join(2)
    os.close(r)
    data = open(raw, "rb").read() if os.path.exists(raw) else b""
    samples = array.array("f", data[: len(data) // 8 * 8])
    left = samples[0::2]
    saves = [int(l[2]) / 1e6 - t_end for l in lines if l and l[0] == "W" and int(l[2]) / 1e6 >= t_end]
    return {"left": left, "stereo": samples, "rc": p.returncode, "stderr": err or "",
            "exit_ms": (t_exit - t_end) * 1000, "save_ms": saves[0] * 1000 if saves else None}


def rms(xs):
    return math.sqrt(sum(x * x for x in xs) / len(xs)) if xs else 0.0


def analyze(left):
    tail = list(left[-RATE // 5:]) + [0.0]       # and the silence once the stream stops
    step_at = max(range(1, len(tail)), key=lambda i: abs(tail[i] - tail[i - 1]))
    step = abs(tail[step_at] - tail[step_at - 1])
    last_rms = rms(left[-1024:])
    win = RATE // 400                            # 2.5 ms windows
    w = [rms(tail[i:i + win]) for i in range(0, len(tail) - 1 - win + 1, win)]
    loud = [i for i, x in enumerate(w) if x >= 0.9 * TONE_RMS]
    fade_ms = None
    if loud:
        quiet = [i for i in range(loud[-1], len(w)) if w[i] < 0.01 * TONE_RMS]
        if quiet:
            fade_ms = (quiet[0] - loud[-1]) * 2.5
    return {"step": step, "step_ms_before_end": (len(tail) - 1 - step_at) * 1000 / RATE,
            "last_rms": last_rms, "loudest": max(w), "fade_ms": fade_ms,
            "peak": max(abs(x) for x in left)}


def write_wav(path, stereo):
    pcm = array.array("h", (max(-32768, min(32767, int(round(x * 32767)))) for x in stereo))
    with wave.open(path, "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(pcm.tobytes())


def main():
    args = sys.argv[1:]
    keep = None
    if "--keep" in args:
        i = args.index("--keep")
        keep = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        print(__doc__)
        return 2
    binary = args[0]
    failures = []

    def check(what, ok, detail):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}: {detail}")
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as d:
        cart = os.path.join(d, "quitfade.wasc")
        open(cart, "wb").write(sine_cart())
        for case in ("quit", "menu_quit", "force_quit"):
            res = run_case(binary, cart, d, case)
            left = res["left"]
            print(f"{case}: {len(left) / RATE:.2f} s of audio, exit {res['rc']} "
                  f"{res['exit_ms']:.0f} ms after the signal")
            if keep:
                os.makedirs(keep, exist_ok=True)
                write_wav(os.path.join(keep, case + ".wav"), res["stereo"])
            if len(left) < RATE:
                check(f"{case}: audio", False, f"only {len(left) / RATE:.2f} s came out; "
                      + " | ".join(res["stderr"].splitlines()[-3:]))
                continue
            a = analyze(left)
            check(f"{case}: the tone played", a["peak"] > 0.9 * AMP, f"peak {a['peak']:.3f}")
            check(f"{case}: no step at the end", a["step"] <= STEP_LIMIT,
                  f"largest step {a['step']:.4f}, {a['step_ms_before_end']:.1f} ms before the "
                  f"end (the sine moves {TONE_STEP:.4f} at most; limit {STEP_LIMIT:.4f})")
            check(f"{case}: silent at the end", a["last_rms"] < 1e-3,
                  f"last block's RMS {a['last_rms']:.4f}")
            if case == "quit":
                check("quit: playing up to the quit", a["loudest"] >= 0.9 * TONE_RMS,
                      f"loudest RMS in the last 200 ms {a['loudest']:.3f} (the tone's {TONE_RMS:.3f})")
                check("quit: a fade, not a cut or a 5 ms decay",
                      a["fade_ms"] is not None and a["fade_ms"] >= FADE_MIN_MS,
                      f"full level to silence in {a['fade_ms']} ms (want {FADE_MIN_MS:.0f}+)")
                check("quit: the save went down first",
                      res["save_ms"] is not None and res["save_ms"] < SAVE_LIMIT_MS,
                      f"save on disk {res['save_ms'] if res['save_ms'] is None else round(res['save_ms'], 1)}"
                      f" ms after SIGTERM (a {SAVE_LIMIT_MS:.0f} ms fade first would put it later)")
            if case != "force_quit":
                check(f"{case}: exit 0", res["rc"] == 0, f"exit {res['rc']}")

    if failures:
        print(f"*** FAIL {len(failures)}: " + "; ".join(failures))
        return 1
    print("PASS: every way out ends in silence, with a fade when there was sound")
    return 0


if __name__ == "__main__":
    sys.exit(main())
