/*
 * OpenEar command console - hardware independent.
 *
 * The same text commands arrive from the USB serial port and from
 * Bluetooth. Everything that touches hardware goes through cli_hw_t, so this
 * file also compiles and is tested on a PC (see ../cli_test).
 * Licence: MIT.
 */
#ifndef OPENEAR_CLI_H
#define OPENEAR_CLI_H

#include <stdint.h>

#include "openear_dsp.h"

#define OE_FW_VERSION "0.3.1"

typedef struct {
    float cpu_now_pct, cpu_peak_pct, cpu_avg_pct; /* DSP interrupt time / real time */
    uint32_t blocks, tx_blocks, late;
    uint32_t window_blocks; /* blocks since the last 'stat' (late counts the same window) */
    int lag_max;            /* worst interrupt start delay, samples */
    int slack_min;          /* fewest samples left before the output deadline */
    uint32_t restarts;      /* audio watchdog restarts */
    unsigned long phase_fixes; /* since the last 'stat' */
    int timer_ok, dwt_ok;
    int block_size, fs;
} cli_audio_info_t;

typedef struct {
    /* output sink for everything the console prints */
    void (*write)(const char *s, int len);
    /* hand new parameters to the audio interrupt */
    void (*push_params)(const oe_params_t *p);
    /* live DSP state for show/levels (read-only) */
    const oe_dsp_t *(*dsp)(void);
    void (*audio_info)(cli_audio_info_t *info, int reset_peak);
    int (*set_hpvol)(uint8_t code);
    int (*sidetone)(int on, uint8_t gain);
    int (*raw)(int16_t *buf, int n);
    int (*batt)(int *mv, float *pct);
    int (*i2c_probe)(int addr); /* 1 if a chip answers at this address */
    int (*save)(const oe_params_t *p, uint8_t hpvol);
    int (*load)(oe_params_t *p, uint8_t *hpvol);
    /* recorder: optional (NULL = not built) */
    int (*rec_start)(int seconds, char *name, int name_len);
    int (*rec_stop)(void);
    void (*rec_status)(char *buf, int len);
    /* bluetooth status line: optional */
    void (*ble_status)(char *buf, int len);
    /* audio streams: optional (NULL = always running) */
    int (*audio_running)(void);
    int (*audio_restart)(void); /* start audio and unmute; 0 = ok */
} cli_hw_t;

void cli_init(const cli_hw_t *hw, const oe_params_t *initial, uint8_t hpvol);
/* Parse and execute one command line (modified in place). */
void cli_handle_line(char *line);
/* While set, commands come from Bluetooth: anyone in radio range could send
 * them, so loudness limits are tighter (hpvol >= 12, vol <= +6 dB). */
#define CLI_REMOTE_HPVOL_MIN 12
#define CLI_REMOTE_VOL_MAX 6.0f
void cli_set_remote(int remote);
void cli_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Call often from the main loop: stops a test tone that has run unchanged for
 * 15 s (the hearing test's silent hold, <= -90 dBFS, after 60 s), so a phone
 * that disconnects can't leave a beep playing or the mic muted. */
#define CLI_TONE_TIMEOUT_MS 15000
#define CLI_TONE_HOLD_DB -90.0f
#define CLI_TONE_HOLD_TIMEOUT_MS 60000
void cli_tick(uint32_t now_ms);

/* for buttons and the meter in main */
oe_params_t *cli_params(void);
void cli_params_changed(void);
uint8_t cli_hpvol(void);

#endif
