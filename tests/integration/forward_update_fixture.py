"""Create/validate an explicitly non-release next-version upgrade fixture.

No private key is accepted here. The current verified ZIP supplies helper tools;
only VERSION differs in the isolated source used to compile the future EXE.
Formal release tooling deliberately rejects this fixture's manifest/name.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
import new_update_feed as feed


def require(value,why):
    if not value:
        raise ValueError(why)


def descriptor(archive):
    with zipfile.ZipFile(archive) as z:
        names=z.namelist()
        require(set(names)==set(feed.REQUIRED_FILES)|{'FORWARD-TEST.json'},'Unexpected forward fixture contents')
        require(len(names)==len(set(names)),'Duplicate forward fixture entry')
        marker=json.loads(z.read('FORWARD-TEST.json'))
        require(marker.get('testOnly') is True and marker.get('kind')=='isolated-next-version','Not a forward test fixture')
        version=marker['futureVersion']
        require(z.read('VERSION')==('PROJECT_VERSION='+version+'\nCPP_VERSION='+version+'\n').encode(),'Future VERSION mismatch')
        files=[]
        for i,name in enumerate(feed.REQUIRED_FILES):
            info=z.getinfo(name)
            require(0<info.file_size<=feed.MAX_FILE_BYTES,'Future runtime size invalid')
            data=z.read(name)
            files.append({'name':name,'size':len(data),'sha256':hashlib.sha256(data).hexdigest(),'url':'asset-%03d.bin'%i})
        require(sum(f['size'] for f in files)<=feed.MAX_TOTAL_BYTES,'Future runtime total too large')
    return {'schema':2,'product':'SerialCtl','architecture':'x64','version':version,'files':files,
            'archive':{'url':archive.name,'size':archive.stat().st_size,'sha256':feed.sha256(archive)}},marker


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('phase',choices=('prepare','validate'))
    p.add_argument('--candidate',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--source-commit',required=True)
    p.add_argument('--build-run',required=True)
    p.add_argument('--exe',type=Path)
    p.add_argument('--metadata',type=Path)
    args=p.parse_args()
    require(re.fullmatch('[a-f0-9]{40}',args.source_commit),'Invalid source identity')
    base=feed.describe_archive(args.candidate)
    parts=list(map(int,base['version'][1:].split('.')));parts[2]+=1
    future='V'+'.'.join(map(str,parts))
    if args.phase=='prepare':
        require(args.exe and args.exe.is_file(),'Missing future native program')
        args.output.mkdir(parents=True,exist_ok=False)
        marker={'testOnly':True,'kind':'isolated-next-version','sourceCommit':args.source_commit,
                'sourceDelta':['VERSION'],'buildRun':args.build_run,'baseVersion':base['version'],
                'baseArchiveSha256':base['archive']['sha256'],'futureVersion':future,
                'scope':'Unreleased next-version fixture compiled from identical production code; never a formal release'}
        archive=args.output/('SerialCtl-TEST-forward-'+future+'.zip')
        with zipfile.ZipFile(args.candidate) as old,zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as z:
            for name in feed.REQUIRED_FILES:
                data=args.exe.read_bytes() if name=='serialctl.exe' else (
                    ('PROJECT_VERSION='+future+'\nCPP_VERSION='+future+'\n').encode() if name=='VERSION' else old.read(name))
                z.writestr(name,data)
            z.writestr('FORWARD-TEST.json',json.dumps(marker).encode())
        payload,marker=descriptor(archive)
        raw=json.dumps(payload,ensure_ascii=False,separators=(',',':')).encode()
        (args.output/'payload.json').write_bytes(raw)
        (args.output/'source.json').write_text(json.dumps(marker,indent=2),encoding='utf-8')
        encoded=base64.b64encode(raw).decode()
        for offset in range(0,len(encoded),2048):print('SERIALCTL_FORWARD_PAYLOAD_CHUNK='+encoded[offset:offset+2048])
        print('FORWARD_FIXTURE_ONLY='+json.dumps(marker))
    else:
        archives=list(args.output.glob('SerialCtl-TEST-forward-*.zip'))
        require(len(archives)==1 and args.metadata,'Expected one future fixture and public signed metadata')
        payload,marker=descriptor(archives[0])
        require(marker['sourceCommit']==args.source_commit and marker['buildRun']==args.build_run and
                marker['baseArchiveSha256']==base['archive']['sha256'] and marker['futureVersion']==future,'Forward fixture provenance mismatch')
        envelope=json.loads(args.metadata.read_bytes())
        require(set(envelope)=={'schema','payload','signature'} and envelope.get('schema')==2 and json.loads(envelope['payload'])==payload and
                re.fullmatch('[a-f0-9]{512}',envelope['signature']),'Future metadata mismatch')
        # Actual original clients below perform the signature, archive, file and
        # healthy-restart checks. Never convert this fixture into a formal bundle.
        print('PASS: exact test-only forward ZIP/payload and production source identity')


if __name__=='__main__':main()
