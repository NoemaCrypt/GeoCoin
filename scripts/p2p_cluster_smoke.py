#!/usr/bin/env python3
import argparse
import base64
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

try:
    import fcntl
except ImportError:  # pragma: no cover - Windows fallback
    fcntl = None

try:
    import msvcrt
except ImportError:  # pragma: no cover - POSIX fallback
    msvcrt = None


class FileLock:
    def __init__(self, path: Path):
        self.path = path
        self.handle = None

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = open(self.path, "a+", encoding="utf-8")
        if fcntl is not None:
            fcntl.flock(self.handle.fileno(), fcntl.LOCK_EX)
        elif msvcrt is not None:
            self.handle.seek(0)
            if self.handle.read(1) == "":
                self.handle.write("0")
                self.handle.flush()
            self.handle.seek(0)
            msvcrt.locking(self.handle.fileno(), msvcrt.LK_LOCK, 1)
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.handle is None:
            return
        try:
            if fcntl is not None:
                fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
            elif msvcrt is not None:
                self.handle.seek(0)
                msvcrt.locking(self.handle.fileno(), msvcrt.LK_UNLCK, 1)
        finally:
            self.handle.close()
            self.handle = None


def rpc_call(url: str, user: str, password: str, method: str, params=None):
    if params is None:
        params = []

    payload = json.dumps({
        "jsonrpc": "2.0",
        "id": method,
        "method": method,
        "params": params,
    }).encode("utf-8")

    token = base64.b64encode(f"{user}:{password}".encode("utf-8")).decode("ascii")
    request = urllib.request.Request(
        url,
        data=payload,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Basic {token}",
        },
        method="POST",
    )

    with urllib.request.urlopen(request, timeout=10) as response:
        body = json.loads(response.read().decode("utf-8"))

    if body.get("error"):
        raise RuntimeError(f"{method} failed: {body['error']}")
    return body.get("result")


def tail_file(path: Path, lines: int = 80) -> str:
    try:
        content = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        return f"<failed to read {path}: {exc}>"
    return "\n".join(content[-lines:])


def write_private_secret_file(path: Path, value: str):
    path.write_text(f"{value}\n", encoding="utf-8")
    if os.name != "nt":
        path.chmod(0o600)


def reserve_free_ports(count: int) -> tuple[list[int], list[socket.socket]]:
    sockets = []
    ports = []
    try:
        for _ in range(count):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.bind(("127.0.0.1", 0))
            ports.append(s.getsockname()[1])
            sockets.append(s)
        return ports, sockets
    except Exception:
        for s in sockets:
            s.close()
        raise


def release_reserved_port(sockets, index: int):
    if 0 <= index < len(sockets) and sockets[index] is not None:
        sockets[index].close()
        sockets[index] = None


def release_all_reserved_ports(sockets):
    for index in range(len(sockets)):
        release_reserved_port(sockets, index)


def stop_process(proc: subprocess.Popen, allow_sigint: bool = True):
    if proc.poll() is not None:
        return

    if allow_sigint:
        try:
            if os.name == "nt":
                proc.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                proc.send_signal(signal.SIGINT)
        except Exception:
            pass

    try:
        proc.wait(timeout=15)
        return
    except subprocess.TimeoutExpired:
        pass

    proc.terminate()
    try:
        proc.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)


def wait_for_rpc(url: str, user: str, password: str, timeout: float, proc: subprocess.Popen, log_path: Path):
    deadline = time.time() + timeout
    last_error = None
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                f"Node exited before RPC became ready (code={proc.returncode}).\n"
                f"Node log tail:\n{tail_file(log_path)}"
            )
        try:
            rpc_call(url, user, password, "help", [])
            return
        except Exception as exc:
            last_error = exc
            time.sleep(0.5)
    raise RuntimeError(f"RPC did not become ready within {timeout}s: {last_error}")


