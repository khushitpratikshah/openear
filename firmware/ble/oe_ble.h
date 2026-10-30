/*
 * OpenEar Bluetooth LE: the same text console as the USB serial port, over
 * ADI/Arm Cordio's proprietary data service (no pairing needed).
 *
 *   Device name : OpenEar
 *   Service     : e0262760-08c2-11e1-9073-0e8ac72e1001
 *   Data char.  : e0262760-08c2-11e1-9073-0e8ac72e0001
 *                 write = a command line, notify = console output lines
 *
 * Works with the OpenEar web app (Chrome on Android/desktop) or any BLE
 * terminal app such as nRF Connect.
 * Licence: MIT (built on ADI's BLE_dats example, Apache-2.0).
 */
#ifndef OPENEAR_BLE_H
#define OPENEAR_BLE_H

#include <stdint.h>

/* One-time init of the whole BLE stack (takes ~0.5 s for the 32 kHz trim). */
void oe_ble_init(void);
/* Run the BLE scheduler once; call continuously from the main loop. */
void oe_ble_poll(void);
/* Queue console text to the connected phone (dropped if nobody listens). */
void oe_ble_write(const char *s, int len);
int oe_ble_connected(void);
void oe_ble_status(char *buf, int len);

/* Provided by the application: called for each complete received line. */
void oe_ble_on_line(char *line);

#endif
