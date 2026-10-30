# OpenEar 0.3.1 test report

Run everything with `./run_all_tests.sh` (about 4 minutes; `--quick` skips the
browser and cycle tests). The full output of the last run is in
`test_results_0.3.1.txt`.

Every suite compiles and runs the **real firmware source files**. Where
hardware is needed, a stub or model stands in, and its assumptions are listed
below. **No PC test proves that the board works.** Only the board's console
output does; the last section lists what is waiting for it.

## Suites (all passing, 0 failures)

| Suite | Runs | Checks | What it proves |
|---|---|---|---|
| `dsp_test` | `firmware/dsp/openear_dsp.c` | 61 + 20 | Flat crossover, band gains, compression curve, limiter ceiling, filter latency, hostile/NaN parameters, click-free bypass. **0.3.0:** output bit-identical to a frozen copy of the 0.2.2 DSP source, both compiled alike, at block sizes 64, 128 and 37 (4.5 s of speech-like signal with parameter changes). Tone plays at the requested level at vol +12/0/−20 dB and with the howl guard engaged (0.2.2: −8 dBFS for a −20 dBFS request). Fade and glide take their designed time (6.0 ms / 11.0 ms, 10-90%). Tone level holds over 10 minutes (±0.05 dB). Mic is silent between hearing-test beeps. Any block length works. **Howl guard:** 3 minutes of steady feedback on 5 noise seeds: one brief re-howl (0.2.2: about 25). With the feedback gone and a loud burst every 20 s, full gain returns within 8 minutes. With the feedback still there: a 3 dB volume step down gives back exactly 3 dB; 30 s at −12 dB and then back up in two steps (SW3), a 5 s bypass A/B (the guard lets go while flat), one band cut and restored, each band cut in turn (nothing given back), all bands cut 6 dB (6 dB back), a flat profile (all back within 30 s), a NaN reset, and a full hearing test (vol 0, tone, vol back) cause no re-howl or new howl event. A 6 dB cut with the feedback gone gives back 6 dB at once and the rest by the slow relax; a cut to −20 dB never makes the guard boost; a feedback path 11 dB stronger, from an odd starting value, stops at 24 dB, and a required attenuation over 24 dB is held at 24; a volume step up with a howl remembered is held back before the next block plays; the memory is relative to the band gains at the time of the howl. The 20 howl-guard checks also run at block 128 (the `block128` builds). Also run under AddressSanitizer + UBSan. |
| `cli_test` | `firmware/cli.c` + DSP | 71 | Every console command, bad input, Bluetooth loudness limits (including `audio restart` over Bluetooth, which holds `hpvol` ≥ 12 and `vol` ≤ +6 dB before unmuting), `STATE`/`LV` formats, `stat` fields, `rec` refused without audio, `audio`/`audio restart`, `hpvol` while audio is off. **Test-tone auto-stop** (`cli_tick`): 15 s for audible tones, restarted by a change, 60 s for the hearing test's silent hold. Clean under ASan/UBSan. |
| `firmware_test/test_recorder.c` | `firmware/recorder.c` + DSP, fake SD card | 13 | Every WAV header field. Every one of 16,000 stereo frames equals a per-sample model of the anti-alias filter + 3:1 decimation. A second recording starts clean. A slow card: overruns are counted exactly, and `rec stop` writes what the RAM ring kept, including a partial chunk (0.2.2 threw away up to 0.5 s). ASan/UBSan. |
| `firmware_test/audio_sim` | `firmware/audio_io.c` + `dsp_link.c` + `cli.c` on a model of the DMA/I²S and NVIC (`sim.c`), block 64 and 128 | 87 | Judged from the samples the simulated codec plays: every input sample comes out once, in order, with a constant 2N−2-sample delay, with Bluetooth-like interrupts that hold off and preempt the audio interrupt. With TX FIFO read-ahead of 2 or 6 samples: 0 late blocks. With 16 (FIFO full, worst case): ≤ 0.2% late. The real console `stat` is driven from the real counters (DSP time, late of window, slack, delay, played). A block that overruns by almost two blocks no longer stops either stream (0.2.2's SDK handler lost the input's interrupt; the output DMA ran out of reloads), and the sound is clean again right after. A stalled input plays silence, not a buzz. 30 audio starts with Bluetooth stealing main-loop time all land at the designed phase. The codec is never unmuted while the output stream is off (tallied over every scenario). Parameter hand-over (`dsp_link.c`): the latest change wins even if the interrupt is stalled, and a change it never took before audio stopped is not replayed over a newer one at the next start. A `save`-style 50 ms pause causes no spurious restart. Stall watchdog: restarts after ~30 ms for RX or TX stalls, at the same volume. Start timeout frees its DMA channels and keeps the codec muted. The volume is never written while audio is stopped. The frozen 0.2.2 `audio_io.c` in the same model shows late blocks with audible glitches. |
| `firmware_test/check_init_order.py` | comment-stripped source of `main.c`, `audio_io.c`, `dsp_link.c`, `max9867.c`, `codec_bus.c`, `fuelgauge.c`, `ble/oe_ble.c` | 28 | Boot order (codec before PMIC, BLE after audio init, start before unmute). Codec muted on every failure path, and never unmuted while audio is stopped. PMIC driver only after a timed probe. No DWT use outside `oe_time.c`. Every busy-wait bounded. Stored volumes clamped. Watchdog disabled before the slow boot steps and enabled after. Both streams watched. The output interrupt silences a buffer only when the input has stalled. |
| `firmware_test/cycle_bench.py` | DSP + sample conversion + `recorder.c` compiled with the firmware's flags, run in the Unicorn Cortex-M4 emulator | 6 | Cycle *estimate* per 64-sample block (133,333 cycles at 100 MHz): mic **36.7%**, hearing-test tone 37.6%, while recording to SD 41.5%. The 0.2.2 DSP source as it shipped (fused multiply-add) comes to 62.5% and 70.3%. Fails above 45%. |
| `app_test` | `app/index.html` in headless Chromium, talking to the real `cli.c` over a mocked Bluetooth link in 20-byte pieces | 28 | Connect, tuning, live levels, console, a full hearing test (some beeps left unanswered past the 1.2 s beep timer), apply and save. No rejected command, no JS errors. The mic stays muted through the whole test, and every audible beep is followed by the silent hold before the next one. |

