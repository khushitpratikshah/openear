/*
 * OpenEar audio I/O - see audio_io.h. Licence: MIT.
 *
 * I2S and codec settings are deliberately identical to ADI's MSDK
 * "I2S" record/playback example for this board (48 kHz, 16-bit, mono,
 * codec is clock master), which is known to work. What's new here is
 * running RX and TX at the same time with DMA reload.
 *
 * Timing (positions in samples of the RX DMA's current block, 0..N-1):
 *   RX: |--blk0--|--blk1--|--blk2--|      interrupt at each boundary -> DSP
 *   TX:       |--blk0--|--blk1--|--       TX DMA started at RX position N-6
 * The TX DMA reads a few samples ahead into the I2S FIFO (~F), so its own
 * buffer boundary falls at about N-6-F. When an RX block completes, the TX
 * DMA has therefore just started a buffer: the other one is free for about
 * N-6-F samples (~80 % of a block). In 0.2.x TX started at 3N/4, which left
 * only ~3N/4-F (~65 %), while the DSP took ~60-70 % of a block: about one
 * block in five finished late, and a late block plays stale audio (static).
 *
 * Buffer choice comes from the DMA's live address registers, not from
 * counting interrupts, so a missed interrupt can't swap the buffers.
 *
 * Diagnostics ('stat'): CPU time per block from the hardware timer, and,
 * independent of any clock, the RX position when the interrupt starts
 * (interrupt latency) and the TX samples still to go when processing ends
 * (slack before the deadline; 0 = late).
 */
#include "audio_io.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "cli.h"
#include "codec_bus.h"
#include "fuelgauge.h"
#include "mxc_delay.h"
#include "dma.h"
#include "i2s.h"
#include "max9867.h"
#include "mxc_device.h"
#include "mxc_errors.h"
#include "mxc_sys.h"
#include "nvic_table.h"
#include "oe_time.h"

#define CODEC_I2C MXC_I2C1 /* SCL P0_16, SDA P0_17 on the FTHR */
#define CODEC_MCLK 12288000
#define BLOCK_BYTES (AUDIO_BLOCK * sizeof(int16_t))
/* Interrupt priorities (lower number = more urgent). Bluetooth's radio
 * interrupts stay at 0, the most urgent, to keep their timing. The output
 * (TX) DMA interrupt only swaps a buffer pointer (~1 us), so it may interrupt
 * the input (RX) interrupt that runs the DSP: then even a DSP block that runs
 * long can't let the output DMA run out of reloads and stop. */
#define AUDIO_TX_IRQ_PRIO 1
#define AUDIO_RX_IRQ_PRIO 2

/* DMA register access. The PC simulation (firmware_test/audio_sim) defines
 * these to model the chip; on the chip they are plain register accesses.
 * The channel status flags are write-1-to-clear. */
#ifndef OE_DMA_STATUS
#define OE_DMA_STATUS(c) (MXC_DMA->ch[(c)].status)
#define OE_DMA_W1C(c, f) (MXC_DMA->ch[(c)].status = (f))
#define OE_DMA_CNT(c) (MXC_DMA->ch[(c)].cnt)
#endif
#define OE_DMA_IF_MASK \
    (MXC_F_DMA_STATUS_CTZ_IF | MXC_F_DMA_STATUS_RLD_IF | MXC_F_DMA_STATUS_BUS_ERR | MXC_F_DMA_STATUS_TO_IF)

static int16_t rx_buf[2][AUDIO_BLOCK] __attribute__((aligned(4)));
/* two extra samples: a block played 2 samples long repeats its last one.
 * Steps are 2 samples because the TX DMA packs two 16-bit samples into
 * each 32-bit FIFO word: an odd length would misalign every later sample. */
#define TX_STEP 2
#define TX_LEN (AUDIO_BLOCK + TX_STEP)
static int16_t tx_buf[2][TX_LEN] __attribute__((aligned(4)));
static int16_t raw_copy[AUDIO_BLOCK]; /* last mic block, copied in the ISR */

static mxc_i2s_req_t i2s_req;
static audio_process_fn process_fn;
static volatile int rx_ch = -1, tx_ch = -1;
static volatile audio_stats_t stats;
static volatile int running;
static int codec_ready; /* codec configured, muted, and still answering */
static uint8_t cur_hpvol = 0x40;
static volatile int check_rearm; /* audio_check: new baseline after a start */

