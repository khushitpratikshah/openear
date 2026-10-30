#!/usr/bin/env python3
"""
OpenEar regression check for the day-1 board bug (codec probe failed with
-6, "I2C bus busy"). It reads the firmware source and fails if the fix is
undone. It can't prove the hardware works; only the board's boot log can.

Checks:
  1. audio_init(): bus_init() -> max9867_init() -> Microphone_Power(),
     the same codec-before-PMIC order as ADI's working I2S example.
  2. The codec powers up MUTED: max9867_init() writes VOL 0x40 (not ADI's
     loud 0x04) before it enables the DAC, and is powered down again if
     audio_init() fails after it.
  3. max9867_init() doesn't reset a bus that is already running, and starts
     a stopped one through bus_init() (thresholds the PMIC driver needs).
  4. Codec transfers retry once after bus_recover() on E_BUSY.
  5. main(): audio_init() runs before oe_ble_init(); the real volume is
     written after Bluetooth init, just before audio_start(), and audio is
     not started if that write failed. sidetone/hpvol need a ready codec.
  8. The PMIC driver (no timeouts) is only called once the PMIC answered a
     timed SDK probe and the bus is set up for it.
  6. The fuel gauge doesn't start the bus with its own MXC_I2C_Init().
  7. The shared bus runs with RX/TX FIFO thresholds of 1: the PMIC driver's
     register-level reads wait forever for the RX flag otherwise.

Run: python3 firmware_test/check_init_order.py   (exit code 0 = pass)
"""
import pathlib
import re
import sys

FW = pathlib.Path(__file__).resolve().parent.parent / "firmware"
fails = 0


def check(ok, what):
    global fails
    print(("  PASS  " if ok else "  FAIL  ") + what)
    if not ok:
        fails += 1


def body(src, func):
    """Text of a C function body (naive brace matching, fine for our code)."""
    m = re.search(r"\n[^\n;]*\b" + re.escape(func) + r"\([^;{\n]*\)\s*\{", src)
    if not m:
        return ""
    i, depth = m.end(), 1
    while depth and i < len(src):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    return src[m.end():i]


def code(path):
    """Source text with comments removed (string literals kept), so a check
    can't be satisfied by commented-out code."""
    src = pathlib.Path(path).read_text()
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1]); i = j + 1
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2); j = n if j < 0 else j + 2
            out.append("\n" * src.count("\n", i, j)); i = j
        elif src.startswith("//", i):
            j = src.find("\n", i); j = n if j < 0 else j
            i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def order(text, *names):
    pos = [text.find(n) for n in names]
    return all(p >= 0 for p in pos) and pos == sorted(pos)


audio = code(FW / "audio_io.c")
init = body(audio, "audio_init")
check(order(init, "bus_init()", "max9867_init(", "Microphone_Power("),
      "audio_init: bus, then codec, then PMIC mic power")
check(init.count("codec_off()") >= 2, "audio_init: codec powered down on failure")
check("max9867_shutdown()" in body(audio, "codec_off") and "max9867_set_volume(0x40)" in body(audio, "codec_off"),
      "codec_off: mutes and shuts the codec down")
check(order(init, "bus_ready_for_pmic()", "i2c_probe(PMIC_ADDR)", "Microphone_Power("),
      "audio_init: PMIC driver only after a timed probe")
check(re.search(r"(?m)^\s*Microphone_Power\(", init) is None,
      "audio_init: mic power return code is used, not ignored")

codec = code(FW / "max9867.c")
cinit = body(codec, "max9867_init")
check(re.search(r"if\s*\(\(i2c_inst->ctrl\s*&", cinit) is not None
      and 0 <= cinit.find("i2c_inst->ctrl") < cinit.find("(err = bus_init())"),
      "max9867_init: leaves a running bus alone")
check("bus_init()" in cinit and "MXC_I2C_Init(" not in cinit,
      "max9867_init: a stopped bus is started via bus_init()")
vol = cinit.find("MAX9867_10_VOL_LEFT, 0x40")
check(vol >= 0 and "0x04 << MAX9867_VOL_POS" not in cinit and vol < cinit.find("MAX9867_17_PWR_SYS,\n"),
      "max9867_init: volume muted (0x40) before the DAC is enabled")
xfer = body(codec, "xfer")
check("E_BUSY" in xfer and "bus_recover()" in xfer,
      "max9867.c: transfers recover a busy bus once")
