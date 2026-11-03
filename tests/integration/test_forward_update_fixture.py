"""Forward test artifacts must never masquerade as formal release bundles."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile
from forward_update_fixture import descriptor, feed


class ForwardFixtureTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.archive=Path(self.temp.name)/'SerialCtl-TEST-forward-V2.0.20.zip'

    def write(self, *, marker=True, version='V2.0.20', extra=None):
        with zipfile.ZipFile(self.archive,'w',compression=zipfile.ZIP_DEFLATED) as z:
            for name in feed.REQUIRED_FILES:
                z.writestr(name,('PROJECT_VERSION='+version+'\nCPP_VERSION='+version+'\n').encode() if name=='VERSION' else b'MZ-test-only-'+name.encode())
            z.writestr('FORWARD-TEST.json',json.dumps({'testOnly':marker,'kind':'isolated-next-version','futureVersion':'V2.0.20'}))
            if extra:z.writestr(extra,b'unexpected')

    def test_exact_bytes_and_runtime_allowlist(self):
        self.write();payload,marker=descriptor(self.archive)
        self.assertTrue(marker['testOnly'])
        self.assertEqual(payload['archive']['sha256'],hashlib.sha256(self.archive.read_bytes()).hexdigest())
        self.assertEqual([f['name'] for f in payload['files']],list(feed.REQUIRED_FILES))
        self.assertEqual(payload['version'],'V2.0.20')

    def test_formal_signing_describer_rejects_fixture(self):
        self.write()
        with self.assertRaises(ValueError):feed.describe_archive(self.archive)

    def test_marker_required(self):
        self.write(marker=False)
        with self.assertRaises(ValueError):descriptor(self.archive)

    def test_version_mismatch_rejected(self):
        self.write(version='V2.0.19')
        with self.assertRaises(ValueError):descriptor(self.archive)

    def test_unknown_or_traversal_member_rejected(self):
        for extra in ('unexpected.exe','../serialctl.exe'):
            with self.subTest(extra=extra):
                self.write(extra=extra)
                with self.assertRaises(ValueError):descriptor(self.archive)


if __name__=='__main__':unittest.main()
