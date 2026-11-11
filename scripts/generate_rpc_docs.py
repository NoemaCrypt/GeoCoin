#!/usr/bin/env python3
"""Generate a compact RPC method reference from getrpcinfo."""

import argparse
import base64
import json
import sys
import urllib.request
from collections import defaultdict
from pathlib import Path


def rpc_call(url: str, user: str, password: str, method: str, params=None):
    if params is None:
        params = []
    payload = json.dumps({
        "jsonrpc": "2.0",
        "id": method,
        "method": method,
        "params": params,
    }, separators=(",", ":")).encode()
    token = base64.b64encode(f"{user}:{password}".encode()).decode()
    req = urllib.request.Request(
        url,
        data=payload,
        headers={
            "Content-Type": "application/json",
            "Authorization": "Basic " + token,
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        body = json.loads(resp.read().decode())
    if body.get("error"):
        raise RuntimeError(f"{method} failed: {body['error']}")
    return body.get("result")


def load_rpcinfo(args):
    if args.input == "-":
        return json.load(sys.stdin)
    if args.input:
        return json.loads(Path(args.input).read_text(encoding="utf-8"))
    return rpc_call(args.url, args.user, args.password, "getrpcinfo")


def render_markdown(rpcinfo: dict) -> str:
    commands = sorted(rpcinfo.get("commands", []), key=lambda c: (c.get("category", ""), c.get("name", "")))
    by_category: dict[str, list[dict]] = defaultdict(list)
    for command in commands:
        by_category[command.get("category") or "uncategorized"].append(command)

    lines = [
        "# GeoCoin RPC Methods",
        "",
        f"API version: `{rpcinfo.get('api_version', 'unknown')}`",
        f"JSON-RPC: `{rpcinfo.get('jsonrpc', '2.0')}`",
        f"Command count: `{rpcinfo.get('command_count', len(commands))}`",
        "",
        "Generated from `getrpcinfo`; rerun this script after changing RPC registrations.",
        "",
    ]

    for category in sorted(by_category):
        lines.append(f"## {category}")
        lines.append("")
        lines.append("| Method | Role | Capabilities | Mutates | Stability | Since | Notes |")
        lines.append("| --- | --- | --- | --- | --- | --- | --- |")
        for command in by_category[category]:
            caps = ", ".join(command.get("capabilities") or [])
            notes = []
            if command.get("requires_wallet"):
                notes.append("wallet")
            if command.get("deprecated"):
                notes.append("deprecated")
            if command.get("deprecation"):
                notes.append(str(command["deprecation"]).replace("|", "\\|"))
            lines.append(
                "| `{name}` | `{role}` | `{caps}` | `{mutates}` | `{stability}` | `{since}` | {notes} |".format(
                    name=command.get("name", ""),
                    role=command.get("role", ""),
                    caps=caps,
                    mutates=str(bool(command.get("mutates"))).lower(),
                    stability=command.get("stability", ""),
                    since=command.get("since", ""),
                    notes=", ".join(notes),
                )
            )
        lines.append("")
    return "\n".join(lines)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", help="Path to getrpcinfo JSON, or '-' for stdin. If omitted, fetch via RPC.")
    parser.add_argument("--url", default="http://127.0.0.1:3033/")
    parser.add_argument("--user", default="local")
    parser.add_argument("--password", default="")
    parser.add_argument("--output", default="docs/rpc/generated/RPC_METHODS.md")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    rpcinfo = load_rpcinfo(args)
    markdown = render_markdown(rpcinfo)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(markdown + "\n", encoding="utf-8")
    print(str(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
