/*
 * OpenEar DSP core
 * -----------------------------------------------------------------------------
 * Portable C99, float only, no OS and no hardware dependencies, so the exact
 * same file runs on the MAX32655 Cortex-M4F and in the PC test bench.
 *
 * Signal chain (per sample, mono):
 *
 *   in -> 6-band Linkwitz-Riley crossover (5 crossovers, LR4)
 *         -> per band: envelope -> WDRC gain (gain, compression, expansion)
 *      -> sum -> master gain (incl. howl guard)
 *      -> [test tone crossfades in here, after the master gain, so its level
 *          is exactly what was asked whatever the volume]
 *      -> peak limiter -> hard clip
 *
 * Processing is done filter by filter over the whole block (not sample by
 * sample through every filter), so each filter's coefficients stay in CPU
 * registers. The arithmetic per sample is unchanged; it is about 1.7x faster
 * on the Cortex-M4F (estimate: firmware_test/cycle_bench.py).
 *
 * The crossover tree uses all-pass phase compensation, so with every band
 * at 0 dB the summed output has a perfectly flat magnitude response. All
 * processing is time-domain (IIR), which adds no frame delay: the only
 * algorithmic latency is the filters' group delay.
 *
 * Licence: MIT. Not a medical device.
 */
#ifndef OPENEAR_DSP_H
#define OPENEAR_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OE_NUM_BANDS 6
#define OE_NUM_XOVER (OE_NUM_BANDS - 1)

/* Safety limits enforced by oe_dsp_set_params(), whatever the caller asks. */
#define OE_MAX_BAND_GAIN_DB 30.0f
#define OE_MIN_BAND_GAIN_DB -30.0f
#define OE_MAX_MASTER_DB 12.0f
#define OE_MIN_MASTER_DB -60.0f
#define OE_MAX_LIMIT_DB -3.0f /* output ceiling can never exceed -3 dBFS */
#define OE_MIN_LIMIT_DB -40.0f
#define OE_MAX_TONE_DB -20.0f /* test tones never louder than this */
#define OE_TONE_FADE_MS 10.0f /* tone fades in/out so it never clicks */
#define OE_TONE_LEVEL_MS 5.0f /* tone level changes glide, so they never click */
#define OE_HOWL_MAX_ATTEN_DB 24.0f /* the howl guard never pulls the gain back further */
/* Largest block processed in one go; longer calls are split into pieces of
 * this size (each piece then counts as one block for the per-block updates). */
#define OE_MAX_BLOCK 256

/* Crossover frequencies (Hz). Bands: <250, 250-500, 500-1k, 1k-2k, 2k-4k, >4k */
extern const float OE_XOVER_HZ[OE_NUM_XOVER];
/* Standard audiogram frequencies used by the fitting helper (Hz). */
extern const float OE_AUDIOGRAM_HZ[OE_NUM_BANDS];

typedef struct {
    float b0, b1, b2, a1, a2; /* normalised, a0 = 1 */
    float z1, z2;             /* transposed direct form II state */
} oe_biquad_t;

typedef struct {
    float gain_db;       /* gain applied to soft/moderate input (dB) */
    float knee_db;       /* compression threshold (dBFS) */
    float ratio;         /* compression ratio above knee (1 = linear) */
    float exp_thresh_db; /* expansion threshold (dBFS); below it gain falls */
    float exp_ratio;     /* expansion ratio (1 = off) */
} oe_band_params_t;

typedef struct {
    oe_band_params_t band[OE_NUM_BANDS];
    float master_db;  /* overall volume (dB) */
    float limit_db;   /* output ceiling (dBFS) */
    float attack_ms;  /* band envelope attack */
    float release_ms; /* band envelope release */
    int bypass;       /* 1 = flat 0 dB, ramped in, still limited (fair A/B) */
    int tone_on;      /* 1 = output a test tone instead of the mic */
    float tone_hz;
    float tone_db;    /* tone level (dBFS), clipped to limit_db */
} oe_params_t;

