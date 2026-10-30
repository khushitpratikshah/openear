/*
 * OpenEar battery readout from the MAX20303's ModelGauge fuel gauge.
 *
 * The MAX20303 datasheet's fuel-gauge register map is VCELL 0x02, SOC 0x04,
 * MODE 0x06, VERSION 0x08, HIBRT 0x0A, CONFIG 0x0C, VALRT 0x14, CRATE 0x16,
 * VRESET/ID 0x18, STATUS 0x1A - the same layout as the MAX17048, so we use
 * its units: VCELL 78.125 uV/LSB, SOC 1/256 % per LSB, both MSB first.
 *
 * The gauge's I2C address isn't confirmed for this board: we try 0x36 (the
 * MAX17048's) and, if that fails, any other address on the bus that isn't
 * the codec (0x18) and answers the VERSION register sensibly. 'i2cscan'
 * lists every chip on the bus so you can check by hand.
 * Licence: MIT.
 */
#include "fuelgauge.h"
#include "codec_bus.h"

#include <stddef.h>

#include "i2c.h"
#include "mxc_device.h"
#include "mxc_errors.h"

#define FG_I2C MXC_I2C1
#define FG_VCELL 0x02
#define FG_SOC 0x04
#define FG_VERSION 0x08
#define CODEC_ADDR 0x18

static int fg_addr = 0x36;
static int fg_found;

static int read16(int addr, uint8_t reg, uint16_t *val)
{
    uint8_t tx = reg, rx[2];
    mxc_i2c_req_t req = { 0 };
    req.i2c = FG_I2C;
    req.addr = (uint8_t)addr;
    req.tx_buf = &tx;
    req.tx_len = 1;
    req.rx_buf = rx;
    req.rx_len = 2;
    req.restart = 0;
    int err = MXC_I2C_MasterTransaction(&req);
    if (err == E_BUSY && bus_recover() == E_NO_ERROR) {
        err = MXC_I2C_MasterTransaction(&req);
    }
    if (err == E_NO_ERROR) {
        *val = (uint16_t)((rx[0] << 8) | rx[1]);
    }
    return err;
}

/* Same probe as ADI's Examples/MAX32655/I2C_SCAN: an address-only transaction.
 * A chip that is present acknowledges; a missing one returns an error. */
int i2c_probe(int addr)
{
    mxc_i2c_req_t req = { 0 };
    req.i2c = FG_I2C;
    req.addr = (uint8_t)addr;
    req.tx_buf = NULL;
    req.tx_len = 0;
    req.rx_buf = NULL;
    req.rx_len = 0;
    req.restart = 0;
    req.callback = NULL;
    int err = MXC_I2C_MasterTransaction(&req);
    if (err == E_BUSY && bus_recover() == E_NO_ERROR) {
        err = MXC_I2C_MasterTransaction(&req); /* a busy bus is not "no chip" */
    }
    return err == E_NO_ERROR;
}

/* Start the bus for 'i2cscan' and 'batt' if audio init failed early. */
void fuelgauge_bus_init(void)
{
    bus_init();
}

static int find_gauge(void)
{
    uint16_t v;
    if (fg_found) {
        return 0;
    }
    if (i2c_probe(0x36) && read16(0x36, FG_VERSION, &v) == E_NO_ERROR) {
        fg_addr = 0x36;
        fg_found = 1;
        return 0;
    }
    for (int a = 0x08; a < 0x78; a++) {
        if (a == CODEC_ADDR || a == 0x36) {
            continue;
        }
        if (!i2c_probe(a)) {
            continue; /* nothing at this address: don't try to read from it */
        }
        /* a ModelGauge VERSION is a small non-zero number, never 0xFFFF */
        if (read16(a, FG_VERSION, &v) == E_NO_ERROR && v != 0 && v != 0xFFFF && v < 0x0100) {
            fg_addr = a;
            fg_found = 1;
            return 0;
        }
    }
    return E_NO_DEVICE;
}

int fuelgauge_read(int *millivolts, float *percent)
{
    uint16_t v, s;
    int err;
    if ((err = find_gauge()) != 0) {
        return err;
    }
    if ((err = read16(fg_addr, FG_VCELL, &v)) != E_NO_ERROR) {
        fg_found = 0; /* look again next time */
        return err;
    }
    if ((err = read16(fg_addr, FG_SOC, &s)) != E_NO_ERROR) {
        return err;
    }
    if (v == 0xFFFF) {
        return E_COMM_ERR; /* bus floating or no chip: not a real reading */
    }
    *millivolts = (int)(((uint32_t)v * 5u) / 64u); /* 78.125 uV per LSB */
    *percent = s / 256.0f;
    return E_NO_ERROR;
}

int fuelgauge_addr(void)
{
    return fg_found ? fg_addr : -1;
}
