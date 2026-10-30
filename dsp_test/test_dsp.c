/*
 * OpenEar DSP test bench (runs on a PC).
 *
 * Compiles the exact DSP file that runs on the MAX32655 and checks it
 * against measurable pass/fail criteria. Writes CSV files for plotting.
 *
 *   make && ./test_dsp results/
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openear_dsp.h"
#include "reference/ref_dsp.h"

#define FS 48000.0f
#ifndef BLOCK
#define BLOCK 64
#endif
#define PI 3.14159265358979

static int failures = 0;
static const char *outdir = ".";

#define CHECK(cond, ...)                         \
    do {                                         \
        if (cond) {                              \
            printf("  PASS  ");                  \
        } else {                                 \
            printf("  FAIL  ");                  \
            failures++;                          \
        }                                        \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
    } while (0)

static FILE *csv(const char *name, const char *header)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", outdir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(2);
    }
    fprintf(f, "%s\n", header);
    return f;
}

static void run(oe_dsp_t *d, const float *in, float *out, int n)
{
    for (int i = 0; i < n; i += BLOCK) {
        oe_dsp_process(d, in + i, out + i, BLOCK);
    }
}

/* Linear (no dynamics) params with given band gains */
static void linear_params(oe_params_t *p, const float *gains)
{
    oe_dsp_default_params(p);
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        p->band[k].gain_db = gains ? gains[k] : 0.0f;
        p->band[k].ratio = 1.0f;
        p->band[k].exp_ratio = 1.0f;
    }
    p->limit_db = -3.0f;
}

/* Steady-state sine gain in dB at frequency f and input level lvl_dbfs */
static double sine_gain_db(oe_dsp_t *d, double f, double lvl_dbfs, double *out_dbfs)
{
    const int warm = (int)(0.6 * FS) / BLOCK * BLOCK;
    const int meas = (int)(0.2 * FS) / BLOCK * BLOCK;
    const int n = warm + meas;
    float *in = malloc(sizeof(float) * n), *out = malloc(sizeof(float) * n);
    double a = pow(10.0, lvl_dbfs / 20.0);
    for (int i = 0; i < n; i++) {
        in[i] = (float)(a * sin(2.0 * PI * f * i / FS));
    }
    run(d, in, out, n);
    double si = 0, so = 0;
    for (int i = warm; i < n; i++) {
        si += (double)in[i] * in[i];
        so += (double)out[i] * out[i];
    }
    free(in);
    free(out);
    if (out_dbfs) {
        *out_dbfs = 10.0 * log10(so / meas * 2.0 + 1e-30); /* sine peak dBFS */
    }
    return 10.0 * log10((so + 1e-30) / (si + 1e-30));
}

/* ------------------------------------------------------------------------ */
static void test_flat_reconstruction(void)
{
    printf("\n[1] Crossover reconstruction (all bands 0 dB must be flat)\n");
    oe_params_t p;
    oe_dsp_t d;
    linear_params(&p, NULL);
    oe_dsp_init(&d, FS, &p);

    FILE *f = csv("flat_response.csv", "freq_hz,gain_db");
    double worst = 0;
    for (double fr = 50; fr <= 16000; fr *= 1.0905) { /* ~8 pts/octave */
        oe_dsp_init(&d, FS, &p);
        double g = sine_gain_db(&d, fr, -40.0, NULL);
        fprintf(f, "%.1f,%.4f\n", fr, g);
        if (fabs(g) > worst) {
            worst = fabs(g);
        }
    }
    fclose(f);
    CHECK(worst < 0.1, "max deviation from flat = %.4f dB (limit 0.1 dB)", worst);
}

/* ------------------------------------------------------------------------ */
static void test_band_gains(void)
{
    printf("\n[2] Band gains follow a fitted profile (sloping high-frequency loss)\n");
    const float hl[OE_NUM_BANDS] = { 10, 15, 25, 40, 55, 60 };
    oe_params_t p;
    oe_dsp_t d;
    oe_dsp_default_params(&p);
    oe_fit_half_gain(&p, hl);
    /* measure the gain curve below the knee and above expansion: linear zone */
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        p.band[k].exp_ratio = 1.0f;
    }
    p.limit_db = -3.0f;

    printf("  fitted gains (dB):");
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        printf(" %.1f", p.band[k].gain_db);
    }
    printf("\n");

    FILE *f = csv("fitted_response.csv", "freq_hz,gain_db");
    for (double fr = 50; fr <= 16000; fr *= 1.0905) {
        oe_dsp_init(&d, FS, &p);
        fprintf(f, "%.1f,%.4f\n", fr, sine_gain_db(&d, fr, -55.0, NULL));
    }
    fclose(f);

    /* check at mid-band frequencies */
    const double fc[OE_NUM_BANDS] = { 120, 354, 707, 1414, 2828, 8000 };
    double worst = 0;
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        oe_dsp_init(&d, FS, &p);
        double g = sine_gain_db(&d, fc[k], -55.0, NULL);
        double e = fabs(g - p.band[k].gain_db);
        if (e > worst) {
            worst = e;
        }
    }
    CHECK(worst < 1.5, "mid-band gain error = %.2f dB (limit 1.5 dB)", worst);
}

/* ------------------------------------------------------------------------ */
static void test_io_curve(void)
{
    printf("\n[3] Input/output curve at 2 kHz (expansion, gain, compression, limit)\n");
    oe_params_t p;
    oe_dsp_t d;
    oe_dsp_default_params(&p);
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        p.band[k].gain_db = 20.0f;
        p.band[k].knee_db = -45.0f;
        p.band[k].ratio = 2.0f;
        p.band[k].exp_thresh_db = -70.0f;
        p.band[k].exp_ratio = 2.0f;
    }
    p.limit_db = -6.0f;

    FILE *f = csv("io_curve.csv", "in_dbfs,out_dbfs");
    uint32_t loud_howl = 0;
    double out_at_m60 = 0, out_at_m50 = 0, out_at_m30 = 0, out_at_m20 = 0, out_max = -999;
    for (int lvl = -95; lvl <= 0; lvl += 1) {
        double o;
        oe_dsp_init(&d, FS, &p);
        sine_gain_db(&d, 2000.0, lvl, &o);
        fprintf(f, "%d,%.3f\n", lvl, o);
        if (lvl >= -15) loud_howl += d.stats.howl_events;
        if (lvl == -60) out_at_m60 = o;
        if (lvl == -50) out_at_m50 = o;
        if (lvl == -30) out_at_m30 = o;
        if (lvl == -20) out_at_m20 = o;
        if (o > out_max) out_max = o;
    }
    fclose(f);
    double lin_slope = (out_at_m50 - out_at_m60) / 10.0;
    double comp_slope = (out_at_m20 - out_at_m30) / 10.0;
    CHECK(fabs(lin_slope - 1.0) < 0.15, "slope in linear zone = %.2f (expect 1.0)", lin_slope);
    CHECK(fabs(comp_slope - 0.5) < 0.15, "slope above knee = %.2f (expect 0.5 for 2:1)",
          comp_slope);
    CHECK(loud_howl == 0, "loud steady sound does not false-trigger the howl guard (%u events)",
          loud_howl);
    CHECK(out_max <= -6.0 + 0.05, "max output = %.2f dBFS (ceiling -6 dBFS)", out_max);
}

