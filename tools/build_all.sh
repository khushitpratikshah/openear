#!/bin/bash
# Build every prebuilt firmware variant into firmware/prebuilt/ (.bin + .hex).
# Needs the ADI MSDK (MAXIM_PATH, default /root/msdk) and arm-none-eabi-gcc on PATH.
#   tools/build_all.sh            all five
#   tools/build_all.sh full ao    only some (ao, ml, a128, full, f128)
set -e
MSDK=${MAXIM_PATH:-/root/msdk}
cd "$(dirname "$0")/../firmware"
b() {
  name=$1; shift
  make clean MAXIM_PATH="$MSDK" >/dev/null 2>&1 || true   # clean needs MAXIM_PATH, or it silently does nothing
  rm -rf build
  make -j8 MAXIM_PATH="$MSDK" "$@" > "build_$name.log" 2>&1 || { echo "BUILD FAIL $name (see firmware/build_$name.log)"; exit 1; }
  w=$(grep -E "warning" "build_$name.log" | grep -v "$MSDK\|recipe for target" | wc -l)
  arm-none-eabi-objcopy -O binary build/max32655.elf "prebuilt/$name.bin"
  arm-none-eabi-objcopy -O ihex build/max32655.elf "prebuilt/$name.hex"
  echo "$name: $(arm-none-eabi-size build/max32655.elf | tail -1 | awk '{print "text",$1,"bss",$3}'), OpenEar warnings: $w"
  rm -f "build_$name.log"
}
set -- ${@:-ao ml a128 full f128}
for v in "$@"; do case $v in
  ao)   b openear-audio-only OE_BLE=0 OE_REC=0 ;;
  ml)   b openear-audio-only-micleft OE_BLE=0 OE_REC=0 AUDIO_CH=LEFT ;;
  a128) b openear-audio-only-block128 OE_BLE=0 OE_REC=0 AUDIO_BLOCK=128 ;;
  full) b openear-full ;;
  f128) b openear-full-block128 AUDIO_BLOCK=128 ;;
esac; done
make clean MAXIM_PATH="$MSDK" >/dev/null 2>&1 || true
