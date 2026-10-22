#!/usr/bin/env python3
"""Real verified ZIP -> temporary test signature -> native production extraction.

Does not install or execute the archived application and never uses a production
private key. Temporary key/metadata/output are deleted after the smoke test.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from test_new_update_feed import feed, make_key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("native_test", type=Path)
    parser.add_argument("--wine", type=Path, help="Optional Linux smoke wrapper; native Windows is the release gate")
    args = parser.parse_args()
    openssl = shutil.which("openssl")
    if not openssl:
        openssl = str(Path("C:/Program Files/Git/usr/bin/openssl.exe"))
    with tempfile.TemporaryDirectory(prefix="serialctl-real-zip-test-") as temporary:
        root = Path(temporary)
        key, header = make_key(root, "temporary-test-key", openssl)
        publication = root / "publication"
        feed.build_archive_feed(args.archive, publication, key, header, openssl)
        blob = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", header.read_text()))
        public = root / "test-public.bin"
        public.write_bytes(blob)
        paths = [publication / "update.json", publication / args.archive.name, public]
        if args.wine:
            paths = ["Z:" + str(path.resolve()).replace("/", "\\") for path in paths]
        command = [str(args.native_test), "--verify-archive", *map(str, paths)]
        if args.wine:
            command.insert(0, str(args.wine))
        subprocess.run(command, check=True, timeout=90)
    print("Full release ZIP/native extraction passed using isolated ephemeral test credentials")


if __name__ == "__main__":
    main()
