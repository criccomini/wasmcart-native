#!/usr/bin/env python3
"""exit_test.py — SIGTERM ends the player promptly, save first, even when
node's platform won't shut down.

At exit, node's platform joins its delayed-task thread, and that thread
lives until its libuv loop has no timers left. V8's memory pool posts one
8 s out from a worker while the isolate is being disposed; when that lands
after the platform has closed the others, the player sits in the join for
the 8 s, and a supervisor gives up and kills it (Couchmix's waits 3 s).

The race is rare, so this test stands in for it: an LD_PRELOAD shim holds
any pthread_join of the thread that names itself "DelayedTaskSche..." for
20 s. The player is run with it, SIGTERMed, and must exit within 2 s, with
code 0 and its save on disk. Linux only (LD_PRELOAD, pthread_setname_np).

Run:  python3 test/exit_test.py build/wasmcart-run ../wasmcart/test/fixtures/savecart.wasc
"""

import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

SHIM = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_t held;
static int have_held;

__attribute__((constructor)) static void loaded(void) {
    fprintf(stderr, "join-shim: loaded\n");
}

int pthread_setname_np(pthread_t t, const char* name) {
    static int (*real)(pthread_t, const char*);
    if (!real) real = (int (*)(pthread_t, const char*))dlsym(RTLD_NEXT, "pthread_setname_np");
    if (strncmp(name, "DelayedTask", 11) == 0) {
        held = t;
        have_held = 1;
        fprintf(stderr, "join-shim: saw %s\n", name);
    }
    return real(t, name);
}

int pthread_join(pthread_t t, void** ret) {
    static int (*real)(pthread_t, void**);
    if (!real) real = (int (*)(pthread_t, void**))dlsym(RTLD_NEXT, "pthread_join");
    if (have_held && pthread_equal(t, held)) {
        fprintf(stderr, "join-shim: holding the delayed-task thread's join\n");
        sleep(20);
    }
    return real(t, ret);
}
"""

MAGIC = 0x5A5EDA7A  # savecart's save starts with it


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    binary, cart = os.path.abspath(sys.argv[1]), sys.argv[2]
    failures = []

    def check(what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f": {detail}" if detail and not ok else ""))
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as d:
        src, shim = os.path.join(d, "shim.c"), os.path.join(d, "shim.so")
        with open(src, "w") as f:
            f.write(SHIM)
        subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-o", shim, src, "-ldl"], check=True)
        game = os.path.join(d, "savecart.wasc")
        shutil.copyfile(cart, game)
        save = game + ".sav"

        env = dict(os.environ, LD_PRELOAD=shim)
        env.setdefault("SDL_VIDEODRIVER", "offscreen")
        env.setdefault("SDL_RENDER_DRIVER", "software")
        env.setdefault("SDL_AUDIODRIVER", "dummy")
        log_path = os.path.join(d, "log")
        with open(log_path, "w") as log:
            p = subprocess.Popen([binary, game], env=env, stdout=log, stderr=subprocess.STDOUT)
            time.sleep(2.0)  # loaded and saving every frame
            t0 = time.monotonic()
            p.send_signal(signal.SIGTERM)
            try:
                rc = p.wait(2.0)
                took = time.monotonic() - t0
            except subprocess.TimeoutExpired:
                rc, took = None, None
                p.kill()
                p.wait()
        err = open(log_path).read()

        check("the shim was loaded and saw node's delayed-task thread",
              "join-shim: loaded" in err and "join-shim: saw DelayedTask" in err, err[-400:])
        check("exits within 2 s of SIGTERM", took is not None,
              "still running after 2 s" + (" (held in the delayed-task join)"
                                           if "holding the delayed-task" in err else ""))
        if took is not None:
            print(f"        ({took:.3f} s)")
        check("exit code 0", rc == 0, rc)
        data = open(save, "rb").read() if os.path.exists(save) else b""
        check("the save is on disk", len(data) >= 8 and struct.unpack("<I", data[:4])[0] == MAGIC,
              data[:8].hex() or "no save")
    print("\nall checks passed" if not failures else f"\nFAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
