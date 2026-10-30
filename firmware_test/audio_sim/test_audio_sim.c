/*
 * OpenEar audio I/O test: the firmware's real audio_io.c on the simulated
 * DMA/I2S hardware of sim.c. Built twice: against firmware/audio_io.c (0.3)
 * and against a frozen copy of the 0.2.2 file (OLD), to show the timing
 * fix under identical conditions.
 *
 * What is checked, from the samples the simulated codec actually played:
 *   - every input sample comes out once, in order, with a constant delay
 *     (no stale, repeated or skipped blocks = no "static")
 *   - the late-block counter agrees with what was heard
 *   - stat's slack / interrupt-delay figures (0.3)
 *   - the main-loop watchdog restarts audio after a DMA stall (0.3)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#ifdef OLD
#include "old_audio_io.h"
#else
#include "audio_io.h"
#include "cli.h"
#include "dsp_link.h"
#endif

static int failures;
#define CHECK(c, ...) do { int ok_ = (c); printf(ok_ ? "  PASS  " : "  FAIL  "); if (!ok_) failures++; printf(__VA_ARGS__); printf("\n"); } while (0)

typedef struct {
    long glitches, played, latency;
    uint32_t blocks, late, fixes;
    int slack_min, lag_max;
} result_t;

/* count places where the played stream does not continue 1, 2, 3, ... */
static void analyse(result_t *r)
{
    long i = 0;
    while (i < sim.nplayed && sim.played[i] == 0) i++;
    r->played = sim.nplayed - i;
    r->glitches = 0;
    r->latency = -1;
    for (long j = i + 1; j < sim.nplayed; j++) {
        int prev = sim.played[j - 1], want = prev % 30000 + 1;
        if (sim.played[j] != want) r->glitches++;
    }
    if (i < sim.nplayed) {
        /* sample value v was captured at clock (v - 1) (+ multiples of 30000) */
        r->latency = (sim.play_k0 + i - (sim.played[i] - 1)) % 30000;
    }
}

static uint32_t blocks0, fixes0;
static void stats(result_t *r)
{
    audio_stats_t s;
    audio_get_stats(&s);
    r->blocks = s.blocks - blocks0;
    r->late = s.late;
#ifdef OLD
    r->fixes = 0;
#else
    r->fixes = s.phase_fixes - fixes0;
#endif
#ifdef OLD
    r->slack_min = -1;
    r->lag_max = -1;
#else
    r->slack_min = (int)s.slack_min;
    r->lag_max = (int)s.lag_max;
#endif
}

/* clock slips for the next run() (0.3.0 board: TX lost ~1 sample per 49 ms) */
static long run_tx_slip, run_rx_slip;

static int run(const char *name, int F, double P, int jitter, int ble_every, int ble_hold, double seconds,
               result_t *r)
{
    sim_reset();
    sim.tx_slip_every = run_tx_slip;
    sim.rx_slip_every = run_rx_slip;
    sim.F = F;
    sim.P = P;
    sim.P_jitter = jitter;
    sim.ble_every = ble_every;
    sim.ble_hold_max = ble_hold;
    if (audio_init(sim_process) != 0) {
        printf("  audio_init failed\n");
        return -1;
    }
    int err = audio_start();
    if (err) {
        printf("  audio_start failed %d\n", err);
        return -1;
    }
    audio_stats_t s0;
#ifdef OLD
    audio_reset_max(); /* 0.2.2 resets cycles_max and late */
#else
    audio_reset_max(); /* late, slack, delay, CPU */
#endif
    audio_get_stats(&s0);
    blocks0 = s0.blocks;
#ifndef OLD
    fixes0 = s0.phase_fixes;
#endif
    double end = sim.t + seconds * 48000.0;
    while (sim.t < end) {
        sim_advance(1.0); /* the main loop doing other things */
    }
    analyse(r);
    stats(r);
    printf("  %-44s blocks %6lu  late %5lu (%4.1f%%)  heard glitches %5ld  latency %ld samples",
           name, (unsigned long)r->blocks, (unsigned long)r->late,
           r->blocks ? 100.0 * r->late / r->blocks : 0.0, r->glitches, r->latency);
#ifndef OLD
    printf("  slack>=%d  delay<=%d  phase fixes %u", r->slack_min, r->lag_max, r->fixes);
#endif
    printf("\n");
    if (r->glitches && !r->late && !run_tx_slip && !run_rx_slip) {
        printf("  FAIL  audible glitches that the late counter did not see\n");
        failures++;
    }
    audio_stop();
    return 0;
}

