/*
 * OpenEar console test (runs on a PC).
 * Compiles the firmware's real cli.c + DSP against fake hardware and checks
 * every command: parsing, bad input, safety clamps, and the STATE / LV lines
 * the companion app parses.
 *
 *   make && ./test_cli
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"

static char outbuf[16384];
static int outlen;
static int failures;
static oe_dsp_t dsp;
static int hp_set = -1, sidetone_on = -1, sidetone_g = -1, saved = 0, pushes = 0;
static oe_params_t saved_p;
static uint8_t saved_hp;
static int rec_running;

#define CHECK(cond, ...)                         \
    do {                                         \
        int ok_ = (cond);                        \
        printf(ok_ ? "  PASS  " : "  FAIL  ");   \
        if (!ok_) failures++;                    \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
    } while (0)

/* ---- fake hardware ---- */
static void w(const char *s, int n)
{
    if (outlen + n < (int)sizeof(outbuf) - 1) {
        memcpy(outbuf + outlen, s, n);
        outlen += n;
        outbuf[outlen] = 0;
    }
}
static float last_pushed_master = -99;
static void push(const oe_params_t *p) { oe_dsp_set_params(&dsp, p); pushes++; last_pushed_master = p->master_db; }
static const oe_dsp_t *gd(void) { return &dsp; }
static void ai(cli_audio_info_t *a, int r)
{
    (void)r;
    a->cpu_now_pct = 23.5f;
    a->cpu_peak_pct = 31.0f;
    a->cpu_avg_pct = 24.25f;
    a->blocks = 1000;
    a->tx_blocks = 999;
    a->window_blocks = 750;
    a->late = 3;
    a->lag_max = 2;
    a->slack_min = 17;
    a->restarts = 0;
    a->timer_ok = 1;
    a->dwt_ok = 0;
    a->block_size = 64;
    a->fs = 48000;
}
static int hp(uint8_t c) { hp_set = c; return 0; }
static int st(int on, uint8_t g) { sidetone_on = on; sidetone_g = g; return 0; }
static int raw(int16_t *b, int n) { for (int i = 0; i < n; i++) b[i] = (int16_t)(i * 100); return 0; }
static int batt(int *mv, float *pct) { *mv = 3912; *pct = 76.5f; return 0; }
static int probe(int a) { return a == 0x18 || a == 0x28 || a == 0x36; }
static int save(const oe_params_t *p, uint8_t v) { saved_p = *p; saved_hp = v; saved = 1; return 0; }
static int load(oe_params_t *p, uint8_t *v) { if (!saved) return -1; *p = saved_p; *v = saved_hp; return 0; }
static int rec_fail;
static int rs(int s, char *n, int l) { (void)s; if (rec_fail) return rec_fail; rec_running = 1; snprintf(n, l, "OE000.WAV"); return 0; }
static int rstop(void) { if (!rec_running) return -1; rec_running = 0; return 0; }
static void rst(char *b, int l) { snprintf(b, l, rec_running ? "REC OE000.WAV" : "idle"); }

static int fake_running = 1, restarts_asked;
static int fake_audio_running(void) { return fake_running; }
static int restart_hp = -1; /* the volume the real restart would unmute at */
static float restart_vol = -99; /* the DSP volume pushed before the restart */
static int fake_audio_restart(void)
{
    restarts_asked++;
    restart_hp = cli_hpvol();
    restart_vol = last_pushed_master;
    fake_running = 1;
    return 0;
}

static cli_hw_t hw = { w, push, gd, ai, hp, st, raw, batt, probe, save, load, rs, rstop, rst, NULL, NULL, NULL };

static const char *run(const char *cmd)
{
    char line[256];
    snprintf(line, sizeof(line), "%s", cmd);
    outlen = 0;
    outbuf[0] = 0;
    cli_handle_line(line);
    return outbuf;
}

static int has(const char *s) { return strstr(outbuf, s) != NULL; }

