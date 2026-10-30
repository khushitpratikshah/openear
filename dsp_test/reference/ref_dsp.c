/* FROZEN REFERENCE: the OpenEar 0.2.2 DSP (per-sample loop), symbols renamed
 * ref_oe_* so tests can compare the optimised firmware DSP against it.
 * Do not edit. */
/*
 * OpenEar DSP core - see openear_dsp.h for the signal chain.
 * Licence: MIT. Not a medical device.
 */
#include "ref_dsp.h"

#include <math.h>
#include <string.h>

#define REF_OE_PI 3.14159265358979f
#define REF_OE_BUTTERWORTH_Q 0.70710678f
/* Rectified-average envelope of a sine is (2/pi)*peak; scale so a full-scale
 * sine reads 0 dBFS. */
#define REF_OE_ENV_TO_PEAK (REF_OE_PI / 2.0f)

const float REF_OE_XOVER_HZ[REF_OE_NUM_XOVER] = { 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f };
const float REF_OE_AUDIOGRAM_HZ[REF_OE_NUM_BANDS] = { 250.0f,  500.0f,  1000.0f,
                                              2000.0f, 4000.0f, 8000.0f };

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */
/* NaN-safe: anything that isn't a number between lo and hi goes to lo,
 * and every caller passes the quiet/safe end as lo. */
static float clampf(float x, float lo, float hi)
{
    if (!(x >= lo)) {
        return lo;
    }
    if (!(x <= hi)) {
        return hi;
    }
    return x;
}

static float db_to_lin(float db)
{
    return powf(10.0f, db * 0.05f);
}

static float lin_to_db(float lin)
{
    return 20.0f * log10f(lin > 1e-9f ? lin : 1e-9f);
}

static void biquad_set(ref_oe_biquad_t *f, float b0, float b1, float b2, float a0, float a1,
                       float a2)
{
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
    f->z1 = f->z2 = 0.0f;
}

/* RBJ Audio EQ Cookbook designs */
void ref_oe_biquad_lowpass(ref_oe_biquad_t *f, float fs, float fc, float q)
{
    float w = 2.0f * REF_OE_PI * fc / fs, c = cosf(w), a = sinf(w) / (2.0f * q);
    biquad_set(f, (1.0f - c) * 0.5f, 1.0f - c, (1.0f - c) * 0.5f, 1.0f + a, -2.0f * c,
               1.0f - a);
}

void ref_oe_biquad_highpass(ref_oe_biquad_t *f, float fs, float fc, float q)
{
    float w = 2.0f * REF_OE_PI * fc / fs, c = cosf(w), a = sinf(w) / (2.0f * q);
    biquad_set(f, (1.0f + c) * 0.5f, -(1.0f + c), (1.0f + c) * 0.5f, 1.0f + a, -2.0f * c,
               1.0f - a);
}

void ref_oe_biquad_allpass(ref_oe_biquad_t *f, float fs, float fc, float q)
{
    float w = 2.0f * REF_OE_PI * fc / fs, c = cosf(w), a = sinf(w) / (2.0f * q);
    biquad_set(f, 1.0f - a, -2.0f * c, 1.0f + a, 1.0f + a, -2.0f * c, 1.0f - a);
}

/* ------------------------------------------------------------------------- */
/* parameters                                                                */
/* ------------------------------------------------------------------------- */
void ref_oe_dsp_default_params(ref_oe_params_t *p)
{
    memset(p, 0, sizeof(*p));
    for (int k = 0; k < REF_OE_NUM_BANDS; k++) {
        p->band[k].gain_db = 0.0f;
        p->band[k].knee_db = -45.0f;
        p->band[k].ratio = 1.0f;          /* linear until fitted */
        p->band[k].exp_thresh_db = -70.0f;
        p->band[k].exp_ratio = 1.0f;      /* expansion off until enabled */
    }
    p->master_db = 0.0f;
    p->limit_db = -6.0f;
    p->attack_ms = 5.0f;
    p->release_ms = 80.0f;
    p->bypass = 0;
    p->tone_on = 0;
    p->tone_hz = 1000.0f;
    p->tone_db = -40.0f;
}

