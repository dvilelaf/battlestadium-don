#!/usr/bin/env python3
"""Per-second checks of a 16-bit PCM stereo WAV: L/R lag, peak correlation, clicks and dropouts.

    uv run --no-project --with numpy python -I tools/verify/wav_check.py FILE [--json]

For every 1-second window (the last one may be shorter) the tool prints:

* `lag`: the offset in samples that maximises the cross-correlation of L and R, searched within
  +-50 ms. 0 means the channels are aligned; a positive lag means R is later than L (R[n] = L[n-lag]).
* `peak_corr`: the correlation coefficient (-1..1) at that lag, normalised by the energy of the window.
* `clicks`: runs of samples where |x[n]-x[n-1]| is more than 8 times the window's median |x[n]-x[n-1]|
  and above 2000, counted per channel. A single spike gives one click (its onset and its return are
  consecutive samples and form one run).
* `dropouts`: runs of frames where L and R are both exactly zero for 5 ms or more. Only windows that
  have at least one non-zero sample are checked, so a silent window is not a dropout.

The last line is a summary. `--json` prints the same figures as JSON instead of the table.
Mono or non-stereo input is reported and not measured (exit code 2).

`--echo` adds, for every 1-second window and each channel separately, the strongest peak of the
normalised autocorrelation (divided by the zero-lag value) at lags of 10 to 500 ms. The search starts
at the later of 10 ms and the first lag where the autocorrelation drops below 0.3 (the main lobe is
skipped). It prints `second`, `channel`, the lag in ms and the peak value, then a summary: the most
common lag (rounded to 1 ms) among the channel-windows whose peak is above 0.3, and how many there are.
With `--json` the same data is under `echo`.

Only the standard library and numpy are used. The WAV files are game output and stay under work/
(AGENTS.md rule 1); the figures may be quoted.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import wave
from collections import Counter
from pathlib import Path

import numpy as np

MAX_LAG_MS = 50
CLICK_RATIO = 8.0
CLICK_MIN = 2000
DROPOUT_MS = 5
ECHO_MIN_LAG_MS = 10
ECHO_MAX_LAG_MS = 500
ECHO_LOBE_END = 0.3
ECHO_PEAK_MIN = 0.3


def load(path: Path) -> tuple[int, int, np.ndarray]:
    """Returns (channels, sample rate, samples as an int64 array of shape (frames, channels))."""
    with wave.open(str(path), "rb") as w:
        channels = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        frames = w.readframes(w.getnframes())
    if width != 2:
        raise SystemExit(f"{path}: {8 * width}-bit samples; only 16-bit PCM is supported")
    samples = np.frombuffer(frames, dtype="<i2").astype(np.int64)
    count = len(samples) // channels
    return channels, rate, samples[: count * channels].reshape(count, channels)


def count_runs(mask: np.ndarray, min_len: int) -> int:
    """Number of runs of True in `mask` that are at least `min_len` long."""
    if not mask.any():
        return 0
    padded = np.concatenate(([False], mask, [False])).astype(np.int8)
    edges = np.diff(padded)
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    return int(np.sum(ends - starts >= min_len))


def click_mask(x: np.ndarray) -> np.ndarray:
    """True at the samples whose step from the previous sample is a click (see the module docstring)."""
    mask = np.zeros(len(x), dtype=bool)
    if len(x) < 2:
        return mask
    step = np.abs(np.diff(x))
    limit = max(CLICK_RATIO * np.median(step), CLICK_MIN)
    mask[1:] = step > limit
    return mask


def best_lag(left: np.ndarray, right: np.ndarray, rate: int) -> tuple[int, float]:
    """Lag in samples (within +-MAX_LAG_MS) of the peak of sum(L[n] * R[n+lag]), and its normalised value."""
    energy = float(np.sum(left * left) * np.sum(right * right))
    if energy == 0.0:
        return 0, 0.0
    max_lag = int(round(rate * MAX_LAG_MS / 1000))
    nfft = 1 << (2 * len(left) - 1).bit_length()
    cross = np.fft.irfft(np.conj(np.fft.rfft(left, nfft)) * np.fft.rfft(right, nfft), nfft)
    lags = np.arange(-max_lag, max_lag + 1)
    values = cross[lags % nfft]
    best = int(np.argmax(values))
    return int(lags[best]), float(values[best] / math.sqrt(energy))


def analyse(samples: np.ndarray, rate: int) -> list[dict]:
    """One dict per 1-second window, in order."""
    min_dropout = max(1, math.ceil(rate * DROPOUT_MS / 1000))
    windows = []
    for index, start in enumerate(range(0, len(samples), rate)):
        block = samples[start : start + rate]
        left = block[:, 0]
        right = block[:, 1]
        lag, peak = best_lag(left.astype(np.float64), right.astype(np.float64), rate)
        clicks = count_runs(click_mask(left), 1) + count_runs(click_mask(right), 1)
        dropouts = 0
        if block.any():
            dropouts = count_runs((left == 0) & (right == 0), min_dropout)
        windows.append(
            {
                "second": index,
                "frames": len(block),
                "lag_samples": lag,
                "lag_ms": round(lag * 1000 / rate, 3),
                "peak_corr": round(peak, 4),
                "clicks": clicks,
                "dropouts": dropouts,
            }
        )
    return windows


def echo_peak(x: np.ndarray, rate: int) -> tuple[int | None, float]:
    """Lag in samples and normalised value of the strongest autocorrelation peak of one channel's window.

    The lags searched are ECHO_MIN_LAG_MS..ECHO_MAX_LAG_MS, starting after the main lobe. Returns
    (None, 0.0) when there is nothing to measure (silence, or no lag past the main lobe).
    """
    energy = float(np.dot(x, x))
    if energy == 0.0:
        return None, 0.0
    min_lag = int(round(rate * ECHO_MIN_LAG_MS / 1000))
    max_lag = int(round(rate * ECHO_MAX_LAG_MS / 1000))
    nfft = 1 << (2 * len(x) + max_lag).bit_length()
    spectrum = np.fft.rfft(x, nfft)
    acf = np.fft.irfft(spectrum * np.conj(spectrum), nfft)[: max_lag + 1] / energy
    below = np.flatnonzero(acf[1:] < ECHO_LOBE_END)
    lobe_end = int(below[0]) + 1 if below.size else max_lag + 1
    start = max(min_lag, lobe_end)
    if start > max_lag:
        return None, 0.0
    best = int(np.argmax(acf[start:]))
    return start + best, float(acf[start + best])


def echo_windows(samples: np.ndarray, rate: int) -> list[dict]:
    """One dict per channel and 1-second window (second, then channel L before R), for --echo."""
    rows = []
    for index, start in enumerate(range(0, len(samples), rate)):
        block = samples[start : start + rate].astype(np.float64)
        for channel, name in enumerate(("L", "R")):
            lag, peak = echo_peak(block[:, channel], rate)
            rows.append(
                {
                    "second": index,
                    "channel": name,
                    "lag_samples": lag,
                    "lag_ms": None if lag is None else round(lag * 1000 / rate, 3),
                    "peak": round(peak, 4),
                }
            )
    return rows


def echo_summary(echo: list[dict]) -> dict:
    """The most common lag (rounded to 1 ms) among channel-windows with a peak above ECHO_PEAK_MIN."""
    lags = [round(e["lag_ms"]) for e in echo if e["lag_ms"] is not None and e["peak"] > ECHO_PEAK_MIN]
    common = Counter(lags).most_common(1)
    return {
        "most_common_lag_ms": common[0][0] if common else None,
        "windows_above": len(lags),
        "channel_windows": len(echo),
        "threshold": ECHO_PEAK_MIN,
    }


def summary(windows: list[dict]) -> dict:
    lags = sorted({w["lag_samples"] for w in windows})
    return {
        "windows": len(windows),
        "lag_samples_min": lags[0] if lags else None,
        "lag_samples_max": lags[-1] if lags else None,
        "min_peak_corr": min((w["peak_corr"] for w in windows), default=None),
        "clicks": sum(w["clicks"] for w in windows),
        "dropouts": sum(w["dropouts"] for w in windows),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Per-second L/R lag, correlation, clicks and dropouts of a WAV.")
    parser.add_argument("file", type=Path)
    parser.add_argument("--json", action="store_true", help="print JSON instead of the table")
    parser.add_argument("--echo", action="store_true", help="add the echo search (10..500 ms) per channel and second")
    args = parser.parse_args(argv)

    channels, rate, samples = load(args.file)
    if channels == 1:
        print(f"{args.file}: mono input; this check needs stereo (2 channels), nothing was measured", file=sys.stderr)
        return 2
    if channels != 2:
        print(f"{args.file}: {channels} channels; this check needs stereo (2 channels)", file=sys.stderr)
        return 2

    windows = analyse(samples, rate)
    stats = summary(windows)
    echo = echo_windows(samples, rate) if args.echo else None
    if args.json:
        result = {"file": args.file.name, "rate": rate, "windows": windows, "summary": stats}
        if echo is not None:
            result["echo"] = {"windows": echo, "summary": echo_summary(echo)}
        print(json.dumps(result, indent=2))
        return 0

    print("second  lag_samples  lag_ms   peak_corr  clicks  dropouts")
    for w in windows:
        print(
            f"{w['second']:>6}  {w['lag_samples']:>11}  {w['lag_ms']:>7.3f}  {w['peak_corr']:>9.4f}"
            f"  {w['clicks']:>6}  {w['dropouts']:>8}"
        )
    print(
        f"summary: {stats['windows']} windows; lag {stats['lag_samples_min']}..{stats['lag_samples_max']} samples; "
        f"min peak_corr {stats['min_peak_corr']}; clicks {stats['clicks']}; dropouts {stats['dropouts']}"
    )
    if echo is not None:
        print(f"echo: lags {ECHO_MIN_LAG_MS}..{ECHO_MAX_LAG_MS} ms, past the main lobe")
        print("second  channel    lag_ms      peak")
        for e in echo:
            lag = "-" if e["lag_ms"] is None else f"{e['lag_ms']:.3f}"
            print(f"{e['second']:>6}  {e['channel']:>7}  {lag:>8}  {e['peak']:>9.4f}")
        found = echo_summary(echo)
        lag = "no lag" if found["most_common_lag_ms"] is None else f"{found['most_common_lag_ms']} ms"
        print(
            f"echo summary: most common lag {lag}; {found['windows_above']} of {found['channel_windows']} "
            f"channel-windows have a peak above {ECHO_PEAK_MIN}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
