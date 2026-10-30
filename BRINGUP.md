# OpenEar bring-up and debugging guide

Work through the steps in order. Each step tests one piece of hardware, so when
something fails you know exactly where to look. Write down what you see at each
step; those notes become your TechForum posts.

Prebuilt firmware in `firmware/prebuilt/`, so every fix below is a different
file to drag across. No toolchain is needed until you change code:

| File | What's in it | Use it when |
|---|---|---|
| `openear-audio-only.bin` | Audio + serial console | **Flash this first.** It's the smallest thing that can work |
| `openear-audio-only-micleft.bin` | Same, mic on the other I²S slot | Stage 2: `raw` stays at zero |
| `openear-audio-only-block128.bin` | Same, 128-sample blocks | Stage 3: `late blocks` above 0 or crackle |
| `openear-full.bin` | Audio + console + Bluetooth + SD recorder | Once the audio-only build passes stage 3 |
| `openear-full-block128.bin` | Full, 128-sample blocks | Stage 5: Bluetooth makes `late blocks` go above 0 |
| `diagnostics/adi-i2s-example-fthr.bin` | ADI's own I2S example, unmodified | To tell a board fault from an OpenEar bug |

Every file also comes as `.hex`. On this board **`.bin` is the format that is
confirmed to work** (both start at 0x10000000).

To flash: plug the board's USB into your PC. A drive called DAPLINK (or similar)
appears. Drag the `.bin` file onto it. The board resets by itself. If the drive
remounts after 2-3 s with `FAIL.TXT` ("An error occurred during the transfer")
and the old program keeps running, update the board's DAPLink firmware first:
hold **SW6** while plugging in USB, a MAINTENANCE drive appears, drop
`max32625_max32655fthr_if_crc_swd_v1.0.3.bin` on it (TROUBLESHOOTING §4.2).

Serial terminal: 115200 baud, 8N1, the board's USB COM port. Type `help`.

If anything looks wrong, `TROUBLESHOOTING.md` has a lookup by symptom.

---

## Stage 1: does it boot?

