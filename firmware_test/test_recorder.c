/*
 * OpenEar SD recorder test (runs on a PC). Compiles the firmware's real
 * recorder.c against an in-memory fake SD card and checks the WAV file it
 * writes: header, length, and every sample against a straightforward
 * per-sample model of the anti-alias filter + 3:1 decimation.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "openear_dsp.h"
#include "recorder.h"

static unsigned char disk[1 << 20];
static long disk_len, pos;
static int failures;
#define CHECK(c, ...) do { int ok_ = (c); printf(ok_ ? "  PASS  " : "  FAIL  "); if (!ok_) failures++; printf(__VA_ARGS__); printf("\n"); } while (0)

FRESULT f_mount(FATFS *fs, const char *p, int o) { (void)fs; (void)p; (void)o; return FR_OK; }
FRESULT f_stat(const char *p, FILINFO *fi) { (void)p; (void)fi; return FR_NO_FILE; }
FRESULT f_open(FIL *f, const char *p, int m) { (void)f; (void)p; (void)m; disk_len = pos = 0; return FR_OK; }
FRESULT f_write(FIL *f, const void *b, UINT n, UINT *bw)
{
    (void)f;
    memcpy(disk + pos, b, n);
    pos += n;
    if (pos > disk_len) disk_len = pos;
    *bw = n;
    return FR_OK;
}
FRESULT f_lseek(FIL *f, long o) { (void)f; pos = o; return FR_OK; }
FRESULT f_close(FIL *f) { (void)f; return FR_OK; }

static int rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24; }
static int rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

/* compare the file with a fresh per-sample model of the AA filter + 3:1 */
static int compare(const int16_t *in, const int16_t *out, int n, int frames)
{
    oe_biquad_t aa[2][2];
    for (int c = 0; c < 2; c++)
        for (int s = 0; s < 2; s++) oe_biquad_lowpass(&aa[c][s], 48000.0f, 7000.0f, 0.70710678f);
    int k = 0, bad = 0, ph = 0;
    for (int i = 0; i < n && k < frames; i++) {
        float l = oe_biquad_run(&aa[0][1], oe_biquad_run(&aa[0][0], (float)in[i]));
        float r = oe_biquad_run(&aa[1][1], oe_biquad_run(&aa[1][0], (float)out[i]));
        if (++ph < 3) continue;
        ph = 0;
        int16_t el = (int16_t)(l > 32767.f ? 32767 : (l < -32768.f ? -32768 : l));
        int16_t er = (int16_t)(r > 32767.f ? 32767 : (r < -32768.f ? -32768 : r));
        const unsigned char *p = disk + 44 + 4 * k;
        bad += ((int16_t)rd16(p) != el) + ((int16_t)rd16(p + 2) != er);
        k++;
    }
    return k == frames ? bad : -1;
}

int main(void)
{
    printf("OpenEar recorder test (real recorder.c, fake SD card)\n");
    const int secs = 1, n = 48000 * secs + 640, blk = 64;
    int16_t *in = malloc(n * 2), *out = malloc(n * 2);
    srand(5);
    for (int i = 0; i < n; i++) {
        in[i] = (int16_t)(12000 * sin(2 * M_PI * 1000 * i / 48000.0) + (rand() % 2001) - 1000);
        out[i] = (int16_t)(-20000 * sin(2 * M_PI * 6000 * i / 48000.0) + (i % 97) * 50);
    }
    char name[24];
    CHECK(rec_start(secs, name, sizeof name) == 0 && !strcmp(name, "OE000.WAV"), "rec_start opens %s", name);
    for (int i = 0; i + blk <= n; i += blk) {
        rec_feed_isr(in + i, out + i, blk);
        rec_poll();
    }
    for (int k = 0; k < 10; k++) rec_poll();
    CHECK(!rec_active(), "recording finished by itself after %d s", secs);
    CHECK(!memcmp(disk, "RIFF", 4) && !memcmp(disk + 8, "WAVEfmt ", 8) && rd32(disk + 16) == 16 && rd16(disk + 20) == 1,
          "WAV header: RIFF/WAVE, PCM fmt chunk");
    CHECK(rd16(disk + 22) == 2 && rd32(disk + 24) == REC_FS && rd32(disk + 28) == REC_FS * 4 &&
              rd16(disk + 32) == 4 && rd16(disk + 34) == 16,
          "WAV format: 2 channels, %d Hz, byte rate %d, block align %d, %d bits", rd16(disk + 22) ? rd32(disk + 24) : 0,
          rd32(disk + 28), rd16(disk + 32), rd16(disk + 34));
    int data = rd32(disk + 40);
    CHECK(data == REC_FS * secs * 4 && disk_len == 44 + data && rd32(disk + 4) == 36 + data,
          "data chunk %d bytes = %d s, RIFF size %d, file %ld bytes", data, secs, rd32(disk + 4), disk_len);
    int bad = compare(in, out, n, REC_FS * secs);
    CHECK(bad == 0, "all %d frames match the per-sample model (left = mic, right = output), %d differ", REC_FS * secs, bad);

    /* a second recording starts from a clean filter and decimation phase */
    CHECK(rec_start(secs, name, sizeof name) == 0, "second recording starts");
    for (int i = 0; i + blk <= n; i += blk) {
        if (i == 0) rec_feed_isr(in, out, 1); /* one odd sample first */
        rec_feed_isr(in + i + (i == 0), out + i + (i == 0), blk - (i == 0));
        rec_poll();
    }
    for (int k = 0; k < 10; k++) rec_poll();
    bad = compare(in, out, n, REC_FS * secs);
    CHECK(bad == 0, "second recording also matches a fresh model (%d differ)", bad);

    /* SD card too slow: the ring overruns, the file stays consistent */
    char st[96];
    CHECK(rec_start(2, name, sizeof name) == 0, "third recording starts");
    for (int i = 0; i + blk <= n; i += blk) rec_feed_isr(in + i, out + i, blk); /* 1 s, no polling */
    rec_status(st, sizeof st);
    int ovr = -1;
    const char *o = strstr(st, "overruns");
    if (o) sscanf(o, "overruns %d", &ovr);
    CHECK(ovr == n / 3 - 8192, "ring overruns counted while the card can't keep up (%d, expected %d)", ovr, n / 3 - 8192);
    rec_stop();
    CHECK(rd32(disk + 40) == 8192 * 4 && disk_len == 44 + 8192 * 4 && compare(in, out, n, 8192) == 0,
          "'rec stop' writes what the ring kept before closing (%d bytes, matches the model)", rd32(disk + 40));
    /* 'rec stop' half way through a chunk: the partial chunk is written too */
    CHECK(rec_start(5, name, sizeof name) == 0, "fourth recording starts (5 s)");
    int fed = 0;
    for (int i = 0; i + blk <= 24000; i += blk) { /* 0.5 s, polled as on the board */
        rec_feed_isr(in + i, out + i, blk);
        rec_poll();
        fed += blk;
    }
    int frames = fed / 3; /* 7968 frames: 7 whole chunks of 1024 + 800 */
    rec_stop();
    CHECK(rd32(disk + 40) == frames * 4 && compare(in, out, fed, frames) == 0,
          "'rec stop' mid-chunk writes all %d frames, the last %d from a partial chunk (%d bytes)", frames,
          frames % 1024, rd32(disk + 40));
    free(in);
    free(out);
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL RECORDER TESTS PASSED", failures);
    return failures != 0;
}