/* Output phase lock. On the board (0.3.0 test, 2026-10-28) the output stream
 * fell behind the input by ~1 sample every ~49 ms, so the output's buffer
 * boundary crept through the input's block and about 36 % of blocks were
 * played before the DSP had finished them (crackle). The RX interrupt
 * measures where the output boundary sits; when it has drifted, the TX
 * interrupt plays the next block 2 samples short (or long) to put it back. */
static volatile int tx_adj;    /* -2 / +2: next TX reload 2 samples short / long */
static int ph_target = -1;     /* wanted phase (samples), set after a start */
static int ph_blocks, ph_sum, ph_n, ph_hold;
static uint32_t phase_fixes_at_reset;

#define PMIC_ADDR 0x28 /* MAX20303 (0x50 in max20303.c is the 8-bit form) */
static void codec_off(void);

/* ------------------------------------------------------------------------ */
static int in_buf(uint32_t addr, const int16_t *buf)
{
    return (addr - (uint32_t)buf) < BLOCK_BYTES;
}

static int in_tx_buf(uint32_t addr, const int16_t *buf)
{
    return (addr - (uint32_t)buf) < TX_LEN * sizeof(int16_t);
}

static void arm_reload_len(int ch, void *src, void *dst, uint32_t len)
{
    mxc_dma_srcdst_t r;
    r.ch = ch;
    r.source = src;
    r.dest = dst;
    r.len = len;
    MXC_DMA_SetSrcReload(r);
}

static void arm_reload(int ch, void *src, void *dst)
{
    arm_reload_len(ch, src, dst, BLOCK_BYTES);
}

/* RX interrupt: phase = RX samples since the block boundary + TX samples
 * still to go in its current buffer = where the output boundary falls in
 * the input block. It does not depend on how late the interrupt started. */
static void phase_track(uint32_t lag)
{
    int ph = (int)lag + (int)((OE_DMA_CNT(tx_ch) & 0xFFFFFF) / 2);
    if (ph_target < 0) {
        /* settle for a few blocks, then hold the phase where it started */
        if (++ph_blocks >= 4) {
            ph_target = ph;
        }
        return;
    }
    int e = ph - ph_target;
    if (e >= AUDIO_BLOCK / 2) {
        e -= AUDIO_BLOCK;
    } else if (e < -AUDIO_BLOCK / 2) {
        e += AUDIO_BLOCK;
    }
    if (ph_hold > 0) { /* a fix takes ~2 blocks to show: don't count twice */
        ph_hold--;
        return;
    }
    ph_sum += e;
    if (++ph_n < 4) {
        return;
    }
    if (ph_sum > 4 * 2) {
        tx_adj = -TX_STEP; /* output boundary late: play one block short */
        ph_hold = 3;
    } else if (ph_sum < -4 * 2) {
        tx_adj = TX_STEP; /* early: one block long */
        ph_hold = 3;
    }
    ph_sum = 0;
    ph_n = 0;
}

/* ------------------------------------------------------------------------ */
/* samples the RX DMA has written into its current block */
static uint32_t rx_pos(void)
{
    uint32_t left = OE_DMA_CNT(rx_ch) & 0xFFFFFF;
    return left >= BLOCK_BYTES ? 0 : (BLOCK_BYTES - left) / 2;
}

