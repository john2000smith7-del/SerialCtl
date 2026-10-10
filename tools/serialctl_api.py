#!/usr/bin/env python3
"""SerialCtl unified WebSocket CLI (same core as serialctl_client.py)."""
import argparse
import base64
import json
import sys
from serialctl_ws import Client, discover


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ip')
    parser.add_argument('--port', type=int)
    parser.add_argument('--instance')
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('discover', 'list', 'power', 'on', 'off'):
        sub.add_parser(name)
    send = sub.add_parser('send')
    send.add_argument('resource')
    send.add_argument('text')
    send.add_argument('--encoding', default='utf-8')
    send.add_argument('--ending', choices=('CR', 'LF', 'CRLF', 'None'), default='CRLF')
    watch = sub.add_parser('watch')
    watch.add_argument('resource')
    watch.add_argument('--after', type=int, default=0)
    args = parser.parse_args()
    if args.command == 'discover':
        print(json.dumps(discover(args.ip, args.port), ensure_ascii=False, indent=2))
        return
    with Client(args.ip, args.port, args.instance) as c:
        if args.command == 'list':
            result = c.resources()
        elif args.command == 'power':
            result = c.request('power.get', 'power-1')
        elif args.command in ('on', 'off'):
            result = c.wait_action(c.output(args.command == 'on'))
        elif args.command == 'send':
            ending = {'CR': '\r', 'LF': '\n', 'CRLF': '\r\n', 'None': ''}[args.ending]
            result = c.input(c.resource(args.resource), (args.text + ending).encode(args.encoding))
        else:
            resource = c.resource(args.resource)
            c.subscribe(resource, args.after)
            while True:
                e = c.next_event(resource)
                if e['kind'] == 'output':
                    sys.stdout.buffer.write(base64.b64decode(e['data'], validate=True))
                    sys.stdout.buffer.flush()
                else:
                    print(json.dumps(e, ensure_ascii=False), file=sys.stderr)
        print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, RuntimeError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
