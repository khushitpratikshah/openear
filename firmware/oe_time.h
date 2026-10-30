/*
 * OpenEar time base: a free-running 32-bit hardware timer (TMR2, on the
 * 50 MHz peripheral clock). Everything that measures time uses this:
 * the main-loop millisecond clock (buttons, LEDs, the test tone's 15 s
 * auto-stop), timeouts, and the audio interrupt's CPU meter.
 *
 * Why not the Cortex-M4 DWT cycle counter (as 0.2.x did): on the real board
 * 'stat' showed the DSP at exactly 0.0 % with 20 % late blocks, i.e. the
 * cycle counter was not counting, which also froze the millisecond clock.
 * oe_time_init() still tests the DWT and reports what it found.
 * Bluetooth (Cordio) uses TMR0, TMR1 and the wake-up timer; TMR2 is free.
 * Licence: MIT.
 */
#ifndef OPENEAR_TIME_H
#define OPENEAR_TIME_H

#include <stdint.h>

/* Start TMR2. Returns 0, or an MSDK error code (then the clock reads 0). */
int oe_time_init(void);
/* Raw timer ticks; wraps every ~86 s. Use differences only. */
uint32_t oe_ticks(void);
uint32_t oe_ticks_per_ms(void);
/* Milliseconds since oe_time_init(); call at least every 80 s (the main
 * loop calls it continuously). */
uint32_t oe_millis(void);
/* 1 if the timer is running (it advanced during init). */
int oe_time_ok(void);
/* What the DWT self-test found: 1 = counting, 0 = not counting. */
int oe_dwt_counts(void);

#endif
