"""Focused core tests; production C++ interoperability is test_gateway_client.py."""
import pathlib
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from serialctl_ws import Client, Wire, ProtocolError
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
