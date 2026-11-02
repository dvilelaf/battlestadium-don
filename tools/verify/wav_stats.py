#!/usr/bin/env python3
"""Statistics of a WAV file, to check the sound the port produces (T-303 step 3).

    uv run --no-project python -I tools/verify/wav_stats.py <file.wav> \
        [--max-silence 0.5] [--max-clip 0.001]

Prints the duration, the RMS level, the fraction of silent samples (absolute value below 1e-4)
and the fraction of saturated samples (absolute value at or above 0.999).  Each fraction for
which a limit was given on the command line is compared with it and the program exits with 1 if
any of them is exceeded.  T-304 uses `--max-silence 0.5` on the audio of a run with the game.

Standard library only (`wave`, `struct`, `argparse`).  `analyze()` is the API, `main()` the
command line; tests/test_wav_stats.py imports the first and runs the second.
"""
from __future__ import annotations

import argparse
import struct
import sys
import wave
from dataclasses import dataclass
from pathlib import Path

# A sample is "silent" below this absolute value and "saturated" at or above this one (the card).
SILENCE_THRESHOLD = 1e-4
SATURATED_THRESHOLD = 0.999


@dataclass
class WavStats:
    path: Path
    frames: int          # sample frames (one per channel set)
    sample_rate: int
    channels: int
    bits: int
    duration: float      # seconds
    rms: float           # root mean square of every sample, 0..1 on a 16-bit full scale
    silent: float        # fraction of samples with |v| < SILENCE_THRESHOLD
    saturated: float     # fraction of samples with |v| >= SATURATED_THRESHOLD

    @property
    def samples(self) -> int:
        return self.frames * self.channels


def read_samples(path: Path):
    """Reads an uncompressed PCM WAV and returns (raws, sample_rate, channels, bits).

    `raws` are the integer samples as they are stored.  8-bit is unsigned, the rest signed
    little-endian; the widths 1, 2, 3 and 4 bytes are accepted.
    """
    with wave.open(str(path), "rb") as w:
        channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        sample_rate = w.getframerate()
        frames = w.getnframes()
        comptype = w.getcomptype()
        raw = w.readframes(frames)
    if comptype != "NONE":
        raise ValueError(f"{path}: compressed WAV ({comptype}) is not supported")
    if sampwidth not in (1, 2, 3, 4):
        raise ValueError(f"{path}: sample width {sampwidth} bytes is not supported")
    if frames and len(raw) != frames * channels * sampwidth:
        # A WAV whose header claims more data than the file holds (for example one written by a
        # run that was killed): keep the samples that are there.
        frames = len(raw) // (channels * sampwidth)

    count = frames * channels
    if sampwidth == 1:
        raws = struct.unpack(f"<{count}B", raw[:count])
        raws = [v - 128 for v in raws]
    elif sampwidth == 2:
        raws = list(struct.unpack(f"<{count}h", raw[: 2 * count]))
    elif sampwidth == 3:
        raws = []
        for i in range(count):
            b = raw[3 * i : 3 * i + 3]
            v = b[0] | (b[1] << 8) | (b[2] << 16)
            raws.append(v - (1 << 24) if v & 0x800000 else v)
    else:
        raws = list(struct.unpack(f"<{count}i", raw[: 4 * count]))
    return raws, sample_rate, channels, sampwidth * 8


def _full_scale(bits: int) -> float:
    return float(1 << (bits - 1))


def analyze(path) -> WavStats:
    """Reads `path` and computes its figures.  Raises OSError / wave.Error / ValueError."""
    path = Path(path)
    raws, sample_rate, channels, bits = read_samples(path)
    total = len(raws)
    if total == 0:
        return WavStats(path, 0, sample_rate, channels, bits, 0.0, 0.0, 1.0, 0.0)
    scale = _full_scale(bits)
    silent = 0
    saturated = 0
    square_sum = 0.0
    for raw in raws:
        v = raw / scale
        if -SILENCE_THRESHOLD < v < SILENCE_THRESHOLD:
            silent += 1
        if v <= -SATURATED_THRESHOLD or v >= SATURATED_THRESHOLD:
            saturated += 1
        square_sum += v * v
    frames = total // channels
    return WavStats(
        path=path,
        frames=frames,
        sample_rate=sample_rate,
        channels=channels,
        bits=bits,
        duration=frames / sample_rate if sample_rate else 0.0,
        rms=(square_sum / total) ** 0.5,
        silent=silent / total,
        saturated=saturated / total,
    )


def format_stats(stats: WavStats) -> str:
    return (
        f"file: {stats.path}\n"
        f"format: {stats.sample_rate} Hz, {stats.channels} channel(s), {stats.bits}-bit PCM\n"
        f"duration: {stats.duration:.6f} s ({stats.frames} frames)\n"
        f"rms: {stats.rms:.6f}\n"
        f"silent: {stats.silent:.6f}\n"
        f"saturated: {stats.saturated:.6f}\n"
    )


def check_limits(stats: WavStats, max_silence: float | None, max_clip: float | None) -> list[str]:
    """The reasons the limits were exceeded, empty when all of them passed.

    `max_silence` is the largest allowed fraction of silent samples, `max_clip` the largest
    allowed fraction of saturated samples.  `None` means no limit was asked for.
    """
    failures = []
    if max_silence is not None and stats.silent > max_silence:
        failures.append(f"silent fraction {stats.silent:.6f} exceeds {max_silence:.6f}")
    if max_clip is not None and stats.saturated > max_clip:
        failures.append(f"saturated fraction {stats.saturated:.6f} exceeds {max_clip:.6f}")
    return failures


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="duration, RMS, silence and clipping of a WAV file")
    parser.add_argument("wav", type=Path, help="the WAV file to read")
    parser.add_argument(
        "--max-silence",
        type=float,
        default=None,
        metavar="FRACTION",
        help="fail (exit 1) when more than this fraction of the samples is silent (for example 0.5)",
    )
    parser.add_argument(
        "--max-clip",
        type=float,
        default=None,
        metavar="FRACTION",
        help="fail (exit 1) when more than this fraction of the samples is saturated (for example 0.001)",
    )
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        stats = analyze(args.wav)
    except (OSError, wave.Error, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    sys.stdout.write(format_stats(stats))
    failures = check_limits(stats, args.max_silence, args.max_clip)
    for reason in failures:
        print(f"FAIL: {reason}")
    if args.max_silence is not None:
        print(f"limit max-silence {args.max_silence:.6f}: {'exceeded' if stats.silent > args.max_silence else 'ok'}")
    if args.max_clip is not None:
        print(f"limit max-clip {args.max_clip:.6f}: {'exceeded' if stats.saturated > args.max_clip else 'ok'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
