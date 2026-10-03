// lifecycle_test.c — suspend, resume and focus reach the cart in spec order.
//
// The lifecycle fixture counts each callback and appends a digit per event
// (1=suspend 2=resume 3=focus_lost 4=focus_gained), so the test reads back
// both delivery and ordering instead of inferring them. It also counts
// rendered frames, which is how the spec's one MUST is checked: no wc_render
// while suspended. A second cart with no lifecycle exports must go through
// the same transitions without errors, since every callback is optional.
//
// Usage:
//   ./lifecycle_test ../wasmcart/test/fixtures/lifecycle.wasc
//       5484 5488 5492 5496 5500 5504 ../wasmcart/test/fixtures/hello.wasc
// The offsets are the fixture's debug fields (suspend, resume, focus_lost,
// focus_gained, frames, sequence); read them with wasmcart's readDebugState().

#include "wasmcart_host.h"
#include "wc_log.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { F_SUSPEND, F_RESUME, F_LOST, F_GAINED, F_FRAMES, F_SEQUENCE, F_COUNT };
static uint32_t off[F_COUNT];
static const char* cart_path;
static int failures = 0;

static uint32_t rd(wc_host_t* host, int field) {
    uint32_t size = 0;
    const uint8_t* mem = wc_host_get_memory(host, &size);
    if (!mem || off[field] + 4 > size) { fprintf(stderr, "FAIL: no memory\n"); exit(2); }
    uint32_t v;
    memcpy(&v, mem + off[field], 4);
    return v;
}

static void check(const char* what, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %u, want %u\n", what, got, want);
        failures++;
    }
}

static void check_bool(const char* what, bool got, bool want) {
    check(what, got ? 1 : 0, want ? 1 : 0);
}

static int frame_no = 0;
static void frame(wc_host_t* host) {
    wc_host_set_time(host, frame_no * 16.7, 16.7, (uint32_t)frame_no);
    wc_host_run_frame(host);
    frame_no++;
}

static wc_host_t* load(const char* path) {
    wc_host_t* host = wc_host_create();
    if (!host) { fprintf(stderr, "FAIL: wc_host_create\n"); exit(2); }
    wc_host_options_t opts = {0};
    if (wc_host_load_file(host, path, &opts) != 0) {
        fprintf(stderr, "FAIL: load %s\n", path);
        exit(2);
    }
    wc_host_enter_v8();  // only after create(), which initialises V8
    return host;
}

static void unload(wc_host_t* host) {
    check_bool("never trapped", wc_host_has_trapped(host), false);
    wc_host_exit_v8();
    wc_host_destroy(host);
}

int main(int argc, char* argv[]) {
    if (argc < 9) {
        fprintf(stderr, "usage: %s <lifecycle.wasc> <off_suspend> <off_resume> "
                        "<off_focus_lost> <off_focus_gained> <off_frames> "
                        "<off_sequence> <no-callbacks.wasc>\n", argv[0]);
        return 1;
    }
    cart_path = argv[1];
    for (int i = 0; i < F_COUNT; i++) off[i] = (uint32_t)strtoul(argv[2 + i], NULL, 10);

    // No wc_render while suspended, and rendering picks up again on resume.
    {
        wc_host_t* h = load(cart_path);
        frame(h); frame(h);
        check("frames before suspend", rd(h, F_FRAMES), 2);
        check_bool("suspend changes state", wc_host_suspend(h), true);
        check_bool("is_suspended", wc_host_is_suspended(h), true);
        frame(h); frame(h);
        check("no wc_render while suspended", rd(h, F_FRAMES), 2);
        check_bool("resume changes state", wc_host_resume(h), true);
        frame(h);
        check("rendering resumes", rd(h, F_FRAMES), 3);
        check("suspend+resume order (lost, suspend, resume, gained)", rd(h, F_SEQUENCE), 3124);
        check_bool("focused after resume", wc_host_is_focused(h), true);
        unload(h);
    }

    // Transitions are idempotent: a second suspend or resume delivers nothing.
    {
        wc_host_t* h = load(cart_path);
        check_bool("first suspend", wc_host_suspend(h), true);
        check_bool("second suspend is a no-op", wc_host_suspend(h), false);
        check_bool("first resume", wc_host_resume(h), true);
        check_bool("second resume is a no-op", wc_host_resume(h), false);
        check("one suspend delivered", rd(h, F_SUSPEND), 1);
        check("one resume delivered", rd(h, F_RESUME), 1);
        unload(h);
    }

    // Focus reported while suspended is refused, so focus_gained can never
    // arrive before resume, whatever order the window system reports in.
    {
        wc_host_t* h = load(cart_path);
        wc_host_suspend(h);
        check_bool("focus refused while suspended", wc_host_focus(h), false);
        check_bool("blur while suspended is a no-op", wc_host_blur(h), false);
        wc_host_resume(h);
        check("order holds when focus comes first", rd(h, F_SEQUENCE), 3124);
        unload(h);
    }

    // Resuming into a background window: the host blurs straight after.
    {
        wc_host_t* h = load(cart_path);
        wc_host_suspend(h);
        wc_host_resume(h);
        check_bool("blur after resume", wc_host_blur(h), true);
        check("resume into background window", rd(h, F_SEQUENCE), 31243);
        frame(h);
        check("unfocused is not suspended: still renders", rd(h, F_FRAMES), 1);
        unload(h);
    }

    // Focus alone never suspends, and an already-unfocused cart gets no
    // second focus_lost when it is suspended.
    {
        wc_host_t* h = load(cart_path);
        wc_host_blur(h);
        frame(h);
        wc_host_focus(h);
        check("alt-tab away and back", rd(h, F_SEQUENCE), 34);
        wc_host_blur(h);
        wc_host_suspend(h);
        wc_host_resume(h);
        check("blur, suspend, resume", rd(h, F_SEQUENCE), 343124);
        check("focus_lost delivered twice in total", rd(h, F_LOST), 2);
        unload(h);
    }

    // A cart with none of the four exports goes through the same transitions,
    // and the host doesn't try to call what isn't there: nothing is logged as
    // having thrown.
    {
        char log_path[] = "lifecycle_test.log";
        wc_log_set_file(log_path);
        wc_host_t* h = load(argv[8]);
        frame(h);
        check_bool("suspend without exports", wc_host_suspend(h), true);
        check_bool("blur without exports", wc_host_blur(h), false);
        frame(h);
        check_bool("resume without exports", wc_host_resume(h), true);
        frame(h);
        unload(h);
        wc_log_set_file(NULL);
        FILE* f = fopen(log_path, "r");
        char line[512];
        int threw = 0;
        while (f && fgets(line, sizeof(line), f)) {
            if (strstr(line, "threw")) { fprintf(stderr, "  %s", line); threw++; }
        }
        if (f) fclose(f);
        remove(log_path);
        check("callbacks reported as thrown for a cart without them", (uint32_t)threw, 0);
    }

    if (failures) return 1;
    printf("PASS: delivery, ordering, idempotence, and no wc_render while suspended\n");
    return 0;
}
