/*
 * OpenEar - open-source, self-tunable assistive hearing device
 * Firmware for the Analog Devices MAX32655FTHR.
 *
 * Mic -> MAX9867 codec -> I2S/DMA -> 6-band WDRC DSP (Cortex-M4F) ->
 * I2S/DMA -> MAX9867 -> earphone.
 *
 * Control: USB serial console (115200 8N1) or Bluetooth LE ("OpenEar"),
 * same commands on both - type "help".
 *   SW2 (P0.2): processing on/off - instant A/B compare
 *   SW3 (P0.3): volume step, cycles -12 / -6 / 0 dB
 *   (SW1 is the PMIC power button; the firmware doesn't use it.)
 * LEDs: green = processing (blinks in bypass), red = howl guard / error.
 * The blue LED is left off: its pin (P0.26) may double as the QSPI flash's
 * chip select on the SPI bus the SD card uses.
 *
 * Build options (make OE_BLE=0 / OE_REC=0) drop Bluetooth or the SD
 * recorder - useful for isolating problems on day 1.
 *
 * NOT A MEDICAL DEVICE. Start quiet; use closed in-ear earphones.
 * Licence: MIT (max9867.c/.h and the BLE stack setup are ADI's, Apache-2.0).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "board.h"
#include "cli.h"
#include "codec_bus.h"
#include "dsp_link.h"
#include "fuelgauge.h"
#include "icc.h"
#include "led.h"
#include "max9867.h"
#include "mxc_device.h"
#include "mxc_errors.h"
#include "mxc_sys.h"
#include "oe_time.h"
#include "openear_dsp.h"
#include "pb.h"
#include "profile.h"
#include "uart.h"
#include "wdt.h"
#if OE_BLE
#include "oe_ble.h"
#endif
#if OE_REC
#include "recorder.h"
#endif

extern mxc_uart_regs_t *ConsoleUart; /* defined in the board support package */

#define DEFAULT_HPVOL 16 /* start quieter than ADI's example (4) */

/* ------------------------------------------------------------------------ */
/* hardware hooks for the console                                           */
/* ------------------------------------------------------------------------ */
#if OE_BLE
static int capture_ble; /* console output also goes to the phone */
#endif

static void out_write(const char *s, int len)
{
    for (int i = 0; i < len; i++) {
        putchar(s[i]);
    }
    fflush(stdout);
#if OE_BLE
    if (capture_ble) {
        oe_ble_write(s, len);
    }
#endif
}

static const oe_dsp_t *get_dsp(void)
{
    return dsp_link_get();
}

static void audio_info(cli_audio_info_t *a, int reset)
{
    audio_get_info(a, reset);
}

static int hw_raw(int16_t *b, int n)
{
    audio_copy_last_input(b, n);
    return 0;
}

static int hw_save(const oe_params_t *p, uint8_t hp)
{
    profile_t pr = { .params = *p, .hp_volume = hp };
    /* a flash erase stalls the CPU for tens of ms: stop audio cleanly
     * (codec muted) and restart it afterwards */
#if OE_REC
    if (rec_active()) {
        rec_stop();
    }
#endif
    int was_running = audio_running();
    audio_stop();
    int err = profile_save(&pr);
    if (was_running) {
        int err2 = audio_restart(hp);
        if (err2) {
            printf("\n[warn] audio restart failed: %d\n", err2);
        }
    }
    return err;
}

/* A stored volume code is checked like a typed one: never louder than the
 * console allows (4), never past mute (0x3F is the quietest audible step). */
static uint8_t safe_hpvol(uint8_t v)
{
    return v < 4 ? 4 : (v > 63 ? 63 : v);
}

static int hw_load(oe_params_t *p, uint8_t *hp)
{
    profile_t pr;
    if (profile_load(&pr)) {
        return -1;
    }
    *p = pr.params;
    *hp = safe_hpvol(pr.hp_volume);
    return 0;
}