def wait_for_cluster_ready(nodes: list[dict], timeout: float):
    deadline = time.time() + timeout
    expected_peers = len(nodes) - 1
    last_snapshot = None

    while time.time() < deadline:
        for node in nodes:
            proc = node["proc"]
            if proc.poll() is not None:
                raise RuntimeError(
                    f"Node {node['index']} exited early (code={proc.returncode}).\n"
                    f"Node log tail:\n{tail_file(node['log_path'])}"
                )

        snapshot = []
        all_ready = True
        for node in nodes:
            try:
                peers = rpc_call(
                    node["url"],
                    node["rpc_user"],
                    node["rpc_pass"],
                    "getpeerinfo",
                    [],
                )
                connection_count = rpc_call(
                    node["url"],
                    node["rpc_user"],
                    node["rpc_pass"],
                    "getconnectioncount",
                    [],
                )
                diagnostics = rpc_call(
                    node["url"],
                    node["rpc_user"],
                    node["rpc_pass"],
                    "getconnectiondiagnostics",
                    [],
                )
            except Exception as exc:
                snapshot.append({"index": node["index"], "error": str(exc)})
                all_ready = False
                continue

            diag_stats = diagnostics.get("stats", {}) if isinstance(diagnostics, dict) else {}
            diag_peers = diagnostics.get("peers", []) if isinstance(diagnostics, dict) else []
            diag_attempts = diagnostics.get("outbound_attempts", []) if isinstance(diagnostics, dict) else []
            diag_disconnect_reasons = diagnostics.get("disconnect_reasons", {}) if isinstance(diagnostics, dict) else {}

            authenticated = [p for p in peers if p.get("authenticated")]
            ready = [
                p for p in authenticated
                if str(p.get("state", "")).lower() == "ready"
            ]
            encrypted = [
                p for p in ready
                if p.get("transport_encrypted") is True
                and p.get("secure_wire_version") == 2
                and p.get("secure_suite") == 1
            ]
            encrypted_diagnostics = [
                p for p in diag_peers
                if isinstance(p, dict)
                and p.get("transport_encrypted") is True
                and p.get("secure_wire_version") == 2
                and p.get("secure_suite") == 1
            ] if isinstance(diag_peers, list) else []
            inbound = sum(1 for p in ready if p.get("inbound"))
            outbound = len(ready) - inbound
            expected_outbound = node["index"]
            expected_inbound = expected_peers - expected_outbound

            node_snapshot = {
                "index": node["index"],
                "connection_count": connection_count,
                "authenticated": len(authenticated),
                "ready": len(ready),
                "encrypted": len(encrypted),
                "inbound": inbound,
                "outbound": outbound,
                "expected_inbound": expected_inbound,
                "expected_outbound": expected_outbound,
                "peer_addrs": [p.get("addr") for p in ready],
                "diagnostic_connected": diag_stats.get("connected_peers"),
                "diagnostic_encrypted": diag_stats.get("encrypted_peers"),
                "diagnostic_encrypted_peers": len(encrypted_diagnostics),
                "diagnostic_peers": len(diag_peers) if isinstance(diag_peers, list) else None,
                "diagnostic_outbound_attempts": len(diag_attempts) if isinstance(diag_attempts, list) else None,
                "diagnostic_disconnect_reasons": (
                    len(diag_disconnect_reasons) if isinstance(diag_disconnect_reasons, dict) else None
                ),
            }
            snapshot.append(node_snapshot)

            if connection_count != expected_peers:
                all_ready = False
            if len(ready) != expected_peers:
                all_ready = False
            if len(encrypted) != expected_peers:
                all_ready = False
            if inbound != expected_inbound or outbound != expected_outbound:
                all_ready = False
            if diag_stats.get("connected_peers") != connection_count:
                all_ready = False
            if diag_stats.get("encrypted_peers") != connection_count:
                all_ready = False
            if not isinstance(diag_peers, list) or len(diag_peers) != len(peers):
                all_ready = False
            if len(encrypted_diagnostics) != len(diag_peers):
                all_ready = False
            if not isinstance(diag_attempts, list):
                all_ready = False
            if not isinstance(diag_disconnect_reasons, dict):
                all_ready = False

        if all_ready:
            return snapshot

        last_snapshot = snapshot
        time.sleep(1)

    raise RuntimeError(
        "P2P cluster did not reach expected authenticated topology within "
        f"{timeout}s. Last snapshot: {json.dumps(last_snapshot, indent=2, sort_keys=True)}"
    )


