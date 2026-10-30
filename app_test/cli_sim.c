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
static oe_dsp_t dsp;
static int hp_set = -1, sidetone_on = -1, sidetone_g = -1, saved = 0, pushes = 0;
static oe_params_t saved_p;
static uint8_t saved_hp;
static int rec_running;


/* ---- fake hardware ---- */
static void w(const char *s, int n)
{
    if (outlen + n < (int)sizeof(outbuf) - 1) {
        memcpy(outbuf + outlen, s, n);
        outlen += n;
        outbuf[outlen] = 0;
    }
}
static void push(const oe_params_t *p) { oe_dsp_set_params(&dsp, p); pushes++; }
static const oe_dsp_t *gd(void) { return &dsp; }
static void ai(cli_audio_info_t *a, int r)
{
    (void)r;
    a->cpu_now_pct = 23.5f;
    a->cpu_peak_pct = 31.0f;
    a->cpu_avg_pct = 24.25f;
    a->blocks = 1000;
    a->tx_blocks = 1000;
    a->window_blocks = 750;
    a->late = 0;
    a->lag_max = 1;
    a->slack_min = 30;
    a->timer_ok = 1;
    a->dwt_ok = 0;
    a->restarts = 0;
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
static int rs(int s, char *n, int l) { (void)s; rec_running = 1; snprintf(n, l, "OE000.WAV"); return 0; }
static int rstop(void) { if (!rec_running) return -1; rec_running = 0; return 0; }
static void rst(char *b, int l) { snprintf(b, l, rec_running ? "REC OE000.WAV" : "idle"); }

static cli_hw_t hw = { w, push, gd, ai, hp, st, raw, batt, probe, save, load, rs, rstop, rst, NULL, NULL, NULL };


int main(void)
{
    oe_params_t p;
    oe_dsp_default_params(&p);
    oe_dsp_init(&dsp, 48000.0f, &p);
    cli_init(&hw, &p, 16);
    cli_set_remote(1); /* the phone is a remote client, like real BLE */
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        outlen = 0; outbuf[0] = 0;
        cli_handle_line(line);
        fwrite(outbuf, 1, outlen, stdout);
        fputs("\x01\n", stdout);
        fflush(stdout);
    }
    return 0;
}
