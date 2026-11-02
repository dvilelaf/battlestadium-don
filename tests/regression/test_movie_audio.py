"""The opening movie's sound must come out of the game once, not twice 170.7 ms apart.

Known bug: from ~13-24 s into the movie the right channel runs one ring (8192 samples, 170.7 ms) ahead of the
left, heard as doubled sound. The reference is the movie's own sound decoded from the user's disc (MOVIE.AFS
member 1, the opening); the game's WAV is compared with it by tools/verify/wav_copies.py. Everything derived
from the game stays in a temporary folder.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

import harness

sys.path.insert(0, str(harness.ROOT / "tools" / "verify"))
import wav_copies  # noqa: E402

pytestmark = pytest.mark.slow

VARIANT = "en-jpvoice"
OPENING_MEMBER = 1
STRONG = 0.5
SECOND_RATIO = 0.3
NEAR_MS = (5.0, 500.0)
SECONDS = 130


def extract_afs_member(afs, index, out):
    with open(afs, "rb") as f:
        head = f.read(8)
        if head[:4] != b"AFS\x00":
            pytest.skip(f"{afs} is not an AFS archive")
        count, = struct.unpack("<I", head[4:])
        if index >= count:
            pytest.skip(f"{afs} has {count} members, no member {index}")
        f.seek(8 + index * 8)
        offset, size = struct.unpack("<II", f.read(8))
        f.seek(offset)
        out.write_bytes(f.read(size))


def build_reference(work):
    reason = harness.skip_reason(VARIANT)
    if reason:
        pytest.skip(reason)
    afs = harness.variant_exe(VARIANT).parent / "MOVIE.AFS"
    if not afs.is_file():
        pytest.skip(f"no MOVIE.AFS at {afs}")
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        pytest.skip("no ffmpeg on PATH")
    decoders = subprocess.run([ffmpeg, "-hide_banner", "-decoders"], capture_output=True, text=True).stdout
    if "adpcm_adx" not in decoders:
        pytest.skip("this ffmpeg has no adpcm_adx decoder")
    sfd = work / "OP.sfd"
    extract_afs_member(afs, OPENING_MEMBER, sfd)
    ref = work / "op_ref.wav"
    done = subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(sfd), "-vn", "-ar", "48000", "-ac", "2",
                           "-c:a", "pcm_s16le", str(ref)], capture_output=True, text=True)
    if done.returncode != 0 or not ref.is_file():
        pytest.skip(f"ffmpeg could not decode the opening's sound: {done.stderr.strip()[:200]}")
    return ref


def test_opening_movie_sound_has_no_second_copy():
    with tempfile.TemporaryDirectory(prefix="don-movie-audio-") as tmp:
        work = Path(tmp)
        ref = build_reference(work)
        user = work / "user"
        user.mkdir()
        game_wav = work / "game.wav"
        env = dict(os.environ, DON_USER_DIR=str(user), DON_PLAY_MOVIES="1", DON_AUDIO_DEVICE="1",
                   SDL_AUDIO_DRIVER="disk", SDL_AUDIO_DISK_OUTPUT_FILE=str(work / "sink.raw"))
        env.pop("DON_MC_DIR", None)
        cmd = [str(harness.binary()), "--headless", "--renderer", "software", "--sound", "on",
               "--input-script", str(harness.ROOT / "tools/run/scripts/intro_movie.txt"),
               "--seconds", str(SECONDS), "--wav", str(game_wav), str(harness.variant_exe(VARIANT))]
        log = work / "game.log"
        with open(log, "wb") as out:
            code = subprocess.run(cmd, cwd=harness.ROOT, env=env, stdout=out, stderr=subprocess.STDOUT,
                                  timeout=harness.TIMEOUT).returncode
        assert code == 0, f"the game exited with {code}; log tail:\n{log.read_text(errors='replace')[-1500:]}"
        assert game_wav.is_file(), "the game wrote no WAV"

        import contextlib
        import io
        import json
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            wav_copies.main([str(ref), str(game_wav), "--excerpts", "8", "--json"])
        result = json.loads(buf.getvalue())

    problems = []
    for item in result["excerpts"]:
        peaks = item["peaks"]
        if not peaks or peaks[0]["height"] <= STRONG:
            continue
        first = peaks[0]
        for other in peaks[1:]:
            distance = abs(other["game_s"] - first["game_s"]) * 1000
            ratio = other["height"] / first["height"]
            if NEAR_MS[0] <= distance <= NEAR_MS[1] and ratio > SECOND_RATIO:
                problems.append(f"reference {item['ref_start_s']:.1f} s: a second copy {distance:.1f} ms from "
                                f"the strongest peak (height {first['height']:.3f}), ratio {ratio:.3f}")
    assert not problems, "the opening movie's sound is doubled:\n" + "\n".join(problems)
