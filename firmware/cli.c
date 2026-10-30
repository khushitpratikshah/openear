/* OpenEar command console - see cli.h. Licence: MIT. */
#include "cli.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const cli_hw_t *hw;
static oe_params_t ui;
static uint8_t hpvol;
static int remote;

void cli_set_remote(int r)
{
    remote = r;
}

#define HPVOL_MIN 4 /* loudest the console allows (ADI example's value) */
#define HPVOL_MAX 63

static const char *band_names[OE_NUM_BANDS] = { " <250", "250-500", "500-1k",
                                                " 1k-2k", " 2k-4k", "  >4k" };

/* ------------------------------------------------------------------------ */
void cli_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    if (hw && hw->write) {
        hw->write(buf, n);
    }
}

void cli_init(const cli_hw_t *h, const oe_params_t *initial, uint8_t vol)
{
    hw = h;
    ui = *initial;
    hpvol = vol;
}

oe_params_t *cli_params(void)
{
    return &ui;
}

void cli_params_changed(void)
{
    hw->push_params(&ui);
}

uint8_t cli_hpvol(void)
{
    return hpvol;
}

/* ------------------------------------------------------------------------ */
/* number parsing that rejects junk (strtof would silently give 0)          */
/* ------------------------------------------------------------------------ */
static int parse_f(const char *s, float *out)
{
    char *end;
    if (!s || !*s) {
        return 0;
    }
    float v = strtof(s, &end);
    if (*end != '\0' || v != v /* NaN */ || isinf(v)) {
        return 0;
    }
    *out = v;
    return 1;
}

static int parse_i(const char *s, int *out)
{
    char *end;
    if (!s || !*s) {
        return 0;
    }
    long v = strtol(s, &end, 0);
    if (*end != '\0') {
        return 0;
    }
    *out = (int)v;
    return 1;
}

