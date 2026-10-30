# OpenEar firmware 0.3.1 (2026-10-28)

One fix on top of 0.3.0, from the first 0.3.0 board test. Nothing else changed:
same features, same commands, same app.

## What the board showed (0.3.0, 2026-10-28)

- **Confirmed fixed:** the CPU meter (DSP time 35.4% average, 41.7% peak; the PC
  estimate was 36.7%), the clock (TMR2 runs; the DWT cycle counter really does
  not count, which explains 0.2.2's "0.0%"), the LEDs, SW2, SW3 and the 15 s tone
  auto-stop. The continuous hiss of 0.2.2 is gone.
- **Still wrong:** 35-38% of blocks were late, with `slack 0`, and the sound had
  "consistent crackles", also in bypass. The first 100 ms after boot were clean
  (late 0, slack 38). Over time `played` fell behind `blocks` by about one block
  every 3 s (11 after the first `stat`, 21 after the second).

## Cause

The output stream loses about 21 samples a second against the input (about one
sample every 49 ms). That slowly moves the output's buffer boundary through the
input block. Whenever it sits inside the 35-42% of the block the DSP needs, the
output plays a block before the DSP has finished it. Averaged over time that is
about as many late blocks as the DSP's share of the block, which is what the board
showed. The PC model of the DMA reproduces it exactly when given the same slip: 36%
late blocks, slack 0. Why the output slips (most likely a brief I2S FIFO underrun)
is not known yet.

## Fix: output phase lock

At every input block, the firmware measures where the output's buffer boundary
falls. When it has drifted by more than 2 samples, the next output block is played
2 samples short (or 2 samples long, repeating its last sample) to put it back. Steps
are 2 samples because the output DMA packs two 16-bit samples into each FIFO word.
The output stream now starts 6 samples before the end of an input block (was 2), to
leave room for the correction. Buffering is 2N-6 samples: 2.5 ms (5.2 ms on the
`block128` builds).

`stat` has one new line: `output phase fixes N` since the last `stat`. About 100 per
10 s means the slip is as measured on 0.3.0.

Tested in the PC model, at block 64 and 128: with the output slipping 1 sample every
49 ms, 0 late blocks (0.3.0: 36%), slack at least 12 samples, about one fix per two
slipped samples, and only the slips and the fixes themselves are heard (504 sample
errors in 10 s, 0.3.0 about 5500). The same with the input slipping instead. With
nothing slipping, no fix is ever made. With the phase lock removed, or turned the
wrong way, the board scenario fails.

**Not fixed:** the slip itself. Each slipped sample and each correction is a
1-2 sample step in the output, about 30 a second in total. If that is still audible
on the board, the next step is the I2S FIFO thresholds, which needs the board's
`phase fixes` count first.

---

# 0.3.0 (2026-10-28)

Builds on 0.2.2, the first firmware that ran end to end on the real board.
Everything below is backed by an automated test (`TEST_REPORT.md`) unless it says
it needs the board. **None of 0.3.0 has run on the board yet.**

## Why this release

On 2026-10-27 the board ran 0.2.2 with good sound, live tuning over Bluetooth,
and "a lot of static". Its `stat` showed:

```
blocks 19741 ...
DSP CPU now 0.0%  peak 0.0% ...  late blocks 4029
```

A DSP at 0.0% cannot miss its deadline in 20% of blocks. The source explains both:

1. **The CPU meter read the Cortex-M4's DWT cycle counter, which was evidently not
   counting** on this board: a peak of exactly 0.0% since boot means it never
   counted a cycle. The same counter was the firmware's only clock. If it was stuck,
   the LEDs, the SW2/SW3 buttons and the test tone's 15-second auto-stop could not
   work. The 0.3.0 boot text reports whether the DWT counts, which settles it.
2. **The DSP took about 60-70% of a block, and the output deadline was about 65%
   away.** I measured it by running the 0.2.2 DSP, compiled like the shipped firmware,
   in a Cortex-M4 emulator: about 62% of a 64-sample block. The output stream started
   3/4 of a block after the input, and the DMA reads a few samples ahead, so a
   processed block had to be ready about 65% of the way through. A block that misses
   it plays stale samples, a click; many clicks a second sound like static. A model
   of the board's DMA running the 0.2.2 `audio_io.c` shows the same late blocks
   with audible glitches.

## What changed

### Static and late blocks (most likely cause of the static; the board will confirm)
- **DSP about 1.7x faster, same arithmetic.** Each filter now runs over the whole
  block with its coefficients in registers, not sample by sample through all 30
  filters. Without fused multiply-add (`-ffp-contract=off`): on the M4F, VFMA takes
  3 cycles, while a VMUL plus a VADD take 2. The test tone uses a two-multiply
  rotating oscillator instead of `sinf()` per sample. Estimated cost per 64-sample
  block: 36.7% (37.6% with the tone, 41.5% while recording), down from 62.5%.
  Compiled alike, the output is identical to the last bit to the 0.2.2 DSP source.
  The shipped 0.2.2 used fused multiply-add, so on the board the two differ by a
  few parts in ten million, far below anything audible. The new int16 rounding
  also differs from `lrintf()` by at most 1 LSB, at exact halves.
- **More time before the output deadline.** The output DMA now starts 2 samples
  before an input block ends, not 3/4 of the way through: a block gets about 85% of
  its time instead of about 65%. The last part of that wait runs with interrupts
  off, at most about 0.2 ms, so a Bluetooth interrupt can't shift it. Cost: 0.3 ms
  more delay. Buffering is now 2.6 ms (two blocks minus 2 samples); the old docs'
  "3.3 ms" was wrong.
- **A long interrupt no longer kills audio.** The SDK's DMA handler clears a
  channel's flags *after* its callback. If one block ever ran past the next
  count-to-zero (Bluetooth interrupts piling onto the DSP), that event was erased,
  the input DMA wasn't re-armed, and audio stopped for good. OpenEar now handles its
  two channels itself. It clears exactly the flags it saw, write-1-to-clear, before
  acting on them. The tiny output interrupt now runs at a higher priority than the
  DSP interrupt, so a long DSP block can't starve the output DMA of reloads either.
  Bluetooth stays the most urgent of all.
- **A stalled input plays silence, not a buzz.** If no input block arrives for two
  output blocks, the output interrupt silences the buffer it re-queues.
- **Late counter made exact.** A block counts as late only if the output really
  played some of its stale samples.

### Measurements you can trust
- **New time base: hardware timer TMR2** (50 MHz). Bluetooth uses TMR0, TMR1 and
  the wake-up timer. All timing uses TMR2: the millisecond clock, buttons, LEDs, the
  tone auto-stop, timeouts and the CPU meter. If TMR2 ever doesn't run, the clock
  falls back to counting audio blocks, so the tone auto-stop still works.
- **`stat` rewritten.** It reports DSP time (average and peak), late blocks out of
  the blocks since the last `stat`, the deadline slack, the worst interrupt start
  delay, played vs processed blocks, audio restarts and clock status. Slack and delay
  come from the DMA counters, independent of any clock.
- **The boot text ends with a timing summary** for the first 100 ms of audio.

### Safety fixes
- **The test-tone cap could be exceeded.** The tone was mixed in *before* the master
  volume, so with `vol` above 0 a "−20 dBFS" tone played louder, up to −8 dBFS at
  +12 dB. The howl guard also changed the tone level, which skews a hearing test. It
  is now mixed in after the master gain.
- **The codec is never unmuted without audio.** At boot audio starts first and is
  unmuted after. `hpvol` or `load` while audio isn't running only remembers the
  volume. Failed starts and restarts leave the codec muted. 0.2.2 could unmute with
  no audio stream.
- **Tone level changes glide** (5 ms) as well as fading in and out (10 ms), so no
  level change can click.
- **No more unbounded waits on the cycle counter.** If the counter doesn't run, the
  parameter hand-over (its stall check), the audio-start wait and Bluetooth's
  send-stall recovery could hang or never recover. All now use the timer plus a
  loop-count limit. The Bluetooth crystal-trim wait has a 2 s limit (ADI's example
  waits forever).
- **Hardware watchdog (about 5.4 s).** A stalled main loop resets the chip, and the
  next boot says so. Example: a stuck I²C bus inside an SDK driver, whose waits have
  no timeouts. It is disabled first thing at boot and re-enabled when the firmware
  is up.
- **Audio-stall watchdog.** If either audio stream stops for 30 ms (a missed DMA
  reload), audio restarts at the same volume; `stat` counts the restarts. It takes
  a fresh baseline after every start, so a `save` doesn't look like a stall.
- **`audio` / `audio restart` console commands** show whether audio runs and
  restart it without a reset. Over Bluetooth, `audio restart` first holds `hpvol`
  (≥ 12) and `vol` (≤ +6 dB) to the Bluetooth limits, because a restart unmutes
  at the stored loudness. Like `load` over Bluetooth, the held values stay. Because the codec is never unmuted without audio,
  `hpvol` now says when it only stored the value, and the sidetone check needs audio
  running.
- **Parameter changes are never dropped.** The hand-over to the audio interrupt is
  "latest wins", written with interrupts off. 0.2.2 could drop a change when the
  interrupt was slow, for example a `tone off`. A change the interrupt never took
  before audio stopped is discarded when a newer one arrives, so a restart can't
  replay it (for example a tone coming back after `tone off`).
- **Stored volume checked like a typed one**: clamped to 4..63 at boot and on `load`.
- **`rec` refuses when audio isn't running**, instead of opening a file that never fills.

### Howl guard
- In steady feedback, 0.2.x pulled the gain back, recovered 1 dB every 2 s, and
  howled again about every 7 s. It now remembers the loop gain that stopped the
  howl (volume plus the highest band gain, minus the attenuation) and keeps the
  gain at or below it, whatever the settings do. Turning the volume or all bands
  down gives that much back; turning them up again, or leaving bypass, takes it
  back at once, before anything can howl. In bypass (flat) the guard lets go.
- Tested with the feedback still there: a volume step down, 30 s at −12 dB and
  then back up in two SW3 steps, a 5 s bypass A/B, band cuts, a flat profile and a
  full hearing test each cause no re-howl. 3 minutes of steady feedback gives one
  brief re-howl (0.2.x: about 25).
- The memory relaxes 1 dB per 30 s without a near-howl (short loud sounds like a
  door knock don't count) and is forgotten after 12 minutes, so full gain returns
  when the feedback path goes (tested with a loud burst every 20 s). A NaN reset
  keeps it. The guard never boosts and never pulls back more than 24 dB.
- By design, right after a howl, turning the volume up doesn't make it louder (the
  red LED stays on while the guard holds gain back). The gain comes back 1 dB per
  30 s. Reseating the earphone stops the feedback; loading a flatter profile gives
  the gain back at once.

### Hearing test (phone app)
- The **microphone stays muted for the whole test**. Between beeps the app holds a
  silent tone (−100 dBFS) instead of switching the tone off. With the mic live, room
  noise was amplified into the ear and masked quiet beeps, and the hiss cutting in
  and out was itself a cue. The silent hold has its own 60 s auto-stop (audible tones
  still stop after 15 s). Needs firmware 0.3.0; with older firmware the app falls
  back to the old behaviour.

### Smaller fixes
- `state`/`show` right after a command show the new values: the hand-over waits up
  to one block for the interrupt to take them.
- `rec stop` writes what is still in the RAM buffer (up to 0.5 s, including a
  partial chunk) before closing.
- The test-tone auto-stop moved into the console module (`cli_tick`), and the
  DSP/mailbox glue moved into `dsp_link.c`, so both are now covered by PC tests.
- `raw` copies the mic block inside the interrupt, so it can't show a
  half-overwritten block.
- The SD recorder's anti-alias filter runs block-wise (less time in the audio interrupt).
- The output conversion no longer calls `lrintf()` per sample.

## Compatibility
- Saved profiles from 0.2.x load unchanged: the parameter structure is the same,
  and a test checks it.
- The app's `STATE`/`LV`/`BATT` lines are unchanged. `stat` is for people and its
  wording changed.

## Board result (29 Oct)
Without the phone: 0 late blocks of 15568, slack 32 samples, 199 phase fixes in
20.8 s (19.2 samples/s, as predicted from 0.3.0). With the phone: 234 of 126293
late (0.19%), 27.8 samples/s. Crackle "very little, ignorable".

## Known limitations (0.3.1)
- **The output slips ~21 samples a second against the input** on the board (cause
  unknown). 0.3.1 keeps the timing in step, but each slip is still a tiny step in the
  output. See above.
- **0.19% late blocks with the phone connected** (about one a second, not heard).
  The `block128` builds double the DSP's time if it ever matters.
- **Watchdog note at boot after flashing.** It does not appear on a normal reset
  (SW5 twice, 30 Oct), only after flashing: most likely the watchdog firing while the
  programmer halts the CPU. No hang seen in use.
- **The mic is not muted between hearing-test beeps on the board** (taps on the mic
  were heard loudly during the test, 30 Oct), although the PC tests show the DSP
  muting it. Not investigated before the deadline. Run the test in a quiet room.
- **Battery gauge without a LiPo:** `batt` gives `fuel gauge read failed (-2)`.
- **Latency measurement spread:** 5 finger snaps gave 3.3, 1.76, 4.9, 3.3 and 3.3 ms;
  1.76 ms is below the 2.5 ms buffering floor, so it is a measurement error.
- **SDK I²C waits have no timeouts.** A chip holding the bus could hang a driver. The
  watchdog turns that into a reset, but not during boot, before the watchdog is on.
- **TX FIFO read-ahead:** the 0.3.0 boot numbers (slack 38 with a 24-26 sample
  DSP) put it near 0 on this board, so the worst case modelled (16 samples) does not
  apply.
- **The I2S FIFO flush** (`MXC_I2S_Flush`) waits on the codec's clock with no timeout.
- **Very long Bluetooth interrupts** (about 1 ms or more) can still make the input
  DMA miss a reload at 64-sample blocks. The 30 ms watchdog restarts it (a short
  dropout), and the `block128` files avoid it. `stat`'s `interrupt delay` shows
  whether this board's Bluetooth comes anywhere near that.
- **Over Bluetooth** only `vol` (≤ +6 dB) and `hpvol` (≥ 12) are tighter than on the
  console. `g`, `cr` and `limit` reach the same −3 dBFS ceiling as on USB, as in 0.2.x.
- **The hearing test's silent hold mutes the mic for up to 60 s** if the phone
  disconnects mid-test (audible tones still stop after 15 s).
- **The howl memory follows the highest band gain**, not the band that howled
  (with equal gains, a howl moves to the next band when one is cut). So cutting
  one band out of several equal ones gives nothing back until the slow relax. If
  the feedback was in a lower-gain band, cutting the top band can let it howl
  once more; the guard catches it as usual.
- **The howl guard can't see a howl whose feedback path is very strong**
  (about −13 dB or stronger, e.g. the earphone lying on the mic). It only counts
  limiting when the mic input is 10 dB below the ceiling, so that a loud room
  doesn't trigger it, and such a howl is loud at the mic itself. The limiter still
  holds the output at the ceiling (−3 dBFS); take the earphone away or press SW2.
  Inherited from 0.2.x. With the earphone in the ear the path is far weaker, so the
  guard does see the howl (tested down to a −15 dB path).
- **A false howl trigger** would hold back that much gain until it relaxes (up to
  12 minutes). None was seen in the tests; loud steady sound doesn't trigger it.
- **`late blocks` can miss a block in a narrow case**: a single block that overruns
  by almost exactly two blocks (about 1.9 to 2.0 blocks, which needs a Bluetooth
  stall of about 2.5 ms). The audio there is already a dropout; the restart counter
  and `interrupt delay` still show it.

## Still to check on the board
Whether 0.3.1 removes the crackle (late blocks and phase fixes, without and with the
phone), the hearing-test beeps, and, if available, `batt` with the LiPo and `rec`
with a card. Still open after that: the end-to-end latency (Audacity click test).
