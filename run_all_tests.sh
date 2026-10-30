#!/bin/bash
# OpenEar: run every automated test. Exit code 0 = all passed.
#   ./run_all_tests.sh          everything (app test needs: pip install playwright; playwright install chromium)
#   ./run_all_tests.sh --quick  skip the browser app test and the cycle bench
# Everything here runs on a PC. None of it proves the board works: only the
# board's own console output does (see BRINGUP.md).
cd "$(dirname "$0")"
QUICK=0; [ "$1" = "--quick" ] && QUICK=1
fail=0
step() { printf '\n==== %s ====\n' "$1"; }
run() { if ! "$@"; then echo "!!!! FAILED: $*"; fail=$((fail+1)); fi; }

step "DSP (bit-exact vs 0.2.2 source, tone safety, howl guard, timing)"
run make -C dsp_test -s clean run
step "DSP under AddressSanitizer + UBSan"
run make -C dsp_test -s asan
step "Console (cli.c)"
run make -C cli_test -s clean
run make -C cli_test -s
step "SD recorder (recorder.c on a fake card, ASan)"
run make -C firmware_test -s recorder
step "Audio I/O simulation (audio_io.c on modelled DMA/I2S, 0.2.2 vs 0.3)"
run make -C firmware_test/audio_sim -s clean run
step "Boot order and safety-path source checks"
run python3 firmware_test/check_init_order.py
if [ $QUICK = 0 ]; then
  step "Cortex-M4 cycle bench (Unicorn emulator)"
  run python3 firmware_test/cycle_bench.py
  step "Phone app against the real console code (headless Chromium)"
  run make -C app_test -s clean
  run make -C app_test -s test
fi
printf '\n==== %s ====\n' "$([ $fail = 0 ] && echo 'ALL TEST SUITES PASSED' || echo "$fail SUITE(S) FAILED")"
exit $fail
