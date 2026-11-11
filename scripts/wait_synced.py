#!/usr/bin/env python3
"""Wait until a GeoCoin JSON-RPC node reports initial sync complete."""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request


def read_secret(path: str) -> str:
    with open(path, "r", encoding="utf-8") as handle:
        return handle.readline().strip()


def rpc_call(url: str, user: str, password: str) -> dict:
    payload = json.dumps({
        "jsonrpc": "2.0",
        "id": "wait-synced",
        "method": "getblockchaininfo",
        "params": [],
    }).encode("utf-8")

    request = urllib.request.Request(
        url,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    if user:
        token = base64.b64encode(f"{user}:{password}".encode("utf-8")).decode("ascii")
        request.add_header("Authorization", f"Basic {token}")

    with urllib.request.urlopen(request, timeout=10) as response:
        data = json.loads(response.read().decode("utf-8"))

    if data.get("error") is not None:
        raise RuntimeError(f"RPC error: {data['error']}")
    result = data.get("result")
    if not isinstance(result, dict):
        raise RuntimeError("RPC response did not contain an object result")
    return result


def is_synced(info: dict) -> bool:
    if "initial_sync_complete" in info:
        return bool(info["initial_sync_complete"])

    blocks = int(info.get("blocks", 0))
    target = int(info.get("target_height", info.get("headers", blocks)))
    best_peer = int(info.get("best_peer_height", target))
    syncing = bool(info.get("syncing", False))
    return not syncing and blocks >= max(target, best_peer)


def format_status(info: dict) -> str:
    blocks = info.get("blocks", "?")
    headers = info.get("headers", "?")
    target = info.get("target_height", "?")
    best_peer = info.get("best_peer_height", "?")
    syncing = info.get("syncing", "?")
    complete = info.get("initial_sync_complete", "?")
    progress = info.get("verificationprogress", "?")
    if isinstance(progress, (int, float)):
        progress = f"{progress:.4f}"
    return (
        f"blocks={blocks} headers={headers} target={target} best_peer={best_peer} "
        f"syncing={syncing} initial_sync_complete={complete} progress={progress}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:3033/")
    parser.add_argument("--user", default=os.environ.get("GEOCOIN_RPC_USER", ""))
    parser.add_argument("--password", default=os.environ.get("GEOCOIN_RPC_PASS", ""))
    parser.add_argument("--password-file", default=os.environ.get("GEOCOIN_RPC_PASS_FILE", ""))
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--interval", type=float, default=5.0)
    args = parser.parse_args()

    password = args.password
    if args.password_file:
        password = read_secret(args.password_file)

    deadline = time.monotonic() + args.timeout
    last_error: Exception | None = None

    while time.monotonic() <= deadline:
        try:
            info = rpc_call(args.url, args.user, password)
            print(format_status(info), flush=True)
            if is_synced(info):
                return 0
            last_error = None
        except (OSError, urllib.error.URLError, RuntimeError, json.JSONDecodeError) as exc:
            last_error = exc
            print(f"wait-synced: {exc}", file=sys.stderr, flush=True)

        sleep_for = min(args.interval, max(0.0, deadline - time.monotonic()))
        if sleep_for <= 0:
            break
        time.sleep(sleep_for)

    if last_error is not None:
        print(f"wait-synced: timed out; last error: {last_error}", file=sys.stderr)
    else:
        print("wait-synced: timed out before initial sync completed", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
