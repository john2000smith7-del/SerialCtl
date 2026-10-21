import json,struct,sys,pathlib
sys.path.insert(0,str(pathlib.Path.cwd()/'tools'))
from serialctl_ws import Wire,ProtocolError
root=pathlib.Path(__file__).parent
records=[]
for line in (root/'wire-responses.jsonl').read_text().splitlines():
 row=json.loads(line); reply=row['reply']
 payload=json.dumps(reply,ensure_ascii=False,separators=(',',':')).encode('utf-8')
 follow={'type':'response','version':1,'instance':'a'*32,'requestId':'follow','ok':True,'result':{'resources':[]}}
 next_payload=json.dumps(follow,separators=(',',':')).encode()
 def frame(data):
  return b'\x81'+(bytes([len(data)]) if len(data)<126 else b'\x7e'+struct.pack('!H',len(data)) if len(data)<=65535 else b'\x7f'+struct.pack('!Q',len(data)))+data
 wire=Wire.__new__(Wire);wire.fragment=bytearray();wire.fragment_opcode=0
 wire.buffer=bytearray(frame(payload)+frame(next_payload))
 assert wire.receive()==reply
 assert wire.receive()==follow
 assert len(payload)<=131072 and reply['ok']==(row['candidateBytes']<=131072)
 records.append({k:row[k] for k in ('idBytes','escaped','candidateBytes')}|{'emittedBytes':len(payload),'followingFrame':'pass'})
wire=Wire.__new__(Wire);wire.fragment=bytearray();wire.fragment_opcode=0
wire.buffer=bytearray(b'\x81\x7f'+struct.pack('!Q',131073))
try:wire.receive()
except ProtocolError as error:
 assert str(error)=='Message too large'
else:raise AssertionError('unbounded baseline frame must fail')
result={'scope':'Production MakeApiResponse output into production Python Wire.receive, preloaded complete frames; no socket/handshake/Windows/hardware','cases':records,'unbounded_rejected':True}
(root/'wire-result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print('Production Python Wire: %d bounded replies and following frames passed; unbounded frame rejected' % len(records))