### 0.3.1: output phase lock (from the 0.3.0 board test)
The audio simulation now models the board's slip: the output loses one sample every 49 ms against the input. Without the lock (0.3.0) the model gives 36% late blocks and slack 0, as the board did (35-38%). With it: 0 late blocks at block 64 and 128, slack at least 12 samples, about one 2-sample fix per two slipped samples, and only the slips and fixes themselves heard (504 sample errors in 10 s against about 5500). The same with the input slipping. With nothing slipping, no fix is ever made, in every normal-operation run. Mutants: lock removed (2641 late), lock turned the wrong way (1736 late): both fail.

### Board results so far (0.3.0 on 28 Oct, 0.3.1 on 29 Oct)
| Checked on the board | Result |
|---|---|
| DSP time | 35.4% average, 41.7% peak (44.1% with the phone); PC estimate 36.7% |
| Clock | TMR2 runs; DWT cycle counter does not count (the 0.2.2 "0.0%") |
| LEDs, SW2, SW3, 15 s tone auto-stop | all work |
| Interrupt delay | at most 4 samples without the phone, 6 with it |
| Late blocks (0.3.0) | 35-38%, slack 0: the output slip |
| Late blocks (0.3.1, 29 Oct) | 0 of 15568 without the phone (slack 32); 234 of 126293 (0.19%) with it; crackle "very little, ignorable" |
| Output slip (0.3.1 phase fixes) | 199 in 20.8 s = 19.2 samples/s without the phone; 2338 in 168 s = 27.8 samples/s with it |
| Hearing-test beeps | clean, no clicks; mic NOT muted between beeps (taps heard, 30 Oct) |
| SD recorder (30 Oct) | stereo WAV plays correctly in Audacity |
| Battery (30 Oct) | 300 mAh LiPo, stopped at 8 h 30 min with 30% left |
| Latency (30 Oct) | 3.3, 1.76, 4.9, 3.3, 3.3 ms (Audacity, finger snaps); median 3.3 ms; 1.76 is below the 2.5 ms floor (measurement error) |
| Watchdog note | only after flashing, not on SW5 resets |
| Battery gauge | no answer without a LiPo (`-2`) |

### Mutation checks (do the tests catch the bugs they are for?)
Each bug was put back into a scratch copy, and the named test failed:
- **DSP:** tone scaled by the master volume; no tone-level glide; glide 1 ms; fade 2 ms; no oscillator renormalisation; a 0.01% band-gain change; old howl recovery (8 re-howls).
- **Recorder:** decimation 2:1.
- **Audio I/O:** wrong output buffer; the SDK's DMA handler again (the stream stops after an overrun).
- **Source checks:** DWT in `main.c`; unmute after a failed restart (also commented out); no volume clamp on load.
- **App:** the beep timer sends `tone off`.
- **Review round 2:**
  - Console: silent-hold timeout back to 15 s; auto-stop removed.
  - Audio I/O: TX interrupt at the same priority as RX (the output dies after an overrun); no watchdog baseline after a start; no silencing on an input stall; unmute before start; latest-wins mailbox back to drop-if-busy.
  - Source checks: watchdog not disabled early.
  - Howl guard: any loud burst resets the relax timer; floor never relaxes; floor not cleared on a volume change.
  - Recorder: partial chunk dropped at `rec stop`.