static void dma_cb(int ch, int err)
{
    (void)err;

    if (ch == tx_ch) {
        /* the buffer the DMA is NOT reading is the one that just finished:
         * queue it to play after the current one */
        int playing = in_tx_buf(MXC_DMA->ch[ch].src, tx_buf[1]);
        /* If no RX block has completed for two output blocks, the input side
         * has stopped (or one DSP block has overrun by two blocks, which is
         * audible anyway): silence the buffer we re-queue, so the codec plays
         * silence and not the last two blocks on a loop (a buzz). In normal
         * operation an RX block completes between any two TX blocks. */
        static uint32_t rx_seen, tx_without_rx;
        if (stats.blocks != rx_seen) {
            rx_seen = stats.blocks;
            tx_without_rx = 0;
        } else if (++tx_without_rx >= 2) {
            memset(tx_buf[!playing], 0, sizeof(tx_buf[0]));
        }
        int adj = tx_adj;
        tx_adj = 0;
        arm_reload_len(ch, tx_buf[!playing], (void *)MXC_DMA->ch[ch].dst,
                       (uint32_t)(AUDIO_BLOCK + adj) * sizeof(int16_t));
        if (adj) {
            stats.phase_fixes++;
        }
        stats.tx_blocks++;
        return;
    }

    if (ch == rx_ch) {
        uint32_t t0 = oe_ticks();
        uint32_t lag = rx_pos(); /* how late this interrupt started */
        /* the buffer the DMA is NOT writing is the block that just completed */
        int filling = in_buf(MXC_DMA->ch[ch].dst, rx_buf[1]);
        const int16_t *in = rx_buf[!filling];
        arm_reload(ch, (void *)MXC_DMA->ch[ch].src, (void *)in);

        if (tx_ch < 0) {
            return; /* TX isn't started yet (first block of audio_start) */
        }
        phase_track(lag);
        int playing = in_tx_buf(MXC_DMA->ch[tx_ch].src, tx_buf[1]);
        int16_t *out = tx_buf[!playing];

        memcpy(raw_copy, in, BLOCK_BYTES);
        if (process_fn) {
            process_fn(in, out, AUDIO_BLOCK);
        } else {
            memcpy(out, in, BLOCK_BYTES);
        }
        out[AUDIO_BLOCK] = out[AUDIO_BLOCK + 1] = out[AUDIO_BLOCK - 1]; /* only a long block plays these */

        /* late = the TX DMA already read part of the buffer we just wrote
         * (it played stale samples). Pointing at its start after a reload,
         * with nothing read yet, is still in time. */
        uint32_t slack;
        uint32_t tx_src = MXC_DMA->ch[tx_ch].src;
        if (in_tx_buf(tx_src, out)) {
            if (tx_src != (uint32_t)out) {
                stats.late++;
            }
            slack = 0;
        } else {
            slack = (OE_DMA_CNT(tx_ch) & 0xFFFFFF) / 2;
        }
        uint32_t dt = oe_ticks() - t0;
        stats.ticks_last = dt;
        if (dt > stats.ticks_max) {
            stats.ticks_max = dt;
        }
        stats.ticks_sum += dt;
        stats.timed_blocks++;
        if (lag > stats.lag_max) {
            stats.lag_max = lag;
        }
        if (slack < stats.slack_min) {
            stats.slack_min = slack;
        }
        stats.blocks++;
    }
}

/* Our own handler for the two audio channels (not MXC_DMA_Handler): it
 * clears a channel's flags BEFORE running its callback. The SDK clears them
 * after, so if a count-to-zero happened while the DSP was still running (a
 * pile-up of Bluetooth interrupts stretching one block past its time), that
 * event was erased, the RX reload was never re-armed, and the input stream
 * stopped for good. Cleared first, it simply raises the interrupt again. */
static void service_ch(int ch)
{
    if (ch < 0) {
        return;
    }
    /* clear exactly the flags we saw (the SDK's ClearFlags does a
     * read-modify-write that can also clear one that arrives in between) */
    uint32_t fl = OE_DMA_STATUS(ch) & OE_DMA_IF_MASK;
    if (fl) {
        OE_DMA_W1C(ch, fl);
    }
    if (fl & MXC_F_DMA_STATUS_CTZ_IF) {
        dma_cb(ch, 0);
    }
}

static void rx_irq(void)
{
    service_ch(rx_ch);
}

static void tx_irq(void)
{
    service_ch(tx_ch);
}