typedef struct {
    float level_db[OE_NUM_BANDS]; /* last band input level (dBFS) */
    float gain_db[OE_NUM_BANDS];  /* last applied band gain (dB) */
    float limiter_gr_db;          /* max limiter gain reduction, last block */
    float howl_atten_db;          /* current howl-guard attenuation */
    uint32_t howl_events;         /* times the howl guard stepped in */
    uint32_t clip_samples;        /* samples hitting the hard clip */
    uint32_t dsp_resets;          /* times a NaN forced a state reset */
} oe_stats_t;

typedef struct {
    float fs;
    oe_params_t p;

    /* crossover: each LR4 filter is two identical cascaded Butterworth biquads */
    oe_biquad_t lp[OE_NUM_XOVER][2];
    oe_biquad_t hp[OE_NUM_XOVER][2];
    /* phase compensation all-passes; band k uses crossovers k+1..4 */
    oe_biquad_t ap[OE_NUM_BANDS][OE_NUM_XOVER];
    int ap_count[OE_NUM_BANDS];

    /* dynamics */
    float att_c, rel_c;
    float env[OE_NUM_BANDS];
    float g_lin[OE_NUM_BANDS]; /* gain currently applied (linear) */
    float g_tgt[OE_NUM_BANDS]; /* gain to reach by end of next block */

    /* limiter */
    float lim_env, lim_rel_c, limit_lin;
    float master_lin; /* applied now (ramped per sample) */
    float master_tgt; /* target for the end of this block */

    /* howl guard */
    float howl_time_s, howl_quiet_s;
    /* memory of the last howl: the loop gain (master + the highest band
     * gain - attenuation) that stopped it. The attenuation is held so the
     * loop gain stays at or below it, whatever the volume, band gains or
     * bypass do. The highest band, because a feedback path that howled in
     * one band howls in the next when that one is cut. It relaxes 1 dB per
     * 30 s without a near-howl and is forgotten after 24 dB (12 minutes). */
    int howl_mem; /* 1 = a howl is remembered */
    float howl_stable_db;
    float howl_relaxed_db;
    float howl_relax_s; /* quiet time towards the next 1 dB relax step */

    /* tone generator */
    float tone_c, tone_s;   /* oscillator state: cos and sin of the phase */
    float tone_rc, tone_rs; /* rotation per sample: cos and sin of 2 pi f / fs */
    float tone_amp;
    float tone_mix;     /* 0 = mic only .. 1 = tone only; ramps over OE_TONE_FADE_MS */
    float tone_amp_now; /* glides to tone_amp over OE_TONE_LEVEL_MS */
    float tone_glide_c;

    /* block scratch: remainder of the crossover tree, one band, the sum */
    float buf_rest[OE_MAX_BLOCK], buf_band[OE_MAX_BLOCK], buf_sum[OE_MAX_BLOCK];

    oe_stats_t stats;
} oe_dsp_t;

void oe_dsp_default_params(oe_params_t *p);
void oe_dsp_init(oe_dsp_t *d, float fs, const oe_params_t *p);
/* Safe to call between blocks; clamps everything to the safety limits. */
void oe_dsp_set_params(oe_dsp_t *d, const oe_params_t *p);
/* Process n mono samples, in-place allowed. Samples are floats in [-1, 1]. */
void oe_dsp_process(oe_dsp_t *d, const float *in, float *out, int n);

/*
 * Rough self-fitting helper: hearing levels (dB HL) at the six audiogram
 * frequencies -> band gains (half-gain rule) and compression ratios.
 * This is a heuristic for experiments, NOT a clinical prescription.
 */
void oe_fit_half_gain(oe_params_t *p, const float hl_db[OE_NUM_BANDS]);

/* Exposed for tests: biquad design and single-sample processing. */
void oe_biquad_lowpass(oe_biquad_t *f, float fs, float fc, float q);
void oe_biquad_highpass(oe_biquad_t *f, float fs, float fc, float q);
void oe_biquad_allpass(oe_biquad_t *f, float fs, float fc, float q);
/* f then g over a block (x may equal y): same arithmetic as two
 * oe_biquad_run() calls per sample, about 2x faster on the Cortex-M4F. */
void oe_biquad2_block(oe_biquad_t *f, oe_biquad_t *g, const float *x, float *y, int n);
static inline float oe_biquad_run(oe_biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENEAR_DSP_H */
