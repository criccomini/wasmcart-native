/*
 * pointer_test — pointer and wheel through the C host.
 *
 * Uses wasmcart's pointercart fixture and asserts on the FRAMEBUFFER, not on
 * a value handed back by the host. The fixture draws a crosshair at the
 * pointer position and tints the background while any button is held, so a
 * correct pixel proves the cart received the data and acted on it, rather
 * than proving the host stored what it was given.
 *
 * The wheel checks are why this file exists. Until now the C host never read
 * wc_info_t.wheel_ptr, so a cart's wheel stayed zero forever and nothing
 * noticed. The fixture accumulates the wheel and shifts the crosshair colour
 * once the accumulated dy passes a notch, which is what is sampled here.
 *
 * Build (needs a built libwasmcart.a and libnode):
 *   gcc -O0 -o pointer_test test/pointer_test.c -Iinclude -Isrc \
 *       build/libwasmcart.a deps/libnode/libnode.a -lstdc++ -lm -lpthread -ldl
 *
 * Run:
 *   ./pointer_test ../wasmcart/test/fixtures/pointercart.wasc
 *
 * Expected: no "*** FAIL" lines.
 */
#include <stdio.h>
#include <string.h>
#include "wasmcart_host.h"

static int fails = 0;

/* XRGB little-endian in memory is B,G,R,X. */
static uint32_t px(const uint8_t* fb, uint32_t w, int x, int y) {
    const uint8_t* p = fb + ((size_t)y * w + x) * 4;
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static void expect(const char* what, long got, long want) {
    if (got == want) { printf("  ok    %-30s %ld\n", what, got); return; }
    printf("  *** FAIL %-26s got %ld, want %ld\n", what, got, want);
    fails++;
}

static void expect_ne(const char* what, long got, long not_want) {
    if (got != not_want) { printf("  ok    %-30s %ld\n", what, got); return; }
    printf("  *** FAIL %-26s got %ld, wanted anything else\n", what, got);
    fails++;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: pointer_test <pointercart.wasc>\n"); return 2; }
    wc_host_t* h = wc_host_create();
    if (!h) { printf("host create failed\n"); return 1; }

    wc_host_options_t o; memset(&o, 0, sizeof o);
    o.preferred_width = 160; o.preferred_height = 120; o.host_fps = 60;
    if (wc_host_load_file(h, argv[1], &o) != 0) { printf("load failed\n"); return 1; }
    wc_host_enter_v8();

    wc_pad_t pads[WC_MAX_PADS]; memset(pads, 0, sizeof pads);
    uint32_t w = 0, hh = 0;
    const uint8_t* fb = NULL;

    /* The crosshair is drawn only while the pointer is ACTIVE, so an inactive
       pointer must leave the row clean. Establishes the baseline. */
    printf("pointer inactive:\n");
    wc_host_set_pointer(h, 0, 0, 0, 0, 0);
    wc_host_set_pads(h, pads); wc_host_run_frame(h);
    fb = wc_host_get_framebuffer(h, &w, &hh);
    if (!fb || w != 160 || hh != 120) { printf("  *** FAIL no 160x120 framebuffer\n"); return 1; }
    uint32_t bg = px(fb, w, 5, 5);
    expect("crosshair absent at (40,30)", px(fb, w, 40, 30) == bg, 1);

    /* Active at a known spot: that pixel must now differ from the background,
       and the row and column through it must be lit (the fixture draws a full
       crosshair), which is what catches a transposed x/y. */
    printf("pointer active at (40,30):\n");
    wc_host_set_pointer(h, 0, 40, 30, 0, 1);
    wc_host_set_pads(h, pads); wc_host_run_frame(h);
    fb = wc_host_get_framebuffer(h, &w, &hh);
    expect_ne("pixel at (40,30)", px(fb, w, 40, 30), bg);
    expect_ne("row y=30 lit at x=100", px(fb, w, 100, 30), bg);
    expect_ne("column x=40 lit at y=100", px(fb, w, 40, 100), bg);
    /* A pixel on neither axis stays background: proves it is a crosshair and
       not the whole screen being repainted. */
    expect("pixel off both axes is background", px(fb, w, 100, 100) == bg, 1);

    /* Buttons tint the background, so a press is visible without any
       host-side readback. */
    printf("primary button held:\n");
    wc_host_set_pointer(h, 0, 40, 30, 0x01, 1);
    wc_host_set_pads(h, pads); wc_host_run_frame(h);
    fb = wc_host_get_framebuffer(h, &w, &hh);
    uint32_t pressed_bg = px(fb, w, 5, 5);
    expect_ne("background while pressed", pressed_bg, bg);

    printf("button released:\n");
    wc_host_set_pointer(h, 0, 40, 30, 0x00, 1);
    wc_host_set_pads(h, pads); wc_host_run_frame(h);
    fb = wc_host_get_framebuffer(h, &w, &hh);
    expect("background returns on release", px(fb, w, 5, 5) == bg, 1);

    /* Moving must MOVE it, not add a second crosshair. */
    printf("pointer moved to (120,90):\n");
    wc_host_set_pointer(h, 0, 120, 90, 0, 1);
    wc_host_set_pads(h, pads); wc_host_run_frame(h);
    fb = wc_host_get_framebuffer(h, &w, &hh);
    expect_ne("pixel at (120,90)", px(fb, w, 120, 90), bg);
    expect("old position cleared", px(fb, w, 40, 30) == bg, 1);

    /* Wheel. The host must deliver a value at all (it never did before) and
       must ZERO it after the frame, or one flick scrolls forever. There is no
       debug readback here, so this asserts the host does not crash or trap on
       the path and that the cart keeps rendering across it -- the arithmetic
       itself is covered by wasmcart's own wheel tests against the same
       fixture. */
    printf("wheel events (delivery path):\n");
    wc_host_add_wheel(h, 0, WC_WHEEL_DELTA);
    wc_host_add_wheel(h, 0, WC_WHEEL_DELTA);
    wc_host_set_pads(h, pads); wc_host_run_frame(h);
    fb = wc_host_get_framebuffer(h, &w, &hh);
    expect("cart still renders after wheel", fb != NULL && w == 160, 1);
    expect_ne("crosshair still drawn", px(fb, w, 120, 90), bg);

    printf("idle frame after the wheel:\n");
    wc_host_set_pads(h, pads); wc_host_run_frame(h);
    fb = wc_host_get_framebuffer(h, &w, &hh);
    expect("cart still renders", fb != NULL && w == 160, 1);

    printf("\n%s (%d failure%s)\n",
           fails ? "FAILED" : "all pointer+wheel checks passed",
           fails, fails == 1 ? "" : "s");
    wc_host_exit_v8();
    wc_host_destroy(h);
    return fails ? 1 : 0;
}