def start_node(args, cluster_dir: Path, index: int, p2p_ports: list[int], rpc_ports: list[int]) -> dict:
    data_dir = cluster_dir / f"node-{index}"
    data_dir.mkdir(parents=True, exist_ok=True)

    rpc_pass_file = data_dir / "rpc-admin.pass"
    rpc_miner_pass_file = data_dir / "rpc-miner.pass"
    rpc_pass = f"{args.rpc_pass_prefix}-{index}"
    rpc_miner_pass = f"{args.rpc_miner_pass_prefix}-{index}"
    write_private_secret_file(rpc_pass_file, rpc_pass)
    write_private_secret_file(rpc_miner_pass_file, rpc_miner_pass)

    command = [
        str(args.node_binary),
        "--data-dir",
        str(data_dir),
        "--p2p-port",
        str(p2p_ports[index]),
        "--rpc-port",
        str(rpc_ports[index]),
        "--rpc",
        "--rpc-user",
        args.rpc_user,
        "--rpc-pass-file",
        str(rpc_pass_file),
        "--rpc-miner-user",
        args.rpc_miner_user,
        "--rpc-miner-pass-file",
        str(rpc_miner_pass_file),
        "--network",
        args.network,
        "--status-interval",
        "3600",
    ]

    if args.activation_manifest:
        command.extend(["--activation-manifest", str(args.activation_manifest)])
    if args.debug:
        command.append("--debug")

    if index == 0:
        command.append("--no-auto-connect")
    else:
        for previous in range(index):
            command.extend(["--connect", f"127.0.0.1:{p2p_ports[previous]}"])

    log_path = data_dir / "geocoin-node.log"
    log_file = open(log_path, "a", encoding="utf-8")
    proc = subprocess.Popen(
        command,
        cwd=str(data_dir),
        stdout=log_file,
        stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
    )

    return {
        "index": index,
        "data_dir": data_dir,
        "log_path": log_path,
        "log_file": log_file,
        "proc": proc,
        "p2p_port": p2p_ports[index],
        "rpc_port": rpc_ports[index],
        "rpc_user": args.rpc_user,
        "rpc_pass": rpc_pass,
        "url": f"http://127.0.0.1:{rpc_ports[index]}/",
        "command": command,
    }


def main():
    parser = argparse.ArgumentParser(description="GeoCoin multi-node P2P smoke test")
    parser.add_argument("--node-binary", required=True, type=Path)
    parser.add_argument("--activation-manifest", type=Path)
    parser.add_argument("--network", default="degree-test")
    parser.add_argument("--nodes", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--rpc-user", default="p2p-smoke")
    parser.add_argument("--rpc-pass-prefix", default="p2p-pass")
    parser.add_argument("--rpc-miner-user", default="p2p-miner")
    parser.add_argument("--rpc-miner-pass-prefix", default="p2p-miner-pass")
    parser.add_argument("--data-dir", type=Path)
    parser.add_argument("--keep-data-dir", action="store_true")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()

    if args.nodes < 2:
        raise RuntimeError("--nodes must be at least 2")
    if args.nodes > 8:
        raise RuntimeError("--nodes is capped at 8 for smoke-test safety")

    args.node_binary = args.node_binary.resolve()
    if args.activation_manifest:
        args.activation_manifest = args.activation_manifest.resolve()

    temp_dir_obj = None
    if args.data_dir is None:
        temp_dir_obj = tempfile.TemporaryDirectory(prefix=f"geocoin-p2p-{args.nodes}-node-")
        cluster_dir = Path(temp_dir_obj.name)
    else:
        cluster_dir = args.data_dir.resolve()
        cluster_dir.mkdir(parents=True, exist_ok=True)

    ports = []
    port_reservations = []
    p2p_ports = []
    rpc_ports = []

    nodes = []
    summary = {
        "node_binary": str(args.node_binary),
        "network": args.network,
        "nodes": args.nodes,
        "data_dir": str(cluster_dir),
    }

    try:
        lock_path = Path(tempfile.gettempdir()) / "geocoin-p2p-cluster-smoke-ports.lock"
        with FileLock(lock_path):
            ports, port_reservations = reserve_free_ports(args.nodes * 2)
            p2p_ports = ports[:args.nodes]
            rpc_ports = ports[args.nodes:]
            summary["p2p_ports"] = p2p_ports
            summary["rpc_ports"] = rpc_ports

            for index in range(args.nodes):
                release_reserved_port(port_reservations, index)
                release_reserved_port(port_reservations, args.nodes + index)
                node = start_node(args, cluster_dir, index, p2p_ports, rpc_ports)
                nodes.append(node)

            for node in nodes:
                wait_for_rpc(
                    node["url"],
                    node["rpc_user"],
                    node["rpc_pass"],
                    args.timeout,
                    node["proc"],
                    node["log_path"],
                )

            release_all_reserved_ports(port_reservations)
            port_reservations = []

        topology = wait_for_cluster_ready(nodes, args.timeout)
        summary["topology"] = topology
        summary["result"] = "ok"
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except Exception as exc:
        summary["result"] = "failed"
        summary["error"] = str(exc)
        print(json.dumps(summary, indent=2, sort_keys=True), file=sys.stderr)
        return 1
    finally:
        release_all_reserved_ports(port_reservations)
        for node in reversed(nodes):
            try:
                rpc_call(node["url"], node["rpc_user"], node["rpc_pass"], "stop", [])
            except Exception:
                pass
        for node in reversed(nodes):
            stop_process(node["proc"])
            node["log_file"].close()
        if temp_dir_obj is not None and not args.keep_data_dir:
            temp_dir_obj.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