/* ------------------------------------------------------------------------ */
int audio_init(audio_process_fn fn)
{
    int err;

    process_fn = fn;
    stats.budget = (uint32_t)((uint64_t)oe_ticks_per_ms() * 1000u * AUDIO_BLOCK / AUDIO_FS);
    stats.slack_min = AUDIO_BLOCK;

    memset(rx_buf, 0, sizeof(rx_buf));
    memset(tx_buf, 0, sizeof(tx_buf));

    i2s_req.wordSize = MXC_I2S_WSIZE_HALFWORD;
    i2s_req.sampleSize = MXC_I2S_SAMPLESIZE_SIXTEEN;
    i2s_req.bitsWord = 16;
    i2s_req.adjust = MXC_I2S_ADJUST_LEFT;
    i2s_req.justify = MXC_I2S_LSB_JUSTIFY;
    i2s_req.wsPolarity = MXC_I2S_POL_NORMAL;
    i2s_req.channelMode = MXC_I2S_EXTERNAL_SCK_EXTERNAL_WS; /* codec is master */
    i2s_req.stereoMode = AUDIO_I2S_CHANNEL;
    i2s_req.bitOrder = MXC_I2S_LSB_FIRST;
    i2s_req.clkdiv = MXC_I2S_CalculateClockDiv(AUDIO_FS, i2s_req.wordSize) + 1;
    i2s_req.rawData = NULL;
    i2s_req.txData = tx_buf[0];
    i2s_req.rxData = rx_buf[0];
    i2s_req.length = BLOCK_BYTES;

    if ((err = MXC_I2S_Init(&i2s_req)) != E_NO_ERROR) {
        return err;
    }
    MXC_I2S_RegisterDMACallback(dma_cb);

    /* Same order as ADI's working I2S example: start I2C1 once, then the
     * codec, and only then the PMIC (mic power). Up to 3 tries, with a bus
     * recovery between them; every failure prints its evidence. */
    codec_ready = 0;
    if ((err = bus_init()) != E_NO_ERROR) {
        printf("[i2c] bus start failed: %d (%s)\n", err, err_name(err));
        bus_report("after bus start");
        return err;
    }
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = max9867_init(CODEC_I2C, CODEC_MCLK, AUDIO_FS);
        if (err == E_NO_ERROR) {
            printf("codec MAX9867: ok%s\n", attempt > 1 ? " (after bus recovery)" : "");
            break;
        }
        printf("codec MAX9867: try %d failed: %d (%s)\n", attempt, err, err_name(err));
        bus_report("after codec try");
        if (attempt < 3) {
            int r = bus_recover();
            printf("[i2c] bus recovery: %s (%d)\n", r ? "FAILED" : "ok", r);
            if (r != E_NO_ERROR) {
                break; /* the bus is unusable; more tries can't help */
            }
            MXC_Delay(MXC_DELAY_MSEC(5));
        }
    }
    if (err != E_NO_ERROR) {
        codec_off(); /* a write may have landed before the error: power down */
        return err;
    }

    /* mic power comes from the PMIC's LDO1. ADI's example works without it,
     * so a failure here is reported, not fatal. The PMIC driver's loops have
     * no timeouts, so it is only called when the PMIC has just answered on
     * the SDK's (timed) path and the bus is set up the way it needs. */
    if (!bus_ready_for_pmic()) {
        printf("mic power (PMIC LDO1): skipped, bus not ready for the PMIC driver\n");
        bus_report("before PMIC");
    } else if (!i2c_probe(PMIC_ADDR)) {
        printf("mic power (PMIC LDO1): skipped, PMIC not answering at 0x%02X\n", PMIC_ADDR);
    } else {
        err = Microphone_Power(POWER_ON);
        printf("mic power (PMIC LDO1): %s (%d)\n", err ? "FAILED" : "ok", err);
    }

    /* the PMIC driver drives the I2C registers directly; check the codec
     * still answers afterwards (xfer() recovers a busy bus once) */
    uint8_t rev = 0;
    err = max9867_read_reg(0xFF, &rev);
    printf("codec check after PMIC: %s (%d, rev 0x%02X)\n", err ? "FAILED" : "ok", err, rev);
    fflush(stdout);
    if (err != E_NO_ERROR) {
        codec_off();
        return err;
    }
    codec_ready = 1;
    return E_NO_ERROR;
}

/* Best effort: power the codec down (it is muted since max9867_init). */
static void codec_off(void)
{
    max9867_set_volume(0x40);
    max9867_shutdown();
}

int audio_codec_ready(void)
{
    return codec_ready;
}

const char *err_name(int err)
{
    switch (err) {
    case E_NO_ERROR: return "ok";
    case E_NULL_PTR: return "null pointer";
    case E_NO_DEVICE: return "no device / no answer";
    case E_BAD_PARAM: return "bad parameter";
    case E_NOT_SUPPORTED: return "wrong chip ID";
    case E_BUSY: return "I2C bus busy";
    case E_BAD_STATE: return "bad state";
    case E_COMM_ERR: return "I2C error: NACK or stuck line";
    case E_TIME_OUT: return "timeout";
    case E_NO_RESPONSE: return "no response";
    case E_INVALID: return "invalid setting";
    case E_UNINITIALIZED: return "not initialised";
    default: return "other";
    }
}

/* Undo a half-finished audio_start(): no DMA channel may be left running
 * with an interrupt that expects both channels to exist. */
