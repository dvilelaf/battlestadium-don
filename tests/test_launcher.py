"""The launcher, without the game, a display or the port: every piece it uses is replaced."""

import importlib.util
import json
import os
from pathlib import Path

import pytest

LAUNCH = Path(__file__).resolve().parents[1] / "tools" / "launcher" / "launch.py"


@pytest.fixture
def L(monkeypatch, tmp_path):
    spec = importlib.util.spec_from_file_location("don_test_launcher", LAUNCH)
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
    monkeypatch.setattr(module.shutil, "which", lambda name: "/usr/bin/" + name)
    module.tmp = tmp_path
    return module


def make_jp(folder):
    folder.mkdir(parents=True, exist_ok=True)
    for name in ("SLPS_256.75", "SYSTEM.CNF", "DIR.DIR", "MOVIE.AFS", "DATA.AFS", "BGM.AFS", "SE.AFS"):
        (folder / name).write_bytes(b"x")
    (folder / "MODULES").mkdir(exist_ok=True)


def make_en(root, which):
    folder = root / "en_patch" / which / "x"
    folder.mkdir(parents=True)
    (folder / "DATA.AFS").write_bytes(b"e")


def states(L):
    return {code: state for code, _l, state, _d in L.variant_table(L.Settings())}


def test_offered_variants_for_each_combination(L):
    assert set(states(L).values()) == {L.MISSING}
    make_jp(L.ROOT / "disc")
    assert states(L) == {"jp": L.PREPARE, "en-jpvoice": L.MISSING, "en-envoice": L.MISSING}
    make_en(L.ROOT, "jpvoice")
    assert states(L) == {"jp": L.PREPARE, "en-jpvoice": L.PREPARE, "en-envoice": L.MISSING}
    make_en(L.ROOT, "envoice")
    assert set(states(L).values()) == {L.PREPARE}
    # English sources present, Japanese files missing: English is not offered either (it is the base).
    (L.ROOT / "disc" / "DATA.AFS").unlink()
    assert set(states(L).values()) == {L.MISSING}
    detail = {c: d for c, _l, _s, d in L.variant_table(L.Settings())}["en-jpvoice"]
    assert "Japanese disc" in detail


def test_missing_variant_says_what_is_missing(L):
    make_jp(L.ROOT / "disc")
    detail = {c: d for c, _l, _s, d in L.variant_table(L.Settings())}["en-envoice"]
    assert "DON_EN_ENVOICE" in detail


def test_image_source_needs_7z(L, monkeypatch):
    make_jp(L.ROOT / "disc")
    (L.ROOT / "en_patch" / "jpvoice").mkdir(parents=True)
    (L.ROOT / "en_patch" / "jpvoice" / "disc.iso").write_bytes(b"i")
    assert states(L)["en-jpvoice"] == L.PREPARE
    monkeypatch.setattr(L.shutil, "which", lambda name: None)
    assert states(L)["en-jpvoice"] == L.MISSING


def test_lookup_order_file_then_environment_then_local_env_then_default(L, monkeypatch):
    for sub in ("default", "local", "env", "conf"):
        (L.tmp / sub).mkdir()
    monkeypatch.setattr(L, "ROOT", L.tmp / "repo")
    assert L.Settings().path("DON_DISC_DIR", "disc") == L.ROOT / "disc"
    (L.ROOT / "local.env").write_text(f': "${{DON_DISC_DIR={L.tmp / "local"}}}"\nexport DON_ISO=/a.iso\n')
    assert L.Settings().path("DON_DISC_DIR", "disc") == L.tmp / "local"
    assert str(L.Settings().path("DON_ISO")) == "/a.iso"
    monkeypatch.setenv("DON_DISC_DIR", str(L.tmp / "env"))
    assert L.Settings().path("DON_DISC_DIR", "disc") == L.tmp / "env"
    (L.tmp / "conf" / L.CONF_NAME).write_text(f"DON_DISC_DIR={L.tmp / 'conf'}\n")
    monkeypatch.setenv("DON_CONFIG_DIR", str(L.tmp / "conf"))
    assert L.Settings().path("DON_DISC_DIR", "disc") == L.tmp / "conf"


def test_local_env_is_read_not_executed(L):
    marker = L.tmp / "executed"
    (L.ROOT / "local.env").write_text(f'$(touch {marker})\nDON_BIN="/x y/don"\n# DON_ISO=/no\n')
    s = L.Settings()
    assert s.get("DON_BIN") == "/x y/don" and s.get("DON_ISO") is None
    assert not marker.exists()