- **Review round 3:**
  - Howl guard: floor cleared on any change (the round-2 code: 3 checks fail); floor not moved by a volume change; floor cleared on bypass only; floor cleared on a band change; no 0 dB lower bound (the guard boosts by 15 dB, and the bit-exact check fails too).
  - Parameter hand-over: stale mailbox not cleared when audio is stopped (a tone comes back after `tone off`).
  - Console: `audio restart` over Bluetooth without the volume limit.
- **Review round 4:**
  - Howl guard: no raise of the attenuation when the settings raise the loop gain (volume back up after a dwell: 4 re-howls); memory dropped by a NaN reset; no 24 dB cap; bypass not treated as flat; memory taken from the lowest band gain; no relax. Tracking only the band that howled (an intermediate version) failed the new each-band-in-turn check and was replaced.
  - Console: `audio restart` over Bluetooth without the `vol` limit.
  - One survivor, an equivalent mutant: letting recovery go below the required attenuation is undone by the raise in the next block.
- **Review round 5:**
  - Howl guard: highest band gain taken from band 0 only; required attenuation not capped at 24 dB; memory taken against a fixed 30 dB band gain; no hold-back in `set_params` (a 12 dB bump for about 2 blocks on a volume step up).
  - Console: `audio restart` over Bluetooth holding only `hpvol` when only `vol` is too loud.
  - Weak test fixed: the 24 dB cap check restarted the feedback every second and passed only because of that; it now runs one continuous 30 s feedback path.
  - Still survives: never forgetting the memory after 12 minutes. Within the volume and gain ranges it behaves the same, because by then the memory has relaxed by 24 dB.

The reviews also found weak tests, now fixed:
- the audio simulation's Makefile could never fail;
- the glide test's level changes all fell on zero crossings;
- the howl test only passed because of where its 6 s window ended;
- the fade and glide limits were loose enough that a fade 33 times too short passed;
- (round 3) the re-howl counter missed a howl that starts in the first window after a change, so the howl checks also count new howl events;
- (round 3) the simulator threw away Bluetooth time stolen from the main loop whenever a block was also preempted. Fixed; all checks still pass.

Known untestable in the model: the interrupt mask around the TX DMA start
(protects a few-microsecond window; risk about 1 in 4,000 starts). A mutant
that removes it survives.

## Model assumptions (where a PC stands in for hardware)

- **Cycle bench:** Cortex-M4 TRM timings:
  - FPv4 VADD/VMUL 1, VMLA/VFMA 3, VDIV 14, VLDR/VSTR 2;
  - LDR/STR 2 (back-to-back loads N+1), LDRD/STRD 3;
  - taken branches +2.

  No flash wait states, no cache misses, no interrupt entry/exit. It is an estimate: treat it as ±20%. The real figure is `stat`'s `DSP time`, and there is no board calibration point yet.
- **Audio I/O model:**
  - The DMA moves one sample per sample period.
  - The TX DMA keeps the I²S FIFO F samples ahead. F is unknown on this chip, so it is tested at 2, 6 and 16. The board's no-Bluetooth `slack` gives it: F ≈ N − 2 − DSP − slack.
  - Reload and count-to-zero match `MXC_DMA_RevA_SetSrcReload`. The chip has 4 DMA channels.
  - Bluetooth is modelled as random higher-priority interrupts of up to 15 samples (0.3 ms), about 50 per second. They delay or stretch the audio interrupt. With 20-30 samples, a few blocks in a thousand are late; at 45 samples (about 1 ms) the input stream can stop, and the 30 ms watchdog restarts it. Real Bluetooth interrupt lengths on this board are unknown; `stat`'s `interrupt delay` measures them.
- **Recorder:** FatFS is replaced by an in-memory file. Real SD write timing is untested.
- **App:** a mocked Web Bluetooth link. Real radio behaviour is untested.

## What only the board can prove (the hardware test asks for these)

| Question | Evidence needed |
|---|---|
| Is the static gone? | Listening, plus `stat`: `late blocks 0 of N` over 10 s, phone connected and not |
| Real DSP time, deadline slack, Bluetooth interrupt delay | `stat` |
| Does TMR2 run? Does the DWT count (the explanation for 0.2.2's 0.0%)? | Boot line `clock: ...` |
| LEDs, SW2/SW3, 15 s tone auto-stop work | Green LED, `[button]` lines, `[tone] stopped automatically` |
| Hearing-test beeps clean, mic silent between beeps | Listening during the app's test |
| Fuel gauge reading | `batt` |
| SD recording | `rec 5`, then the WAV on a PC |
| End-to-end latency | Audacity click test (README) |
