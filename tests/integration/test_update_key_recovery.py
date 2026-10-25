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
from recover_update_key import restore_key
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

    def test_versioned_backup_matches_public_identity(self):
        receipt = json.loads((ROOT / "release-signing/identity.json").read_text())
        encrypted = (ROOT / "release-signing" / receipt["encrypted_backup"]).read_bytes()
        self.assertTrue(encrypted.startswith(b"-----BEGIN ENCRYPTED PRIVATE KEY-----"))
        self.assertEqual(hashlib.sha256(encrypted).hexdigest(), receipt["encrypted_backup_sha256"])
        self.assertEqual(hashlib.sha256(trusted_public_der(ROOT / "src/update/UpdateTrustKey.h")).hexdigest(),
                         receipt["spki_sha256"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
