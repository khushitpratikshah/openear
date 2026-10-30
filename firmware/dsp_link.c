/* OpenEar DSP link - see dsp_link.h. Licence: MIT. */
#include "dsp_link.h"

#include "audio_convert.h"
#include "audio_io.h"
#include "mxc_device.h"
#include "oe_time.h"
#if OE_REC
#include "recorder.h"
#endif

static oe_dsp_t dsp;
static oe_params_t pending;
static volatile int pending_flag;

void dsp_link_init(const oe_params_t *p)
{
    oe_dsp_init(&dsp, (float)AUDIO_FS, p);
}

void dsp_link_process(const int16_t *in, int16_t *out, int n)
{
    static float x[AUDIO_BLOCK], y[AUDIO_BLOCK];

    if (pending_flag) {
        __DMB(); /* see the struct the main loop wrote before the flag */
        oe_dsp_set_params(&dsp, &pending);
        __DMB();
        pending_flag = 0;
    }
    audio_to_float(in, x, n);
    oe_dsp_process(&dsp, x, y, n);
    audio_from_float(y, out, n); /* clips; a NaN becomes silence */
#if OE_REC
    rec_feed_isr(in, out, n);
#endif
}

/* Wait (at most 20 ms) until the interrupt has taken the mailbox. Uses the
 * hardware timer, with a loop count as a second limit, so it can never hang
 * even if the timer were stopped. */
static int wait_taken(void)
{
    uint32_t t0 = oe_ticks(), limit = oe_ticks_per_ms() * 20, spins = 0;
    while (pending_flag && oe_ticks() - t0 < limit && ++spins < 4000000u) {
    }
    return !pending_flag;
}

void dsp_link_push(const oe_params_t *p)
{
    if (!audio_running()) {
        /* No interrupt to race with. A change the interrupt never took (it
         * stalled, then audio stopped) is older than this one: drop it, or
         * the next audio start would apply it over this one, e.g. replay a
         * tone after 'tone off'. */
        __disable_irq();
        pending_flag = 0;
        __enable_irq();
        oe_dsp_set_params(&dsp, p);
        return;
    }
    /* Latest wins: the mailbox is overwritten even if the interrupt hasn't
     * taken the previous change yet (0.2.2 dropped it, and a dropped
     * "tone off" left a tone playing). Written with interrupts off, so the
     * interrupt never sees a half-written struct (~1 us). Main loop only. */
    __disable_irq();
    pending = *p;
    pending_flag = 1;
    __enable_irq();
    wait_taken(); /* so a 'state' right after shows the new values */
}

const oe_dsp_t *dsp_link_get(void)
{
    return &dsp;
}
