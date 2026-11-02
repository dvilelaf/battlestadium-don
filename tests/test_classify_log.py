"""Tests of tools/run/classify_log.py with synthetic logs (they do not need the game)."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "run"))

import classify_log

BOOT = "[don] override applied\nELF file loaded successfully. Entry point: 0x100008\n"


def counters(frames, vsyncs):
    lines = [f"[don] frame {n}" for n in range(60, frames + 1, 60)]
    lines += [f"[don] vsync {n}" for n in range(60, vsyncs + 1, 60)]
    return "\n".join(lines) + "\n"


def test_missing_function():
    text = BOOT + "Error: No exact recompiled function for guest PC 0x00138b00 tableBase=0x0\n"
    assert classify_log.classify(text) == ("MISSING_FUNCTION", "0x00138b00")


def test_missing_branch_target():
    text = BOOT + "[guest-branch:missing-target] kind=jal op=jal source=0x1 target=0x2D5D28 pc=0x1\n"
    assert classify_log.classify(text) == ("MISSING_BRANCH_TARGET", "0x2d5d28")


def test_unimplemented_syscall():
    text = BOOT + ("[Syscall TODO] encoded=0xfd v1=0xfd pc=0x10c030\n"
                   "Warning: Unimplemented PS2 syscall called. PC=0x10c030, RA=0x1, Encoded=0xfd\n"
                   "  -> Unknown syscallId=0xfd hits=1\n")
    category, detail = classify_log.classify(text)
    assert category == "UNIMPLEMENTED_SYSCALL"
    assert "0xfd" in detail


def test_syscall_todo_alone():
    category, _ = classify_log.classify(BOOT + "[Syscall TODO] encoded=0x7 v1=0x7\n")
    assert category == "UNIMPLEMENTED_SYSCALL"


def test_iop_unhandled():
    text = BOOT + "[IOP/RPC trace:unhandled] sid=0x80000592 rpc=0x1 pc=0x0\n"
    category, detail = classify_log.classify(text)
    assert category == "IOP_UNHANDLED"
    assert "sid=0x80000592" in detail


def test_iop_error():
    category, detail = classify_log.classify(BOOT + "[ps2xIOP:error] import not supported\n")
    assert category == "IOP_ERROR"
    assert "import not supported" in detail


def test_startup_failed():
    category, _ = classify_log.classify("Failed to load ELF file: disc/SLPS_256.75\n")
    assert category == "STARTUP_FAILED"


def test_crash_message_at_the_end():
    category, detail = classify_log.classify(BOOT + "[terminate] std::exception: boom\n")
    assert category == "CRASH"
    assert "boom" in detail


def test_crash_signal_from_exit_status():
    category, detail = classify_log.classify(BOOT + "[smoke] exit status 139\n")
    assert (category, detail.split()[0]) == ("CRASH", "SIGSEGV")


def test_timeout_and_pipe_cap_are_not_crashes():
    for status in (124, 141):
        category, _ = classify_log.classify(BOOT + f"[smoke] exit status {status}\n")
        assert category == "NO_PROGRESS"


def test_ok_vsync_loop():
    text = BOOT + counters(600, 360) + "[smoke] exit status 124\n"
    assert classify_log.classify(text) == ("OK_VSYNC_LOOP", "600 360")


def test_ok_needs_both_counters_at_the_threshold():
    assert classify_log.classify(counters(300, 300))[0] == "OK_VSYNC_LOOP"
    assert classify_log.classify(counters(300, 240))[0] == "FRAME_LOOP_UNPACED"
    assert classify_log.classify(counters(240, 600))[0] == "NO_PROGRESS"


def test_frame_loop_unpaced():
    text = BOOT + counters(3000, 0)
    assert classify_log.classify(text) == ("FRAME_LOOP_UNPACED", "3000")


def test_no_progress_reports_the_last_progress_line():
    text = (BOOT + "[don-tty] loading modules\nINFO: TEXTURE: noise\n"
            "WARNING: noise too\n[smoke] exit status 124\n")
    category, detail = classify_log.classify(text)
    assert category == "NO_PROGRESS"
    assert detail == "[smoke] exit status 124" or "loading modules" in detail


def test_no_progress_on_empty_log():
    assert classify_log.classify("")[0] == "NO_PROGRESS"


def test_diagnostic_lines_are_not_progress():
    """[don-ram] and [don-gs] are diagnostics, not progress: they must not change a classification.

    DON_DUMP_RAM prints "[don-ram] RAM dump ..." after the run and DON_TRACE_GS_REGS prints
    "[don-gs] regs ..." during it. Both used to be taken for progress lines (the pattern only anchored
    the "don" prefix), so a classification made with either turned on reported the diagnostic as the last
    progress line.
    """
    ram_dump = "[don-ram] RAM dump work/ramdump/run.bin: 33554432 of 33554432 bytes\n"
    gs_regs = ("[don-gs] regs PMODE=0x8067 SMODE1=0x740814504 SMODE2=0x1 SRFSH=0x8 SYNCH1=0x7F5B61F06F040 "
               "SYNCH2=0x33A4D8 SYNCV=0xC7800601A01801 CSR=0x2200 IMR=0xFF00\n")
    noise = ram_dump + gs_regs

    base = BOOT + "[don-tty] loading modules\n"
    category, detail = classify_log.classify(base + noise)
    assert category == "NO_PROGRESS"
    assert "loading modules" in detail and "RAM dump" not in detail and "regs" not in detail

    # A run that reached the loop is still the loop, and one that did not is still NO_PROGRESS with the
    # same last real progress line.
    reached = BOOT + counters(600, 600)
    assert classify_log.classify(reached + noise) == classify_log.classify(reached)
    short = BOOT + counters(240, 600) + "[don-tty] loading modules\n"
    assert classify_log.classify(short + noise) == classify_log.classify(short)
    assert classify_log.classify(short + noise)[0] == "NO_PROGRESS"
    # The prefixes that do mean progress still match.
    assert "loading modules" in classify_log.classify(BOOT + noise + "[don-tty] loading modules\n")[1]
    assert classify_log.classify(BOOT + noise + "[don] frame 60\n")[1].startswith("[don] frame 60")


def test_priority_missing_function_over_ok():
    text = counters(600, 600) + "No exact recompiled function for guest PC 0x200000\n"
    assert classify_log.classify(text)[0] == "MISSING_FUNCTION"


def test_priority_syscall_over_iop():
    text = "[IOP/RPC trace:unhandled] sid=0x1\n[Syscall TODO] encoded=0x1\n"
    assert classify_log.classify(text)[0] == "UNIMPLEMENTED_SYSCALL"


def test_patterns_match_when_lines_run_together():
    text = "Starting execution at address 0x100008Error: No exact recompiled function for guest PC 0x1234 x\n"
    assert classify_log.classify(text) == ("MISSING_FUNCTION", "0x1234")


def test_main_writes_json_and_exit_codes(tmp_path, capsys):
    bad = tmp_path / "bad.log"
    bad.write_text("\n".join(f"line {n}" for n in range(100)) + "\n")
    assert classify_log.main(["classify_log.py", str(bad)]) == 1
    data = json.loads(bad.with_suffix(".json").read_text())
    assert data["category"] == "NO_PROGRESS"
    assert len(data["last_lines"]) == 30
    assert data["last_lines"][-1] == "line 99"

    good = tmp_path / "good.log"
    good.write_text(counters(600, 600))
    assert classify_log.main(["classify_log.py", str(good)]) == 0
    assert "RESULT: OK_VSYNC_LOOP 600 600" in capsys.readouterr().out


def test_missing_function_summary_includes_csv_row(tmp_path, capsys):
    log = tmp_path / "m.log"
    log.write_text("No exact recompiled function for guest PC 0x00138b10\n")
    assert classify_log.main(["classify_log.py", str(log)]) == 1
    out = capsys.readouterr().out
    assert "RESULT: MISSING_FUNCTION 0x00138b10" in out
    assert "row of config/functions.ghidra.csv" in out
