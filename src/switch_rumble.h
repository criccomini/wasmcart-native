// switch_rumble.h — Switch pads' rumble, written to the pad's hidraw node.
//
// On Linux, hid-nintendo sends a Switch pad's rumble through a rate limiter
// that wants three input reports 8-17 ms apart before every packet it sends.
// A pad whose link delivers its reports in bursts never gets there (an 8BitDo
// in Switch mode sends four every 40 ms), so each packet waits out the
// limiter's 500 tries, about 5 s. The pad, for its part, keeps its motors
// going only ~150 ms past the last packet. So for these pads the player writes
// the rumble output report itself, as SDL's HIDAPI driver does: report 0x10,
// about every 50 ms while the rumble lasts, and one silent one to stop.
// hid-nintendo keeps everything else: setup, input, LEDs.
//
// No SDL:  cc -Isrc -o switch_rumble_test test/switch_rumble_test.c -lm

#ifndef SWITCH_RUMBLE_H
#define SWITCH_RUMBLE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#endif

// The pad holds a packet ~150 ms; SDL refreshes every 50. Ticks come once a
// frame, so this is a floor: at 60 frames a second, every third frame (50 ms).
#define SWITCH_RUMBLE_REFRESH_MS 40
#define SWITCH_RUMBLE_MIN_GAP_MS 30  // SDL's floor between writes; faster can drop the link

typedef struct {
    int fd;               // the pad's hidraw node, -1 for none
    uint8_t num;          // packet counter, 0..15
    uint64_t sent_ms;     // when the last report went out
    bool sent_any;
    uint8_t rumble[8];    // left motor's 4 bytes, then the right's
    bool on;              // rumble[] isn't silence
    bool dirty;           // rumble[] changed since the last report
} switch_rumble_t;

// One motor at hid-nintendo's default frequencies (160 Hz low band, 320 Hz
// high band) and an amplitude of mag/65535, both bands at the same strength,
// as hid-nintendo encodes it. Amplitude codes 0..100 follow the table in
// dekuNukem's Nintendo_Switch_Reverse_Engineering (rumble_data_table.md):
// the smallest code at least as strong as asked, in thousandths, with 65535
// meaning 1.003 (code 100), as hid-nintendo scales it.
static int switch_rumble_code(uint16_t mag) {
    static int amp[101];  // each code's amplitude in thousandths
    if (!amp[100]) {
        for (int e = 1; e <= 100; e++) {
            double a = e < 16 ? exp2(e / 4.0) / 120.0
                     : e < 32 ? exp2(e / 16.0) / 17.0
                              : exp2(e / 32.0) / 8.7;
            amp[e] = (int)(a * 1000.0 + 0.5);
        }
    }
    int want = (int)((uint32_t)mag * 1003u / 65535u);
    if (want <= 0) return 0;
    for (int e = 1; e < 100; e++)
        if (amp[e] >= want) return e;
    return 100;
}

static void switch_rumble_encode(uint8_t out[4], uint16_t mag) {
    int e = switch_rumble_code(mag);
    out[0] = 0x00;                            // high band 320 Hz (0x0100), low byte
    out[1] = (uint8_t)(0x01 | (e << 1));      // its 9th bit, and the high band's amplitude
    out[2] = (uint8_t)(0x40 | ((e & 1) << 7));// low band 160 Hz, and its amplitude's top bit
    out[3] = (uint8_t)(0x40 + e / 2);         // the low band's amplitude
}

static void switch_rumble_init(switch_rumble_t* s) {
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    switch_rumble_encode(s->rumble, 0);
    switch_rumble_encode(s->rumble + 4, 0);
}

