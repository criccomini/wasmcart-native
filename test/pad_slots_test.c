// pad_slots_test.c — controllers keep their player slot (src/pad_slots.h).
//
// No V8 or SDL:  cc -Isrc -o pad_slots_test test/pad_slots_test.c

#include "pad_slots.h"
#include <stdio.h>

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); failures++; } } while (0)

int main(void) {
    pad_slots_t t;

    // Plain mode is first-free: a pad that leaves frees its slot.
    pad_slots_init(&t, false, NULL);
    EXPECT(pad_slots_attach(&t, "a") == 0, "a takes 0");
    EXPECT(pad_slots_attach(&t, "b") == 1, "b takes 1");
    pad_slots_detach(&t, 0);
    EXPECT(pad_slots_attach(&t, "c") == 0, "plain: c takes the freed 0");

    // Sticky: a pad that leaves keeps its slot and gets it back.
    pad_slots_init(&t, true, NULL);
    EXPECT(pad_slots_attach(&t, "a") == 0, "a takes 0");
    EXPECT(pad_slots_attach(&t, "b") == 1, "b takes 1");
    pad_slots_detach(&t, 0);
    EXPECT(!t.connected[0] && strcmp(t.key[0], "a") == 0, "0 is held for a");
    EXPECT(pad_slots_attach(&t, "c") == 2, "c skips held 0, takes 2");
    EXPECT(pad_slots_attach(&t, "a") == 0, "a comes back to 0");
    EXPECT(pad_slots_attach(&t, "d") == 3, "d takes 3");
    EXPECT(pad_slots_attach(&t, "e") == -1, "no slot for a fifth pad");
    pad_slots_detach(&t, 3);
    EXPECT(pad_slots_attach(&t, "e") == -1, "e can't take d's held slot");

    // Seeded by the supervisor: slots follow its numbering, not arrival.
    pad_slots_init(&t, true, "1=E4:17:D8:C5:97:08,0=usb-xhci-1/input0");
    EXPECT(pad_slots_attach(&t, "e4:17:d8:c5:97:08") == 1, "seeded pad takes its slot, case-folded");
    EXPECT(pad_slots_attach(&t, "new") == 2, "unknown pad skips seeded 0");
    EXPECT(pad_slots_attach(&t, "usb-xhci-1/input0") == 0, "late seeded pad gets 0");

    // Bad seed entries are skipped, good ones kept.
    pad_slots_init(&t, true, "9=x,=y,2=,a=b,3=ok,,1=has space");
    EXPECT(strcmp(t.key[3], "ok") == 0, "3=ok kept");
    EXPECT(strcmp(t.key[1], "has_space") == 0, "space cleaned to underscore");
    EXPECT(!t.key[0][0] && !t.key[2][0], "0 and 2 empty");

    // Two devices with one key (a pad on Bluetooth and USB at once) get two
    // slots rather than sharing one.
    pad_slots_init(&t, true, NULL);
    EXPECT(pad_slots_attach(&t, "same") == 0, "first same takes 0");
    EXPECT(pad_slots_attach(&t, "same") == 1, "second same takes 1");

    // An empty key still takes a slot, and that slot isn't handed out twice.
    pad_slots_init(&t, false, NULL);
    EXPECT(pad_slots_attach(&t, "") == 0, "empty key takes 0");
    EXPECT(pad_slots_attach(&t, "x") == 1, "next pad takes 1, not 0 again");

    if (failures) return 1;
    printf("PASS: pads keep their slots\n");
    return 0;
}
