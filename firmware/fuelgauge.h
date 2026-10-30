#ifndef OPENEAR_FUELGAUGE_H
#define OPENEAR_FUELGAUGE_H

/* Read battery voltage (mV) and state of charge (%). Returns 0 on success. */
int fuelgauge_read(int *millivolts, float *percent);
/* I2C address the gauge was found at, or -1. */
int fuelgauge_addr(void);
/* 1 if a chip acknowledges at this 7-bit address on the codec/PMIC bus. */
int i2c_probe(int addr);
/* Start the I2C1 bus if the codec driver didn't get that far. */
void fuelgauge_bus_init(void);

#endif