// Report 0x10 (rumble only) with what the pad should be doing now.
static bool switch_rumble_write(switch_rumble_t* s, uint64_t now) {
    if (s->fd < 0) return false;
    uint8_t rep[10] = { 0x10, (uint8_t)(s->num & 0x0F) };
    memcpy(rep + 2, s->rumble, 8);
    s->num = (uint8_t)((s->num + 1) & 0x0F);
    s->sent_ms = now;
    s->sent_any = true;
    s->dirty = false;
#ifdef __linux__
    if (write(s->fd, rep, sizeof(rep)) != (ssize_t)sizeof(rep)) {
        // ENODEV, EIO: the pad went away. Anything else, try again next time.
        if (errno == ENODEV || errno == EIO || errno == EBADF) return false;
    }
#endif
    return true;
}

static bool switch_rumble_gap_ok(const switch_rumble_t* s, uint64_t now) {
    return !s->sent_any || now - s->sent_ms >= SWITCH_RUMBLE_MIN_GAP_MS;
}

// A new strength, low for the left (strong) motor and high for the right
// (weak) one, as hid-nintendo maps them. It goes out now unless the last
// report was under SWITCH_RUMBLE_MIN_GAP_MS ago; then the next tick sends it.
// Returns false if the node has failed (the caller drops it).
static bool switch_rumble_set(switch_rumble_t* s, uint16_t low, uint16_t high, uint64_t now) {
    uint8_t r[8];
    switch_rumble_encode(r, low);
    switch_rumble_encode(r + 4, high);
    s->on = low || high;
    if (memcmp(r, s->rumble, 8) != 0) {
        memcpy(s->rumble, r, 8);
        s->dirty = true;
    }
    if (s->dirty && switch_rumble_gap_ok(s, now)) return switch_rumble_write(s, now);
    return true;
}

// Silence, at once: a stop doesn't wait out the gap (it may be the last
// thing the player does).
static bool switch_rumble_stop(switch_rumble_t* s, uint64_t now) {
    bool was = s->on || s->dirty;
    switch_rumble_encode(s->rumble, 0);
    switch_rumble_encode(s->rumble + 4, 0);
    s->on = false;
    return was ? switch_rumble_write(s, now) : true;
}

// Once a frame: a change that had to wait, or the refresh a rumble needs.
static bool switch_rumble_tick(switch_rumble_t* s, uint64_t now) {
    if (s->dirty && switch_rumble_gap_ok(s, now)) return switch_rumble_write(s, now);
    if (s->on && now - s->sent_ms >= SWITCH_RUMBLE_REFRESH_MS) return switch_rumble_write(s, now);
    return true;
}

// The hidraw node behind an evdev node ("event7") of a Switch pad that
// hid-nintendo drives, as "/dev/hidraw3"; sys is "/sys" outside tests.
// 0 if found, -1 if it isn't such a pad or has no hidraw node.
static int switch_rumble_find_hidraw(const char* sys, const char* event, char* out, size_t n) {
#ifdef __linux__
    char path[512], line[256];
    snprintf(path, sizeof(path), "%s/class/input/%s/device/device/uevent", sys, event);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    unsigned bus = 0, vendor = 0, product = 0;
    bool nintendo = false, id = false;
    while (fgets(line, sizeof(line), f)) {
        if (strcmp(line, "DRIVER=nintendo\n") == 0) nintendo = true;
        if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vendor, &product) == 3) id = true;
    }
    fclose(f);
    // Pro Controller (and pads posing as one), Joy-Cons, the charging grip
    // and the N64 pad: the ones hid-nintendo gives rumble.
    if (!nintendo || !id || vendor != 0x057E) return -1;
    if (product != 0x2009 && product != 0x2006 && product != 0x2007 &&
        product != 0x200E && product != 0x2019) return -1;
    snprintf(path, sizeof(path), "%s/class/input/%s/device/device/hidraw", sys, event);
    DIR* d = opendir(path);
    if (!d) return -1;
    int found = -1;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "hidraw", 6) == 0) {
            snprintf(out, n, "/dev/%s", de->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
#else
    (void)sys; (void)event; (void)out; (void)n;
    return -1;
#endif
}

#endif