void ref_oe_dsp_set_params(ref_oe_dsp_t *d, const ref_oe_params_t *in)
{
    ref_oe_params_t p = *in;

    for (int k = 0; k < REF_OE_NUM_BANDS; k++) {
        ref_oe_band_params_t *b = &p.band[k];
        b->gain_db = clampf(b->gain_db, REF_OE_MIN_BAND_GAIN_DB, REF_OE_MAX_BAND_GAIN_DB);
        b->knee_db = clampf(b->knee_db, -90.0f, 0.0f);
        b->ratio = clampf(b->ratio, 1.0f, 10.0f);
        b->exp_thresh_db = clampf(b->exp_thresh_db, -100.0f, b->knee_db);
        b->exp_ratio = clampf(b->exp_ratio, 1.0f, 4.0f);
    }
    p.master_db = clampf(p.master_db, REF_OE_MIN_MASTER_DB, REF_OE_MAX_MASTER_DB);
    p.limit_db = clampf(p.limit_db, REF_OE_MIN_LIMIT_DB, REF_OE_MAX_LIMIT_DB);
    p.attack_ms = clampf(p.attack_ms, 0.5f, 100.0f);
    p.release_ms = clampf(p.release_ms, 5.0f, 1000.0f);
    p.tone_hz = clampf(p.tone_hz, 20.0f, 0.45f * d->fs);
    /* a pure tone in an in-ear phone is uncomfortable long before it is
     * loud by speech standards: cap it well below the ceiling */
    p.tone_db = clampf(p.tone_db, -100.0f, REF_OE_MAX_TONE_DB);
    if (p.tone_db > p.limit_db) {
        p.tone_db = p.limit_db;
    }

    d->p = p;
    d->att_c = expf(-1.0f / (p.attack_ms * 0.001f * d->fs));
    d->rel_c = expf(-1.0f / (p.release_ms * 0.001f * d->fs));
    d->limit_lin = db_to_lin(p.limit_db);
    d->master_tgt = db_to_lin(p.master_db - d->stats.howl_atten_db);
    d->tone_inc = 2.0f * REF_OE_PI * p.tone_hz / d->fs;
    d->tone_amp = db_to_lin(p.tone_db);
}

/* ------------------------------------------------------------------------- */
/* init                                                                      */
/* ------------------------------------------------------------------------- */
void ref_oe_dsp_init(ref_oe_dsp_t *d, float fs, const ref_oe_params_t *p)
{
    memset(d, 0, sizeof(*d));
    d->fs = fs;

    for (int c = 0; c < REF_OE_NUM_XOVER; c++) {
        for (int s = 0; s < 2; s++) {
            ref_oe_biquad_lowpass(&d->lp[c][s], fs, REF_OE_XOVER_HZ[c], REF_OE_BUTTERWORTH_Q);
            ref_oe_biquad_highpass(&d->hp[c][s], fs, REF_OE_XOVER_HZ[c], REF_OE_BUTTERWORTH_Q);
        }
    }
    /* LR4 LP + LR4 HP == 2nd-order all-pass at fc with Butterworth Q.
     * Band k is taken from crossover k, so it must be delayed by the
     * all-passes of every later crossover (k+1 .. last) to stay in phase. */
    for (int k = 0; k < REF_OE_NUM_BANDS; k++) {
        d->ap_count[k] = 0;
        for (int c = k + 1; c < REF_OE_NUM_XOVER; c++) {
            ref_oe_biquad_allpass(&d->ap[k][d->ap_count[k]++], fs, REF_OE_XOVER_HZ[c],
                              REF_OE_BUTTERWORTH_Q);
        }
    }

    d->lim_rel_c = expf(-1.0f / (0.050f * fs)); /* 50 ms limiter release */
    d->master_lin = 1.0f;
    for (int k = 0; k < REF_OE_NUM_BANDS; k++) {
        d->g_lin[k] = d->g_tgt[k] = 1.0f;
    }
    ref_oe_dsp_set_params(d, p);
}

