/*
 * OpenEar audio I/O simulator (PC). A sample-accurate model of the parts of
 * the MAX32655 that firmware/audio_io.c drives:
 *   - RX DMA: writes one mic sample per sample period into its buffer,
 *     reloads from the reload registers at count-to-zero (CTZ) if RLDEN is
 *     set, otherwise stops; raises its interrupt flag at CTZ.
 *   - TX DMA: keeps the I2S TX FIFO F samples ahead of the codec (the DMA
 *     "reads ahead"); same reload/CTZ behaviour.
 *   - Interrupts: one level (audio), masked by __disable_irq; can be held off
 *     for a while to model a higher-priority Bluetooth interrupt. The SDK's
 *     MXC_DMA_Handler is modelled exactly: callback first, then the channel's
 *     flags are cleared (a CTZ during the callback is lost, as on the chip).
 * Time advances only through the CPU-side calls the firmware makes (timer
 * reads, delays) and through the test driver, so busy-wait loops behave as
 * they would on the board. Time unit: one sample period (1/48 kHz).
 */
#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dma.h"
#include "i2s.h"

mxc_dma_regs_t sim_dma;
sim_dwt_t sim_dwt;
sim_coredebug_t sim_coredebug;
uint32_t SystemCoreClock = 100000000u;
mxc_i2c_regs_t sim_i2c1;
sim_cfg_t sim;

#define CTRL_EN 1u
#define RLDEN 0x80000000u
#define FIFO_ADDR 0x40060040u

static double frac;
static int irq_on = 1, in_isr;
static int prio[8];             /* NVIC priority per DMA channel IRQ */
static int cur_prio = 1000;     /* priority of what the CPU runs now (1000 = main) */
static int nvic_en[8];
static void (*vec[8])(void);
static int pend[8];
static int used[8];
static void (*dma_cb)(int, int);
static int rx_ch = -1, tx_ch = -1;
static int16_t fifo[256];
static int fifo_n, fifo_head;
static uint32_t rnd = 12345;

static uint32_t rand32(void)
{
    rnd = rnd * 1664525u + 1013904223u;
    return rnd >> 8;
}

void sim_reset(void)
{
    memset(&sim_dma, 0, sizeof sim_dma);
    frac = 0;
    irq_on = 1;
    in_isr = 0;
    memset(nvic_en, 0, sizeof nvic_en);
    memset(vec, 0, sizeof vec);
    memset(pend, 0, sizeof pend);
    memset(prio, 0, sizeof prio);
    cur_prio = 1000;
    memset(used, 0, sizeof used);
    rx_ch = tx_ch = -1;
    fifo_n = fifo_head = 0;
    rnd = 12345;
    sim.t = 0;
    sim.k = 0;
    sim.nplayed = 0;
    sim.hold_until = 0;
    sim.preempt_debt = 0;
    sim.main_debt = 0;
    sim.deferred_ble = 0;
    sim.rx_stalled = 0;
    sim.volume = -1;
    sim.tx_slip_every = sim.rx_slip_every = 0;
    sim.tx_slips = sim.rx_slips = 0;
    /* unmute_without_audio is kept across resets: a global safety tally */
}

static int16_t in_sample(long k)
{
    return (int16_t)(k % 30000 + 1); /* never 0: 0 means "nothing written yet" */
}

static void ctz(int ch)
{
    mxc_dma_ch_regs_t *c = &sim_dma.ch[ch];
    if (c->cntrld & RLDEN) {
        c->src = c->srcrld;
        c->dst = c->dstrld;
        c->cnt = c->cntrld & ~RLDEN;
        c->cntrld &= ~RLDEN;
    } else {
        c->ctrl &= ~CTRL_EN; /* no reload armed: the channel stops */
    }
    pend[ch] = 1;
}

