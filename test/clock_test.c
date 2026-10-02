// clock_test.c — the player's frame clock (src/frame_clock.h).
//
// Pins the three spec rules for time: delta_ms is clamped to WC_MAX_DELTA_MS,
// time_ms stays consistent with the deltas delivered, and a known gap
// (suspend) is rebased away entirely. The rebase check fails if rebase does
// nothing: the clamp alone would hand the cart a 250ms step, so asserting
// "under 250" is not enough and this asserts the first resumed delta is tiny.
//
// No V8, no SDL:  cc -Iinclude -Isrc -o clock_test test/clock_test.c

#include "frame_clock.h"
#include <math.h>
#include <stdio.h>

static int failures = 0;

static void expect(const char* what, double got, double want) {
    if (fabs(got - want) > 1e-9) {
        fprintf(stderr, "FAIL: %s: got %.6f, want %.6f\n", what, got, want);
        failures++;
    }
}

int main(void) {
    wc_frame_clock_t c;
    double t, d, sum = 0;

    wc_frame_clock_start(&c, 1000);
    wc_frame_clock_tick(&c, 1016, &t, &d); sum += d;
    expect("first delta", d, 16);
    expect("first time", t, 16);
    wc_frame_clock_tick(&c, 1032, &t, &d); sum += d;
    expect("second time", t, 32);

    // A ten-minute stall nobody reported reaches the cart as one clamped step,
    // and time_ms moves by exactly that step, not by ten minutes.
    wc_frame_clock_tick(&c, 601032, &t, &d); sum += d;
    expect("stall delta is clamped", d, WC_MAX_DELTA_MS);
    expect("stall time follows the clamped delta", t, 32 + WC_MAX_DELTA_MS);
    wc_frame_clock_tick(&c, 601048, &t, &d); sum += d;
    expect("next delta is ordinary", d, 16);
    expect("time stays consistent after a stall", t, 32 + WC_MAX_DELTA_MS + 16);
    expect("deltas sum to time_ms", sum, t);

    // A ten-minute suspend the host knows about costs the cart nothing.
    double before = t;
    wc_frame_clock_rebase(&c, 1201048);
    wc_frame_clock_tick(&c, 1201048, &t, &d);
    if (d >= 1.0) {
        fprintf(stderr, "FAIL: first delta after rebase is %.3fms; rebase did not remove the gap\n", d);
        failures++;
    }
    expect("time does not jump across a rebased suspend", t, before + d);
    before = t;
    wc_frame_clock_tick(&c, 1201064, &t, &d);
    expect("delta after resume is ordinary", d, 16);
    expect("time continues after resume", t, before + 16);

    // Time never runs backwards, and a delta is never zero. A 1ms clock puts
    // several fast frames on one tick; their substitute deltas count in
    // time_ms too, or the two drift apart (0.2s a minute at 4,900 fps).
    before = t;
    wc_frame_clock_tick(&c, 1201000, &t, &d);
    if (!(d > 0)) { fprintf(stderr, "FAIL: zero delta\n"); failures++; }
    if (t < before) { fprintf(stderr, "FAIL: time ran backwards\n"); failures++; }
    expect("a substitute delta is counted in time_ms", t, before + d);

    // Rebasing with no gap changes nothing.
    before = t;
    wc_frame_clock_rebase(&c, 1201000);
    wc_frame_clock_tick(&c, 1201080, &t, &d);
    expect("rebase without a gap is a no-op", t, before + 16);

    // Ten seconds uncapped at 4,900 fps, read through a 1ms clock: most
    // frames share a tick, and the deltas still sum to time_ms.
    wc_frame_clock_t u;
    double ut = 0, ud, usum = 0;
    wc_frame_clock_start(&u, 0);
    for (int i = 1; i <= 4900 * 10; i++) {
        wc_frame_clock_tick(&u, (double)((i * 1000) / 4900), &ut, &ud);
        usum += ud;
    }
    if (fabs(usum - ut) > 1e-6) {
        fprintf(stderr, "FAIL: uncapped: deltas sum to %.6f but time_ms is %.6f\n", usum, ut);
        failures++;
    }

    if (failures) return 1;
    printf("PASS: delta clamped, time consistent, suspend rebased away\n");
    return 0;
}
