#!/usr/bin/env python3
"""Restore an owner-held update identity; never generate or replace a key."""
import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from new_update_feed import trusted_public_der

REPOSITORY = Path(__file__).resolve().parents[1]


def active_backup(identity, trust_header):
    """Choose only a backup explicitly bound to the current client identity."""
    identity = Path(identity)
    receipt = json.loads(identity.read_text(encoding="utf-8"))
    expected = hashlib.sha256(trusted_public_der(Path(trust_header))).hexdigest()
    if receipt.get("spki_sha256") != expected:
        raise ValueError("Backup identity does not match the current client")
    name = receipt.get("encrypted_backup")
    if not isinstance(name, str) or not name or Path(name).name != name:
        raise ValueError("Active identity has no encrypted repository backup; use the recovered original private key")
    backup = identity.parent / name
    if hashlib.sha256(backup.read_bytes()).hexdigest() != receipt.get("encrypted_backup_sha256"):
        raise ValueError("Encrypted backup digest mismatch")
    return backup


def restore_key(encrypted, output, *, password_file=None, password=None,
                trust_header=None, openssl="openssl"):
    encrypted, output = Path(encrypted).resolve(strict=True), Path(output).absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Refusing to overwrite an existing key")
    output = output.parent.resolve() / output.name
    if output == REPOSITORY or REPOSITORY in output.parents:
        raise ValueError("Keep the decrypted signing key outside the repository")
    if not encrypted.is_file() or not 0 < encrypted.stat().st_size <= 32768:
        raise ValueError("Invalid encrypted recovery file")
    if not encrypted.read_bytes().startswith(b"-----BEGIN ENCRYPTED PRIVATE KEY-----"):
        raise ValueError("Recovery input must be an encrypted PKCS#8 key")
    expected = trusted_public_der(Path(trust_header or REPOSITORY / "src/update/UpdateTrustKey.h"))
    output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".serialctl-key-restore-", dir=output.parent) as temporary:
        key = Path(temporary) / "identity.pem"
        # Create the file with private permissions before OpenSSL opens it.
        key.touch(mode=0o600)
        arguments = [openssl, "pkey", "-in", str(encrypted), "-out", str(key), "-passin"]
        if password_file is not None:
            arguments.append("file:" + str(Path(password_file).resolve(strict=True)))
            secret_input = None
        elif password is not None:
            arguments.append("stdin")
            secret_input = (password + "\n").encode("utf-8")
        else:
            raise ValueError("A private recovery password is required")
        result = subprocess.run(arguments, input=secret_input, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=30)
        if result.returncode:
            raise ValueError("Recovery failed; check the private password and encrypted backup")
        result = subprocess.run([openssl, "pkey", "-in", str(key), "-pubout", "-outform", "DER"],
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        if result.returncode or result.stdout != expected:
            raise ValueError("Recovered identity does not match the client's trusted public key")
        # Same-filesystem link is atomic and refuses replacement if a key
        # appears between the initial check and this final commit.
        os.link(key, output)
        return hashlib.sha256(expected).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--encrypted", type=Path,
                        help="Owner-held encrypted copy of the active identity; historical identities are not selected automatically")
    parser.add_argument("--output", type=Path, required=True, help="Private key path outside the repository")
    parser.add_argument("--password-file", type=Path, help="Owner-held private recovery password file")
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args()
    try:
        encrypted = args.encrypted or active_backup(REPOSITORY / "release-signing/identity.json",
                                                    REPOSITORY / "src/update/UpdateTrustKey.h")
        password = None if args.password_file else getpass.getpass("Private recovery password: ")
        digest = restore_key(encrypted, args.output, password_file=args.password_file,
                             password=password, openssl=args.openssl)
        print("Recovered existing update identity; public SPKI SHA-256: " + digest)
    except (OSError, ValueError, subprocess.SubprocessError):
        parser.exit(1, "Recovery failed; key not replaced. Check the active identity and matching owner-held encrypted backup/password. Historical backups cannot restore a different identity.\n")


if __name__ == "__main__":
    main()
