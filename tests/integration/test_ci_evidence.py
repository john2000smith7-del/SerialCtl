"""Transport integrity checks, not Windows or release acceptance."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ci_evidence", ROOT / "tools/ci_evidence.py")
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.meta = evidence.identity("123", "a" * 40, "SerialCtl-Win7-portable", 2)
        self.source = self.root / "input"
        self.source.mkdir()
        self.original = bytes(range(256)) * 99
        (self.source / "program.zip").write_bytes(self.original)
        self.archive = self.root / "transport.zip"
        self.manifest = evidence.pack(self.archive, self.meta, str(self.source / "*.zip"))
        self.destination = self.root / "output"

    def rewrite(self, mutate, data=None):
        manifest = copy.deepcopy(self.manifest)
        mutate(manifest)
        with zipfile.ZipFile(self.archive, "w") as out:
            out.writestr("manifest.json", json.dumps(manifest))
            out.writestr("files/" + manifest["files"][0]["name"], self.original if data is None else data)

    def test_exact_original_zip_restores_without_repacking(self):
        evidence.unpack(self.archive, self.destination, self.meta)
        self.assertEqual((self.destination / "program.zip").read_bytes(), self.original)
        evidence.unpack(self.archive, self.destination, self.meta)  # identical input is idempotent

    def test_source_run_name_and_attempt_cannot_be_substituted(self):
        for field, value in (("source_sha", "b" * 40), ("run_id", "124"),
                             ("name", "other"), ("attempt", 3)):
            with self.subTest(field=field):
                other = dict(self.meta, **{field: value})
                with self.assertRaises(ValueError):
                    evidence.unpack(self.archive, self.destination, other)

    def test_hash_failure_publishes_no_files(self):
        self.rewrite(lambda m: None, b"x" * len(self.original))
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            evidence.unpack(self.archive, self.destination, self.meta)
        self.assertFalse((self.destination / "program.zip").exists())

    def test_zip_slip_and_windows_path_tricks_reject(self):
        for name in ("../escape", "/escape", "C:/escape", "dir\\escape", "a/../escape", "a//escape"):
            with self.subTest(name=name):
                self.rewrite(lambda m: m["files"][0].update(name=name))
                with self.assertRaises(ValueError):
                    evidence.unpack(self.archive, self.destination, self.meta)

    def test_existing_different_file_not_overwritten(self):
        self.destination.mkdir()
        target = self.destination / "program.zip"
        target.write_bytes(b"existing")
        with self.assertRaises(ValueError):
            evidence.unpack(self.archive, self.destination, self.meta)
        self.assertEqual(target.read_bytes(), b"existing")

    def test_unlisted_extra_member_rejects(self):
        with zipfile.ZipFile(self.archive, "a") as out:
            out.writestr("files/unlisted", "other")
        with self.assertRaises(ValueError):
            evidence.unpack(self.archive, self.destination, self.meta)

    def test_common_root_preserves_source_subdirectories(self):
        second = self.source / "ui"
        second.mkdir()
        (second / "capture.png").write_bytes(b"real-capture-fixture")
        files = evidence.select_files(str(second / "*.png") + "\n" + str(self.source / "program.zip"))
        self.assertEqual({name for _, name in files}, {"ui/capture.png", "program.zip"})

    def test_resolver_binds_current_run_sha_ref_and_latest_existing_attempt(self):
        run = {"head_repository": {"full_name": "owner/repo"}, "head_sha": "a" * 40,
               "head_branch": "main", "event": "push", "run_attempt": 3}
        def api(repo, endpoint):
            if endpoint == "actions/runs/123":
                return run
            self.assertIn("ref=refs%2Fheads%2Fmain", endpoint)
            self.assertIn(evidence.prefix(self.meta), endpoint)
            return {"actions_caches": [{"key": evidence.key(dict(self.meta, attempt=a))}
                                      for a in (1, 2, 9)]}
        with mock.patch.object(evidence, "gh", side_effect=api):
            self.assertEqual(evidence.resolve("owner/repo", "123", self.meta["name"]), self.meta)
            run["event"] = "pull_request"
            with self.assertRaises(ValueError):
                evidence.resolve("owner/repo", "123", self.meta["name"])

    def test_evicted_cache_fails_instead_of_using_another_build(self):
        with mock.patch.object(evidence, "gh", side_effect=[
            {"head_repository": {"full_name": "owner/repo"}, "head_sha": "a"*40,
             "head_branch": "main", "event": "push", "run_attempt": 2},
            {"actions_caches": []},
        ]):
            with self.assertRaisesRegex(ValueError, "rebuild required"):
                evidence.resolve("owner/repo", "123", self.meta["name"])

    def test_github_utf8_json_does_not_use_windows_legacy_console_codec(self):
        result = subprocess.CompletedProcess(["gh"], 0, stdout='{"title":"中文提交"}'.encode("utf-8"))
        with mock.patch.object(evidence.subprocess, "run", return_value=result) as run:
            self.assertEqual(evidence.gh("owner/repo", "actions/runs/123")["title"], "中文提交")
            self.assertFalse(run.call_args.kwargs.get("text", False))


if __name__ == "__main__":
    unittest.main()
