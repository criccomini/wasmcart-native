// pad_layout_test.c — wc_pad_t matches the ABI's layout, field by field.
//
// The header's static assert catches a wrong size. This also catches a field
// that moved, which a size check can't: carts index into wc_pads[] by these
// offsets (docs/input.md in the wasmcart repo, "wc_pad_t Layout").
//
// No V8 or SDL:  cc -Iinclude -o pad_layout_test test/pad_layout_test.c

#include <stddef.h>
#include <stdio.h>
#include "wasmcart_host.h"

static int failures = 0;
static void expect(const char* what, size_t got, size_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s is %zu, want %zu\n", what, got, want);
        failures++;
    }
}

int main(void) {
    expect("sizeof(wc_pad_t)", sizeof(wc_pad_t), 20);
    expect("buttons", offsetof(wc_pad_t, buttons), 0);
    expect("sizeof(buttons)", sizeof(((wc_pad_t*)0)->buttons), 4);
    expect("left_x", offsetof(wc_pad_t, left_x), 4);
    expect("left_y", offsetof(wc_pad_t, left_y), 6);
    expect("right_x", offsetof(wc_pad_t, right_x), 8);
    expect("right_y", offsetof(wc_pad_t, right_y), 10);
    expect("left_trigger", offsetof(wc_pad_t, left_trigger), 12);
    expect("sizeof(left_trigger)", sizeof(((wc_pad_t*)0)->left_trigger), 2);
    expect("right_trigger", offsetof(wc_pad_t, right_trigger), 14);
    expect("connected", offsetof(wc_pad_t, connected), 16);
    expect("WC_PAD_SIZE", WC_PAD_SIZE, 20);
    // Four pads, back to back: pad 1's buttons start at byte 20.
    wc_pad_t pads[WC_MAX_PADS];
    expect("pad 1 offset", (size_t)((char*)&pads[1] - (char*)&pads[0]), 20);
    expect("all pads", sizeof(pads), 80);
    if (failures) return 1;
    printf("PASS: wc_pad_t is 20 bytes with ABI v4's field offsets\n");
    return 0;
}
