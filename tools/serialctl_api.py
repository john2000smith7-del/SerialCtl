#!/usr/bin/env python3
"""SerialCtl automatic local-network API client. Legacy serialctl_client.py stays compatible."""
import argparse
import base64
import concurrent.futures
import json
import socket
import sys
import time
import urllib.error
import urllib.request
import uuid


def _discover_one(ip, port, timeout):
    try:
        with socket.create_connection((ip, port), timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(b"SERIALCTL/3 DISCOVER\n")
            data = b""
            while len(data) < 4096 and b"\nEND\n" not in data:
                chunk = sock.recv(4096 - len(data))
                if not chunk:
                    break
                data += chunk
        lines = data.decode("ascii").splitlines()
        if not lines or lines[0] != "SERIALCTL/3 OK":
            return None
        fields = dict(line.split(" ", 1) for line in lines[1:] if " " in line)
        api_port = int(fields["API"])
        if not 1 <= api_port <= 65535:
            return None
        return {"instance": fields["INSTANCE"], "port": api_port,
                "capabilities": fields.get("CAPABILITIES", "").split(",")}
    except (OSError, ValueError, KeyError, UnicodeError):
        return None


def discover(ip, timeout=0.7):
    """Read-only V3 probe; never send HTTP or SCPI to legacy serial sockets."""
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
        results = executor.map(lambda port: _discover_one(ip, port, timeout), range(7000, 7016))
        instances = {}
        for result in results:
            if result:
                instances[result["instance"]] = result
    return list(instances.values())


class Client:
    def __init__(self, ip, port=None, instance=None, timeout=5):
        self.ip = ip
        if port is None:
            found = discover(ip)
            if instance:
                found = [item for item in found if item["instance"] == instance]
            if len(found) != 1:
                raise ValueError("Select one instance/port: " + json.dumps(found))
            port = found[0]["port"]
        self.port, self.timeout = int(port), timeout
        self.base = "http://%s:%d/api/v1" % (ip, self.port)
        # Direct device-LAN connection; no ambient HTTP proxy or redirect.
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())

    def request(self, path, body=None):
        headers = {}
        payload = None
        if body is not None:
            payload = json.dumps(body, allow_nan=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(self.base + path, data=payload, headers=headers)
        try:
            with self.opener.open(request, timeout=self.timeout) as response:
                result = json.load(response)
        except urllib.error.HTTPError as error:
            raise RuntimeError("API HTTP %d: %s" % (error.code, error.read(8192).decode("utf-8", "replace"))) from error
        if "error" in result:
            raise RuntimeError(result["error"])
        return result

    def resources(self):
        return self.request("/resources")

    def resource(self, name):
        matches = [r for r in self.resources()["resources"] if r["id"] == name or
                   r["name"] == name or r.get("com", "").upper() == name.upper()]
        if len(matches) != 1:
            raise ValueError("Resource missing or ambiguous; use the ID returned by list")
        return matches[0]["id"]

    def input(self, resource, data):
        if not 1 <= len(data) <= 65536:
            raise ValueError("Input size must be 1..65536 bytes")
        return self.request("/sessions/%s/input" % resource,
                            {"data": base64.b64encode(data).decode("ascii")})

    def events(self, resource, after=0):
        return self.request("/sessions/%s/events?after=%d" % (resource, after))

    def output(self, enabled, request_id=None):
        if not isinstance(enabled, bool):
            raise ValueError("enabled must be bool")
        return self.request("/power-supplies/power-1/channels/output",
                            {"enabled": enabled,
                             "requestId": request_id or uuid.uuid4().hex})

    def wait_action(self, action, timeout=30):
        deadline = time.monotonic() + timeout
        while action.get("state") in ("queued", "running"):
            if time.monotonic() >= deadline:
                raise TimeoutError("Action still pending; query /actions/" + action["id"])
            time.sleep(0.1)
            action = self.request("/actions/" + action["id"])
        if action.get("state") != "completed":
            raise RuntimeError(json.dumps(action, ensure_ascii=False))
        return action


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise RuntimeError("API redirects are refused")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ip")
    parser.add_argument("--port", type=int)
    parser.add_argument("--instance")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("discover")
    sub.add_parser("list")
    sub.add_parser("power")
    for name in ("on", "off"):
        sub.add_parser(name)
    send = sub.add_parser("send")
    send.add_argument("resource")
    send.add_argument("text")
    send.add_argument("--encoding", default="utf-8")
    send.add_argument("--ending", choices=("CR", "LF", "CRLF", "None"), default="CRLF")
    watch = sub.add_parser("watch")
    watch.add_argument("resource")
    watch.add_argument("--after", type=int, default=0)
    args = parser.parse_args()
    if args.command == "discover":
        print(json.dumps(discover(args.ip), indent=2))
        return
    client = Client(args.ip, port=args.port, instance=args.instance)
    if args.command == "list":
        result = client.resources()
    elif args.command == "power":
        result = client.request("/power-supplies/power-1")
    elif args.command in ("on", "off"):
        result = client.wait_action(client.output(args.command == "on"))
    elif args.command == "send":
        ending = {"CR": "\r", "LF": "\n", "CRLF": "\r\n", "None": ""}[args.ending]
        result = client.input(client.resource(args.resource), (args.text + ending).encode(args.encoding))
    else:
        resource, cursor = client.resource(args.resource), args.after
        while True:
            response = client.events(resource, cursor)
            if response["gap"]:
                raise RuntimeError("History expired: stream gap; refresh and restart")
            for event in response["events"]:
                if event["type"] == "output":
                    sys.stdout.buffer.write(base64.b64decode(event["data"]))
                    sys.stdout.buffer.flush()
                else:
                    print(json.dumps(event, ensure_ascii=False), file=sys.stderr)
            cursor = response["cursor"]
            time.sleep(0.1)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, RuntimeError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
