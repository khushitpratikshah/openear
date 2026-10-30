/*
 * OpenEar recorder: writes a stereo 16 kHz WAV to the microSD card.
 *   left  = raw microphone (what you'd hear without OpenEar)
 *   right = OpenEar's processed output (what goes to the earphone)
 * The audio interrupt only copies samples into a RAM ring; the main loop
 * does the slow SD writes, so recording never glitches the audio.
 * Licence: MIT.
 */
#ifndef OPENEAR_RECORDER_H
#define OPENEAR_RECORDER_H

#include <stdint.h>

#define REC_FS 16000

/* main loop */
int rec_start(int seconds, char *name, int name_len);
int rec_stop(void);
void rec_poll(void); /* call often from the main loop */
void rec_status(char *buf, int len);
int rec_active(void);

/* audio interrupt: one block at 48 kHz (decimated to 16 kHz inside) */
void rec_feed_isr(const int16_t *in, const int16_t *out, int n);

#endif