/* band argument: "0".."5" -> mask bit, "all" -> all bands */
static int parse_bands(const char *s, unsigned *mask)
{
    int k;
    if (!s) {
        return 0;
    }
    if (!strcmp(s, "all")) {
        *mask = (1u << OE_NUM_BANDS) - 1;
        return 1;
    }
    if (parse_i(s, &k) && k >= 0 && k < OE_NUM_BANDS) {
        *mask = 1u << k;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
static void print_help(void)
{
    cli_printf("\nOpenEar " OE_FW_VERSION " commands  (band = 0..5 or 'all')\n");
    cli_printf("  show | stat | state | levels | ver\n");
    cli_printf("  g <band> <dB>           band gain        (-30..30)\n");
    cli_printf("  cr <band> <ratio>       compression ratio (1..10)\n");
    cli_printf("  knee <band> <dBFS>      compression knee\n");
    cli_printf("  exp <band> <dBFS> <r>   expansion threshold + ratio (r=1 off)\n");
    cli_printf("  att <ms> | rel <ms>     envelope attack / release\n");
    cli_printf("  vol <dB>                master volume    (-60..12)\n");
    cli_printf("  limit <dBFS>            output ceiling   (-40..-3)\n");
    cli_printf("  fit a b c d e f         hearing levels dB HL at 250 500 1k 2k 4k 8k\n");
    cli_printf("  bypass [0|1]            flat A/B compare (also button SW2)\n");
    cli_printf("  tone <Hz> <dBFS> | tone off   test tone (capped at -20 dBFS, stops by itself after 15 s)\n");
    cli_printf("  hpvol <4..63>           codec headphone volume (higher = quieter)\n");
    cli_printf("  audio | audio restart   audio stream status / start it again\n");
    cli_printf("  sidetone <0|1> [16..31] codec mic->ear loop, no MCU (hardware check)\n");
    cli_printf("  raw                     raw mic samples\n");
    cli_printf("  batt                    battery voltage / charge\n");
    cli_printf("  i2cscan                 list chips on the codec/PMIC I2C bus\n");
    cli_printf("  rec [seconds] | rec stop | rec ?   record mic+output WAV to microSD\n");
    cli_printf("  save | load | reset     profile in flash\n");
    cli_printf("Bands: 0:<250 1:250-500 2:500-1k 3:1k-2k 4:2k-4k 5:>4k Hz\n");
}

static void print_show(void)
{
    const oe_dsp_t *d = hw->dsp();
    cli_printf("\nband      gain  ratio   knee  expThr expR | level  applied\n");
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        const oe_band_params_t *b = &d->p.band[k];
        cli_printf("%d %-7s %5.1f  %5.2f  %5.0f  %5.0f  %4.1f | %6.1f  %6.1f\n", k,
                   band_names[k], (double)b->gain_db, (double)b->ratio, (double)b->knee_db,
                   (double)b->exp_thresh_db, (double)b->exp_ratio,
                   (double)d->stats.level_db[k], (double)d->stats.gain_db[k]);
    }
    cli_printf("master %.1f dB  limit %.1f dBFS  attack %.1f ms  release %.0f ms\n",
               (double)d->p.master_db, (double)d->p.limit_db, (double)d->p.attack_ms,
               (double)d->p.release_ms);
    cli_printf("bypass %d  tone %s", d->p.bypass, d->p.tone_on ? "on" : "off");
    if (d->p.tone_on) {
        cli_printf(" (%.0f Hz, %.0f dBFS)", (double)d->p.tone_hz, (double)d->p.tone_db);
    }
    cli_printf("  hpvol %d\n", hpvol);
}

static void print_stat(void)
{
    cli_audio_info_t a;
    const oe_dsp_t *d = hw->dsp();
    hw->audio_info(&a, 1);
    double blk_ms = 1000.0 * a.block_size / a.fs;
    cli_printf("\nblocks %lu (played %lu)  block %d samples (%.2f ms) @ %d Hz\n",
               (unsigned long)a.blocks, (unsigned long)a.tx_blocks, a.block_size, blk_ms, a.fs);
    cli_printf("DSP time avg %.1f%%  peak %.1f%% of real time  late blocks %lu of %lu  (since last 'stat')\n",
               (double)a.cpu_avg_pct, (double)a.cpu_peak_pct, (unsigned long)a.late,
               (unsigned long)a.window_blocks);
    cli_printf("deadline slack >= %d samples (%.2f ms)  interrupt delay <= %d samples  audio restarts %lu\n",
               a.slack_min, a.slack_min * 1000.0 / a.fs, a.lag_max, (unsigned long)a.restarts);
    cli_printf("output phase fixes %lu  (2-sample steps keeping output in step with input)\n",
               a.phase_fixes);
    cli_printf("clock: timer %s, DWT %s\n", a.timer_ok ? "ok" : "NOT RUNNING",
               a.dwt_ok ? "counting" : "not counting");
    cli_printf("limiter GR %.1f dB  howl events %lu  howl atten %.0f dB  clipped %lu  resets %lu\n",
               (double)d->stats.limiter_gr_db, (unsigned long)d->stats.howl_events,
               (double)d->stats.howl_atten_db, (unsigned long)d->stats.clip_samples,
               (unsigned long)d->stats.dsp_resets);
    /* input block + output start 6 samples before the next block ends */
    cli_printf("est. latency: %.1f ms buffering + ~1 ms filters + codec\n",
               (2.0 * a.block_size - 6.0) * 1000.0 / a.fs);
    if (hw->rec_status) {
        char buf[96];
        hw->rec_status(buf, sizeof(buf));
        cli_printf("recorder: %s\n", buf);
    }
    if (hw->ble_status) {
        char buf[96];
        hw->ble_status(buf, sizeof(buf));
        cli_printf("bluetooth: %s\n", buf);
    }
}

/* One-line, machine-readable state for the companion app. */
static void print_state(void)
{
    const oe_dsp_t *d = hw->dsp();
    const oe_params_t *p = &d->p;
    cli_printf("STATE v=" OE_FW_VERSION " g=");
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        cli_printf("%s%.1f", k ? "," : "", (double)p->band[k].gain_db);
    }
    cli_printf(" cr=");
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        cli_printf("%s%.2f", k ? "," : "", (double)p->band[k].ratio);
    }
    cli_printf(" knee=");
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        cli_printf("%s%.0f", k ? "," : "", (double)p->band[k].knee_db);
    }
    cli_printf(" et=");
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        cli_printf("%s%.0f", k ? "," : "", (double)p->band[k].exp_thresh_db);
    }
    cli_printf(" er=");
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        cli_printf("%s%.1f", k ? "," : "", (double)p->band[k].exp_ratio);
    }
    cli_printf(" vol=%.1f lim=%.1f byp=%d tone=%d hp=%d\n", (double)p->master_db,
               (double)p->limit_db, p->bypass, p->tone_on, hpvol);
}

