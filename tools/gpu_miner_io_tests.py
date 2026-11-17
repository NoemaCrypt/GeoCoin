"""Offline HTTP/pipe contracts. The executable contains fake GPU backends only."""
import argparse
import contextlib
import json
import queue
import socketserver
import subprocess
import threading
import time
import unittest

BINARY = None


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        length = 0
        while True:
            line = self.rfile.readline()
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":", 1)[1])
            if not line or line == b"\r\n":
                break
        self.rfile.read(length)
        self.server.requests += 1
        mode = self.server.mode
        if mode == "stall":
            self.server.received.set()
            self.server.finish.wait(8)
            return
        if mode == "retry" or (mode == "recover" and self.server.requests == 1):
            raw = b"HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\n\r\n"
        elif mode == "auth":
            raw = b"HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n"
        elif mode == "truncated":
            raw = b"HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n{\"result\":null}"
        elif mode == "bad-chunk":
            raw = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nnot-hex\r\n"
        elif mode == "truncated-chunk":
            raw = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n"
        elif mode == "corrupt":
            raw = b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n{"
        else:
            body = json.dumps({"result": {"bits": 545259519, "blockhex": "00" * 124, "height": 1}}).encode()
            if mode == "chunked":
                split = len(body) // 2
                chunks = (body[:split], body[split:])
                raw = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                raw += b"".join(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n" for chunk in chunks)
                raw += b"0\r\nX-Fixture: done\r\n\r\n"
            else:
                raw = b"HTTP/1.1 200 OK\r\nContent-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body
        if mode == "fragmented":
            for start in range(0, len(raw), 7):
                self.request.sendall(raw[start:start + 7])
                time.sleep(0.001)
        else:
            self.request.sendall(raw)
        self.server.received.set()
        if mode in ("chunked", "length", "fragmented"):
            # Completion must be based on HTTP framing, not connection close.
            self.server.finish.wait(8)


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = False


@contextlib.contextmanager
def fixture(mode):
    with Server(("127.0.0.1", 0), Handler) as server:
        server.mode = mode
        server.requests = 0
        server.received = threading.Event()
        server.finish = threading.Event()
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            yield server, f"http://127.0.0.1:{server.server_address[1]}/"
        finally:
            server.finish.set()
            server.shutdown()
            worker.join(2)


class Child:
    def __init__(self, *args):
        self.process = subprocess.Popen([BINARY, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        self.events = []
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            event = json.loads(line)
            self.events.append(event)
            self.lines.put(event)

    def state(self, name):
        deadline = time.monotonic() + 4.5
        while time.monotonic() < deadline:
            event = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            if event.get("event") == "state" and event.get("name") == name:
                return event
        raise AssertionError(f"No {name} state: {self.events}")

    def stop(self, eof=False):
        start = time.monotonic()
        if eof:
            self.process.stdin.close()
        else:
            self.process.stdin.write("stop\n")
            self.process.stdin.flush()
        self.process.wait(timeout=4.5)
        self.reader.join(1)
        assert self.process.returncode == 0, self.process.stderr.read()
        assert [e["name"] for e in self.events if e.get("event") == "state"][-2:] == ["Stopping", "Stopped"]
        return time.monotonic() - start

    def close(self):
        if self.process.poll() is None:
            self.process.kill()  # Only the fixture child created by this test.
        self.process.wait(timeout=5)
        self.reader.join(1)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()


class Contracts(unittest.TestCase):
    def child(self, *args):
        child = Child(*args)
        self.addCleanup(child.close)
        return child

    def test_stop_and_eof_during_mock_backend(self):
        for eof in (False, True):
            child = self.child("--stdin-fixture")
            child.state("Running")
            self.assertLess(child.stop(eof), 4.5)

    def test_stop_during_hung_io(self):
        with fixture("stall") as (server, url):
            child = self.child("--rpc-fixture", url)
            self.assertTrue(server.received.wait(3))
            self.assertLess(child.stop(), 4.5)

    def test_stop_during_retry(self):
        with fixture("retry") as (_, url):
            child = self.child("--rpc-fixture", url)
            retry = child.state("Retrying")
            self.assertEqual(retry["retry_delay_ms"], 1000)
            self.assertLess(child.stop(), 4.5)

    def test_retry_recovery(self):
        with fixture("recover") as (_, url):
            child = self.child("--rpc-fixture", url)
            child.state("Retrying")
            child.state("Running")
            child.stop()

    def test_framing_without_eof(self):
        for mode in ("length", "chunked", "fragmented"):
            with fixture(mode) as (_, url):
                child = self.child("--rpc-fixture", url)
                child.state("Running")
                child.stop()

    def test_auth_corrupt_and_truncation_fatal(self):
        for mode, category in (("auth", "auth"), ("truncated", "corrupt"),
                               ("bad-chunk", "corrupt"), ("truncated-chunk", "corrupt"), ("corrupt", "corrupt")):
            with fixture(mode) as (server, url):
                child = self.child("--rpc-fixture", url)
                event = child.state("Error")
                self.assertEqual(event["category"], category)
                self.assertEqual(child.process.wait(timeout=3), 1)
                self.assertEqual(server.requests, 1)

    def test_hostname_resolver(self):
        with fixture("length") as (_, url):
            child = self.child("--rpc-fixture", url.replace("127.0.0.1", "localhost"))
            child.state("Running")
            child.stop()

    def test_gui_crash_fixture(self):
        child = self.child("--crash-fixture")
        self.assertEqual(child.state("Error")["message"], "offline fixture crash")
        self.assertEqual(child.process.wait(timeout=3), 7)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args, remainder = parser.parse_known_args()
    BINARY = args.binary
    unittest.main(argv=[__file__, *remainder])