static void abort_start(void)
{
    if (rx_ch >= 0) {
        NVIC_DisableIRQ(MXC_DMA_CH_GET_IRQ(rx_ch));
        MXC_DMA_Stop(rx_ch);
        MXC_DMA_ReleaseChannel(rx_ch);
        rx_ch = -1;
    }
    if (tx_ch >= 0) {
        NVIC_DisableIRQ(MXC_DMA_CH_GET_IRQ(tx_ch));
        MXC_DMA_Stop(tx_ch);
        MXC_DMA_ReleaseChannel(tx_ch);
        tx_ch = -1;
    }
    MXC_I2S_RXDisable();
    MXC_I2S_TXDisable();
    MXC_I2S_Flush();
}

int audio_start(void)
{
    int ch;

    /* the codec has been clocking I2S with no DMA (e.g. during Bluetooth
     * init): drop whatever piled up in the FIFOs */
    MXC_I2S_Flush();
    memset(tx_buf, 0, sizeof(tx_buf));

    ch = MXC_I2S_RXDMAConfig(rx_buf[0], BLOCK_BYTES);
    if (ch < 0) {
        return ch;
    }
    rx_ch = ch;
    NVIC_SetPriority(MXC_DMA_CH_GET_IRQ(rx_ch), AUDIO_RX_IRQ_PRIO);
    MXC_NVIC_SetVector(MXC_DMA_CH_GET_IRQ(rx_ch), rx_irq);
    arm_reload(rx_ch, (void *)MXC_DMA->ch[rx_ch].src, rx_buf[1]);

    /* Start TX when RX is 6 samples from the end of a block (see the timing
     * note at the top). The last part of the wait runs with interrupts off,
     * so a Bluetooth interrupt can't push the TX start into the wrong place;
     * if the window is missed, try again on the next block. Give up after
     * 50 ms if the codec's clocks never start. */
    const uint32_t t_start = oe_ticks(), t_limit = oe_ticks_per_ms() * 50;
    /* N-6, not N-2: the phase lock needs a few samples' room before the
     * output boundary would wrap past the next input boundary */
    const uint32_t near_end = AUDIO_BLOCK - 12;
    uint32_t start_at = AUDIO_BLOCK - 6;
    uint32_t spins = 0; /* second limit (all loops), in case the timer isn't running */
    int started = 0, missed = 0;
    while (!started) {
        if (oe_ticks() - t_start > t_limit || ++spins > 4000000u) {
            abort_start();
            return E_TIME_OUT;
        }
        if (rx_pos() < near_end) {
            continue;
        }
        /* interrupts off for at most ~8 samples (~0.2 ms), and never more
         * than 4000 polls (if the RX DMA froze, we fall out and time out) */
        __disable_irq();
        uint32_t p, inner = 0;
        while ((p = rx_pos()) >= near_end && p < start_at && ++inner < 4000u) {
        }
        spins += inner;
        if (p >= start_at) {
            ch = MXC_I2S_TXDMAConfig(tx_buf[0], BLOCK_BYTES);
            if (ch >= 0) {
                tx_ch = ch;
                NVIC_SetPriority(MXC_DMA_CH_GET_IRQ(tx_ch), AUDIO_TX_IRQ_PRIO);
                MXC_NVIC_SetVector(MXC_DMA_CH_GET_IRQ(tx_ch), tx_irq);
                arm_reload(tx_ch, tx_buf[1], (void *)MXC_DMA->ch[tx_ch].dst);
            }
            started = 1;
        } else if (p < near_end && ++missed >= 3) {
            /* the DMA moved past the window in one step three times: accept
             * any position in the last 12 samples (a little less slack) */
            start_at = near_end;
        }
        __enable_irq();
        if (started && ch < 0) {
            abort_start();
            return ch;
        }
    }
    tx_adj = 0;
    ph_target = -1;
    ph_blocks = ph_sum = ph_n = ph_hold = 0;
    running = 1;
    check_rearm = 1;
    return E_NO_ERROR;
}

/* Stop both streams cleanly, e.g. before a flash erase (which stalls the
 * CPU long enough to miss DMA reloads). Codec is muted first: no click. */
void audio_stop(void)
{
    if (!running) {
        return;
    }
    max9867_set_volume(0x40); /* mute */
    NVIC_DisableIRQ(MXC_DMA_CH_GET_IRQ(rx_ch));
    NVIC_DisableIRQ(MXC_DMA_CH_GET_IRQ(tx_ch));
    MXC_DMA_Stop(rx_ch);
    MXC_DMA_Stop(tx_ch);
    MXC_I2S_RXDisable();
    MXC_I2S_TXDisable();
    MXC_I2S_Flush();
    MXC_DMA_ReleaseChannel(rx_ch);
    MXC_DMA_ReleaseChannel(tx_ch);
    rx_ch = tx_ch = -1;
    running = 0;
    memset(tx_buf, 0, sizeof(tx_buf));
}

