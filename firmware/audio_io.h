/*
 * OpenEar audio I/O for the MAX32655FTHR.
 *
 * The MAX9867 codec is the I2S clock master (48 kHz, from its 12.288 MHz
 * MCLK). The onboard digital mic is wired into the codec, so mic -> codec ->
 * I2S RX -> DSP -> I2S TX -> codec -> headphone jack is one full-duplex path.
 *
 * Two DMA channels run continuously with hardware reload (ping-pong
 * buffers). The DSP runs inside the RX DMA interrupt, so nothing in the
 * main loop (printing, flash writes) can cause an audio dropout.
 *
 * Licence: MIT.
 */
#ifndef OPENEAR_AUDIO_IO_H
#define OPENEAR_AUDIO_IO_H

#include <stdint.h>

#define AUDIO_FS 48000
#ifndef AUDIO_BLOCK
#define AUDIO_BLOCK 64 /* samples per block: 64 @ 48 kHz = 1.33 ms */
#endif
/* Which I2S slot carries the mic. ADI's example uses the right one; if
 * 'raw' prints zeros while you talk, build with: make AUDIO_CH=LEFT */
#ifndef AUDIO_I2S_CHANNEL
#define AUDIO_I2S_CHANNEL MXC_I2S_MONO_RIGHT_CH
#endif

/* Called from the RX interrupt with one block of mic samples. Must fill
 * `out` with AUDIO_BLOCK samples and return quickly (< half a block). */
typedef void (*audio_process_fn)(const int16_t *in, int16_t *out, int n);

typedef struct {
    uint32_t blocks;          /* blocks processed */
    uint32_t tx_blocks;       /* output buffers played (should track blocks) */
    uint32_t late;            /* blocks finished after TX had started playing them */
    uint32_t ticks_last;      /* timer ticks spent in the last block's interrupt */
    uint32_t ticks_max;       /* worst case since last reset */
    uint64_t ticks_sum;       /* for the average since last reset */
    uint32_t timed_blocks;    /* blocks in ticks_sum */
    uint32_t budget;          /* timer ticks per block (real time) */
    uint32_t lag_max;         /* worst interrupt start delay (samples) */
    uint32_t slack_min;       /* fewest samples left before the deadline */
    uint32_t blocks_at_reset; /* blocks when 'stat' last reset the counters */
    uint32_t restarts;        /* audio watchdog restarts */
    uint32_t phase_fixes;     /* output blocks played 2 samples short/long to stay in step */
} audio_stats_t;

/* Sets up I2S, the I2C bus, the codec and mic power, printing one line per
 * step. Returns 0 or the first fatal MSDK error code. */
int audio_init(audio_process_fn fn);
/* 1 once audio_init() configured the codec (muted) and it still answers. */
int audio_codec_ready(void);
/* Plain-words name for an MSDK error code (E_BUSY -> "I2C bus busy"). */
const char *err_name(int err);
int audio_start(void);
/* Stop/restart both streams (used around flash writes). */
void audio_stop(void);
int audio_running(void);
void audio_get_stats(audio_stats_t *s);
void audio_reset_max(void);
#include "cli.h"
/* What 'stat' prints, from the interrupt's counters; reset starts a new window. */
void audio_get_info(cli_audio_info_t *a, int reset);
/* Codec headphone volume. Written only while audio runs (the codec is never
 * unmuted without audio); otherwise remembered and applied at the next start. */
int audio_set_volume(uint8_t hp_volume);
/* Start audio, then unmute at hp_volume; stops and mutes on any failure. */
int audio_restart(uint8_t hp_volume);
/* Call from the main loop: restarts audio if either stream has stopped for
 * 30 ms. Returns 1 if it restarted audio, -1 if that failed, else 0. */
int audio_check(uint32_t now_ms);
/* Copy of the most recent raw mic block (for the "raw" diagnostic). */
void audio_copy_last_input(int16_t *dst, int n);

#endif