/* ------------------------------------------------------------------------- */
/* WDRC gain computer (dB domain)                                            */
/* ------------------------------------------------------------------------- */
static float wdrc_gain_db(const ref_oe_band_params_t *b, float level_db)
{
    float g = b->gain_db;
    if (level_db > b->knee_db) {
        g -= (level_db - b->knee_db) * (1.0f - 1.0f / b->ratio);
    }
    if (level_db < b->exp_thresh_db) {
        g -= (b->exp_thresh_db - level_db) * (b->exp_ratio - 1.0f);
    }
    return clampf(g, -60.0f, REF_OE_MAX_BAND_GAIN_DB);
}

/* ------------------------------------------------------------------------- */
/* processing                                                                */
/* ------------------------------------------------------------------------- */
void ref_oe_dsp_process(ref_oe_dsp_t *d, const float *in, float *out, int n)
{
    const float inv_n = 1.0f / (float)n;
    float g_step[REF_OE_NUM_BANDS];
    float max_env = 0.0f, in_peak = 0.0f;
    int bad_samples = 0;
    /* master gain is ramped too, so volume changes and howl-guard steps
     * never click */
    const float m_step = (d->master_tgt - d->master_lin) * inv_n;
    const float lim = d->limit_lin;

    for (int k = 0; k < REF_OE_NUM_BANDS; k++) {
        g_step[k] = (d->g_tgt[k] - d->g_lin[k]) * inv_n;
    }

    const float tone_step = 1.0f / (REF_OE_TONE_FADE_MS * 0.001f * d->fs);
    for (int i = 0; i < n; i++) {
        float x = in[i];
        float y;
        if (fabsf(x) > in_peak) {
            in_peak = fabsf(x);
        }

        /* split into bands (the filters always run so bypass/tone toggles
         * are click-free and the CPU cost is constant) */
        float band[REF_OE_NUM_BANDS];
        float rest = x;
        for (int c = 0; c < REF_OE_NUM_XOVER; c++) {
            float lo = ref_oe_biquad_run(&d->lp[c][1], ref_oe_biquad_run(&d->lp[c][0], rest));
            rest = ref_oe_biquad_run(&d->hp[c][1], ref_oe_biquad_run(&d->hp[c][0], rest));
            band[c] = lo;
        }
        band[REF_OE_NUM_BANDS - 1] = rest;

        y = 0.0f;
        for (int k = 0; k < REF_OE_NUM_BANDS; k++) {
            float s = band[k];
            for (int a = 0; a < d->ap_count[k]; a++) {
                s = ref_oe_biquad_run(&d->ap[k][a], s);
            }
            /* envelope follower on the band signal */
            float r = fabsf(s);
            float c = (r > d->env[k]) ? d->att_c : d->rel_c;
            d->env[k] = c * d->env[k] + (1.0f - c) * r;

            d->g_lin[k] += g_step[k];
            y += s * d->g_lin[k];
        }

        /* Test tone: fade it in and out (and the mic out and in) over about
         * 10 ms. Switching hard at a random point of the sine wave makes a
         * click, and a click is what a listener answers "heard" to. */
        {
            float want = d->p.tone_on ? 1.0f : 0.0f;
            if (d->tone_mix < want) {
                d->tone_mix = fminf(want, d->tone_mix + tone_step);
            } else if (d->tone_mix > want) {
                d->tone_mix = fmaxf(want, d->tone_mix - tone_step);
            }
            if (d->tone_mix > 0.0f) {
                float w = d->tone_mix * d->tone_mix * (3.0f - 2.0f * d->tone_mix); /* smooth */
                float t = d->tone_amp * sinf(d->tone_phase);
                d->tone_phase += d->tone_inc;
                if (d->tone_phase > 2.0f * REF_OE_PI) {
                    d->tone_phase -= 2.0f * REF_OE_PI;
                }
                y = (1.0f - w) * y + w * t;
            }
        }
        y *= d->master_lin;
        d->master_lin += m_step;

        /* peak limiter: instant attack, so |y * gain| <= ceiling always */
        float a = fabsf(y);
        d->lim_env = (a > d->lim_env) ? a : d->lim_env * d->lim_rel_c;
        if (d->lim_env > lim) {
            y *= lim / d->lim_env;
        }
        if (d->lim_env > max_env) {
            max_env = d->lim_env;
        }
        /* belt and braces hard clip, NaN-safe: a non-number becomes silence */
        if (!(fabsf(y) <= lim)) {
            if (y > 0.0f) {
                y = lim;
            } else if (y < 0.0f) {
                y = -lim;
            } else {
                y = 0.0f;
                bad_samples++;
            }
            d->stats.clip_samples++;
        }
        out[i] = y;
    }

    /* ---- once per block ---- */
    /* A NaN anywhere in the filter or envelope state would stay there for
     * ever: wipe the state and start again (the output was already silenced). */
    if (bad_samples || !(d->lim_env >= 0.0f) || !(d->master_lin >= 0.0f)) {
        ref_oe_params_t keep = d->p;
        ref_oe_stats_t st = d->stats;
        ref_oe_dsp_init(d, d->fs, &keep);
        d->stats = st;
        d->stats.dsp_resets++;
        return;
    }

    /* new gain targets, stats, howl guard */
    for (int k = 0; k < REF_OE_NUM_BANDS; k++) {
        d->g_lin[k] = d->g_tgt[k]; /* remove float drift from the ramp */
        float lvl = lin_to_db(d->env[k] * REF_OE_ENV_TO_PEAK);
        /* bypass = flat 0 dB, reached through the same per-block ramp so
         * toggling it never jumps */
        float gdb = d->p.bypass ? 0.0f : wdrc_gain_db(&d->p.band[k], lvl);
        d->g_tgt[k] = db_to_lin(gdb);
        d->stats.level_db[k] = lvl;
        d->stats.gain_db[k] = gdb;
    }

    d->master_lin = d->master_tgt; /* remove float drift from the ramp */
    float gr = (max_env > lim) ? lin_to_db(max_env / lim) : 0.0f;
    d->stats.limiter_gr_db = gr;

    /* Howl guard. In a feedback loop the limiter settles at exactly the
     * excess loop gain (often only ~1 dB), so the cue is *sustained* limiting
     * of any depth, not deep limiting. A loud room also causes limiting, so
     * only count it when the *input* is well below the ceiling, i.e. our own
     * gain is what pushed the output into the limiter. Step master gain down
     * 3 dB, recover 1 dB every 2 s of quiet. */
    if (!d->p.tone_on) {
        float dt = (float)n / d->fs;
        int self_driven = lin_to_db(in_peak) < d->p.limit_db - 10.0f;
        if (gr > 0.5f && self_driven) {
            d->howl_time_s += dt;
            d->howl_quiet_s = 0.0f;
            if (d->howl_time_s > 0.5f && d->stats.howl_atten_db < 24.0f) {
                d->stats.howl_atten_db += 3.0f;
                d->stats.howl_events++;
                d->howl_time_s = 0.0f;
            }
        } else {
            /* leaky: brief dips in a howl don't reset the detector */
            d->howl_time_s -= 0.5f * dt;
            if (d->howl_time_s < 0.0f) {
                d->howl_time_s = 0.0f;
            }
            d->howl_quiet_s += dt;
            if (d->howl_quiet_s > 2.0f && d->stats.howl_atten_db > 0.0f) {
                d->stats.howl_atten_db -= 1.0f;
                d->howl_quiet_s = 0.0f;
            }
        }
        d->master_tgt = db_to_lin(d->p.master_db - d->stats.howl_atten_db);
    }
}

/* ------------------------------------------------------------------------- */
/* fitting helper                                                            */
/* ------------------------------------------------------------------------- */
void ref_oe_fit_half_gain(ref_oe_params_t *p, const float hl_db[REF_OE_NUM_BANDS])
{
    /* Bands sit between audiogram points: band k spans roughly
     * hl[k-1]..hl[k], so use the mean of its two edges. */
    for (int k = 0; k < REF_OE_NUM_BANDS; k++) {
        float hl = (k == 0) ? hl_db[0] : 0.5f * (hl_db[k - 1] + hl_db[k]);
        hl = clampf(hl, 0.0f, 90.0f);

        ref_oe_band_params_t *b = &p->band[k];
        b->gain_db = clampf(0.5f * hl, 0.0f, REF_OE_MAX_BAND_GAIN_DB);
        b->ratio = clampf(1.0f + hl / 60.0f, 1.0f, 3.0f);
        b->knee_db = -45.0f;
        b->exp_thresh_db = -70.0f;
        b->exp_ratio = 2.0f;
    }
}