#ifndef OLD
static char cap[8192];
static int caplen;
static void capw(const char *t, int n)
{
    if (caplen + n < (int)sizeof cap - 1) {
        memcpy(cap + caplen, t, n);
        caplen += n;
        cap[caplen] = 0;
    }
}
static oe_dsp_t fake_dsp;
static const oe_dsp_t *gd(void) { return &fake_dsp; }
static void nop_push(const oe_params_t *p) { (void)p; }
static cli_hw_t clihw = { .write = capw, .push_params = nop_push, .dsp = gd, .audio_info = audio_get_info };

static void run_keep(const char *name, int F, double P, int jit, int ble_every, int ble_hold, double seconds)
{
    sim_reset();
    sim.F = F;
    sim.P = P;
    sim.P_jitter = jit;
    sim.ble_every = ble_every;
    sim.ble_hold_max = ble_hold;
    audio_init(sim_process);
    audio_start();
    double end = sim.t + seconds * 48000.0;
    while (sim.t < end) sim_advance(1.0);
    printf("  %s\n", name);
}

/* 'stat' resets the window; the second one covers the next 0.5 s */
/* the firmware's real audio callback (DSP + mailbox), taking P samples */
static void link_process(const int16_t *in, int16_t *out, int n)
{
    dsp_link_process(in, out, n);
    sim_advance(sim.P);
}

static long win_rx, win_tx;
static void stat_twice(void)
{
    oe_params_t p;
    oe_dsp_default_params(&p);
    oe_dsp_init(&fake_dsp, 48000.0f, &p);
    cli_init(&clihw, &p, 16);
    char l1[] = "stat";
    cli_handle_line(l1);
    audio_stats_t a0, a1;
    audio_get_stats(&a0);
    double end = sim.t + 0.5 * 48000.0;
    while (sim.t < end) sim_advance(1.0);
    audio_get_stats(&a1);
    win_rx = (long)(a1.blocks - a0.blocks);
    win_tx = (long)(a1.tx_blocks - a0.tx_blocks);
    caplen = 0;
    cap[0] = 0;
    char l2[] = "stat";
    cli_handle_line(l2);
    printf("%s", cap);
}
#endif

