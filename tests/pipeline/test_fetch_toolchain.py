"""Tests of tools/pipeline/fetch_toolchain.py that need no download."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "pipeline"))
import fetch_toolchain as ft  # noqa: E402


def test_pin_round_trip_per_platform(tmp_path):
    pin = tmp_path / "pin.sha256"
    assert ft.read_pin(pin, ft.WINDOWS_ARCHIVE_NAME) == ""
    ft.write_pin("ab" * 32, pin, ft.WINDOWS_ARCHIVE_NAME)
    assert ft.read_pin(pin, ft.WINDOWS_ARCHIVE_NAME) == "ab" * 32
    assert ft.read_pin(pin, ft.ARCHIVE_NAME) == ""


def test_windows_pin_file_matches_archive_name():
    assert ft.read_pin(ft.WINDOWS_PIN_FILE, ft.WINDOWS_ARCHIVE_NAME) != ""


def test_check_windows_detects_tamper(tmp_path):
    (tmp_path / "bin").mkdir()
    (tmp_path / "bin" / "clang++.exe").write_bytes(b"x")
    (tmp_path / ft.TREE_HASH_FILE).write_text(ft.tree_hash(tmp_path) + "  t\n")
    assert ft.check(tmp_path, "bin/clang++.exe") == 0
    (tmp_path / "bin" / "clang++.exe").write_bytes(b"y")
    assert ft.check(tmp_path, "bin/clang++.exe") == 1
