#!/usr/bin/env python3
"""Authorized LOCAL/TEST SerialCtl only. One connection per probe, no input/power writes.
Run: python security_handshake_probe.py --host 127.0.0.1 --port 7000
Not run in this audit's Linux environment: needs actual running Windows gateway.
"""
import argparse,base64,hashlib,json,os,socket,time
p=argparse.ArgumentParser();p.add_argument('--host',default='127.0.0.1');p.add_argument('--port',type=int,default=7000);args=p.parse_args()
def probe(slow,origin):
 key=base64.b64encode(os.urandom(16)).decode()
 chunks=[b'GET /serialctl HTTP/1.1\r\n',b'Host: 127.0.0.1\r\n',b'Upgrade: websocket\r\n',b'Connection: Upgrade\r\n',b'Sec-WebSocket-Version: 13\r\n',b'Sec-WebSocket-Protocol: serialctl.v1\r\n',('Sec-WebSocket-Key: '+key+'\r\n').encode()]
 if origin:chunks.append(b'Origin: http://audit.invalid\r\n')
 chunks.append(b'\r\n');start=time.monotonic();response=b''
 try:
  with socket.create_connection((args.host,args.port),3) as s:
   s.settimeout(3)
   for chunk in chunks:
    s.sendall(chunk)
    if slow:time.sleep(.9)
   while b'\r\n\r\n' not in response:
    chunk=s.recv(4096)
    if not chunk:break
    response+=chunk
  return {'probe':'slow-handshake' if slow else 'untrusted-origin','seconds':time.monotonic()-start,'header':response.partition(b'\r\n\r\n')[0].decode('ascii',errors='replace'),'response_sha256':hashlib.sha256(response).hexdigest()}
 except OSError as e:return {'probe':'slow-handshake' if slow else 'untrusted-origin','seconds':time.monotonic()-start,'error':str(e)}
print(json.dumps([probe(False,True),probe(True,False)],indent=2))
