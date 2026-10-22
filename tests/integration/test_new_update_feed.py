#!/usr/bin/env python3
"""Administrator tool tests with temporary keys and controlled package bytes.

These do not execute a Windows updater, verify PE compatibility, or use the
production signing key. OpenSSL is required; test keys are deleted on exit.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zipfile

REPOSITORY = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("new_update_feed", REPOSITORY / "tools/new_update_feed.py")
feed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(feed)


def make_key(directory, name, openssl):
    private = directory / (name + ".pem")
    subprocess.run([openssl, "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                    "-out", str(private)], check=True, stdin=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    modulus_text = subprocess.check_output([openssl, "rsa", "-in", str(private), "-noout", "-modulus"],
                                           stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    modulus = int(modulus_text.strip().split(b"=", 1)[1], 16)
    blob = struct.pack("<BBHIIII", 6, 2, 0, 0x2400, 0x31415352, 2048, 65537)
    blob += modulus.to_bytes(256, "little")
    header = directory / (name + ".h")
    header.write_text("inline constexpr unsigned char kTrustedPublicKey[] = {" +
                      ",".join("0x%02x" % byte for byte in blob) + "};\n", encoding="ascii")
    return private, header


class FeedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.openssl = shutil.which("openssl")
        if not cls.openssl and os.name == "nt":
            candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Git/usr/bin/openssl.exe"
            if candidate.is_file():
                cls.openssl = str(candidate)
        if not cls.openssl:
            raise RuntimeError("OpenSSL is required for update-feed tests")
        cls.keys = tempfile.TemporaryDirectory(prefix="serialctl-feed-test-keys-")
        cls.private, cls.header = make_key(Path(cls.keys.name), "test", cls.openssl)
        cls.wrong_private, _ = make_key(Path(cls.keys.name), "wrong", cls.openssl)

    @classmethod
    def tearDownClass(cls):
        cls.keys.cleanup()

    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="serialctl-feed-test-")
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.package = self.root / "package"
        self.package.mkdir()
        self.output = self.root / "published"
        for name in feed.REQUIRED_FILES[:3]:
            (self.package / name).write_bytes(b"Controlled fixture; not a Windows executable: " + name.encode("ascii"))
        (self.package / "VERSION").write_bytes(b"PROJECT_VERSION=V1.0.8\nCPP_VERSION=V1.0.8\n")
        (self.package / "README.md").write_text("Readme fixture\n", encoding="utf-8")
        (self.package / "常用命令示例.txt").write_text("echo test\n", encoding="utf-8")
        (self.package / "update.ini").write_text("[update]\nurl=http://private.example/update.json\n")
        (self.package / "unapproved.exe").write_bytes(b"must not publish")
        (self.package / "drivers").mkdir()
        (self.package / "drivers/device.inf").write_bytes(b"must not publish")
        self.write_metadata()

    def write_metadata(self, manifest=None):
        if manifest is None:
            manifest = {"product": "SerialCtl", "version": "V1.0.8",
                        "verification": {"automated_tests": "passed-before-packaging"}}
        (self.package / "MANIFEST.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.rehash()

    def rehash(self):
        records = []
        for path in sorted(self.package.rglob("*")):
            if path.is_file() and path.name != "SHA256SUMS.txt":
                records.append(feed.sha256(path) + "  " + path.relative_to(self.package).as_posix())
        (self.package / "SHA256SUMS.txt").write_text("\n".join(records) + "\n", encoding="utf-8")

    def build(self, **changes):
        arguments = dict(package_dir=self.package, output_dir=self.output,
                         private_key=self.private, trust_header=self.header, openssl=self.openssl)
        arguments.update(changes)
        return feed.build_feed(**arguments)

    def reject(self, **changes):
        with self.assertRaises(ValueError):
            self.build(**changes)
        self.assertFalse(self.output.exists(), "Rejected feed left a publishable directory")

    def test_signed_round_trip_and_allowlist(self):
        result = self.build()
        manifest_path = self.output / "update.json"
        data = manifest_path.read_bytes()
        manifest = json.loads(data)
        self.assertEqual(result["manifest_sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(manifest["version"], "V1.0.8")
        self.assertEqual((self.output / "update.json.sig").stat().st_size, 256)
        public = self.root / "public.pem"
        subprocess.run([self.openssl, "pkey", "-in", str(self.private), "-pubout", "-out", str(public)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([self.openssl, "dgst", "-sha256", "-verify", str(public), "-signature",
                        str(self.output / "update.json.sig"), str(manifest_path)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        actual = {p.relative_to(self.output).as_posix() for p in self.output.rglob("*") if p.is_file()}
        expected = {"update.json", "update.json.sig"}
        for record in manifest["files"]:
            expected.add(record["url"])
            self.assertTrue(record["url"].isascii())
            payload = self.output / record["url"]
            self.assertEqual(payload.read_bytes(), (self.package / record["name"]).read_bytes())
            self.assertEqual(feed.sha256(payload), record["sha256"])
            self.assertEqual(payload.stat().st_size, record["size"])
        self.assertEqual(actual, expected)
        self.assertNotIn("update.ini", {record["name"] for record in manifest["files"]})
        self.assertNotIn("MANIFEST.json", {record["name"] for record in manifest["files"]})
        self.assertFalse(any(name.endswith(".pem") or "drivers" in name for name in actual))

    def test_allowlist_matches_client(self):
        source = (REPOSITORY / "src/update/UpdateManifest.cpp").read_text(encoding="utf-8")
        array = source.split("static const std::set<std::string> allowed = {", 1)[1].split("};", 1)[0]
        names = re.findall(r'(?:u8)?"([^"]+)"', array)
        self.assertEqual(set(names), set(feed.ALLOWED_FILES))

    def test_wrong_signing_key(self):
        self.reject(private_key=self.wrong_private)

    def test_tampered_payload(self):
        (self.package / "serialctl.exe").write_bytes(b"altered")
        self.reject()

    def test_tampered_manifest(self):
        with (self.package / "MANIFEST.json").open("a") as stream:
            stream.write(" ")
        self.reject()

    def test_required_file_missing(self):
        (self.package / "psftp.exe").unlink()
        self.rehash()
        self.reject()

    def test_wrong_product_and_malformed_manifest(self):
        for value in ([], {"product": "Other", "version": "V1.0.8"},
                      {"product": "SerialCtl", "version": "V1.0.8", "verification": []}):
            with self.subTest(value=value):
                self.write_metadata(value)
                self.reject()

    def test_unverified_package(self):
        self.write_metadata({"product": "SerialCtl", "version": "V1.0.8",
                             "verification": {"automated_tests": "not-run"}})
        self.reject()

    def test_existing_output_preserved(self):
        self.output.mkdir()
        marker = self.output / "existing.txt"
        marker.write_text("keep")
        with self.assertRaises(ValueError):
            self.build()
        self.assertEqual(marker.read_text(), "keep")

    def test_output_inside_package(self):
        self.reject(output_dir=self.package / "web")

    def test_key_inside_output_or_input(self):
        key_copy = self.package / "private.pem"
        shutil.copyfile(self.private, key_copy)
        self.reject(private_key=key_copy)
        with self.assertRaises(ValueError):
            self.build(output_dir=self.private.parent)

    def test_bom_and_noncanonical_versions(self):
        for value in (b"\xef\xbb\xbfPROJECT_VERSION=V1.0.8\nCPP_VERSION=V1.0.8\n",
                      b"PROJECT_VERSION=V01.0.8\nCPP_VERSION=V01.0.8\n",
                      b"PROJECT_VERSION=V1.0.8\nCPP_VERSION=V1.0.9\n"):
            with self.subTest(value=value):
                (self.package / "VERSION").write_bytes(value)
                self.rehash()
                self.reject()

    def test_empty_payload(self):
        (self.package / "README.md").write_bytes(b"")
        self.rehash()
        self.reject()

    def test_oversized_payload(self):
        with (self.package / "serialctl.exe").open("wb") as stream:
            stream.truncate(feed.MAX_FILE_BYTES + 1)
        self.reject()

    def test_duplicate_checksum(self):
        checksum = self.package / "SHA256SUMS.txt"
        text = checksum.read_text()
        checksum.write_text(text + text.splitlines()[0] + "\n")
        self.reject()

    def test_crlf_version(self):
        (self.package / "VERSION").write_bytes(b"PROJECT_VERSION=V1.0.8\r\nCPP_VERSION=V1.0.8\r\n")
        self.rehash()
        self.assertEqual(self.build()["version"], "V1.0.8")


    def make_archive(self):
        archive = self.root / "SerialCtl-V1.0.8-Win7-x64.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
            for path in sorted(self.package.rglob("*")):
                if path.is_file():
                    output.write(path, path.relative_to(self.package).as_posix())
        return archive

    def archive_feed(self, archive):
        return feed.build_archive_feed(archive, self.output, self.private, self.header, self.openssl)

    def test_two_file_archive_round_trip(self):
        archive = self.make_archive()
        result = self.archive_feed(archive)
        self.assertEqual({p.name for p in self.output.iterdir()}, {archive.name, "update.json"})
        self.assertEqual(feed.sha256(archive), feed.sha256(self.output / archive.name))
        self.assertTrue(feed.verify_archive_feed(self.output / "update.json", self.output / archive.name,
                                                self.header, self.openssl)["verified"])
        envelope = json.loads((self.output / "update.json").read_text())
        payload = json.loads(envelope["payload"])
        self.assertEqual(payload["archive"]["sha256"], result["archive_sha256"])
        self.assertEqual(len(envelope["signature"]), 512)
        self.assertNotIn("update.ini", {row["name"] for row in payload["files"]})

    def test_archive_wrong_key(self):
        with self.assertRaises(ValueError):
            feed.build_archive_feed(self.make_archive(), self.output, self.wrong_private, self.header, self.openssl)
        self.assertFalse(self.output.exists())

    def test_archive_checksum_mismatch(self):
        (self.package / "README.md").write_bytes(b"modified after verification")
        with self.assertRaises(ValueError):
            self.archive_feed(self.make_archive())
        self.assertFalse(self.output.exists())

    def test_archive_traversal_and_duplicate_names(self):
        for name in ("../escape", "drivers\\device.inf"):
            archive = self.make_archive()
            with zipfile.ZipFile(archive, "a") as output:
                output.writestr(name, b"untrusted")
            with self.assertRaises(ValueError):
                self.archive_feed(archive)
            self.assertFalse(self.output.exists())

    def test_archive_symlink(self):
        archive = self.make_archive()
        with zipfile.ZipFile(archive, "a") as output:
            entry = zipfile.ZipInfo("drivers/link")
            entry.create_system = 3
            entry.external_attr = 0o120777 << 16
            output.writestr(entry, "../../target")
        with self.assertRaises(ValueError):
            self.archive_feed(archive)

    def test_embedded_signature_tamper(self):
        archive = self.make_archive()
        self.archive_feed(archive)
        metadata = self.output / "update.json"
        envelope = json.loads(metadata.read_text())
        envelope["payload"] += " "
        metadata.write_text(json.dumps(envelope))
        with self.assertRaises(ValueError):
            feed.verify_archive_feed(metadata, archive, self.header, self.openssl)

    def test_archive_substitution(self):
        archive = self.make_archive()
        self.archive_feed(archive)
        with zipfile.ZipFile(archive, "a") as output:
            output.writestr("extra.txt", "archive differs")
        with self.assertRaises(ValueError):
            feed.verify_archive_feed(self.output / "update.json", archive, self.header, self.openssl)

    def test_archive_requires_verified_release(self):
        self.write_metadata({"product": "SerialCtl", "version": "V1.0.8", "verification": {}})
        with self.assertRaises(ValueError):
            self.archive_feed(self.make_archive())


if __name__ == "__main__":
    unittest.main(verbosity=2)
