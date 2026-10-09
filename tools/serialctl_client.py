#!/usr/bin/env python3
"""SerialCtl named-COM client. Python 3 standard library, binary-safe, no extra packages."""
import argparse
import concurrent.futures
import json
import socket
import struct
import sys
import threading

PORTS = range(7000, 7016)
MAX_FRAME = 1024 * 1024


def _query(ip, port, timeout):
    with socket.create_connection((ip, port), timeout) as conn:
        conn.settimeout(timeout)
        conn.sendall(b'SERIALCTL/1 LIST\n')
        reply = bytearray()
        while len(reply) < 65536:
            try:
                part = conn.recv(4096)
            except socket.timeout:
                if b'\n.\n' in reply:
                    break  # Older servers may keep the socket open after their list.
                raise
            if not part:
                break
            reply.extend(part)
        if len(reply) >= 65536 or not reply.startswith(b'SERIALCTL/1 PORTS\n') or b'\n.\n' not in reply:
            raise ValueError('Not a SerialCtl discovery response')
        names, _, extension = bytes(reply[18:]).partition(b'.\n')
        entries = {name.decode('utf-8'): {'name': name.decode('utf-8'), 'state': 'open', 'baudrate': None, 'clients': None}
                   for name in names.splitlines() if name}
        lines = extension.decode('utf-8').splitlines()
        if lines and lines[0] == 'SERIALCTL_INFO\t3':
            for line in lines[1:]:
                fields = line.split('\t')
                if len(fields) == 8 and fields[0] in entries:
                    entries[fields[0]].update(zip(('baudrate', 'data_bits', 'parity', 'stop_bits', 'flow_control', 'clients'), map(int, fields[1:7])))
                    entries[fields[0]]['state'] = fields[7]
        return {'ip': ip, 'port': port, 'ports': list(entries.values())}


def discover(ip, port=None, timeout=1.5):
    """Probe only SerialCtl's bounded default range; an explicit port overrides it."""
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        futures = {pool.submit(_query, ip, p, timeout): p for p in ([port] if port else PORTS)}
        found = []
        for future in concurrent.futures.as_completed(futures):
            try:
                found.append(future.result())
            except (OSError, ValueError, UnicodeError):
                pass
    if not found:
        raise ConnectionError('No SerialCtl gateway found; check IP, opened COMs and firewall (TCP 7000–7015)')
    return min(found, key=lambda item: item['port'])


def list_ports(ip, port=None, timeout=1.5):
    return discover(ip, port, timeout)['ports']


class SerialStream:
    """One socket bound to one COM. recv() returns bytes, b'' means closed; timeout raises."""
    def __init__(self, ip, com, port=None, timeout=5):
        if not com.upper().startswith('COM') or not com[3:].isdigit():
            raise ValueError('COM must be a name such as COM5')
        endpoint = discover(ip, port)
        com = com.upper()
        if com not in [entry['name'] for entry in endpoint['ports']]:
            raise ConnectionError(com + ' is not currently opened/shared')
        self.socket = socket.create_connection((ip, endpoint['port']), timeout)
        self.socket.settimeout(timeout)
        self._buffer = bytearray()
        self._send_lock = threading.Lock()
        self._read_lock = threading.Lock()
        try:
            self.socket.sendall(('SERIALCTL/2 OPEN ' + com + '\n').encode('ascii'))
            while b'\n' not in self._buffer and len(self._buffer) < 4096:
                part = self.socket.recv(4096)
                if not part:
                    raise ConnectionError('Connection closed during OPEN')
                self._buffer.extend(part)
            line, separator, tail = self._buffer.partition(b'\n')
            if not separator or line != b'SERIALCTL/2 OK WRITE':
                raise ConnectionError('COM connection refused: ' + line.decode('ascii', 'replace'))
            self._buffer = bytearray(tail)
        except BaseException:
            self.close()
            raise

    def send(self, data):
        data = bytes(data)
        if len(data) > MAX_FRAME:
            raise ValueError('Frame exceeds 1 MiB')
        with self._send_lock:
            self.socket.sendall(b'D' + struct.pack('!I', len(data)) + data)

    sendall = send

    def recv(self):
        # Preserve partial headers/payloads across timeouts; never lose framing.
        with self._read_lock:
            while True:
                while len(self._buffer) < 5:
                    part = self.socket.recv(65536)
                    if not part:
                        if self._buffer:
                            raise ConnectionError('Truncated frame header')
                        return b''
                    self._buffer.extend(part)
                kind = self._buffer[0]
                length = struct.unpack('!I', self._buffer[1:5])[0]
                if length > MAX_FRAME or kind not in (ord('D'), ord('C')):
                    self.close()
                    raise ConnectionError('Invalid SerialCtl frame')
                while len(self._buffer) < length + 5:
                    part = self.socket.recv(min(65536, length + 5 - len(self._buffer)))
                    if not part:
                        raise ConnectionError('Truncated frame payload')
                    self._buffer.extend(part)
                payload = bytes(self._buffer[5:length + 5])
                del self._buffer[:length + 5]
                if kind == ord('D') and payload:
                    return payload

    def close(self):
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def open_serial(ip, com, port=None, timeout=5):
    return SerialStream(ip, com, port, timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('list', 'stream'))
    parser.add_argument('ip')
    parser.add_argument('com', nargs='?')
    parser.add_argument('--port', type=int)
    args = parser.parse_args()
    if args.action == 'list':
        print(json.dumps(discover(args.ip, args.port), ensure_ascii=False, indent=2))
        return
    if not args.com:
        parser.error('stream requires COM name')
    with open_serial(args.ip, args.com, args.port) as stream:
        stopped = threading.Event()
        def receive():
            try:
                while not stopped.is_set():
                    try:
                        data = stream.recv()
                    except socket.timeout:
                        continue
                    if not data:
                        break
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
            except OSError as error:
                if not stopped.is_set():
                    print(str(error), file=sys.stderr)
            finally:
                stopped.set()
        reader = threading.Thread(target=receive, daemon=True)
        reader.start()
        try:
            while not stopped.is_set():
                data = sys.stdin.buffer.read1(4096)
                if not data:
                    break
                stream.send(data)
        finally:
            stopped.set()
            stream.close()
            reader.join(2)


if __name__ == '__main__':
    main()
