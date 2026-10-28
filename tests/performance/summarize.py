"""Summarize paired Windows benchmark logs, retaining the raw runs separately."""

import json
import re
import statistics
import sys
from pathlib import Path


def json_run(path, required):
    candidates = []
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and required <= value.keys():
            candidates.append(value)
    if len(candidates) != 1:
        raise ValueError(f"Expected exactly one benchmark result in {path}")
    return candidates[0]


def network_run(path):
    rows = {kind: {} for kind in ("forwarding", "load")}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        match = re.match(r"^(forwarding|load) (.+)$", line)
        if not match:
            continue
        values = dict(re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", match.group(2)))
        count = int(values.pop("clients"))
        if count in rows[match.group(1)]:
            raise ValueError(f"Duplicate {match.group(1)} {count}: {path}")
        rows[match.group(1)][count] = values
    if any(set(group) != {1, 2, 8, 32} for group in rows.values()):
        raise ValueError(f"Incomplete network results: {path}")
    result = {}
    for count in (1, 2, 8, 32):
        delay, load = rows["forwarding"][count], rows["load"][count]
        if int(delay["samples"]) != count * 1000 or int(load["verified_output_bytes"]) != count * 64000:
            raise ValueError(f"Invalid network sample/byte counts: {path}")
        wall = float(load["wall_ms"])
        if wall <= 0:
            raise ValueError(f"Invalid network duration: {path}")
        result[str(count)] = {
            "p50Ms": float(delay["P50_ms"]), "p95Ms": float(delay["P95_ms"]),
            "p99Ms": float(delay["P99_ms"]), "wallMs": wall,
            "cpuMs": float(load["cpu_ms"]),
            "verifiedBytesPerSecond": int(load["verified_output_bytes"]) * 1000 / wall,
            **{key: int(load[key]) for key in (
                "working_set", "peak_working_set", "threads_idle", "threads_active",
                "threads_reclaimed", "handles_idle", "handles_active", "handles_reclaimed")},
        }
    return result


def medians(runs):
    return {key: statistics.median(run[key] for run in runs) for key in runs[0]}


def main(directory):
    directory = Path(directory)
    output = {"environment": json.loads((directory / "environment.json").read_text(encoding="utf-8-sig")),
              "method": "Median of three separate runs; see raw logs for distributions. "
                        "Each network run checks 1000 packets/client on loopback; no physical UART.",
              "baseline": {}, "candidate": {}}
    for label in ("baseline", "candidate"):
        previews, paints, networks = [], [], []
        for index in (1, 2, 3):
            preview = json_run(directory / f"preview-{label}-{index}.txt", {"iterations", "samplesMs"})
            paint = json_run(directory / f"paint-{label}-{index}.txt", {"paintCount", "resizeCount"})
            if (preview["iterations"] != 30 or len(preview["samplesMs"]) != 30 or
                    paint["paintCount"] != 200 or len(paint["terminalPaintSamples"]) != 200 or
                    paint["resizeCount"] != 40 or len(paint["inputSamples"]) != 1000 or
                    paint["inputBytes"] != 1000):
                raise ValueError(f"Unexpected sample counts for {label} run {index}")
            previews.append({key: preview[key] for key in ("p50Ms", "p95Ms", "workingBeforeBytes",
                            "maxWorkingDuringPreviewBytes", "peakWorkingSetBytes")})
            previews[-1]["observedWorkingIncreaseBytes"] = (
                preview["maxWorkingDuringPreviewBytes"] - preview["workingBeforeBytes"])
            paints.append({key: paint[key] for key in ("paintP50Ms", "paintP95Ms",
                          "terminalPaintP50Ms", "terminalPaintP95Ms", "resizeP50Ms", "resizeP95Ms",
                          "inputP50Ms", "inputP95Ms", "inputWallMs", "inputCpuMs", "inputBytesPerSecond",
                          "wallMs", "cpuMs", "gdiBeforeApp", "gdiWarm", "gdiAfterWork",
                          "gdiAfterApp", "peakWorkingSetBytes")})
            networks.append(network_run(directory / f"network-{label}-{index}.txt"))
        output[label]["preview"] = medians(previews)
        output[label]["paint"] = medians(paints)
        output[label]["network"] = {str(count): medians([run[str(count)] for run in networks])
                                     for count in (1, 2, 8, 32)}
    target = directory / "comparison.json"
    target.write_text(json.dumps(output, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(target.read_text(encoding="utf-8"))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: summarize.py EVIDENCE_DIRECTORY")
    main(sys.argv[1])
