from pathlib import Path
import re,html,json,base64,hashlib,os
root=Path(__file__).parent
run=os.environ.get('NATIVE_RUN_ID','39549514414')
page=(root/('windows-'+run+'.html')).read_text()
plain=html.unescape(re.sub('<[^>]+>',' ',page))
themes={}
for m in re.finditer(r'CMD paste evidence\s*(\{)',plain):
 d=json.JSONDecoder().raw_decode(plain[m.start(1):])[0];themes[d['prefix']]=d
assert set(themes)=={'x64-light-paste','x64-dark-paste'},'native summaries not available yet'
expected_limits={'utf8-encoded-tail-over-limit','clipboard-units-over-limit','control-in-tail','unrepresentable-cp437-tail','utf8-tail-8190-bytes','normalized-batch-65537','normalized-batch-65536'}
expected_batch=('\r\n'.join('rem '+'x'*((8183 if i==7 else 8191)-4) for i in range(8))+'\r\n').encode()
assert len(expected_batch)==65536
for d in themes.values():
 assert d['status']=='PASS' and d['case_count']==37 and len(d['matrix'])==27 and len(d['ordinary'])==3 and len(d['limits'])==7
 sample=d['preserved_draft_sample'];assert sample['rejected_gui_bytes']==0 and sample['preserved_caret']==5 and sample['original_draft_units']==22
 later=base64.b64decode(sample['later_enter_base64'],validate=True)
 token=re.fullmatch(b'echo KEPTORIG_([0-9a-f]{8})_END\r\n',later).group(1).decode()
 original='echo ORIG_'+token+'_END'
 keys={r[0] for r in d['matrix']}
 assert keys=={f'tail-{n}-nl-{nl}-caret-{caret}' for n in (8190,8191,8192) for nl in ('0d','0a','0d0a') for caret in (0,5,22)}
 for name,gui,rejected,caret in d['matrix']:
  n=int(name.split('-')[1]);assert name.endswith('-'+str(caret))
  if n==8192:assert rejected==0 and gui==len(later)
  else:assert rejected is None and gui==len((original[:caret]+'rem PREFIX_'+token+'\r\n').encode())
 for name,gui,async_ok,enter in d['ordinary']:
  nl=name.split('-')[1];assert nl in ('0d','0a','0d0a') and async_ok and enter
  first='echo PASTE_'+token+'_^FIRST_'+nl;tail='echo PASTE_'+token+'_^TAIL_'+nl
  assert gui==len((first+'\r\n'+tail+'\r\n').encode())
 limits={o['case']:o for o in d['limits']};assert set(limits)==expected_limits
 for name in expected_limits-{'utf8-tail-8190-bytes','normalized-batch-65537','normalized-batch-65536'}:
  o=limits[name];assert o['rejected_gui_bytes']==0 and o['preserved_caret']==5 and o['gui_bytes']==len(later) and o['gui_sha256']==hashlib.sha256(later).hexdigest()
 o=limits['utf8-tail-8190-bytes'];assert o['tail_characters']==2730 and o['encoded_tail_bytes']==8190 and o['gui_bytes']==12 and o['gui_sha256']==hashlib.sha256(b'rem PREFIX\r\n').hexdigest()
 o=limits['normalized-batch-65537'];assert o['normalized_bytes']==65537 and o['gui_bytes']==0 and o['gui_sha256']==hashlib.sha256(b'').hexdigest() and o['raw_clipboard_units']==65529
 o=limits['normalized-batch-65536'];assert o['normalized_bytes']==65536 and o['gui_bytes']==65536 and o['gui_event_chunks']==4 and o['gui_sha256']==hashlib.sha256(expected_batch).hexdigest() and o['raw_clipboard_units']==65528
assert len({d['program_sha256'] for d in themes.values()})==1
print(json.dumps({'themes':len(themes),'cases_per_theme':37,'matrix_cases':27,'ordinary_cases':3,'limit_cases':7,'program_sha256':next(iter(themes.values()))['program_sha256'],'normalized_65536_sha256':hashlib.sha256(expected_batch).hexdigest()},indent=2))
(root/'verified-native-summaries.json').write_text(json.dumps(list(themes.values()),ensure_ascii=False,indent=2)+'\n')
