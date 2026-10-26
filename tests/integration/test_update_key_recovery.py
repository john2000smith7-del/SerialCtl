"""Encrypted-backup recovery with temporary test identities, never production secrets."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_new_update_feed import make_key

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from recover_update_key import active_backup, restore_key
from new_update_feed import trusted_public_der


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="serialctl-recovery-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.openssl = shutil.which("openssl") or "C:/Program Files/Git/usr/bin/openssl.exe"
        self.private, self.header = make_key(self.root, "test-key", self.openssl)
        self.password = self.root / "test-password.txt"
        self.password.write_text("ephemeral-test-password-only\n")
        self.encrypted = self.root / "test-encrypted.pem"
        subprocess.run([self.openssl, "pkcs8", "-topk8", "-v2", "aes-256-cbc", "-iter", "600000",
                        "-in", str(self.private), "-out", str(self.encrypted),
                        "-passout", "file:" + str(self.password)], check=True,
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        self.output = self.root / "restored.pem"

    def recover(self, **overrides):
        options = dict(password_file=self.password, trust_header=self.header, openssl=self.openssl)
        options.update(overrides)
        return restore_key(self.encrypted, self.output, **options)

    def test_round_trip_and_no_overwrite(self):
        self.recover()
        actual = subprocess.check_output([self.openssl, "pkey", "-in", str(self.output), "-pubout", "-outform", "DER"],
                                         stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.assertEqual(actual, trusted_public_der(self.header))
        before = self.output.read_bytes()
        with self.assertRaises(ValueError):
            self.recover()
        self.assertEqual(self.output.read_bytes(), before)
        if os.name != "nt":
            self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)

    def test_wrong_password_leaves_no_key(self):
        self.password.write_text("incorrect-test-password\n")
        with self.assertRaises(ValueError):
            self.recover()
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob(".serialctl-key-restore-*")))

    def test_wrong_identity_leaves_no_key(self):
        _, other_header = make_key(self.root, "other-test-key", self.openssl)
        with self.assertRaises(ValueError):
            self.recover(trust_header=other_header)
        self.assertFalse(self.output.exists())

    def test_plaintext_input_rejected(self):
        self.encrypted = self.private
        with self.assertRaises(ValueError):
            self.recover()
        self.assertFalse(self.output.exists())

    def test_source_output_rejected(self):
        self.output = ROOT / "must-never-contain-a-decrypted-test-key.pem"
        with self.assertRaises(ValueError):
            self.recover()
        self.assertFalse(self.output.exists())

    def test_active_backup_selection_checks_identity_and_digest(self):
        receipt = {"spki_sha256": hashlib.sha256(trusted_public_der(self.header)).hexdigest(),
                   "encrypted_backup": self.encrypted.name,
                   "encrypted_backup_sha256": hashlib.sha256(self.encrypted.read_bytes()).hexdigest()}
        identity = self.root / "identity.json"
        identity.write_text(json.dumps(receipt))
        self.assertEqual(active_backup(identity, self.header), self.encrypted)
        receipt["encrypted_backup_sha256"] = "0" * 64
        identity.write_text(json.dumps(receipt))
        with self.assertRaises(ValueError):
            active_backup(identity, self.header)
        receipt["encrypted_backup"] = None
        identity.write_text(json.dumps(receipt))
        with self.assertRaises(ValueError):
            active_backup(identity, self.header)


class OriginalIdentityTests(unittest.TestCase):
    def test_original_identity_is_active_and_historical_backup_is_retained(self):
        receipt = json.loads((ROOT / "release-signing/identity.json").read_text())
        public = hashlib.sha256(trusted_public_der(ROOT / "src/update/UpdateTrustKey.h")).hexdigest()
        self.assertEqual(public, "196b703ac1274da483936513794f0c03eb7be806c302db4d932b30378a78a21a")
        self.assertEqual(public, receipt["spki_sha256"])
        self.assertIsNone(receipt["encrypted_backup"])
        with self.assertRaises(ValueError):
            active_backup(ROOT / "release-signing/identity.json", ROOT / "src/update/UpdateTrustKey.h")
        historical = json.loads((ROOT / "release-signing" / receipt["historical_identity"]).read_text())
        encrypted = (ROOT / "release-signing" / historical["encrypted_backup"]).read_bytes()
        self.assertTrue(encrypted.startswith(b"-----BEGIN ENCRYPTED PRIVATE KEY-----"))
        self.assertEqual(hashlib.sha256(encrypted).hexdigest(), historical["encrypted_backup_sha256"])
        self.assertNotEqual(public, historical["spki_sha256"])

    def test_original_v202_release_signature_is_accepted(self):
        # Public release fixture only; no production signing key is read or used.
        raw = (ROOT / "tests/fixtures/update-v2.0.2-r2.json").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),
                         "7a0ddcd00efc572bfc67ee836a04831bd04604f280ec8555ac63f9cdea1934d4")
        envelope = json.loads(raw)
        self.assertEqual(json.loads(envelope["payload"])["version"], "V2.0.2")
        openssl = shutil.which("openssl") or "C:/Program Files/Git/usr/bin/openssl.exe"
        with tempfile.TemporaryDirectory(prefix="serialctl-original-identity-test-") as temporary:
            p = Path(temporary)
            (p / "public.der").write_bytes(trusted_public_der(ROOT / "src/update/UpdateTrustKey.h"))
            (p / "payload.json").write_bytes(envelope["payload"].encode("utf-8"))
            (p / "signature.bin").write_bytes(bytes.fromhex(envelope["signature"]))
            subprocess.run([openssl, "pkey", "-pubin", "-inform", "DER", "-in", str(p / "public.der"),
                            "-out", str(p / "public.pem")], check=True, capture_output=True)
            result = subprocess.run([openssl, "dgst", "-sha256", "-verify", str(p / "public.pem"),
                                     "-signature", str(p / "signature.bin"), str(p / "payload.json")],
                                    capture_output=True)
            self.assertEqual(result.returncode, 0, "Original V2.0.2 identity is no longer trusted")


if __name__ == "__main__":
    unittest.main(verbosity=2)