/* ------------------------------------------------------------------------ */
static void test_limiter_never_exceeds(void)
{
    printf("\n[4] Safety: output never exceeds the ceiling, even with max gain\n");
    oe_params_t p;
    oe_dsp_t d;
    oe_dsp_default_params(&p);
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        p.band[k].gain_db = 99.0f; /* will be clamped to the safety max */
    }
    p.master_db = 99.0f;
    p.limit_db = 0.0f; /* will be clamped to -3 dBFS */
    oe_dsp_init(&d, FS, &p);

    const int n = (int)FS * 2 / BLOCK * BLOCK;
    float *in = malloc(sizeof(float) * n), *out = malloc(sizeof(float) * n);
    srand(1234);
    for (int i = 0; i < n; i++) {
        double noise = ((double)rand() / RAND_MAX - 0.5) * 1.6;
        double burst = (i > n / 2 && i < n / 2 + 4800) ? 1.0 : 0.0;
        in[i] = (float)fmax(-1.0, fmin(1.0, noise + burst));
    }
    run(&d, in, out, n);
    double peak = 0;
    for (int i = 0; i < n; i++) {
        if (fabs(out[i]) > peak) peak = fabs(out[i]);
    }
    double peak_db = 20 * log10(peak);
    CHECK(d.p.band[0].gain_db <= OE_MAX_BAND_GAIN_DB, "band gain clamped to %.0f dB",
          d.p.band[0].gain_db);
    CHECK(d.p.limit_db <= OE_MAX_LIMIT_DB, "ceiling clamped to %.0f dBFS", d.p.limit_db);
    CHECK(peak_db <= OE_MAX_LIMIT_DB + 0.01, "peak output = %.3f dBFS", peak_db);
    free(in);
    free(out);
}

/* ------------------------------------------------------------------------ */
static void test_latency(void)
{
    printf("\n[5] Algorithmic delay (group delay of the filter bank)\n");
    oe_params_t p;
    oe_dsp_t d;
    linear_params(&p, NULL);
    oe_dsp_init(&d, FS, &p);

    const int n = 4096;
    float in[4096] = { 0 }, out[4096];
    in[0] = 0.25f;
    run(&d, in, out, n);

    FILE *f = csv("impulse.csv", "ms,amplitude");
    int peak_i = 0;
    for (int i = 0; i < 960; i++) {
        fprintf(f, "%.4f,%.6f\n", 1000.0 * i / FS, out[i] / 0.25f);
    }
    for (int i = 0; i < n; i++) {
        if (fabs(out[i]) > fabs(out[peak_i])) peak_i = i;
    }
    fclose(f);

    /* group delay at selected frequencies via phase slope of the DFT */
    FILE *g = csv("group_delay.csv", "freq_hz,delay_ms");
    double gd_1k = 0, gd_2k = 0, gd_4k = 0;
    for (double fr = 100; fr <= 12000; fr *= 1.0905) {
        double df = 1.0, ph[2];
        for (int t = 0; t < 2; t++) {
            double w = 2 * PI * (fr + t * df) / FS, re = 0, im = 0;
            for (int i = 0; i < n; i++) {
                re += out[i] * cos(w * i);
                im -= out[i] * sin(w * i);
            }
            ph[t] = atan2(im, re);
        }
        double dph = ph[1] - ph[0];
        while (dph > PI) dph -= 2 * PI;
        while (dph < -PI) dph += 2 * PI;
        double gd = -dph / (2 * PI * df) * 1000.0;
        fprintf(g, "%.1f,%.4f\n", fr, gd);
        if (fabs(fr - 1000) < 50) gd_1k = gd;
        if (fabs(fr - 2000) < 100) gd_2k = gd;
        if (fabs(fr - 4000) < 200) gd_4k = gd;
    }
    fclose(g);
    printf("  impulse peak at %.3f ms; group delay ~%.2f ms @1k, %.2f ms @2k, %.2f ms @4k\n",
           1000.0 * peak_i / FS, gd_1k, gd_2k, gd_4k);
    CHECK(gd_1k < 2.0 && gd_2k < 2.0, "speech-band filter delay under 2 ms");
}

/* ------------------------------------------------------------------------ */
/* feedback loop: mic hears fb x the output 2 ms later, plus quiet room noise,
 * plus optional short loud bursts. Returns re-howls after the first pull-back
 * (0.5 s windows above -30 dBFS rms). */
static int howl_loop(oe_dsp_t *d, double seconds, double fb, unsigned seed, double burst_every_s,
                     FILE *f)
{
    const int n = (int)(FS * seconds) / BLOCK * BLOCK, delay = 96, win = (int)(FS / 2) / BLOCK * BLOCK;
    float *buf = calloc(n + delay, sizeof(float));
    float in[BLOCK], out[BLOCK];
    srand(seed);
    double we = 0;
    int rehowls = 0, loud_prev = 0, settled = 0;
    for (int b = 0; b < n / BLOCK; b++) {
        for (int i = 0; i < BLOCK; i++) {
            int idx = b * BLOCK + i;
            double noise = ((double)rand() / RAND_MAX - 0.5) * 2e-4;
            double t = idx / FS, burst = 0;
            if (burst_every_s > 0 && fmod(t, burst_every_s) < 0.005) burst = 0.1 * sin(2 * PI * 1500 * t);
            in[i] = (float)(fb * buf[idx] + noise + burst);
        }
        oe_dsp_process(d, in, out, BLOCK);
        double s = 0;
        for (int i = 0; i < BLOCK; i++) {
            buf[b * BLOCK + i + delay] = out[i];
            s += out[i] * out[i];
        }
        we += s;
        if (((b + 1) * BLOCK) % win == 0) {
            int loud = 10 * log10(we / win + 1e-20) > -30.0;
            if (!loud) settled = 1;
            if (settled && loud && !loud_prev) rehowls++;
            loud_prev = loud;
            we = 0;
        }
        if (f && b % 8 == 0) {
            fprintf(f, "%.4f,%.2f,%.1f\n", b * BLOCK / FS, 10 * log10(s / BLOCK + 1e-20), d->stats.howl_atten_db);
        }
    }
    free(buf);
    return rehowls;
}

