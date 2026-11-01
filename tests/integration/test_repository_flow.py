"""Release evidence/policy regressions; no publishing, production keys or Windows claims."""
import copy
import hashlib
import importlib.util
import json
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile


ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


gate = module("release_gate", "tools/release_gate.py")
policy = module("repository_policy", "tools/repository_policy.py")
upgrade = module("verify_release_upgrade", "tests/integration/verify_release_upgrade.py")
settings = module("configure_repository", "tools/configure_repository.py")


class ReleaseGateTests(unittest.TestCase):
    def setUp(self):
        self.sha = "1" * 40
        self.archive = "2" * 64
        self.request = {"version": "V2.0.19", "source_commit": self.sha, "archive_sha256": self.archive,
                        "build_run_id": "101", "desktop_run_id": "102", "upgrade_run_id": "103"}
        self.metadata = json.dumps({"schema": 2, "payload": json.dumps(
            {"version": "V2.0.19", "archive": {"sha256": self.archive}}),
            "signature": "controlled-public-envelope-fixture"}).encode()
        self.runs = {}
        for kind, filename, jobs in (("build", "windows-build.yml", gate.WINDOWS_JOBS),
                                     ("desktop", "desktop-interaction.yml", {"desktop"}),
                                     ("upgrade", "verify-release-upgrade.yml", {"upgrade"})):
            self.runs[kind] = {"id": int(self.request[kind + "_run_id"]), "status": "completed",
                               "conclusion": "success", "head_sha": self.sha, "head_branch": "main",
                               "path": ".github/workflows/" + filename, "event": "workflow_dispatch",
                               "jobs": [{"name": name, "status": "completed", "conclusion": "success"} for name in jobs]}
        self.receipts = {kind: {"schema": 1, "kind": kind, "status": "PASS", "build_run_id": "101",
                                "source_commit": self.sha, "archive_sha256": self.archive}
                         for kind in ("desktop", "upgrade")}
        self.receipts["upgrade"].update(metadata_sha256=hashlib.sha256(self.metadata).hexdigest(),
                                        previous_version="V2.0.18", older_version="V2.0.6")

    def check(self, **overrides):
        arguments = dict(request=self.request, runs=self.runs, receipts=self.receipts,
                         main_sha=self.sha, latest_version="V2.0.18", metadata=self.metadata)
        arguments.update(overrides)
        return gate.validate(**arguments)

    def test_exact_completed_evidence_accepts(self):
        self.assertEqual(self.check()["status"], "PASS")

    def test_each_workflow_must_fully_succeed(self):
        for kind in self.runs:
            for field, value in (("status", "in_progress"), ("conclusion", "failure")):
                with self.subTest(kind=kind, field=field):
                    runs = copy.deepcopy(self.runs)
                    runs[kind][field] = value
                    with self.assertRaises(ValueError):
                        self.check(runs=runs)

    def test_skipped_mandatory_windows_jobs_reject(self):
        for name in gate.WINDOWS_JOBS:
            with self.subTest(job=name):
                runs = copy.deepcopy(self.runs)
                next(job for job in runs["build"]["jobs"] if job["name"] == name)["conclusion"] = "skipped"
                with self.assertRaises(ValueError):
                    self.check(runs=runs)

    def test_wrong_workflow_source_branch_run_or_event_rejects(self):
        for kind in self.runs:
            for field, value in (("head_sha", "3" * 40), ("head_branch", "feat/example"),
                                 ("path", ".github/workflows/diagnostic.yml"), ("id", 999), ("event", "pull_request")):
                with self.subTest(kind=kind, field=field):
                    runs = copy.deepcopy(self.runs)
                    runs[kind][field] = value
                    with self.assertRaises(ValueError):
                        self.check(runs=runs)

    def test_stale_main_and_same_version_reject(self):
        with self.assertRaises(ValueError):
            self.check(main_sha="3" * 40)
        with self.assertRaises(ValueError):
            self.check(latest_version="V2.0.19")

    def test_receipt_wrong_zip_source_build_or_status_rejects(self):
        for kind in self.receipts:
            for field, value in (("archive_sha256", "4" * 64), ("source_commit", "3" * 40),
                                 ("build_run_id", "999"), ("status", "FAIL"), ("kind", "other")):
                with self.subTest(kind=kind, field=field):
                    receipts = copy.deepcopy(self.receipts)
                    receipts[kind][field] = value
                    with self.assertRaises(ValueError):
                        self.check(receipts=receipts)

    def test_even_outer_json_byte_change_requires_retested_receipt(self):
        with self.assertRaises(ValueError):
            self.check(metadata=self.metadata + b"\n")

    def test_signed_metadata_cannot_describe_another_archive_or_version(self):
        for field, value in (("version", "V2.0.20"), ("archive", {"sha256": "4" * 64})):
            envelope = json.loads(self.metadata)
            payload = json.loads(envelope["payload"])
            payload[field] = value
            envelope["payload"] = json.dumps(payload)
            with self.assertRaises(ValueError):
                self.check(metadata=json.dumps(envelope).encode())

    def test_real_previous_and_distinct_older_client_required(self):
        for field, value in (("previous_version", "V2.0.17"), ("older_version", "V2.0.18")):
            receipts = copy.deepcopy(self.receipts)
            receipts["upgrade"][field] = value
            with self.assertRaises(ValueError):
                self.check(receipts=receipts)

    def test_claimed_pass_requires_all_six_actual_upgrade_cases(self):
        report = {"status": "PASS", "candidateSha256": self.archive, "metadataSha256": "4" * 64,
                  "previousVersion": "V2.0.18", "olderVersion": "V2.0.6", "cases": []}
        for version in ("V2.0.18", "V2.0.6"):
            for fields in (("invalidSignature",), ("corruptArchive", "oldExecutablePreserved"),
                           ("signature", "installation", "healthyRestart", "configurationAndNotebook", "driverAndUserFiles")):
                report["cases"].append({"from": version, **dict.fromkeys(fields, "PASS")})
        gate.validate_upgrade_report(report, self.archive, "4" * 64)
        for index in range(6):
            reduced = copy.deepcopy(report)
            reduced["cases"].pop(index)
            with self.assertRaises(ValueError):
                gate.validate_upgrade_report(reduced, self.archive, "4" * 64)


