#!/usr/bin/env python3
"""Where does a reference sound occur inside a game recording, and is there a second copy?

    uv run --no-project --with numpy python -I tools/verify/wav_copies.py REF.wav GAME.wav [options]

Both files are 16-bit PCM at the same sample rate. Each is mixed to one channel (the mean of the
channels) for the search. An excerpt of the reference (`--length` seconds, 3 by default) starting at
`--ref-start` seconds (10 by default) is compared with the whole game recording by normalised
cross-correlation, computed with FFTs: at every offset the dot product is divided by the norms of
the excerpt and of the game window under it, so the value is between -1 and 1 and 1 is an exact copy.

The strongest `--peaks` peaks (5 by default) are printed with their position in the game recording
in seconds and their height. Two peaks closer than 5 ms are the same peak. The distance between the
two highest peaks is printed in ms with the ratio of their heights. A double copy (the same sound
mixed in again with an echo or a delay) gives a second peak at the delay with a smaller height.

Then `--excerpts N` (5 by default) excerpts spread evenly over the reference are analysed the same
way, one line each. A consistent double copy shows the same distance on every line.

`--json` prints the same data as JSON. Only the standard library and numpy are used. Game output
stays under work/ (AGENTS.md rule 1); the figures may be quoted.
"""
from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path

import numpy as np

PEAK_GAP_MS = 5
MIN_WINDOW_ENERGY = 1e-9


def load_mono(path: Path) -> tuple[int, np.ndarray]:
    """Returns (sample rate, the channels' mean as float64 samples)."""
    with wave.open(str(path), "rb") as w:
        channels = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        frames = w.readframes(w.getnframes())
    if width != 2:
        raise SystemExit(f"{path}: {8 * width}-bit samples; only 16-bit PCM is supported")
    samples = np.frombuffer(frames, dtype="<i2").astype(np.float64)
    count = len(samples) // channels
    return rate, samples[: count * channels].reshape(count, channels).mean(axis=1)


def normalised_correlation(game: np.ndarray, excerpt: np.ndarray) -> np.ndarray:
    """corr[t] = <game[t:t+L], excerpt> / (|game[t:t+L]| |excerpt|), for every t with a full window."""
    n_game = len(game)
    n_ex = len(excerpt)
    size = 1 << (n_game + n_ex - 1).bit_length()
    convolution = np.fft.irfft(np.fft.rfft(game, size) * np.fft.rfft(excerpt[::-1], size), size)
    cross = convolution[n_ex - 1 : n_game]
    squares = np.concatenate(([0.0], np.cumsum(game * game)))
    window_energy = squares[n_ex:] - squares[:-n_ex]
    excerpt_energy = float(np.dot(excerpt, excerpt))
    norm = np.sqrt(np.maximum(window_energy, 0.0) * excerpt_energy)
    corr = np.zeros_like(cross)
    ok = norm > MIN_WINDOW_ENERGY
    corr[ok] = cross[ok] / norm[ok]
    return corr


def find_peaks(corr: np.ndarray, min_gap: int, count: int) -> list[tuple[int, float]]:
    """The highest local maxima above zero, at least `min_gap` samples apart, highest first."""
    if len(corr) < 3:
        return []
    inner = corr[1:-1]
    maxima = np.flatnonzero((inner > corr[:-2]) & (inner >= corr[2:])) + 1
    maxima = maxima[corr[maxima] > 0]
    order = maxima[np.argsort(-corr[maxima], kind="stable")]
    blocked = np.zeros(len(corr), dtype=bool)
    peaks: list[tuple[int, float]] = []
    for idx in order:
        if blocked[idx]:
            continue
        peaks.append((int(idx), float(corr[idx])))
        if len(peaks) == count:
            break
        blocked[max(0, idx - min_gap + 1) : idx + min_gap] = True
    return peaks


