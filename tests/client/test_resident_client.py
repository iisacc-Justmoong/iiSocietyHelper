import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import tempfile
import threading
import unittest

source = Path(__file__).resolve().parents[2] / "src/Client/resident_client.py"
spec = importlib.util.spec_from_file_location("resident_client", source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def frame(value):
    payload = json.dumps(value).encode()
    return struct.pack(">I", len(payload)) + payload


def receive(peer):
    def exact(n):
        chunks = bytearray()
        while len(chunks) < n:
            part = peer.recv(n - len(chunks))
            if not part:
                raise EOFError
            chunks.extend(part)
        return chunks
    return json.loads(exact(struct.unpack(">I", exact(4))[0]))


@unittest.skipUnless(hasattr(socket, "AF_UNIX") and os.name != "nt", "Unix socket fixture")
class ResidentClientTests(unittest.TestCase):
    def test_stream_cancel_and_no_other_client_response(self):
        with tempfile.TemporaryDirectory(dir=os.getcwd()) as root:
            path = os.path.join(root, "backend.sock")
            server = socket.socket(socket.AF_UNIX)
            server.bind(path)
            os.chmod(path, 0o600)
            server.listen()
            errors = []
            def daemon():
                try:
                    with server.accept()[0] as peer:
                        sub = receive(peer)
                        peer.sendall(frame({"type": "result", "id": sub["id"], "accepted": True,
                                            "resident": {"state": "ready"}}))
                        request = receive(peer)
                        peer.sendall(frame({"type": "result", "id": request["id"], "accepted": True}))
                        peer.sendall(frame({"type": "iisaccsonEvent", "id": request["id"],
                                            "event": {"kind": "delta", "text": "answer"}}))
                        cancel = receive(peer)
                        self.assertEqual(cancel["requestId"], request["id"])
                        peer.sendall(frame({"type": "iisaccsonEvent", "id": request["id"],
                                            "event": {"kind": "finished", "finish_reason": "cancelled"}}))
                except Exception as exc:
                    errors.append(exc)
            worker = threading.Thread(target=daemon, daemon=True)
            worker.start()
            try:
                with module.ResidentClient(root) as client:
                    self.assertEqual(client.receive()["resident"]["state"], "ready")
                    ticket = client.generate([{"role": "user", "content": "hello"}])
                    self.assertTrue(client.receive()["accepted"])
                    self.assertEqual(client.receive()["event"]["text"], "answer")
                    client.cancel(ticket)
                    self.assertEqual(client.receive()["event"]["finish_reason"], "cancelled")
                    with self.assertRaises(ValueError):
                        client.cancel(ticket)
            finally:
                worker.join(timeout=5)
                server.close()
            self.assertFalse(worker.is_alive())
            self.assertEqual(errors, [])

    def test_invalid_frame_closes_connection(self):
        with tempfile.TemporaryDirectory(dir=os.getcwd()) as root:
            path = os.path.join(root, "backend.sock")
            server = socket.socket(socket.AF_UNIX)
            server.bind(path)
            os.chmod(path, 0o600)
            server.listen()
            def daemon():
                with server.accept()[0] as peer:
                    receive(peer)
                    peer.sendall(struct.pack(">I", 0xFFFFFFFF))
            worker = threading.Thread(target=daemon, daemon=True)
            worker.start()
            try:
                client = module.ResidentClient(root)
                client.connect()
                with self.assertRaises(ValueError):
                    client.receive()
                with self.assertRaises(ConnectionError):
                    client.receive()
            finally:
                worker.join(timeout=5)
                server.close()
            self.assertFalse(worker.is_alive())


if __name__ == "__main__":
    unittest.main()
