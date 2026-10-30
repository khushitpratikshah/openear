/* FROZEN REFERENCE: the OpenEar 0.2.2 DSP (per-sample loop), symbols renamed
 * ref_oe_* so tests can compare the optimised firmware DSP against it.
 * Do not edit. */
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
 *      -> sum -> master gain (incl. howl guard) -> peak limiter -> hard clip
 *
 * The crossover tree uses all-pass phase compensation, so with every band
 * at 0 dB the summed output has a perfectly flat magnitude response. All
 * processing is time-domain (IIR), which adds no frame delay: the only
 * algorithmic latency is the filters' group delay.
 *
 * Licence: MIT. Not a medical device.
 */
#ifndef REF_OPENEAR_DSP_H
#define REF_OPENEAR_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REF_OE_NUM_BANDS 6
#define REF_OE_NUM_XOVER (REF_OE_NUM_BANDS - 1)

/* Safety limits enforced by ref_oe_dsp_set_params(), whatever the caller asks. */
#define REF_OE_MAX_BAND_GAIN_DB 30.0f
#define REF_OE_MIN_BAND_GAIN_DB -30.0f
#define REF_OE_MAX_MASTER_DB 12.0f
#define REF_OE_MIN_MASTER_DB -60.0f
#define REF_OE_MAX_LIMIT_DB -3.0f /* output ceiling can never exceed -3 dBFS */
#define REF_OE_MIN_LIMIT_DB -40.0f
#define REF_OE_MAX_TONE_DB -20.0f /* test tones never louder than this */
#define REF_OE_TONE_FADE_MS 10.0f /* tone fades in/out so it never clicks */

/* Crossover frequencies (Hz). Bands: <250, 250-500, 500-1k, 1k-2k, 2k-4k, >4k */
extern const float REF_OE_XOVER_HZ[REF_OE_NUM_XOVER];
/* Standard audiogram frequencies used by the fitting helper (Hz). */
extern const float REF_OE_AUDIOGRAM_HZ[REF_OE_NUM_BANDS];

typedef struct {
    float b0, b1, b2, a1, a2; /* normalised, a0 = 1 */
    float z1, z2;             /* transposed direct form II state */
} ref_oe_biquad_t;

typedef struct {
    float gain_db;       /* gain applied to soft/moderate input (dB) */
    float knee_db;       /* compression threshold (dBFS) */
    float ratio;         /* compression ratio above knee (1 = linear) */
    float exp_thresh_db; /* expansion threshold (dBFS); below it gain falls */
    float exp_ratio;     /* expansion ratio (1 = off) */
} ref_oe_band_params_t;

typedef struct {
    ref_oe_band_params_t band[REF_OE_NUM_BANDS];
    float master_db;  /* overall volume (dB) */
    float limit_db;   /* output ceiling (dBFS) */
    float attack_ms;  /* band envelope attack */
    float release_ms; /* band envelope release */
    int bypass;       /* 1 = flat 0 dB, ramped in, still limited (fair A/B) */
    int tone_on;      /* 1 = output a test tone instead of the mic */
    float tone_hz;
    float tone_db;    /* tone level (dBFS), clipped to limit_db */
} ref_oe_params_t;

typedef struct {
    float level_db[REF_OE_NUM_BANDS]; /* last band input level (dBFS) */
    float gain_db[REF_OE_NUM_BANDS];  /* last applied band gain (dB) */
    float limiter_gr_db;          /* max limiter gain reduction, last block */
    float howl_atten_db;          /* current howl-guard attenuation */
    uint32_t howl_events;         /* times the howl guard stepped in */
    uint32_t clip_samples;        /* samples hitting the hard clip */
    uint32_t dsp_resets;          /* times a NaN forced a state reset */
} ref_oe_stats_t;

typedef struct {
    float fs;
    ref_oe_params_t p;

    /* crossover: each LR4 filter is two identical cascaded Butterworth biquads */
    ref_oe_biquad_t lp[REF_OE_NUM_XOVER][2];
    ref_oe_biquad_t hp[REF_OE_NUM_XOVER][2];
    /* phase compensation all-passes; band k uses crossovers k+1..4 */
    ref_oe_biquad_t ap[REF_OE_NUM_BANDS][REF_OE_NUM_XOVER];
    int ap_count[REF_OE_NUM_BANDS];

    /* dynamics */
    float att_c, rel_c;
    float env[REF_OE_NUM_BANDS];
    float g_lin[REF_OE_NUM_BANDS]; /* gain currently applied (linear) */
    float g_tgt[REF_OE_NUM_BANDS]; /* gain to reach by end of next block */

    /* limiter */
    float lim_env, lim_rel_c, limit_lin;
    float master_lin; /* applied now (ramped per sample) */
    float master_tgt; /* target for the end of this block */

    /* howl guard */
    float howl_time_s, howl_quiet_s;

    /* tone generator */
    float tone_phase, tone_inc, tone_amp;
    float tone_mix; /* 0 = mic only .. 1 = tone only; ramps over REF_OE_TONE_FADE_MS */

    ref_oe_stats_t stats;
} ref_oe_dsp_t;

void ref_oe_dsp_default_params(ref_oe_params_t *p);
void ref_oe_dsp_init(ref_oe_dsp_t *d, float fs, const ref_oe_params_t *p);
/* Safe to call between blocks; clamps everything to the safety limits. */
void ref_oe_dsp_set_params(ref_oe_dsp_t *d, const ref_oe_params_t *p);
/* Process n mono samples, in-place allowed. Samples are floats in [-1, 1]. */
void ref_oe_dsp_process(ref_oe_dsp_t *d, const float *in, float *out, int n);

/*
 * Rough self-fitting helper: hearing levels (dB HL) at the six audiogram
 * frequencies -> band gains (half-gain rule) and compression ratios.
 * This is a heuristic for experiments, NOT a clinical prescription.
 */
void ref_oe_fit_half_gain(ref_oe_params_t *p, const float hl_db[REF_OE_NUM_BANDS]);

/* Exposed for tests: biquad design and single-sample processing. */
void ref_oe_biquad_lowpass(ref_oe_biquad_t *f, float fs, float fc, float q);
void ref_oe_biquad_highpass(ref_oe_biquad_t *f, float fs, float fc, float q);
void ref_oe_biquad_allpass(ref_oe_biquad_t *f, float fs, float fc, float q);
static inline float ref_oe_biquad_run(ref_oe_biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

#ifdef __cplusplus
}
#endif

#endif /* REF_OPENEAR_DSP_H */
