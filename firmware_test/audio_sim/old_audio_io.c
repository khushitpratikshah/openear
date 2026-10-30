/* COPY of firmware/audio_io.c from OpenEar 0.2.2 (git eb2541a), for the
 * before/after simulation only. DWT->CYCCNT reads are replaced by
 * sim_cyccnt() so time passes in the simulator. Do not edit. */
/*
 * OpenEar audio I/O - see audio_io.h. Licence: MIT.
 *
 * I2S and codec settings are deliberately identical to ADI's MSDK
 * "I2S" record/playback example for this board (48 kHz, 16-bit, mono,
 * codec is clock master), which is known to work. What's new here is
 * running RX and TX at the same time with DMA reload.
 *
 * Timing:
 *   RX: |--blk0--|--blk1--|--blk2--|      interrupt at each boundary -> DSP
 *   TX:        |--blk0--|--blk1--|--      started 3/4 of a block later
 * When an RX block completes, TX is a quarter of the way into a buffer, so
 * the other TX buffer is free for ~3/4 of a block (~1 ms at 64 samples),
 * minus the few samples the DMA reads ahead into the I2S FIFO.
 *
 * Buffer choice comes from the DMA's live address registers, not from
 * counting interrupts, so a missed interrupt can't swap the buffers.
 */
#include "old_audio_io.h"
uint32_t sim_cyccnt(void);

#include <stdio.h>
#include <string.h>

#include "board.h"
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

#define CODEC_I2C MXC_I2C1 /* SCL P0_16, SDA P0_17 on the FTHR */
#define CODEC_MCLK 12288000
#define BLOCK_BYTES (AUDIO_BLOCK * sizeof(int16_t))
/* Audio is priority 1 so the BLE radio's short interrupts (priority 0)
 * can still preempt it and keep their timing. */
#define AUDIO_IRQ_PRIO 1

static int16_t rx_buf[2][AUDIO_BLOCK] __attribute__((aligned(4)));
static int16_t tx_buf[2][AUDIO_BLOCK] __attribute__((aligned(4)));

static mxc_i2s_req_t i2s_req;
static audio_process_fn process_fn;
static volatile int rx_ch = -1, tx_ch = -1;
static volatile const int16_t *last_in;
static volatile audio_stats_t stats;
static volatile int running;
static int codec_ready; /* codec configured, muted, and still answering */

#define PMIC_ADDR 0x28 /* MAX20303 (0x50 in max20303.c is the 8-bit form) */
static void codec_off(void);

/* ------------------------------------------------------------------------ */
static void cyccnt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static int in_buf(uint32_t addr, const int16_t *buf)
{
    return (addr - (uint32_t)buf) < BLOCK_BYTES;
}

static void arm_reload(int ch, void *src, void *dst)
{
    mxc_dma_srcdst_t r;
    r.ch = ch;
    r.source = src;
    r.dest = dst;
    r.len = BLOCK_BYTES;
    MXC_DMA_SetSrcReload(r);
}

/* ------------------------------------------------------------------------ */
static void dma_cb(int ch, int err)
{
    (void)err;

    if (ch == tx_ch) {
        /* the buffer the DMA is NOT reading is the one that just finished:
         * queue it to play after the current one */
        int playing = in_buf(MXC_DMA->ch[ch].src, tx_buf[1]);
        arm_reload(ch, tx_buf[!playing], (void *)MXC_DMA->ch[ch].dst);
        return;
    }

    if (ch == rx_ch) {
        uint32_t t0 = sim_cyccnt();
        /* the buffer the DMA is NOT writing is the block that just completed */
        int filling = in_buf(MXC_DMA->ch[ch].dst, rx_buf[1]);
        const int16_t *in = rx_buf[!filling];
        arm_reload(ch, (void *)MXC_DMA->ch[ch].src, (void *)in);

        if (tx_ch < 0) {
            return; /* TX isn't started yet (first block of audio_start) */
        }
        int playing = in_buf(MXC_DMA->ch[tx_ch].src, tx_buf[1]);
        int16_t *out = tx_buf[!playing];

        last_in = in;
        if (process_fn) {
            process_fn(in, out, AUDIO_BLOCK);
        } else {
            memcpy(out, in, BLOCK_BYTES);
        }

        uint32_t dt = sim_cyccnt() - t0;
        stats.cycles_last = dt;
        if (dt > stats.cycles_max) {
            stats.cycles_max = dt;
        }
        /* late = TX already started reading the buffer we just wrote */
        if (in_buf(MXC_DMA->ch[tx_ch].src, out)) {
            stats.late++;
        }
        stats.blocks++;
    }
}

static void dma_irq(void)
{
    MXC_DMA_Handler();
}

/* ------------------------------------------------------------------------ */
int audio_init(audio_process_fn fn)
{
    int err;

    process_fn = fn;
    cyccnt_init();
    stats.budget = (uint32_t)((uint64_t)SystemCoreClock * AUDIO_BLOCK / AUDIO_FS);

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
    uint32_t t0;
    int ch;

    /* the codec has been clocking I2S with no DMA (e.g. during Bluetooth
     * init): drop whatever piled up in the FIFOs */
    MXC_I2S_Flush();

    ch = MXC_I2S_RXDMAConfig(rx_buf[0], BLOCK_BYTES);
    if (ch < 0) {
        return ch;
    }
    rx_ch = ch;
    MXC_NVIC_SetVector(MXC_DMA_CH_GET_IRQ(rx_ch), dma_irq);
    NVIC_SetPriority(MXC_DMA_CH_GET_IRQ(rx_ch), AUDIO_IRQ_PRIO);
    arm_reload(rx_ch, (void *)MXC_DMA->ch[rx_ch].src, rx_buf[1]);

    /* wait until RX is 3/4 through its first block (count is in bytes).
     * If the codec's clocks never start, give up after 50 ms. */
    t0 = sim_cyccnt();
    while ((MXC_DMA->ch[rx_ch].cnt & 0xFFFFFF) > BLOCK_BYTES / 4) {
        if (sim_cyccnt() - t0 > SystemCoreClock / 20) {
            abort_start();
            return E_TIME_OUT;
        }
    }

    ch = MXC_I2S_TXDMAConfig(tx_buf[0], BLOCK_BYTES);
    if (ch < 0) {
        abort_start();
        return ch;
    }
    tx_ch = ch;
    MXC_NVIC_SetVector(MXC_DMA_CH_GET_IRQ(tx_ch), dma_irq);
    NVIC_SetPriority(MXC_DMA_CH_GET_IRQ(tx_ch), AUDIO_IRQ_PRIO);
    arm_reload(tx_ch, tx_buf[1], (void *)MXC_DMA->ch[tx_ch].dst);
    running = 1;
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

int audio_restart(uint8_t hp_volume)
{
    int err = audio_start();
    max9867_set_volume(hp_volume);
    return err;
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

void audio_reset_max(void)
{
    stats.cycles_max = 0;
    stats.late = 0;
}

void audio_copy_last_input(int16_t *dst, int n)
{
    if (n > AUDIO_BLOCK) {
        n = AUDIO_BLOCK;
    }
    __disable_irq();
    const int16_t *src = (const int16_t *)last_in;
    if (src) {
        memcpy(dst, src, n * sizeof(int16_t));
    } else {
        memset(dst, 0, n * sizeof(int16_t));
    }
    __enable_irq();
}
