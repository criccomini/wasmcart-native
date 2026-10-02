// frame_clock.h — the standalone player's frame clock (SPEC: Frame timing).
//
// Turns wall-clock samples into the time_ms / delta_ms pair a cart sees:
//
//   - delta_ms is clamped to WC_MAX_DELTA_MS. Any stall inflates it (a GC
//     pause, a slow disk, a debugger, a suspend nobody reported), and a cart
//     integrating velocity by dt would move the whole stall in one step,
//     straight through whatever it should have hit.
//   - time_ms stays consistent with the deltas delivered: the time a clamp
//     discards is taken out of time_ms too, or a cart summing deltas and one
//     reading time_ms drift apart for the rest of the session.
//   - time_ms never runs backwards.
//   - delta_ms is never zero (a 1ms clock puts several fast frames on one
//     tick), and the substitute 0.001 is counted in time_ms as well.
//   - rebase() removes a gap the host knows about (a suspend), so the cart
//     sees none of it, not even one clamped frame.
//
// Doubles in milliseconds throughout, matching CartHost.js.

#ifndef WC_FRAME_CLOCK_H
#define WC_FRAME_CLOCK_H

#include "../include/wasmcart_host.h"

typedef struct {
    double start_ms;  // wall time that time_ms counts from
    double last_ms;   // wall time of the previous frame
} wc_frame_clock_t;

static inline void wc_frame_clock_start(wc_frame_clock_t* c, double now_ms) {
    c->start_ms = now_ms;
    c->last_ms = now_ms;
}

static inline void wc_frame_clock_tick(wc_frame_clock_t* c, double now_ms,
                                       double* time_ms, double* delta_ms) {
    if (now_ms < c->last_ms) now_ms = c->last_ms;
    double raw = now_ms - c->last_ms;
    double delta = raw;
    if (raw > WC_MAX_DELTA_MS) {
        c->start_ms += raw - WC_MAX_DELTA_MS;
        delta = WC_MAX_DELTA_MS;
    }
    if (delta <= 0.0) {
        delta = 0.001;
        c->start_ms -= delta;  // keep time_ms equal to the sum of the deltas
    }
    c->last_ms = now_ms;
    *time_ms = now_ms - c->start_ms;
    *delta_ms = delta;
}

static inline void wc_frame_clock_rebase(wc_frame_clock_t* c, double now_ms) {
    double gap = now_ms - c->last_ms;
    if (gap > 0.0) {
        c->start_ms += gap;
        c->last_ms = now_ms;
    }
}

#endif
