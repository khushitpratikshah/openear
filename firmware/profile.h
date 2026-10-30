/*
 * OpenEar profile storage: the user's fitting lives in one page of the
 * MAX32655's internal flash, so it survives power-off with no phone present.
 * Licence: MIT.
 */
#ifndef OPENEAR_PROFILE_H
#define OPENEAR_PROFILE_H

#include <stdint.h>

#include "openear_dsp.h"

/* One 8 KB page well above the firmware image (~310 KB at most; profile.c checks
 * this at run time) and below the area
 * the BLE stack may later use for its own storage at the end of flash. */
#define PROFILE_ADDR 0x10070000UL

typedef struct {
    oe_params_t params;
    uint8_t hp_volume; /* MAX9867 volume code */
} profile_t;

int profile_save(const profile_t *p);
/* returns 0 if a valid profile was found */
int profile_load(profile_t *p);
int profile_erase(void);

#endif
