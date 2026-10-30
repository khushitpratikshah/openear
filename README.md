# OpenEar

An open-source, self-tunable assistive hearing device on the Analog Devices
**MAX32655FTHR**. Built for the CircuitDigest × DigiKey *Learn, Build & Win*
program by Khushit Shah.

> **Not a medical device.** OpenEar is an assistive-listening prototype and a
> platform for self-fitting experiments. Always start at low volume, use closed
> in-ear earphones, and see an ENT doctor about any sudden or one-sided hearing loss.

## How it works

```
digital mic ──► MAX9867 codec ──I²S/DMA──► Cortex-M4F DSP ──I²S/DMA──► MAX9867 ──► earphone
                (clock master, 48 kHz)        │
                                               ├─ 6-band Linkwitz-Riley crossover (flat when unity)
                                               ├─ per band: gain + compression + noise expansion
                                               ├─ master volume, click-free ramps (bypass too)
                                               ├─ howl guard (detects acoustic feedback)
                                               └─ peak limiter + NaN-safe hard ceiling

control:  USB serial console  ─┐
          Bluetooth LE app    ─┴─► same text commands ─► profile saved in flash
record:   rec 10  ─► stereo WAV on microSD (left = mic, right = OpenEar output)
```

* **All time-domain (IIR) processing.** No FFT frames, about 1 ms of filter delay in the
  speech band.
* **DSP runs inside the DMA interrupt.** Console printing, Bluetooth and SD writes can't
  cause audio dropouts. Each filter runs over a whole block with its coefficients in
  registers: about 37% of the CPU per block (estimate; 0.2.x needed about 62%,
  against a deadline at about 65%, which made roughly one block in five late on the real board).
* **Safety limits live in the DSP itself.** Band gain is capped at 30 dB, output at
  −3 dBFS and test tones at −20 dBFS whatever the volume, and non-numbers (NaN) fall
  to the quiet end. A hardware watchdog resets the chip if the main loop ever stalls.
  Tests cover all of this.

## Folder map

| Folder | What's in it |
|---|---|
| `BRINGUP.md` | **Start here when the board arrives:** staged bring-up with a fix for each symptom |
| `SETUP_PC.md` | Install everything on your PC (Part A is all you need on day 1) |
| `TROUBLESHOOTING.md` | Look up any symptom (flashing, boot, audio, Bluetooth, SD, battery, case) |
| `RELEASE_NOTES_0.3.1.md` | What changed in 0.3.0 and 0.3.1 and why, plus known limitations |
| `TEST_REPORT.md` | Every automated test, what it proves, and what only the board can prove |
| `run_all_tests.sh` | Runs every PC test suite in one go |
| `firmware/` | MSDK project (`make`), modules: `audio_io`, `cli`, `recorder`, `ble/`, `dsp/` |
| `firmware/prebuilt/` | Ready-to-flash `.bin` (and `.hex`) files: full, audio-only, the fallback variants, and ADI's I2S example in `diagnostics/` |
| `firmware/dsp/` | DSP core, portable C, shared by the board and the PC test bench |
| `dsp_test/` | PC test bench: 61 checks (incl. bit-exact vs the 0.2.2 DSP source) plus charts for your TechForum posts |
| `cli_test/` | PC test of every console command against fake hardware: 71 checks |
| `firmware_test/` | Audio I/O simulation (real `audio_io.c` on modelled DMA), SD recorder test, Cortex-M4 cycle bench, source safety checks |
| `app_test/` | Browser test: the phone app against the real console code |
| `app/` | Phone app (Web Bluetooth): hearing test, fitting, tuning, live levels, console |
| `enclosure/` | Parametric 3D-printable body-worn case (Python → STL) |

## Status (firmware 0.3.1, 30 Oct 2026)

✅ = shown on the real board, 🧪 = shown by PC tests only, ⏳ = not yet shown.
`TEST_REPORT.md` has the details.

| Part | State |
|---|---|
| Codec, mic, earphone output, six-band DSP | ✅ on the board |
| Bluetooth app + live tuning, saved profiles | ✅ on the board |
| Static / crackle | ✅ 0.3.1: 0 late blocks of 15568 without the phone, 0.19% with it (0.2.2: 20%, 0.3.0: 37%) |
| DSP load | ✅ 35% average, 42-44% peak of each 1.33 ms block |
| CPU meter, LEDs, buttons, tone auto-stop | ✅ on the board |
| Hearing test | ✅ clean beeps · ⚠️ mic not muted between beeps on the board (see known limitations) |
| SD recorder | ✅ stereo WAV (left = mic, right = output) plays correctly |
| Battery | ✅ 300 mAh LiPo: stopped at 8 h 30 min with 30% left |
| End-to-end latency | ✅ median 3.3 ms (Audacity, 5 finger snaps; design ~3.5 ms) |
| Enclosure | ⏳ printing |

Known limitations are listed in `RELEASE_NOTES_0.3.1.md`.

## Console commands

Type `help` on the board. The same commands work over USB serial and Bluetooth:

| Group | Commands |
|---|---|
| Info | `show`, `stat`, `state`, `levels`, `ver`, `i2cscan`, `batt` |
| Bands | `g`, `cr`, `knee`, `exp` (band 0–5 or `all`) |
| Overall | `vol`, `limit`, `att`, `rel`, `bypass` |
| Fitting | `fit <250> <500> <1k> <2k> <4k> <8k>` (dB HL), `save`, `load`, `reset` |
| Tests | `tone <Hz> <dBFS>`, `tone off`, `sidetone`, `raw`, `hpvol`, `audio`, `audio restart` |
| Recording | `rec [seconds]`, `rec stop`, `rec ?` |

Bluetooth: device name **OpenEar**, service `e0262760-08c2-11e1-9073-0e8ac72e1001`,
characteristic `…0001` (write a command line, receive output as notifications).
There is no pairing, so commands from Bluetooth get tighter loudness limits
(`hpvol` ≥ 12, `vol` ≤ +6 dB); the USB console has the full range.

## Measuring latency (no oscilloscope needed)

1. Record **stereo** in Audacity on a laptop.
2. Put one mic next to the board's mic, and feed the earphone output into the other channel
   (a 3.5 mm cable through an attenuator, or the earbud held against a second mic).
3. Snap your fingers.
4. Zoom in. The gap between the two channels is the end-to-end latency.

Expected: about 2.5 ms of buffering (two 64-sample blocks minus 6 samples, by
design; the audio I/O simulation confirms it), plus about 1 ms of filters, plus the
codec's converter delay. Publish whatever you measure.

## Building from source

1. Install ADI's MSDK (step by step in `SETUP_PC.md`): https://analogdevicesinc.github.io/msdk/USERGUIDE/
2. `cd firmware && make` (with `MAXIM_PATH` pointing at the MSDK).
   Options: `OE_BLE=0`, `OE_REC=0`, `AUDIO_BLOCK=128`, `AUDIO_CH=LEFT`.
3. `make flash.openocd`, or
   `arm-none-eabi-objcopy -O ihex build/max32655.elf openear.hex` and drag it onto the
   DAPLink drive.

PC tests (Linux, macOS or WSL): `./run_all_tests.sh` (or `--quick`). Charts: `cd dsp_test && make plots`.

## Licence

MIT for OpenEar code. `firmware/max9867.c/.h`, the MSDK `Makefile`, and the BLE stack setup
in `firmware/ble/oe_ble.c` derive from Analog Devices examples under Apache-2.0 (headers kept).