def test_folder_is_made_when_missing_and_reused_when_present(L, monkeypatch):
    make_jp(L.ROOT / "disc")
    make_en(L.ROOT, "jpvoice")
    calls = []

    class FakeMaker:
        MakeError = RuntimeError

        @staticmethod
        def make(jp_dir, source, dest):
            calls.append((Path(jp_dir), Path(source), Path(dest)))
            make_jp(Path(dest))
            (Path(dest) / ".make_variant_dir.json").write_text(json.dumps({"variant": "en-jpvoice"}))
            return "en-jpvoice", {}

    monkeypatch.setattr(L, "load_make_variant_dir", lambda: FakeMaker)
    messages = []
    first = L.prepare(L.Settings(), "en-jpvoice", say=messages.append)
    assert first == L.tmp / "variants" / "en-jpvoice"
    assert calls == [(L.ROOT / "disc", L.ROOT / "en_patch" / "jpvoice" / "x", first)]
    assert messages and "first start only" in messages[0]
    assert L.prepare(L.Settings(), "en-jpvoice", say=messages.append) == first
    assert len(calls) == 1                                # reused, not made again
    assert states(L)["en-jpvoice"] == L.AVAILABLE


def test_wrong_variant_in_the_source_is_refused(L, monkeypatch):
    make_jp(L.ROOT / "disc")
    make_en(L.ROOT, "envoice")

    class FakeMaker:
        MakeError = RuntimeError
        make = staticmethod(lambda *a: ("en-jpvoice", {}))

    monkeypatch.setattr(L, "load_make_variant_dir", lambda: FakeMaker)
    with pytest.raises(L.LauncherError):
        L.prepare(L.Settings(), "en-envoice", say=lambda _: None)


def test_command_for_each_variant_has_no_iso_and_no_don_iso(L, monkeypatch):
    monkeypatch.setenv("DON_BIN", "/opt/port/don")
    monkeypatch.setenv("DON_ISO", "/japanese.iso")
    for code in L.CODES:
        argv, env = L.build_command(L.Settings(), code, [])
        assert argv == ["/opt/port/don", str(L.tmp / "variants" / code / "SLPS_256.75")]
        assert "--iso" not in argv
        assert "DON_ISO" not in env


def test_default_binary_is_the_checkouts(L):
    argv, _ = L.build_command(L.Settings(), "jp", [])
    assert Path(argv[0]) == L.ROOT / "build" / "linux-dev" / ("don.exe" if L.os.name == "nt" else "don")


@pytest.mark.skipif(os.name == "nt", reason="the Linux player folder")
def test_player_folder_is_preferred_and_says_its_commit(L, monkeypatch):
    player = L.ROOT / "build" / "player" / "don-linux"
    player.mkdir(parents=True)
    (player / "don").write_text("")
    (player / "BUILT_FROM.txt").write_text("commit 0123456789abcdef0123\nsubject T-1: a subject\nbuilt now\n")
    argv, _ = L.build_command(L.Settings(), "jp", [])
    assert Path(argv[0]) == player / "don"                      # preferred over build/linux-dev
    line = L.describe_bin(L.Settings(), player / "don")
    assert "player folder" in line and "0123456789ab" in line and "T-1: a subject" in line
    monkeypatch.setenv("DON_BIN", "/opt/port/don")             # DON_BIN still overrides
    argv, _ = L.build_command(L.Settings(), "jp", [])
    assert argv[0] == "/opt/port/don"
    assert "DON_BIN" in L.describe_bin(L.Settings(), Path("/opt/port/don"))


def ready_all(L, monkeypatch):
    make_jp(L.ROOT / "disc")
    make_en(L.ROOT, "jpvoice")
    make_en(L.ROOT, "envoice")
    for code in L.CODES:
        folder = L.tmp / "variants" / code
        make_jp(folder)
        (folder / ".make_variant_dir.json").write_text(json.dumps({"variant": code}))
    port = L.tmp / "don"
    port.write_text("")
    monkeypatch.setenv("DON_BIN", str(port))
    return port


def test_passthrough_without_shell_splitting(L, monkeypatch):
    port = ready_all(L, monkeypatch)
    calls = []
    monkeypatch.setattr(L.subprocess, "call", lambda argv, env=None: calls.append((argv, env)) or 0)
    extra = ["--print", "--input-script", "/a dir/script one.txt", "--seconds", "20"]
    assert L.main(["launch.py", "en-envoice", "--", *extra]) == 0
    argv, env = calls[0]
    assert argv == [str(port), str(L.tmp / "variants" / "en-envoice" / "SLPS_256.75"), *extra]
    assert "DON_ISO" not in env


