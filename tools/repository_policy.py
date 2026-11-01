#!/usr/bin/env python3
"""Lightweight development policy checks, not a substitute for Windows acceptance."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


BRANCH = re.compile(r"feat/[A-Za-z0-9][A-Za-z0-9_-]*_v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)_[0-9]{8}_[0-9]{4}")
TITLE = re.compile(r"(?:feat|fix|docs|test|build)\((V[0-9]+\.[0-9]+\.[0-9]+)\): \S.+")
PRIVATE_KEY = re.compile(rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH )?PRIVATE KEY-----\s+[A-Za-z0-9+/=\r\n]{80,}-----END (?:RSA |EC |DSA |OPENSSH )?PRIVATE KEY-----")
TOKEN = re.compile(rb"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16})")


def validate_message(message, version):
    lines = message.splitlines()
    match = TITLE.fullmatch(lines[0] if lines else "")
    if not match or match[1] != version:
        raise ValueError("Commit/PR title must be type(Vx.y.z): summary and match VERSION")
    sections = {line.lstrip("# ").strip() for line in lines}
    if not all(section in sections for section in ("修改内容：", "验证情况：", "兼容性：")):
        raise ValueError("Commit/PR body needs modification, verification and compatibility sections")


def git(*args):
    return subprocess.check_output(["git", *args])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--branch", default=os.environ.get("POLICY_BRANCH", "main"))
    parser.add_argument("--pr-message", type=Path)
    parser.add_argument("--event-path", default=os.environ.get("GITHUB_EVENT_PATH"))
    args = parser.parse_args()
    event = json.loads(Path(args.event_path).read_text()) if args.event_path else {}
    pull = event.get("pull_request")
    if pull:
        args.base, args.head, args.branch = pull["base"]["sha"], pull["head"]["sha"], pull["head"]["ref"]
    elif "before" in event:
        args.base = event["before"]
    if args.branch.startswith("feat/"):
        import datetime
        try:
            datetime.datetime.strptime(args.branch.rsplit("_", 2)[1] + "_" + args.branch.rsplit("_", 2)[2], "%Y%m%d_%H%M")
        except (ValueError, IndexError):
            raise ValueError("Invalid branch date/time") from None
    if args.branch != "main" and not BRANCH.fullmatch(args.branch):
        raise ValueError("Temporary branch must be feat/topic_vx.y.z_YYYYMMDD_HHMM (Beijing time)")
    version_lines = git("show", args.head + ":VERSION").decode().splitlines()
    if len(version_lines) != 2 or version_lines[0].replace("PROJECT_VERSION=", "CPP_VERSION=", 1) != version_lines[1]:
        raise ValueError("VERSION entries disagree")
    version = version_lines[0].split("=", 1)[1]
    if not re.fullmatch(r"V(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version):
        raise ValueError("VERSION is not canonical Vx.y.z")
    if args.branch != "main" and ".".join(BRANCH.fullmatch(args.branch).groups()) != version[1:]:
        raise ValueError("Branch product version differs from VERSION")
    if pull:
        validate_message(pull["title"] + "\n\n" + (pull.get("body") or ""), version)
    if args.pr_message:
        validate_message(args.pr_message.read_text(encoding="utf-8"), version)
    if args.base and set(args.base) != {"0"}:
        for sha in git("rev-list", "--no-merges", args.base + ".." + args.head).decode().splitlines():
            commit_version = git("show", sha + ":VERSION").decode().splitlines()[0].split("=", 1)[1]
            validate_message(git("show", "-s", "--format=%B", sha).decode(), commit_version)
    # No secret values are printed. Large/binary assets are outside this basic guard.
    files = git("ls-files", "-z").decode().split("\0")
    for name in files:
        if not name:
            continue
        path = Path(name)
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 2 * 1024 * 1024:
            continue
        data = path.read_bytes()
        if PRIVATE_KEY.search(data) or TOKEN.search(data):
            raise ValueError("Possible credential material in tracked file: " + name)
    for workflow in Path(".github/workflows").glob("*.yml"):
        for action in re.findall(r"uses:\s*([^\s#]+)", workflow.read_text(encoding="utf-8")):
            if not action.startswith("./") and not re.fullmatch(r"[A-Za-z0-9_./-]+@[0-9a-f]{40}", action):
                raise ValueError("Workflow action must use a full pinned commit: " + str(workflow))
    print("PASS: repository policy (basic pattern guard; Windows/real-device validation remains required)")


if __name__ == "__main__":
    try:
        main()
    except ValueError as error:
        raise SystemExit(str(error))