static void howl_params(oe_params_t *p)
{
    oe_dsp_default_params(p);
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        p->band[k].gain_db = 30.0f;
        p->band[k].ratio = 1.0f;
    }
    p->master_db = 6.0f;
}

static void test_howl_guard(void)
{
    printf("\n[6] Howl guard: feedback is pulled back and stays back; gain returns when it's gone\n");
    /* Loop gain = 0.05 x 30 dB band gain x 6 dB master ~ 3.2 (> 1): without the
     * guard this howls for ever. 0.2.x pulled it back, recovered 1 dB / 2 s and
     * howled again every ~7 s (about 25 times in 3 minutes). */
    int worst60 = 0, worst180 = 0, engaged = 1;
    for (int seed = 1; seed <= 5; seed++) {
        oe_params_t p;
        static oe_dsp_t d;
        howl_params(&p);
        oe_dsp_init(&d, FS, &p);
        FILE *f = seed == 1 ? csv("howl_guard.csv", "t_s,out_rms_dbfs,howl_atten_db") : NULL;
        int r60 = howl_loop(&d, 60, 0.05, seed, 0, f);
        int r180 = r60 + howl_loop(&d, 120, 0.05, seed + 100, 0, f);
        if (f) fclose(f);
        engaged &= d.stats.howl_events > 0;
        if (r60 > worst60) worst60 = r60;
        if (r180 > worst180) worst180 = r180;
        printf("  seed %d: howl events %u, re-howls after the first pull-back: %d in 60 s, %d in 180 s\n", seed,
               d.stats.howl_events, r60, r180);
    }
    CHECK(engaged, "howl guard engaged under feedback (every seed)");
    CHECK(worst60 <= 1, "at most one brief re-howl in the first 60 s (worst seed %d)", worst60);
    CHECK(worst180 <= 6, "at most 6 brief re-howls in 3 minutes of steady feedback (worst seed %d; 0.2.x ~25)",
          worst180);

    /* the feedback path goes away (earphone pushed in), but short loud sounds
     * (a door knock every 20 s) keep coming: full gain must come back */
    oe_params_t p;
    static oe_dsp_t d;
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    float att0 = d.stats.howl_atten_db;
    howl_loop(&d, 480, 0.0, 9, 20.0, NULL);
    CHECK(att0 >= 6 && d.stats.howl_atten_db == 0.0f,
          "feedback gone, loud bursts every 20 s: attenuation %.0f dB -> %.0f dB within 8 minutes", att0,
          d.stats.howl_atten_db);

    /* User changes while the feedback path is still there. A re-howl is
     * counted from the output windows and from new howl events (a howl that
     * starts at once never shows a quiet window first). (round-3 review:
     * 0.3.0-rc cleared the remembered floor on any change, so a bypass A/B,
     * a volume step down or the app's hearing test brought the howl straight
     * back). Windows are 20 s, before the floor's 30 s relax step. */
    /* (a) volume 3 dB down: the guard gives back exactly those 3 dB, no howl */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    att0 = d.stats.howl_atten_db;
    p.master_db = 3.0f;
    oe_dsp_set_params(&d, &p);
    uint32_t ev0 = d.stats.howl_events;
    int rh = howl_loop(&d, 20, 0.05, 9, 0, NULL) + (int)(d.stats.howl_events - ev0);
    printf("  volume +6 -> +3 dB with feedback: attenuation %.0f -> %.0f dB, re-howls + new howl events %d\n", att0,
           d.stats.howl_atten_db, rh);
    CHECK(att0 >= 6 && rh == 0 && d.stats.howl_atten_db == att0 - 3.0f,
          "volume 3 dB down with feedback: no re-howl in 20 s, attenuation %.0f -> %.0f dB (expected %.0f)", att0,
          d.stats.howl_atten_db, att0 - 3.0f);

    /* (b) bypass A/B (SW2 or the app) for 5 s, then back on */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    att0 = d.stats.howl_atten_db;
    ev0 = d.stats.howl_events;
    p.bypass = 1;
    oe_dsp_set_params(&d, &p);
    howl_loop(&d, 5, 0.05, 9, 0, NULL);
    float att_byp = d.stats.howl_atten_db;
    /* bypass is flat: it can't howl here, so the guard lets go */
    CHECK(att_byp <= att0 - 2.0f, "in bypass the guard lets go (%.0f -> %.0f dB in 5 s)", att0, att_byp);
    p.bypass = 0;
    oe_dsp_set_params(&d, &p);
    rh = howl_loop(&d, 20, 0.05, 9, 0, NULL) + (int)(d.stats.howl_events - ev0);
    printf("  bypass on 5 s, off: attenuation %.0f -> %.0f dB, re-howls + new howl events %d\n", att0, d.stats.howl_atten_db, rh);
    /* (the floor may take its 1 dB relax step in these 33 s) */
    CHECK(rh == 0 && d.stats.howl_atten_db >= att0 - 1.0f,
          "bypass A/B with feedback: no re-howl in 20 s (%d), attenuation kept (%.0f -> %.0f dB)", rh, att0,
          d.stats.howl_atten_db);

    /* (c) the app's hearing test: vol 0, beeps, then the saved volume back */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    att0 = d.stats.howl_atten_db;
    ev0 = d.stats.howl_events;
    p.master_db = 0.0f;
    oe_dsp_set_params(&d, &p);
    p.tone_on = 1;
    p.tone_db = -40.0f;
    oe_dsp_set_params(&d, &p);
    howl_loop(&d, 5, 0.05, 9, 0, NULL);
    p.tone_on = 0;
    oe_dsp_set_params(&d, &p);
    p.master_db = 6.0f;
    oe_dsp_set_params(&d, &p);
    rh = howl_loop(&d, 20, 0.05, 9, 0, NULL) + (int)(d.stats.howl_events - ev0);
    printf("  hearing test (vol 0, tone, vol back): attenuation %.0f -> %.0f dB, re-howls + new howl events %d\n", att0,
           d.stats.howl_atten_db, rh);
    CHECK(rh == 0 && d.stats.howl_atten_db == att0, "hearing test then volume restored: no re-howl in 20 s (%d)",
          rh);

    /* (c2) live tuning: one band cut 6 dB for 5 s, then restored */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    att0 = d.stats.howl_atten_db;
    ev0 = d.stats.howl_events;
    p.band[3].gain_db = 24.0f;
    oe_dsp_set_params(&d, &p);
    howl_loop(&d, 5, 0.05, 9, 0, NULL);
    p.band[3].gain_db = 30.0f;
    oe_dsp_set_params(&d, &p);
    rh = howl_loop(&d, 20, 0.05, 9, 0, NULL) + (int)(d.stats.howl_events - ev0);
    CHECK(rh == 0, "band cut and restored with feedback: no re-howl in 20 s (%d; attenuation %.0f -> %.0f dB)", rh,
          att0, d.stats.howl_atten_db);

    /* (c3) round-4 review: the volume down for a while, then back up (SW3
     * steps -12 -> -6 -> 0). The guard gave back the cut while it was down;
     * going up must take it again at once, not after a new howl. */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    att0 = d.stats.howl_atten_db;
    ev0 = d.stats.howl_events;
    p.master_db = -6.0f;
    oe_dsp_set_params(&d, &p);
    howl_loop(&d, 30, 0.05, 9, 0, NULL);
    float att_low = d.stats.howl_atten_db;
    p.master_db = 0.0f;
    oe_dsp_set_params(&d, &p);
    rh = howl_loop(&d, 5, 0.05, 9, 0, NULL);
    p.master_db = 6.0f;
    oe_dsp_set_params(&d, &p);
    rh += howl_loop(&d, 10, 0.05, 9, 0, NULL) + (int)(d.stats.howl_events - ev0);
    printf("  volume -12 dB for 30 s, then +6, +6: attenuation %.0f -> %.0f -> %.0f dB, re-howls + new howl events %d\n",
           att0, att_low, d.stats.howl_atten_db, rh);
    CHECK(rh == 0 && att_low <= att0 - 11.0f,
          "volume down 12 dB for 30 s, then back up in two steps: no re-howl (%d); the cut was given back while "
          "down (attenuation %.0f dB)", rh, att_low);

    /* (c3b) with equal band gains the howl moves to the next band when one
     * is cut, so a one-band cut gives nothing back (and stays quiet); all
     * bands cut 6 dB give back 6 dB. (Round 4: tracking only the band that
     * howled re-howled here.) */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    att0 = d.stats.howl_atten_db;
    ev0 = d.stats.howl_events;
    rh = 0;
    for (int hb = 0; hb < OE_NUM_BANDS; hb++) {
        p.band[hb].gain_db = 24.0f; /* one band at a time, the others back at 30 */
        oe_dsp_set_params(&d, &p);
        rh += howl_loop(&d, 3, 0.05, 9, 0, NULL);
        p.band[hb].gain_db = 30.0f;
    }
    /* a longer cut of the first and the last band alone (20 s: long enough
     * for the full give-back if the guard looked at one band only) */
    p.band[0].gain_db = 24.0f;
    oe_dsp_set_params(&d, &p);
    rh += howl_loop(&d, 10, 0.05, 9, 0, NULL);
    p.band[0].gain_db = 30.0f;
    p.band[OE_NUM_BANDS - 1].gain_db = 24.0f;
    oe_dsp_set_params(&d, &p);
    rh += howl_loop(&d, 10, 0.05, 9, 0, NULL);
    p.band[OE_NUM_BANDS - 1].gain_db = 30.0f;
    oe_dsp_set_params(&d, &p);
    float att_one = d.stats.howl_atten_db;
    for (int k = 0; k < OE_NUM_BANDS; k++) p.band[k].gain_db = 24.0f;
    oe_dsp_set_params(&d, &p);
    rh += howl_loop(&d, 20, 0.05, 9, 0, NULL) + (int)(d.stats.howl_events - ev0);
    /* (66 s in all: up to two 1 dB relax steps) */
    CHECK(rh == 0 && att_one >= att0 - 2.0f && d.stats.howl_atten_db <= att0 - 6.0f &&
              d.stats.howl_atten_db >= att0 - 8.0f,
          "one band cut at a time: attenuation kept (%.0f dB); all bands cut 6 dB: %.0f -> %.0f dB; no re-howl (%d)",
          att_one, att0, d.stats.howl_atten_db, rh);

    /* (c4) a flat profile ('reset': all band gains 0 dB) cannot howl at this
     * feedback: the attenuation goes within 30 s, not minutes */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    att0 = d.stats.howl_atten_db;
    ev0 = d.stats.howl_events;
    for (int k = 0; k < OE_NUM_BANDS; k++) p.band[k].gain_db = 0.0f;
    oe_dsp_set_params(&d, &p);
    rh = howl_loop(&d, 30, 0.05, 9, 0, NULL) + (int)(d.stats.howl_events - ev0);
    CHECK(rh == 0 && d.stats.howl_atten_db == 0.0f,
          "after a flat profile the attenuation goes within 30 s (%.0f -> %.0f dB, %d re-howls)", att0,
          d.stats.howl_atten_db, rh);

    /* (c5) a NaN wipes the filter state but not the howl memory */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    ev0 = d.stats.howl_events;
    {
        float nb[BLOCK], ob[BLOCK];
        for (int i = 0; i < BLOCK; i++) nb[i] = (i == 5) ? NAN : 0.0f;
        uint32_t resets0 = d.stats.dsp_resets;
        oe_dsp_process(&d, nb, ob, BLOCK);
        CHECK(d.stats.dsp_resets == resets0 + 1, "a NaN input resets the DSP state");
    }
    p.master_db = 0.0f; /* down and up again: needs the memory */
    oe_dsp_set_params(&d, &p);
    howl_loop(&d, 20, 0.05, 9, 0, NULL);
    p.master_db = 6.0f;
    oe_dsp_set_params(&d, &p);
    rh = howl_loop(&d, 10, 0.05, 9, 0, NULL) + (int)(d.stats.howl_events - ev0);
    CHECK(rh == 0, "after a NaN reset the howl memory is kept (re-howls + new events %d)", rh);

    /* (c6) the 24 dB cap, from an odd starting value: settle, raise the
     * volume to +11.5 dB (held at 17.5 dB), then a feedback path 11 dB
     * stronger in one continuous run, so the howl keeps going and 3 dB
     * steps would pass 24 (round 5: the earlier version restarted the
     * feedback every second and passed only because of that) */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    p.master_db = 11.5f;
    oe_dsp_set_params(&d, &p);
    float att_odd = d.stats.howl_atten_db;
    ev0 = d.stats.howl_events;
    howl_loop(&d, 30, 0.18, 9, 0, NULL);
    CHECK(att_odd == 17.5f && d.stats.howl_events > ev0 && d.stats.howl_atten_db == OE_HOWL_MAX_ATTEN_DB,
          "stronger feedback from %.1f dB: %u more steps, attenuation stops at %.1f dB (cap 24)", att_odd,
          d.stats.howl_events - ev0, d.stats.howl_atten_db);

    /* (c6b) a volume step up is held back in set_params already, so not one
     * block plays louder (round 5: a 12 dB bump for ~2 blocks) */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    att0 = d.stats.howl_atten_db;
    p.master_db = -6.0f;
    oe_dsp_set_params(&d, &p);
    howl_loop(&d, 30, 0.05, 9, 0, NULL); /* the guard gives the cut back */
    float att_dn = d.stats.howl_atten_db;
    p.master_db = 6.0f;
    oe_dsp_set_params(&d, &p);
    CHECK(att_dn <= att0 - 11.0f && d.stats.howl_atten_db >= att_dn + 11.0f,
          "volume up 12 dB with a howl remembered: attenuation %.0f -> %.0f dB before the next block", att_dn,
          d.stats.howl_atten_db);

    /* (c6c) the required attenuation is capped at 24 dB too */
    d.howl_mem = 1;
    d.howl_stable_db = 0.0f;
    p.master_db = 12.0f; /* 12 + 30 - 0 = 42 dB would be needed */
    oe_dsp_set_params(&d, &p);
    float att_req = d.stats.howl_atten_db;
    howl_loop(&d, 1, 0.0, 9, 0, NULL);
    CHECK(att_req == OE_HOWL_MAX_ATTEN_DB && d.stats.howl_atten_db == OE_HOWL_MAX_ATTEN_DB,
          "a required attenuation over 24 dB is held at 24 (%.1f, then %.1f dB)", att_req, d.stats.howl_atten_db);

    /* (c7) the memory is relative to the highest band gain at the time of the
     * howl: settle with bands at 24 dB and volume +12, then all bands to 30:
     * the 6 dB are taken at once, no re-howl */
    howl_params(&p);
    for (int k = 0; k < OE_NUM_BANDS; k++) p.band[k].gain_db = 24.0f;
    p.master_db = 12.0f;
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    att0 = d.stats.howl_atten_db;
    ev0 = d.stats.howl_events;
    for (int k = 0; k < OE_NUM_BANDS; k++) p.band[k].gain_db = 30.0f;
    oe_dsp_set_params(&d, &p);
    rh = howl_loop(&d, 20, 0.05, 9, 0, NULL) + (int)(d.stats.howl_events - ev0);
    CHECK(att0 >= 6 && rh == 0 && d.stats.howl_atten_db == att0 + 6.0f,
          "settled at band gain 24 dB, all bands to 30: attenuation %.0f -> %.0f dB, no re-howl (%d)", att0,
          d.stats.howl_atten_db, rh);

    /* (d) volume 6 dB down and the feedback gone: 6 dB come back at once
     * (1 dB / 2 s), the rest through the slow relax */
    howl_params(&p);
    oe_dsp_init(&d, FS, &p);
    howl_loop(&d, 8, 0.05, 9, 0, NULL);
    att0 = d.stats.howl_atten_db;
    p.master_db = 0.0f;
    oe_dsp_set_params(&d, &p);
    howl_loop(&d, 20, 0.0, 9, 0, NULL);
    float att20 = d.stats.howl_atten_db;
    howl_loop(&d, 400, 0.0, 9, 0, NULL);
    float att7m = d.stats.howl_atten_db;
    /* (e) a big cut with no howl memory left: the guard never boosts */
    p.master_db = -20.0f;
    oe_dsp_set_params(&d, &p);
    howl_loop(&d, 30, 0.0, 9, 0, NULL);
    float att_cut = d.stats.howl_atten_db;
    CHECK(att0 >= 6 && att20 == att0 - 6.0f && att7m == 0.0f,
          "volume 6 dB down, feedback gone: attenuation %.0f -> %.0f dB in 20 s -> %.0f dB after 7 min", att0, att20,
          att7m);
    CHECK(att_cut == 0.0f, "volume cut to -20 dB afterwards: attenuation stays at 0 dB, never a boost (%.1f dB)",
          att_cut);
}