def test_net_argv_for_host_and_join(L, monkeypatch):
    """T-507: --host/--join become the port's --net-* arguments, before the `--` arguments."""
    port = ready_all(L, monkeypatch)
    calls = []
    monkeypatch.setattr(L.subprocess, "call", lambda argv, env=None: calls.append(argv) or 0)
    base = [str(port), str(L.tmp / "variants" / "jp" / "SLPS_256.75")]

    assert L.main(["launch.py", "jp", "--host"]) == 0
    assert calls[0] == base + ["--net-host", "7877", "--net-players", "2"]

    assert L.main(["launch.py", "jp", "--host", "--port", "9000", "--players", "4", "--delay", "2"]) == 0
    assert calls[1] == base + ["--net-host", "9000", "--net-players", "4", "--net-delay", "2"]

    assert L.main(["launch.py", "jp", "--join", "10.20.30.5"]) == 0
    assert calls[2] == base + ["--net-join", "10.20.30.5:7877"]

    assert L.main(["launch.py", "jp", "--join", "h:9000"]) == 0
    assert calls[3] == base + ["--net-join", "h:9000"]


def test_net_host_with_join_is_an_error(L, monkeypatch):
    ready_all(L, monkeypatch)
    monkeypatch.setattr(L.subprocess, "call", lambda *a, **k: pytest.fail("started"))
    assert L.main(["launch.py", "jp", "--host", "--join", "h"]) == 2


def test_no_net_argv_without_host_or_join(L, monkeypatch):
    port = ready_all(L, monkeypatch)
    calls = []
    monkeypatch.setattr(L.subprocess, "call", lambda argv, env=None: calls.append(argv) or 0)
    assert L.main(["launch.py", "jp"]) == 0
    assert "--net-host" not in calls[0] and "--net-join" not in calls[0]


def test_gs_cache_is_not_passed_by_the_launcher_anymore(L, monkeypatch):
    """T-524: the launcher no longer sets DON_GS_DATA_DIR (or --mc-dir); the program decides those."""
    ready_all(L, monkeypatch)
    calls = []
    monkeypatch.setattr(L.subprocess, "call", lambda argv, env=None: calls.append(env) or 0)
    assert L.main(["launch.py", "jp"]) == 0
    assert "DON_GS_DATA_DIR" not in calls[0]


def test_remembered_choice_is_written_and_preselected(L, monkeypatch):
    ready_all(L, monkeypatch)
    monkeypatch.setattr(L.subprocess, "call", lambda argv, env=None: 0)
    assert L.main(["launch.py", "en-jpvoice"]) == 0
    assert (L.tmp / "conf" / L.CONF_NAME).read_text().splitlines() == ["LAST_VARIANT=en-jpvoice"]
    seen = []
    monkeypatch.setattr(L, "pick", lambda table, pre: seen.append(pre) or "jp")
    assert L.main(["launch.py"]) == 0
    assert seen == ["en-jpvoice"]
    assert L.Settings().last() == "jp"


def test_remembering_keeps_the_other_settings(L):
    (L.tmp / "conf").mkdir()
    (L.tmp / "conf" / L.CONF_NAME).write_text("DON_BIN=/p/don\nLAST_VARIANT=jp\n")
    L.Settings().remember("en-envoice")
    assert (L.tmp / "conf" / L.CONF_NAME).read_text().splitlines() == ["DON_BIN=/p/don", "LAST_VARIANT=en-envoice"]


def test_cancel_starts_nothing(L, monkeypatch):
    ready_all(L, monkeypatch)
    started = []
    monkeypatch.setattr(L.subprocess, "call", lambda *a, **k: started.append(a) or 0)

    def cancel(table, pre):
        raise L.Cancelled()

    monkeypatch.setattr(L, "pick", cancel)
    assert L.main(["launch.py"]) == 0
    assert started == []
    assert not (L.tmp / "conf" / L.CONF_NAME).exists()


def test_terminal_prompt_cancel_and_choice(L, monkeypatch):
    table = [("jp", "J", L.AVAILABLE, ""), ("en-jpvoice", "E", L.MISSING, "missing x")]
    monkeypatch.setattr("builtins.input", lambda _p="": "")
    with pytest.raises(L.Cancelled):
        L.pick_terminal(table, "jp")
    monkeypatch.setattr("builtins.input", lambda _p="": "1")
    assert L.pick_terminal(table, "jp") == "jp"
    monkeypatch.setattr("builtins.input", lambda _p="": "2")
    assert L.pick_terminal(table, "jp") == ""              # unavailable: not accepted


def test_terminal_prompt_shows_the_keys_help(L, monkeypatch, capsys):
    monkeypatch.setattr("builtins.input", lambda _p="": "")
    with pytest.raises(L.Cancelled):
        L.pick_terminal([("jp", "J", L.AVAILABLE, "")], "jp")
    assert "Enter = Start" in capsys.readouterr().out and "L = confirm" in L.HELP


