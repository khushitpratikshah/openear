# OpenEar troubleshooting guide

Everything you need to fix a problem **without asking anyone**. Look up what you
see, hear or read on screen; each entry gives the likely cause and the next
thing to try. For the step-by-step order of first power-up, use `BRINGUP.md`.
Several entries mention other files in this folder (`firmware/`, `app/`,
`enclosure/`); all paths are relative to the project root.

**Contents**

1. [Golden rules and panic buttons](#1-golden-rules-and-panic-buttons)
2. [Which half is broken? A 3-check split for any audio fault](#2-which-half-is-broken)
3. [Reading `stat`](#3-reading-stat)
4. [Flashing and serial problems](#4-flashing-and-serial-problems)
5. [Boot problems](#5-boot-problems)
6. [Audio problems](#6-audio-problems)
7. [Saving and loading settings](#7-saving-and-loading-settings)
8. [Bluetooth and the phone app](#8-bluetooth-and-the-phone-app)
9. [SD card recorder](#9-sd-card-recorder)
10. [Battery and power](#10-battery-and-power)
11. [Building the firmware yourself](#11-building-the-firmware-yourself)
12. [3D-printed case](#12-3d-printed-case)
14. [Still stuck: where to get help](#14-still-stuck-where-to-get-help)
15. [Error code cheat sheet](#15-error-code-cheat-sheet)

---

## 1. Golden rules and panic buttons

**Rules that save days**

1. **Change one thing at a time**, then test. If two things changed and it broke,
   you won't know which one did it.
2. **Keep the last working `.hex` file** in a folder called `known-good`. Flashing it
   again is always allowed.
3. **Earphones out of your ears** whenever you flash, reset, or type a new
   volume or gain for the first time. Put them in only after you've heard the level
   from an inch away.
4. **Write down what you see** (screenshot or copy-paste the console). Those notes
   become your TechForum posts, and they're what you'd show a helper.
5. **Power from USB** until audio works. Only add the LiPo battery when everything
   else is stable (see section 10 for the polarity check).
6. **Don't edit code until the unmodified prebuilt file has been tried.** If
   `openear-audio-only.hex` doesn't work, the cause is almost never your code.

**Panic buttons**

| Situation | Do this |
|---|---|
| Too loud or painful | Take the earphone out. Then `hpvol 63` (quietest) and `vol -20`. |
| Squealing / howling | Move the earphone away from the mic, or take it out. `bypass 1` or `vol -20` also helps. The red LED means the howl guard is acting. |
| Board frozen or console silent | Press **SW5** (reset). If that fails, unplug USB and plug it back in. |
| Board stuck in a bad state | Drag `openear-audio-only.hex` onto the DAPLINK drive again. |
| Settings are a mess | `reset` (restores defaults in RAM), then `save` to overwrite the stored copy. |
| Burning smell, hot battery, swelling | Unplug everything **now**. Don't charge that battery again. |

---

## 2. Which half is broken?

OpenEar has three signal paths. You can test each without the others, so a
silent or dead device can be narrowed down in about two minutes.

```
 mic ─► codec ADC ─► I²S RX ─► MCU (DSP) ─► I²S TX ─► codec DAC ─► earphone
        └──────── A ───────────────────────────────────────┘ (sidetone, no MCU)
                    └── B: raw ──┘
                                                 └──── C: tone ───────┘
```

| Test | Command | What it proves | If it fails |
|---|---|---|---|
| **A: analog path** | `sidetone 1 20` (then `sidetone 0`) | mic → codec → earphone, no MCU or I²S involved | Codec / mic power / earphone / jack. Go to 6.1. (From 0.3.0 the codec is only unmuted while audio runs: if `audio` says NOT running, type `audio restart` first, or the sidetone test is silent for that reason.) |
| **B: mic into MCU** | `raw` (twice: silent, then talking) | codec ADC → I²S RX → MCU | Peak stays at zero: wrong I²S slot, see 6.2 |
| **C: MCU to earphone** | `tone 1000 -40` (then `tone off`) | DSP → I²S TX → codec DAC → earphone | Tone silent but A works: TX problem, see 6.3 |

Reading the results:

| A | B | C | Meaning |
|---|---|---|---|
| ✅ | ✅ | ✅ | All three work. If you still hear nothing from live speech, check `bypass 0` and `vol` (6.4). |
| ✅ | ❌ | ✅ | Mic data isn't arriving. Flash `openear-audio-only-micleft.hex`. |
| ✅ | ✅ | ❌ | Output side. Check `stat`: is `blocks` rising? Then 6.3. |
| ❌ | any | any | Hardware side: earphone, jack, codec power, mic power. 6.1. |

---

## 3. Reading `stat`

Type `stat`. Its counters cover the time **since the previous `stat`**, so type it
twice, about 10 s apart, and read the second one. Example of a healthy reading
(0.3.0; the numbers are an illustration, not a measurement):

```
blocks 183204 (played 183204)  block 64 samples (1.33 ms) @ 48000 Hz
DSP time avg 33.0%  peak 41.0% of real time  late blocks 0 of 7500  (since last 'stat')
deadline slack >= 14 samples (0.29 ms)  interrupt delay <= 3 samples  audio restarts 0
clock: timer ok, DWT not counting
limiter GR 0.0 dB  howl events 0  howl atten 0 dB  clipped 0  resets 0
est. latency: 2.5 ms buffering + ~1 ms filters + codec
```

| Field | Healthy | What a bad value means |
|---|---|---|
| `blocks` / `played` | Both rise by ~750 per second and stay about equal | Either stuck: an audio stream stopped (the firmware restarts both after 30 ms; see `audio restarts`). `played` far from `blocks`: report it. |
| `DSP time avg` / `peak` | Peak under ~60% | Measured with a hardware timer (0.2.x showed 0.0% because its cycle counter wasn't counting). Over ~80%: tight, try a `block128` file. |
| `late blocks N of M` | **0 of M** | N > 0: N blocks finished after the codec had started playing them; each is a click/crackle. Note whether Bluetooth was connected, send `stat`, try a `block128` file. |
| `deadline slack` | Above 0 | The fewest samples left before the deadline, over the window. 0 means at least one block was at or past it. |
| `interrupt delay` | A few samples | How late the audio interrupt started (Bluetooth interrupts go first). Large values (>20) and late blocks together: Bluetooth is the cause. |
| `audio restarts` | **0** | The audio interrupt stopped and the firmware restarted it. Report it with what you were doing. |
| `clock: timer` | `ok` | `NOT RUNNING`: the hardware timer didn't start; CPU figures and the LED/button timing are then wrong. Report it. |
| `limiter GR` | 0 (idle) in normal speech | Constant reduction: input or gains too high. Lower `vol` or `g`. |
| `howl events` / `howl atten` | 0 | Acoustic feedback. Earphone too close to the mic. 6.8. |
| `clipped` | 0 | Output hit the hard ceiling. Lower `vol`/gains. |
| `resets` | **0** | The DSP saw a bad number (NaN) and restarted itself. It recovered, but **note what you typed** and report it. |

`stat` also prints `recorder:` and `bluetooth:` lines when those features are in the build.

---

## 4. Flashing and serial problems

### 4.1 The DAPLINK drive doesn't appear

| Cause | Fix |
|---|---|
| Charge-only USB cable | Use a cable that carries data. Try another. |
| Wrong connector | Use the board's USB connector (it carries both the debugger drive and the serial port). |
| Board unpowered by battery-mode PMIC | Press **SW1** (power button), or plug in USB. |
| Computer / hub issue | Try another port, no hub, another computer. |
| A broken debugger image (rare) | Follow ADI's MAX32655FTHR user guide for restoring the DAPLink debugger firmware. |

### 4.2 Dragging the `.hex` does nothing, or a `FAIL.TXT` appears

| Cause | Fix |
|---|---|
| Copy didn't finish | Wait until the drive disappears and reappears (about 10–20 s). Don't unplug in between. |
| `FAIL.TXT` on the drive | Open it. It names the problem (often "The transfer timed out" or "flash algo error"). Unplug, replug, try again. |
| `FAIL.TXT`: "An error occurred during the transfer", and `DETAILS.TXT` Git SHA is **not** `34fe95` | **Wrong/old debugger firmware (this happened on the real board, 2026-10-26).** Download `max32625_max32655fthr_if_crc_swd_v1.0.3.bin` from https://github.com/analogdevicesinc/max32625pico-firmware-images (64 KB, "Download raw file"). Unplug, hold **SW6**, plug in → drive **MAINTENANCE** → drop the file → it returns as DAPLINK → replug, Git SHA should start `34fe95`. Then flashing works. |
| Old factory demo still runs (`Dats got evt`, `>>> Advertising started <<<`) | The flash didn't take: see the row above. |
| Drag from a zip | Extract the zip first. |
| Wrong file type | Use the `.hex` files in `firmware/prebuilt/`. A renamed `.elf` or `.map` won't work. |
| Firmware was flashed but behaves like the old one | Press **SW5** (reset), then check the boot banner for the `build:` line. |

### 4.3 No COM port / garbled text

| Symptom | Fix |
|---|---|
| No COM port | Windows: look in Device Manager under Ports ("mbed Serial Port" / "DAPLink CMSIS-DAP"). Linux: `ls /dev/ttyACM*`, and add yourself to the `dialout` group. macOS: `ls /dev/tty.usbmodem*`. |
| Port is there, nothing prints | Settings must be **115200 baud, 8 data bits, no parity, 1 stop bit**. Press SW5 to re-print the banner. Only one program may hold the port open at a time. |
| Garbled characters | Wrong baud rate. Use 115200. |
| Your typing isn't echoed | Normal for some terminals. Press Enter; the `>` prompt should return. Make sure "Send line endings" is CR, LF or CR+LF (any works). |
| Console dies when you start recording | Fine to ignore; the recorder shares the CPU, not the UART. If the console stops responding for good, reset. |

Terminal programs that work: Tera Term or PuTTY (Windows), `screen /dev/ttyACM0 115200` (Linux/macOS), the VS Code Serial Monitor extension, or the Arduino IDE serial monitor. Any serial tool is fine.

---

## 5. Boot problems

A healthy boot (build with Bluetooth and SD off) looks like this:

```
=== OpenEar 0.3.0 - open assistive hearing on MAX32655 ===
Not a medical device. Start quiet; use closed in-ear earphones.
build: BLE off, SD recorder off, block 64
no saved profile: flat pass-through (all bands 0 dB)
[i2c] at boot, before init: SCL=1 SDA=1 ctrl=0x.... status=0x.... intfl0=0x......
codec MAX9867: ok
mic power (PMIC LDO1): ok (0)
codec check after PMIC: ok (0, rev 0x42)
audio running: 48000 Hz, 64-sample blocks. Type 'help'.
>
```

The full build also prints `starting bluetooth...` before the last line.
The **green LED** is on once audio runs (blinking = bypass, red = howl guard).

### 5.1 `audio init failed: -N` (0.2.0 said `audio init (codec on I2C1 not answering?) failed: -N`)

The firmware could not talk to the codec chip. The console **stays alive** (the red LED blinks and the
banner says `AUDIO IS NOT RUNNING`), so you can run `i2cscan` and `help`.

**What happened on the first board (2026-10-26).** Firmware 0.2.0 printed `failed: -6`. -6 is
E_BUSY: the MAX32655's I²C block said the bus was busy, so the codec was never even asked. 0.2.0
started I²C1 three times (PMIC driver, codec driver, fuel gauge) and talked to the PMIC, whose
driver drives the I²C registers directly, before the codec. ADI's working I2S example starts the
bus once and talks only to the codec. Firmware 0.2.1 copies ADI's order (codec first, then PMIC mic
power, then Bluetooth), starts the bus once, retries the codec up to 3 times with a bus recovery in
between, and prints what it sees at each step.

**Read the 0.2.1 boot lines in this order:**

| Line | Healthy | If not |
|---|---|---|
| `[i2c] at boot, before init: SCL=? SDA=?` | `SCL=1 SDA=1` (the board's pull-ups) | A 0 means something holds the line low before any code ran: hardware. Remove anything on the headers, power-cycle, and run ADI's example |
| `codec MAX9867: ...` | `ok` (or `ok (after bus recovery)`) | `try 3 failed: -6` = bus stays busy; `-9` = no answer or line stuck; `-17` = a chip at 0x18 that isn't a MAX9867 |
| `mic power (PMIC LDO1): ...` | `ok (0)` | Not fatal. If `raw` still follows your voice in Stage 2, ignore it |
| `codec check after PMIC: ...` | `ok (0, rev 0x42)` | The PMIC access left the bus stuck. Audio stays off on purpose, since the volume can't be set safely |

**Then the deciding test:** flash `firmware/prebuilt/diagnostics/adi-i2s-example-fthr.bin` (ADI's own
`Examples/MAX32655/I2S`, built unmodified for this board) and press SW5.

| ADI's example prints | Meaning | Next |
|---|---|---|
| `Press and hold SW3 to start recording.` | Codec and bus hardware are fine; the fault is in OpenEar | Send the OpenEar 0.2.1 boot text and `i2cscan` |
| `Error configuring audio codec: N` | Board-level problem on I²C1 | Multimeter on SDA/SCL at idle (both should be high, about 1.8-3.3 V; near 0 V = stuck). Contact CircuitDigest/DigiKey with both logs |

| Other cause | Fix |
|---|---|
| Board isn't a MAX32655FTHR, or wrong BSP build | The prebuilt files target the FTHR (`BOARD = FTHR_Apps_P1`). |
| Codec still powering up | Unplug and re-plug USB, wait 3 s. |
| Want evidence | Type `i2cscan`. A healthy board lists `0x18` (the codec) and the PMIC addresses. 0.2.1 retries a probe once after a bus recovery, so `none` really means nothing answered. |

Error numbers: see [section 15](#15-error-code-cheat-sheet). `-6` is "bus busy", `-9` is a communication error (nothing answered, or a line is stuck), and `-17` means a chip answered at the codec address but didn't identify as a MAX9867.

### 5.2 `audio start failed: -10`

Timeout waiting for the first audio block: the codec isn't sending clocks. The console stays alive
and the red LED blinks; audio is cleanly stopped (nothing is left half-running).

| Cause | Fix |
|---|---|
| Codec didn't finish configuring | Power-cycle (unplug USB, wait, replug). |
| Persistent | Re-flash `openear-audio-only.hex`; compare with ADI's `I2S` example, which uses the same codec setup at 48 kHz. |

### 5.3 Banner prints, `audio running` prints, but `blocks` doesn't rise

| Cause | Fix |
|---|---|
| Audio interrupt blocked | Reset. Check that you're not running a modified build with a high-priority interrupt. |
| Stuck in a flash save | Reset (SW5). Note what you were doing (`save` while Bluetooth was connected?). |

### 5.4 `no saved profile` on every boot after `save`

| Cause | Fix |
|---|---|
| `save` printed `FAILED` | See 7.1. |
| Re-flashing erased it | Expected: flashing a new `.hex` can erase the settings page. Run `save` again. |
| Firmware image grew into the settings page | `save` would report `FAILED` by design. Check `profile.c` (`PROFILE_ADDR 0x10070000`); the full build is ~311 KB, well below it. |

### 5.5 Random reboots / resets

| Cause | Fix |
|---|---|
| Weak USB power or battery at the end of its charge | Try a different port or charger; check `batt`. |
| Stack overflow after editing code | The stack is 8 KB (`project.mk`: `__STACK_SIZE=0x2000`). Don't allocate big arrays inside functions; make them `static`. |
| You pressed SW1 | SW1 is the PMIC power button, not a user button. SW2 = bypass, SW3 = volume. |
| The boot text says `NOTE: the last reset was the watchdog` | 0.3.0 resets the chip if its main loop stalls for ~5 s (a hung bus or driver). Audio stops for about a second, then comes back with your saved profile. Send the boot text and what you were doing. |

---

## 6. Audio problems

### 6.1 Silence even with `sidetone 1 20` (test A fails)

| Cause | Fix |
|---|---|
| Earphone not fully in the jack | Push it in until it clicks. Try another earphone. |
| Volume at its quietest | `hpvol 16`, then lower numbers are louder (`hpvol 10`). The limit is 4. |
| Earphone has an in-line microphone (4-ring plug) | May work, but wiring differs between brands. Try a plain 3-ring stereo earphone first. |
| Mic has no power | Press SW5 (reset) and talk. Run `raw` while talking (B). If the peak never changes, check the mic's power rail (PMIC LDO1, set in `audio_io.c` by `Microphone_Power(POWER_ON)`). |
| Codec not configured | Boot banner must say `audio running`. |
| Audio not running (after a failed start or save) | `audio` shows it; `audio restart` starts it and unmutes. 0.3.0 never unmutes the codec without audio, so `hpvol` alone only stores the volume. |

### 6.2 `raw` shows zeros or a constant value

| You see | Cause | Fix |
|---|---|---|
| Peak 0 every time, even when talking | Mic data is on the **other I²S slot** | Flash `openear-audio-only-micleft.hex`. To rebuild: `make AUDIO_CH=LEFT`. |
| Peak is stuck at one non-zero value | Mic unpowered or bus floating | Power-cycle; verify `Microphone_Power(POWER_ON)` is called before the codec init (it is in `audio_init`). |
| Peak changes, but quiet (−60 dBFS even when shouting) | Mic is working but level is low | Normal for a MEMS mic across a room; speak close. Raise `g all 10`. |
| Peak jumps wildly in silence | Noise / electrical interference | Keep USB cables away; try battery power. |

### 6.3 Test C fails: `tone 1000 -40` is silent but sidetone works

| Cause | Fix |
|---|---|
| `blocks` not rising | See 5.3. |
| Wrong output slot (mono mode) | Try the `micleft` variant; it may change the I²S channel selection for both directions. |
| `vol` very low or `bypass` stuck | `reset`, then `tone 1000 -40` again. |
| Tone is audible but live speech isn't | The mic side is the problem. Go back to B (6.2). |

### 6.4 Everything "works" but I can't hear myself

1. `bypass 0` (the green LED should be solid, not blinking). SW2 toggles bypass.
2. `reset` to restore defaults (all bands 0 dB, unity). Speak and listen.
3. `vol 0`, `hpvol 16`. If still faint, lower `hpvol` a few steps at a time (`hpvol 12`).
4. `show`: look at the `level` column while talking. If the level is −90, the DSP isn't getting mic data (go to B).

### 6.5 Crackle, clicks, dropouts, "digital" roughness

| `stat` shows | Cause | Fix |
|---|---|---|
| `late blocks` above 0 | A block missed its deadline (heard as a click). The most likely cause of the 0.2.2 static, not yet confirmed on the board. | Send two `stat`s 10 s apart. Try a `block128` file (adds 2.7 ms delay, more time per block). |
| `late blocks` 0, still crackle | Not timing. Possibly the unused I²S slot, a loose jack, or USB power noise | Try a power bank instead of the PC's USB; try `sidetone 1 20` (no MCU); report which ear crackles. |
| `clipped` rising | Output hitting the ceiling | `vol -6`, lower the `g` of the loud bands; `limit -10`. |
| Crackle only while Bluetooth is connected | Radio interrupts delaying audio | Flash `openear-full-block128.hex`; or keep Bluetooth for setup only. |
| Crackle only while recording | SD writes | Use a faster (class 10 / U1) card; `rec` only for short clips. |

### 6.6 Hiss and noise

Hiss is amplified microphone noise. Options, in order:

1. Lower high-band gain: `g 5 10`.
2. Raise the noise-gate: `exp all -60 2` (levels below −60 dBFS are reduced).
3. Use less compression: `cr all 1.5` (compression lifts quiet noise up).
4. Move the mic away from the earphone cable and from clothing rub.

### 6.7 Distortion, harshness, "boxy" sound

| Cause | Fix |
|---|---|
| Too much gain | `show`: `applied` gains above ~25 dB are a lot. Lower `g` per band. |
| Your own voice is loud in your ear | Normal for any hearing aid (occlusion). Lower the 250–500 Hz bands (`g 1 0`). |
| Limiter pumping | `limit -8`, `rel 150` (slower recovery), or lower `vol`. |

### 6.8 Squeal / howling (acoustic feedback)

The earphone's sound leaks back into the microphone and gets amplified again.

1. Use **closed in-ear tips** and push them in.
2. Move the board (mic) away from the earphone, at least 20 cm; keep the cable away.
3. Lower gain in the problem band (often 2–4 kHz): `g 4 10`.
4. The firmware's howl guard watches for this and cuts gain automatically (red LED). Frequent `howl events` in `stat` means the physical setup needs fixing.
5. **Volume won't go up after a howl (red LED on)?** That is the guard (0.3.0): it remembers the loudest setting that didn't howl and holds you there, so turning up can't bring the howl back. `show` prints `howl atten`. It gives 1 dB back every 30 s and forgets after 12 minutes. Fix the leak (steps 1-2) and the gain comes back on that schedule.
6. A loud squeal with the earphone **out of the ear and near the mic** may not light the red LED: the sound at the mic is so loud the guard takes it for a loud room. The output is still limited to the ceiling. Press SW2 (bypass) or move the earphone away.

### 6.9 Feels delayed or echoey

Latency is ~2.5 ms buffering (2 blocks minus 6 samples, by design) + ~1 ms
filters + the codec's own converters: well below the 10 ms that people notice.
It has not been measured on the board yet (README: the Audacity click test). If you hear a comb-filter/"tube" effect, it's the real sound
leaking around the earphone and mixing with the delayed processed sound.
**Closed tips** fix this. A `block128` file adds 2.7 ms (5.3 ms buffering).

### 6.10 Sound in one ear only

The codec is in mono mode with a single output. Fine for one earpiece. If you use a stereo earphone, one side may be silent: use one earbud.

### 6.11 Click when pressing SW2 / changing settings

There shouldn't be one: bypass, volume and gain changes are ramped. A click only on `save` is expected (audio mutes for a moment). If you hear clicks elsewhere, note exactly which command caused it and `stat`'s `resets` value.

### 6.12 A test beep keeps playing

Tones are capped at −20 dBFS (from 0.3.0 the cap holds whatever `vol` is; in 0.2.x a positive `vol` raised the tone above it) and stop with `tone off`. The firmware also **stops any tone by itself after 15 seconds**
(you'll see `[tone] stopped automatically after 15 s`), so a phone that disconnects in the middle of the hearing test
can't leave a beep playing. A tone is never saved with `save` and is never restored at boot.

---

## 7. Saving and loading settings

### 7.1 `save` prints `FAILED`

| Cause | Fix |
|---|---|
| Flash page locked or worn | Try again once. Power-cycle and try again. |
| Image too large | Only if you added a lot of code: the image must end below `0x10070000`. |
| Audio didn't restart | Console prints `[warn] audio restart failed: N`. Reset the board. |

Note that `save` mutes audio for a moment. That is by design: flash erase pauses the CPU.

### 7.2 Settings don't survive power-off

- Run `save` after changes (the app's "Send to OpenEar and save" does it for you).
- Re-flashing new firmware may clear them; run `save` again.

### 7.3 Want a clean start

`reset` restores defaults in RAM only. `save` afterwards overwrites the stored copy.

---

## 8. Bluetooth and the phone app

Use the **full** build (`openear-full.hex`); the audio-only build has no Bluetooth.
The banner should say `BLE on` and print `starting bluetooth...`.

### 8.1 App won't connect / device not listed

| Check | Detail |
|---|---|
| Browser | **Chrome on Android, Windows, macOS, or Edge.** Not Safari, not Firefox. **iPhone: use the free "Bluefy" browser.** |
| Page address | Must be `https://` (GitHub Pages) or `http://localhost`. A page opened from a file or an `http://` address on the network can't use Bluetooth. |
| Phone settings | Bluetooth on. On Android, also Location on (older versions need it for scanning) and give Chrome the "Nearby devices" permission. |
| Device | In the console, `stat` ends with a `bluetooth:` line (`advertising as OpenEar` or `connected`). **nRF Connect** (free app) should show a device named **OpenEar**. If nRF Connect sees it but the app doesn't: it's the browser/page. If neither: it's the firmware, so try the full build again. (The name is in the scan response; the app also searches by service UUID, so either is enough.) |
| Old pairing | After re-flashing, toggle the phone's Bluetooth off/on. |
| Range | Stay within 2 m while testing. |

**Test the app locally** (no GitHub needed): in the `app` folder run `python3 -m http.server 8000`, then open `http://localhost:8000` in desktop Chrome. Localhost counts as secure.

### 8.2 Connects, then drops

The console prints `[ble] disconnected (reason 0xNN)`:

| Reason | Meaning | What to do |
|---|---|---|
| `0x08` | Connection timeout: the link went silent | Move closer; if it happens right after `save`, that's the flash write: reconnect. If it happens during audio, try `openear-full-block128.hex`. |
| `0x13` | Phone ended the connection | Normal (page closed, tab backgrounded). |
| `0x16` | Board ended the connection | Normal after a reset. |
| `0x3D` | Link-layer integrity failure | Reconnect. |
| `0x3E` | Connection failed to establish | Retry; toggle phone Bluetooth. |

### 8.3 Connected, but nothing answers

| Cause | Fix |
|---|---|
| Notifications not enabled | The app enables them. In nRF Connect, press the triple-arrow on the characteristic ending `…0001`. |
| Wrong characteristic | Service `e0262760-08c2-11e1-9073-0e8ac72e1001`, characteristic `e0262760-08c2-11e1-9073-0e8ac72e0001`. |
| Line too long | Send commands with a newline (`\n`) at the end. The app does this for you. |
| Output is slow | Bluetooth replies travel in small pieces (as little as 20 bytes each) at one batch per connection interval, which ADI's settings put at 120–180 ms. A long reply such as `show` can take a few seconds; this is normal. |

### 8.4 Commands refused over Bluetooth

By design: Bluetooth has no pairing, so anyone nearby could connect. Over Bluetooth
`hpvol` can't go below 12 (louder than that is blocked) and `vol` is capped at +6 dB.
The message `(over Bluetooth, volume is limited to +6 dB)` is expected. Use USB for the full range.

### 8.5 Page looks broken or shows all tabs at once

That was a bug in an earlier version (fixed). Make sure you're using the current `app/index.html` and do a hard refresh (Ctrl+Shift+R, or clear the site data on the phone).

### 8.6 Hearing test results look wrong

- With no reference, results are *relative* to your best pitch. See `app/README.md` for the calibration steps.
- If many points are marked as "at the ceiling," the earphone is too quiet: `hpvol 10` (louder) and repeat. Never go below 4.
- Quiet room, earphone fully in, same volume each time.
- With firmware 0.3.0 and the 0.3 app, the device's microphone stays muted for the whole test (the app holds a silent tone between beeps), so room noise is no longer amplified into your ear while you listen for quiet beeps. With older firmware the app falls back to the old behaviour.
- It's an experiment tool, not a clinical audiogram.

### 8.7 Putting the app online (GitHub Pages)

1. Create a GitHub repo and upload the project.
2. Settings → Pages → Source: "Deploy from a branch", branch `main`, folder `/ (root)`. (Pages can only publish from the root or `/docs`; the `index.html` in the root forwards to `app/index.html`.)
3. Wait a minute, then open `https://<you>.github.io/<repo>/` on the phone.

---

## 9. SD card recorder

Needs the **full** build and a **FAT32** microSD (32 GB or smaller works best; class 10/U1).

Start with `rec 10`. You should see `recording 10 s to OE000.WAV (L = mic, R = OpenEar output)`, and after ten seconds `[rec] saved OE000.WAV: 10.0 s, 0 overruns`.

| You see | Cause | Fix |
|---|---|---|
| `recorder not built into this firmware` | Audio-only build | Flash `openear-full.hex`. |
| `rec failed (-103)` | `FR_NOT_READY`: no card, or card not seated | Reseat the card; reset the board. |
| `rec failed (-101)` | `FR_DISK_ERR`: card read error | Try another card; check for dirt on the contacts. |
| `rec failed (-113)` | `FR_NO_FILESYSTEM`: not FAT | Format as FAT32 on a PC. |
| `rec failed (-207)` / `-210` | Directory full / card write-protected | Delete old files; slide the lock switch. |
| `rec failed (-10)` | Already recording | `rec stop` first. |
| `rec failed (-3xx)`, `-4xx` | File-system errors while naming or writing the header | Re-format the card. |
| `[rec] SD write error, stopping` | Card removed or failed mid-recording | Reseat/replace; reset the board. |
| `overruns` above 0, or gaps in the WAV | Card too slow | Use a class 10 / U1 card, record shorter clips. |
| Audacity shows odd speed | The WAV is 16 kHz, 16-bit, stereo | Open as a normal WAV. **Left = raw mic, right = OpenEar's output.** |

Formatting a card as FAT32: Windows: right-click → Format → FAT32 (for cards above 32 GB, use a tool like *guiformat*). macOS: Disk Utility → MS-DOS (FAT). Linux: `sudo mkfs.vfat -F 32 /dev/sdX1`.

If you pull the card while recording, wait a few seconds, then reset the board.

---

## 10. Battery and power

> **Check LiPo polarity before connecting it.** Many cheap batteries have their red/black wires reversed relative to the board's connector. Compare against the + and − marks next to the battery connector. A reversed battery can destroy the PMIC. When in doubt, use a multimeter on the plug.

| Symptom | Cause | Fix |
|---|---|---|
| `fuel gauge read failed (N)` | Gauge address differs from the guess, or no battery | Connect the battery, run `i2cscan`, and note all addresses: 0x18 is the codec; others are the PMIC/gauge. If the gauge sits at an address other than 0x36, the firmware also scans on its own. To force one, change `fg_addr` in `firmware/fuelgauge.c` and rebuild. |
| `batt` shows a very odd percentage right after plugging in | Gauge needs a few minutes to learn | Wait; re-check later. |
| Board only runs on USB | Battery disconnected/flat | Charge it from USB (give it a couple of hours, not unattended). |
| Doesn't turn on from battery | PMIC is off | Press **SW1** (the PMIC power button), holding it for about a second. The exact power-on/off press length is in ADI's user guide. |
| Battery drains fast | Bluetooth + audio at 100 MHz are not tiny | Expect hours, not days. Measure and report; that's a result for your post. |

Safe LiPo habits: only use cells with a protection circuit; never puncture or squeeze; stop using a puffy battery; don't charge unattended; keep it away from metal in the case.

---

## 11. Building the firmware yourself

You only need this if you change code. The prebuilt `.hex` files cover day-to-day testing.

### 11.1 One-time setup

(Full Windows walkthrough: `SETUP_PC.md`.)

1. Install ADI's **MSDK** (https://analogdevicesinc.github.io/msdk/USERGUIDE/): installers for Windows, macOS and Linux, which include the Arm compiler. Note where it installs (the `MaximSDK` folder).
2. Easiest: copy the `firmware` folder into `MaximSDK/Examples/MAX32655/OpenEar`. The Makefile then finds the SDK by itself.
   Otherwise, set `MAXIM_PATH` to the MSDK folder (see below).
3. Open the **MSDK shell** (Windows: "MSYS2 MSDK" shortcut). Linux/macOS: any terminal with the toolchain on your PATH.

### 11.2 Build and flash

```bash
cd firmware
make -j4                      # full build
make OE_BLE=0 OE_REC=0        # audio only
make AUDIO_BLOCK=128          # bigger blocks
make AUDIO_CH=LEFT            # mic on the other I2S slot
make clean                    # needed when you change these options
arm-none-eabi-objcopy -O ihex build/max32655.elf openear.hex
```

Then drag `openear.hex` onto the DAPLINK drive. (`make flash.openocd` flashes directly if OpenOCD is set up.)

### 11.3 Build errors

| Error | Fix |
|---|---|
| `arm-none-eabi-gcc: command not found` | The toolchain isn't on PATH: use the MSDK shell, or add the toolchain `bin` folder to PATH. |
| `MAXIM_PATH is not set` warning, then missing files | `export MAXIM_PATH=/path/to/MaximSDK` (Windows: `set MAXIM_PATH=C:/MaximSDK`). |
| `No rule to make target ... libCordio` or similar | Change of options: `make clean`, then rebuild. |
| `make: *** No rule to make target` | Run from the `firmware` folder (the one containing `Makefile` and `project.mk`). |
| `region FLASH overflowed` | Too much code; switch to `OE_BLE=0`. |
| A warning flood from SDK files | Normal: warnings in `PeriphDrivers`, `Cordio` and `SDHC` files come from ADI's code. The OpenEar files themselves build with no warnings. |
| Your edit doesn't change behaviour | Did you flash the new file? Check the `build:` line at boot. |

### 11.4 Changing common things

| Want to change | Where |
|---|---|
| Default volume | `DEFAULT_HPVOL` in `main.c` (higher = quieter) |
| Block size | `AUDIO_BLOCK` (make option) |
| Which mic slot | `AUDIO_CH` (make option) |
| Safety caps (band gain, output ceiling, tone) | `OE_MAX_*` in `firmware/dsp/openear_dsp.h`. **Lower them if in doubt, don't raise.** |
| Band edges | `openear_dsp.c`, `OE_XOVER_HZ` |
| Fitting rule | `oe_fit_half_gain` in `openear_dsp.c` |
| Bluetooth name | the `scanData` array in `oe_ble.c` (the letters of `OpenEar`) |
| Button actions | `main.c`, search for `button` |

After any change to `dsp/` or `cli.c`, run the PC tests (section 11.5) before flashing.

### 11.5 PC tests (no board needed)

```bash
./run_all_tests.sh            # everything (about 3 minutes); --quick skips the browser and cycle tests
cd dsp_test && make plots     # also draws the charts for your posts
```

All lines must say PASS. If you change the DSP and a test fails, undo the change or update the test on purpose.
`TEST_REPORT.md` lists every suite, what it proves, and what only the board can prove.

---

## 12. 3D-printed case

See `enclosure/README.md` for the full workflow. The short list of problems:

| Problem | Fix |
|---|---|
| `pip install` of `manifold3d` fails | Use Python 3.9 or newer; try `pip install --upgrade pip`. |
| `python3 openear_case.py` complains about a missing module | `pip install manifold3d trimesh numpy matplotlib`. |
| Holes don't line up with the buttons | Print `openear_fitcheck.stl` (10 min) first. Re-measure with calipers, edit `CONFIG`, regenerate. |
| Board doesn't fit | Increase `fit` (clearance) from 0.4 to 0.6 mm; check `board_l` / `board_w`. |
| Jack hole too small for the plug | Raise `d` in the `jacks` entries (8 mm default). |
| Lid too tight/loose | Adjust `lid_lip_gap` (0.25). Smaller = tighter. |
| Parts on top are taller than the lid clearance | Raise `over_clear` / `under_clear` after measuring. |
| Battery doesn't fit | Check your battery thickness against `battery_h` (6.5 mm). |
| Print warps or has gaps | PLA or PETG, 0.2 mm layers, 3 walls, no supports. Print the tray open-side up and the lid flat-face down. |
| USB or jack opening in the wrong wall | Edit `usb` / `jacks[].face`. Faces are `end_usb`, `end_far`, `side_left`, `side_right`. |
| Feedback howl inside the case | Keep the mic port away from the earphone cable; the case's mic holes should face away from where the cable exits. |

---

## 14. Still stuck: where to get help

1. **Re-read the matching section here**, then `BRINGUP.md`.
2. Check **`RELEASE_NOTES_0.3.1.md`** and **`TEST_REPORT.md`**: what is verified on the board, what only on the PC, and the known limitations.
3. **Ask a person**: the DigiKey TechForum thread for this project, ADI EngineerZone (ez.analog.com, "Microcontrollers" community), the MSDK's GitHub issues (github.com/analogdevicesinc/msdk).
4. **Ask any AI assistant**: paste `README.md` and `RELEASE_NOTES_0.3.1.md` first, then your console output.

What to include when asking for help (people can't help without these):

- The full console text from boot, plus the output of `stat` and `i2cscan`.
- Which `.hex` you flashed.
- What you did right before the problem, and what you expected.
- A phone photo of the setup (earphone position, cables, battery).

---

## 15. Error code cheat sheet

**MSDK errors** (printed as `... failed: -N`):

| Code | Name | Typical meaning here |
|---|---|---|
| 0 | `E_NO_ERROR` | OK |
| −1 | `E_NULL_PTR` | A required pointer was missing |
| −2 | `E_NO_DEVICE` | Device absent (`batt`: no fuel gauge found at any address) |
| −3 | `E_BAD_PARAM` | Invalid setting (bad I²S or codec setting) |
| −6 | `E_BUSY` | I²C bus busy: the I²C block saw the bus in use, so the chip was never addressed (first board, 0.2.0) |
| −7 | `E_BAD_STATE` | Wrong state (e.g. flash write refused because the image grew into the settings page) |
| −9 | `E_COMM_ERR` | Communication error (I²C to the codec) |
| −17 | `E_NOT_SUPPORTED` | A chip answered at the codec address but isn't a MAX9867 |
| −10 | `E_TIME_OUT` | Timed out (codec clocks not running) |
| −14 | `E_NONE_AVAIL` | No free DMA channel |

**Recorder errors** (`rec failed (N)`): `-(FatFS code) - offset`, where the offset is 100 (mount), 200 (open), 300 (choose file name) or 400 (write header). FatFS codes: 1 disk error, 3 not ready (no card), 7 denied/full, 10 write-protected, 13 no FAT file system.

| `rec failed` | Reads as |
|---|---|
| −101 | mount: disk error |
| −103 | mount: not ready (no card) |
| −113 | mount: no FAT file system |
| −207 | open: denied or directory full |
| −210 | open: card write-protected |
| −10 | already recording |
| −11 | no free file name |

**Bluetooth disconnect reasons**: see 8.2.