class PolicyTests(unittest.TestCase):
    def test_current_plan_defers_paid_rulesets_without_requesting_them(self):
        def response(endpoint, method="GET", data=None):
            self.assertNotIn("rulesets", endpoint)
            filename = {"": "repository-settings.json", "actions/permissions/workflow": "workflow-token-permissions.json",
                        "actions/permissions/access": "actions-sharing.json", "actions/permissions": "actions-permissions.json",
                        "actions/permissions/selected-actions": "allowed-actions.json"}[endpoint]
            return json.loads((ROOT / ".github" / filename).read_text())
        output = io.StringIO()
        with mock.patch.object(settings, "api", side_effect=response), mock.patch("sys.argv", ["configure_repository.py"]), contextlib.redirect_stdout(output):
            with self.assertRaises(SystemExit) as exit_result:
                settings.main()
        self.assertEqual(exit_result.exception.code, 0)
        report = json.loads(output.getvalue())
        self.assertFalse(report["optional_rulesets_included"])
        self.assertEqual([row["status"] for row in report["results"] if row["setting"] in ("main", "release-tags")],
                         ["DEFERRED_CURRENT_PLAN", "DEFERRED_CURRENT_PLAN"])

    def test_commit_and_markdown_pr_sections(self):
        for prefix in ("", "## "):
            policy.validate_message("build(V2.0.18): 更新流程\n\n" + "\n".join(
                prefix + heading for heading in ("修改内容：", "验证情况：", "兼容性：")), "V2.0.18")

    def test_wrong_commit_version_or_missing_sections_reject(self):
        for message in ("fix(V2.0.17): 错误版本\n修改内容：\n验证情况：\n兼容性：", "docs(V2.0.18): 缺失验证"):
            with self.assertRaises(ValueError):
                policy.validate_message(message, "V2.0.18")

    def test_basic_guard_distinguishes_plain_private_key_from_historical_ciphertext(self):
        plain = b"-----BEGIN " + b"PRIVATE KEY-----\n" + b"A" * 96 + b"\n-----END " + b"PRIVATE KEY-----"
        encrypted = plain.replace(b"PRIVATE KEY", b"ENCRYPTED PRIVATE KEY")
        self.assertIsNotNone(policy.PRIVATE_KEY.search(plain))
        self.assertIsNone(policy.PRIVATE_KEY.search(encrypted))

    def test_settings_comparison_ignores_server_metadata_but_requires_every_rule(self):
        self.assertTrue(settings.contains({"id": 1, "rules": [{"type": "deletion"}]}, {"rules": [{"type": "deletion"}]}))
        self.assertFalse(settings.contains({"rules": []}, {"rules": [{"type": "deletion"}]}))

    def test_upgrade_uses_actual_archive_version(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "client.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("VERSION", "PROJECT_VERSION=V2.0.18\r\nCPP_VERSION=V2.0.18\r\n")
            self.assertEqual(upgrade.archive_version(archive), "V2.0.18")
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("VERSION", "PROJECT_VERSION=V2.0.18\nCPP_VERSION=V2.0.17\n")
            with self.assertRaises(RuntimeError):
                upgrade.archive_version(archive)


if __name__ == "__main__":
    unittest.main()
