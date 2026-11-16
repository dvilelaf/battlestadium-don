"""Incident 2026-11-16: the packagers reconfigure a folder lacking a flag and refuse a game build that links the game."""
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "module"))
sys.path.insert(0, str(REPO / "tools" / "pipeline"))
import package_linux  # noqa: E402
from package_checks import check_game_not_linked  # noqa: E402


def folder(tmp_path: Path, cache: dict[str, str]) -> Path:
    (tmp_path / "build.ninja").write_text("")
    (tmp_path / "CMakeCache.txt").write_text("".join(f"{k}:STRING={v}\n" for k, v in cache.items()))
    return tmp_path


def test_missing_flag_reconfigures(tmp_path):
    build = folder(tmp_path, {"DON_RUNTIME_DIR": "/rt"})
    assert package_linux.needs_configure(build, Path("/rt"), ["-DDON_LINK_GENERATED=OFF"])


def test_equal_flag_does_not_reconfigure(tmp_path):
    build = folder(tmp_path, {"DON_RUNTIME_DIR": "/rt", "DON_LINK_GENERATED": "OFF"})
    assert not package_linux.needs_configure(build, Path("/rt"), ["-DDON_LINK_GENERATED=OFF"])


def test_different_flag_reconfigures(tmp_path):
    build = folder(tmp_path, {"DON_RUNTIME_DIR": "/rt", "DON_LINK_GENERATED": "ON"})
    assert package_linux.needs_configure(build, Path("/rt"), ["-DDON_LINK_GENERATED=OFF"])


def test_game_build_linking_the_generated_code_is_refused(tmp_path):
    with pytest.raises(SystemExit, match="DON_LINK_GENERATED=ON"):
        check_game_not_linked(folder(tmp_path, {"DON_LINK_GENERATED": "ON"}))


def test_game_build_without_the_option_is_refused(tmp_path):
    with pytest.raises(SystemExit, match="DON_LINK_GENERATED=None"):
        check_game_not_linked(folder(tmp_path, {}))


def test_game_build_with_the_option_off_passes(tmp_path):
    check_game_not_linked(folder(tmp_path, {"DON_LINK_GENERATED": "OFF"}))
