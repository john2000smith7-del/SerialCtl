import hashlib,json,os,subprocess,zipfile
from pathlib import Path
source='285ef89d96969f5f55fd59a6b9d49ad64f519367';version='V2.0.7';tag='v2.0.7';repo=os.environ['GITHUB_REPOSITORY']
root=Path(os.environ['RUNNER_TEMP'])/'v207-published';root.mkdir()
release=json.loads(subprocess.check_output(['gh','api',f'repos/{repo}/releases/tags/{tag}']))
expected={'SerialCtl-V2.0.7-Win7-x64.zip','update.json'}
assert not release['draft'] and not release['prerelease'] and release['target_commitish']==source
assert {a['name'] for a in release['assets']}==expected and all(a['state']=='uploaded' for a in release['assets'])
for name in sorted(expected):subprocess.run(['gh','release','download',tag,'--repo',repo,'--pattern',name,'--dir',str(root)],check=True)
metadata=(root/'update.json').read_bytes();assert metadata==Path('candidate-update.json').read_bytes()
payload=json.loads(json.loads(metadata)['payload']);assert payload['version']==version
archive=root/'SerialCtl-V2.0.7-Win7-x64.zip'
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
 return h.hexdigest()
assert sha(archive)==payload['archive']['sha256'] and archive.stat().st_size==payload['archive']['size']
subprocess.run(['python','tools/new_update_feed.py','--verify-archive',str(archive),'--metadata',str(root/'update.json')],check=True)
with zipfile.ZipFile(archive) as z:
 assert z.testzip() is None
 exe=hashlib.sha256(z.read('serialctl.exe')).hexdigest();assert exe==next(f['sha256'] for f in payload['files'] if f['name']=='serialctl.exe')
parts=[]
with archive.open('rb') as f:
 for i in range(6):
  block=f.read(24*1024*1024)
  assert block
  folder=root/f'part-{i}';folder.mkdir();p=folder/f'part-{i}.bin';p.write_bytes(block)
  parts.append({'name':p.name,'size':len(block),'sha256':hashlib.sha256(block).hexdigest()})
 assert not f.read(1),'Archive exceeds the explicitly bounded six-part retrieval'
receipt={'status':'PASS','source':source,'version':version,'release':release['html_url'],'archive_sha256':sha(archive),'archive_size':archive.stat().st_size,'metadata_sha256':sha(root/'update.json'),'exe_sha256':exe,'signature_and_full_archive_verified':True,'urls':{a['name']:a['browser_download_url'] for a in release['assets']},'parts':parts}
(root/'public-release-verification.json').write_text(json.dumps(receipt,indent=2))
print('SERIALCTL_PUBLIC_RELEASE_VERIFIED='+json.dumps(receipt,separators=(',',':')))
