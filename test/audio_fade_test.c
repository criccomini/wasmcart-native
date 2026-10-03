// audio_fade_test.c — the output stage never clicks (src/audio_mix.h).
//
// Drives the mixer with a 440 Hz tone through startup, steady play, a pause,
// a resume and a dry spell, and checks the output never jumps by more than a
// sine at that pitch can move in one sample. A cut to silence mid-wave is a
// jump of up to the tone's full amplitude, which is the click this guards
// against. The same run with the fade switched off must fail the check, so
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

static float run(bool fade) {
    wc_audio_mix_t m;
    if (!audio_mix_init(&m, RATE, RATE / 2)) { fprintf(stderr, "FAIL: init\n"); exit(2); }
    if (!fade) { m.step = 1.0f; m.decay = 0.0f; }
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
    audio_mix_free(&m);
    return worst;
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
    if (failures) return 1;
    printf("PASS: largest step %.4f with fades (hard cuts: %.4f)\n", faded, cut);
    return 0;
}
