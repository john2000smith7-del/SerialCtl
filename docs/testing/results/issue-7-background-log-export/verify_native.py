"""Verify raw public Actions annotations; pass the downloaded run HTML path."""
import argparse
import html
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('page', type=Path)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
plain = html.unescape(re.sub('<[^>]+>', ' ', args.page.read_text()))

def observations(title):
    return [json.JSONDecoder().raw_decode(plain[m.start(1):])[0]
            for m in re.finditer(re.escape(title) + r'\s*(\{)', plain)]

native = observations('Native logger export evidence')
gui = observations('GUI background log export evidence')
assert len(native) == 1 and native[0]['success'], 'native result missing or failed'
cases = native[0]['cases']
assert len(cases) == 7, 'incomplete native cases'
large = cases[0]
assert large['bytes'] == large['progress_final'] == 65 * 1024 * 1024
assert large['sha256'] == '293ac2ec51c4b810c496f147ebba14e5916f360a27cab012e7d8eccd1946f8ef'
assert large['producer_ms'] < 1000 and large['stop_while_copy_blocked']
assert large['later_bytes_excluded'] == 1024 * 1024
assert len(gui) == 2, 'both actual GUI themes required'
assert len({d['program_sha256'] for d in gui}) == 1
assert all(re.fullmatch('[0-9a-f]{64}', d['program_sha256']) for d in gui)
themes = set()
for observation in gui:
    export = observation['export']
    assert export['cutoff'] == export['bytes'] >= 65 * 1024 * 1024
    assert export['sha256'] == export['source_prefix_sha256']
    assert export['parts_read'] >= 2 and export['normal_speed_only']
    assert export['ui_max_ms'] < 2000 and export['api_max_ms'] < 2000
    assert observation['source']['batches'] > 0
    assert '不支持的控制字符' in observation['preserved_warning']
    assert '日志已保存' in observation['settings_note']
    theme = re.search(r'x64-(light|dark)-export-large-background\.log', observation['settings_note'])
    assert theme
    themes.add(theme[1])
assert themes == {'light', 'dark'}
verified = {'native': native[0], 'gui': gui}
if args.output:
    args.output.write_text(json.dumps(verified, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({'native_cases': len(cases), 'themes': sorted(themes),
                  'program_sha256': gui[0]['program_sha256'],
                  'busy_samples': [d['export']['observed_busy_samples'] for d in gui]}, indent=2))
