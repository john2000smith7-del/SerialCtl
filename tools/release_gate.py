#!/usr/bin/env python3
"""Bind a formal release to successful exact-main Windows evidence; no keys."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


WINDOWS_JOBS = {
    "build", "Native callback destruction memory safety",
    "Packaged frequent-click and layout endurance",
    *("UI screenshots - " + page for page in
      ("ai", "power-light", "power-dark", "sftp-settings", "workspace")),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def version_tuple(value):
    require(isinstance(value, str) and re.fullmatch(r"V(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value),
            "Expected canonical product version Vx.y.z")
    return tuple(map(int, value[1:].split(".")))


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def validate_upgrade_report(report, archive_sha, metadata_sha):
    require(report.get("status") == "PASS" and report.get("candidateSha256") == archive_sha,
            "Actual upgrade report did not pass on this ZIP")
    require(report.get("metadataSha256") == metadata_sha, "Actual upgrade used different JSON")
    versions = (report["previousVersion"], report["olderVersion"])
    require(version_tuple(versions[1]) < version_tuple(versions[0]), "Previous and older clients must differ")
    expected = (
        {"invalidSignature"}, {"corruptArchive", "oldExecutablePreserved"},
        {"signature", "installation", "healthyRestart", "configurationAndNotebook", "driverAndUserFiles"},
    )
    cases = report.get("cases", [])
    require(len(cases) == 6, "Actual upgrade evidence must contain all six client cases")
    for version in versions:
        rows = [row for row in cases if row.get("from") == version]
        require(len(rows) == 3, "Missing actual cases for " + version)
        for fields in expected:
            require(sum(all(row.get(field) == "PASS" for field in fields) for row in rows) == 1,
                    "Missing actual rejection/install/preservation evidence for " + version)


def validate(request, runs, receipts, main_sha, latest_version, metadata):
    require(re.fullmatch(r"[0-9a-f]{40}", request["source_commit"]), "Invalid source SHA")
    require(re.fullmatch(r"[0-9a-f]{64}", request["archive_sha256"]), "Invalid archive SHA-256")
    require(request["source_commit"] == main_sha, "Release must use current exact main")
    require(version_tuple(request["version"]) > version_tuple(latest_version),
            "Formal release version must exceed the latest release")
    payload = json.loads(json.loads(metadata)["payload"])
    require(payload["version"] == request["version"] and
            payload["archive"]["sha256"] == request["archive_sha256"],
            "Public signed metadata differs from requested version/archive")
    metadata_sha = hashlib.sha256(metadata).hexdigest()
    for kind, workflow, jobs in (
            ("build", "windows-build.yml", WINDOWS_JOBS),
            ("desktop", "desktop-interaction.yml", {"desktop"}),
            ("upgrade", "verify-release-upgrade.yml", {"upgrade"})):
        run = runs[kind]
        require(str(run["id"]) == str(request[kind + "_run_id"]), "Wrong " + kind + " run ID")
        require(run["status"] == "completed" and run["conclusion"] == "success",
                kind + " workflow has not completely succeeded")
        require(run["head_branch"] == "main" and run["head_sha"] == main_sha,
                kind + " workflow is not the exact main commit")
        require(run["path"] == ".github/workflows/" + workflow, "Wrong workflow identity")
        events = ({"workflow_run", "workflow_dispatch"} if kind == "desktop" else
                  {"workflow_dispatch"} if kind == "upgrade" else {"push", "workflow_dispatch"})
        require(run["event"] in events, "Wrong " + kind + " workflow event")
        successful = {job["name"] for job in run["jobs"]
                      if job["status"] == "completed" and job["conclusion"] == "success"}
        require(jobs <= successful, kind + " required jobs were skipped or failed")
        if kind == "build":
            continue
        receipt = receipts[kind]
        require(receipt.get("schema") == 1 and receipt.get("kind") == kind and
                receipt.get("status") == "PASS", "Missing successful " + kind + " receipt")
        require(str(receipt.get("build_run_id")) == str(request["build_run_id"]) and
                receipt.get("source_commit") == main_sha and
                receipt.get("archive_sha256") == request["archive_sha256"],
                kind + " evidence belongs to another build or ZIP")
        if kind == "upgrade":
            require(receipt.get("metadata_sha256") == metadata_sha,
                    "Upgrade did not test these exact signed JSON bytes")
            require(receipt.get("previous_version") == latest_version,
                    "Upgrade evidence omitted the actual previous formal version")
            require(version_tuple(receipt.get("older_version")) < version_tuple(latest_version),
                    "Upgrade evidence needs a distinct older original-identity client")
    return {"status": "PASS", "source_commit": main_sha,
            "archive_sha256": request["archive_sha256"], "metadata_sha256": metadata_sha}


def gh(repository, endpoint):
    result = subprocess.run(["gh", "api", "repos/" + repository + "/" + endpoint],
                            check=True, capture_output=True, text=True)
    return json.loads(result.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    receipt = sub.add_parser("receipt")
    receipt.add_argument("--kind", choices=("desktop", "upgrade"), required=True)
    for name in ("archive", "build-run-id", "source-commit", "output"):
        receipt.add_argument("--" + name, required=True)
    receipt.add_argument("--metadata")
    receipt.add_argument("--report")
    check = sub.add_parser("check")
    for name in ("version", "source-commit", "archive-sha256", "build-run-id", "desktop-run-id", "upgrade-run-id",
                 "metadata", "evidence-dir"):
        check.add_argument("--" + name, required=True)
    check.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY"))
    args = parser.parse_args()
    if args.command == "receipt":
        require(re.fullmatch(r"[0-9a-f]{40}", args.source_commit), "Invalid source SHA")
        require(re.fullmatch(r"[0-9]+", args.build_run_id), "Invalid build run ID")
        data = {"schema": 1, "kind": args.kind, "status": "PASS",
                "build_run_id": args.build_run_id, "source_commit": args.source_commit,
                "archive_sha256": digest(args.archive)}
        if args.kind == "upgrade":
            require(args.metadata and args.report, "Upgrade receipt requires signed JSON and actual test report")
            report = json.loads(Path(args.report).read_text(encoding="utf-8-sig"))
            validate_upgrade_report(report, data["archive_sha256"], digest(args.metadata))
            data.update(metadata_sha256=report["metadataSha256"], previous_version=report["previousVersion"],
                        older_version=report["olderVersion"])
        Path(args.output).write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        return
    require(args.repository and re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository), "Invalid repository")
    request = vars(args)
    runs = {}
    for kind in ("build", "desktop", "upgrade"):
        run_id = request[kind + "_run_id"]
        require(re.fullmatch(r"[0-9]+", run_id), "Invalid run ID")
        runs[kind] = gh(args.repository, "actions/runs/" + run_id)
        # Current workflows have fewer than 100 jobs; refuse truncation.
        jobs = gh(args.repository, "actions/runs/" + run_id + "/jobs?per_page=100")
        require(jobs["total_count"] == len(jobs["jobs"]), "Job list is incomplete")
        runs[kind]["jobs"] = jobs["jobs"]
    receipts = {kind: json.loads((Path(args.evidence_dir) / kind / "release-receipt.json").read_text(encoding="utf-8-sig"))
                for kind in ("desktop", "upgrade")}
    latest = gh(args.repository, "releases/latest")["tag_name"]
    require(re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:-r[1-9][0-9]*)?", latest), "Unrecognized previous release tag")
    latest_version = "V" + latest[1:].split("-r", 1)[0]
    result = validate(request, runs, receipts, gh(args.repository, "git/ref/heads/main")["object"]["sha"],
                      latest_version, Path(args.metadata).read_bytes())
    print(json.dumps(result))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        # Command errors can include redirected signed URLs; never dump stderr.
        raise SystemExit("Release gate rejected: " + (str(error) if isinstance(error, ValueError) else type(error).__name__))
