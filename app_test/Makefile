# Builds the PC version of the firmware console (the real cli.c + DSP) that the
# browser test talks to.
CC ?= cc
CFLAGS ?= -O1 -std=c99 -Wall -Wextra -I../firmware -I../firmware/dsp

cli_sim: cli_sim.c ../firmware/cli.c ../firmware/cli.h ../firmware/dsp/openear_dsp.c
	$(CC) $(CFLAGS) -o $@ cli_sim.c ../firmware/cli.c ../firmware/dsp/openear_dsp.c -lm

test: cli_sim
	python3 -I test_app.py

clean:
	rm -f cli_sim