int main(void)
{
    result_t r;
    const int N = AUDIO_BLOCK;
#ifdef OLD
    printf("OpenEar 0.2.2 audio_io.c on the simulated hardware (block %d)\n", N);
    /* 0.2.2 DSP ~= 65 %% of a block; Bluetooth-like hold-offs of up to 0.3 ms */
    run("0.2.2: DSP 65%, F=6, no Bluetooth", 6, 0.65 * N, 0, 0, 0, 2, &r);
    long base_glitch = r.glitches;
    run("0.2.2: DSP 65%, F=6, Bluetooth hold-offs", 6, 0.65 * N, 2, 960, 15, 4, &r);
    CHECK(r.late > 0 && r.glitches > 0,
          "0.2.2 timing with a 65%% DSP and Bluetooth: late blocks and audible glitches (qualitatively as on the board; the model's rates are not calibrated)");
    (void)base_glitch;
    printf("\n%s\n", failures ? "0.2.2 MODEL SHOWED NO LATE BLOCKS" : "0.2.2 MODEL SHOWS LATE BLOCKS AND GLITCHES (as on the board)");
    return failures != 0;
#else
    printf("OpenEar 0.3 audio_io.c on the simulated hardware (block %d)\n", N);
    printf("\n[normal operation: DSP 36%% (cycle bench) + 5%% recorder and interrupt overhead, Bluetooth-like interrupts]\n");
    const int Fs[3] = { 2, 6, 16 };
    for (int f = 0; f < 3; f++) {
        char name[64];
        snprintf(name, sizeof name, "DSP 41%%, FIFO read-ahead F=%d", Fs[f]);
        run(name, Fs[f], 0.41 * N, 2, 960, 15, 4, &r);
        if (Fs[f] <= 6) {
            CHECK(r.late == 0 && r.glitches == 0 && r.played > 3 * 48000,
                  "F=%d: every sample played once, in order (no static), no late blocks", Fs[f]);
        } else {
            /* worst case: the TX DMA keeps the whole FIFO full. The board's
             * no-Bluetooth 'slack' tells F (F ~ N-6-DSP-slack); if it is this
             * big, the TX start can be moved later in a follow-up build. */
            /* the board's boot numbers (slack 38 with a 24-26 sample DSP at
             * start N-2) put its F near 0, so this is a margin check only */
            CHECK(r.late * 200 <= r.blocks && r.played > 3 * 48000,
                  "F=%d (full FIFO, worst case; the board measured F ~ 0): audio keeps running, late %.2f%% (<= 0.5%%)", Fs[f],
                  100.0 * r.late / r.blocks);
        }
        CHECK(r.fixes == 0, "F=%d: no phase fixes when nothing slips (%u)", Fs[f], r.fixes);
        CHECK(r.latency == 2 * N - 6,
              "F=%d: constant delay %ld samples (%.2f ms) = 2N-6, as 'stat' reports", Fs[f], r.latency,
              r.latency / 48.0);
    }
    run("DSP 41%, F=6, no Bluetooth", 6, 0.41 * N, 0, 0, 0, 2, &r);
    int expect = N - 6 - 6 - (int)(0.41 * N);
    CHECK(abs(r.slack_min - expect) <= 1, "slack figure %d samples matches the timing design (N-6-F-DSP = %d)",
          r.slack_min, expect);
    CHECK(r.lag_max <= 1, "interrupt delay %d samples with no Bluetooth", r.lag_max);
    run("DSP 41%, F=6, Bluetooth hold-offs <= 15", 6, 0.41 * N, 0, 960, 15, 4, &r);
    CHECK(r.lag_max >= 8 && r.lag_max <= 17, "interrupt delay shows the Bluetooth hold-offs: %d samples (<= 15 + 2)", r.lag_max);

    printf("\n[the 0.3.0 board: the output side loses ~21 samples a second against the input]\n");
    /* 2026-10-28 board: late 35-38 %% of blocks, slack 0, 'played' falling
     * behind 'blocks' by ~1 block per 3 s. 0.3.0 without the phase lock
     * gave 36 %% late here too. */
    run_tx_slip = 2365;
    run("BOARD: DSP 35-42%, F=6, TX slips every 49 ms", 6, 0.355 * N, 4, 960, 6, 10, &r);
    long slips = sim.tx_slips;
    CHECK(r.late == 0 && r.slack_min >= 8, "output slipping 1 sample / 49 ms: no late blocks (%u), slack >= %d",
          r.late, r.slack_min);
    CHECK(labs((long)r.fixes - slips / 2) <= 3, "phase fixes %u for %ld slipped samples (2 samples each)", r.fixes,
          slips);
    CHECK(r.glitches <= 2 * slips + (long)r.fixes + 4,
          "only the slips themselves and the fixes are heard: %ld glitches for %ld slips + %u fixes (0.3.0 "
          "without the lock: ~5500)", r.glitches, slips, r.fixes);
    run_tx_slip = 0;
    run_rx_slip = 2365;
    run("input slipping instead (other direction)", 6, 0.355 * N, 4, 960, 6, 10, &r);
    slips = sim.rx_slips;
    CHECK(r.late == 0 && labs((long)r.fixes - slips / 2) <= 3,
          "input slipping 1 sample / 49 ms: no late blocks (%u), %u phase fixes for %ld slips", r.late, r.fixes,
          slips);
    run_rx_slip = 0;

    printf("\n[tolerance: longer Bluetooth interrupts (unknown on this board; stat's 'interrupt delay' will tell)]\n");
    const int holds[3] = { 20, 30, 45 };
    for (int h = 0; h < 3; h++) {
        char name[64];
        snprintf(name, sizeof name, "DSP 41%%, F=6, hold-offs <= %d samples", holds[h]);
        run(name, 6, 0.41 * N, 2, 960, holds[h], 4, &r);
    }
    printf("  (informational: the board's numbers decide; at 64 samples a block, "
           "Bluetooth interrupts up to ~%d samples are absorbed)\n", N - 6 - 6 - (int)(0.41 * N));

    printf("\n[stat end to end: the real cli.c 'stat' on the real audio_io.c counters]\n");
    run_keep("DSP 40%, F=6, no Bluetooth", 6, 0.40 * N, 0, 0, 0, 1);
    stat_twice();
    float cpu = 0, peak = 0;
    unsigned long late = 9, win = 0, played = 0, blocks = 0;
    int slack = -1, delay = -1;
    const char *o = cap;
    const char *l2 = strstr(o, "DSP time avg");
    int ok = l2 && sscanf(l2, "DSP time avg %f%%  peak %f%% of real time  late blocks %lu of %lu", &cpu, &peak, &late, &win) == 4;
    const char *l3 = strstr(o, "deadline slack >=");
    ok = ok && l3 && sscanf(l3, "deadline slack >= %d samples (%*f ms)  interrupt delay <= %d", &slack, &delay) == 2;
    const char *l1 = strstr(o, "\nblocks ");
    ok = ok && l1 && sscanf(l1, "\nblocks %lu (played %lu)", &blocks, &played) == 2;
    audio_stats_t st;
    audio_get_stats(&st);
    CHECK(ok, "stat output parses");
    CHECK(fabs(cpu - 40.0f) < 1.5f && peak >= cpu && peak < 42.0f, "stat DSP time avg %.1f%% / peak %.1f%% (simulated DSP: 40%%)", cpu, peak);
    CHECK(late == 0 && labs((long)win - 24000 / N) <= 2, "stat late blocks %lu of %lu = the 0.5 s window since the first stat", late, win);
    CHECK(abs(slack - (N - 6 - 6 - (int)(0.40 * N))) <= 2 && delay <= 2,
          "stat slack %d samples (design %d), interrupt delay %d", slack, N - 6 - 6 - (int)(0.40 * N), delay);
    CHECK(blocks == st.blocks && played == st.tx_blocks && labs(win_rx - win_tx) <= 1,
          "stat blocks %lu / played %lu are the interrupt counters; both grew by %ld / %ld in the window", blocks,
          played, win_rx, win_tx);
    audio_stop();

    printf("\n[one block overruns by more than a block (a pile-up of interrupts)]\n");
    sim_reset();
    sim.F = 6;
    sim.P = 0.41 * N;
    audio_init(sim_process);
    audio_restart(22);
    for (int i = 0; i < 24000; i++) sim_advance(1.0);
    audio_stats_t o0, o1;
    audio_get_stats(&o0);
    sim.one_overrun = 1.5 * N; /* the DSP takes ~1.9 blocks once */
    for (int i = 0; i < 24000; i++) sim_advance(1.0); /* no audio_check: no watchdog help */
    audio_get_stats(&o1);
    CHECK(o1.blocks > o0.blocks + 300 && o1.restarts == o0.restarts,
          "audio carries on by itself (blocks %lu -> %lu): the count-to-zero during the overrun was not lost",
          (unsigned long)o0.blocks, (unsigned long)o1.blocks);
    CHECK(o1.tx_blocks > o0.tx_blocks + 300, "the output stream carries on too (played %lu -> %lu)",
          (unsigned long)o0.tx_blocks, (unsigned long)o1.tx_blocks);
    long tail_glitch = 0;
    for (long j = sim.nplayed - 9600; j < sim.nplayed; j++)
        tail_glitch += sim.played[j] != sim.played[j - 1] % 30000 + 1;
    CHECK(tail_glitch == 0, "and the last 0.2 s is clean again (%ld glitches)", tail_glitch);
    audio_stop();

    printf("\n[parameter hand-over (dsp_link.c): latest wins, even if the interrupt is slow]\n");
    {
        oe_params_t pp;
        oe_dsp_default_params(&pp);
        sim_reset();
        sim.F = 6;
        sim.P = 0.41 * N;
        dsp_link_init(&pp);
        audio_init(link_process);
        audio_restart(22);
        for (int i = 0; i < 4800; i++) sim_advance(1.0);
        pp.master_db = -3.0f;
        dsp_link_push(&pp);
        CHECK(dsp_link_get()->p.master_db == -3.0f, "a change is in the DSP when push returns (state shows it)");
        sim.hold_until = sim.t + 2400; /* audio interrupt held off for 50 ms */
        pp.master_db = -20.0f;
        dsp_link_push(&pp); /* times out after 20 ms: not taken yet */
        pp.master_db = -7.0f;
        pp.tone_on = 0;
        dsp_link_push(&pp); /* must replace it, not be dropped */
        for (int i = 0; i < 4800; i++) sim_advance(1.0);
        CHECK(dsp_link_get()->p.master_db == -7.0f, "after a stalled interrupt the latest change wins (vol %.0f dB)",
              dsp_link_get()->p.master_db);

        /* round-3 review: a change the interrupt never took, then audio
         * stops, then a newer change is applied directly: the next start
         * must not apply the stale one over it (a tone back after 'tone off') */
        sim.hold_until = sim.t + 2400;
        pp.tone_on = 1;
        pp.tone_db = -20.0f;
        dsp_link_push(&pp); /* interrupt held off: still in the mailbox */
        audio_stop();
        pp.tone_on = 0;
        dsp_link_push(&pp); /* audio stopped: applied directly */
        audio_restart(22);
        for (int i = 0; i < 4800; i++) sim_advance(1.0);
        CHECK(audio_running() && dsp_link_get()->p.tone_on == 0,
              "a stale change is not applied over a newer one after a restart (tone %s)",
              dsp_link_get()->p.tone_on ? "ON" : "off");
        audio_stop();
    }

    printf("\n[a 'save' pauses audio for a 50 ms flash write]\n");
    sim_reset();
    sim.F = 6;
    sim.P = 0.41 * N;
    audio_init(sim_process);
    audio_restart(22);
    audio_stats_t v0, v1;
    for (int i = 0; i < 9600; i++) { sim_advance(1.0); audio_check((uint32_t)(sim.t / 48.0)); }
    audio_get_stats(&v0);
    audio_stop();
    /* the flash write blocks the main loop: no audio_check() meanwhile */
    for (int i = 0; i < 2400; i++) sim_advance(1.0);
    audio_restart(22);
    for (int i = 0; i < 9600; i++) { sim_advance(1.0); audio_check((uint32_t)(sim.t / 48.0)); }
    audio_get_stats(&v1);
    CHECK(v1.restarts == v0.restarts && v1.blocks > v0.blocks + 100,
          "no spurious watchdog restart after the pause (restarts %lu -> %lu)", (unsigned long)v0.restarts,
          (unsigned long)v1.restarts);
    audio_stop();

    printf("\n[input stops and nothing restarts it (no timer: watchdog off)]\n");
    sim_reset();
    sim.F = 6;
    sim.P = 0.41 * N;
    audio_init(sim_process);
    audio_restart(22);
    for (int i = 0; i < 9600; i++) sim_advance(1.0);
    sim_kill_rx();
    long k0 = sim.nplayed;
    for (int i = 0; i < 9600; i++) sim_advance(1.0);
    long loud = 0;
    for (long j = k0 + 5 * N; j < sim.nplayed; j++) loud += sim.played[j] != 0;
    CHECK(loud == 0, "after 5 blocks the codec plays silence, not the last blocks on a loop (%ld non-zero)", loud);
    audio_stop();

    printf("\n[30 audio starts while Bluetooth interrupts steal time from the main loop]\n");
    int bad_starts = 0, worst = N;
    for (int k = 0; k < 30; k++) {
        sim_reset();
        sim.F = 6;
        sim.P = 0.41 * N;
        sim.ble_every = 120; /* busy radio: often in the start window */
        sim.ble_hold_max = 15;
        for (int w = 0; w < k * 7; w++) sim_advance(1.0); /* a different phase each time */
        audio_init(sim_process);
        audio_restart(22);
        audio_reset_max();
        sim.ble_every = 0; /* then quiet, so only the start phase matters */
        for (int i = 0; i < 9600; i++) sim_advance(1.0);
        audio_stats_t z;
        audio_get_stats(&z);
        /* worst designed start: the fallback window at N-12 (slack N-12-F-DSP),
         * less 2 samples of DMA granularity */
        if (z.late || (int)z.slack_min < N - 12 - 6 - (int)(0.41 * N) - 2) bad_starts++;
        if ((int)z.slack_min < worst) worst = (int)z.slack_min;
        audio_stop();
    }
    CHECK(bad_starts == 0, "every start landed at the designed phase (worst slack %d samples, %d bad)", worst, bad_starts);
    CHECK(sim.unmute_without_audio == 0, "the codec was never unmuted while the output stream was off (%d times)",
          sim.unmute_without_audio);

    printf("\n[headroom: the 0.2.2 DSP cost (65%%) with the 0.3 timing]\n");
    run("DSP 65%, F=6, Bluetooth hold-offs", 6, 0.65 * N, 2, 960, 15, 4, &r);
    printf("  (informational: with the old DSP cost the new timing leaves %.2f%% late; the faster DSP is needed too)\n",
           100.0 * r.late / r.blocks);

    printf("\n[overload: DSP longer than the deadline]\n");
    run("DSP 95%, F=6", 6, 0.95 * N, 0, 0, 0, 1, &r);
    CHECK(r.late > r.blocks / 2 && r.slack_min == 0,
          "late blocks are counted (%lu of %lu) and slack reads 0", (unsigned long)r.late,
          (unsigned long)r.blocks);

    printf("\n[watchdog: the RX DMA stalls, the main loop restarts audio]\n");
    sim_reset();
    sim.F = 6;
    sim.P = 0.36 * N;
    CHECK(audio_init(sim_process) == 0 && audio_codec_ready(), "audio_init: codec ready");
    CHECK(audio_restart(22) == 0 && sim.volume == 22, "audio_restart starts audio, then unmutes at 22");
    for (int i = 0; i < 48000; i++) {
        sim_advance(1.0);
        audio_check((uint32_t)(sim.t / 48.0));
    }
    audio_stats_t s0, s1;
    audio_get_stats(&s0);
    sim_kill_rx(); /* a missed DMA reload: the RX channel stops */
    long p0 = sim.nplayed;
    for (int i = 0; i < 1200; i++) { /* 25 ms: not yet */
        sim_advance(1.0);
        audio_check((uint32_t)(sim.t / 48.0));
    }
    audio_get_stats(&s1);
    CHECK(s1.restarts == 0 && s1.blocks == s0.blocks, "audio stopped; no restart within 25 ms");
    (void)p0;
    int restarted = 0;
    double t_kill = sim.t - 1200;
    for (int i = 0; i < 4800 && !restarted; i++) {
        sim_advance(1.0);
        restarted = audio_check((uint32_t)(sim.t / 48.0));
    }
    double ms = (sim.t - t_kill) / 48.0;
    CHECK(ms >= 29 && ms <= 40, "the restart came %.1f ms after the stall (design: 30 ms)", ms);
    for (int i = 0; i < 9600; i++) {
        sim_advance(1.0);
        audio_check((uint32_t)(sim.t / 48.0));
    }
    audio_get_stats(&s1);
    CHECK(restarted == 1 && s1.restarts == 1 && s1.blocks > s0.blocks + 100 && sim.volume == 22 && audio_running(),
          "RX stall: restarted once within ~30 ms, audio flows again at the same volume (blocks %lu -> %lu)",
          (unsigned long)s0.blocks, (unsigned long)s1.blocks);
    /* now the TX side stops (RX keeps counting blocks) */
    audio_get_stats(&s0);
    sim_kill_tx();
    restarted = 0;
    for (int i = 0; i < 4800 && !restarted; i++) {
        sim_advance(1.0);
        restarted = audio_check((uint32_t)(sim.t / 48.0));
    }
    for (int i = 0; i < 9600; i++) {
        sim_advance(1.0);
        audio_check((uint32_t)(sim.t / 48.0));
    }
    audio_get_stats(&s1);
    CHECK(restarted == 1 && s1.restarts == 2 && s1.tx_blocks > s0.tx_blocks + 100 && sim.volume == 22,
          "TX stall: also restarted (played blocks %lu -> %lu)", (unsigned long)s0.tx_blocks,
          (unsigned long)s1.tx_blocks);
    /* volume while audio is stopped is remembered, never written */
    audio_stop();
    int v_before = sim.volume;
    audio_set_volume(4);
    CHECK(sim.volume == v_before && v_before == 0x40, "with audio stopped, a volume change does not unmute the codec");
    CHECK(audio_restart(4) == 0 && sim.volume == 4, "audio_restart unmutes only after starting");
    audio_stop();
    /* the codec's clocks never start: audio_start gives up, frees its DMA
     * channels, and the codec stays muted */
    sim.rx_stalled = 1;
    int e = audio_restart(30);
    CHECK(e != 0 && sim.volume == 0x40 && !audio_running(), "no codec clock: start times out (%d), codec muted", e);
    sim.rx_stalled = 0;
    CHECK(audio_restart(30) == 0 && sim.volume == 30 && audio_running(),
          "after the timeout the DMA channels were released: the next start works");
    for (int i = 0; i < 4800; i++) sim_advance(1.0);
    audio_get_stats(&s1);
    audio_stop();
    CHECK(sim.volume == 0x40, "audio_stop mutes the codec");

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL AUDIO I/O SIMULATION TESTS PASSED", failures);
    return failures != 0;
#endif
}