/* The codec is only touched from the console once audio_init() left it
 * configured and muted: if init failed half-way, its state (volume!) is
 * unknown, so sidetone and hpvol are refused rather than guessed. */
static int hw_set_hpvol(uint8_t code)
{
    if (!audio_codec_ready()) {
        printf("\n[codec] not ready (audio init failed), volume not changed\n");
        return E_BAD_STATE;
    }
    return audio_set_volume(code);
}

static int hw_sidetone(int on, uint8_t gain)
{
    if (on && !audio_codec_ready()) {
        printf("\n[codec] not ready (audio init failed), sidetone refused\n");
        return E_BAD_STATE;
    }
    if (!audio_codec_ready()) {
        return E_NO_ERROR; /* "off" with no codec: nothing to switch off */
    }
    return max9867_sidetone(on, gain);
}

static int hw_audio_restart(void)
{
    if (!audio_codec_ready()) {
        return E_BAD_STATE; /* the codec never initialised: reset the board */
    }
    if (audio_running()) {
        audio_stop();
    }
    return audio_restart(cli_hpvol());
}

#if OE_REC
/* recording needs the audio interrupt: without it the file never fills */
static int hw_rec_start(int seconds, char *name, int name_len)
{
    if (!audio_running()) {
        return -20;
    }
    return rec_start(seconds, name, name_len);
}
#endif

static const cli_hw_t hw = {
    .write = out_write,
    .push_params = dsp_link_push,
    .dsp = get_dsp,
    .audio_info = audio_info,
    .set_hpvol = hw_set_hpvol,
    .sidetone = hw_sidetone,
    .raw = hw_raw,
    .batt = fuelgauge_read,
    .i2c_probe = i2c_probe,
    .save = hw_save,
    .load = hw_load,
#if OE_REC
    .rec_start = hw_rec_start,
    .rec_stop = rec_stop,
    .rec_status = rec_status,
#endif
#if OE_BLE
    .ble_status = oe_ble_status,
#endif
    .audio_running = audio_running,
    .audio_restart = hw_audio_restart,
};

/* ------------------------------------------------------------------------ */
/* inputs                                                                   */
/* ------------------------------------------------------------------------ */
#if OE_BLE
/* lines from the phone, queued by the BLE stack and run by the main loop
 * (one dispatcher pass can deliver several writes) */
#define BLEQ 8
static char bleq[BLEQ][128];
static uint32_t bleq_head, bleq_tail;

void oe_ble_on_line(char *line)
{
    if (bleq_head - bleq_tail < BLEQ) {
        snprintf(bleq[bleq_head % BLEQ], sizeof(bleq[0]), "%s", line);
        bleq_head++;
    }
}
#endif

static void console_poll(void)
{
    static char line[128];
    static int len;
    while (MXC_UART_GetRXFIFOAvailable(ConsoleUart) > 0) {
        int c = MXC_UART_ReadCharacterRaw(ConsoleUart);
        if (c < 0) {
            break;
        }
        if (c == '\r' || c == '\n') {
            if (len) {
                printf("\n");
                line[len] = 0;
                len = 0;
                cli_handle_line(line);
                printf("> ");
            }
        } else if ((c == 8 || c == 127) && len) {
            len--;
            printf("\b \b");
        } else if (c >= 32 && len < (int)sizeof(line) - 1) {
            line[len++] = (char)c;
            putchar(c); /* echo */
        }
        fflush(stdout);
    }
}

/* Main-loop clock: the hardware timer, or, if it isn't running, the audio
 * block count (1.33 ms per block), so the tone auto-stop still works. */
static uint32_t now_ms(void)
{
    if (oe_time_ok()) {
        return oe_millis();
    }
    audio_stats_t s;
    audio_get_stats(&s);
    return (uint32_t)((uint64_t)s.blocks * AUDIO_BLOCK * 1000u / AUDIO_FS);
}