static void test_demo_signal(void)
{
    printf("\n[7] Demo signal for before/after spectrograms\n");
    /* 3 s: soft log chirp 100 Hz-10 kHz + steady fan-like noise, then a
     * loud chirp. Shows high-frequency boost, expansion of quiet noise and
     * compression of the loud part. */
    const int n = (int)(FS * 4) / BLOCK * BLOCK;
    float *in = malloc(sizeof(float) * n), *out = malloc(sizeof(float) * n);
    srand(42);
    double ph = 0, lp = 0;
    for (int i = 0; i < n; i++) {
        double t = i / FS;
        double tt = fmod(t, 2.0);
        double f0 = 100, f1 = 10000, T = 2.0;
        double fr = f0 * pow(f1 / f0, tt / T);
        ph += 2 * PI * fr / FS;
        double amp = (t < 2.0) ? 0.01 : 0.3; /* -40 dBFS then -10 dBFS */
        double w = (double)rand() / RAND_MAX - 0.5;
        lp = 0.97 * lp + 0.03 * w; /* low rumble */
        in[i] = (float)(amp * sin(ph) + 0.004 * lp * 8);
    }
    oe_params_t p;
    oe_dsp_t d;
    const float hl[OE_NUM_BANDS] = { 10, 15, 25, 40, 55, 60 };
    oe_dsp_default_params(&p);
    oe_fit_half_gain(&p, hl);
    oe_dsp_init(&d, FS, &p);
    run(&d, in, out, n);

    FILE *f = csv("demo_signal.csv", "in,out");
    for (int i = 0; i < n; i++) {
        fprintf(f, "%.6f,%.6f\n", in[i], out[i]);
    }
    fclose(f);
    printf("  wrote %d samples\n", n);
    free(in);
    free(out);
}

