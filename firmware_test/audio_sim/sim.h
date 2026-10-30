/* OpenEar audio I/O simulator - see sim.c */
#ifndef SIM_H
#define SIM_H
#include <stdint.h>
#include "mxc_device.h"
#define SIM_MAX_PLAY 2000000
typedef struct {
    double t;            /* now, in sample periods */
    long k;              /* samples clocked so far */
    int F;               /* TX DMA read-ahead into the I2S FIFO (samples) */
    double P;            /* DSP time per block (samples) */
    int P_jitter;        /* + up to this many samples, random */
    int ble_every;       /* a holding interrupt on average every N samples (0 = none) */
    int ble_hold_max;    /* holding it for up to this many samples */
    double hold_until;
    double one_overrun;  /* add this much to the next DSP block only */
    double preempt_debt;
    double main_debt;    /* Bluetooth time taken from the main code */
    double deferred_ble; /* Bluetooth held off by __disable_irq */
    int unmute_without_audio; /* Bluetooth time taken from a running audio interrupt */
    int rx_stalled;      /* freeze the RX DMA (watchdog test) */
    int volume;          /* last codec volume written */
    long underruns;
    long tx_slip_every;  /* the TX FIFO misses one frame every N samples (0 = never) */
    long rx_slip_every;  /* the RX side loses one sample every N samples (0 = never) */
    long tx_slips, rx_slips;
    long nplayed;
    long play_k0;        /* sample clock when played[0] came out */
    int16_t played[SIM_MAX_PLAY];
} sim_cfg_t;
extern sim_cfg_t sim;
void sim_reset(void);
void sim_advance(double samples);
void sim_kill_rx(void);
void sim_kill_tx(void);
void sim_process(const int16_t *in, int16_t *out, int n);
#endif
