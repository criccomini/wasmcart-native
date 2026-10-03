// audio_mix.h — the player's audio output stage, without SDL.
//
// A cart's samples go into a ring; the audio device's callback pulls them out
// through this mixer, which never jumps: any change of level is a ramp.
//
//   - Pause and resume fade over AUDIO_FADE_MS. Cutting a waveform off
//     mid-cycle is a step, and a step is a click; on a TV it is a loud one.
//   - A ring that runs dry (the cart fell behind) decays the last sample to
//     zero over AUDIO_DECAY_MS instead of dropping to silence at once.
//   - Output starts only once AUDIO_PRIME_MS of audio is waiting, at startup
//     and after any dry spell, then fades in. Starting on the first few
//     samples would just run dry again.
//   - A paused stage throws away what it held once the fade is done, so a
//     resume never replays stale audio.
//
// Interleaved stereo float. One producer (the frame loop) and one consumer
// (the audio callback); the caller serializes them (SDL_LockAudioDevice).

#ifndef WC_AUDIO_MIX_H
#define WC_AUDIO_MIX_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_FADE_MS  10.0
#define AUDIO_DECAY_MS 5.0
#define AUDIO_PRIME_MS 50.0

typedef struct {
    float* ring;          // cap frames of L,R
    uint32_t cap, head, count;
    uint32_t prime;       // frames to wait for before playing
    float gain, step;     // current gain and its per-frame ramp
    float decay;          // per-frame factor for a dry ring
    float last_l, last_r; // last sample out, decayed when the ring is dry
    bool paused, playing;
} wc_audio_mix_t;

static inline bool audio_mix_init(wc_audio_mix_t* m, uint32_t rate, uint32_t cap_frames) {
    memset(m, 0, sizeof(*m));
    m->ring = (float*)calloc((size_t)cap_frames * 2, sizeof(float));
    if (!m->ring) return false;
    m->cap = cap_frames;
    m->prime = (uint32_t)(rate * AUDIO_PRIME_MS / 1000.0);
    if (m->prime > cap_frames / 2) m->prime = cap_frames / 2;
    m->step = (float)(1.0 / (rate * AUDIO_FADE_MS / 1000.0));
    m->decay = (float)exp(log(0.001) / (rate * AUDIO_DECAY_MS / 1000.0));
    return true;
}

static inline void audio_mix_free(wc_audio_mix_t* m) {
    free(m->ring);
    m->ring = NULL;
}

// Producer: append frames. Frames that don't fit are dropped, which keeps
// latency bounded if a cart writes faster than the device plays.
static inline void audio_mix_push_f32(wc_audio_mix_t* m, const float* lr, uint32_t frames) {
    for (uint32_t i = 0; i < frames && m->count < m->cap; i++) {
        uint32_t at = (m->head + m->count) % m->cap;
        m->ring[at * 2] = lr[i * 2];
        m->ring[at * 2 + 1] = lr[i * 2 + 1];
        m->count++;
    }
}

static inline void audio_mix_push_s16(wc_audio_mix_t* m, const int16_t* lr, uint32_t frames) {
    for (uint32_t i = 0; i < frames && m->count < m->cap; i++) {
        uint32_t at = (m->head + m->count) % m->cap;
        m->ring[at * 2] = lr[i * 2] / 32768.0f;
        m->ring[at * 2 + 1] = lr[i * 2 + 1] / 32768.0f;
        m->count++;
    }
}

static inline void audio_mix_set_paused(wc_audio_mix_t* m, bool paused) {
    m->paused = paused;
}

// Consumer: fill `frames` frames of interleaved stereo float.
static inline void audio_mix_pull(wc_audio_mix_t* m, float* out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        if (!m->playing && !m->paused && m->count >= m->prime) m->playing = true;

        float l, r;
        if (m->playing && m->count > 0) {
            l = m->ring[m->head * 2];
            r = m->ring[m->head * 2 + 1];
            m->head = (m->head + 1) % m->cap;
            m->count--;
        } else {
            // Dry, or not started: let the last sample die away rather
            // than step to zero, and wait to prime again before playing.
            if (m->playing && m->count == 0) m->playing = false;
            l = m->last_l * m->decay;
            r = m->last_r * m->decay;
        }
        m->last_l = l;
        m->last_r = r;

        float target = (m->paused || !m->playing) ? 0.0f : 1.0f;
        if (m->gain < target) m->gain = fminf(target, m->gain + m->step);
        else if (m->gain > target) m->gain = fmaxf(target, m->gain - m->step);

        out[i * 2] = l * m->gain;
        out[i * 2 + 1] = r * m->gain;

        // Faded all the way out while paused: drop what's left, so the
        // resume starts from the cart's next samples, not old ones.
        if (m->paused && m->gain == 0.0f && m->count > 0) {
            m->head = (m->head + m->count) % m->cap;
            m->count = 0;
            m->playing = false;
        }
    }
}

#endif