static void tx_fill(void)
{
    mxc_dma_ch_regs_t *c = &sim_dma.ch[tx_ch];
    while ((c->ctrl & CTRL_EN) && fifo_n < sim.F) {
        int16_t v = *(int16_t *)(uintptr_t)c->src;
        fifo[(fifo_head + fifo_n) % 256] = v;
        fifo_n++;
        c->src += 2;
        c->cnt -= 2;
        if (c->cnt == 0) {
            ctz(tx_ch);
        }
    }
}

/* one sample period of hardware */
static void tick(void)
{
    int rx_slip = sim.rx_slip_every && sim.k % sim.rx_slip_every == sim.rx_slip_every - 1;
    if (rx_slip && rx_ch >= 0) {
        sim.rx_slips++;
    }
    if (rx_ch >= 0 && (sim_dma.ch[rx_ch].ctrl & CTRL_EN) && !sim.rx_stalled && !rx_slip) {
        mxc_dma_ch_regs_t *c = &sim_dma.ch[rx_ch];
        *(int16_t *)(uintptr_t)c->dst = in_sample(sim.k);
        c->dst += 2;
        c->cnt -= 2;
        if (c->cnt == 0) {
            ctz(rx_ch);
        }
    }
    if (tx_ch >= 0) {
        int16_t out = 0;
        int tx_slip = sim.tx_slip_every && sim.k % sim.tx_slip_every == sim.tx_slip_every - 1;
        if (tx_slip && fifo_n) {
            sim.tx_slips++; /* this frame goes out empty; the FIFO is not read */
        } else if (fifo_n) {
            out = fifo[fifo_head];
            fifo_head = (fifo_head + 1) % 256;
            fifo_n--;
        } else if (sim_dma.ch[tx_ch].ctrl & CTRL_EN) {
            sim.underruns++;
        }
        if (sim.nplayed == 0) {
            sim.play_k0 = sim.k; /* clock of played[0] */
        }
        if (sim.nplayed < SIM_MAX_PLAY) {
            sim.played[sim.nplayed++] = out;
        }
        tx_fill();
    }
    sim.k++;
    /* a Bluetooth-like interrupt (higher priority): holds the audio
     * interrupt off, or, if the audio interrupt is running, preempts it so
     * the DSP takes that much longer */
    if (sim.ble_every && rand32() % sim.ble_every == 0) {
        double len = 1 + rand32() % (sim.ble_hold_max + 1);
        if (in_isr) {
            sim.preempt_debt += len;
        } else {
            if (sim.t + len > sim.hold_until) {
                sim.hold_until = sim.t + len;
            }
            if (irq_on) {
                sim.main_debt += len;     /* it also stops the main code */
            } else {
                sim.deferred_ble += len;  /* masked: runs at __enable_irq */
            }
        }
    }
}

/* Cortex-M4 NVIC: the most urgent pending, enabled interrupt runs if it is
 * more urgent (lower number) than what is running now; it can nest. */
static void service(void)
{
    if (!irq_on || sim.t < sim.hold_until) {
        return;
    }
    for (int guard = 0; guard < 64; guard++) {
        int ch = -1;
        for (int i = 0; i < 8; i++) {
            if (pend[i] && nvic_en[i] && vec[i] && prio[i] < cur_prio && (ch < 0 || prio[i] < prio[ch])) {
                ch = i;
            }
        }
        if (ch < 0) {
            return;
        }
        int saved = cur_prio;
        cur_prio = prio[ch];
        in_isr++;
        vec[ch]();
        in_isr--;
        cur_prio = saved;
    }
}

void sim_advance(double samples)
{
    frac += samples;
    while (frac >= 1.0) {
        frac -= 1.0;
        sim.t += 1.0;
        tick();
        service(); /* may nest a more urgent interrupt into a running one */
    }
    service();
}

/* a missed reload: the RX channel stops (what happens on the chip when its
 * interrupt is held off for more than a block) */
void sim_kill_rx(void)
{
    if (rx_ch >= 0) {
        sim_dma.ch[rx_ch].ctrl &= ~CTRL_EN;
    }
}

void sim_kill_tx(void)
{
    if (tx_ch >= 0) {
        sim_dma.ch[tx_ch].ctrl &= ~CTRL_EN;
    }
}

