"""Python clients against the production C++ gateway with labelled echo transports."""
import base64
import pathlib
import queue
import subprocess
import sys
import threading
import time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from serialctl_ws import Client, ProtocolError, discover
from serialctl_client import SerialStream

fixture = subprocess.Popen([sys.argv[1], '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
clients = []
try:
    port = int(fixture.stdout.readline())
    c = Client('127.0.0.1', port=port)
    clients.append(c)
    assert c.resources()['resources'][0]['kind'] == 'serial'
    found = discover('127.0.0.1')
    assert any(x['port'] == port for x in found)
    a = SerialStream('127.0.0.1', 'COM3', port=port)
    b = SerialStream('127.0.0.1', 'com3', port=port)
    other = SerialStream('127.0.0.1', 'COM5', port=port)
    clients.extend((a, b, other))
    data = bytes(range(256)) * 256  # full 64 KiB; covers NUL, FF, CR/LF and 64-bit WS length
    a.send(data)
    def exact(stream, size):
        result = bytearray()
        while len(result) < size:
            result.extend(stream.recv(size - len(result)))
        return bytes(result)
    assert exact(a, len(data)) == data and exact(b, len(data)) == data
    other.send(b'COM5\x00\xff')
    assert exact(other, len(b'COM5\x00\xff')) == b'COM5\x00\xff'
    for op, params in [('disconnect', {}), ('power.output', {'enabled': True, 'channels': [2]}),
                       ('power.parameters', {'voltage': 1}), ('input', {'data': '!bad'})]:
        try:
            c.request(op, 'power-1' if op.startswith('power') else 'session-3', params)
        except ProtocolError:
            pass
        else:
            raise AssertionError('Unauthorized/invalid operation accepted: ' + op)
    fixture.stdin.write('close COM3\n')
    fixture.stdin.flush()
    assert fixture.stdout.readline().strip() == 'closed COM3'
    other.send(b'still-isolated')
    assert exact(other, 14) == b'still-isolated'
    try:
        a.send(b'missing')
    except (ConnectionError, ProtocolError):
        pass
    else:
        raise AssertionError('Gone target remained writable')
    print('Python / production C++: binary 0-255, 64 KiB, multi-COM, two clients, scope, target loss PASS')
finally:
    for client in clients:
        client.close()
    fixture.stdin.write('quit\n')
    fixture.stdin.flush()
    fixture.wait(timeout=10)
