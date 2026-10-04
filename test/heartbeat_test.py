#!/usr/bin/env python3
"""heartbeat_test.py — the lines a supervisor reads off COUCHMIX_HEARTBEAT_FD.

Runs a cart with a save region under the player with a heartbeat pipe and
drives it the way Couchmix does: SIGUSR1 to pause, SIGUSR2 to resume,
SIGTERM to quit. Checks, in order:
  I <abi> <save_size> <us>   once, when the cart has loaded (abi is 4)
  F <frame> <us>             per frame
  S <frame> <us>, then P     on SIGUSR1, and no F while paused
  R <frame> <us>, then F     on SIGUSR2
  W 0 <us>                   the save on disk after SIGTERM, exit 0
and that a refused v3 cart writes no I line.

Run:  python3 test/heartbeat_test.py build/wasmcart-run ../wasmcart/test/fixtures/savecart.wasc [<v3 cart>]
"""

import os
import select
import signal
import subprocess
import sys
import tempfile
import time


def read_lines(fd, seconds):
    out, buf, end = [], b"", time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            buf += chunk
            *lines, buf = buf.split(b"\n")
            out += [l.decode().split() for l in lines]
    return out


def start(binary, cart, save, w, log):
    env = dict(os.environ, COUCHMIX_HEARTBEAT_FD=str(w))
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env.setdefault("SDL_RENDER_DRIVER", "software")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    return subprocess.Popen([binary, cart, "--save", save], env=env, pass_fds=(w,),
                            stdout=subprocess.DEVNULL, stderr=log)


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        return 2
    binary, cart = sys.argv[1], sys.argv[2]
    failures = []

    def check(what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail and not ok else ""))
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as d:
        save = os.path.join(d, "s.sav")
        r, w = os.pipe()
        p = start(binary, cart, save, w, open(os.path.join(d, "log"), "w"))
        os.close(w)
        lines = read_lines(r, 3.0)
        kinds = [l[0] for l in lines]
        info = [l for l in lines if l[0] == "I"]
        check("one I line, ABI 4, with the save size", len(info) == 1 and info[0][1] == "4"
              and int(info[0][2]) > 0, info)
        check("I comes before the first F", "I" in kinds and "F" in kinds
              and kinds.index("I") < kinds.index("F"), kinds[:5])
        check("F lines while running", kinds.count("F") > 30, kinds.count("F"))

        p.send_signal(signal.SIGUSR1)
        lines = read_lines(r, 1.5)
        kinds = [l[0] for l in lines]
        s_at = kinds.index("S") if "S" in kinds else -1
        check("S on SIGUSR1", s_at >= 0, kinds[:8])
        after = kinds[s_at + 1:] if s_at >= 0 else []
        check("P while paused, no F", "P" in after and "F" not in after, after)

        p.send_signal(signal.SIGUSR2)
        lines = read_lines(r, 1.0)
        kinds = [l[0] for l in lines]
        check("R on SIGUSR2, then F", "R" in kinds and "F" in kinds[kinds.index("R"):]
              if "R" in kinds else False, kinds[:8])

        p.send_signal(signal.SIGTERM)
        lines = read_lines(r, 3.0)
        rc = p.wait(5)
        kinds = [l[0] for l in lines]
        check("W after SIGTERM, save on disk", "W" in kinds and os.path.getsize(save) > 0
              if os.path.exists(save) else False, kinds[-5:])
        check("exit 0", rc == 0, rc)
        os.close(r)

        if len(sys.argv) == 4:
            r, w = os.pipe()
            p = start(binary, sys.argv[3], save + "3", w, open(os.path.join(d, "log3"), "w"))
            os.close(w)
            lines = read_lines(r, 4.0)
            rc = p.wait(5)
            err = open(os.path.join(d, "log3")).read()
            check("a v3 cart: refused, exit 1, no I line", rc == 1 and "ABI version mismatch" in err
                  and not any(l[0] == "I" for l in lines), (rc, lines[:3]))
            os.close(r)
    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