/* ---- CPU and SDK stubs ---- */
void __disable_irq(void) { irq_on = 0; }
void __enable_irq(void)
{
    irq_on = 1;
    sim.main_debt += sim.deferred_ble;
    sim.deferred_ble = 0;
    service();
}

/* main code reading a register: first pay any time a Bluetooth interrupt took */
static void main_read(void);
static void main_read_impl(void)
{
    if (!in_isr && irq_on && sim.main_debt > 0) {
        double d = sim.main_debt;
        sim.main_debt = 0;
        sim_advance(d);
    }
    sim_advance(0.01);
}

static void main_read(void) { main_read_impl(); }

uint32_t sim_dma_status(int ch)
{
    uint32_t st = (sim_dma.ch[ch].ctrl & CTRL_EN) ? MXC_F_DMA_STATUS_STATUS : 0;
    if (pend[ch]) {
        st |= MXC_F_DMA_STATUS_CTZ_IF | MXC_F_DMA_STATUS_IPEND;
    }
    return st;
}

void sim_dma_w1c(int ch, uint32_t f)
{
    if (f & MXC_F_DMA_STATUS_CTZ_IF) {
        pend[ch] = 0; /* only the bits written are cleared */
    }
}

uint32_t sim_dma_cnt(int ch)
{
    main_read();
    return sim_dma.ch[ch].cnt;
}
void NVIC_DisableIRQ(IRQn_Type n) { nvic_en[n - 100] = 0; }
void NVIC_SetPriority(IRQn_Type n, uint32_t p) { prio[n - 100] = (int)p; }
void MXC_NVIC_SetVector(IRQn_Type n, void (*h)(void))
{
    vec[n - 100] = h;
    nvic_en[n - 100] = 1;
}

void MXC_DMA_Handler(void)
{
    for (int i = 0; i < 8; i++) {
        if (pend[i]) {
            if (dma_cb) {
                dma_cb(i, 0);
            }
            pend[i] = 0; /* as the SDK: flags cleared after the callback */
        }
    }
}

/* the status register as the 0.3 handler uses it: count-to-zero flag */
int MXC_DMA_ChannelGetFlags(int ch)
{
    return pend[ch] ? (int)MXC_F_DMA_STATUS_CTZ_IF : 0;
}

int MXC_DMA_ChannelClearFlags(int ch, int flags)
{
    if (flags & MXC_F_DMA_STATUS_CTZ_IF) {
        pend[ch] = 0;
    }
    return 0;
}

int MXC_DMA_SetSrcReload(mxc_dma_srcdst_t s)
{
    mxc_dma_ch_regs_t *c = &sim_dma.ch[s.ch];
    c->srcrld = (uint32_t)(uintptr_t)s.source;
    c->dstrld = (uint32_t)(uintptr_t)s.dest;
    c->cntrld = (c->ctrl & CTRL_EN) ? (RLDEN | (uint32_t)s.len) : (uint32_t)s.len;
    return 0;
}

int MXC_DMA_Stop(int ch)
{
    sim_dma.ch[ch].ctrl &= ~CTRL_EN;
    return 0;
}

int MXC_DMA_ReleaseChannel(int ch)
{
    used[ch] = 0;
    pend[ch] = 0;
    if (ch == rx_ch) rx_ch = -1;
    if (ch == tx_ch) {
        tx_ch = -1;
        fifo_n = 0;
    }
    return 0;
}

static int acquire(void)
{
    for (int i = 0; i < 4; i++) { /* the MAX32655 has 4 DMA channels (max32655.h) */
        if (!used[i]) {
            used[i] = 1;
            return i;
        }
    }
    return -1;
}

int MXC_I2S_Init(mxc_i2s_req_t *r) { (void)r; return 0; }
int MXC_I2S_CalculateClockDiv(int fs, int w) { (void)fs; (void)w; return 0; }
void MXC_I2S_RegisterDMACallback(void (*cb)(int, int)) { dma_cb = cb; }
void MXC_I2S_RXDisable(void) {}
void MXC_I2S_TXDisable(void) {}
void MXC_I2S_Flush(void) { fifo_n = 0; }

