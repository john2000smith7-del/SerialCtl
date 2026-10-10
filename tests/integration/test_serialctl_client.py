"""Focused core tests; production C++ interoperability is test_gateway_client.py."""
import pathlib
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from serialctl_ws import Client, Wire, ProtocolError, CapacityError
from serialctl_client import SerialStream
import serialctl_api
import serialctl_client


class CoreTests(unittest.TestCase):
    def test_common_core(self):
        self.assertIs(serialctl_api.Client, serialctl_client.Client)
        self.assertIs(serialctl_api.Client, Client)

    def test_no_random_instance_selection(self):
        with patch('serialctl_ws.discover', return_value=[{'port': 7000, 'instance': 'a'}, {'port': 7001, 'instance': 'b'}]):
            with self.assertRaisesRegex(ValueError, 'AMBIGUOUS'):
                Client('127.0.0.1')

    def test_explicit_com(self):
        for name in ('', 'first', 'COM', 'COM3\n'):
            with self.assertRaises(ValueError):
                SerialStream('127.0.0.1', name)

    def test_capacity_is_not_no_service(self):
        with patch('serialctl_ws.discover', return_value=[{'port': 7000, 'instance': 'a', 'error': 'CLIENT_LIMIT'}]):
            with self.assertRaisesRegex(CapacityError, 'CLIENT_LIMIT'):
                Client('127.0.0.1')

    def test_busy_instance_still_counts_for_ambiguity(self):
        found = [{'port': 7000, 'instance': 'a', 'error': 'CLIENT_LIMIT'}, {'port': 7001, 'instance': 'b'}]
        with patch('serialctl_ws.discover', return_value=found):
            with self.assertRaisesRegex(ValueError, 'AMBIGUOUS'):
                Client('127.0.0.1')

    def test_early_capacity_reply_survives_write_error(self):
        class EarlyReply:
            def settimeout(self, _): pass
            def setsockopt(self, *args): pass
            def getpeername(self): return ('127.0.0.1', 7000)
            def sendall(self, _): raise ConnectionResetError('Peer already half-closed')
            def recv(self, _):
                return (b'HTTP/1.1 503 Service Unavailable\r\nSec-WebSocket-Protocol: serialctl.v1\r\n'
                        b'X-SerialCtl-Instance: instance-a\r\nContent-Length: 12\r\n\r\nCLIENT_LIMIT')
        with self.assertRaisesRegex(CapacityError, 'instance-a'):
            Wire(EarlyReply())

    def test_input_bounds(self):
        client = object.__new__(Client)
        with self.assertRaises(ValueError):
            client.input('session-1', b'')
        with self.assertRaises(ValueError):
            client.input('session-1', bytes(65537))

    def test_frame_limit(self):
        wire = object.__new__(Wire)
        with self.assertRaises(ValueError):
            wire.send(1, bytes(128 * 1024 + 1))

    def test_power_boolean(self):
        client = object.__new__(Client)
        for value in (0, 1, 'false', None):
            with self.assertRaises(ValueError):
                client.output(value)


if __name__ == '__main__':
    unittest.main()
