# 046 · The fast check run on the build that crashed for the owner

- **Date:** 2026-10-26
- **Task:** T-710
- **Status:** open (verdict: blind; the check has to change before it gates integration)

## Symptom

The owner's package from main `899dddd` crashed the whole program when a folder was chosen in the in-app file
browser. `8ef0961` (T-701) fixed it. Question of T-710: does today's fast check (`tools/run/setup_check.py`, with the
paths of `tools/run/setup_check_paths.py` from main) FAIL on `899dddd`?

## Investigation

What was run (no product code changed):

- A worktree of `899dddd` in a folder of the clone (`work/t710/old`, not `/tmp`, see Notes); main's
  `tools/run/setup_check.py` and `tools/run/setup_check_paths.py` copied into it; `third_party/` and `en_patch/`
  symlinked to the main checkout (the same as the isolation rule for worktrees).
- `DON_CHECK_BUILD_DIR=<worktree>/build/check-899 DON_JOBS=6 DON_CHECK_ISO=<clone>/en_patch/jpvoice/disc.iso
  uv run --no-project python -I tools/run/setup_check.py > work/t710/check-899.log 2>&1`, default mode, display 97.
- Build: `don` linked (171/171 steps). Summary: `setup_check: 13 passed, 13 failed (668 s)`.

Result of each path on `899dddd`:

| Paths | Result on 899dddd | Why |
|---|---|---|
| `s1-keys`, `s1-choose-dialog`, `s1-choose-then-back`, `resume-show`, `resume-continue` | PASS | - |
| `browse-places`, `browse-in-out-back`, `browse-empty-folder`, `browse-unreadable-folder`, `browse-odd-name-folder`, `browse-loop-link`, `browse-5000-files`, `browse-escape-to-places-and-up` | PASS (all 8) | no crash, no sanitizer line, exit 0 |
| `browse-choose-non-disc-file`, `browse-choose-junk-iso`, `browse-choose-truncated-iso`, `error-choose-another`, `error-copy-report`, `error-dismiss-back`, `error-try-again`, `choose-game-folder` | FAIL | the screen was reached, but the key printed is `checking-error` with no code; the path expects the code (E-DISC-3, E-FILE-4, E-PICK-3). Code added by T-709, not on 899dddd |
| `s1-quit`, `drop-disc` | FAIL | did not exit in time (hung); `s1-quit` reached the browser (expected `welcome` only) |
| `disc-use-stop-early`, `stop-while-preparing` | FAIL | no `welcome-offer` screen on 899dddd; `stop-while-preparing` also uses `now:` (key unknown to 899dddd) |
| `drop-not-disc` | FAIL | `drop:` is unknown to 899dddd: the window stays on `welcome` |

Paths that use script keys 899dddd does not know (apart from the list above): `stop-while-preparing` (`now:`),
`drop-disc` and `drop-not-disc` (`drop:`). They fail for that reason only.

The crash, from the fix: `8ef0961` changes `FileBrowserModel::open_dir` from `const fs::path&` to a value parameter
(`src/host/file_browser.h`, `.cpp`). On `899dddd`, `navigate` on a folder row calls `open_dir(e.path)` with `e` a
reference into `entries_`; `open_dir` then runs `entries_.clear()` and copies `dir` into `dir_`. The copy reads the
path that was just freed. Only a folder row confirmed inside a listing takes this action; the start places do not
(`open_dir(starts_[sel_])`).

## Cause

The use-after-free in `open_dir` (see above) is the cause of the owner's crash (T-701, fixed in `8ef0961`).

## Verdict

**Blind.** No browser path fails on `899dddd`: all eight browser paths that confirm or enter folders pass, including
`browse-empty-folder` and `browse-odd-name-folder`, which confirm a folder row inside a listing, i.e. the action
that reaches the bug. The FAIL lines above are other causes (missing trace codes, unknown script keys, a path the
build did not reach); none of them is the crash.

Why the check is blind to it:

1. **No sanitizer.** The check builds `don` as Dev `-O2 -DNDEBUG` with no AddressSanitizer. A read of freed memory
   usually returns the old bytes, so nothing is logged and the process exits 0. The T-701 test
   (`tests/setup/test_setup_browser_crash.cpp`) runs only under `-fsanitize=address,undefined`.
2. **Sizes.** The freed block is the listing being left. `browse-5000-files` enters the 5000-file folder from a
   small listing (Documents, about 8 rows), so the freed block is small. The owner's case probably had a large
   listing being left; no path in `setup_check_paths.py` leaves a large listing by confirming a folder row.
3. **Not run under the sanitizer build** in any gate.

## Fix

Not done (T-710 changes no product code). Proposed for the check (for the orchestrator to decide):

- add a path that confirms a folder row in a large listing (for example, `browse-5000-files` going into a folder
  inside `e-many` and back), and
