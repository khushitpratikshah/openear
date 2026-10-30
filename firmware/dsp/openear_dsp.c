/*
 * OpenEar DSP core - see openear_dsp.h for the signal chain.
 * Licence: MIT. Not a medical device.
 */
#include "openear_dsp.h"

#include <math.h>
#include <string.h>

#define OE_PI 3.14159265358979f
#define OE_BUTTERWORTH_Q 0.70710678f
/* Rectified-average envelope of a sine is (2/pi)*peak; scale so a full-scale
 * sine reads 0 dBFS. */
#define OE_ENV_TO_PEAK (OE_PI / 2.0f)

const float OE_XOVER_HZ[OE_NUM_XOVER] = { 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f };
const float OE_AUDIOGRAM_HZ[OE_NUM_BANDS] = { 250.0f,  500.0f,  1000.0f,
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

/* howl guard: the highest band gain (bypass is flat, 0 dB) */
static float howl_gmax(const oe_params_t *p)
{
    if (p->bypass) {
        return 0.0f;
    }
    float g = p->band[0].gain_db;
    for (int k = 1; k < OE_NUM_BANDS; k++) {
        if (p->band[k].gain_db > g) {
            g = p->band[k].gain_db;
        }
    }
    return g;
}

/* howl guard: the attenuation that keeps the loop gain at the remembered
 * stable level for the current settings (0 without a howl memory) */
static float howl_req(const oe_dsp_t *d)
{
    if (!d->howl_mem) {
        return 0.0f;
    }
    return clampf(d->p.master_db + howl_gmax(&d->p) - d->howl_stable_db, 0.0f, OE_HOWL_MAX_ATTEN_DB);
}

static float db_to_lin(float db)
{
    return powf(10.0f, db * 0.05f);
}

static float lin_to_db(float lin)
{
    return 20.0f * log10f(lin > 1e-9f ? lin : 1e-9f);
}

static void biquad_set(oe_biquad_t *f, float b0, float b1, float b2, float a0, float a1,
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
void oe_biquad_lowpass(oe_biquad_t *f, float fs, float fc, float q)
{
    float w = 2.0f * OE_PI * fc / fs, c = cosf(w), a = sinf(w) / (2.0f * q);
    biquad_set(f, (1.0f - c) * 0.5f, 1.0f - c, (1.0f - c) * 0.5f, 1.0f + a, -2.0f * c,
               1.0f - a);
}

void oe_biquad_highpass(oe_biquad_t *f, float fs, float fc, float q)
{
    float w = 2.0f * OE_PI * fc / fs, c = cosf(w), a = sinf(w) / (2.0f * q);
    biquad_set(f, (1.0f + c) * 0.5f, -(1.0f + c), (1.0f + c) * 0.5f, 1.0f + a, -2.0f * c,
               1.0f - a);
}

void oe_biquad_allpass(oe_biquad_t *f, float fs, float fc, float q)
{
    float w = 2.0f * OE_PI * fc / fs, c = cosf(w), a = sinf(w) / (2.0f * q);
    biquad_set(f, 1.0f - a, -2.0f * c, 1.0f + a, 1.0f + a, -2.0f * c, 1.0f - a);
}

/* ------------------------------------------------------------------------- */
/* parameters                                                                */
/* ------------------------------------------------------------------------- */
void oe_dsp_default_params(oe_params_t *p)
{
    memset(p, 0, sizeof(*p));
    for (int k = 0; k < OE_NUM_BANDS; k++) {
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

void oe_dsp_set_params(oe_dsp_t *d, const oe_params_t *in)
{
    oe_params_t p = *in;

    for (int k = 0; k < OE_NUM_BANDS; k++) {
        oe_band_params_t *b = &p.band[k];
        b->gain_db = clampf(b->gain_db, OE_MIN_BAND_GAIN_DB, OE_MAX_BAND_GAIN_DB);
        b->knee_db = clampf(b->knee_db, -90.0f, 0.0f);
        b->ratio = clampf(b->ratio, 1.0f, 10.0f);
        b->exp_thresh_db = clampf(b->exp_thresh_db, -100.0f, b->knee_db);
        b->exp_ratio = clampf(b->exp_ratio, 1.0f, 4.0f);
    }
    p.master_db = clampf(p.master_db, OE_MIN_MASTER_DB, OE_MAX_MASTER_DB);
    p.limit_db = clampf(p.limit_db, OE_MIN_LIMIT_DB, OE_MAX_LIMIT_DB);
    p.attack_ms = clampf(p.attack_ms, 0.5f, 100.0f);
    p.release_ms = clampf(p.release_ms, 5.0f, 1000.0f);
    p.tone_hz = clampf(p.tone_hz, 20.0f, 0.45f * d->fs);
    /* a pure tone in an in-ear phone is uncomfortable long before it is
     * loud by speech standards: cap it well below the ceiling */
    p.tone_db = clampf(p.tone_db, -100.0f, OE_MAX_TONE_DB);
    if (p.tone_db > p.limit_db) {
        p.tone_db = p.limit_db;
    }

    d->p = p;
    /* a settings change that raises the loop gain past what the howl guard
     * remembers as stable takes the attenuation up now, so not even one
     * block plays louder (the per-block guard does the same) */
    float req = howl_req(d);
    if (d->stats.howl_atten_db < req) {
        d->stats.howl_atten_db = req;
    }
    d->att_c = expf(-1.0f / (p.attack_ms * 0.001f * d->fs));
    d->rel_c = expf(-1.0f / (p.release_ms * 0.001f * d->fs));
    d->limit_lin = db_to_lin(p.limit_db);
    d->master_tgt = db_to_lin(p.master_db - d->stats.howl_atten_db);
    float w = 2.0f * OE_PI * p.tone_hz / d->fs;
    d->tone_rc = cosf(w); /* a new frequency keeps the phase: no click */
    d->tone_rs = sinf(w);
    d->tone_amp = db_to_lin(p.tone_db);
    d->tone_glide_c = 1.0f - expf(-1.0f / (OE_TONE_LEVEL_MS * 0.001f * d->fs));
    if (d->tone_mix <= 0.0f) {
        d->tone_amp_now = d->tone_amp; /* a tone starting from silence fades in at its level */
    }
}

/* ------------------------------------------------------------------------- */
/* init                                                                      */
/* ------------------------------------------------------------------------- */
void oe_dsp_init(oe_dsp_t *d, float fs, const oe_params_t *p)
{
    memset(d, 0, sizeof(*d));
    d->fs = fs;

    for (int c = 0; c < OE_NUM_XOVER; c++) {
        for (int s = 0; s < 2; s++) {
            oe_biquad_lowpass(&d->lp[c][s], fs, OE_XOVER_HZ[c], OE_BUTTERWORTH_Q);
            oe_biquad_highpass(&d->hp[c][s], fs, OE_XOVER_HZ[c], OE_BUTTERWORTH_Q);
        }
    }
    /* LR4 LP + LR4 HP == 2nd-order all-pass at fc with Butterworth Q.
     * Band k is taken from crossover k, so it must be delayed by the
     * all-passes of every later crossover (k+1 .. last) to stay in phase. */
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        d->ap_count[k] = 0;
        for (int c = k + 1; c < OE_NUM_XOVER; c++) {
            oe_biquad_allpass(&d->ap[k][d->ap_count[k]++], fs, OE_XOVER_HZ[c],
                              OE_BUTTERWORTH_Q);
        }
    }

    d->lim_rel_c = expf(-1.0f / (0.050f * fs)); /* 50 ms limiter release */
    d->tone_c = 1.0f; /* tone oscillator starts at phase 0 */
    d->tone_s = 0.0f;
    d->master_lin = 1.0f;
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        d->g_lin[k] = d->g_tgt[k] = 1.0f;
    }
    oe_dsp_set_params(d, p);
}

/* ------------------------------------------------------------------------- */
/* WDRC gain computer (dB domain)                                            */
/* ------------------------------------------------------------------------- */
static float wdrc_gain_db(const oe_band_params_t *b, float level_db)
{
    float g = b->gain_db;
    if (level_db > b->knee_db) {
        g -= (level_db - b->knee_db) * (1.0f - 1.0f / b->ratio);
    }
    if (level_db < b->exp_thresh_db) {
        g -= (b->exp_thresh_db - level_db) * (b->exp_ratio - 1.0f);
    }
    return clampf(g, -60.0f, OE_MAX_BAND_GAIN_DB);
}

/* ------------------------------------------------------------------------- */
/* processing                                                                */
/* ------------------------------------------------------------------------- */
/* One biquad, then a second one, over a block. Coefficients and state live in
 * registers for the whole loop; the arithmetic is exactly oe_biquad_run().
 * x and y may be the same buffer. */
void oe_biquad2_block(oe_biquad_t *f, oe_biquad_t *g, const float *x, float *y, int n)
{
    const float fb0 = f->b0, fb1 = f->b1, fb2 = f->b2, fa1 = f->a1, fa2 = f->a2;
    const float gb0 = g->b0, gb1 = g->b1, gb2 = g->b2, ga1 = g->a1, ga2 = g->a2;
    float f1 = f->z1, f2 = f->z2, g1 = g->z1, g2 = g->z2;
    for (int i = 0; i < n; i++) {
        float xi = x[i];
        float u = fb0 * xi + f1;
        f1 = fb1 * xi - fa1 * u + f2;
        f2 = fb2 * xi - fa2 * u;
        float v = gb0 * u + g1;
        g1 = gb1 * u - ga1 * v + g2;
        g2 = gb2 * u - ga2 * v;
        y[i] = v;
    }
    f->z1 = f1;
    f->z2 = f2;
    g->z1 = g1;
    g->z2 = g2;
}

static void biquad1_block(oe_biquad_t *f, const float *x, float *y, int n)
{
    const float b0 = f->b0, b1 = f->b1, b2 = f->b2, a1 = f->a1, a2 = f->a2;
    float z1 = f->z1, z2 = f->z2;
    for (int i = 0; i < n; i++) {
        float xi = x[i];
        float u = b0 * xi + z1;
        z1 = b1 * xi - a1 * u + z2;
        z2 = b2 * xi - a2 * u;
        y[i] = u;
    }
    f->z1 = z1;
    f->z2 = z2;
}

/* Band k: phase-compensation all-passes, envelope follower, ramped gain,
 * added into the sum. Same per-sample arithmetic as the 0.2 per-sample loop. */
static void band_block(oe_dsp_t *d, int k, float *s, float *sum, float g_step, int n)
{
    int a = 0;
    for (; a + 1 < d->ap_count[k]; a += 2) {
        oe_biquad2_block(&d->ap[k][a], &d->ap[k][a + 1], s, s, n);
    }
    if (a < d->ap_count[k]) {
        biquad1_block(&d->ap[k][a], s, s, n);
    }
    const float att = d->att_c, rel = d->rel_c;
    const float att1 = 1.0f - att, rel1 = 1.0f - rel; /* same values as 1-c per sample */
    float env = d->env[k], g = d->g_lin[k];
    for (int i = 0; i < n; i++) {
        float r = fabsf(s[i]);
        float c, c1;
        if (r > env) {
            c = att;
            c1 = att1;
        } else {
            c = rel;
            c1 = rel1;
        }
        env = c * env + c1 * r;
        g += g_step;
        sum[i] += s[i] * g;
    }
    d->env[k] = env;
    d->g_lin[k] = g;
}

static void process_block(oe_dsp_t *d, const float *in, float *out, int n);

void oe_dsp_process(oe_dsp_t *d, const float *in, float *out, int n)
{
    while (n > 0) {
        int m = n > OE_MAX_BLOCK ? OE_MAX_BLOCK : n;
        process_block(d, in, out, m);
        in += m;
        out += m;
        n -= m;
    }
}

static void process_block(oe_dsp_t *d, const float *in, float *out, int n)
{
    const float inv_n = 1.0f / (float)n;
    float g_step[OE_NUM_BANDS];
    float max_env = 0.0f, in_peak = 0.0f;
    int bad_samples = 0;
    /* master gain is ramped too, so volume changes and howl-guard steps
     * never click */
    const float m_step = (d->master_tgt - d->master_lin) * inv_n;
    const float lim = d->limit_lin;
    float *rest = d->buf_rest, *band = d->buf_band, *sum = d->buf_sum;

    for (int k = 0; k < OE_NUM_BANDS; k++) {
        g_step[k] = (d->g_tgt[k] - d->g_lin[k]) * inv_n;
    }

    /* input peak; the crossover tree works on a copy (in may equal out) */
    for (int i = 0; i < n; i++) {
        float x = in[i];
        if (fabsf(x) > in_peak) {
            in_peak = fabsf(x);
        }
        rest[i] = x;
        sum[i] = 0.0f;
    }

    /* split into bands, band 0 (lowest) first. The filters always run, so
     * bypass/tone toggles are click-free and the CPU cost is constant. */
    for (int c = 0; c < OE_NUM_XOVER; c++) {
        oe_biquad2_block(&d->lp[c][0], &d->lp[c][1], rest, band, n);
        oe_biquad2_block(&d->hp[c][0], &d->hp[c][1], rest, rest, n);
        band_block(d, c, band, sum, g_step[c], n);
    }
    band_block(d, OE_NUM_BANDS - 1, rest, sum, g_step[OE_NUM_BANDS - 1], n);

    /* Test tone: fades in and out (and the mic out and in) over about 10 ms,
     * and level changes glide over about 5 ms. A hard switch at a random
     * point of the sine wave is a click, and a click is what a listener
     * answers "heard" to. The tone is mixed in after the master gain, so
     * its level is what was asked (capped at OE_MAX_TONE_DB) whatever the
     * volume or the howl guard are doing. */
    const float tone_step = 1.0f / (OE_TONE_FADE_MS * 0.001f * d->fs);
    const float tone_want = d->p.tone_on ? 1.0f : 0.0f;
    /* per-sample state in registers (written back below) */
    float master = d->master_lin, lim_env = d->lim_env, mix = d->tone_mix;
    float t_amp = d->tone_amp_now, t_c = d->tone_c, t_s = d->tone_s;
    const float t_rc = d->tone_rc, t_rs = d->tone_rs, t_target = d->tone_amp;
    const float t_glide = d->tone_glide_c, lim_rel = d->lim_rel_c;
    const int tone_active = (mix != tone_want) || (tone_want > 0.0f);
    if (tone_active && mix <= 0.0f) {
        t_c = 1.0f; /* a beep starting from silence starts at a zero crossing */
        t_s = 0.0f;
    }
    for (int i = 0; i < n; i++) {
        float y = sum[i] * master;
        master += m_step;

        if (tone_active) {
            if (mix < tone_want) {
                mix += tone_step;
                if (mix > tone_want) {
                    mix = tone_want;
                }
            } else if (mix > tone_want) {
                mix -= tone_step;
                if (mix < tone_want) {
                    mix = tone_want;
                }
            }
            t_amp += t_glide * (t_target - t_amp);
            float w = mix * mix * (3.0f - 2.0f * mix); /* smooth */
            float t = t_amp * t_s;
            float c2 = t_c * t_rc - t_s * t_rs; /* rotate the phasor by 2 pi f / fs */
            t_s = t_s * t_rc + t_c * t_rs;
            t_c = c2;
            y = (1.0f - w) * y + w * t;
        }

        /* peak limiter: instant attack, so |y * gain| <= ceiling always */
        float a = fabsf(y);
        lim_env = (a > lim_env) ? a : lim_env * lim_rel;
        if (lim_env > lim) {
            y *= lim / lim_env;
        }
        if (lim_env > max_env) {
            max_env = lim_env;
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
    if (tone_active) {
        /* keep the oscillator on the unit circle (float error would drift
         * its level by ~1e-7 per sample) */
        float g = 1.5f - 0.5f * (t_c * t_c + t_s * t_s);
        t_c *= g;
        t_s *= g;
    }
    d->master_lin = master;
    d->lim_env = lim_env;
    d->tone_mix = mix;
    d->tone_amp_now = t_amp;
    d->tone_c = t_c;
    d->tone_s = t_s;

    /* ---- once per block ---- */
    /* A NaN anywhere in the filter or envelope state would stay there for
     * ever: wipe the state and start again (the output was already silenced). */
    if (bad_samples || !(d->lim_env >= 0.0f) || !(d->master_lin >= 0.0f)) {
        oe_params_t keep = d->p;
        oe_stats_t st = d->stats;
        int hm = d->howl_mem; /* keep the howl memory: the feedback path is still there */
        float hs = d->howl_stable_db, hr = d->howl_relaxed_db;
        oe_dsp_init(d, d->fs, &keep);
        d->stats = st;
        d->howl_mem = hm;
        d->howl_stable_db = hs;
        d->howl_relaxed_db = hr;
        d->stats.dsp_resets++;
        return;
    }

    /* new gain targets, stats, howl guard */
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        d->g_lin[k] = d->g_tgt[k]; /* remove float drift from the ramp */
        float lvl = lin_to_db(d->env[k] * OE_ENV_TO_PEAK);
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
     * 3 dB and remember the loop gain that stopped the howl. Recover 1 dB
     * every 2 s of quiet, but only down to the attenuation that keeps the
     * loop gain at that remembered level (0.2.x recovered all the way and
     * howled again every ~7 s in a steady feedback path). The required
     * attenuation follows the user's settings: turning the volume (or the
     * highest band gain) down gives that much back, turning it up or leaving
     * bypass raises the attenuation at once, so the howl never comes back
     * from a settings change. The memory relaxes 1 dB per 30 s without a
     * near-howl (short loud sounds don't count) and is forgotten after
     * 24 dB, so gain comes back if the feedback path goes. */
    if (!d->p.tone_on) {
        float dt = (float)n / d->fs;
        float gmax = howl_gmax(&d->p);
        float req = howl_req(d);
        if (d->stats.howl_atten_db < req) {
            d->stats.howl_atten_db = req; /* a settings change raised the loop gain */
        }
        int self_driven = lin_to_db(in_peak) < d->p.limit_db - 10.0f;
        if (gr > 0.5f && self_driven) {
            d->howl_time_s += dt;
            d->howl_quiet_s = 0.0f;
            if (d->howl_time_s > 0.25f) {
                d->howl_relax_s = 0.0f; /* a near-howl, not a short loud burst */
            }
            if (d->howl_time_s > 0.5f && d->stats.howl_atten_db < OE_HOWL_MAX_ATTEN_DB) {
                float a = d->stats.howl_atten_db + 3.0f;
                d->stats.howl_atten_db = a < OE_HOWL_MAX_ATTEN_DB ? a : OE_HOWL_MAX_ATTEN_DB;
                d->stats.howl_events++;
                d->howl_time_s = 0.0f;
                d->howl_mem = 1;
                d->howl_stable_db = d->p.master_db + gmax - d->stats.howl_atten_db;
                d->howl_relaxed_db = 0.0f;
            }
        } else {
            /* leaky: brief dips in a howl don't reset the detector */
            d->howl_time_s -= 0.5f * dt;
            if (d->howl_time_s < 0.0f) {
                d->howl_time_s = 0.0f;
            }
            d->howl_quiet_s += dt;
            if (d->howl_quiet_s > 2.0f && d->stats.howl_atten_db > req) {
                float a = d->stats.howl_atten_db - 1.0f;
                d->stats.howl_atten_db = a > req ? a : req;
                d->howl_quiet_s = 0.0f;
            }
        }
        d->howl_relax_s += dt;
        if (d->howl_relax_s > 30.0f && d->howl_mem) {
            d->howl_stable_db += 1.0f;
            d->howl_relaxed_db += 1.0f;
            d->howl_relax_s = 0.0f;
            if (d->howl_relaxed_db >= OE_HOWL_MAX_ATTEN_DB) {
                d->howl_mem = 0; /* 12 minutes without a near-howl: forget it */
            }
        }
        d->master_tgt = db_to_lin(d->p.master_db - d->stats.howl_atten_db);
    }
}

/* ------------------------------------------------------------------------- */
/* fitting helper                                                            */
/* ------------------------------------------------------------------------- */
void oe_fit_half_gain(oe_params_t *p, const float hl_db[OE_NUM_BANDS])
{
    /* Bands sit between audiogram points: band k spans roughly
     * hl[k-1]..hl[k], so use the mean of its two edges. */
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        float hl = (k == 0) ? hl_db[0] : 0.5f * (hl_db[k - 1] + hl_db[k]);
        hl = clampf(hl, 0.0f, 90.0f);

        oe_band_params_t *b = &p->band[k];
        b->gain_db = clampf(0.5f * hl, 0.0f, OE_MAX_BAND_GAIN_DB);
        b->ratio = clampf(1.0f + hl / 60.0f, 1.0f, 3.0f);
        b->knee_db = -45.0f;
        b->exp_thresh_db = -70.0f;
        b->exp_ratio = 2.0f;
    }
}