/* Start the streams, then (and only then) unmute at hp_volume. On any
 * failure audio is stopped and the codec stays muted. */
int audio_restart(uint8_t hp_volume)
{
    int err = audio_start();
    if (err == E_NO_ERROR) {
        err = audio_set_volume(hp_volume);
    }
    if (err != E_NO_ERROR) {
        audio_stop();
        max9867_set_volume(0x40); /* never leave the DAC unmuted without audio */
    }
    return err;
}

/* The codec is only ever unmuted while audio runs. Without audio the new
 * volume is remembered and applied when audio (re)starts. */
int audio_set_volume(uint8_t hp_volume)
{
    cur_hpvol = hp_volume;
    if (!running) {
        return E_NO_ERROR;
    }
    return max9867_set_volume(hp_volume);
}

/* Main-loop watchdog: if the audio interrupt stops (e.g. a DMA reload was
 * missed during a long interrupt-free stretch), restart the streams. */
int audio_check(uint32_t now_ms)
{
    static uint32_t last_rx, last_tx, since;
    if (!running || check_rearm) {
        /* stopped, or just (re)started (e.g. after a 'save' paused audio for
         * a flash write): take a fresh baseline */
        check_rearm = 0;
        last_rx = stats.blocks;
        last_tx = stats.tx_blocks;
        since = now_ms;
        return 0;
    }
    uint32_t rx = stats.blocks, tx = stats.tx_blocks;
    if (rx != last_rx && tx != last_tx) {
        last_rx = rx;
        last_tx = tx;
        since = now_ms;
        return 0;
    }
    /* either stream stuck (a missed DMA reload) for 30 ms: restart both */
    if (now_ms - since < 30) {
        return 0;
    }
    since = now_ms;
    stats.restarts++;
    audio_stop();
    return audio_restart(cur_hpvol) == E_NO_ERROR ? 1 : -1;
}

int audio_running(void)
{
    return running;
}

void audio_get_stats(audio_stats_t *s)
{
    __disable_irq();
    memcpy(s, (const void *)&stats, sizeof(*s));
    __enable_irq();
}

/* The numbers 'stat' and 'levels' print (cli_audio_info_t), from the
 * interrupt's counters. reset starts a new window (late, peak, slack...). */
void audio_get_info(cli_audio_info_t *a, int reset)
{
    audio_stats_t s;
    audio_get_stats(&s);
    a->blocks = s.blocks;
    a->tx_blocks = s.tx_blocks;
    a->window_blocks = s.blocks - s.blocks_at_reset;
    a->late = s.late;
    a->cpu_now_pct = s.budget ? 100.0f * s.ticks_last / s.budget : 0;
    a->cpu_peak_pct = s.budget ? 100.0f * s.ticks_max / s.budget : 0;
    a->cpu_avg_pct = (s.budget && s.timed_blocks)
                         ? (float)(100.0 * ((double)s.ticks_sum / s.timed_blocks) / s.budget)
                         : 0;
    a->lag_max = (int)s.lag_max;
    a->slack_min = (int)s.slack_min;
    a->restarts = s.restarts;
    a->phase_fixes = s.phase_fixes - phase_fixes_at_reset;
    a->timer_ok = oe_time_ok();
    a->dwt_ok = oe_dwt_counts();
    a->block_size = AUDIO_BLOCK;
    a->fs = AUDIO_FS;
    if (reset) {
        audio_reset_max();
    }
}

void audio_reset_max(void)
{
    __disable_irq();
    stats.ticks_max = 0;
    stats.ticks_sum = 0;
    stats.timed_blocks = 0;
    stats.late = 0;
    stats.lag_max = 0;
    stats.slack_min = AUDIO_BLOCK;
    stats.blocks_at_reset = stats.blocks;
    phase_fixes_at_reset = stats.phase_fixes;
    __enable_irq();
}

void audio_copy_last_input(int16_t *dst, int n)
{
    if (n > AUDIO_BLOCK) {
        n = AUDIO_BLOCK;
    }
    __disable_irq(); /* the ISR rewrites raw_copy every block */
    if (running) {
        memcpy(dst, raw_copy, n * sizeof(int16_t));
    } else {
        memset(dst, 0, n * sizeof(int16_t));
    }
    __enable_irq();
}