def analyse(ref: np.ndarray, game: np.ndarray, rate: int, start_s: float, length_s: float, peaks: int) -> dict:
    start = round(start_s * rate)
    n = round(length_s * rate)
    if start < 0 or start + n > len(ref):
        raise SystemExit(
            f"excerpt {start_s:.2f} s + {length_s:.2f} s does not fit in the reference "
            f"({len(ref) / rate:.2f} s)"
        )
    if n > len(game):
        raise SystemExit("the game recording is shorter than the excerpt")
    corr = normalised_correlation(game, ref[start : start + n])
    found = find_peaks(corr, max(1, round(PEAK_GAP_MS * rate / 1000)), peaks)
    result = {
        "ref_start_s": start / rate,
        "peaks": [{"game_s": idx / rate, "height": h} for idx, h in found],
        "distance_ms": None,
        "ratio": None,
    }
    if len(found) >= 2:
        result["distance_ms"] = (found[1][0] - found[0][0]) * 1000 / rate
        result["ratio"] = found[1][1] / found[0][1]
    return result


def excerpt_starts(ref_seconds: float, length_s: float, count: int) -> list[float]:
    """Start times of `count` excerpts spread evenly over the reference (centres of equal slices)."""
    room = max(0.0, ref_seconds - length_s)
    return [(i + 0.5) / count * room for i in range(count)]


def format_pair(peak_1: dict, peak_2: dict | None, distance: float | None, ratio: float | None) -> str:
    text = f"game {peak_1['game_s']:.3f} s  height {peak_1['height']:.4f}"
    if peak_2 is not None:
        text += f"   game {peak_2['game_s']:.3f} s  height {peak_2['height']:.4f}"
        text += f"   distance {distance:.1f} ms  ratio {ratio:.3f}"
    return text


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("reference", type=Path)
    parser.add_argument("game", type=Path)
    parser.add_argument("--ref-start", type=float, default=10.0, help="start of the main excerpt, in s")
    parser.add_argument("--length", type=float, default=3.0, help="excerpt length, in s")
    parser.add_argument("--excerpts", type=int, default=5, help="excerpts spread over the reference")
    parser.add_argument("--peaks", type=int, default=5, help="peaks listed for the main excerpt")
    parser.add_argument("--json", action="store_true", help="print JSON instead of text")
    args = parser.parse_args(argv)

    ref_rate, ref = load_mono(args.reference)
    game_rate, game = load_mono(args.game)
    if ref_rate != game_rate:
        raise SystemExit(f"sample rates differ: reference {ref_rate} Hz, game {game_rate} Hz")
    rate = ref_rate
    ref_seconds = len(ref) / rate

    main_result = analyse(ref, game, rate, args.ref_start, args.length, args.peaks)
    excerpts = [
        analyse(ref, game, rate, start, args.length, 2)
        for start in excerpt_starts(ref_seconds, args.length, args.excerpts)
    ] if args.excerpts > 0 else []

    if args.json:
        print(json.dumps({
            "reference": str(args.reference),
            "game": str(args.game),
            "rate": rate,
            "reference_s": ref_seconds,
            "game_s": len(game) / rate,
            "length_s": args.length,
            "main": main_result,
            "excerpts": excerpts,
        }, indent=2))
        return 0

    print(f"reference {args.reference}  {rate} Hz  {ref_seconds:.2f} s")
    print(f"game      {args.game}  {len(game) / rate:.2f} s")
    print(f"excerpt at {main_result['ref_start_s']:.2f} s, {args.length:.2f} s long: strongest peaks")
    for peak in main_result["peaks"]:
        print(f"  game {peak['game_s']:.3f} s  height {peak['height']:.4f}")
    if main_result["distance_ms"] is not None:
        print(f"  two highest: distance {main_result['distance_ms']:.1f} ms, "
              f"height ratio {main_result['ratio']:.3f}")
    if excerpts:
        print(f"{len(excerpts)} excerpts spread over the reference, two highest peaks each:")
        for item in excerpts:
            first, second = (item["peaks"] + [None, None])[:2]
            if first is None:
                print(f"  ref {item['ref_start_s']:.2f} s  no positive peak")
                continue
            print(f"  ref {item['ref_start_s']:.2f} s  " + format_pair(
                first, second, item["distance_ms"], item["ratio"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
