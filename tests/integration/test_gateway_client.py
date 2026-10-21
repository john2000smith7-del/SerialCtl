"""Python clients against the production C++ gateway with labelled echo transports."""
import base64
import json
import pathlib
import queue
import subprocess
import sys
import threading
import time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from serialctl_ws import Client, ProtocolError, CapacityError, discover
from serialctl_client import SerialStream

fixture = subprocess.Popen([sys.argv[1], '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
clients = []
try:
    port = int(fixture.stdout.readline())
    for id_bytes in (1, 128):
        for wire_bytes in (131071, 131072, 131073):
            fixture.stdin.write('history-boundary %d %d\n' % (wire_bytes, id_bytes))
            fixture.stdin.flush()
            prepared = json.loads(fixture.stdout.readline())
            assert prepared == {'wireBytes': wire_bytes, 'events': 6, 'cursor': 6}
            with Client('127.0.0.1', port=port) as boundary:
                def request_history():
                    try:
                        result = boundary.request('events', 'session-5', {'after': 0}, request_id='r' * id_bytes)
                    except ProtocolError as error:
                        assert wire_bytes > 131072, str(error)
                        assert json.loads(str(error))['code'] == 'HISTORY_TOO_LARGE'
                        return None
                    assert wire_bytes <= 131072
                    assert len(result['events']) == 6 and result['gap'] is False
                    return result
                result = request_history()
                assert request_history() == result  # cached success or cached bounded error
                assert boundary.resources()['resources']
                assert boundary.subscribe('session-5')['cursor'] == 6
                history = [boundary.next_event('session-5', timeout=5) for _ in range(6)]
                assert [e['seq'] for e in history] == list(range(1, 7))
                if result is not None:
                    assert history == result['events']
                for index, event in enumerate(history, 1):
                    data = base64.b64decode(event['data'], validate=True)
                    assert data == bytes([index]) * len(data)
                    assert 1 <= len(data) <= 16384
                    if index <= 5:
                        assert len(data) == 16384
                assert history[-1]['source'].startswith('设备')
                boundary.input('session-5', b'LIVE_AFTER_HISTORY_BOUNDARY')
                while True:
                    event = boundary.next_event('session-5', timeout=5)
                    if event['kind'] == 'output':
                        assert base64.b64decode(event['data']) == b'LIVE_AFTER_HISTORY_BOUNDARY'
                        break
                print('Python/C++ history: id_bytes=%d candidate_wire_bytes=%d continued_query_and_subscribe=PASS' %
                      (id_bytes, wire_bytes))
    # Boundary fixtures replace caches; start the remaining scenarios afresh.
    fixture.stdin.write('clear history\n')
    fixture.stdin.flush()
    assert fixture.stdout.readline().strip() == 'history cleared'
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
    other.close()
    fixture.stdin.write('overflow COM5\n')
    fixture.stdin.flush()
    assert fixture.stdout.readline().strip() == 'overflow COM5'
    with SerialStream('127.0.0.1', 'COM5', port=port) as fresh:
        assert fresh.subscription['gap'] is True
        fresh.send(b'LIVE_AFTER_EXPLICIT_GAP')
        assert exact(fresh, 23) == b'LIVE_AFTER_EXPLICIT_GAP'
    capacity = subprocess.Popen([sys.argv[1], '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    held = []
    try:
        full_port = int(capacity.stdout.readline())
        for _ in range(128):
            held.append(Client('127.0.0.1', port=full_port))
        full_instance = held[0].instance
        try:
            Client('127.0.0.1', port=full_port)
        except CapacityError as error:
            assert error.instance == full_instance
        else:
            raise AssertionError('Explicit full port was not diagnosed')
        candidates = discover('127.0.0.1')
        assert any(x['instance'] == full_instance and x['error'] == 'CLIENT_LIMIT' for x in candidates
                   if x['port'] == full_port), {'full_port':full_port,'instance':full_instance,'discovered':candidates}
        for kwargs in ({'port': full_port}, {'instance': full_instance}):
            try:
                Client('127.0.0.1', **kwargs)
            except CapacityError:
                pass
            else:
                raise AssertionError('Full instance was not diagnosed')
        try:
            Client('127.0.0.1')
        except ValueError as error:
            assert 'AMBIGUOUS' in str(error)
        else:
            raise AssertionError('Busy instance vanished from ambiguity check')
    finally:
        for client in held:
            client.close()
        capacity.stdin.write('quit\n')
        capacity.stdin.flush()
        capacity.wait(timeout=10)
    print('Python / production C++: binary 0-255, 64 KiB, multi-COM, two clients, scope, target loss, 128 capacity and ambiguity PASS')
finally:
    for client in clients:
        client.close()
    fixture.stdin.write('quit\n')
    fixture.stdin.flush()
    fixture.wait(timeout=10)
