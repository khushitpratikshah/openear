/*
 * OpenEar: int16 <-> float sample conversion used by the audio interrupt.
 * Header-only so the PC tests and the cycle bench run the exact same code.
 * Licence: MIT.
 */
#ifndef OPENEAR_AUDIO_CONVERT_H
#define OPENEAR_AUDIO_CONVERT_H

#include <stdint.h>

static inline void audio_to_float(const int16_t *in, float *x, int n)
{
    for (int i = 0; i < n; i++) {
        x[i] = (float)in[i] * (1.0f / 32768.0f);
    }
}

/* Float [-1, 1) -> int16, rounded to nearest (halves away from zero).
 * Out-of-range values clip; a NaN becomes silence rather than garbage.
 * (Replaces lrintf(), which is a library call per sample on the M4.) */
static inline void audio_from_float(const float *y, int16_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        float v = y[i] * 32767.0f;
        int16_t s;
        if (v >= 0.0f) {
            s = (v >= 32766.5f) ? (int16_t)32767 : (int16_t)(int32_t)(v + 0.5f);
        } else if (v < 0.0f) {
            s = (v <= -32767.5f) ? (int16_t)-32768 : (int16_t)(int32_t)(v - 0.5f);
        } else {
            s = 0; /* NaN fails both comparisons */
        }
        out[i] = s;
    }
}

#endif
