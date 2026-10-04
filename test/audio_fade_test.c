// audio_fade_test.c — the output stage never clicks (src/audio_mix.h).
//
// Drives the mixer with a 440 Hz tone through startup, steady play, a pause,
// a resume, a dry spell and the stop before the device closes, and checks the
// output never jumps by more than a sine at that pitch can move in one
// sample. A cut to silence mid-wave is a jump of up to the tone's full
// amplitude, which is the click this guards against. The same run with the fade switched off must fail the check, so
// the test can't pass vacuously.
//
// No SDL:  cc -Iinclude -Isrc -o audio_fade_test test/audio_fade_test.c -lm

#include "audio_mix.h"
#include <stdio.h>

#define RATE 48000
#define AMP 0.5f

static double phase = 0;
static void push_tone(wc_audio_mix_t* m, uint32_t frames) {
    float buf[2048 * 2];
    for (uint32_t i = 0; i < frames; i++) {
        float v = AMP * (float)sin(phase);
        phase += 2 * M_PI * 440.0 / RATE;
        buf[i * 2] = buf[i * 2 + 1] = v;
    }
    audio_mix_push_f32(m, buf, frames);
}

static float last = 0, worst = 0;
static void pull(wc_audio_mix_t* m, uint32_t frames) {
    float buf[1024 * 2];
    while (frames) {
        uint32_t n = frames > 1024 ? 1024 : frames;
        audio_mix_pull(m, buf, n);
        for (uint32_t i = 0; i < n; i++) {
            float d = fabsf(buf[i * 2] - last);
            if (d > worst) worst = d;
            last = buf[i * 2];
        }
        frames -= n;
    }
}

// One "frame" of the game loop: the cart writes 800 frames (60 fps), the
// device plays 800.
static void play(wc_audio_mix_t* m, int frames, bool cart_writes) {
    for (int f = 0; f < frames; f++) {
        if (cart_writes) push_tone(m, 800);
        pull(m, 800);
    }
}

// The stop on exit: pull device-sized buffers until the mixer says the
// device can close, as main.c does. Returns frames pulled; *fade_frames is
// how many of them it took to reach silence.
static uint32_t stop(wc_audio_mix_t* m, uint32_t* fade_frames) {
    audio_mix_stop(m);
    uint32_t pulled = 0;
    *fade_frames = 0;
    while (!audio_mix_stopped(m) && pulled < RATE) {
        pull(m, 1024);
        pulled += 1024;
        if (!*fade_frames && m->gain == 0.0f) *fade_frames = pulled;
    }
    return pulled;
}

static float run(bool fade) {
    wc_audio_mix_t m;
    if (!audio_mix_init(&m, RATE, RATE / 2)) { fprintf(stderr, "FAIL: init\n"); exit(2); }
    if (!fade) { m.step = 1.0f; m.stop_step = 1.0f; m.decay = 0.0f; }
    last = 0;
    worst = 0;
    phase = 0;

    play(&m, 30, true);                   // start, prime, fade in, play
    audio_mix_set_paused(&m, true);       // Home: the cart stops writing
    play(&m, 30, false);
    if (m.count != 0) { fprintf(stderr, "FAIL: paused mixer kept %u stale frames\n", m.count); exit(1); }
    audio_mix_set_paused(&m, false);      // Resume
    play(&m, 30, true);
    play(&m, 3, false);                   // the cart falls behind: dry spell
    play(&m, 30, true);                   // and catches up
    uint32_t fade_frames;
    stop(&m, &fade_frames);               // quit: the device is about to close
    pull(&m, 1);                          // and the silence after it
    audio_mix_free(&m);
    return worst;
}

// The stop's own timing: a playing stage with enough queued fades over about
// AUDIO_STOP_FADE_MS and then plays its tail; a paused one, silent already,
// is done at once.
static int stop_timing(void) {
    const double buf_ms = 1024 * 1000.0 / RATE;
    int failures = 0;
    wc_audio_mix_t m;
    if (!audio_mix_init(&m, RATE, RATE / 2)) { fprintf(stderr, "FAIL: init\n"); exit(2); }
    play(&m, 30, true);
    push_tone(&m, 1600);                  // more than the fade's worth queued (see stop_short)
    uint32_t fade_frames, pulled = stop(&m, &fade_frames);
    double fade_ms = fade_frames * 1000.0 / RATE, total_ms = pulled * 1000.0 / RATE;
    audio_mix_free(&m);
    if (fade_ms < AUDIO_STOP_FADE_MS || fade_ms > AUDIO_STOP_FADE_MS + buf_ms) {
        fprintf(stderr, "FAIL: playing, the stop reached silence after %.1f ms\n", fade_ms);
        failures++;
    }
    if (total_ms < fade_ms + AUDIO_TAIL_MS - buf_ms || total_ms > fade_ms + AUDIO_TAIL_MS + buf_ms) {
        fprintf(stderr, "FAIL: playing, the stop was done %.1f ms after silence\n", total_ms - fade_ms);
        failures++;
    }

    if (!audio_mix_init(&m, RATE, RATE / 2)) { fprintf(stderr, "FAIL: init\n"); exit(2); }
    play(&m, 30, true);
    audio_mix_set_paused(&m, true);       // Home, then Quit from the menu
    play(&m, 30, false);
    uint32_t paused_pulled = stop(&m, &fade_frames);
    audio_mix_free(&m);
    if (paused_pulled != 0) {
        fprintf(stderr, "FAIL: paused, the stop still took %u frames\n", paused_pulled);
        failures++;
    }
    if (!failures)
        printf("stop: silent within %.1f ms, done %.1f ms after the stop; paused: done at once\n",
            fade_ms, total_ms);
    return failures;
}

