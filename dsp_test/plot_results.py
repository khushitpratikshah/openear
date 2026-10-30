"""Draw the OpenEar test-bench results as PNG charts for TechForum posts.

    python3 plot_results.py results
"""
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e4e3df"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
XOVER = [250, 500, 1000, 2000, 4000]

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID, "axes.labelcolor": INK2, "xtick.color": INK2,
    "ytick.color": INK2, "text.color": INK, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.spines.top": False, "axes.spines.right": False,
    "font.size": 11, "axes.titlesize": 13, "axes.titleweight": "bold",
    "axes.titlelocation": "left", "lines.linewidth": 2,
})


def load(d, name):
    with open(os.path.join(d, name)) as f:
        rows = list(csv.reader(f))
    return rows[0], np.array(rows[1:], dtype=float)


def freq_axis(ax):
    ax.set_xscale("log")
    ax.set_xlim(50, 16000)
    ticks = [100, 250, 500, 1000, 2000, 4000, 8000, 16000]
    ax.set_xticks(ticks)
    ax.set_xticklabels(["100", "250", "500", "1k", "2k", "4k", "8k", "16k"])
    ax.set_xlabel("Frequency (Hz)")
    for x in XOVER:
        ax.axvline(x, color=GRID, lw=1, ls="--", zorder=0)


def save(fig, d, name):
    fig.tight_layout()
    fig.savefig(os.path.join(d, name), dpi=150)
    plt.close(fig)
    print("wrote", name)


def main(d):
    # 1. flat reconstruction
    _, a = load(d, "flat_response.csv")
    fig, ax = plt.subplots(figsize=(8, 3.6))
    ax.plot(a[:, 0], a[:, 1], color=BLUE)
    freq_axis(ax)
    ax.set_ylim(-1, 1)
    ax.set_ylabel("Gain (dB)")
    ax.set_title("Crossover with every band at 0 dB: flat within ±0.1 dB")
    save(fig, d, "1_flat_response.png")

    # 2. fitted response
    _, a = load(d, "fitted_response.csv")
    fig, ax = plt.subplots(figsize=(8, 3.8))
    ax.plot(a[:, 0], a[:, 1], color=BLUE)
    freq_axis(ax)
    ax.set_ylabel("Gain (dB)")
    ax.set_title("Gain for a sloping high-frequency loss (half-gain fit)")
    save(fig, d, "2_fitted_response.png")

    # 3. I/O curve
    _, a = load(d, "io_curve.csv")
    fig, ax = plt.subplots(figsize=(6, 5))
    ax.plot(a[:, 0], a[:, 0], color=INK2, lw=1, ls=":")
    ax.text(-30, -33, "unity (no gain)", color=INK2, fontsize=9, rotation=45)
    ax.plot(a[:, 0], a[:, 1], color=BLUE)
    ax.axhline(-6, color=GRID, lw=1, ls="--")
    for x, label in [(-70, "expansion\nbelow -70"), (-45, "2:1 compression\nabove -45")]:
        ax.axvline(x, color=GRID, lw=1, ls="--")
        ax.text(x + 1, -85, label, color=INK2, fontsize=9)
    ax.text(-94, -4.5, "-6 dBFS safety ceiling", color=INK2, fontsize=9)
    ax.set_xlim(-95, 0)
    ax.set_ylim(-95, 0)
    ax.set_xlabel("Input level (dBFS)")
    ax.set_ylabel("Output level (dBFS)")
    ax.set_title("Input/output at 2 kHz, 20 dB gain")
    save(fig, d, "3_io_curve.png")

    # 4. group delay
    _, a = load(d, "group_delay.csv")
    fig, ax = plt.subplots(figsize=(8, 3.6))
    ax.plot(a[:, 0], a[:, 1], color=BLUE)
    freq_axis(ax)
    ax.set_xlim(100, 12000)
    ax.set_ylabel("Delay (ms)")
    ax.set_ylim(bottom=0)
    ax.set_title("Filter-bank delay: about 1 ms in the speech band")
    save(fig, d, "4_group_delay.png")

    # 5. howl guard
    _, a = load(d, "howl_guard.csv")
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8, 5), sharex=True)
    ax1.plot(a[:, 0], a[:, 1], color=BLUE)
    ax1.set_ylabel("Output (dBFS rms)")
    ax1.set_title("Howl guard pulling a simulated feedback loop back")
    ax2.step(a[:, 0], a[:, 2], color=BLUE, where="post")
    ax2.set_ylabel("Attenuation (dB)")
    ax2.set_xlabel("Time (s)")
    save(fig, d, "5_howl_guard.png")

    # 6. before/after spectrograms
    _, a = load(d, "demo_signal.csv")
    fs = 48000
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True)
    for ax, sig, title in [(axes[0], a[:, 0], "Before (microphone)"),
                           (axes[1], a[:, 1], "After OpenEar")]:
        ax.specgram(sig + 1e-9, NFFT=1024, Fs=fs, noverlap=768, cmap="Blues",
                    vmin=-110, vmax=-30)
        ax.set_yscale("log")
        ax.set_ylim(80, 16000)
        ax.set_yticks([125, 250, 500, 1000, 2000, 4000, 8000])
        ax.set_yticklabels(["125", "250", "500", "1k", "2k", "4k", "8k"])
        ax.set_title(title)
        ax.set_xlabel("Time (s)")
        ax.grid(False)
        ax.text(0.1, 9500, "soft sweep", color=INK, fontsize=9)
        ax.text(2.1, 9500, "loud sweep", color=INK, fontsize=9)
    axes[0].set_ylabel("Frequency (Hz)")
    save(fig, d, "6_spectrogram_before_after.png")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "results")
