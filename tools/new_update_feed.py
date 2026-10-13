#!/usr/bin/env python3
"""Build a signed, static SerialCtl update feed from an extracted release bundle.

Administrator-side tool only: Python 3 and OpenSSL are NOT client dependencies.
The private key and this tool never belong in the public HTTP directory.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


ALLOWED_FILES = (
    "serialctl.exe", "plink.exe", "psftp.exe", "VERSION", "README.md",
    "RELEASE-NOTES.md", "AUTO-UPDATE.md", "AI-API.md", "AI-SERIAL.md",
    "WEBSOCKET-PROTOCOL.md", "POWER.md", "THIRD-PARTY-NOTICES.md",
    "JSON-LICENSE.txt", "PUTTY-LICENCE.txt", "YY-THUNKS-LICENSE.txt",
    "WIN7-HARDWARE-TEST.md", "MANUAL-REGRESSION.md", "V1.0.6-ACCEPTANCE.md",
    "AUTO-UPDATE-ACCEPTANCE.md", "serialctl_client.py", "serialctl_api.py",
    "serialctl_ws.py", "常用命令示例.txt",
)
REQUIRED_FILES = ALLOWED_FILES[:4]
MAX_FILE_BYTES = 32 * 1024 * 1024
MAX_TOTAL_BYTES = 64 * 1024 * 1024


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_openssl(command, *arguments):
    result = subprocess.run([command, *map(str, arguments)], check=False,
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=30)
    if result.returncode:
        # Do not echo command arguments or key material into release logs.
        raise ValueError("OpenSSL failed; check the key, permissions, and OpenSSL installation")
    return result.stdout


def der(tag, content):
    size = len(content)
    length = bytes([size]) if size < 128 else size.to_bytes((size.bit_length() + 7) // 8, "big")
    if size >= 128:
        length = bytes([0x80 | len(length)]) + length
    return bytes([tag]) + length + content


def der_integer(number):
    value = number.to_bytes((number.bit_length() + 7) // 8, "big")
    if value[0] & 0x80:
        value = b"\x00" + value
    return der(0x02, value)


def trusted_public_der(header):
    source = header.read_text(encoding="utf-8-sig")
    match = re.search(r"kTrustedPublicKey\[\]\s*=\s*\{([^}]+)\}", source)
    if not match:
        raise ValueError("Trust-key header has no kTrustedPublicKey byte array")
    blob = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})\b", match[1]))
    if len(blob) != 276:
        raise ValueError("Trust key must be a 2048-bit CryptoAPI PUBLICKEYBLOB")
    kind, version, reserved, algorithm, magic, bits, exponent = struct.unpack("<BBHIIII", blob[:20])
    if (kind, version, reserved, algorithm, magic, bits) != (6, 2, 0, 0x2400, 0x31415352, 2048):
        raise ValueError("Unsupported trust-key header")
    modulus = int.from_bytes(blob[20:], "little")
    rsa = der(0x30, der_integer(modulus) + der_integer(exponent))
    rsa_algorithm = bytes.fromhex("300d06092a864886f70d0101010500")
    return der(0x30, rsa_algorithm + der(0x03, b"\x00" + rsa))


def read_version(source):
    lines = source.read_text(encoding="utf-8").splitlines()
    if len(lines) != 2 or not lines[0].startswith("PROJECT_VERSION="):
        raise ValueError("VERSION must contain the two production version entries")
    version = lines[0].split("=", 1)[1]
    if not re.fullmatch(r"V(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version):
        raise ValueError("VERSION must be Vmajor.minor.patch")
    if any(int(part) > 65535 for part in version[1:].split(".")):
        raise ValueError("Version components exceed the PE version range")
    if lines[1] != "CPP_VERSION=" + version:
        raise ValueError("VERSION entries disagree")
    return version


def package_hashes(root, version):
    manifest = json.loads((root / "MANIFEST.json").read_text(encoding="utf-8-sig"))
    if not isinstance(manifest, dict) or manifest.get("product") != "SerialCtl" or manifest.get("version") != version:
        raise ValueError("MANIFEST.json product/version does not match VERSION")
    verification = manifest.get("verification", {})
    if not isinstance(verification, dict) or verification.get("automated_tests") != "passed-before-packaging":
        raise ValueError("Use a bundle produced by scripts/release.ps1 after its required tests")
    records = {}
    for line in (root / "SHA256SUMS.txt").read_text(encoding="utf-8-sig").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match or match[2] in records:
            raise ValueError("Invalid or duplicate SHA256SUMS record")
        records[match[2]] = match[1]
    for name in ("MANIFEST.json", *REQUIRED_FILES):
        if name not in records or not (root / name).is_file():
            raise ValueError("Release bundle is missing required file: " + name)
    if sha256(root / "MANIFEST.json") != records["MANIFEST.json"]:
        raise ValueError("Release MANIFEST.json checksum mismatch")
    return records


def build_feed(package_dir, output_dir, private_key, trust_header, openssl="openssl"):
    root = Path(package_dir).resolve(strict=True)
    output = Path(output_dir).absolute()
    key = Path(private_key).resolve(strict=True)
    if not root.is_dir() or not key.is_file():
        raise ValueError("Package directory or signing key is invalid")
    if output.exists() or output.is_symlink():
        raise ValueError("Output directory must not exist; publish a newly generated directory")
    # A release source or web output must never accidentally contain the signing key.
    for directory in (root, output.resolve()):
        if key == directory or directory in key.parents:
            raise ValueError("Keep the signing key outside release input and web output directories")
    if root == output.resolve() or root in output.resolve().parents:
        raise ValueError("Output must be outside the extracted release directory")
    expected_public = trusted_public_der(Path(trust_header))
    actual_public = run_openssl(openssl, "pkey", "-in", key, "-pubout", "-outform", "DER")
    if actual_public != expected_public:
        raise ValueError("Signing private key does not match the public key embedded in SerialCtl")
    version = read_version(root / "VERSION")
    checksums = package_hashes(root, version)
    files = []
    total = 0
    for index, name in enumerate(ALLOWED_FILES):
        source = root / name
        if not source.exists():
            continue
        if source.is_symlink() or not source.is_file():
            raise ValueError("Update input must be a regular file: " + name)
        size = source.stat().st_size
        total += size
        if size <= 0 or size > MAX_FILE_BYTES or total > MAX_TOTAL_BYTES:
            raise ValueError("Update payload exceeds limits or contains an empty file")
        digest = sha256(source)
        if checksums.get(name) != digest:
            raise ValueError("Release checksum mismatch or missing checksum: " + name)
        target = name if name.isascii() else "asset-%03d.bin" % index
        files.append({"name": name, "size": size, "sha256": digest,
                      "url": "files/" + version + "/" + target})
    feed = {"schema": 1, "product": "SerialCtl", "architecture": "x64",
            "version": version, "files": files}
    encoded = (json.dumps(feed, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if len(encoded) > 64 * 1024:
        raise ValueError("Update manifest exceeds 64 KiB")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".serialctl-feed-", dir=output.parent) as temporary:
        staging = Path(temporary) / "feed"
        staging.mkdir()
        for record in files:
            destination = staging / record["url"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / record["name"], destination)
            if sha256(destination) != record["sha256"]:
                raise ValueError("Payload changed while copying: " + record["name"])
        manifest = staging / "update.json"
        signature = staging / "update.json.sig"
        manifest.write_bytes(encoded)
        run_openssl(openssl, "dgst", "-sha256", "-sign", key, "-out", signature, manifest)
        if signature.stat().st_size != 256:
            raise ValueError("Expected a raw 256-byte RSA-2048 signature")
        public_der = Path(temporary) / "public.der"
        public_pem = Path(temporary) / "public.pem"
        public_der.write_bytes(expected_public)
        run_openssl(openssl, "pkey", "-pubin", "-inform", "DER", "-in", public_der,
                    "-pubout", "-out", public_pem)
        run_openssl(openssl, "dgst", "-sha256", "-verify", public_pem,
                    "-signature", signature, manifest)
        staging.rename(output)
    return {"version": version, "file_count": len(files), "payload_bytes": total,
            "manifest_sha256": hashlib.sha256(encoded).hexdigest(), "output": str(output)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-dir", required=True, help="Freshly extracted verified release ZIP")
    parser.add_argument("--output-dir", required=True, help="New static website directory (must not exist)")
    parser.add_argument("--private-key", required=True, help="Offline RSA-2048 PEM signing key")
    parser.add_argument("--trust-key-header", default=str(Path(__file__).resolve().parents[1] /
                                                        "src/update/UpdateTrustKey.h"))
    parser.add_argument("--openssl", default="openssl", help="OpenSSL executable on the administrator machine")
    args = parser.parse_args()
    try:
        result = build_feed(args.package_dir, args.output_dir, args.private_key,
                            args.trust_key_header, args.openssl)
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, "Update feed rejected: " + str(error) + "\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
