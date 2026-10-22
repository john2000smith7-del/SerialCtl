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
import zipfile
import base64


ALLOWED_FILES = (
    "serialctl.exe", "plink.exe", "psftp.exe", "VERSION", "README.md",
    "RELEASE-NOTES.md", "AUTO-UPDATE.md", "AI-API.md", "AI-SERIAL.md",
    "WEBSOCKET-PROTOCOL.md", "POWER.md", "THIRD-PARTY-NOTICES.md",
    "JSON-LICENSE.txt", "MINIZ-LICENSE.txt", "PUTTY-LICENCE.txt", "YY-THUNKS-LICENSE.txt",
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


def describe_archive(archive_path):
    archive = Path(archive_path).resolve(strict=True)
    if not 0 < archive.stat().st_size <= 512 * 1024 * 1024:
        raise ValueError("Archive exceeds 512 MiB")
    original_digest = sha256(archive)
    with zipfile.ZipFile(archive) as bundle:
        entries = {}
        for item in bundle.infolist():
            name = item.filename.replace(chr(92), "/")
            parts = name.rstrip("/").split("/")
            if (not name or len(name) > 1024 or name.startswith("/") or ":" in name or
                    any(c < " " or ord(c) == 127 for c in name) or
                    any(part in ("", ".", "..") or part.endswith((".", " ")) for part in parts) or
                    name in entries or item.flag_bits & 1 or
                    item.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) or
                    (item.external_attr >> 16) & 0o170000 == 0o120000):
                raise ValueError("Unsafe or duplicate ZIP entry")
            entries[name] = item
        if len(entries) > 4096:
            raise ValueError("Too many ZIP entries")
        def read(name, maximum):
            item = entries[name]
            if not 0 < item.file_size <= maximum:
                raise ValueError("ZIP member exceeds size limit: " + name)
            return bundle.read(item)
        version_bytes = read("VERSION", 256)
        version_text = version_bytes.decode("utf-8").replace("\r", "").splitlines()
        if len(version_text) != 2 or not version_text[0].startswith("PROJECT_VERSION="):
            raise ValueError("Invalid archive VERSION")
        version = version_text[0].split("=", 1)[1]
        if (not re.fullmatch(r"V(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version) or
                any(int(part) > 65535 for part in version[1:].split(".")) or
                version_text[1] != "CPP_VERSION=" + version):
            raise ValueError("Invalid archive version entries")
        if archive.name != "SerialCtl-" + version + "-Win7-x64.zip":
            raise ValueError("ZIP filename must match its production VERSION")
        manifest_bytes = read("MANIFEST.json", 64 * 1024)
        manifest = json.loads(manifest_bytes.decode("utf-8-sig"))
        if (manifest.get("product") != "SerialCtl" or manifest.get("version") != version or
                manifest.get("verification", {}).get("automated_tests") != "passed-before-packaging"):
            raise ValueError("Archive must be a verified production release")
        checksums = {}
        for line in read("SHA256SUMS.txt", 256 * 1024).decode("utf-8-sig").splitlines():
            match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
            if not match or match[2] in checksums:
                raise ValueError("Invalid or duplicate release checksum")
            checksums[match[2]] = match[1]
        if hashlib.sha256(manifest_bytes).hexdigest() != checksums.get("MANIFEST.json"):
            raise ValueError("Release manifest checksum mismatch")
        files = []
        total = 0
        for index, name in enumerate(ALLOWED_FILES):
            if name not in entries:
                if name in REQUIRED_FILES:
                    raise ValueError("Missing required runtime: " + name)
                continue
            data = read(name, MAX_FILE_BYTES)
            total += len(data)
            digest = hashlib.sha256(data).hexdigest()
            if total > MAX_TOTAL_BYTES or checksums.get(name) != digest:
                raise ValueError("Runtime size/checksum mismatch: " + name)
            files.append({"name": name, "size": len(data), "sha256": digest, "url": "asset-%03d.bin" % index})
    digest = sha256(archive)
    if digest != original_digest:
        raise ValueError("Archive changed while reading its release metadata")
    return {"schema": 2, "product": "SerialCtl", "architecture": "x64", "version": version,
                          "files": files, "archive": {"url": archive.name, "size": archive.stat().st_size,
                                                      "sha256": digest}}


def build_archive_feed(archive_path, output_dir, private_key, trust_header, openssl="openssl"):
    """Publish exactly the verified full ZIP and its embedded-signature JSON."""
    archive = Path(archive_path).resolve(strict=True)
    output = Path(output_dir).absolute()
    key = Path(private_key).resolve(strict=True)
    if output.exists() or output.is_symlink():
        raise ValueError("Output directory must not exist")
    if output.resolve() in key.parents or output.resolve() == key:
        raise ValueError("Keep signing key outside the public output")
    if not 0 < archive.stat().st_size <= 512 * 1024 * 1024:
        raise ValueError("Archive exceeds 512 MiB")
    expected_public = trusted_public_der(Path(trust_header))
    if run_openssl(openssl, "pkey", "-in", key, "-pubout", "-outform", "DER") != expected_public:
        raise ValueError("Signing private key does not match the public key embedded in SerialCtl")
    descriptor = describe_archive(archive)
    version = descriptor["version"]
    files = descriptor["files"]
    digest = descriptor["archive"]["sha256"]
    payload = json.dumps(descriptor, ensure_ascii=False, separators=(",", ":"))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".serialctl-two-file-", dir=output.parent) as temporary:
        temporary = Path(temporary)
        staging = temporary / "publish"
        staging.mkdir()
        target = staging / archive.name
        shutil.copyfile(archive, target)
        if sha256(target) != digest:
            raise ValueError("ZIP changed during publication")
        raw = temporary / "payload.json"
        raw.write_bytes(payload.encode("utf-8"))
        signature = temporary / "signature.bin"
        run_openssl(openssl, "dgst", "-sha256", "-sign", key, "-out", signature, raw)
        signed = signature.read_bytes()
        if len(signed) != 256:
            raise ValueError("Expected RSA-2048 signature")
        public_der = temporary / "public.der"
        public_der.write_bytes(expected_public)
        public_pem = temporary / "public.pem"
        run_openssl(openssl, "pkey", "-pubin", "-inform", "DER", "-in", public_der, "-out", public_pem)
        run_openssl(openssl, "dgst", "-sha256", "-verify", public_pem, "-signature", signature, raw)
        envelope = {"schema": 2, "payload": payload, "signature": signed.hex()}
        encoded = (json.dumps(envelope, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        if len(encoded) > 64 * 1024:
            raise ValueError("Signed metadata exceeds 64 KiB")
        (staging / "update.json").write_bytes(encoded)
        staging.rename(output)
    return {"version": version, "archive_sha256": digest, "archive_bytes": archive.stat().st_size,
            "file_count": len(files), "output": str(output)}


def verify_archive_feed(metadata_path, archive_path, trust_header, openssl="openssl"):
    encoded = Path(metadata_path).read_bytes()
    if not 0 < len(encoded) <= 64 * 1024:
        raise ValueError("Invalid signed metadata size")
    def unique(pairs):
        result = {}
        for name, value in pairs:
            if name in result:
                raise ValueError("Duplicate metadata key")
            result[name] = value
        return result
    envelope = json.loads(encoded.decode("utf-8"), object_pairs_hook=unique)
    if (not isinstance(envelope, dict) or set(envelope) != {"schema", "payload", "signature"} or
            envelope["schema"] != 2 or not isinstance(envelope["payload"], str) or
            not isinstance(envelope["signature"], str) or
            not re.fullmatch(r"[0-9a-f]{512}", envelope["signature"])):
        raise ValueError("Invalid signed update envelope")
    with tempfile.TemporaryDirectory(prefix="serialctl-verify-feed-") as temporary:
        temporary = Path(temporary)
        public_der = temporary / "public.der"
        public_der.write_bytes(trusted_public_der(Path(trust_header)))
        public_pem = temporary / "public.pem"
        run_openssl(openssl, "pkey", "-pubin", "-inform", "DER", "-in", public_der, "-out", public_pem)
        raw = temporary / "payload.json"
        raw.write_bytes(envelope["payload"].encode("utf-8"))
        signature = temporary / "signature.bin"
        signature.write_bytes(bytes.fromhex(envelope["signature"]))
        run_openssl(openssl, "dgst", "-sha256", "-verify", public_pem, "-signature", signature, raw)
    payload = json.loads(envelope["payload"], object_pairs_hook=unique)
    expected = describe_archive(archive_path)
    if payload != expected:
        raise ValueError("Signed metadata does not exactly describe the verified release archive")
    return {"version": payload["version"], "archive_sha256": payload["archive"]["sha256"], "verified": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--describe-archive", help="Emit public candidate metadata for offline signing; not a deployment file")
    group.add_argument("--verify-archive", help="Verify embedded signature and its exact matching full ZIP")
    parser.add_argument("--metadata", help="Signed update.json used with --verify-archive")
    group.add_argument("--archive", help="Verified full release ZIP; generates exactly two deployable files")
    group.add_argument("--package-dir", help="Legacy per-file feed from an extracted release directory")
    parser.add_argument("--output-dir", help="New static website directory (must not exist)")
    parser.add_argument("--private-key", help="Offline RSA-2048 PEM signing key")
    parser.add_argument("--trust-key-header", default=str(Path(__file__).resolve().parents[1] /
                                                        "src/update/UpdateTrustKey.h"))
    parser.add_argument("--openssl", default="openssl", help="OpenSSL executable on the administrator machine")
    args = parser.parse_args()
    try:
        if args.describe_archive:
            payload = json.dumps(describe_archive(args.describe_archive), ensure_ascii=False, separators=(",", ":"))
            print("SERIALCTL_UPDATE_PAYLOAD_BASE64=" + base64.b64encode(payload.encode("utf-8")).decode("ascii"))
            return
        if args.verify_archive:
            if not args.metadata:
                raise ValueError("--verify-archive requires --metadata")
            result = verify_archive_feed(args.metadata, args.verify_archive, args.trust_key_header, args.openssl)
        else:
            if not args.output_dir or not args.private_key:
                raise ValueError("Publication requires --output-dir and --private-key")
            build = build_archive_feed if args.archive else build_feed
            result = build(args.archive or args.package_dir, args.output_dir, args.private_key,
                           args.trust_key_header, args.openssl)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.TimeoutExpired) as error:
        parser.exit(1, "Update feed rejected: " + str(error) + "\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