**Expect:**
```
=== OpenEar 0.3.0 - open assistive hearing on MAX32655 ===
Not a medical device. Start quiet; use closed in-ear earphones.
build: BLE off, SD recorder off, block 64
clock: TMR2 ok (0), DWT cycle counter not counting (not used)
no saved profile: flat pass-through (all bands 0 dB)
[i2c] at boot, before init: SCL=1 SDA=1 ctrl=0x.... status=0x.... intfl0=0x......
codec MAX9867: ok
mic power (PMIC LDO1): ok (0)
codec check after PMIC: ok (0, rev 0x42)
audio running: 48000 Hz, 64-sample blocks. Timing over 100 ms: CPU 3x% avg 3x% peak, slack >= NN samples, late 0 of 75. Type 'help'.
>
```
(The full build also prints `starting bluetooth...` before the last line.)
(The `0x....` numbers are raw register values; they vary, just copy them.)
(`DWT ... counting` or `not counting` are both fine: 0.3.0 no longer uses it.
The `clock: TMR2 ok` part matters: if it says `NOT RUNNING`, report it.)
The last line is the most important new evidence: **`late 0`** means no audio
block missed its deadline in the first 100 ms (0.2.2 had about 1 in 5 late,
heard as static).
Copy the whole boot text into your notes even when it works: the `[i2c]`,
`codec` and `mic power` lines are the evidence for your Day 1 post.
The green LED should be on (solid = processing, blinking = bypass). If audio can't start, the
console **still works** and the red LED blinks, so you can run `i2cscan`.
(On 0.2.x the LEDs and the SW2/SW3 buttons depended on the DWT clock; if it
wasn't counting they did nothing. 0.3.0 uses a hardware timer for them.)

| You see | Likely cause | Fix |
|---|---|---|
| No COM port | Charge-only USB cable | Use a data cable |
| COM port but no text | Wrong baud, or banner missed | 115200, then press the reset button (SW5) |
| `[i2c] at boot ...: SCL=0` or `SDA=0` | Something holds an I²C line low before the firmware touches it | Hardware: unplug everything from the headers, power-cycle, check again. Then flash `diagnostics/adi-i2s-example-fthr.bin` |
| `codec MAX9867: try N failed: -6 (I2C bus busy)` then `ok (after bus recovery)` | The bus got stuck but recovery freed it | Fine; note it in your post. Audio runs |
| `codec MAX9867: try 3 failed` and `audio init failed: -N` | Codec not answering on I²C1 | Type `i2cscan`. If 0x18 is missing, flash `diagnostics/adi-i2s-example-fthr.bin` and press SW5 (TROUBLESHOOTING §5.1) |
| `mic power (PMIC LDO1): skipped, ...` but audio runs | The PMIC didn't answer a safe probe, so its driver (which can freeze on a bad bus) wasn't called | Same as the next row: check `raw` in stage 2 |
| `mic power (PMIC LDO1): FAILED (-N)` but audio runs | PMIC didn't take the mic-power command | Go on to stage 2: if `raw` follows your voice, the mic has power anyway (ADI's example never sets it) |
| `NOTE: the last reset was the watchdog` | The main loop stalled for ~5 s and the chip reset itself | Send the whole boot text and what you were doing just before |
| `codec check after PMIC: FAILED` | Talking to the PMIC left the bus stuck | Audio stays off on purpose (the volume couldn't be set) and the codec is powered down. Send the boot text |
| `[codec] not ready ... refused` after `sidetone` or `hpvol` | Audio init failed, so the codec's state is unknown | On purpose, for safety. Fix the boot error first |
| `audio start failed: -10` (timeout) | Codec clocks never started | Power-cycle the board. If it persists, the codec isn't getting MCLK |
| Red LED blinking, banner says `AUDIO IS NOT RUNNING` | One of the two errors above | Read the message on the console; the console still works |

## Stage 2: does the mic work?

Type `raw` twice, once in silence and once while talking.

| You see | Likely cause | Fix |
|---|---|---|
| Peak jumps when you talk (e.g. −60 → −25 dBFS) | Working | Go to stage 3 |
| All zeros every time | Mic data is on the other I²S slot | Flash `openear-audio-only-micleft.hex` (source: `make AUDIO_CH=LEFT`) |
| Constant value, doesn't change | Mic not powered | `Microphone_Power()` in `audio_io.c`; check `i2cscan` shows the PMIC |
| Numbers jump around even in silence | Normal mic noise | Fine if the peak stays below about −50 dBFS |

## Stage 3: does sound reach the earphone?

Use **closed in-ear earphones**, start with them *out* of your ears, and turn
`hpvol 30` first (higher = quieter).

1. **Hardware path, no MCU:** `sidetone 1 20`. You should hear yourself. Then `sidetone 0`.
   (The codec is unmuted only while audio runs: if you hear nothing, check `audio` first.)
2. **Full DSP path:** with everything at 0 dB (the default), talk. You should hear yourself.
3. **A/B:** press **SW2** (green LED blinks = bypass). At 0 dB both settings must sound identical.
4. **Tone:** `tone 1000 -40`, then `tone off`.
5. **Timing:** `stat`, wait 10 s, `stat` again. In the second one, **late blocks
   must be `0 of ~7500`**, `DSP time` should be about 30-45%, and `slack` above 0.

| You see / hear | Likely cause | Fix |
|---|---|---|
| Sidetone works, DSP path silent | TX DMA not running | Send the `stat` output; check that `blocks` is increasing |
| Clicks or crackle, `late blocks` above 0 | DSP misses its deadline | Send both `stat` outputs. Meanwhile flash `openear-audio-only-block128.bin` (+2.7 ms latency) |
| Steady hiss but `late blocks` is 0 | Not a timing problem: mic/codec noise or gain | Try `bypass 1`, then `sidetone 1 20` (no MCU at all): if the hiss is there too, it's analog. Send what you heard |
| Sound in one ear only | Mono I²S mode | Fine for a single earpiece; note which ear, and whether the other ear has static |
| Squeal | Acoustic feedback | Earphone further from the mic, lower `vol`. The howl guard (red LED) steps in by itself |
| Too loud even at `hpvol 63` | Gain set too high | `reset`, then raise slowly |

**When stage 3 passes, you have a working hearing aid.** Post it.

## Stage 4: fitting and tuning

1. `fit 10 15 25 40 55 60` applies an example sloping loss. Toggle SW2 and hear the high frequencies come up.
2. `show` lists the per-band level and applied gain live.
3. `save`, then power-cycle the board. It should print `loaded saved profile`.
4. Measure latency (see README): the Audacity click test.

| You see | Likely cause | Fix |
|---|---|---|
| `save` prints FAILED | Flash write error | Note the number. The profile page is 0x10070000 |
| Audio doesn't come back after `save` | Restart failed | It should print a warning; reset the board and report it |

## Stage 5: full firmware (Bluetooth + SD)

Flash `openear-full.hex`. The banner should say `BLE on, SD recorder on`.

1. Repeat `stat` (twice, 10 s apart), with the phone connected and not.
   **Compare `DSP time`, `late blocks` and `slack` with stage 3.** If late blocks are
   now above 0, Bluetooth is interfering: send the numbers, and try `openear-full-block128.bin`.
2. Phone: open the OpenEar Fitting app, press **Connect**, pick "OpenEar".
   The console prints `[ble] phone connected`. Try the Tune sliders.
3. If the app can't find it, use the **nRF Connect** app: it should see a device
   called OpenEar. Write `stat` (as text) to the characteristic ending in `0001`
   with notifications on.
4. SD: insert a FAT32 microSD card, type `rec 10`, talk, wait. Copy `OE000.WAV` to the
   PC. The **left** channel is the raw mic and the **right** is OpenEar's output: the
   before/after for your post.
5. `batt` with the LiPo connected. If it fails, run `i2cscan` and note the addresses.

| You see | Likely cause | Fix |
|---|---|---|
| Phone can't see "OpenEar" | Not advertising | Check the banner says `BLE on` and you see `starting bluetooth...` |
| Connects then drops | Audio interrupt starving the radio | Report it with `stat` numbers; try `openear-full-block128.hex` |
| App connects, nothing answers | Notifications off | In nRF Connect, enable notify on the 0001 characteristic |
| `rec failed (-1xx)` | Card not FAT32, or not inserted | Format FAT32 on the PC |
| WAV has gaps, `overruns` > 0 | Slow card | Use a class 10 / U1 card |
| `batt` fails | Fuel gauge at a different address | `i2cscan`; tell me the addresses and we'll fix `fuelgauge.c` (your board showed 0x36, which is where it looks first) |
| `rec failed: audio is not running` | Recording needs the audio interrupt | Fix the audio first (stage 1) |

## What to send if you're stuck

Copy the full console output from boot, plus `stat` and `i2cscan`, and say which
stage failed and what you heard. That's enough to diagnose almost anything above.