int MXC_I2S_RXDMAConfig(void *dst, int len)
{
    int ch = acquire();
    if (ch < 0) return -1;
    mxc_dma_ch_regs_t *c = &sim_dma.ch[ch];
    memset(c, 0, sizeof *c);
    c->src = FIFO_ADDR;
    c->dst = (uint32_t)(uintptr_t)dst;
    c->cnt = (uint32_t)len;
    c->ctrl = CTRL_EN;
    rx_ch = ch;
    return ch;
}

int MXC_I2S_TXDMAConfig(void *src, int len)
{
    main_read(); /* configuring the channel takes a few microseconds */
    int ch = acquire();
    if (ch < 0) return -1;
    mxc_dma_ch_regs_t *c = &sim_dma.ch[ch];
    memset(c, 0, sizeof *c);
    c->src = (uint32_t)(uintptr_t)src;
    c->dst = FIFO_ADDR;
    c->cnt = (uint32_t)len;
    c->ctrl = CTRL_EN;
    tx_ch = ch;
    fifo_n = fifo_head = 0;
    tx_fill(); /* the DMA fills the empty FIFO straight away */
    return ch;
}

int MXC_Delay(uint32_t us)
{
    sim_advance(us * 0.048);
    return 0;
}

/* board / codec / bus: all fine */
int Microphone_Power(int on) { (void)on; return 0; }
int bus_init(void) { return 0; }
int bus_recover(void) { return 0; }
int bus_ready_for_pmic(void) { return 1; }
void bus_report(const char *t) { (void)t; }
int i2c_probe(int a) { (void)a; return 1; }
int max9867_init(mxc_i2c_regs_t *i, int m, int l) { (void)i; (void)m; (void)l; return 0; }
int max9867_shutdown(void) { return 0; }
int max9867_set_volume(uint8_t c)
{
    sim.volume = c;
    if (c != 0x40 && !(tx_ch >= 0 && (sim_dma.ch[tx_ch].ctrl & CTRL_EN))) {
        sim.unmute_without_audio++; /* the codec unmuted with no output stream */
    }
    return 0;
}
int max9867_read_reg(uint8_t r, uint8_t *v) { (void)r; *v = 0x42; return 0; }

/* OpenEar 0.3 time base, driven by the simulated clock (50 MHz timer) */
uint32_t oe_ticks(void)
{
    main_read(); /* reading the timer takes a moment */
    return (uint32_t)(sim.t * (50e6 / 48000.0));
}
uint32_t oe_ticks_per_ms(void) { return 50000; }
uint32_t oe_millis(void) { return (uint32_t)(sim.t / 48.0); }
int oe_time_ok(void) { return 1; }
int oe_dwt_counts(void) { return 1; }

/* 0.2.x read the DWT directly; its copy is built with DWT->CYCCNT -> this */
uint32_t sim_cyccnt(void)
{
    sim_advance(0.02);
    return (uint32_t)(sim.t * (100e6 / 48000.0));
}

/* the DSP: takes P (+ jitter) sample periods, and like the firmware's
 * process_block() writes the output samples only at the end */
void sim_process(const int16_t *in, int16_t *out, int n)
{
    static int16_t copy[1024];
    for (int i = 0; i < n; i++) copy[i] = in[i]; /* audio_to_float() */
    double p = sim.P + (sim.P_jitter ? (rand32() % (sim.P_jitter + 1)) : 0);
    p += sim.one_overrun; /* test hook: stretch one block */
    sim.one_overrun = 0;
    sim_advance(p * 0.95);
    while (sim.preempt_debt > 0) { /* time stolen by Bluetooth interrupts */
        double d = sim.preempt_debt;
        sim.preempt_debt = 0;
        sim_advance(d);
    }
    for (int i = 0; i < n; i++) out[i] = copy[i]; /* audio_from_float() */
    sim_advance(p * 0.05);
}
