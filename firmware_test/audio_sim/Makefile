SHELL := /bin/bash -o pipefail
# OpenEar audio I/O simulation: the real firmware/audio_io.c on modelled
# DMA/I2S hardware (see sim.c). Non-PIE so the buffers sit below 4 GB and
# fit the 32-bit DMA address registers, as on the chip.
CC ?= cc
CFLAGS = -O1 -g -std=gnu99 -Wall -Wextra -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -no-pie -fno-pie \
         -fsanitize=undefined -fno-sanitize-recover=all -Istubs -I. -I../../firmware -I../../firmware/dsp -DOE_REC=0

all: run

NEW = test_audio_sim.c sim.c ../../firmware/audio_io.c ../../firmware/cli.c ../../firmware/dsp_link.c ../../firmware/dsp/openear_dsp.c
new64: $(NEW)
	$(CC) $(CFLAGS) -o $@ $(NEW) -lm
new128: $(NEW)
	$(CC) $(CFLAGS) -DAUDIO_BLOCK=128 -o $@ $(NEW) -lm
old64: test_audio_sim.c sim.c old_audio_io.c
	$(CC) $(CFLAGS) -DOLD -o $@ test_audio_sim.c sim.c old_audio_io.c -lm

run: new64 new128 old64
	./old64 | grep -v "^codec\\|^mic\\|^\\[i2c"
	./new64 | grep -v "^codec\\|^mic\\|^\\[i2c"
	./new128 | grep -v "^codec\\|^mic\\|^\\[i2c"

clean:
	rm -f new64 new128 old64
.PHONY: all run clean
