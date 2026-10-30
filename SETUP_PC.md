# Setting up your PC for OpenEar

This guide assumes **Windows 10 or 11** (if yours is different, see section 9).
It is ordered by urgency: **Part A is all you need on the day the board arrives.**
Parts B and C are only needed when you want to change the code, run the tests or print the case.

| Part | What | Needed when | Time |
|---|---|---|---|
| **A** | Serial terminal + flashing the prebuilt `.bin` | Day 1 | 10 min |
| **B** | Phone / Chrome for the app, Audacity for the latency test | Day 3–4 | 15 min |
| **C** | ADI MSDK (compiler) to rebuild the firmware | Only if you edit code | 30–45 min |
| **D** | Python for the 3D case; WSL for the PC tests | Day 4–5 | 20 min |

Do A today. Do the rest when you get to that day. There is no point installing big tools
before you know the board works.

---

## Part A: the minimum (no compiler at all)

### A1. Unzip the project properly

Right-click `OpenEar.zip` → **Extract All…** to a short path with no spaces, for example
`C:\OpenEar`. Do not run files from inside the zip (dragging a `.hex` out of a zip window fails).

### A2. Install a serial terminal

Install **Tera Term** (https://teratermproject.github.io/) or **PuTTY** (https://www.putty.org/).
Either one is fine. You do not need an IDE for this.

### A3. Plug in the board and check three things

Use a **USB cable that carries data** (many cables are charge-only) and plug it into the
board's USB connector. Press **SW1** if nothing lights up.

1. **A drive named DAPLINK** (or similar) appears in File Explorer. This is where you drop `.bin` files.
2. **A COM port appears.** Open Device Manager → *Ports (COM & LPT)*. Look for "mbed Serial Port",
   "DAPLink CMSIS-DAP" or "USB Serial Device" and note the number, e.g. `COM5`.

If the COM port has a yellow warning icon or doesn't appear, see `TROUBLESHOOTING.md` §4.3.
On Windows 10/11 no driver or admin rights should be needed for this step.

### A4. Open the console

Tera Term: *File → New connection → Serial → your COMx*, then *Setup → Serial port*:
**115200 baud, 8 data, no parity, 1 stop**. (PuTTY: *Serial*, line `COM5`, speed `115200`.)
Press **SW5** (reset) on the board to see the banner, then type `help`.

### A5. Flash

Drag `C:\OpenEar\firmware\prebuilt\openear-audio-only.bin` onto the DAPLINK drive. Wait 10–20 s
until the drive disappears and returns. Then follow `BRINGUP.md`. (`.bin` is the format confirmed
on this board; the `.hex` files are the same programs.)

If the drive comes back after 2–3 s with a `FAIL.TXT` saying "An error occurred during the
transfer" and the old program keeps running, the board's DAPLink firmware needs updating once:
hold **SW6** while plugging in USB, a **MAINTENANCE** drive appears, drop ADI's
`max32625_max32655fthr_if_crc_swd_v1.0.3.bin` (from
https://github.com/analogdevicesinc/max32625pico-firmware-images) onto it, then replug normally.
`DETAILS.TXT` on the DAPLINK drive should then show Git SHA `34fe95…`. Full steps:
`TROUBLESHOOTING.md` §4.2.

**Part A done.** You can stop here and go build the thing.

---

## Part B: phone, browser and measuring tools

### B1. The phone app

The app needs **Web Bluetooth**, which works in:

- **Android:** Chrome. Nothing to install.
- **iPhone/iPad:** Safari does *not* support it. Install **Bluefy** (free, App Store).
- **Windows/Mac/Linux desktop:** Chrome or Edge (not Firefox, not Safari).

Web Bluetooth also requires the page to be served over **https** (or from `localhost`).
Two ways:

1. **On your PC for testing** (nothing to install if you have Python, see D1):
   ```
   cd C:\OpenEar\app
   py -m http.server 8000
   ```
   then open http://localhost:8000/index.html in desktop Chrome. (A phone cannot reach `localhost` on your PC.)
2. **For your phone: GitHub Pages** (free https hosting). `TROUBLESHOOTING.md` §8.7 has the steps.
   You need a free GitHub account. Upload `app/index.html` to a new public repo, then
   *Settings → Pages → Deploy from branch → main*. The URL it gives you is what you open on the phone.

You can also try the app **without the board**: it has a simulated demo device.

### B2. Phone tools for debugging Bluetooth

Install **nRF Connect for Mobile** (Nordic, free). If the app can't find the board, nRF Connect
should still show a device called `OpenEar`. That tells you whether it's the board or the app.

### B3. Audacity (latency and recording tests)

Install **Audacity** (https://www.audacityteam.org/). It is used in the latency measurement and to
look at the WAV files the SD recorder writes (left = raw mic, right = processed output).

### B4. A text editor

Windows Notepad is not great for these files. **VS Code** (https://code.visualstudio.com/) is
free, shows Markdown nicely (Ctrl+Shift+V) and is useful for Part C too.

---

## Part C: building the firmware yourself (optional)

Only do this when you actually want to change the code. The prebuilt hex files are built from this
exact source. The firmware was verified to build cleanly with **both** a current GNU Arm compiler
(15.2) and **GCC 10.3.1, the compiler ADI's installer provides** for this chip family, using an MSDK
checked out on 2026-10-14. If your installer is an older release and a build fails on an SDK
file (not on an OpenEar file), see C6.

### C1. Download the MSDK installer

Go to ADI's MSDK page: https://analogdevicesinc.github.io/msdk/USERGUIDE/ , section
**Installation**, and download the **Windows installer**. You'll need admin rights.

### C2. Run the installer

- Install to a **short path without spaces** (the default, `C:\MaximSDK`, is good).
- When it asks for components, keep these ticked:
  - **Core** (SDK, Libraries, Examples)
  - **Arm Embedded GNU toolchain**
  - **OpenOCD** (optional, only for `make flash`; you can drag-and-drop without it)
  - **MSYS2 / MinGW** (this gives you `make` and a Unix-like shell)
- You do **not** need the RISC-V toolchain (that's for a different core), or the Keil/IAR options.
- Component names can change between releases; if they look different, install the defaults.

### C3. Open the MSDK shell

Start menu → the **MSYS2 MSDK** shortcut (named *MaximSDK shell* / *MSYS2 MSDK* depending on version).
Ordinary Command Prompt or PowerShell will not have `make` on the PATH.

Check it:

```bash
arm-none-eabi-gcc --version      # first line should mention GNU Arm Embedded Toolchain
make --version
echo $MAXIM_PATH                 # should print the MaximSDK folder
```

If `echo $MAXIM_PATH` is empty:

```bash
export MAXIM_PATH=/c/MaximSDK     # adjust if you installed elsewhere
```

### C4. Check the toolchain with ADI's own example first

Before touching OpenEar, prove the install works with something ADI already tested:

```bash
cd $MAXIM_PATH/Examples/MAX32655/Hello_World
make -j4
```

It should end with a line about the memory used and no `Error`. If this fails, the install is the
problem, not OpenEar. Fix this before moving on.

### C5. Build OpenEar

The simplest layout is to copy the project into the SDK's examples:

```bash
cp -r /c/OpenEar/firmware $MAXIM_PATH/Examples/MAX32655/OpenEar
cd $MAXIM_PATH/Examples/MAX32655/OpenEar
make -j4                                   # full firmware
arm-none-eabi-objcopy -O ihex build/max32655.elf openear.hex
```

(Or leave it at `C:\OpenEar\firmware` and run `make` there with `MAXIM_PATH` set as above.)

Variants (run `make clean` when you change an option):

| Command | Result |
|---|---|
| `make -j4` | Full: audio + Bluetooth + SD recorder |
| `make OE_BLE=0 OE_REC=0` | Audio only, the safest build |
| `make AUDIO_BLOCK=128` | Bigger blocks (use if `stat` shows late blocks) |
| `make AUDIO_CH=LEFT` | Mic from the other I²S slot (use if `raw` is zero) |

Success looks like a memory summary with `FLASH` and `RAM` lines and no `Error`. Warnings from files
under `PeriphDrivers`, `Cordio` or `SDHC` are ADI's, not yours. Drag the resulting `openear.hex`
onto the DAPLINK drive. After flashing, the boot banner's `build:` line tells you it's your build.

`make flash.openocd` programs the board directly if you installed OpenOCD, but drag-and-drop is
simpler and always works.

### C6. If the build fails

| What you see | What to do |
|---|---|
| `arm-none-eabi-gcc: command not found` | You're not in the MSDK shell (C3). |
| `MAXIM_PATH` problems / missing `.mk` files | `export MAXIM_PATH=/c/MaximSDK` (C3), or use the copy-into-Examples layout (C5). |
| `make: *** No rule to make target` | Run `make` in the folder containing `Makefile` and `project.mk`. |
| Errors in `ble/` or `libCordio` after changing options | `make clean`, then rebuild. |
| Errors inside an ADI file (not `main.c`, `audio_io.c`, `cli.c`, …) | Your installed SDK is probably older than the one used here. Use the prebuilt hex for now, and ask CircuitDigest/ADI which MSDK version they recommend. Updating the MSDK from the installer's *Update* option, or from the GitHub repo https://github.com/analogdevicesinc/msdk , is the fix. |
| `region FLASH overflowed` | Build audio only: `make OE_BLE=0 OE_REC=0`. |

`TROUBLESHOOTING.md` §11 has more.

### C7. Optional: VS Code with the board

Not required. If you want it: install VS Code, then the extensions **C/C++** (Microsoft) and
**Cortex-Debug**. ADI's user guide describes the MSDK settings for VS Code (it needs the
`MAXIM_PATH` setting pointing at your MaximSDK folder). Debugging uses the board's built-in
debugger over the same USB cable and needs OpenOCD. Building from the MSDK shell is simpler and is
what this guide verified.

---

## Part D: Python, PC tests and the 3D case

### D1. Python

Install Python 3.11 or newer from https://www.python.org/downloads/ . On the first screen,
**tick "Add python.exe to PATH"**. Verify in a new Command Prompt:

```
py --version
py -m pip --version
```

### D2. The case generator

```
py -m pip install manifold3d trimesh numpy matplotlib
cd C:\OpenEar\enclosure
py openear_case.py
```

It writes `openear_tray.stl`, `openear_lid.stl` and `openear_fitcheck.stl` (plus a preview) into
`enclosure\stl`. **Edit the `CONFIG` numbers first** after you've measured the board with calipers
(see `enclosure/README.md`). Print `openear_fitcheck.stl` first.

If `pip install manifold3d` fails, check `py --version` (needs a recent Python, 64-bit). The pre-made
STLs in `enclosure/stl` are a fallback.

### D3. A slicer

Install the slicer for your printer (**PrusaSlicer**, **Cura** or **Bambu Studio**). Settings used in
the design: PLA or PETG, 0.2 mm layers, no supports.

### D4. PC tests (optional, but run them if you change `dsp/` or `cli.c`)

The tests are plain C and need a C compiler and `make`. The easiest way on Windows is **WSL**
(Windows Subsystem for Linux):

1. Open PowerShell as administrator: `wsl --install`, restart, create a username.
2. In the Ubuntu window:
   ```bash
   sudo apt update && sudo apt install -y build-essential python3 python3-pip python3-matplotlib
   cd /mnt/c/OpenEar/dsp_test && make plots      # expect 19 PASS
   cd ../cli_test && make                         # expect 51 PASS
   ```

(You can instead run the tests inside the MSDK shell if it has a native `gcc`; but WSL is the sure thing.)

The optional browser test of the app (`app_test/`) is described in its own README
(`pip install playwright` and `playwright install chromium`, then `make test`). Skip it unless
you change `app/index.html`.

### D5. Git (optional)

Git for Windows (https://git-scm.com/) is useful to back up your work and for GitHub Pages.
Nothing here requires it.

---

## 9. Other operating systems

**macOS:** the MSDK has a macOS installer on the same page. Serial: `ls /dev/tty.usbmodem*` and
`screen /dev/tty.usbmodemXXXX 115200`. The DAPLINK drive appears on the Desktop. Python,
Audacity, Chrome work as above. `make` for the PC tests is available after
`xcode-select --install`.

**Ubuntu/Linux:** the MSDK has a Linux installer. Serial: `ls /dev/ttyACM*`,
`screen /dev/ttyACM0 115200`, and add yourself to the `dialout` group
(`sudo usermod -aG dialout $USER`, then log out and in). PC tests:
`sudo apt install build-essential python3-matplotlib`.

**Chromebook / locked-down school PC:** you can still do Part A if the machine lets you save a
`.hex` file to the DAPLINK drive and use a web-based serial terminal in Chrome (for example the
"Serial Terminal" web apps that use Web Serial). Install Part C tools only on a computer you own.

---

## 10. Checklist: is my PC ready?

- [ ] `OpenEar.zip` extracted to a short path (A1)
- [ ] Serial terminal installed; board shows a COM port and the DAPLINK drive (A3)
- [ ] Banner appears when I press SW5 (A4)
- [ ] I can drag `openear-audio-only.bin` onto DAPLINK (A5)
- [ ] Chrome (Android) or Bluefy (iPhone) installed (B1)
- [ ] *Optional:* `arm-none-eabi-gcc --version` and the Hello_World build both work in the MSDK shell (C3, C4)
- [ ] *Optional:* `py -m pip install manifold3d trimesh numpy matplotlib` works (D2)

## 11. Common install problems

| Problem | Fix |
|---|---|
| No DAPLINK drive and no COM port | Charge-only cable. Try another cable, another USB port, no hub. Press SW1. |
| COM port number changes each time | Normal when you change USB ports. Check Device Manager again. |
| Tera Term says "port in use" | Another program (a second terminal, the Arduino IDE) holds the port. Close it. |
| Installer blocked by Windows SmartScreen | *More info → Run anyway*, but only for the installer downloaded from ADI's own page. |
| Installer or shell fails on a path with spaces or non-ASCII characters | Install to `C:\MaximSDK`; keep the project in `C:\OpenEar`. |
| `make` not found in Command Prompt | Use the MSDK shell instead of Command Prompt/PowerShell. |
| Antivirus quarantines `arm-none-eabi-gcc.exe` | Allow the MaximSDK folder in your antivirus, reinstall if files were removed. |
| `py` not recognised | Python isn't on PATH. Re-run its installer, choose *Modify*, tick "Add to PATH". |
| Web Bluetooth button does nothing in the app | Not Chrome/Edge/Bluefy, or the page isn't https/localhost, or Bluetooth is off on the PC. |
| Phone can't open `localhost` page | `localhost` is the phone itself. Use GitHub Pages (B1). |

If you get stuck, copy the **exact error text** and the command you ran; that is what anyone helping
you (or an AI assistant) needs. See `TROUBLESHOOTING.md` §14 for where to ask.