/* parse "key=a,b,c" out of a STATE line */
static int parse_list(const char *line, const char *key, float *v, int n)
{
    char pat[16];
    snprintf(pat, sizeof(pat), " %s=", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    for (int i = 0; i < n; i++) {
        char *end;
        v[i] = strtof(p, &end);
        if (end == p) return 0;
        p = end + (*end == ',');
    }
    return 1;
}

int main(void)
{
    oe_params_t p;
    oe_dsp_default_params(&p);
    oe_dsp_init(&dsp, 48000.0f, &p);
    cli_init(&hw, &p, 16);

    printf("OpenEar console test\n\n[help / info]\n");
    run("help");
    CHECK(has("fit a b c d e f") && has("i2cscan") && has("rec"), "help lists the commands");
    run("ver");
    CHECK(has(OE_FW_VERSION), "ver prints %s", OE_FW_VERSION);
    run("");
    CHECK(outlen == 0, "empty line prints nothing");
    run("bogus 1 2");
    CHECK(has("unknown command"), "unknown command is reported");

    printf("\n[band settings]\n");
    run("g all 12");
    CHECK(has("OK") && dsp.p.band[0].gain_db == 12.0f && dsp.p.band[5].gain_db == 12.0f,
          "g all 12 sets every band");
    run("g 3 20");
    CHECK(dsp.p.band[3].gain_db == 20.0f && dsp.p.band[2].gain_db == 12.0f, "g 3 20 sets band 3 only");
    run("g 6 10");
    CHECK(has("bad arguments"), "band 6 is rejected");
    run("g 2 abc");
    CHECK(has("bad arguments") && dsp.p.band[2].gain_db == 12.0f, "non-number rejected, value kept");
    run("g 2 99");
    CHECK(dsp.p.band[2].gain_db == OE_MAX_BAND_GAIN_DB, "g 2 99 clamps to %.0f dB", dsp.p.band[2].gain_db);
    run("cr 1 2.5");
    CHECK(dsp.p.band[1].ratio == 2.5f, "cr 1 2.5");
    run("knee all -50");
    CHECK(dsp.p.band[4].knee_db == -50.0f, "knee all -50");
    run("exp 0 -65 2");
    CHECK(dsp.p.band[0].exp_thresh_db == -65.0f && dsp.p.band[0].exp_ratio == 2.0f, "exp 0 -65 2");
    run("exp 0 -65");
    CHECK(has("bad arguments"), "exp with a missing ratio is rejected");
    run("att 3");
    run("rel 120");
    CHECK(dsp.p.attack_ms == 3.0f && dsp.p.release_ms == 120.0f, "att 3 / rel 120");

    printf("\n[safety through the console]\n");
    run("limit nan");
    CHECK(has("bad arguments") && dsp.p.limit_db == -6.0f, "limit nan rejected (stays -6)");
    run("vol inf");
    CHECK(has("bad arguments"), "vol inf rejected");
    run("limit 0");
    CHECK(dsp.p.limit_db == OE_MAX_LIMIT_DB, "limit 0 clamps to %.0f dBFS", dsp.p.limit_db);
    run("tone 1000 0");
    CHECK(dsp.p.tone_on && dsp.p.tone_db <= OE_MAX_TONE_DB, "tone 1000 0 plays at %.0f dBFS", dsp.p.tone_db);
    run("tone off");
    CHECK(!dsp.p.tone_on, "tone off");
    run("tone 1000");
    CHECK(has("bad arguments"), "tone without a level is rejected");
    run("hpvol 0");
    CHECK(hp_set == 4, "hpvol 0 clamps to the loudest allowed code (4), got %d", hp_set);
    run("hpvol 99");
    CHECK(hp_set == 63, "hpvol 99 clamps to 63");
    run("sidetone 1 2");
    CHECK(has("bad arguments") && sidetone_on == -1, "sidetone gain 2 (too loud) rejected");
    run("sidetone 1 20");
    CHECK(sidetone_on == 1 && sidetone_g == 20, "sidetone 1 20 accepted");

    printf("\n[bluetooth limits]\n");
    cli_set_remote(1);
    run("hpvol 4");
    CHECK(hp_set == CLI_REMOTE_HPVOL_MIN, "over BLE, hpvol 4 is held at %d", hp_set);
    run("vol 12");
    CHECK(dsp.p.master_db == CLI_REMOTE_VOL_MAX && has("limited"), "over BLE, vol 12 is held at +%.0f dB", dsp.p.master_db);
    cli_set_remote(0);
    run("hpvol 4");
    CHECK(hp_set == 4, "over USB, hpvol 4 is allowed");
    run("vol 0");

    printf("\n[fitting, bypass, profile]\n");
    run("reset");
    run("fit 10 15 25 40 55 60");
    CHECK(has("fitted") && dsp.p.band[5].gain_db > dsp.p.band[1].gain_db, "fit raises high bands more");
    run("fit 10 15 25");
    CHECK(has("bad arguments"), "fit with 3 values rejected");
    run("bypass");
    CHECK(dsp.p.bypass == 1 && has("BYPASS"), "bypass toggles on");
    run("bypass 0");
    CHECK(dsp.p.bypass == 0, "bypass 0");
    run("save");
    CHECK(saved && has("ok"), "save stores the profile");
    run("g all 0");
    run("load");
    CHECK(has("profile loaded") && dsp.p.band[5].gain_db == saved_p.band[5].gain_db, "load restores it");

    printf("\n[a stored profile can't get around the Bluetooth limits]\n");
    saved_p.master_db = 9.0f;
    saved_hp = 4;
    cli_set_remote(1);
    run("load");
    CHECK(hp_set == CLI_REMOTE_HPVOL_MIN && dsp.p.master_db == CLI_REMOTE_VOL_MAX,
          "over Bluetooth, load of a loud profile is held to hpvol %d / +%.0f dB", hp_set,
          (double)dsp.p.master_db);
    cli_set_remote(0);
    run("load");
    CHECK(hp_set == 4 && dsp.p.master_db == 9.0f, "over USB the stored values load as saved");
    run("tone 1000 -40");
    run("save");
    CHECK(!saved_p.tone_on, "a playing test tone is not written to the profile");
    saved_p.tone_on = 1;
    run("load");
    CHECK(!dsp.p.tone_on, "a stored tone is not restored by load");
    run("reset");

    printf("\n[app protocol]\n");
    run("state");
    float g[6], cr[6];
    CHECK(!strncmp(outbuf, "STATE ", 6) && parse_list(outbuf, "g", g, 6) && parse_list(outbuf, "cr", cr, 6),
          "STATE line parses: %s", outbuf);
    CHECK(fabsf(g[5] - dsp.p.band[5].gain_db) < 0.06f, "STATE gains match the DSP");
    CHECK(strchr(outbuf, '\n') == outbuf + outlen - 1, "STATE is exactly one line");
    run("levels");
    float lv[6];
    CHECK(!strncmp(outbuf, "LV ", 3) && parse_list(outbuf, "lv", lv, 6) && has(" cpu="),
          "LV line parses: %s", outbuf);
    run("batt");
    CHECK(has("BATT mv=3912 pct=76.5"), "batt prints a parseable BATT line");
    run("i2cscan");
    CHECK(has("0x18") && has("0x28") && has("0x36"), "i2cscan lists the fake devices");

    printf("\n[recorder]\n");
    run("rec 5");
    CHECK(rec_running && has("OE000.WAV"), "rec 5 starts");
    run("rec ?");
    CHECK(has("REC"), "rec ? shows status");
    run("rec stop");
    CHECK(!rec_running && has("stopped"), "rec stop");
    run("rec 99999");
    CHECK(has("bad arguments"), "rec 99999 s rejected");
    rec_fail = -20;
    run("rec 5");
    CHECK(has("audio is not running"), "rec refused when audio is not running");
    rec_fail = 0;
    hw.rec_start = NULL;
    run("rec 5");
    CHECK(has("not built"), "rec without a recorder says so");

    printf("\n[stat / show / raw]\n");
    run("stat");
    CHECK(has("DSP time avg 24.2%") && has("peak 31.0%") && has("late blocks 3 of 750"),
          "stat shows CPU (avg, peak) and late blocks with their window");
    CHECK(has("slack >= 17 samples (0.35 ms)") && has("interrupt delay <= 2 samples") &&
              has("played 999"),
          "stat shows the deadline slack, interrupt delay and played blocks");
    CHECK(has("timer ok") && has("DWT not counting"), "stat shows the clock status");
    CHECK(has("est. latency: 2.5 ms") && has("output phase fixes"),
          "stat latency matches the 0.3 timing (2N-6 samples) and shows the phase fixes");
    run("show");
    CHECK(has("band") && has("master"), "show prints the band table");
    run("raw");
    CHECK(has("peak 1500"), "raw prints samples and peak");

    printf("\n[audio status / restart, hpvol while audio is off]\n");
    hw.audio_running = fake_audio_running;
    hw.audio_restart = fake_audio_restart;
    run("audio");
    CHECK(has("audio running"), "audio shows running");
    fake_running = 0;
    run("audio");
    CHECK(has("NOT running"), "audio shows not running");
    run("hpvol 20");
    CHECK(hp_set == 20 && has("stored") && has("stays muted"), "hpvol while audio is off says it is only stored");
    run("audio restart");
    CHECK(restarts_asked == 1 && fake_running && has("audio restart: ok"), "audio restart restarts the streams");
    /* round-3 review: over Bluetooth the restart must not unmute at a
     * console-only volume (hpvol 4 is louder than the Bluetooth limit) */
    run("hpvol 4");
    run("vol 12");
    fake_running = 0;
    cli_set_remote(1);
    run("audio restart");
    cli_set_remote(0);
    CHECK(restarts_asked == 2 && restart_hp == CLI_REMOTE_HPVOL_MIN && hp_set == CLI_REMOTE_HPVOL_MIN,
          "audio restart over Bluetooth unmutes within the Bluetooth limit (hpvol %d, codec set %d)", restart_hp,
          hp_set);
    CHECK(restart_vol == CLI_REMOTE_VOL_MAX && cli_params()->master_db == CLI_REMOTE_VOL_MAX,
          "and at no more than +%.0f dB volume (DSP had %.1f dB at the restart)", (double)CLI_REMOTE_VOL_MAX,
          restart_vol);
    /* only the volume over the limit (hpvol already quiet enough) */
    run("hpvol 20");
    run("vol 12");
    fake_running = 0;
    cli_set_remote(1);
    run("audio restart");
    cli_set_remote(0);
    CHECK(restart_hp == 20 && restart_vol == CLI_REMOTE_VOL_MAX,
          "audio restart over Bluetooth with only vol too loud: held to +%.0f dB, hpvol kept at %d",
          (double)restart_vol, restart_hp);
    run("hpvol 4");
    fake_running = 0;
    run("audio restart");
    CHECK(restart_hp == 4, "audio restart on the console keeps the console volume (hpvol %d)", restart_hp);
    fake_running = 1;
    run("audio bogus");
    CHECK(has("bad arguments"), "audio with a bad argument is rejected");

    printf("\n[test-tone auto-stop (cli_tick)]\n");
    run("tone 1000 -40");
    cli_tick(1000);
    cli_tick(15900);
    CHECK(cli_params()->tone_on == 1, "an audible tone still plays after 14.9 s");
    outlen = 0;
    cli_tick(16100);
    CHECK(cli_params()->tone_on == 0 && dsp.p.tone_on == 0 && has("stopped automatically after 15 s"),
          "an audible tone stops by itself after 15 s, and the DSP is told");
    run("tone 1000 -40");
    cli_tick(20000);
    cli_tick(30000);
    run("tone 2000 -40"); /* a new beep restarts the clock */
    cli_tick(31000);
    cli_tick(45000);
    CHECK(cli_params()->tone_on == 1, "a changed tone restarts the 15 s clock");
    cli_tick(46100);
    CHECK(cli_params()->tone_on == 0, "...and then stops 15 s after the change");
    run("tone 1000 -100"); /* the hearing test's silent hold */
    cli_tick(50000);
    cli_tick(100000);
    CHECK(cli_params()->tone_on == 1, "the silent hold (-100 dBFS) is not stopped at 15 s (mic stays muted between beeps)");
    outlen = 0;
    cli_tick(110100);
    CHECK(cli_params()->tone_on == 0 && has("after 60 s"), "the silent hold stops after 60 s, so the mic comes back");
    run("tone off");
    cli_tick(200000);
    CHECK(cli_params()->tone_on == 0, "tone off stays off");

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL CONSOLE TESTS PASSED", failures);
    return failures != 0;
}
