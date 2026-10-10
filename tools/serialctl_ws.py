#!/usr/bin/env python3
"""SerialCtl v1 WebSocket core. Python 3 standard library; no REST/Telnet fallback."""
import base64
import concurrent.futures
import hashlib
import json
import os
import queue
import socket
import struct
import threading
import time
import uuid

PORTS = range(7000, 7016)
MAX_MESSAGE = 128 * 1024


class ProtocolError(RuntimeError):
    pass


class Wire:
    def __init__(self, sock, timeout=3):
        self.socket, self.buffer = sock, bytearray()
        self.send_lock = threading.Lock()
        self.fragment, self.fragment_opcode = bytearray(), 0
        sock.settimeout(timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        key = base64.b64encode(os.urandom(16)).decode('ascii')
        sock.sendall(('GET /serialctl HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n'
                      'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                      'Sec-WebSocket-Protocol: serialctl.v1\r\nSec-WebSocket-Key: %s\r\n\r\n'
                      % (*sock.getpeername()[:2], key)).encode('ascii'))
        while b'\r\n\r\n' not in self.buffer:
            self.buffer.extend(self._read(2048))
            if len(self.buffer) > 8192:
                raise ProtocolError('Handshake header too large')
        header, _, self.buffer = self.buffer.partition(b'\r\n\r\n')
        lines = header.decode('ascii').split('\r\n')
        fields = {}
        for line in lines[1:]:
            name, value = line.split(':', 1)
            name = name.lower()
            if name in fields:
                raise ProtocolError('Duplicate handshake header')
            fields[name] = value.strip()
        accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        if (lines[0] != 'HTTP/1.1 101 Switching Protocols' or
                fields.get('sec-websocket-accept') != accept or
                fields.get('sec-websocket-protocol') != 'serialctl.v1' or
                fields.get('upgrade', '').lower() != 'websocket' or
                'upgrade' not in fields.get('connection', '').lower().split(', ')):
            raise ProtocolError('Upgrade/protocol mismatch: ' + lines[0])

    def _read(self, size):
        data = self.socket.recv(size)
        if not data:
            raise ConnectionError('Disconnected; execution may be unknown, do not resend')
        return data

    def exact(self, size):
        while len(self.buffer) < size:
            self.buffer.extend(self._read(min(65536, size - len(self.buffer))))
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    def send(self, opcode, data, final=True):
        data = bytes(data)
        if len(data) > MAX_MESSAGE:
            raise ValueError('Message exceeds 128 KiB')
        mask = os.urandom(4)
        n = len(data)
        head = bytes([(128 if final else 0) | opcode])
        if n < 126:
            head += bytes([128 | n])
        elif n <= 65535:
            head += b'\xfe' + struct.pack('!H', n)
        else:
            head += b'\xff' + struct.pack('!Q', n)
        with self.send_lock:
            self.socket.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def receive(self):
        while True:
            a, b = self.exact(2)
            final, opcode, n = bool(a & 128), a & 15, b & 127
            if a & 112 or b & 128 or opcode not in (0, 1, 2, 8, 9, 10):
                raise ProtocolError('Invalid server frame')
            marker = n
            if n == 126:
                n = struct.unpack('!H', self.exact(2))[0]
                if n < 126:
                    raise ProtocolError('Noncanonical length')
            elif n == 127:
                n = struct.unpack('!Q', self.exact(8))[0]
                if n <= 65535 or n >> 63:
                    raise ProtocolError('Invalid length')
            if opcode & 8 and (not final or marker >= 126):
                raise ProtocolError('Invalid control frame')
            if n > MAX_MESSAGE or (not opcode & 8 and n + len(self.fragment) > MAX_MESSAGE):
                raise ProtocolError('Message too large')
            data = self.exact(n)
            if opcode == 8:
                if len(data) == 1:
                    raise ProtocolError('Invalid close')
                reason = data[2:].decode('utf-8') if len(data) >= 2 else ''
                raise ConnectionError('Server closed: ' + reason)
            if opcode == 9:
                self.send(10, data)
                continue
            if opcode == 10:
                continue
            if opcode == 0:
                if not self.fragment_opcode:
                    raise ProtocolError('Unexpected continuation')
                self.fragment.extend(data)
                if not final:
                    continue
                opcode, data = self.fragment_opcode, bytes(self.fragment)
                self.fragment_opcode, self.fragment = 0, bytearray()
            elif self.fragment_opcode:
                raise ProtocolError('Interleaved message')
            elif not final:
                self.fragment_opcode, self.fragment = opcode, bytearray(data)
                continue
            if opcode != 1:
                raise ProtocolError('JSON text required')
            return json.loads(data.decode('utf-8'))


def _probe(ip, port, timeout):
    try:
        with Client(ip, port=port, timeout=timeout) as client:
            return {'ip': ip, 'port': port, 'instance': client.instance,
                    'resources': client.resources()['resources']}
    except (OSError, ValueError, RuntimeError, KeyError):
        return None


def discover(ip, port=None, timeout=1.5):
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        results = pool.map(lambda p: _probe(ip, p, timeout), [port] if port else PORTS)
        return sorted((x for x in results if x), key=lambda x: x['port'])


class Client:
    def __init__(self, ip, port=None, instance=None, timeout=5):
        self.timeout, self.closed = timeout, False
        self.socket = None
        if port is None:
            found = discover(ip)
            if instance:
                found = [item for item in found if item['instance'] == instance]
            if len(found) != 1:
                raise ValueError('INSTANCE_AMBIGUOUS_OR_NOT_FOUND: select --instance or --port: ' + json.dumps(found))
            port, instance = found[0]['port'], found[0]['instance']
        self.ip, self.port = ip, int(port)
        self.pending, self.events, self.cursors = {}, {}, {}
        self.lock = threading.Lock()
        self.failure = None
        try:
            self.socket = socket.create_connection((ip, self.port), timeout)
            self.wire = Wire(self.socket, timeout)
            hello = self.wire.receive()
            if hello.get('type') != 'hello' or hello.get('version') != 1:
                raise ProtocolError('PROTOCOL_MISMATCH')
            self.instance = hello['instance']
            if instance and instance != self.instance:
                raise ProtocolError('INSTANCE_MISMATCH')
            self.socket.settimeout(None)
            self.reader = threading.Thread(target=self._receive, name='serialctl-ws', daemon=True)
            self.reader.start()
        except BaseException:
            if self.socket:
                self.socket.close()
            raise

    def _receive(self):
        try:
            while not self.closed:
                msg = self.wire.receive()
                if msg.get('instance', self.instance) != self.instance:
                    raise ProtocolError('INSTANCE_MISMATCH')
                with self.lock:
                    if msg.get('type') == 'response':
                        target = self.pending.get(msg.get('requestId'))
                    else:
                        target = self.events.get(msg.get('resource'))
                    if target:
                        # A stopped consumer must not grow client memory without bound.
                        target.put_nowait(msg)
        except (OSError, ValueError, RuntimeError, queue.Full) as error:
            self.failure = error
            with self.lock:
                for target in list(self.pending.values()) + list(self.events.values()):
                    try:
                        target.put_nowait(error)
                    except queue.Full:
                        pass
            try:
                self.socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def request(self, op, resource='', params=None, request_id=None):
        request_id = request_id or uuid.uuid4().hex
        target = queue.Queue(maxsize=1)
        with self.lock:
            if self.failure or self.closed:
                raise ConnectionError(str(self.failure or 'Client closed'))
            if request_id in self.pending:
                raise ValueError('requestId already pending')
            self.pending[request_id] = target
        try:
            message = {'type': 'request', 'version': 1, 'instance': self.instance,
                       'requestId': request_id, 'op': op, 'params': {} if params is None else params}
            if resource:
                message['resource'] = resource
            self.wire.send(1, json.dumps(message, allow_nan=False, separators=(',', ':')).encode('utf-8'))
            try:
                reply = target.get(timeout=self.timeout)
            except queue.Empty as error:
                raise TimeoutError('Request %s timed out; execution unknown, do not resend' % request_id) from error
            if isinstance(reply, BaseException):
                raise reply
            if not reply.get('ok'):
                raise ProtocolError(json.dumps(reply.get('error'), ensure_ascii=False))
            return reply['result']
        finally:
            with self.lock:
                self.pending.pop(request_id, None)

    def resources(self):
        return self.request('resources')

    def resource(self, name):
        matches = [r for r in self.resources()['resources'] if
                   r['id'] == name or r.get('name') == name or r.get('com', '').upper() == name.upper()]
        if len(matches) != 1:
            raise ValueError('TARGET_MISSING_OR_AMBIGUOUS: select an existing resource ID')
        return matches[0]['id']

    def input(self, resource, data, request_id=None):
        if not 1 <= len(data) <= 65536:
            raise ValueError('Input size must be 1..65536')
        return self.request('input', resource, {'data': base64.b64encode(data).decode()}, request_id)

    def subscribe(self, resource, after=0):
        with self.lock:
            if resource in self.events:
                raise ValueError('Already subscribed')
            self.events[resource] = queue.Queue(maxsize=128)  # <= 4 MiB decoded JSON events
        try:
            return self.request('subscribe', resource, {'after': after})
        except BaseException:
            with self.lock:
                self.events.pop(resource, None)
            raise

    def unsubscribe(self, resource):
        result = self.request('unsubscribe', resource)
        with self.lock:
            self.events.pop(resource, None)
        return result

    def next_event(self, resource, timeout=None):
        event = self.events[resource].get(timeout=timeout)
        if isinstance(event, BaseException):
            raise event
        if event.get('type') == 'resource_gone':
            raise ConnectionError('TARGET_GONE: ' + resource)
        self.cursors[resource] = event['seq']
        return event

    def events_since(self, resource, after=0):
        return self.request('events', resource, {'after': after})

    def output(self, enabled, request_id=None):
        if type(enabled) is not bool:
            raise ValueError('enabled must be boolean')
        return self.request('power.output', 'power-1', {'enabled': enabled}, request_id)

    def wait_action(self, action, timeout=30):
        deadline = time.monotonic() + timeout
        while action.get('state') in ('queued', 'running'):
            if time.monotonic() >= deadline:
                raise TimeoutError('Action pending: query action.get id=' + action['id'])
            time.sleep(0.1)  # action status only; terminal output is event driven
            action = self.request('action.get', params={'id': action['id']})
        if action.get('state') != 'completed':
            raise ProtocolError(json.dumps(action, ensure_ascii=False))
        return action

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.socket:
            try:
                self.socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.socket.close()
        if hasattr(self, 'reader') and self.reader is not threading.current_thread():
            self.reader.join(timeout=4)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