/* ------------------------------------------------------------------------ */
static void test_hostile_params(void)
{
    printf("\n[8] Safety: hostile or corrupt parameters stay quiet\n");
    oe_params_t p;
    oe_dsp_t d;
    oe_dsp_default_params(&p);
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        p.band[k].gain_db = 30.0f;
    }
    p.master_db = 12.0f;
    p.limit_db = NAN; /* e.g. 'limit nan' or a corrupt saved profile */
    oe_dsp_init(&d, FS, &p);
    double o;
    sine_gain_db(&d, 1000.0, -10.0, &o);
    CHECK(o <= OE_MIN_LIMIT_DB + 0.1, "limit=NaN -> ceiling falls to the quiet end: %.1f dBFS", o);

    oe_dsp_default_params(&p);
    p.tone_on = 1;
    p.tone_hz = 1000.0f;
    p.tone_db = 60.0f;
    oe_dsp_init(&d, FS, &p);
    sine_gain_db(&d, 1000.0, -90.0, &o);
    CHECK(o <= OE_MAX_TONE_DB + 0.1, "tone asked at +60 dBFS -> plays at %.1f dBFS (cap %.0f)", o,
          OE_MAX_TONE_DB);

    oe_dsp_default_params(&p);
    p.master_db = NAN;
    p.band[2].gain_db = INFINITY;
    p.band[3].ratio = NAN;
    oe_dsp_init(&d, FS, &p);
    CHECK(d.p.master_db == OE_MIN_MASTER_DB && d.p.band[2].gain_db == OE_MAX_BAND_GAIN_DB &&
              d.p.band[3].ratio == 1.0f,
          "NaN/inf fields clamp to safe values (master %.0f, gain %.0f, ratio %.0f)",
          d.p.master_db, d.p.band[2].gain_db, d.p.band[3].ratio);
}

