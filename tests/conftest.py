import sys
from pathlib import Path

# python -I does not add the script folder to sys.path; the tests import the modules in tools/.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: runs the real game for minutes (tests/regression); skipped unless -m slow is given")


def pytest_collection_modifyitems(config, items):
    import pytest
    if "slow" in (config.getoption("markexpr") or ""):
        return
    skip = pytest.mark.skip(reason="slow: run with -m slow (tests/regression runs the real game)")
    for item in items:
        if "slow" in item.keywords:
            item.add_marker(skip)
