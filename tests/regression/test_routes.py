"""Regression check of the real game (T-209): each route of tools/run/scripts/ listed in routes.json is run with
the recompiled game, `--sound off`, and must exit 0, keep its frame loop alive (tools/verify/liveness.py) and
reproduce the md5 of every landmark shot recorded in routes.json.

    uv run --no-project --with pytest python -I -m pytest tests/regression -m slow -q

It is marked `slow` and is skipped unless `-m slow` is given (tests/conftest.py): a route takes minutes.
The game binary is build/linux-dev/don ($DON_BINARY) and the English variant is en_patch/jpvoice/disc/SLPS_256.75
($DON_REGRESSION_EXE); the suite is skipped, with the reason, when either is missing. Shots and logs stay in
work/regression/<route>/. To re-record after a change that is meant to alter pictures: record.py (this folder).
With DON_REGRESSION_SOUND=1 the routes are also run with sound (the default) and only their liveness is checked.
"""
import os

import pytest

import harness

pytestmark = pytest.mark.slow
ROUTES = harness.load_table()


@pytest.fixture(scope="module")
def ready():
    reason = harness.skip_reason("en-jpvoice")
    if reason:
        pytest.skip(reason)


@pytest.mark.parametrize("route", ROUTES, ids=[r["name"] for r in ROUTES])
def test_route(route, ready):
    # a route with "then" is two processes on the same card folder (harness.run_route): the second runs after the first
    for step in [route] + ([route["then"]] if "then" in route else []):
        code, shots, log = harness.run_route(route, step=step if step is not route else None)
        assert code == 0, f"{route['name']}: the game exited with {code}; log {log}"
        assert harness.alive(log) == 0, f"{route['name']}: the frame loop stopped; log {log}"
        wrong = []
        for lm in step["landmarks"]:
            shot = shots / f"{lm['name']}.png"
            got = harness.md5_of(shot) if shot.is_file() else "no shot taken"
            if got != lm["md5"]:
                wrong.append(f"  blank {lm['blank']} {lm['name']} ({lm['what']}): {got}, recorded {lm['md5']}; shot left at {shot}")
        assert not wrong, f"route {route['name']} ({step['script']}): landmarks changed:\n" + "\n".join(wrong)


@pytest.mark.skipif(not os.environ.get("DON_REGRESSION_SOUND"), reason="set DON_REGRESSION_SOUND=1 to run the routes with sound")
@pytest.mark.parametrize("route", ROUTES, ids=[r["name"] for r in ROUTES])
def test_route_alive_with_sound(route, ready):
    for step in [route] + ([route["then"]] if "then" in route else []):
        code, _, log = harness.run_route(route, sound=True, step=step if step is not route else None)
        assert code == 0, f"{route['name']} with sound: the game exited with {code}; log {log}"
        assert harness.alive(log) == 0, f"{route['name']} with sound: the frame loop stopped; log {log}"
