/*
 * OpenEar: shared I2C1 bus helpers - see codec_bus.h. Licence: MIT.
 *
 * Why this exists: on the first real board the codec probe failed with
 * E_BUSY (-6), meaning the I2C block reported the bus busy before the codec
 * was even addressed. ADI's working I2S example starts I2C1 once and talks
 * only to the codec. OpenEar used to start the bus three times (PMIC
 * driver, codec driver, fuel gauge) with the PMIC driver's own
 * register-level transfers in between. Now the bus is started once, here.
 */
#include "codec_bus.h"

#include <stdio.h>

#include "gpio.h"
#include "i2c.h"
#include "mxc_delay.h"
#include "mxc_device.h"
#include "mxc_errors.h"

#define BUS MXC_I2C1
#define BUS_HZ 100000 /* slow and safe; both chips allow 400 kHz */
#define PIN_SCL MXC_GPIO_PIN_16
#define PIN_SDA MXC_GPIO_PIN_17

static int bus_busy(void)
{
    return (BUS->status & MXC_F_I2C_STATUS_BUSY) ? 1 : 0;
}

static int bus_running(void)
{
    const uint32_t want = MXC_F_I2C_CTRL_EN | MXC_F_I2C_CTRL_MST_MODE;
    return (BUS->ctrl & want) == want;
}

/* Start the block and match what the PMIC driver (max20303.c) sets up when
 * it starts the bus itself: its register-level reads wait, with no timeout,
 * for the RX threshold flag after ONE byte, so the thresholds must be 1.
 * The SDK's own transfers also finish on the DONE flag, so they work with
 * any threshold. */
static int bus_start(void)
{
    int err;
    if ((err = MXC_I2C_Init(BUS, 1, 0)) != E_NO_ERROR) {
        return err;
    }
    if (MXC_I2C_SetFrequency(BUS, BUS_HZ) < 0) {
        return E_BAD_PARAM;
    }
    MXC_I2C_SetTXThreshold(BUS, 1);
    MXC_I2C_SetRXThreshold(BUS, 1);
    return E_NO_ERROR;
}

int bus_init(void)
{
    if (bus_running()) {
        return E_NO_ERROR;
    }
    return bus_start();
}

int bus_recover(void)
{
    /* 1. the I2C block may think it is still inside a transfer: ask for a
     * STOP and give it up to 2 ms (helps a chip waiting for one) */
    if (bus_running() && bus_busy()) {
        BUS->mstctrl |= MXC_F_I2C_MSTCTRL_STOP;
        for (int i = 0; i < 200 && bus_busy(); i++) {
            MXC_Delay(10);
        }
    }
    /* 2. always reset the I2C block with its clock still running
     * (MXC_I2C_Init resets it with the clock off) and start it again. The
     * init's MXC_I2C_Recover toggles SCL until a chip holding SDA low lets
     * go (up to 16 tries). If that fails the block is left disabled;
     * bus_start() reports the error and nothing else may use the bus. */
    MXC_I2C_Reset(BUS);
    int err = bus_start();
    if (err != E_NO_ERROR) {
        return err;
    }
    return bus_busy() ? E_BUSY : E_NO_ERROR;
}

int bus_ready_for_pmic(void)
{
    /* the PMIC driver's reads only finish if RX threshold is 1 */
    if (!bus_running()) {
        return 0;
    }
    uint32_t rx = (BUS->rxctrl0 & MXC_F_I2C_RXCTRL0_THD_LVL) >> MXC_F_I2C_RXCTRL0_THD_LVL_POS;
    uint32_t tx = (BUS->txctrl0 & MXC_F_I2C_TXCTRL0_THD_VAL) >> MXC_F_I2C_TXCTRL0_THD_VAL_POS;
    return rx == 1 && tx == 1 && !bus_busy();
}

void bus_lines(int *scl, int *sda)
{
    /* make sure the input buffers are on, so 'in' shows the real pad level
     * whatever the pin function (the I2C alternate function also uses them) */
    MXC_GPIO0->inen |= PIN_SCL | PIN_SDA;
    uint32_t in = MXC_GPIO0->in;
    *scl = (in & PIN_SCL) ? 1 : 0;
    *sda = (in & PIN_SDA) ? 1 : 0;
}

uint32_t bus_status(void)
{
    return BUS->status;
}

void bus_report(const char *tag)
{
    int scl, sda;
    bus_lines(&scl, &sda);
    /* raw registers too: they are what tells *why* a bus is busy */
    printf("[i2c] %s: SCL=%d SDA=%d ctrl=0x%04lX status=0x%04lX intfl0=0x%06lX\n", tag, scl, sda,
           (unsigned long)BUS->ctrl, (unsigned long)BUS->status, (unsigned long)BUS->intfl0);
    fflush(stdout);
}
