"""Execute production workspace commands on POSIX; no SSH or Windows claims."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True, type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve()

    def command(*args):
        return subprocess.check_output([str(probe), *args], text=True)

    def replace(root, old, exists, new, new_exists, ok=True):
        script = command('--replace-script', old, str(int(exists)), new, str(int(new_exists)))
        result = subprocess.run(['sh', '-c', script], cwd=root, capture_output=True, text=True, timeout=10)
        assert (result.returncode == 0) == ok, result.stderr
        assert not list(root.glob('.serialctl-agents*')), 'Staging or writer lock leaked'

    with tempfile.TemporaryDirectory(prefix='serialctl-workspace-') as temporary:
        root = Path(temporary)
        path = root / 'AGENTS.md'
        read = command('--read-script')
        assert subprocess.check_output(['sh', '-c', read], cwd=root) == b'ABSENT\n'
        replace(root, '', False, 'verified build\n', True)
        assert path.read_bytes() == b'verified build\n'
        replace(root, 'verified build\n', True, '', False)
        assert not path.exists(), 'Rollback must restore original absence'

        original = '# User rules\r\n中文规则\r\n'
        path.write_bytes(original.encode())
        updated = original + 'managed practice\n'
        replace(root, original, True, updated, True)
        assert path.read_bytes() == updated.encode(), 'User bytes and CRLF changed'
        replace(root, updated, True, original, True)
        assert path.read_bytes() == original.encode()

        path.write_text('concurrent user edit', encoding='utf-8')
        replace(root, original, True, updated, True, ok=False)
        assert path.read_text() == 'concurrent user edit'
        replace(root, '', False, updated, True, ok=False)
        assert path.read_text() == 'concurrent user edit'

        # Two cooperating writers based on the same snapshot: exactly one may commit.
        path.write_text('base', encoding='utf-8')
        scripts = [command('--replace-script', 'base', '1', value, '1') for value in ['first', 'second']]
        processes = [subprocess.Popen(['sh', '-c', script], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                     for script in scripts]
        results = [process.communicate(timeout=10) for process in processes]
        assert sorted(process.returncode == 0 for process in processes) == [False, True], results
        assert path.read_text() in ['first', 'second']

        path.unlink()
        outside = root / 'outside'
        outside.write_text('unchanged', encoding='utf-8')
        path.symlink_to(outside)
        replace(root, 'unchanged', True, 'danger', True, ok=False)
        assert outside.read_text() == 'unchanged'
        result = subprocess.run(['sh', '-c', read], cwd=root, capture_output=True)
        assert result.returncode != 0, 'Symlink read must be rejected too'
        path.unlink()
        path.mkdir()
        replace(root, '', False, 'danger', True, ok=False)
        assert path.is_dir()
    print('PASS: production workspace read, create, preserve, rollback absence, stale edit, concurrent writers, symlink, directory rejection')


if __name__ == '__main__':
    main()
