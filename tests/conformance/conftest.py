import sys
from pathlib import Path

# python -I does not put the test folder on sys.path; the modules of the harness sit next to the tests.
sys.path.insert(0, str(Path(__file__).resolve().parent))