static void test_nan_input_recovery(void)
{
    printf("\n[9] A NaN sample silences, resets the DSP, and audio recovers\n");
    oe_params_t p;
    oe_dsp_t d;
    oe_dsp_default_params(&p);
    oe_dsp_init(&d, FS, &p);
    float in[BLOCK], out[BLOCK];
    for (int i = 0; i < BLOCK; i++) in[i] = 0.1f * sinf(0.1f * i);
    in[10] = NAN;
    oe_dsp_process(&d, in, out, BLOCK);
    int finite = 1;
    for (int i = 0; i < BLOCK; i++) finite &= isfinite(out[i]) && fabsf(out[i]) <= 1.0f;
    in[10] = 0.0f;
    double o;
    double g = sine_gain_db(&d, 1000.0, -30.0, &o);
    CHECK(finite, "every output sample finite and in range");
    CHECK(d.stats.dsp_resets == 1, "DSP state reset once (%u)", d.stats.dsp_resets);
    CHECK(fabs(g) < 0.5, "normal audio afterwards (gain %.2f dB)", g);
}

static void test_bypass_ramp(void)
{
    printf("\n[10] Bypass toggle ramps instead of jumping\n");
    oe_params_t p;
    oe_dsp_t d;
    oe_dsp_default_params(&p);
    for (int k = 0; k < OE_NUM_BANDS; k++) p.band[k].gain_db = 20.0f;
    p.limit_db = -3.0f;
    oe_dsp_init(&d, FS, &p);
    const int n = 48000 / BLOCK * BLOCK;
    float *in = malloc(sizeof(float) * n), *out = malloc(sizeof(float) * n);
    for (int i = 0; i < n; i++) in[i] = (float)(0.005 * sin(2 * PI * 1000.0 * i / FS));
    run(&d, in, out, n / 2);
    p.bypass = 1;
    oe_dsp_set_params(&d, &p);
    run(&d, in + n / 2, out + n / 2, n / 2);
    /* largest change in amplitude between consecutive half-cycles (24 samples)
     * around the toggle */
    double worst = 0, prev = 0;
    for (int c = n / 2 - 480; c < n / 2 + 960; c += 24) {
        double pk = 0;
        for (int i = c; i < c + 24; i++) pk = fmax(pk, fabs(out[i]));
        if (prev > 0) worst = fmax(worst, fabs(20 * log10(pk / prev)));
        prev = pk;
    }
    CHECK(worst < 12.0, "largest step between half-cycles %.1f dB (a hard switch = 20 dB)", worst);
    free(in);
    free(out);
}

static void test_tone_clicks(void)
{
    printf("\n[11] Test tone fades in and out (no clicks)\n");
    const double hz[2] = {1000.0, 4000.0};
    for (int h = 0; h < 2; h++) {
        oe_params_t p;
        oe_dsp_t d;
        oe_dsp_default_params(&p);
        oe_dsp_init(&d, FS, &p);
        const int n = (int)FS * 20 / BLOCK * BLOCK;
        float *in = calloc(n, sizeof(float)), *out = malloc(sizeof(float) * n);
        /* bursts of odd lengths, so each starts and stops at a different
         * point of the sine wave, like the app's 1.2 s beeps */
        int on[6], off[6], t = 24000;
        for (int j = 0; j < 6; j++) {
            on[j] = t / BLOCK * BLOCK;
            t += 57600 + 137 * (j * 7 + 3);
            off[j] = t / BLOCK * BLOCK;
            t += 60000;
        }
        p.tone_hz = (float)hz[h];
        p.tone_db = -40.0f;
        for (int b = 0; b < n / BLOCK; b++) {
            for (int j = 0; j < 6; j++) {
                if (b * BLOCK == on[j] || b * BLOCK == off[j]) {
                    p.tone_on = (b * BLOCK == on[j]);
                    oe_dsp_set_params(&d, &p);
                }
            }
            oe_dsp_process(&d, in + b * BLOCK, out + b * BLOCK, BLOCK);
        }
        double amp = pow(10.0, -40.0 / 20.0);
        double normal = amp * 2 * PI * hz[h] / FS; /* largest step inside a clean tone */
        double worst = 0, lvl = 0;
        for (int i = 1; i < n; i++) worst = fmax(worst, fabs(out[i] - out[i - 1]));
        for (int i = on[0] + 4800; i < on[0] + 9600; i++) lvl = fmax(lvl, fabs(out[i]));
        CHECK(worst < 1.2 * normal, "%.0f Hz: biggest sample step %.0f%% of a clean tone's (a click would be far more)",
              hz[h], 100.0 * worst / normal);
        CHECK(fabs(20 * log10(lvl / amp)) < 0.5, "%.0f Hz: tone still plays at the asked level (%.2f dB off)", hz[h],
              20 * log10(lvl / amp));
        /* silent again after the fade-out */
        double tail = 0;
        for (int i = off[0] + 1500; i < off[0] + 3000; i++) tail = fmax(tail, fabs(out[i]));
        CHECK(tail < 1e-6, "%.0f Hz: silent after the tone is switched off", hz[h]);
        free(in);
        free(out);
    }
}

/* ------------------------------------------------------------------------ */
/* 0.3 regression tests                                                     */
/* ------------------------------------------------------------------------ */
_Static_assert(sizeof(oe_params_t) == sizeof(ref_oe_params_t),
               "parameter struct must not change (saved profiles depend on it)");

/* A test signal that exercises everything: speech-like bursts, quiet noise
 * (expander), loud passages (compressor, limiter), and silence. */
static void rich_signal(float *x, int n, unsigned seed)
{
    srand(seed);
    double ph1 = 0, ph2 = 0, lp = 0;
    for (int i = 0; i < n; i++) {
        double t = i / FS;
        double env = 0.5 + 0.5 * sin(2 * PI * 3.0 * t);             /* syllables */
        double sect = fmod(t, 1.5) < 0.5 ? 0.02 : (fmod(t, 1.5) < 1.0 ? 0.3 : 0.001);
        ph1 += 2 * PI * (180 + 60 * sin(2 * PI * 0.7 * t)) / FS;     /* voice-ish pitch */
        ph2 += 2 * PI * 2600 / FS;
        double w = (double)rand() / RAND_MAX - 0.5;
        lp = 0.9 * lp + 0.1 * w;
        x[i] = (float)(sect * env * (0.7 * sin(ph1) + 0.3 * sin(3 * ph1) + 0.2 * sin(ph2)) +
                       0.003 * lp + 0.0005 * w);
    }
}