check("return MXC_I2C_MasterTransaction(&i2c_req);" not in codec,
      "max9867.c: every register access goes through xfer()")

main = code(FW / "main.c")
m = body(main, "main")
check(order(m, "bus_report(", "audio_init(", "oe_ble_init()", "audio_restart(hpvol)"),
      "main: pin report, audio init, bluetooth, then start audio and unmute (audio_restart)")
check("audio_codec_ready()" in body(main, "hw_set_hpvol") and "audio_codec_ready()" in body(main, "hw_sidetone")
      and ".sidetone = hw_sidetone" in main and ".set_hpvol = hw_set_hpvol" in main,
      "main: sidetone/hpvol refused unless the codec is ready")
check(re.search(r"if\s*\(!err\s*&&\s*\(err\s*=\s*audio_restart\(hpvol\)\)", m) is not None
      and "max9867_set_volume(" not in m.replace("max9867_set_volume(0x40)", ""),
      "main: audio starts only if init succeeded, and main never unmutes the codec itself")

fg = code(FW / "fuelgauge.c")
check("MXC_I2C_Init(" not in fg, "fuelgauge.c: no second bus init")

bus = code(FW / "codec_bus.c")
start = body(bus, "bus_start")
check("MXC_I2C_SetRXThreshold(BUS, 1)" in start and "MXC_I2C_SetTXThreshold(BUS, 1)" in start,
      "codec_bus.c: FIFO thresholds 1, as the PMIC driver needs")
check("bus_start()" in body(bus, "bus_init") and "bus_start()" in body(bus, "bus_recover"),
      "codec_bus.c: init and recovery both use bus_start()")

# ---- 0.3.0: timekeeping, timeouts, volume safety on failure paths ----
import glob
own = [f for f in glob.glob(str(FW / "*.c")) + glob.glob(str(FW / "ble" / "*.c"))]
dwt_users = [pathlib.Path(f).name for f in own
             if "DWT->CYCCNT" in code(f) and not f.endswith("oe_time.c")]
check(not dwt_users, f"nothing but oe_time.c reads the DWT cycle counter {dwt_users}")
nm = body(main, "now_ms")
check("oe_millis()" in nm and "s.blocks" in nm and "now_ms()" in m,
      "main loop clock: the hardware timer, falling back to the audio block count")
link = code(FW / "dsp_link.c")
check("spins" in body(link, "wait_taken") and "spins" in body(audio, "audio_start"),
      "parameter hand-over and audio start waits have a loop-count limit too")
ble = code(FW / "ble" / "oe_ble.c")
check("while (!wutTrimComplete) {}" not in ble, "Bluetooth crystal trim wait has a timeout")
rs = body(audio, "audio_restart")
check(re.search(r"err = audio_start\(\);\s*if \(err == E_NO_ERROR\) \{\s*err = audio_set_volume\(", rs) is not None
      and "max9867_set_volume(0x40)" in rs,
      "audio_restart: unmutes only after a good start, mutes on failure")
sv = body(audio, "audio_set_volume")
check(re.search(r"if \(!running\) \{\s*return E_NO_ERROR;", sv) is not None and sv.find("!running") < sv.find("max9867_set_volume("),
      "audio_set_volume: the codec is never unmuted while audio isn't running (hpvol, load)")
cb = body(audio, "dma_cb")
check(re.search(r"\+\+tx_without_rx >= 2\) \{\s*memset\(tx_buf\[!playing\], 0", cb) is not None
      and cb.count("memset(tx_buf") == 1,
      "TX interrupt silences a buffer only after 2 output blocks with no input block (a stalled input)")
ac = body(audio, "audio_check")
check("tx_blocks" in ac and "stats.blocks" in ac, "audio watchdog watches both the RX and the TX stream")
check(m.count("safe_hpvol(") >= 1 and "safe_hpvol(pr.hp_volume)" in main,
      "stored volume codes are clamped like typed ones (boot and 'load')")
check("MXC_WDT_ResetTimer" in m and "MXC_WDT_Enable" in m, "hardware watchdog enabled and fed by the main loop")
check(order(m, "MXC_WDT_Disable(", "Console_Init(", "MXC_WDT_Enable("),
      "watchdog disabled before the slow boot steps, enabled only after them")
check("audio_check(" in m, "main loop restarts audio if its interrupt stops")

print("\nALL INIT-ORDER CHECKS PASSED" if not fails else f"\n{fails} CHECK(S) FAILED")
sys.exit(1 if fails else 0)
