#!/usr/bin/env python3
"""Replay the measured oversized header into the production Python frame parser.

The handshake/network is replaced with a prepared receive buffer. This does not
claim an end-to-end Windows server reproduction.
"""
from pathlib import Path
import json
import struct
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'source' / 'tools'))
from serialctl_ws import Wire, ProtocolError

measured = json.loads((root / 'evidence/security/production-routines-result.json').read_text())
wire = Wire.__new__(Wire)
wire.fragment = bytearray()
wire.fragment_opcode = 0
wire.buffer = bytearray(b'\x81\x7f' + struct.pack('!Q', measured['events_wire_payload_bytes']))
try:
    wire.receive()
except ProtocolError as error:
    result = {'method': 'production Wire.receive; preloaded oversized header; no handshake/socket',
              'declared_bytes': measured['events_wire_payload_bytes'], 'error': str(error)}
    assert str(error) == 'Message too large'
else:
    raise AssertionError('Expected rejection did not occur')
print(json.dumps(result, indent=2))
