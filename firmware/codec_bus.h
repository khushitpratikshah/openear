/*
 * OpenEar: the shared I2C1 bus (MAX9867 codec 0x18 + MAX20303 PMIC/fuel
 * gauge) on the MAX32655FTHR, SCL P0.16 / SDA P0.17.
 *
 * One owner starts the bus once, so no driver re-initialises it while
 * another has just used it, and a "bus busy" error (-6) gets a recovery
 * attempt instead of ending audio for good.
 * Licence: MIT.
 */
#ifndef OPENEAR_CODEC_BUS_H
#define OPENEAR_CODEC_BUS_H

#include <stdint.h>

/* Start I2C1 as master at 100 kHz. Does nothing if it is already running. */
int bus_init(void);
/* Try to clear a stuck/busy bus: send a STOP, then reset the I2C block
 * (clock left on) and start it again. Returns 0 if the bus is idle after. */
int bus_recover(void);
/* 1 if the bus is running, idle, and set up the way the PMIC driver
 * (max20303.c, no timeouts in its loops) needs: FIFO thresholds of 1. */
int bus_ready_for_pmic(void);
/* Live pin levels (1 = high) read straight from the pads, and the I2C
 * block's status register. Works before and after bus_init(). */
void bus_lines(int *scl, int *sda);
uint32_t bus_status(void);
/* Print one diagnostic line: pin levels plus the raw ctrl/status/intfl0
 * registers of the I2C block. */
void bus_report(const char *tag);

#endif
