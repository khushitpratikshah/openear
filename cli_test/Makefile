# Build and run the OpenEar console test on a PC: make
CC ?= cc
CFLAGS ?= -O1 -std=c99 -Wall -Wextra -I../firmware -I../firmware/dsp

test_cli: test_cli.c ../firmware/cli.c ../firmware/cli.h ../firmware/dsp/openear_dsp.c
	$(CC) $(CFLAGS) -o $@ test_cli.c ../firmware/cli.c ../firmware/dsp/openear_dsp.c -lm
	./test_cli

clean:
	rm -f test_cli

.PHONY: clean
