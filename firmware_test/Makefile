# OpenEar firmware-level PC tests:  make
#   test_recorder   the real recorder.c against a fake SD card (with ASan/UBSan)
#   init order      source checks for the boot order and safety paths
#   cycle bench     Cortex-M4 cycle estimate of the audio interrupt's work
#                   (needs arm-none-eabi-gcc and: pip install unicorn capstone)
CC ?= cc
SAN = -fsanitize=address,undefined -fno-sanitize-recover=all

all: recorder initorder cycles

recorder:
	$(CC) -O1 -g -std=gnu99 -Wall -Wextra -Werror -DOE_REC=1 -Istubs -I../firmware -I../firmware/dsp $(SAN) \
	  -o test_recorder test_recorder.c ../firmware/recorder.c ../firmware/dsp/openear_dsp.c -lm
	./test_recorder

initorder:
	python3 check_init_order.py

cycles:
	python3 cycle_bench.py

clean:
	rm -f test_recorder

.PHONY: all recorder initorder cycles clean