/* One-line live levels for the companion app. */
static void print_levels(void)
{
    const oe_dsp_t *d = hw->dsp();
    cli_audio_info_t a;
    hw->audio_info(&a, 0);
    cli_printf("LV lv=");
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        cli_printf("%s%.1f", k ? "," : "", (double)d->stats.level_db[k]);
    }
    cli_printf(" ga=");
    for (int k = 0; k < OE_NUM_BANDS; k++) {
        cli_printf("%s%.1f", k ? "," : "", (double)d->stats.gain_db[k]);
    }
    cli_printf(" gr=%.1f howl=%.0f cpu=%.1f\n", (double)d->stats.limiter_gr_db,
               (double)d->stats.howl_atten_db, (double)a.cpu_now_pct);
}

/* ------------------------------------------------------------------------ */
void cli_tick(uint32_t now)
{
    static int armed;
    static uint32_t since;
    static float last_hz, last_db;

    if (!ui.tone_on) {
        armed = 0;
        return;
    }
    if (!armed || ui.tone_hz != last_hz || ui.tone_db != last_db) {
        armed = 1; /* a new or changed tone restarts the clock */
        since = now;
        last_hz = ui.tone_hz;
        last_db = ui.tone_db;
        return;
    }
    uint32_t limit = (ui.tone_db <= CLI_TONE_HOLD_DB) ? CLI_TONE_HOLD_TIMEOUT_MS : CLI_TONE_TIMEOUT_MS;
    if (now - since > limit) {
        ui.tone_on = 0;
        armed = 0;
        hw->push_params(&ui);
        cli_printf("\n[tone] stopped automatically after %lu s\n> ", (unsigned long)(limit / 1000));
    }
}

