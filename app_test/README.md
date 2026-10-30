# App test: the phone app against the real console code

`test_app.py` opens `app/index.html` in a headless Chromium browser and
connects it, through a *mocked* Bluetooth link, to `cli_sim`: the firmware's real
`cli.c` and DSP compiled for the PC. Replies arrive in 20-byte pieces, like a
real Bluetooth link. The test then drives the app through connect, tuning,
live levels, the console, a full simulated hearing test, and apply-and-save,
and fails if the app ever sends a command the firmware rejects or throws a
JavaScript error.

Run it after changing `app/index.html`, `firmware/cli.c`, or any console output.

```bash
pip install playwright
playwright install chromium
cd app_test
make test            # builds cli_sim, then runs test_app.py (about a minute)
```

It does not test real radio behaviour; only the board can do that.
