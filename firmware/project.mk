# OpenEar build configuration for the MSDK build system.
#   make                       full firmware (audio + Bluetooth + SD recorder)
#   make OE_BLE=0 OE_REC=0     audio only - use this first if anything misbehaves
#   make AUDIO_BLOCK=128       bigger audio blocks if 'stat' shows late blocks
#   make AUDIO_CH=LEFT         take the mic from the other I2S slot
#   make flash.openocd         flash over the board's USB debugger

TARGET = MAX32655
BOARD = FTHR_Apps_P1

OE_BLE ?= 1
OE_REC ?= 1
PROJ_CFLAGS += -DOE_BLE=$(OE_BLE) -DOE_REC=$(OE_REC)
ifneq ($(AUDIO_BLOCK),)
PROJ_CFLAGS += -DAUDIO_BLOCK=$(AUDIO_BLOCK)
endif
# make AUDIO_CH=LEFT if 'raw' prints zeros while you talk
ifneq ($(AUDIO_CH),)
PROJ_CFLAGS += -DAUDIO_I2S_CHANNEL=MXC_I2S_MONO_$(AUDIO_CH)_CH
endif

# DSP core lives in its own folder so the PC test bench can share it
VPATH += dsp
IPATH += dsp

ifeq ($(OE_BLE),1)
# Arm Cordio BLE stack, peripheral role only, no trace output on the UART
LIB_CORDIO = 1
INIT_PERIPHERAL = 1
INIT_CENTRAL = 0
TRACE = 0
VPATH += ble
IPATH += ble
endif

ifeq ($(OE_REC),1)
# FatFS over SPI for the microSD slot
LIB_SDHC = 1
FATFS_VERSION = ff15
endif

# 8 KB main stack (SDK default 4 KB): float printf + FatFS + nested
# audio and BLE interrupts
PROJ_AFLAGS += -D__STACK_SIZE=0x2000

# Audio math wants speed, not size
MXC_OPTIMIZE_CFLAGS = -O2

# No fused multiply-add: on the Cortex-M4F a VFMA takes 3 cycles but a VMUL +
# VADD pair takes 2, and unfused maths matches the PC tests bit for bit.
PROJ_CFLAGS += -ffp-contract=off

# printf of floats promotes to double by design; fine for a console
PROJ_CFLAGS += -Wno-double-promotion
PROJ_LDFLAGS += -u _printf_float
