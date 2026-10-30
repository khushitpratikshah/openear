/* OpenEar time base - see oe_time.h. Licence: MIT. */
#include "oe_time.h"

#include "mxc_device.h"
#include "mxc_errors.h"
#include "mxc_sys.h"
#include "tmr.h"

#define OE_TMR MXC_TMR2

static uint32_t per_ms = 1; /* never 0: callers divide by it */
static int tmr_ok, dwt_ok;

static uint32_t raw_count(void)
{
    /* the SDK reads the count register twice (MXC_TMR_RevB_GetCount) */
    volatile uint32_t c = OE_TMR->cnt;
    c = OE_TMR->cnt;
    return c;
}

int oe_time_init(void)
{
    /* DWT self-test, for the record only (see oe_time.h) */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    uint32_t d0 = DWT->CYCCNT;
    for (volatile int i = 0; i < 1000; i++) {
    }
    dwt_ok = (DWT->CYCCNT - d0) != 0;

    mxc_tmr_cfg_t cfg = { 0 };
    cfg.pres = MXC_TMR_PRES_1;
    cfg.mode = MXC_TMR_MODE_CONTINUOUS;
    cfg.bitMode = MXC_TMR_BIT_MODE_32;
    cfg.clock = MXC_TMR_APB_CLK;
    cfg.cmp_cnt = 0xFFFFFFFFu;
    cfg.pol = 0;
    int err = MXC_TMR_Init(OE_TMR, &cfg, false);
    if (err != E_NO_ERROR) {
        return err;
    }
    MXC_TMR_Start(OE_TMR);
    per_ms = PeripheralClock / 1000u;
    if (per_ms == 0) {
        per_ms = 1;
    }
    uint32_t t0 = raw_count();
    for (volatile int i = 0; i < 1000; i++) {
    }
    tmr_ok = raw_count() != t0;
    return tmr_ok ? E_NO_ERROR : E_BAD_STATE;
}

uint32_t oe_ticks(void)
{
    return raw_count();
}

uint32_t oe_ticks_per_ms(void)
{
    return per_ms;
}

uint32_t oe_millis(void)
{
    static uint32_t last, ms, frac;
    uint32_t now = raw_count(), d = now - last;
    last = now;
    frac += d;
    ms += frac / per_ms;
    frac %= per_ms;
    return ms;
}

int oe_time_ok(void)
{
    return tmr_ok;
}

int oe_dwt_counts(void)
{
    return dwt_ok;
}