static void test_equivalence(void)
{
    printf("\n[12] Optimised DSP == the 0.2.2 per-sample DSP source (frozen copy, compiled alike)\n");
    const int sizes[3] = { 64, 128, 37 };
    const int n = (int)(FS * 4.5);
    float *in = malloc(sizeof(float) * n), *a = malloc(sizeof(float) * n),
          *b = malloc(sizeof(float) * n);
    rich_signal(in, n, 7);
    for (int z = 0; z < 3; z++) {
        const int blk = sizes[z];
        oe_params_t p;
        ref_oe_params_t rp;
        static oe_dsp_t d;
        static ref_oe_dsp_t r;
        const float hl[OE_NUM_BANDS] = { 10, 15, 25, 40, 55, 60 };
        oe_dsp_default_params(&p);
        oe_fit_half_gain(&p, hl);
        p.master_db = 6.0f;
        memcpy(&rp, &p, sizeof(p));
        oe_dsp_init(&d, FS, &p);
        ref_oe_dsp_init(&r, FS, &rp);
        double worst = 0;
        long differ = 0;
        int i = 0, step = 0;
        while (i < n) {
            int m = (n - i < blk) ? n - i : blk;
            /* change parameters mid-stream like a user would */
            if (step == 400 || step == 900 || step == 1300) {
                p.bypass = (step == 400);
                p.master_db = (step == 900) ? -6.0f : 3.0f;
                p.band[4].gain_db = (step == 1300) ? 25.0f : p.band[4].gain_db;
                memcpy(&rp, &p, sizeof(p));
                oe_dsp_set_params(&d, &p);
                ref_oe_dsp_set_params(&r, &rp);
            }
            oe_dsp_process(&d, in + i, a + i, m);
            ref_oe_dsp_process(&r, in + i, b + i, m);
            i += m;
            step++;
        }
        for (int k = 0; k < n; k++) {
            double e = fabs((double)a[k] - (double)b[k]);
            if (e > worst) worst = e;
            if (a[k] != b[k]) differ++;
        }
        int stats_same = 1;
        for (int k = 0; k < OE_NUM_BANDS; k++) {
            stats_same &= d.stats.level_db[k] == r.stats.level_db[k] &&
                          d.stats.gain_db[k] == r.stats.gain_db[k];
        }
        stats_same &= d.stats.howl_events == r.stats.howl_events &&
                      d.stats.clip_samples == r.stats.clip_samples;
        CHECK(worst == 0 && differ == 0 && stats_same,
              "block %3d: max difference %.2e over %d samples (%ld not bit-identical), stats %s",
              blk, worst, n, differ, stats_same ? "identical" : "DIFFER");
    }
    free(in);
    free(a);
    free(b);
}

/* tone level (dBFS) in the output after it has settled */
static double tone_out_db(oe_dsp_t *d, int samples)
{
    float *in = calloc(samples, sizeof(float)), *out = malloc(sizeof(float) * samples);
    run(d, in, out, samples);
    double pk = 0;
    for (int i = samples / 2; i < samples; i++) pk = fmax(pk, fabs(out[i]));
    free(in);
    free(out);
    return 20 * log10(pk + 1e-12);
}

static void test_tone_level_safety(void)
{
    printf("\n[13] Test tone level does not depend on the volume (safety cap holds)\n");
    const float masters[3] = { 12.0f, 0.0f, -20.0f };
    for (int m = 0; m < 3; m++) {
        oe_params_t p;
        static oe_dsp_t d;
        static ref_oe_dsp_t r;
        oe_dsp_default_params(&p);
        p.limit_db = -3.0f;
        p.master_db = masters[m];
        p.tone_on = 1;
        p.tone_hz = 1000.0f;
        p.tone_db = -20.0f; /* the cap */
        oe_dsp_init(&d, FS, &p);
        double o = tone_out_db(&d, 9600);
        CHECK(fabs(o - -20.0) < 0.1, "vol %+.0f dB, tone -20 dBFS -> plays at %.2f dBFS", masters[m], o);
        if (m == 0) {
            /* the frozen 0.2.2 code: shows the bug this test guards against */
            ref_oe_params_t rp;
            memcpy(&rp, &p, sizeof(p));
            ref_oe_dsp_init(&r, FS, &rp);
            float *z = calloc(9600, sizeof(float)), *y = malloc(sizeof(float) * 9600);
            for (int i = 0; i < 9600; i += BLOCK) ref_oe_dsp_process(&r, z + i, y + i, BLOCK);
            double pk = 0;
            for (int i = 4800; i < 9600; i++) pk = fmax(pk, fabs(y[i]));
            printf("        (0.2.2 played the same request at %.1f dBFS: over the cap)\n",
                   20 * log10(pk));
            free(z);
            free(y);
        }
    }
    /* howl guard attenuation must not make a hearing-test tone quieter */
    oe_params_t p;
    static oe_dsp_t d;
    oe_dsp_default_params(&p);
    p.tone_on = 1;
    p.tone_db = -40.0f;
    oe_dsp_init(&d, FS, &p);
    d.stats.howl_atten_db = 9.0f;
    oe_dsp_set_params(&d, &p);
    double o = tone_out_db(&d, 9600);
    CHECK(fabs(o - -40.0) < 0.1, "howl guard at -9 dB, tone -40 dBFS -> plays at %.2f dBFS", o);
}

static void test_tone_level_change(void)
{
    printf("\n[14] Tone level changes glide (no clicks), mic stays muted between beeps\n");
    oe_params_t p;
    static oe_dsp_t d;
    oe_dsp_default_params(&p);
    const float hl[OE_NUM_BANDS] = { 10, 15, 25, 40, 55, 60 };
    oe_fit_half_gain(&p, hl);
    oe_dsp_init(&d, FS, &p);
    /* segments of 24128 samples = 502.67 periods of 997 Hz: level changes
     * land at an arbitrary point of the sine wave (with 0.5 s segments any
     * whole-number frequency would sit on a zero crossing and hide a click) */
    const int n = ((int)FS * 3 + 1000) / BLOCK * BLOCK;
    float *in = malloc(sizeof(float) * n), *out = malloc(sizeof(float) * n);
    rich_signal(in, n, 3); /* loud room noise the mic would pick up */
    for (int i = 0; i < n; i++) in[i] *= 3.0f;
    /* the app's hearing test: tone on at -100 (silent), beeps at -30/-50/-25,
     * back to -100 between beeps, never 'tone off' until the end */
    const float lvls[6] = { -100, -30, -100, -50, -100, -25 };
    int seg = n / 6 / BLOCK * BLOCK;
    p.tone_on = 1;
    p.tone_hz = 997.0f;
    for (int s6 = 0; s6 < 6; s6++) {
        p.tone_db = lvls[s6];
        oe_dsp_set_params(&d, &p);
        run(&d, in + s6 * seg, out + s6 * seg, seg);
    }
    double step_max = 0;
    for (int i = seg; i < 6 * seg; i++) step_max = fmax(step_max, fabs(out[i] - out[i - 1]));
    double clean = pow(10, -25 / 20.0) * 2 * PI * 997 / FS; /* largest step in a clean -25 dBFS tone */
    CHECK(step_max < 1.05 * clean, "largest sample step %.0f%% of a clean -25 dBFS tone's", 100 * step_max / clean);
    /* between beeps (tone at -100) the loud mic must be inaudible */
    double pk = 0;
    for (int i = 2 * seg + seg / 2; i < 3 * seg; i++) pk = fmax(pk, fabs(out[i]));
    CHECK(20 * log10(pk + 1e-12) < -95, "between beeps the output is %.1f dBFS (mic muted)", 20 * log10(pk + 1e-12));
    free(in);
    free(out);
}