- run the browser paths against a build with `-fsanitize=address,undefined` (the way `test_setup_browser_crash.cpp`
  is built), so that a use-after-free ends the run with an ASan line that `BAD_LOG_PATTERNS` already catches.

## Verification

- `tail -1 work/t710/check-899.log` → `setup_check: 13 passed, 13 failed (668 s)`.
- The FAIL lines of that log are the list above (13 paths).
- Worktree removed: `git worktree list` shows only the clone.

## Notes

- The check's screen trace on 899dddd prints the key only (`checking-error`), so the code expectations of T-709 fail
  there. This is not a fault of the check on current main.
- The first attempt used `/tmp/t710-old`, as the card says. The container of `tools/container/run.sh` did not see
  the worktree that was created in this agent's shell (`/tmp` differs between the two; the container found an old
  `/tmp/t710-old` with a build of 22:24 that this agent cannot see or remove). Its build was not used. The worktree
  was moved to `work/t710/old` in the clone, which the container mounts. That stale `/tmp/t710-old` is still on the
  machine: whoever has `/tmp` on the host (outside this sandbox) should remove it with
  `git worktree prune` and `rm -rf /tmp/t710-old`.
- Build flags of the check are Dev `-O2 -DNDEBUG`, not the release build of the owner, and the owner's build was not
  sanitized either.

## Temporary diagnostics

None. The check ran unchanged from main (`tools/run/setup_check.py`, `setup_check_paths.py`).

## Review notes (orchestrator, 2026-10-26)

- The two "hung" paths on `899dddd` are not crashes: `s1-quit` presses keys laid out for today's S1 menu, which T-702
  changed after `899dddd`, so on the old menu they open the browser and the path waits for an exit that never comes;
  `drop-disc` uses `drop:`, unknown to `899dddd`, so the window stays on `welcome` (log: `screens welcome`).
- "Sizes" (cause 2 above) is a hypothesis, not measured. Cause 1 (no sanitizer) is the one T-713 tests: the same
  paths with `-fsanitize=address,undefined` on `899dddd` must end with an AddressSanitizer line.
- The run's log stayed in the T-710 clone (`work/t710/check-899.log`, not versioned): `setup_check: 13 passed, 13
  failed (668 s)`; its FAIL lines match the table above.

## Correction 2026-10-27 (T-713): the sanitizer build did not make the check see the bug

**Status: still open.** T-713 changed the check as T-710 proposed, and it still does not fail on `899dddd`.

What changed in the check (`tools/run/setup_check*.py`):

- `don` is built with `-DDON_SANITIZE=address,undefined` in `build/check-asan` (`DON_CHECK_SANITIZE=""` gives a plain
  build); runs use `ASAN_OPTIONS=halt_on_error=1:abort_on_error=0:detect_leaks=0:exitcode=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=1`. Leak detection is off: it was not measured clean on main.
- The image `don-build:1` has clang 17.0.6 but no `libclang_rt.asan*` (journal 015), so the first link failed. The check
  now fetches the same release's `libclang-rt-17-dev` (apt.llvm.org, SHA-256 pinned) into `build/check-asan-rt` and passes
  `-resource-dir`. Nothing is installed; a runtime in the image would make this step unnecessary.
- New path `browse-5000-files-enter-folder`: into `e-many` (now holding a folder `a-sub` and 5000 files), confirm the folder
  row, back, confirm it again, back. Default path timeout 90 -> 180 s: under ASan the 8 % preparing paths take 56-84 s and
  `drop-disc` timed out once at 90 s on a warm run (passed 3/3 alone afterwards).

Measured on this branch: cold `27 passed, 0 failed (661 s)`; warm `27 passed, 0 failed (400 s)` (the first warm run, with the
90 s timeout, was `26 passed, 1 failed (522 s)`: `drop-disc` hung by timeout).

Measured on `899dddd` (main's check files copied in, same sanitizer build): `14 passed, 13 failed (908 s)`. The 13 FAILs are
the same kinds as in T-710 (unknown script keys, no `welcome-offer`, error code missing from the trace). **No AddressSanitizer
or `runtime error:` line anywhere; `browse-5000-files-enter-folder` PASSED on 899dddd.** The binary was instrumented
(`__asan_init` present). Attempted: `free_fill_byte=255:max_free_fill_size=4096:quarantine_size_mb=0` on the old build: still PASS.

Likely reason (unverified): the read of the dangling `dir` happens inside libstdc++'s `directory_iterator(dir)`, which is not
instrumented, and no ASan interceptor covers `opendir`'s path argument. The unit test `test_setup_browser_crash.cpp` is the
tool that sees this bug; the fast check with an instrumented `don` does not. Verdict: the check stays blind to this class; what
catches it is a sanitizer unit test in the gates, not the real-program walk.
