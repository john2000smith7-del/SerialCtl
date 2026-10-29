"""Extract failed real-input observations from an existing desktop audit artifact."""

import json
import sys
from pathlib import Path


def main(directory):
    reports = []
    for path in sorted(Path(directory).rglob("*-desktop-user-actions.json")):
        report = json.loads(path.read_text(encoding="utf-8-sig"))
        failures = []
        for operation in report.get("operations", []):
            if operation.get("status") != "FAIL":
                continue
            observations = operation.get("single_click_observations", [])
            last = observations[-1] if observations else {}
            second = last.get("seconds")
            nearby = [event for event in report.get("input_trace", [])
                      if second is not None and abs(event.get("seconds", -9999) - second) < 0.5]
            failures.append({"name": operation.get("name"), "error": operation.get("error"),
                             "initial_toggle_bounds": operation.get("initial_toggle_bounds"),
                             "stationary_pointer": operation.get("stationary_pointer"),
                             "last_clicks": observations[-2:], "nearby_input": nearby})
        reports.append({"file": path.name, "status": report.get("status"),
                        "behavior_failures": report.get("behavior_failures"), "operations": failures})
    if not reports:
        raise SystemExit("No original desktop user-action reports found")
    print(json.dumps(reports, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: inspect_desktop_report.py ARTIFACT_DIRECTORY")
    main(sys.argv[1])
