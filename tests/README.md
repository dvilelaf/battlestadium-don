# Tests

Run all tests from the repository root: `uv run --no-project --with pytest python -I -m pytest tests -q`

Run one file: `uv run --no-project --with pytest python -I -m pytest tests/test_fpk.py -q`

No test in this folder may need game files: build synthetic data in the test (with `tmp_path`) and never read `disc/`, `en_patch/` or `gc_rvz/`.

## Suites and how to run them

| Folder / file | Needs | What it checks |
|---|---|---|
| `tests/*.py`, `tests/audio`, `tests/gs`, `tests/vu`, ... | nothing of the game (some need a built helper, and skip with the reason) | tools and library pieces with synthetic data |
| `tests/regression/` | the game binary (`build/linux-dev/don`) and the English variant folder (`en_patch/jpvoice/disc/`); **slow, about 15 minutes** | the real game: the routes of `tools/run/scripts/` (default title idle, a practice fight past blank 7076, a two-pad Multiplayer match, Survival to its result screen) with `--sound off`: exit 0, liveness, and the md5 of every landmark shot |

`tests/regression` is marked `slow` (`tests/conftest.py`): `pytest tests -q` skips it with a message; run it with `-m slow`:

    uv run --no-project --with pytest python -I -m pytest tests/regression -m slow -q

Skipped, with the reason, when the binary or the variant folder is missing (`$DON_BINARY`, `$DON_REGRESSION_EXE` change the paths). The landmark table is `tests/regression/routes.json` (route, script, environment, and per landmark: blank, name, md5, what it shows). A failure names the route, the blank and the folder `work/regression/<route>/` where the shot was left. With `DON_REGRESSION_SOUND=1` the routes also run with sound and only their liveness is checked. After a change that is meant to alter pictures, re-record on purpose with `uv run --no-project python -I tests/regression/record.py [route ...]`, look at the shots and commit the table. The suite relies on `--sound off` giving a guest state that does not depend on host timing (patch 0038): two runs give the same pictures at the same blanks. The routes' timing holds only for the binary and `--sound off`: a run with sound moves the blanks (the Multiplayer match ends about 600 blanks earlier).
