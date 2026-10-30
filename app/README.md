# OpenEar Fitting app

A phone web app that connects to OpenEar over Bluetooth: hearing test,
fitting, band tuning, live levels and a console. It also runs a simulated
device, so every screen works without the board.

## Using it with the real device

Web Bluetooth works in **Chrome on Android, Windows and macOS** (and Edge).
It does not work in Safari or any iOS browser; on an iPhone, use the free
"Bluefy" browser.

The page must be served over https:

1. Put this repository on GitHub.
2. Settings → Pages → deploy from the `main` branch, folder `/ (root)`. The
   `index.html` in the repository root forwards to this app.
3. Open `https://<you>.github.io/<repo>/` on your phone and press **Connect**.

`index.html` is the standalone page; `openear-app.html` is the same app
without the html/head wrapper (used for the hosted preview).

## Hearing test notes

* Without calibration, loss is measured against your own best pitch. That
  captures the *shape* of a loss but not its absolute level.
* To calibrate: have someone with normal hearing take the test once with the
  same earphone, phone and `hpvol`, then tap "Use this result as the
  normal-hearing reference". Results after that are in dB HL for that setup.
* Test tones are capped at −20 dBFS by the firmware.
* Not a clinical audiogram. See an audiologist for diagnosis.
