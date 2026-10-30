/* OpenEar recorder - see recorder.h. Licence: MIT. */
#if OE_REC
#include "recorder.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "mxc_device.h"
#include "wdt.h"
#include "openear_dsp.h"

/* RAM ring of stereo int16 frames at 16 kHz: 8192 frames = 512 ms of slack
 * for slow SD writes (32 KB). */
#define RING_FRAMES 8192
#define CHUNK_FRAMES 1024 /* write 4 KB at a time */

static int16_t ring[RING_FRAMES][2];
static volatile uint32_t head, tail; /* frames; head written by ISR only */
static volatile uint32_t overruns;
static volatile int armed;           /* ISR copies only when armed */

static FATFS fs;
static FIL file;
static int mounted, open_file;
static uint32_t frames_target, frames_written;
static char cur_name[24];
static int last_err;

/* anti-alias: 2 cascaded Butterworth low-passes at 7 kHz per channel */
static oe_biquad_t aa[2][2];
static int decim_phase;

/* ------------------------------------------------------------------------ */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wav_header(uint8_t h[44], uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    h[20] = 1; /* PCM */
    h[21] = 0;
    h[22] = 2; /* stereo */
    h[23] = 0;
    put_le32(h + 24, REC_FS);
    put_le32(h + 28, REC_FS * 4);
    h[32] = 4; /* block align */
    h[33] = 0;
    h[34] = 16; /* bits */
    h[35] = 0;
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

/* ------------------------------------------------------------------------ */
void rec_feed_isr(const int16_t *in, const int16_t *out, int n)
{
    static float l[64], r[64];
    if (!armed) {
        return;
    }
    while (n > 0) {
        int m = n > 64 ? 64 : n;
        for (int i = 0; i < m; i++) {
            l[i] = (float)in[i];
            r[i] = (float)out[i];
        }
        oe_biquad2_block(&aa[0][0], &aa[0][1], l, l, m);
        oe_biquad2_block(&aa[1][0], &aa[1][1], r, r, m);
        for (int i = 0; i < m; i++) {
            if (++decim_phase < 3) {
                continue;
            }
            decim_phase = 0;
            if (head - tail >= RING_FRAMES) {
                overruns++;
                continue;
            }
            int16_t *f = ring[head % RING_FRAMES];
            float lv = l[i], rv = r[i];
            f[0] = (int16_t)(lv > 32767.f ? 32767 : (lv < -32768.f ? -32768 : lv));
            f[1] = (int16_t)(rv > 32767.f ? 32767 : (rv < -32768.f ? -32768 : rv));
            __DMB(); /* frame is in the ring before the consumer can see it */
            head++;
        }
        in += m;
        out += m;
        n -= m;
    }
}

/* ------------------------------------------------------------------------ */
static int pick_name(void)
{
    FILINFO fi;
    for (int k = 0; k < 1000; k++) {
        MXC_WDT_ResetTimer(MXC_WDT0); /* a card full of recordings is slow to scan */
        snprintf(cur_name, sizeof(cur_name), "OE%03d.WAV", k);
        FRESULT r = f_stat(cur_name, &fi);
        if (r == FR_NO_FILE) {
            return 0;
        }
        if (r != FR_OK) {
            /* card pulled or failing: don't spin through 1000 slow timeouts */
            f_mount(NULL, "", 0);
            mounted = 0;
            return -(int)r - 300;
        }
    }
    return -1;
}

int rec_start(int seconds, char *name, int name_len)
{
    UINT bw;
    uint8_t h[44];

    if (open_file) {
        return -10; /* already recording */
    }
    if (!mounted) {
        if ((last_err = f_mount(&fs, "", 1)) != FR_OK) {
            return -last_err - 100;
        }
        mounted = 1;
    }
    int pn = pick_name();
    if (pn) {
        return pn < -1 ? pn : -11;
    }
    if ((last_err = f_open(&file, cur_name, FA_CREATE_ALWAYS | FA_WRITE)) != FR_OK) {
        mounted = 0; /* card may have been swapped: remount next time */
        f_mount(NULL, "", 0);
        return -last_err - 200;
    }
    wav_header(h, 0);
    if ((last_err = f_write(&file, h, sizeof(h), &bw)) != FR_OK || bw != sizeof(h)) {
        f_close(&file);
        return -last_err - 400;
    }
    open_file = 1;

    for (int c = 0; c < 2; c++) {
        for (int s = 0; s < 2; s++) {
            oe_biquad_lowpass(&aa[c][s], 48000.0f, 7000.0f, 0.70710678f);
        }
    }
    decim_phase = 0;
    frames_target = (uint32_t)seconds * REC_FS;
    frames_written = 0;
    overruns = 0;
    tail = head;
    armed = 1;
    if (name) {
        snprintf(name, name_len, "%s", cur_name);
    }
    return 0;
}

static void finish(void)
{
    UINT bw;
    uint8_t h[44];
    armed = 0;
    wav_header(h, frames_written * 4);
    f_lseek(&file, 0);
    f_write(&file, h, sizeof(h), &bw);
    f_close(&file);
    open_file = 0;
    printf("\n[rec] saved %s: %.1f s, %lu overruns\n> ", cur_name,
           (double)frames_written / REC_FS, (unsigned long)overruns);
}

int rec_stop(void)
{
    if (!open_file) {
        return -1;
    }
    /* write what is still in the RAM ring (up to ~0.5 s) before closing */
    armed = 0;
    uint32_t guard = RING_FRAMES / CHUNK_FRAMES + 2;
    while (open_file && head != tail && frames_written < frames_target && guard--) {
        rec_poll();
        if (open_file && head - tail < CHUNK_FRAMES) {
            /* rec_poll only writes whole chunks before the end: flush the rest */
            UINT bw;
            static int16_t rest[CHUNK_FRAMES][2];
            uint32_t n = head - tail, left = frames_target - frames_written;
            if (n > left) n = left;
            for (uint32_t i = 0; i < n; i++) {
                rest[i][0] = ring[(tail + i) % RING_FRAMES][0];
                rest[i][1] = ring[(tail + i) % RING_FRAMES][1];
            }
            tail += n;
            if (f_write(&file, rest, n * 4, &bw) == FR_OK && bw == n * 4) {
                frames_written += n;
            }
            break;
        }
    }
    if (open_file) {
        finish();
    }
    return 0;
}

void rec_poll(void)
{
    static int16_t chunk[CHUNK_FRAMES][2];
    UINT bw;

    if (!open_file) {
        return;
    }
    uint32_t avail = head - tail;
    __DMB(); /* read the frames only after reading head */
    uint32_t left = frames_target - frames_written;
    if (avail >= CHUNK_FRAMES || (avail && avail >= left)) {
        uint32_t n = avail < CHUNK_FRAMES ? avail : CHUNK_FRAMES;
        if (n > left) {
            n = left;
        }
        for (uint32_t i = 0; i < n; i++) {
            chunk[i][0] = ring[(tail + i) % RING_FRAMES][0];
            chunk[i][1] = ring[(tail + i) % RING_FRAMES][1];
        }
        tail += n;
        if (f_write(&file, chunk, n * 4, &bw) != FR_OK || bw != n * 4) {
            printf("\n[rec] SD write error, stopping\n> ");
            finish();
            return;
        }
        frames_written += n;
    }
    if (frames_written >= frames_target) {
        finish();
    }
}

int rec_active(void)
{
    return open_file;
}

void rec_status(char *buf, int len)
{
    if (open_file) {
        snprintf(buf, len, "REC %s %.1f/%.0f s overruns %lu", cur_name,
                 (double)frames_written / REC_FS, (double)frames_target / REC_FS,
                 (unsigned long)overruns);
    } else {
        snprintf(buf, len, "idle%s", mounted ? " (card mounted)" : "");
    }
}

#endif /* OE_REC */
