#!/usr/bin/env python3
"""Exact-run CI cache transport. Missing evidence fails; formal assets stay separate."""
import argparse
import glob
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile
import zipfile

MAX_BYTES = 8 * 1024**3
FORMAT = "serialctl-evidence-v1"


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def identity(run, source, name, attempt=None):
    require(re.fullmatch(r"[1-9][0-9]*", str(run)), "Invalid evidence run")
    require(re.fullmatch(r"[0-9a-f]{40}", source), "Invalid evidence source")
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,99}", name), "Invalid evidence name")
    value = {"schema": 1, "run_id": str(run), "source_sha": source, "name": name}
    if attempt is not None:
        require(re.fullmatch(r"[1-9][0-9]*", str(attempt)), "Invalid evidence attempt")
        value["attempt"] = int(attempt)
    return value


def prefix(meta):
    return f'{FORMAT}-{meta["run_id"]}-{meta["source_sha"]}-{meta["name"]}-a'


def key(meta):
    return prefix(meta) + str(meta["attempt"])


def safe_name(name):
    path = PurePosixPath(name)
    require(bool(name) and "\\" not in name and ":" not in name and
            not path.is_absolute() and all(p not in ("", ".", "..") for p in name.split("/")),
            "Unsafe evidence member")
    return path


def select_files(patterns):
    files, roots = set(), []
    for pattern in patterns.splitlines():
        pattern = pattern.strip()
        if not pattern:
            continue
        require("!" not in pattern, "Exclusion patterns are not supported")
        expanded = Path(pattern).expanduser().absolute()
        before_glob = str(expanded)
        wildcard = re.search(r"[*?[]", before_glob)
        if wildcard:
            before_glob = before_glob[:wildcard.start()]
            root = Path(before_glob).parent if not before_glob.endswith(("/", "\\")) else Path(before_glob)
        else:
            root = expanded if expanded.is_dir() else expanded.parent
        selected = []
        for match in (Path(p) for p in glob.glob(str(expanded), recursive=True)):
            selected.extend(match.rglob("*") if match.is_dir() else [match])
        actual = [p for p in selected if p.is_file()]
        if actual:
            roots.append(str(root))
        for path in actual:
            require(not path.is_symlink() and path.resolve() == path.absolute(), "Symlink evidence")
            files.add(path)
    require(bool(files), "No evidence files found")
    common = Path(os.path.commonpath(roots))
    return [(p, p.relative_to(common).as_posix()) for p in sorted(files)]


def pack(destination, meta, patterns):
    selected = select_files(patterns)
    require(sum(p.stat().st_size for p, _ in selected) <= MAX_BYTES, "Evidence exceeds transport budget")
    manifest = dict(meta, files=[])
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
        for path, name in selected:
            safe_name(name)
            record = {"name": name, "size": path.stat().st_size, "sha256": sha256(path)}
            archive.write(path, "files/" + name)
            require(path.stat().st_size == record["size"] and sha256(path) == record["sha256"],
                    "Evidence changed while packing")
            manifest["files"].append(record)
        archive.writestr("manifest.json", json.dumps(manifest, sort_keys=True).encode("utf-8"))
    return manifest


def unpack(source, destination, expected):
    destination = Path(destination).absolute()
    destination.mkdir(parents=True, exist_ok=True)
    require(destination.resolve() == destination, "Symlink evidence destination")
    with zipfile.ZipFile(source) as archive:
        require(len(archive.infolist()) <= 20000, "Too many evidence files")
        names = archive.namelist()
        require(len(names) == len(set(names)) and names.count("manifest.json") == 1,
                "Missing/duplicate evidence manifest or members")
        info = archive.getinfo("manifest.json")
        require(info.file_size <= 4 * 1024 * 1024, "Manifest too large")
        manifest = json.loads(archive.read(info))
        require(all(manifest.get(k) == v for k, v in expected.items()), "Evidence source/run/name/attempt mismatch")
        files = manifest.get("files", [])
        require(isinstance(files, list) and bool(files), "Empty evidence manifest")
        seen, total = set(), 0
        for row in files:
            name = row["name"]
            safe_name(name)
            require(name.casefold() not in seen, "Case-colliding evidence files")
            seen.add(name.casefold())
            require(type(row["size"]) is int and row["size"] >= 0 and
                    re.fullmatch(r"[0-9a-f]{64}", row["sha256"]), "Invalid evidence file record")
            member = archive.getinfo("files/" + name)
            require(member.file_size == row["size"] and not member.is_dir() and
                    ((member.external_attr >> 16) & 0o170000) != 0o120000, "Invalid evidence member")
            total += member.file_size
        require(total <= MAX_BYTES and set(names) == {"manifest.json", *("files/" + row["name"] for row in files)},
                "Unexpected or oversized evidence members")
        # Verify the whole container before publishing any restored file.
        with tempfile.TemporaryDirectory(prefix="serialctl-evidence-") as stage:
            for index, row in enumerate(files):
                path = Path(stage) / str(index)
                with archive.open("files/" + row["name"]) as inp, path.open("wb") as out:
                    shutil.copyfileobj(inp, out, 1024 * 1024)
                require(sha256(path) == row["sha256"], "Evidence SHA-256 mismatch")
            for index, row in enumerate(files):
                path = destination.joinpath(*safe_name(row["name"]).parts)
                require(path.resolve().is_relative_to(destination), "Evidence path escaped destination")
                if path.exists():
                    require(path.is_file() and sha256(path) == row["sha256"], "Evidence would overwrite different data")
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(Path(stage) / str(index), path)
    return manifest


