"""Exercise the shipped Python API against a controlled HTTP peer."""
import base64
import importlib.util
import json
import pathlib
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("serialctl_api", pathlib.Path(__file__).resolve().parents[2] / "tools" / "serialctl_api.py")
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)


class Peer(BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *_):
        pass

    def do_GET(self):
        self.reply()

    def do_POST(self):
        self.reply()

    def reply(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        Peer.requests.append((self.path, self.headers.get("Authorization"), json.loads(body) if body else None))
        result = {"resources": [{"id": "session-5", "name": "COM5", "com": "COM5", "kind": "serial"}]}
        if self.path.endswith("/input"):
            result = {"accepted": len(base64.b64decode(json.loads(body)["data"]))}
        elif self.path.endswith("/output"):
            result = {"id": "power-action-1", "state": "queued"}
        elif "/actions/" in self.path:
            result = {"id": "power-action-1", "state": "completed"}
        elif self.path == "/api/v1/redirect":
            self.send_response(302)
            self.send_header("Location", "/api/v1/resources")
            self.end_headers()
            return
        payload = json.dumps(result).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


class ClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.peer = ThreadingHTTPServer(("127.0.0.1", 0), Peer)
        cls.thread = threading.Thread(target=cls.peer.serve_forever)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.peer.shutdown()
        cls.thread.join()
        cls.peer.server_close()

    def client(self):
        return api.Client("127.0.0.1", port=self.peer.server_port)

    def test_com_name_resolves_to_selected_resource(self):
        self.assertEqual(self.client().resource("COM5"), "session-5")

    def test_binary_input_is_exact(self):
        raw = b"\0\xffA\r\n"
        self.assertEqual(self.client().input("session-5", raw)["accepted"], len(raw))
        path, auth, body = Peer.requests[-1]
        self.assertEqual(path, "/api/v1/sessions/session-5/input")
        self.assertIsNone(auth)
        self.assertEqual(base64.b64decode(body["data"]), raw)

    def test_output_only_presses_button_and_keeps_request_id(self):
        client = self.client()
        action = client.output(False, "retry-1")
        self.assertEqual(Peer.requests[-1][2], {"enabled": False, "requestId": "retry-1"})
        self.assertEqual(client.wait_action(action)["state"], "completed")

    def test_non_boolean_output_rejected_before_network(self):
        before = len(Peer.requests)
        with self.assertRaises(ValueError):
            self.client().output("false")
        self.assertEqual(len(Peer.requests), before)

    def test_no_token_or_authorization_ui_required(self):
        client = api.Client("127.0.0.1", port=self.peer.server_port)
        self.assertEqual(client.resource("COM5"), "session-5")
        self.assertIsNone(Peer.requests[-1][1])

    def test_redirect_is_refused(self):
        with self.assertRaises(RuntimeError):
            self.client().request("/redirect")

    def test_ambiguous_instance_requires_selection(self):
        with patch.object(api, "discover", return_value=[{"instance": "one", "port": 7080}, {"instance": "two", "port": 7081}]):
            with self.assertRaises(ValueError):
                api.Client("127.0.0.1", )
            self.assertEqual(api.Client("127.0.0.1", instance="two").port, 7081)


if __name__ == "__main__":
    unittest.main()
