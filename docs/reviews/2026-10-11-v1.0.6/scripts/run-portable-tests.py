#!/usr/bin/env python3
"""Audit-only runner. Does not edit production files or claim Windows coverage."""
import datetime
import json
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
src = root / "source"
out = root / "evidence" / "local-tests"
out.mkdir(parents=True, exist_ok=True)
cases = [
    ("terminal-model", ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Isrc/terminal",
      "tests/unit/TerminalModelTest.cpp", "src/terminal/TerminalModel.cpp"]),
    ("terminal-model-sanitized", ["g++", "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
      "-fno-omit-frame-pointer", "-Isrc/terminal", "tests/unit/TerminalModelTest.cpp", "src/terminal/TerminalModel.cpp"]),
]
results = []
for name, command in cases:
    command += ["-o", str(out / (name + "-test"))]
    for stage, cmd in [("build", command), ("run", [str(out / (name + "-test"))])]:
        r = subprocess.run(cmd, cwd=src, text=True, capture_output=True)
        (out / (name + "-" + stage + ".log")).write_text(
            "COMMAND: " + repr(cmd) + "\n" + r.stdout + r.stderr + "\nEXIT: " + str(r.returncode) + "\n")
        results.append({"test": name, "stage": stage, "exit": r.returncode,
                        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat()})
        print(name, stage, r.returncode, flush=True)
        if r.returncode:
            break
(out / "results.json").write_text(json.dumps(results, indent=2))
sys.exit(any(r["exit"] for r in results))