def gh(repository, endpoint):
    result = subprocess.run(["gh", "api", "repos/" + repository + "/" + endpoint],
                            check=True, capture_output=True, text=True)
    return json.loads(result.stdout)


def resolve(repository, run_id, name):
    from urllib.parse import urlencode
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository), "Invalid repository")
    require(re.fullmatch(r"[1-9][0-9]*", str(run_id)), "Invalid source run")
    run = gh(repository, "actions/runs/" + str(run_id))
    require(run["head_repository"]["full_name"].lower() == repository.lower(), "Foreign source repository")
    require(run["event"] in ("push", "workflow_dispatch", "workflow_run"), "Untrusted evidence event")
    meta = identity(run_id, run["head_sha"], name)
    result, page = [], 1
    while True:
        response = gh(repository, "actions/caches?" + urlencode({
            "key": prefix(meta), "ref": "refs/heads/" + run["head_branch"], "per_page": 100, "page": page}))
        rows = response["actions_caches"]
        result.extend(rows)
        require(page <= 20, "Evidence cache listing too large")
        if len(rows) < 100:
            break
        page += 1
    matches = []
    for row in result:
        suffix = row["key"].removeprefix(prefix(meta))
        if row["key"].startswith(prefix(meta)) and re.fullmatch(r"[1-9][0-9]*", suffix):
            attempt = int(suffix)
            if attempt <= run["run_attempt"]:
                matches.append(attempt)
    require(bool(matches), "Exact-run cache missing/evicted; rebuild required (no substitute artifact)")
    return identity(run_id, run["head_sha"], name, max(matches))


def output(values):
    with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as out:
        for name, value in values.items():
            out.write(f"{name}={value}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "verify"))
    parser.add_argument("--mode", choices=("save", "restore"), required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID"))
    parser.add_argument("--attempt")
    parser.add_argument("--source-sha")
    parser.add_argument("--path", default=os.environ.get("EVIDENCE_PATHS", ""))
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY"))
    args = parser.parse_args()
    if args.command == "prepare":
        if args.mode == "save":
            source = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
            meta = identity(os.environ["GITHUB_RUN_ID"], source, args.name, os.environ["GITHUB_RUN_ATTEMPT"])
        else:
            meta = resolve(args.repository, args.run_id, args.name)
        archive = Path(".ci-evidence") / key(meta) / "bundle.zip"
        if args.mode == "save":
            pack(archive, meta, args.path)
        output({"key": key(meta), "path": archive.as_posix(), "source": meta["source_sha"],
                "attempt": meta["attempt"], "run": meta["run_id"]})
    else:
        meta = identity(args.run_id, args.source_sha, args.name, args.attempt)
        archive = Path(".ci-evidence") / key(meta) / "bundle.zip"
        if args.mode == "restore":
            manifest = unpack(archive, args.path, meta)
            print(f'PASS: restored {len(manifest["files"])} exact-run files ({key(meta)})')
        else:
            # cache/save can warn on a storage failure: require actual API visibility.
            require(resolve(args.repository, args.run_id, args.name) == meta, "Evidence cache was not saved")
            print("PASS: evidence cache entry confirmed: " + key(meta))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        # Never dump redirected signed storage URLs or credential-bearing stderr.
        raise SystemExit("CI evidence rejected: " + (str(error) if isinstance(error, ValueError) else type(error).__name__))
