/*
 * OpenEar: the link between the main loop and the DSP in the audio interrupt.
 * Owns the DSP instance, the audio callback, and the parameter mailbox.
 * Licence: MIT.
 */
#ifndef OPENEAR_DSP_LINK_H
#define OPENEAR_DSP_LINK_H

#include <stdint.h>

#include "openear_dsp.h"

void dsp_link_init(const oe_params_t *p);
/* The audio callback (runs in the RX DMA interrupt). */
void dsp_link_process(const int16_t *in, int16_t *out, int n);
/* Hand new parameters to the DSP (main loop only). Latest wins; waits up to
 * 20 ms for the interrupt to take them. */
void dsp_link_push(const oe_params_t *p);
/* Live DSP state for show/levels (read-only). */
const oe_dsp_t *dsp_link_get(void);

#endif
