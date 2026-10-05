// switch_rumble_test.c — Switch pads' hidraw rumble (src/switch_rumble.h).
//
// No V8 or SDL:  cc -Isrc -o switch_rumble_test test/switch_rumble_test.c -lm
//
// The encoding (checked against the bytes hid-nintendo puts on the air for
// the same strengths), the send pattern a held rumble gets, and finding a
// pad's hidraw node in a fake sysfs tree.

#include "switch_rumble.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); failures++; } } while (0)

static void hex(char* out, const uint8_t* b, int n) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

static void expect_motor(uint16_t mag, const char* want) {
    uint8_t m[4];
    char got[9];
    switch_rumble_encode(m, mag);
    hex(got, m, 4);
    EXPECT(strcmp(got, want) == 0, "magnitude %u encodes as %s, want %s", mag, got, want);
}

typedef struct { uint64_t t; uint8_t rep[10]; } sent_t;

// Drain what the pad was sent (the read end of a pipe standing in for hidraw).
static int drain(int fd, sent_t* out, int max, uint64_t t) {
    int n = 0;
    while (n < max && read(fd, out[n].rep, 10) == 10) out[n++].t = t;
    return n;
}

static void write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static void mkdirs(const char* path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

// A fake /sys with one input node, event7, under a HID device.
static void fake_pad(const char* sys, const char* uevent, bool with_hidraw) {
    char dir[512], path[600];
    snprintf(dir, sizeof(dir), "%s/class/input/event7/device/device", sys);
    mkdirs(dir);
    snprintf(path, sizeof(path), "%s/uevent", dir);
    write_file(path, uevent);
    snprintf(path, sizeof(path), "%s/hidraw/hidraw3", dir);
    if (with_hidraw) mkdirs(path);
}

int main(void) {
    // ── Encoding: the bytes hid-nintendo sends for these strengths (seen
    //    over the air from the 8BitDo with btmon, and in its amplitude table).
    expect_motor(0, "00014040");       // silent
    expect_motor(1, "00014040");       // rounds to nothing, as in hid-nintendo
    expect_motor(100, "0003c040");     // the weakest step, code 1
    expect_motor(16384, "00494052");   // a quarter, code 36
    expect_motor(32768, "00894062");   // a half, code 68
    expect_motor(65534, "00c94072");   // full, code 100
    expect_motor(65535, "00c94072");
    int prev = 0, monotonic = 1;
    for (uint32_t m = 0; m <= 65535; m += 97) {
        int e = switch_rumble_code((uint16_t)m);
        if (e < prev) monotonic = 0;
        prev = e;
    }
    EXPECT(monotonic, "amplitude codes grow with the magnitude");

    // ── The send pattern, on a pipe in place of the hidraw node.
    int p[2];
    if (pipe(p) != 0) { perror("pipe"); return 1; }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    switch_rumble_t s;
    switch_rumble_init(&s);
    s.fd = p[1];
    sent_t got[256];
    int n = 0;

    EXPECT(switch_rumble_set(&s, 65535, 65535, 1000), "set");
    n += drain(p[0], got + n, 256 - n, 1000);
    EXPECT(n == 1, "a new rumble goes out at once (%d reports)", n);
    switch_rumble_tick(&s, 1017);
    switch_rumble_tick(&s, 1033);
    n += drain(p[0], got + n, 256 - n, 1033);
    EXPECT(n == 1, "nothing again for two frames (%d reports)", n);
    switch_rumble_tick(&s, 1050);
    n += drain(p[0], got + n, 256 - n, 1050);
    EXPECT(n == 2, "refreshed on the third frame (%d reports)", n);
    switch_rumble_set(&s, 65535, 65535, 1060);  // the same strength: nothing new
    switch_rumble_set(&s, 32768, 32768, 1061);  // a change 11 ms after the last report
    n += drain(p[0], got + n, 256 - n, 1061);
    EXPECT(n == 2, "a change waits out the 30 ms gap (%d reports)", n);
    switch_rumble_tick(&s, 1070);
    n += drain(p[0], got + n, 256 - n, 1070);
    EXPECT(n == 2, "still waiting at 20 ms (%d reports)", n);
    switch_rumble_tick(&s, 1080);
    n += drain(p[0], got + n, 256 - n, 1080);
    EXPECT(n == 3, "the change goes out at 30 ms (%d reports)", n);
    switch_rumble_stop(&s, 1085);
    n += drain(p[0], got + n, 256 - n, 1085);
    EXPECT(n == 4, "a stop goes out at once, gap or not (%d reports)", n);
    for (uint64_t t = 1100; t <= 1500; t += 16) switch_rumble_tick(&s, t);
    switch_rumble_stop(&s, 1501);
    n += drain(p[0], got + n, 256 - n, 1501);
    EXPECT(n == 4, "nothing while off, and no second stop (%d reports)", n);
    if (n == 4) {
        char a[21];
        hex(a, got[0].rep, 10);
        EXPECT(strcmp(a, "1000" "00c9407200c94072") == 0, "first report %s", a);
        hex(a, got[1].rep, 10);
        EXPECT(strcmp(a, "1001" "00c9407200c94072") == 0, "refresh %s", a);
        hex(a, got[2].rep, 10);
        EXPECT(strcmp(a, "1002" "0089406200894062") == 0, "half %s", a);
        hex(a, got[3].rep, 10);
        EXPECT(strcmp(a, "1003" "0001404000014040") == 0, "stop %s", a);
    }

    // A 3 s hold at 60 frames a second: a report every third frame (50 ms),
    // and the counter wraps at 16.
    switch_rumble_init(&s);
    s.fd = p[1];
    n = 0;
    uint64_t t0 = 5000;
    switch_rumble_set(&s, 65535, 0, t0);
    n += drain(p[0], got + n, 256 - n, t0);
    for (int f = 1; f <= 180; f++) {
        uint64_t t = t0 + (uint64_t)(f * 1000 / 60);
        switch_rumble_set(&s, 65535, 0, t);  // a cart asks again every frame
        switch_rumble_tick(&s, t);
        n += drain(p[0], got + n, 256 - n, t);
    }
    uint64_t gap = 0;
    for (int i = 1; i < n; i++)
        if (got[i].t - got[i - 1].t > gap) gap = got[i].t - got[i - 1].t;
    EXPECT(n >= 59 && n <= 61, "%d reports in 3 s, want about 60", n);
    EXPECT(gap <= 51, "longest gap %llu ms", (unsigned long long)gap);
    EXPECT(n > 16 && got[16].rep[1] == 0 && got[15].rep[1] == 15, "the counter wraps");
    char a[21];
    hex(a, got[0].rep + 2, 8);
    EXPECT(strcmp(a, "00c9407200014040") == 0, "low only is the left motor: %s", a);
    close(p[0]);
    close(p[1]);

    // A node that's gone: the write fails and says so.
    switch_rumble_init(&s);
    s.fd = 1000;  // not open
    EXPECT(!switch_rumble_set(&s, 65535, 65535, 1), "a dead node reports failure");

    // ── Finding the node.
    char sys[] = "/tmp/switch_rumble_test.XXXXXX";
    if (!mkdtemp(sys)) { perror("mkdtemp"); return 1; }
    char node[64], cmd[600];
    const char* pro = "DRIVER=nintendo\nHID_ID=0005:0000057E:00002009\nHID_NAME=Pro Controller\n";
    fake_pad(sys, pro, true);
    EXPECT(switch_rumble_find_hidraw(sys, "event7", node, sizeof(node)) == 0 &&
           strcmp(node, "/dev/hidraw3") == 0, "a Pro Controller's node: %s", node);
    EXPECT(switch_rumble_find_hidraw(sys, "event8", node, sizeof(node)) == -1, "no such node");
    snprintf(cmd, sizeof(cmd), "rm -rf %s/class", sys);
    if (system(cmd) != 0) return 1;
    fake_pad(sys, "DRIVER=nintendo\nHID_ID=0003:0000057E:00002007\n", true);
    EXPECT(switch_rumble_find_hidraw(sys, "event7", node, sizeof(node)) == 0,
           "a right Joy-Con over USB");
    if (system(cmd) != 0) return 1;
    fake_pad(sys, "DRIVER=hid-generic\nHID_ID=0005:0000057E:00002009\n", true);
    EXPECT(switch_rumble_find_hidraw(sys, "event7", node, sizeof(node)) == -1,
           "not without hid-nintendo (it's what enables the pad's rumble)");
    if (system(cmd) != 0) return 1;
    fake_pad(sys, "DRIVER=nintendo\nHID_ID=0005:0000057E:00002017\n", true);
    EXPECT(switch_rumble_find_hidraw(sys, "event7", node, sizeof(node)) == -1,
           "not an SNES pad, which has no rumble");
    if (system(cmd) != 0) return 1;
    fake_pad(sys, "DRIVER=playstation\nHID_ID=0005:0000054C:000009CC\n", true);
    EXPECT(switch_rumble_find_hidraw(sys, "event7", node, sizeof(node)) == -1, "not a DualShock 4");
    if (system(cmd) != 0) return 1;
    fake_pad(sys, pro, false);
    EXPECT(switch_rumble_find_hidraw(sys, "event7", node, sizeof(node)) == -1, "no hidraw node");
    snprintf(cmd, sizeof(cmd), "rm -rf %s", sys);
    if (system(cmd) != 0) return 1;

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("switch_rumble_test: all checks passed\n");
    return 0;
}