// A stop with less than the fade's worth queued. A cart that writes what
// each frame needs keeps about AUDIO_PRIME_MS in the ring, less just after
// the device has taken a buffer, and nothing more comes once the loop has
// ended. Fading at the full fade's pace, the ring ran dry part-way down and
// the last sample fell to zero in AUDIO_DECAY_MS, from wherever the gain had
// got to: a thump. Now the fade fits what's queued and is at zero with its
// last sample, and with only a few ms queued it takes AUDIO_FADE_MS, as a
// pause does, so whatever is left when it runs dry is less.
static int stop_short(void) {
    int failures = 0;
    const double queued_ms[] = {30.0, 20.0, 4.0};
    const uint32_t fade_min = (uint32_t)(RATE * AUDIO_FADE_MS / 1000.0);
    for (int c = 0; c < 3; c++) {
        wc_audio_mix_t m;
        if (!audio_mix_init(&m, RATE, RATE / 2)) { fprintf(stderr, "FAIL: init\n"); exit(2); }
        phase = 0;
        play(&m, 30, true);               // steady, AUDIO_PRIME_MS queued
        uint32_t left = (uint32_t)(RATE * queued_ms[c] / 1000.0);
        pull(&m, m.count - left);         // the device takes the rest
        audio_mix_stop(&m);
        float buf[2 * 2], prev = last, step = 0, after = 0;
        uint32_t zero_at = 0;
        for (uint32_t i = 0; i < left + 1024; i++) {
            audio_mix_pull(&m, buf, 1);
            if (fabsf(buf[0] - prev) > step) step = fabsf(buf[0] - prev);
            prev = buf[0];
            if (!zero_at && m.gain == 0.0f) zero_at = i + 1;
            if (i >= left && fabsf(buf[0]) > after) after = fabsf(buf[0]);
        }
        audio_mix_free(&m);
        char when[48];
        if (zero_at) snprintf(when, sizeof when, "after %.1f ms", zero_at * 1000.0 / RATE);
        else snprintf(when, sizeof when, "never in %.0f ms", (left + 1024) * 1000.0 / RATE);
        if (queued_ms[c] >= AUDIO_FADE_MS) {
            if (!zero_at || zero_at > left || zero_at + 4 < left) {
                fprintf(stderr, "FAIL: %.0f ms queued: silent %s, not with the last sample\n",
                        queued_ms[c], when);
                failures++;
            }
            if (after > 0.0f) {
                fprintf(stderr, "FAIL: %.0f ms queued: %.4f still playing once it ran out\n", queued_ms[c], after);
                failures++;
            }
        } else if (!zero_at || zero_at < fade_min || zero_at > fade_min + 1) {
            fprintf(stderr, "FAIL: %.0f ms queued: silent %s, not after the pause's %.0f ms\n",
                    queued_ms[c], when, AUDIO_FADE_MS);
            failures++;
        }
        if (step > 0.04f) {
            fprintf(stderr, "FAIL: %.0f ms queued: a step of %.4f\n", queued_ms[c], step);
            failures++;
        }
        if (!failures)
            printf("stop with %.0f ms queued: silent %s\n", queued_ms[c], when);
    }
    return failures;
}

int main(void) {
    // A 440 Hz sine at this amplitude moves at most AMP * 2*pi*440/RATE per
    // sample (about 0.029); allow a little for the ramps on top.
    const float limit = 0.04f;
    float faded = run(true);
    float cut = run(false);
    int failures = 0;
    if (faded > limit) {
        fprintf(stderr, "FAIL: largest step with fades is %.4f (limit %.4f)\n", faded, limit);
        failures++;
    }
    if (cut <= limit) {
        fprintf(stderr, "FAIL: control: hard cuts gave only %.4f; the check can't see clicks\n", cut);
        failures++;
    }
    failures += stop_timing();
    failures += stop_short();
    if (failures) return 1;
    printf("PASS: largest step %.4f with fades (hard cuts: %.4f)\n", faded, cut);
    return 0;
}