def test_print_prints_the_command_and_starts_nothing(L, monkeypatch, capsys):
    port = ready_all(L, monkeypatch)
    monkeypatch.setattr(L.subprocess, "call", lambda *a, **k: pytest.fail("started"))
    assert L.main(["launch.py", "--print", "jp", "--", "--seconds", "5"]) == 0
    out = capsys.readouterr().out.strip()
    assert out == L.format_command([str(port), str(L.tmp / "variants" / "jp" / "SLPS_256.75"),
                                    "--seconds", "5"])


def test_print_does_not_prepare_or_remember(L, monkeypatch, capsys):
    make_jp(L.ROOT / "disc")
    monkeypatch.setattr(L, "prepare", lambda *a, **k: pytest.fail("prepared"))
    assert L.main(["launch.py", "--print", "jp"]) == 0
    assert not (L.tmp / "conf" / L.CONF_NAME).exists()


def test_list_shows_state_and_what_is_missing(L, capsys):
    make_jp(L.ROOT / "disc")
    assert L.main(["launch.py", "--list"]) == 0
    lines = capsys.readouterr().out.splitlines()
    assert [ln.split()[0] for ln in lines[:3]] == L.CODES
    assert lines[3] == f"memory cards: {L.tmp / 'data' / 'don' / 'memcards'} (DON_MC_DIR)"
    assert L.PREPARE in lines[0] and L.MISSING in lines[1] and "DON_EN_JPVOICE" in lines[1]


def test_settings_file_with_unknown_variant_is_ignored(L, monkeypatch):
    ready_all(L, monkeypatch)
    (L.tmp / "conf").mkdir()
    (L.tmp / "conf" / L.CONF_NAME).write_text("LAST_VARIANT=klingon\n")
    assert L.Settings().last() == ""
    seen = []
    monkeypatch.setattr(L, "pick", lambda table, pre: seen.append(pre) or "jp")
    monkeypatch.setattr(L.subprocess, "call", lambda *a, **k: 0)
    L.main(["launch.py"])
    assert seen == [""]


def test_unknown_code_unavailable_variant_and_unknown_option(L, monkeypatch, capsys):
    make_jp(L.ROOT / "disc")
    assert L.main(["launch.py", "klingon"]) == 2
    assert L.main(["launch.py", "en-envoice"]) == 1       # nothing to make it from
    assert L.main(["launch.py", "--bogus"]) == 2
    err = capsys.readouterr().err
    assert "klingon" in err and "DON_EN_ENVOICE" in err and "--bogus" in err


def test_missing_port_is_reported_not_started(L, monkeypatch, capsys):
    ready_all(L, monkeypatch)
    monkeypatch.setenv("DON_BIN", str(L.tmp / "nowhere"))
    monkeypatch.setattr(L.subprocess, "call", lambda *a, **k: pytest.fail("started"))
    assert L.main(["launch.py", "jp"]) == 1
    assert "DON_BIN" in capsys.readouterr().err


def test_memory_card_folder_setting_file_environment_default(L, monkeypatch):
    default = L.tmp / "data" / "don" / "memcards"
    assert L.Settings().memcards() == default
    monkeypatch.setenv("DON_MC_DIR", str(L.tmp / "from-env"))
    assert L.Settings().memcards() == L.tmp / "from-env"
    conf = L.tmp / "conf" / L.CONF_NAME
    conf.parent.mkdir(parents=True)
    conf.write_text(f"DON_MC_DIR={L.tmp / 'from-file'}\n")
    assert L.Settings().memcards() == L.tmp / "from-file"          # the settings file wins
    argv, _env = L.build_command(L.Settings(), "jp", [])
    assert "--mc-dir" not in argv  # T-524: the program decides the cards from its own user folder


def test_old_save_in_a_variant_folder_is_reported_not_moved(L, capsys):
    assert L.legacy_saves_note(L.Settings()) is None
    old = L.tmp / "variants" / "en-jpvoice" / "mc0" / "BASLUS-1"
    old.mkdir(parents=True)
    (old / "icon.sys").write_bytes(b"s")
    note = L.legacy_saves_note(L.Settings())
    assert note and "en-jpvoice" in note and "\n" not in note
    (L.tmp / "data" / "don" / "memcards" / "mc0").mkdir(parents=True)
    (L.tmp / "data" / "don" / "memcards" / "mc0" / "f").write_bytes(b"x")
    assert L.legacy_saves_note(L.Settings()) is None               # the shared folder has a save
    assert (old / "icon.sys").exists()                              # nothing was deleted
    assert L.main(["launch.py", "--list"]) == 0
    assert "memory cards:" in capsys.readouterr().out

