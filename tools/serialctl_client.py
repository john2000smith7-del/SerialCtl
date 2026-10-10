#!/usr/bin/env python3
"""SerialCtl named-COM stream, using only serialctl.v1 WebSocket."""
import base64
import queue
import sys
from serialctl_ws import Client, discover
from serialctl_api import main


def list_ports(ip, port=None, timeout=1.5):
    found = discover(ip, port, timeout)
    if len(found) != 1:
        raise ValueError('Select instance/port explicitly: ' + str(found))
    return [r for r in found[0]['resources'] if r['kind'] == 'serial']


class SerialStream:
    def __init__(self, ip, com, port=None, timeout=5, instance=None):
        if not com.upper().startswith('COM') or not com[3:].isdigit():
            raise ValueError('Explicit COM name required')
        self.client = Client(ip, port, instance, timeout)
        self.timeout, self.buffer = timeout, bytearray()
        try:
            self.resource = self.client.resource(com)
            self.client.subscribe(self.resource)
        except BaseException:
            self.close()
            raise

    def send(self, data):
        return self.client.input(self.resource, bytes(data))

    sendall = send

    def recv(self, size=65536):
        while not self.buffer:
            try:
                e = self.client.next_event(self.resource, self.timeout)
            except queue.Empty as error:
                raise TimeoutError('No output before read timeout') from error
            if e['kind'] == 'output':
                self.buffer.extend(base64.b64decode(e['data'], validate=True))
        result = bytes(self.buffer[:size])
        del self.buffer[:size]
        return result

    def close(self):
        self.client.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, RuntimeError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
