# Build and run the OpenEar DSP test bench on a PC (Linux, macOS, or WSL).
#   make         -> build + run, results in ./results
#   make plots   -> also draw PNG charts (needs python3 + matplotlib)

CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -I../firmware/dsp

all: run

SRC = test_dsp.c ../firmware/dsp/openear_dsp.c reference/ref_dsp.c

test_dsp: $(SRC) ../firmware/dsp/openear_dsp.h reference/ref_dsp.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

# same tests with AddressSanitizer + UndefinedBehaviorSanitizer (buffer errors, UB)
asan: $(SRC)
	mkdir -p results
	$(CC) -O1 -g -std=c99 -Wall -Wextra -I../firmware/dsp -fsanitize=address,undefined -fno-sanitize-recover=all -o test_dsp_asan $(SRC) -lm
	./test_dsp_asan results

# the howl guard runs once per block: also check it at the block128 builds' size
test_howl128: $(SRC) ../firmware/dsp/openear_dsp.h reference/ref_dsp.h
	$(CC) $(CFLAGS) -DBLOCK=128 -DHOWL_ONLY -o $@ $(SRC) -lm

run: test_dsp test_howl128
	mkdir -p results
	./test_dsp results
	@echo; echo "---- howl guard at block 128 ----"
	mkdir -p results/howl128 && ./test_howl128 results/howl128

plots: run
	python3 plot_results.py results

clean:
	rm -rf test_dsp test_dsp_asan test_howl128 results

.PHONY: all run plots clean asan
