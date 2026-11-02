"""The launcher's one user-folder rule and the folders derived from it."""

import importlib.util
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

LAUNCH = Path(__file__).resolve().parents[1] / "tools" / "launcher" / "launch.py"


@pytest.fixture
def L(monkeypatch, tmp_path):
    spec = importlib.util.spec_from_file_location("don_test_launcher_paths", LAUNCH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    root = tmp_path / "repo"
    root.mkdir()
    monkeypatch.setattr(module, "ROOT", root)
    for name in ("DON_BIN", "DON_DISC_DIR", "DON_ISO", "DON_EN_JPVOICE", "DON_EN_ENVOICE", "DON_MC_DIR",
                 "DON_GS_DATA_DIR", "DON_USER_DIR"):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("DON_CONFIG_DIR", str(tmp_path / "conf"))
    monkeypatch.setenv("DON_VARIANTS_DIR", str(tmp_path / "variants"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "data"))
    module.tmp = tmp_path
    return module


def test_user_dir_rule_on_linux(L):
    home = L.tmp / "home"
    assert L.user_dir({"HOME": str(home)}) == home / ".local" / "share" / "don"
    assert L.user_dir({"HOME": str(home), "XDG_DATA_HOME": str(L.tmp / "xdg")}) == L.tmp / "xdg" / "don"
    assert L.user_dir({}) == Path(".local/share/don")                 # HOME unset: as user_dir.h, no pwd lookup


def test_user_dir_rule_on_windows(L, monkeypatch):
    # launcher_paths's os is replaced, so only its own os.name check sees Windows; pathlib stays Posix here.
    monkeypatch.setattr(sys.modules["launcher_paths"], "os", SimpleNamespace(name="nt"))
    assert L.user_dir({"APPDATA": str(L.tmp / "roam")}) == L.tmp / "roam" / "don"
    profile = L.tmp / "profile"
    assert L.user_dir({"USERPROFILE": str(profile)}) == profile / "AppData" / "Roaming" / "don"
    assert L.user_dir({}) == Path("don")


def test_user_dir_override_moves_every_folder_the_launcher_uses(L):
    user = L.tmp / "dev-user"
    env = {"DON_USER_DIR": str(user), "XDG_DATA_HOME": str(L.tmp / "data"), "HOME": str(L.tmp / "home"),
           "XDG_CONFIG_HOME": str(L.tmp / "xcfg")}
    assert L.user_dir(env) == user
    assert L.conf_dir(env) == user
    assert L.variants_dir(env) == user / "variants"
    assert L.gs_cache(env) == user / "gs-cache"
    settings = L.Settings(environ=env, root=L.tmp / "repo")
    assert settings.conf_path == user / L.CONF_NAME
    assert settings.memcards() == user / "memcards"


def test_variants_default_follows_the_user_folder_not_the_old_one(L, monkeypatch):
    monkeypatch.delenv("DON_VARIANTS_DIR")
    monkeypatch.delenv("DON_USER_DIR", raising=False)
    monkeypatch.setenv("XDG_DATA_HOME", str(L.tmp / "data"))
    assert L.variants_dir() == L.tmp / "data" / "don" / "variants"
    monkeypatch.setenv("DON_USER_DIR", str(L.tmp / "dev"))
    assert L.variants_dir() == L.tmp / "dev" / "variants"           # DON_USER_DIR moves it; the old folder is unused
    assert not (L.tmp / "data" / "don" / "variants").exists()