static void test_odd_and_long_blocks(void)
{
    printf("\n[15] Any block length works (split above %d samples)\n", OE_MAX_BLOCK);
    const int lens[4] = { 1, 255, 256, 1000 };
    for (int z = 0; z < 4; z++) {
        oe_params_t p;
        static oe_dsp_t d;
        oe_dsp_default_params(&p);
        oe_dsp_init(&d, FS, &p);
        int n = lens[z] * 8;
        float *in = malloc(sizeof(float) * n), *out = malloc(sizeof(float) * n);
        for (int i = 0; i < n; i++) in[i] = (float)(0.1 * sin(2 * PI * 440 * i / FS));
        for (int i = 0; i < n; i += lens[z]) oe_dsp_process(&d, in + i, out + i, lens[z]);
        int ok = 1;
        for (int i = 0; i < n; i++) ok &= isfinite(out[i]) && fabsf(out[i]) <= 0.5f;
        CHECK(ok, "block length %d: all %d output samples finite and sane", lens[z], n);
        free(in);
        free(out);
    }
}

/* 10%..90% transition time (ms) of a tone's envelope between two levels,
 * from the peak in each 0.5 ms window */
static double transition_ms(const float *x, int start, int len, double a0, double a1)
{
    const int w = 24;
    double lo = a0 + 0.1 * (a1 - a0), hi = a0 + 0.9 * (a1 - a0);
    int t10 = -1, t90 = -1;
    for (int i = start; i + w <= start + len; i += w) {
        double pk = 0;
        for (int j = i; j < i + w; j++) pk = fmax(pk, fabs(x[j]));
        int past10 = (a1 > a0) ? pk >= lo : pk <= lo;
        int past90 = (a1 > a0) ? pk >= hi : pk <= hi;
        if (t10 < 0 && past10) t10 = i;
        if (t90 < 0 && past90) t90 = i;
    }
    return (t10 < 0 || t90 < 0) ? -1 : (t90 - t10) * 1000.0 / FS;
}

static void test_transition_times(void)
{
    printf("\n[16] Tone fades and level glides take their designed time (no fast edges)\n");
    oe_params_t p;
    static oe_dsp_t d;
    oe_dsp_default_params(&p);
    oe_dsp_init(&d, FS, &p);
    const int seg = 9600; /* 200 ms */
    float *z = calloc(5 * seg, sizeof(float)), *y = malloc(sizeof(float) * 5 * seg);
    p.tone_hz = 1013.0f;
    const float lv[5] = { -40, -40, -20, -40, -40 };
    const int on[5] = { 0, 1, 1, 1, 0 };
    for (int k = 0; k < 5; k++) {
        p.tone_on = on[k];
        p.tone_db = lv[k];
        oe_dsp_set_params(&d, &p);
        run(&d, z + k * seg, y + k * seg, seg);
    }
    double a40 = pow(10, -2.0), a20 = pow(10, -1.0);
    double fin = transition_ms(y, seg, seg, 0, a40), gup = transition_ms(y, 2 * seg, seg, a40, a20),
           gdn = transition_ms(y, 3 * seg, seg, a20, a40), fout = transition_ms(y, 4 * seg, seg, a40, 0);
    /* designed: smoothstep fade over 10 ms -> ~6 ms 10-90%; one-pole 5 ms glide -> ~11 ms */
    CHECK(fin >= 4.0 && fin <= 9.0, "fade-in 10-90%% takes %.1f ms (design ~6)", fin);
    CHECK(fout >= 4.0 && fout <= 9.0, "fade-out 10-90%% takes %.1f ms (design ~6)", fout);
    CHECK(gup >= 7.0 && gup <= 16.0, "level glide up 10-90%% takes %.1f ms (design ~11)", gup);
    CHECK(gdn >= 7.0 && gdn <= 16.0, "level glide down 10-90%% takes %.1f ms (design ~11)", gdn);
    free(z);
    free(y);
}

static void test_long_tone_level(void)
{
    printf("\n[17] A tone keeps its level for 10 minutes (oscillator stays on its circle)\n");
    oe_params_t p;
    static oe_dsp_t d;
    oe_dsp_default_params(&p);
    p.tone_on = 1;
    p.tone_hz = 997.0f;
    p.tone_db = -30.0f;
    oe_dsp_init(&d, FS, &p);
    float z[BLOCK] = { 0 }, y[BLOCK];
    double pk = 0;
    const long blocks = (long)(600 * FS) / BLOCK;
    for (long b = 0; b < blocks; b++) {
        oe_dsp_process(&d, z, y, BLOCK);
        if (b >= blocks - 750) /* last second */
            for (int i = 0; i < BLOCK; i++) pk = fmax(pk, fabs(y[i]));
    }
    double err = 20 * log10(pk) + 30.0;
    CHECK(fabs(err) < 0.05, "after 600 s the tone is %.3f dB off its level", err);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        outdir = argv[1];
    }
    printf("OpenEar DSP test bench  (fs = %.0f Hz, block = %d samples = %.2f ms)\n", FS, BLOCK,
           1000.0 * BLOCK / FS);
#ifdef HOWL_ONLY /* the howl guard at the block size of other firmware builds */
    test_howl_guard();
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
#endif
    test_flat_reconstruction();
    test_band_gains();
    test_io_curve();
    test_limiter_never_exceeds();
    test_latency();
    test_howl_guard();
    test_demo_signal();
    test_hostile_params();
    test_nan_input_recovery();
    test_bypass_ramp();
    test_tone_clicks();
    test_equivalence();
    test_tone_level_safety();
    test_tone_level_change();
    test_odd_and_long_blocks();
    test_transition_times();
    test_long_tone_level();
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
