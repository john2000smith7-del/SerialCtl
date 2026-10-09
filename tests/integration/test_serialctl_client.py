import importlib.util
from pathlib import Path
import socket
import struct
import threading
import unittest

spec = importlib.util.spec_from_file_location('client', Path(__file__).resolve().parents[2] / 'tools/serialctl_client.py')
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)

class ClientTest(unittest.TestCase):
    def setUp(self):
        self.server = socket.socket()
        self.server.bind(('127.0.0.1', 0))
        self.server.listen()
        self.port = self.server.getsockname()[1]
        self.stopped = threading.Event()
        self.threads = []
        self.accept = threading.Thread(target=self.serve)
        self.accept.start()

    def tearDown(self):
        self.stopped.set()
        self.server.close()
        self.accept.join(2)
        for thread in self.threads:
            thread.join(2)

    def serve(self):
        self.server.settimeout(.1)
        while not self.stopped.is_set():
            try:
                conn, _ = self.server.accept()
            except OSError:
                continue
            thread = threading.Thread(target=self.handle, args=(conn,))
            self.threads.append(thread)
            thread.start()

    def handle(self, conn):
        with conn:
            conn.settimeout(2)
            request = b''
            while b'\n' not in request:
                part = conn.recv(4096)
                if not part:
                    return
                request += part
            if request == b'SERIALCTL/1 LIST\n':
                conn.sendall(b'SERIALCTL/1 PORTS\nCOM3\nCOM5\n.\nSERIALCTL_INFO\t3\nCOM5\t115200\t8\t0\t0\t0\t2\topen\n')
            elif request == b'SERIALCTL/2 OPEN COM5\n':
                # Coalesce OPEN response with a binary frame, then fragment another.
                conn.sendall(b'SERIALCTL/2 OK WRITE\nD\0\0\0\3\0\xff\n')
                packet = bytearray()
                while len(packet) < 5:
                    packet.extend(conn.recv(5-len(packet)))
                length = struct.unpack('!I', packet[1:5])[0]
                while len(packet) < length + 5:
                    packet.extend(conn.recv(length+5-len(packet)))
                for value in packet:
                    conn.sendall(bytes([value]))

    def test_metadata_and_binary(self):
        names = client.list_ports('127.0.0.1', self.port)
        self.assertEqual([entry['name'] for entry in names], ['COM3','COM5'])
        self.assertEqual(names[1]['baudrate'], 115200)
        self.assertEqual(names[1]['clients'], 2)
        with client.open_serial('127.0.0.1', 'com5', self.port) as stream:
            self.assertEqual(stream.recv(), b'\0\xff\n')
            stream.send(b'\x80hello\r')
            self.assertEqual(stream.recv(), b'\x80hello\r')
            self.assertEqual(stream.recv(), b'')

    def test_auto_fallback(self):
        original = client.PORTS
        client.PORTS = [self.port - 1, self.port]
        try:
            self.assertEqual(client.discover('127.0.0.1', timeout=.2)['port'], self.port)
        finally:
            client.PORTS = original

    def test_unknown_com(self):
        with self.assertRaises(ConnectionError):
            client.open_serial('127.0.0.1', 'COM8', self.port)
        with self.assertRaises(ValueError):
            client.open_serial('127.0.0.1', 'COM5\n', self.port)

if __name__ == '__main__':
    unittest.main()
