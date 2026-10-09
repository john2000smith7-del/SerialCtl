"""Python AI client versus production C++ gateway; only SerialDevice is in-memory."""
import importlib.util
from pathlib import Path
import subprocess
import sys

spec = importlib.util.spec_from_file_location('client', Path(__file__).resolve().parents[2] / 'tools/serialctl_client.py')
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)
process = subprocess.Popen([sys.argv[1], '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
try:
    port = int(process.stdout.readline().strip())
    # Exercise the same auto-probe function with a controlled ephemeral range.
    client.PORTS = [port]
    entries = client.list_ports('127.0.0.1')
    assert [entry['name'] for entry in entries] == ['COM3', 'COM5'], entries
    assert entries[1]['baudrate'] == 115200 and entries[1]['clients'] == 0, entries
    with client.open_serial('127.0.0.1', 'COM3') as com3, client.open_serial('127.0.0.1', 'COM5') as com5:
        assert client.list_ports('127.0.0.1')[1]['clients'] == 1
        data3 = b'COM3:hello\r\0\xff'
        data5 = b'COM5:hello\r\0\x80'
        com3.send(data3)
        com5.send(data5)
        assert com3.recv() == data3
        assert com5.recv() == data5
        process.stdin.write('close COM3\n')
        process.stdin.flush()
        assert process.stdout.readline().strip() == 'closed COM3'
        assert com3.recv() == b''
        assert [entry['name'] for entry in client.list_ports('127.0.0.1')] == ['COM5']
        payload = bytes(range(256)) * 128
        com5.send(payload)
        assert com5.recv() == payload
        with client.open_serial('127.0.0.1', 'COM5') as other:
            other.send(b'both-writable')
            assert com5.recv() == b'both-writable'
            assert other.recv() == b'both-writable'
    process.stdin.write('quit\n')
    process.stdin.flush()
    assert process.wait(5) == 0
    print('Python client / production multi-COM gateway integration passed')
finally:
    if process.poll() is None:
        process.kill()
    process.communicate(timeout=5)
