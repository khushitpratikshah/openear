/*
 * OpenEar cycle bench harness (runs inside an instruction-set emulator, not on
 * the board). It runs one audio block exactly the way the firmware's audio
 * interrupt does: int16 -> float, oe_dsp_process(), float -> int16; and once
 * more with the SD recorder's rec_feed_isr() (real recorder.c, FatFS stubbed).
 * cycle_bench.py counts the Cortex-M4 instructions executed between the
 * marker writes.
 */
#include <stdint.h>
#ifdef USE_REF
/* the frozen 0.2.2 DSP, for a before/after number from the same harness */
#include "ref_dsp.h"
#define oe_dsp_t ref_oe_dsp_t
#define oe_params_t ref_oe_params_t
#define oe_dsp_default_params ref_oe_dsp_default_params
#define oe_fit_half_gain ref_oe_fit_half_gain
#define oe_dsp_init ref_oe_dsp_init
#define oe_dsp_set_params ref_oe_dsp_set_params
#define oe_dsp_process ref_oe_dsp_process
#define OE_NUM_BANDS REF_OE_NUM_BANDS
#else
#include "openear_dsp.h"
#endif
#include "audio_convert.h"
#ifndef USE_REF
#include "ff.h"
#include "recorder.h"
FRESULT f_mount(FATFS *fs, const char *p, int o) { (void)fs; (void)p; (void)o; return FR_OK; }
FRESULT f_stat(const char *p, FILINFO *fi) { (void)p; (void)fi; return FR_NO_FILE; }
FRESULT f_open(FIL *f, const char *p, int m) { (void)f; (void)p; (void)m; return FR_OK; }
FRESULT f_write(FIL *f, const void *b, UINT n, UINT *bw) { (void)f; (void)b; *bw = n; return FR_OK; }
FRESULT f_lseek(FIL *f, long o) { (void)f; (void)o; return FR_OK; }
FRESULT f_close(FIL *f) { (void)f; return FR_OK; }
int snprintf(char *s, unsigned long n, const char *f, ...) { (void)f; if (n) s[0] = 0; return 0; }
int printf(const char *f, ...) { (void)f; return 0; }
#endif

#ifndef N
#define N 64
#endif
static oe_dsp_t dsp;
static oe_params_t p;
static int16_t in[N], out[N];
static float x[N], y[N];
volatile uint32_t marker;

static void block(void)
{
    audio_to_float(in, x, N);
    oe_dsp_process(&dsp, x, y, N);
    audio_from_float(y, out, N);
}

static void fill(uint32_t *s, int amp)
{
    for (int i = 0; i < N; i++) {
        *s = *s * 1103515245u + 12345u;
        in[i] = (int16_t)((int)((*s >> 16) % (2 * amp + 1)) - amp);
    }
}

void bench_entry(void)
{
    uint32_t s = 12345;
    oe_dsp_default_params(&p);
    const float hl[OE_NUM_BANDS] = { 10, 15, 25, 40, 55, 60 };
    oe_fit_half_gain(&p, hl);
    oe_dsp_init(&dsp, 48000.0f, &p);
    fill(&s, 3000);
    block();
    block();
    marker = 1; /* mic block, fitted profile */
    block();
    marker = 2;
    p.tone_on = 1;
    p.tone_db = -30.0f;
    oe_dsp_set_params(&dsp, &p);
    for (int k = 0; k < 12; k++) block(); /* past the fade-in */
    marker = 3; /* tone block (hearing test) */
    block();
    marker = 4;
#ifndef USE_REF
    p.tone_on = 0;
    oe_dsp_set_params(&dsp, &p);
    for (int k = 0; k < 12; k++) block();
    char name[24];
    rec_start(10, name, sizeof name);
    block();
    rec_feed_isr(in, out, N);
    marker = 5; /* mic block while recording to SD */
    block();
    rec_feed_isr(in, out, N);
    marker = 6;
#endif
    __asm volatile("bkpt #0");
}

void _exit(int c) { (void)c; for (;;) {} }
int _kill(int a, int b) { (void)a; (void)b; return -1; }
int _getpid(void) { return 1; }
void *_sbrk(int i) { static char h[4096]; static int u; void *r = h + u; u += i; return r; }
