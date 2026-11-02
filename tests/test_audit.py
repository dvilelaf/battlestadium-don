"""Tests of tools/pipeline/audit.py with a synthetic log (no game files, no real output)."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "pipeline"))
import audit  # noqa: E402

TEXT = (0x00100000, 0x003902D4)
LAYOUT = "[text]\nstart = 0x00100000\nend   = 0x003902D4\n"

# Six lines: the count of functions, one unhandled instruction inside the text, one outside it
# (in the data area), one control-flow warning, one info line and the completion line.
LOG = """[recompiler] recompiling 3 functions
  [error] unhandled-instruction function=sub_00100100 addr=0x100120 - Unhandled SPECIAL instruction: 0x1 raw=0x1
  [error] unhandled-instruction function=sub_00400004 addr=0x4b32e4 - Unhandled SPECIAL instruction: 0x1 raw=0x1
  [warning] control-flow function=sub_00100200 addr=0x100210 - unresolved JR/JALR at 0x100210; promoted 4 fallback entries
  [info] patch - Applied patch at 0x11a31c
Recompilation completed successfully
"""

CLEAN_LOG = """[recompiler] recompiling 2 functions
  [warning] control-flow function=sub_00100200 addr=0x100210 - unresolved JR/JALR at 0x100210; promoted 4 fallback entries
  [info] patch - Applied patch at 0x11a31c
Recompilation completed successfully
"""


def test_parse_synthetic_log():
    report = audit.audit_log(LOG, TEXT, generated_files=2, recompiler_exit=0)
    assert report == {
        "functions": 3,
        "generated_files": 2,
        "unhandled_total": 2,
        "unhandled_in_text": 1,
        "functions_in_data": ["sub_00400004"],
        "control_flow_warnings": 1,
        "log_counts": {
            "error unhandled-instruction": 2,
            "info patch": 1,
            "recompiler recompiling": 1,
            "warning control-flow": 1,
        },
        "recompiler_exit": 0,
        "completed": True,
    }
    assert audit.failure_reasons(report) == [
        "1 unhandled instruction(s) inside the text",
        "1 function(s) with unhandled instructions in the data area",
    ]


def test_functions_in_data_sorted_and_distinct():
    log = ("  [error] unhandled-instruction function=sub_B addr=0x400000 - x raw=0x1\n"
           "  [error] unhandled-instruction function=sub_A addr=0x400004 - x raw=0x1\n"
           "  [error] unhandled-instruction function=sub_B addr=0x400008 - x raw=0x1\n")
    report = audit.audit_log(log, TEXT)
    assert report["functions_in_data"] == ["sub_A", "sub_B"]
    assert report["unhandled_total"] == 3


def test_clean_log_has_no_failures():
    report = audit.audit_log(CLEAN_LOG, TEXT)
    assert report["unhandled_in_text"] == 0
    assert report["functions_in_data"] == []
    assert report["completed"] is True
    assert audit.failure_reasons(report) == []


def test_missing_completion_line_fails():
    report = audit.audit_log(CLEAN_LOG.replace("Recompilation completed successfully", ""), TEXT)
    assert report["completed"] is False
    assert audit.failure_reasons(report) == ["the log does not contain the completion line"]


def test_error_lines_limited_to_twenty():
    log = "".join("  [error] unhandled-instruction function=f addr=0x100000 - x raw=0x1\n"
                  for _ in range(25))
    assert len(audit.error_lines(log)) == 20


def write_inputs(tmp_path, log_text):
    (tmp_path / "recomp.log").write_text(log_text)
    (tmp_path / "layout.toml").write_text(LAYOUT)
    out = tmp_path / "generated"
    out.mkdir()
    (out / "a.cpp").write_text("")
    (out / "b.cpp").write_text("")
    (out / "b.h").write_text("")
    return ["--log", str(tmp_path / "recomp.log"), "--layout", str(tmp_path / "layout.toml"),
            "--out", str(out), "--json", str(tmp_path / "report" / "audit.json")]


def test_main_clean_exits_0(tmp_path, capsys):
    rc = audit.main(write_inputs(tmp_path, CLEAN_LOG) + ["--exit-code", "0"])
    assert rc == 0
    saved = json.loads((tmp_path / "report" / "audit.json").read_text())
    assert saved["generated_files"] == 2
    assert saved["functions"] == 2
    assert "FAILED" not in capsys.readouterr().out


def test_main_failure_exits_1_and_prints_error_lines(tmp_path, capsys):
    rc = audit.main(write_inputs(tmp_path, LOG))
    assert rc == 1
    out = capsys.readouterr().out
    assert "sub_00400004" in out
    assert "  [error] unhandled-instruction function=sub_00100100" in out


def test_main_recompiler_exit_nonzero_exits_1(tmp_path):
    rc = audit.main(write_inputs(tmp_path, CLEAN_LOG) + ["--exit-code", "3"])
    assert rc == 1
    saved = json.loads((tmp_path / "report" / "audit.json").read_text())
    assert saved["recompiler_exit"] == 3