/* ------------------------------------------------------------------------ */
void cli_handle_line(char *line)
{
    char *argv[10];
    int argc = 0;
    unsigned mask = 0;
    float f1 = 0, f2 = 0;
    int changed = 0, bad = 0;

    for (char *t = strtok(line, " \t\r\n"); t && argc < 10; t = strtok(NULL, " \t\r\n")) {
        argv[argc++] = t;
    }
    if (!argc) {
        return;
    }
    const char *cmd = argv[0];

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        print_help();
    } else if (!strcmp(cmd, "ver")) {
        cli_printf("OpenEar " OE_FW_VERSION "\n");
    } else if (!strcmp(cmd, "show")) {
        print_show();
    } else if (!strcmp(cmd, "stat")) {
        print_stat();
    } else if (!strcmp(cmd, "state")) {
        print_state();
    } else if (!strcmp(cmd, "levels")) {
        print_levels();
    } else if (!strcmp(cmd, "g") || !strcmp(cmd, "cr") || !strcmp(cmd, "knee")) {
        bad = argc != 3 || !parse_bands(argv[1], &mask) || !parse_f(argv[2], &f1);
        for (int k = 0; !bad && k < OE_NUM_BANDS; k++) {
            if (mask & (1u << k)) {
                if (cmd[0] == 'g') {
                    ui.band[k].gain_db = f1;
                } else if (cmd[0] == 'c') {
                    ui.band[k].ratio = f1;
                } else {
                    ui.band[k].knee_db = f1;
                }
            }
        }
        changed = !bad;
    } else if (!strcmp(cmd, "exp")) {
        bad = argc != 4 || !parse_bands(argv[1], &mask) || !parse_f(argv[2], &f1) ||
              !parse_f(argv[3], &f2);
        for (int k = 0; !bad && k < OE_NUM_BANDS; k++) {
            if (mask & (1u << k)) {
                ui.band[k].exp_thresh_db = f1;
                ui.band[k].exp_ratio = f2;
            }
        }
        changed = !bad;
    } else if (!strcmp(cmd, "att") || !strcmp(cmd, "rel")) {
        bad = argc != 2 || !parse_f(argv[1], &f1);
        if (!bad) {
            if (cmd[0] == 'a') {
                ui.attack_ms = f1;
            } else {
                ui.release_ms = f1;
            }
        }
        changed = !bad;
    } else if (!strcmp(cmd, "vol")) {
        bad = argc != 2 || !parse_f(argv[1], &f1);
        if (!bad) {
            if (remote && f1 > CLI_REMOTE_VOL_MAX) {
                f1 = CLI_REMOTE_VOL_MAX;
                cli_printf("(over Bluetooth, volume is limited to +%.0f dB)\n", (double)f1);
            }
            ui.master_db = f1;
        }
        changed = !bad;
    } else if (!strcmp(cmd, "limit")) {
        bad = argc != 2 || !parse_f(argv[1], &ui.limit_db);
        changed = !bad;
    } else if (!strcmp(cmd, "fit")) {
        float hl[OE_NUM_BANDS];
        bad = argc != 1 + OE_NUM_BANDS;
        for (int k = 0; !bad && k < OE_NUM_BANDS; k++) {
            bad = !parse_f(argv[k + 1], &hl[k]);
        }
        if (!bad) {
            oe_fit_half_gain(&ui, hl);
            changed = 1;
            cli_printf("fitted (half-gain rule). 'show' to inspect, 'save' to keep.\n");
        }
    } else if (!strcmp(cmd, "bypass")) {
        int v = !ui.bypass;
        bad = argc > 2 || (argc == 2 && !parse_i(argv[1], &v));
        if (!bad) {
            ui.bypass = v ? 1 : 0;
            cli_printf("%s\n", ui.bypass ? "BYPASS (flat)" : "OPENEAR ON");
        }
        changed = !bad;
    } else if (!strcmp(cmd, "tone")) {
        if (argc == 2 && !strcmp(argv[1], "off")) {
            ui.tone_on = 0;
        } else if (argc == 3 && parse_f(argv[1], &f1) && parse_f(argv[2], &f2)) {
            ui.tone_on = 1;
            ui.tone_hz = f1;
            ui.tone_db = f2;
        } else {
            bad = 1;
        }
        changed = !bad;
    } else if (!strcmp(cmd, "hpvol")) {
        int v;
        bad = argc != 2 || !parse_i(argv[1], &v);
        if (!bad) {
            int lo = remote ? CLI_REMOTE_HPVOL_MIN : HPVOL_MIN;
            hpvol = (uint8_t)(v < lo ? lo : (v > HPVOL_MAX ? HPVOL_MAX : v));
            int e = hw->set_hpvol(hpvol);
            if (e) {
                cli_printf("hpvol %d: I2C error\n", hpvol);
            } else if (hw->audio_running && !hw->audio_running()) {
                cli_printf("hpvol %d: stored; audio is not running, so the codec stays muted "
                           "('audio restart')\n", hpvol);
            } else {
                cli_printf("hpvol %d: ok\n", hpvol);
            }
        }
    } else if (!strcmp(cmd, "audio")) {
        int running = hw->audio_running ? hw->audio_running() : 1;
        if (argc == 2 && !strcmp(argv[1], "restart")) {
            if (!hw->audio_restart) {
                cli_printf("audio restart not available\n");
            } else {
                /* a restart unmutes at the stored loudness: over Bluetooth it
                 * must be within the Bluetooth limits, as 'load' is */
                if (remote && (hpvol < CLI_REMOTE_HPVOL_MIN || ui.master_db > CLI_REMOTE_VOL_MAX)) {
                    if (hpvol < CLI_REMOTE_HPVOL_MIN) {
                        hpvol = CLI_REMOTE_HPVOL_MIN;
                        hw->set_hpvol(hpvol);
                    }
                    if (ui.master_db > CLI_REMOTE_VOL_MAX) {
                        ui.master_db = CLI_REMOTE_VOL_MAX;
                        hw->push_params(&ui); /* in the DSP before the restart unmutes */
                    }
                    cli_printf("(over Bluetooth, loudness is held to the Bluetooth limits)\n");
                }
                int e = hw->audio_restart();
                cli_printf("audio restart: %s (%d)\n", e ? "FAILED" : "ok", e);
            }
        } else if (argc == 1) {
            cli_printf("audio %s\n", running ? "running" : "NOT running (codec muted; try 'audio restart')");
        } else {
            bad = 1;
        }
    } else if (!strcmp(cmd, "sidetone")) {
        int on = 0, g = 16;
        bad = argc < 2 || argc > 3 || !parse_i(argv[1], &on) ||
              (argc == 3 && !parse_i(argv[2], &g)) || g < 16 || g > 31;
        if (!bad) {
            cli_printf("sidetone %s: %s\n", on ? "on" : "off",
                       hw->sidetone(on, (uint8_t)g) ? "I2C error" : "ok");
        }
    } else if (!strcmp(cmd, "raw")) {
        int16_t r[16];
        int32_t peak = 0;
        hw->raw(r, 16);
        for (int i = 0; i < 16; i++) {
            cli_printf("%d ", r[i]);
            if (abs(r[i]) > peak) {
                peak = abs(r[i]);
            }
        }
        cli_printf("\npeak %ld (%.1f dBFS)\n", (long)peak,
                   20.0 * log10(peak / 32768.0 + 1e-9));
    } else if (!strcmp(cmd, "batt")) {
        int mv;
        float pct;
        int err = hw->batt(&mv, &pct);
        if (err) {
            cli_printf("fuel gauge read failed (%d)\n", err);
        } else {
            cli_printf("BATT mv=%d pct=%.1f\n", mv, (double)pct);
        }
    } else if (!strcmp(cmd, "i2cscan")) {
        int found = 0;
        cli_printf("I2C1 devices:");
        for (int a = 0x08; a < 0x78; a++) {
            if (hw->i2c_probe(a)) {
                cli_printf(" 0x%02X", a);
                found++;
            }
        }
        cli_printf("%s\n(codec MAX9867 = 0x18; the rest are the PMIC / fuel gauge)\n",
                   found ? "" : " none");
    } else if (!strcmp(cmd, "rec")) {
        if (!hw->rec_start) {
            cli_printf("recorder not built into this firmware\n");
        } else if (argc == 2 && !strcmp(argv[1], "stop")) {
            int err = hw->rec_stop();
            cli_printf(err ? "rec stop: not recording\n" : "rec stopped\n");
        } else if (argc == 2 && !strcmp(argv[1], "?")) {
            char buf[96];
            hw->rec_status(buf, sizeof(buf));
            cli_printf("%s\n", buf);
        } else {
            int secs = 10;
            bad = argc > 2 || (argc == 2 && (!parse_i(argv[1], &secs) || secs < 1 || secs > 3600));
            if (!bad) {
                char name[24];
                int err = hw->rec_start(secs, name, sizeof(name));
                if (err) {
                    if (err == -20) {
                        cli_printf("rec failed: audio is not running\n");
                    } else {
                        cli_printf("rec failed (%d): card inserted? FAT32?\n", err);
                    }
                } else {
                    cli_printf("recording %d s to %s (L = mic, R = OpenEar output)\n", secs,
                               name);
                }
            }
        }
    } else if (!strcmp(cmd, "save")) {
        cli_printf("saving (audio mutes for a moment)... ");
        oe_params_t to_save = ui;
        to_save.tone_on = 0; /* a test tone is never stored */
        cli_printf("%s\n", hw->save(&to_save, hpvol) ? "FAILED" : "ok");
    } else if (!strcmp(cmd, "load")) {
        oe_params_t p;
        uint8_t v;
        if (hw->load(&p, &v) == 0) {
            ui = p;
            hpvol = v;
            ui.tone_on = 0;
            if (remote) {
                /* nobody has to pair to send this, so a stored loud profile
                 * must not bypass the Bluetooth loudness limits */
                if (hpvol < CLI_REMOTE_HPVOL_MIN) {
                    hpvol = CLI_REMOTE_HPVOL_MIN;
                }
                if (ui.master_db > CLI_REMOTE_VOL_MAX) {
                    ui.master_db = CLI_REMOTE_VOL_MAX;
                }
                cli_printf("(over Bluetooth, loudness is held to the Bluetooth limits)\n");
            }
            hw->set_hpvol(hpvol);
            changed = 1;
            cli_printf("profile loaded\n");
        } else {
            cli_printf("no valid profile\n");
        }
    } else if (!strcmp(cmd, "reset")) {
        oe_dsp_default_params(&ui);
        changed = 1;
        cli_printf("defaults restored (flash unchanged; 'save' to overwrite)\n");
    } else {
        cli_printf("? unknown command '%s' - type help\n", cmd);
        return;
    }

    if (bad) {
        cli_printf("? bad arguments for '%s' - type help\n", cmd);
        return;
    }
    if (changed) {
        hw->push_params(&ui);
        cli_printf("OK\n");
    }
}
