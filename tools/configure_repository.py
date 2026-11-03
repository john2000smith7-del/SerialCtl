#!/usr/bin/env python3
"""Inspect/apply reviewed GitHub settings. Uses gh authentication; never asks for tokens."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "john2000smith7-del/SerialCtl"


def api(endpoint, method="GET", data=None):
    command = ["gh", "api", "repos/" + REPOSITORY + ("/" + endpoint if endpoint else ""), "--method", method]
    with tempfile.TemporaryDirectory(prefix="serialctl-settings-") as directory:
        if data is not None:
            source = Path(directory) / "request.json"
            source.write_text(json.dumps(data), encoding="utf-8")
            command += ["--input", str(source)]
        result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        # API errors only; no environment dump, credential prompts or redirected URLs.
        message = next((line for line in result.stderr.splitlines() if line.startswith("gh:")), "GitHub API unavailable")
        raise RuntimeError(message)
    return json.loads(result.stdout) if result.stdout.strip() else {}


def contains(actual, expected):
    if isinstance(expected, dict):
        return isinstance(actual, dict) and all(k in actual and contains(actual[k], v) for k, v in expected.items())
    if isinstance(expected, list):
        return isinstance(actual, list) and len(actual) == len(expected) and all(
            any(contains(item, wanted) for item in actual) for wanted in expected)
    return actual == expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="Apply reviewed settings; default is read-only")
    parser.add_argument("--include-rulesets", action="store_true", help="Explicitly include optional paid private-repository rulesets")
    parser.add_argument("--output", type=Path, help="Write actual results outside the repository")
    args = parser.parse_args()
    if args.output and (ROOT in args.output.resolve().parents or not args.output.parent.is_dir()):
        raise SystemExit("Settings report must use an existing directory outside the repository")
    desired = json.loads((ROOT / ".github/repository-settings.json").read_text())
    repo = api("")
    if not repo.get("private"):
        raise SystemExit("Refusing to manage a public repository; review actual visibility first")
    results = []
    action_settings = {
        "workflow-token": ("actions/permissions/workflow", "workflow-token-permissions.json"),
        "actions-sharing": ("actions/permissions/access", "actions-sharing.json"),
        "actions-permissions": ("actions/permissions", "actions-permissions.json"),
        "allowed-actions": ("actions/permissions/selected-actions", "allowed-actions.json"),
    }
    for name in ("repository", "main", "release-tags", *action_settings):
        if name in ("main", "release-tags") and not args.include_rulesets:
            results.append({"setting": name, "status": "DEFERRED_CURRENT_PLAN"})
            continue
        try:
            if name == "repository":
                actual = repo
                if not contains(actual, desired) and args.apply:
                    api("", "PATCH", desired)
                    actual = api("")
                status = "ENFORCED" if contains(actual, desired) else "DIFFERENT"
            elif name in action_settings:
                endpoint, filename = action_settings[name]
                wanted = json.loads((ROOT / ".github" / filename).read_text())
                actual = api(endpoint)
                if not contains(actual, wanted) and args.apply:
                    api(endpoint, "PUT", wanted)
                    actual = api(endpoint)
                status = "ENFORCED" if contains(actual, wanted) else "DIFFERENT"
            else:
                wanted = json.loads((ROOT / ".github/rulesets" / (name + ".json")).read_text())
                records = api("rulesets?per_page=100")
                if len(records) >= 100:
                    raise RuntimeError("Ruleset list may be truncated; inspect pagination before changing")
                found = [r for r in records if r["name"] == wanted["name"]]
                if len(found) > 1:
                    raise RuntimeError("Duplicate named rulesets require review")
                endpoint = "rulesets/" + str(found[0]["id"]) if found else "rulesets"
                actual = api(endpoint) if found else {}
                if not contains(actual, wanted) and args.apply:
                    saved = api(endpoint, "PUT" if found else "POST", wanted)
                    actual = api("rulesets/" + str(saved["id"]))
                status = "ENFORCED" if contains(actual, wanted) else "DIFFERENT"
            results.append({"setting": name, "status": status})
        except RuntimeError as error:
            results.append({"setting": name, "status": "BLOCKED", "reason": str(error)})
    report = {"repository": REPOSITORY, "apply_requested": args.apply,
              "optional_rulesets_included": args.include_rulesets, "results": results}
    if args.output:
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    raise SystemExit(0 if all(r["status"] in ("ENFORCED", "DEFERRED_CURRENT_PLAN") for r in results) else 2)


if __name__ == "__main__":
    main()