/* ------------------------------------------------------------------------ */
int main(void)
{
    /* first: a watchdog left running by the reset it caused must not fire
     * again during the slow boot steps below (it is re-enabled at the end) */
    mxc_wdt_cfg_t wcfg = { 0 };
    wcfg.mode = MXC_WDT_COMPATIBILITY;
    wcfg.upperResetPeriod = MXC_WDT_PERIOD_2_28; /* 2^28 / 50 MHz = 5.4 s */
    MXC_WDT_Init(MXC_WDT0, &wcfg);
    MXC_WDT_Disable(MXC_WDT0);

    MXC_ICC_Enable(MXC_ICC0);
    MXC_SYS_Clock_Select(MXC_SYS_CLOCK_IPO); /* 100 MHz */
    SystemCoreClockUpdate();
    Console_Init();
    LED_Off(LED_RED);
    LED_Off(LED_GREEN);

    printf("\n\n=== OpenEar " OE_FW_VERSION " - open assistive hearing on MAX32655 ===\n");
    printf("Not a medical device. Start quiet; use closed in-ear earphones.\n");
    printf("build: BLE %s, SD recorder %s, block %d\n", OE_BLE ? "on" : "off",
           OE_REC ? "on" : "off", AUDIO_BLOCK);

    /* the watchdog's flag survives its reset: say so if it fired last time */
    if (MXC_WDT_GetResetFlag(MXC_WDT0)) {
        printf("NOTE: the last reset was the watchdog (the main loop had stalled for ~5 s)\n");
        MXC_WDT_ClearResetFlag(MXC_WDT0);
    }
    int terr = oe_time_init();
    printf("clock: TMR2 %s (%d), DWT cycle counter %s\n",
           oe_time_ok() ? "ok" : "NOT RUNNING (timing falls back to audio blocks)", terr,
           oe_dwt_counts() ? "counting" : "not counting (not used)");

    oe_params_t p;
    uint8_t hpvol = DEFAULT_HPVOL;
    oe_dsp_default_params(&p);
    profile_t prof;
    if (profile_load(&prof) == 0) {
        p = prof.params;
        hpvol = safe_hpvol(prof.hp_volume);
        p.tone_on = 0;
        printf("loaded saved profile\n");
    } else {
        printf("no saved profile: flat pass-through (all bands 0 dB)\n");
    }
    dsp_link_init(&p);
    cli_init(&hw, &p, hpvol);

    /* Evidence before anything touches I2C1: both lines should read 1 (the
     * board's pull-ups). A 0 here means a chip or a short holds the line. */
    bus_report("at boot, before init");

    /* Audio first, the way ADI's working I2S example does it (no Bluetooth
     * yet), then Bluetooth, then start the audio streams. If audio can't
     * start, keep the console (and Bluetooth) alive: 'i2cscan', 'batt' and
     * 'help' are exactly the tools needed to find out why. */
    int err = audio_init(dsp_link_process);
    if (err) {
        bus_init(); /* make sure i2cscan has a bus to scan */
        printf("audio init failed: %d (%s)\n", err, err_name(err));
    }

#if OE_BLE
    /* its init toggles the 32 MHz crystal and takes ~0.5 s; the codec stays
     * muted meanwhile */
    printf("starting bluetooth...\n");
    oe_ble_init();
#endif

    /* start the streams with the codec still muted, then unmute: the codec
     * is never unmuted without audio (audio_restart mutes on any failure) */
    if (!err && (err = audio_restart(hpvol)) != 0) {
        printf("audio start failed: %d (%s); codec muted\n", err, err_name(err));
    }
    if (audio_running()) {
        /* evidence for the timing fix: 100 ms of audio, then the numbers */
        cli_audio_info_t a;
        audio_info(&a, 1);
        uint32_t t0 = now_ms(), spins = 0;
        while (now_ms() - t0 < 100 && ++spins < 5000000u) {
#if OE_BLE
            oe_ble_poll();
#endif
        }
        audio_info(&a, 1);
        printf("audio running: %d Hz, %d-sample blocks. Timing over 100 ms: CPU %.0f%% avg %.0f%% peak, "
               "slack >= %d samples, late %lu of %lu. Type 'help'.\n> ",
               AUDIO_FS, AUDIO_BLOCK, (double)a.cpu_avg_pct, (double)a.cpu_peak_pct, a.slack_min,
               (unsigned long)a.late, (unsigned long)a.window_blocks);
    } else {
        printf("AUDIO IS NOT RUNNING (red LED blinks). The console still works: try 'i2cscan'.\n> ");
    }
    fflush(stdout);

    /* hardware watchdog: if the main loop ever stalls for ~5 s (a hung bus,
     * a stuck driver), the chip resets and the boot text says why. Audio
     * runs in interrupts and would not notice such a stall by itself. */
    MXC_WDT_SetClockSource(MXC_WDT0, MXC_WDT_PCLK);
    MXC_WDT_SetResetPeriod(MXC_WDT0, &wcfg);
    MXC_WDT_ResetTimer(MXC_WDT0);
    MXC_WDT_EnableReset(MXC_WDT0);
    MXC_WDT_Enable(MXC_WDT0);

    int last_b1 = 0, last_b2 = 0;
    uint32_t t_btn = 0, t_led = 0;

    while (1) {
#if OE_BLE
        oe_ble_poll();
        while (bleq_tail != bleq_head) {
            char line[128];
            snprintf(line, sizeof(line), "%s", bleq[bleq_tail % BLEQ]);
            bleq_tail++;
            printf("\n[ble] %s\n", line);
            capture_ble = 1;
            cli_set_remote(1);
            cli_handle_line(line);
            cli_set_remote(0);
            capture_ble = 0;
            printf("> ");
            fflush(stdout);
        }
#endif
        console_poll();
#if OE_REC
        rec_poll();
#endif
        MXC_WDT_ResetTimer(MXC_WDT0);
        uint32_t now = now_ms();
        int ac = oe_time_ok() ? audio_check(now) : 0; /* the fallback clock stops with audio */
        if (ac) {
            printf("\n[audio] interrupt had stopped; restart %s\n> ", ac > 0 ? "ok" : "FAILED");
            fflush(stdout);
        }

        /* buttons: sampled every 20 ms, which also debounces them */
        if (now - t_btn >= 20) {
            t_btn = now;
            int b1 = PB_Get(0), b2 = PB_Get(1);
            if (b1 && !last_b1) {
                oe_params_t *u = cli_params();
                u->bypass = !u->bypass;
                cli_params_changed();
                printf("\n[button] %s\n> ", u->bypass ? "BYPASS (flat)" : "OPENEAR ON");
            }
            if (b2 && !last_b2) {
                oe_params_t *u = cli_params();
                u->master_db = (u->master_db >= -0.5f) ? -12.0f : u->master_db + 6.0f;
                cli_params_changed();
                printf("\n[button] volume %.0f dB\n> ", (double)u->master_db);
            }
            last_b1 = b1;
            last_b2 = b2;
            fflush(stdout);
        }

        /* LEDs 10x per second: green solid = processing, blinking = bypass */
        cli_tick(now); /* test-tone auto-stop */

        if (now - t_led >= 100) {
            t_led = now;
            if (!audio_running()) {
                LED_Off(LED_GREEN);
                if ((now / 250) & 1) {
                    LED_On(LED_RED);
                } else {
                    LED_Off(LED_RED);
                }
                continue;
            }
            if (cli_params()->bypass && ((now / 500) & 1)) {
                LED_Off(LED_GREEN);
            } else {
                LED_On(LED_GREEN);
            }
            if (dsp_link_get()->stats.howl_atten_db > 0) {
                LED_On(LED_RED);
            } else {
                LED_Off(LED_RED);
            }
        }
    }
}
