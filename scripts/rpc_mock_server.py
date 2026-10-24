#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    server_version = "GeoCoinRPCMock/0.1"

    def _unauthorized(self) -> None:
        self.send_response(401)
        self.send_header("Content-Type", "text/plain")
        self.send_header("WWW-Authenticate", 'Basic realm="geocoin-rpc"')
        self.end_headers()
        self.wfile.write(b"Unauthorized\n")

    def _too_many_requests(self) -> None:
        self.send_response(429)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Retry-After", "1")
        self.end_headers()
        self.wfile.write(b"Too Many Requests\n")

    def _json(self, obj: dict) -> None:
        raw = json.dumps(obj).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _check_auth(self) -> bool:
        user = self.server.rpc_user  # type: ignore[attr-defined]
        passwd = self.server.rpc_pass  # type: ignore[attr-defined]
        if not user or not passwd:
            return False
        hdr = self.headers.get("Authorization", "")
        if not hdr.startswith("Basic "):
            return False
        try:
            decoded = base64.b64decode(hdr.split(" ", 1)[1]).decode("utf-8")
        except Exception:
            return False
        if ":" not in decoded:
            return False
        u, p = decoded.split(":", 1)
        return u == user and p == passwd

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/":
            self.send_response(404)
            self.end_headers()
            return

        if not self._check_auth():
            self._unauthorized()
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b"{}"
        try:
            req = json.loads(body.decode("utf-8"))
        except Exception as exc:
            self._json({"result": None, "error": {"code": -32700, "message": f"parse error: {exc}"}, "id": None})
            return

        # Optional: simulate server latency.
        delay_ms = self.server.delay_ms  # type: ignore[attr-defined]
        if delay_ms > 0:
            time.sleep(delay_ms / 1000.0)

        # Optional: simulate rate-limits.
        fail_ratio = self.server.fail_ratio  # type: ignore[attr-defined]
        if fail_ratio > 0 and (time.time_ns() % 1000) < int(fail_ratio * 1000):
            self._too_many_requests()
            return

        method = req.get("method", "")
        rid = req.get("id", None)

        # Minimal mocked subset.
        if method == "help":
            result = "== blockchain ==\ngetblockcount\ngetblockhash\ngetblock\n\n== control ==\nhelp\n"
        elif method == "getblockcount":
            result = 7
        elif method == "getblockhash":
            result = "00" * 32
        elif method == "getblockchaininfo":
            result = {"blocks": 7, "network": "mock"}
        else:
            result = True

        self._json({"result": result, "error": None, "id": rid})

    def log_message(self, fmt: str, *args) -> None:  # noqa: D401
        # Keep output quiet by default.
        if getattr(self.server, "verbose", False):  # type: ignore[attr-defined]
            super().log_message(fmt, *args)


def main() -> int:
    ap = argparse.ArgumentParser(description="Mock HTTP JSON-RPC server (Basic Auth) for stress test self-checks.")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=18778)
    ap.add_argument("--user", required=True)
    ap.add_argument("--pass", dest="passwd", required=True)
    ap.add_argument("--delay-ms", type=int, default=0)
    ap.add_argument("--fail-ratio", type=float, default=0.0, help="0..1 fraction of requests that return 429")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    httpd: ThreadingHTTPServer = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.rpc_user = args.user  # type: ignore[attr-defined]
    httpd.rpc_pass = args.passwd  # type: ignore[attr-defined]
    httpd.delay_ms = args.delay_ms  # type: ignore[attr-defined]
    httpd.fail_ratio = args.fail_ratio  # type: ignore[attr-defined]
    httpd.verbose = args.verbose  # type: ignore[attr-defined]

    print(f"rpc_mock_server: listening on http://{args.host}:{args.port}/ (user={args.user})")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
