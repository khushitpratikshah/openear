"""
Drive the real app (index.html) in headless Chromium against the REAL firmware
console code (cli.c compiled for the PC), through a mocked Web Bluetooth link
that delivers replies in 20-byte notifications like a real connection.
"""
import asyncio, subprocess, sys, re, json
from playwright.async_api import async_playwright

import os, pathlib
HERE = os.path.dirname(os.path.abspath(__file__))
APP = (pathlib.Path(HERE).parent / "app" / "index.html").as_uri()

sim = subprocess.Popen([os.path.join(HERE, "cli_sim")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
lock = asyncio.Lock()
traffic = []          # (command, reply)
tone_cmds = []        # (hz, db) as the firmware saw them
failures = []


def check(cond, msg):
    print(("  PASS  " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def fw_sync(line: str) -> bytes:
    sim.stdin.write((line + "\n").encode())
    out = b""
    while not out.endswith(b"\x01\n"):
        out += sim.stdout.read(1)
    return out[:-2]


async def fw(line: str) -> bytes:
    async with lock:
        return await asyncio.get_event_loop().run_in_executor(None, fw_sync, line)


MOCK = """
(() => {
  Math.random = () => 0;                       // shortest, deterministic test delays
  const listeners = [];
  const chr = {
    startNotifications: async () => chr,
    addEventListener: (t, fn) => { if (t === 'characteristicvaluechanged') listeners.push(fn); },
    writeValueWithResponse: async (bytes) => {
      const resp = await window.__fw(Array.from(bytes));
      for (let i = 0; i < resp.length; i += 20) {              // ATT MTU 23 -> 20-byte notifications
        const chunk = new Uint8Array(resp.slice(i, i + 20));
        listeners.forEach((fn) => fn({ target: { value: new DataView(chunk.buffer) } }));
        await new Promise((r) => setTimeout(r, 1));
      }
    },
  };
  const dev = { addEventListener() {}, gatt: { connect: async () => ({ getPrimaryService: async () => ({ getCharacteristic: async () => chr }) }) } };
  Object.defineProperty(navigator, 'bluetooth', { value: { requestDevice: async () => dev }, configurable: true });
})();
"""

# What a listener with a sloping loss can hear (dBFS thresholds)
THR = {250: -78, 500: -76, 1000: -70, 2000: -60, 4000: -50, 8000: -46}


async def main():
    async with async_playwright() as pw:
        browser = await pw.chromium.launch()
        ctx = await browser.new_context(viewport={"width": 420, "height": 900})
        page = await ctx.new_page()
        errors = []
        page.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))
        page.on("console", lambda m: errors.append(f"console.{m.type}: {m.text}") if m.type in ("error", "warning") else None)

        async def bridge(bytes_in):
            line = bytes(bytes_in).decode().strip()
            out = await fw(line)
            traffic.append((line, out.decode()))
            m = re.match(r"tone (\d+) (-?\d+)$", line)
            if m:
                tone_cmds.append((int(m.group(1)), int(m.group(2))))
            return list(out)

        await page.expose_function("__fw", bridge)
        await page.add_init_script(MOCK)
        await page.goto(APP)

        print("\n[load]")
        check(await page.title() != "", "page loads with a title: %r" % await page.title())
        check(await page.locator("#status-text").inner_text() == "Demo device", "starts in demo mode")

        print("\n[connect through the mocked Bluetooth link]")
        await page.click("#connect")
        await page.wait_for_function("document.getElementById('status-text').textContent.includes('Connected')", timeout=5000)
        check(True, "status shows Connected")
        check(await page.locator("#banner").is_hidden(), "demo banner hidden once connected")
        await page.wait_for_timeout(500)

        print("\n[tune view reads the real STATE line]")
        # put some non-default values in the firmware first
        await fw("fit 10 15 25 40 55 60")
        await page.click("#tab-tune")
        await page.wait_for_timeout(600)
        vals = [await page.input_value(f"#g{k}") for k in range(6)]
        want = json.loads(json.dumps([round(x) for x in [5.0, 6.2, 10.0, 16.2, 23.8, 28.8]]))
        # real fit values come from the half-gain rule; just require they are non-zero and rising
        iv = [int(v) for v in vals]
        check(iv[5] > iv[1] > 0, f"sliders show the device's gains: {iv}")
        check(await page.locator("#comp").input_value() in ("1", "1.5", "2", "3"), "compression dropdown set from cr=")

        print("\n[moving a slider reaches the firmware]")
        await page.evaluate("""() => { const s = document.getElementById('g3'); s.value = 20; s.dispatchEvent(new Event('input')); }""")
        await page.wait_for_timeout(500)
        st = (await fw("state")).decode()
        m = re.search(r" g=([-\d.,]+)", st)
        g = [float(x) for x in m.group(1).split(",")]
        check(abs(g[3] - 20.0) < 0.06, f"band 3 gain is now {g[3]} dB in the firmware")
        await page.evaluate("""() => { const s = document.getElementById('vol'); s.value = 12; s.dispatchEvent(new Event('input')); }""")
        await page.wait_for_timeout(500)
        st = (await fw("state")).decode()
        check("vol=6.0" in st, "volume 12 from the slider is held at +6 dB over Bluetooth (%s)" % re.search(r"vol=\S+", st).group(0))
        await page.check("#bypass")
        await page.wait_for_timeout(300)
        check("byp=1" in (await fw("state")).decode(), "bypass checkbox reaches the firmware")
        await page.uncheck("#bypass")
        await page.select_option("#comp", "3")
        await page.wait_for_timeout(300)
        check("cr=3.00,3.00" in (await fw("state")).decode(), "compression dropdown sets cr all 3")
        await page.uncheck("#nr")
        await page.wait_for_timeout(300)
        check("er=1.0,1.0" in (await fw("state")).decode(), "noise-reduction off sets er=1")

        print("\n[live view]")
        await page.click("#tab-live")
        await page.wait_for_timeout(1500)
        cpu = await page.locator("#s-cpu").inner_text()
        batt = await page.locator("#s-batt").inner_text()
        check("23.5" in cpu, f"DSP load shown from LV line: {cpu!r}")
        check("76.5%" in batt and "3.91" in batt, f"battery shown from BATT line: {batt!r}")
        check(await page.locator("#s-howl").inner_text() != "–", "howl guard field populated")

        print("\n[console tab]")
        await page.click("#tab-console")
        await page.fill("#cmd", "stat")
        await page.press("#cmd", "Enter")
        await page.wait_for_timeout(500)
        log = await page.locator("#log").inner_text()
        check("DSP time avg 24.2%" in log and "slack >= 30 samples" in log,
              "console shows stat output reassembled from 20-byte chunks")
        await page.fill("#cmd", "bogus")
        await page.press("#cmd", "Enter")
        await page.wait_for_timeout(300)
        check("unknown command" in await page.locator("#log").inner_text(), "unknown command is reported")

        print("\n[full hearing test against the firmware]")
        await fw("reset")
        await page.click("#tab-test")
        n_before = len(traffic)
        await page.click("#start-test")
        presentations = 0
        for _ in range(400):
            if await page.locator("#fit-box").is_visible():
                break
            await page.wait_for_function("!document.getElementById('heard').disabled || document.getElementById('fit-box').offsetParent !== null", timeout=15000)
            if await page.locator("#fit-box").is_visible():
                break
            # the last AUDIBLE presentation (-100 dBFS is the silent hold between beeps)
            hz, db = [t for t in tone_cmds if t[1] > -100][-1]
            presentations += 1
            if presentations % 5 == 0:
                # leave some beeps unanswered past the app's 1.2 s beep timer
                await page.wait_for_timeout(1500)
            await page.click("#heard" if db >= THR[hz] else "#not-heard")
            await page.wait_for_timeout(30)
        check(await page.locator("#fit-box").is_visible(), f"test finished after {presentations} presentations")
        check(all(db <= -20 for _, db in tone_cmds), f"no tone above -20 dBFS was ever requested (max {max(d for _, d in tone_cmds)})")
        check((await fw("state")).decode().count("tone=0") == 1, "tone is off at the end of the test")
        during = [c for c, _ in traffic[n_before:]]
        last_off = max(i for i, c in enumerate(during) if c == "tone off")
        first_tone = min(i for i, c in enumerate(during) if c.startswith("tone "))
        check(during[first_tone] == "tone 1000 -100" and all(c != "tone off" for c in during[:last_off]),
              "mic stays muted through the whole test (silent tone between beeps, 'tone off' only at the end)")
        # order: every audible tone is followed by a silent hold before any other tone
        tones = [c for c in during if c.startswith("tone ")]
        bad = 0
        for i, c in enumerate(tones):
            m2 = re.match(r"tone (\d+) (-?\d+)$", c)
            if m2 and int(m2.group(2)) > -100:
                nxt = tones[i + 1] if i + 1 < len(tones) else ""
                if not (re.match(r"tone \d+ -100$", nxt)):
                    bad += 1
        unanswered = sum(1 for i in range(len(tones) - 1)
                         if re.match(r"tone \d+ -100$", tones[i]) and tones[i] == tones[i + 1])
        check(bad == 0, f"every audible beep is followed by the silent hold, never by the mic ({bad} not)")
        check(unanswered >= presentations // 5 - 1,
              f"beeps left unanswered were ended by the app's timer with the silent hold ({unanswered} seen)")
        rows = await page.locator("#fit-table tbody tr").count()
        check(rows == 6, "fitting table has 6 rows")
        tag = await page.locator("#ag-tag").inner_text()
        check("Your result" in tag, f"audiogram tagged {tag!r}")

        print("\n[apply fitting]")
        await page.click("#apply-fit")
        await page.wait_for_timeout(800)
        st = (await fw("state")).decode()
        g = [float(x) for x in re.search(r" g=([-\d.,]+)", st).group(1).split(",")]
        check(g[5] >= g[1] and g[5] > 0, f"firmware gains after Apply: {g}")
        saved_ok = any(c == "save" and "ok" in r.lower() for c, r in traffic)
        check(saved_ok, "firmware acknowledged 'save'")

        print("\n[protocol health over the whole session]")
        bad = [(c, r) for c, r in traffic if "bad arguments" in r or "unknown command" in r]
        bad = [b for b in bad if b[0] != "bogus"]
        check(not bad, f"the app never sent a command the firmware rejected ({len(traffic)} commands) {bad[:3]}")
        cmds = sorted({c.split()[0] for c, _ in traffic})
        print("  commands used:", ", ".join(cmds))
        check(not errors, f"no JavaScript errors or warnings {errors[:3]}")

        await browser.close()
    sim.stdin.close()
    print("\n%s: %d failure(s)" % ("FAILED" if failures else "ALL APP TESTS PASSED", len(failures)))
    return 1 if failures else 0


sys.exit(asyncio.run(main()))
