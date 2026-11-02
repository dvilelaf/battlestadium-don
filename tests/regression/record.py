"""Re-records the md5 of the landmarks of routes.json: ON PURPOSE, after a change meant to alter pictures.

    uv run --no-project python -I tests/regression/record.py [route ...]    # all routes if none is named

Runs each route with `--sound off`, refuses to record when the run does not exit 0 or is not alive or a landmark
shot is missing, and writes the new md5 into routes.json (blanks, names and descriptions are kept). Look at the shots
in work/regression/<route>/ before committing the table; run it twice and compare to see that the landmarks hold.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import harness  # noqa: E402


def main(names):
    table = json.loads(harness.TABLE.read_text())
    reason = harness.skip_reason("en-jpvoice")
    if reason:
        print(reason)
        return 2
    for route in table["routes"]:
        if names and route["name"] not in names:
            continue
        for step in [route] + ([route["then"]] if "then" in route else []):
            code, shots, log = harness.run_route(route, step=step if step is not route else None)
            if code != 0 or harness.alive(log) != 0:
                print(f"{route['name']}: exit {code} or not alive; not recorded (log {log})")
                return 1
            for lm in step["landmarks"]:
                shot = shots / f"{lm['name']}.png"
                if not shot.is_file():
                    print(f"{route['name']}: no shot {lm['name']} (blank {lm['blank']}); not recorded")
                    return 1
                lm["md5"] = harness.md5_of(shot)
            print(f"{route['name']}: recorded {len(step['landmarks'])} landmarks, shots in {shots}")
        harness.TABLE.write_text(json.dumps(table, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
