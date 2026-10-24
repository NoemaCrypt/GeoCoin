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
import urllib.request
from pathlib import Path


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def write_secret(path: Path, value: str) -> None:
    path.write_text(value + "\n", encoding="utf-8")
    if os.name != "nt":
        path.chmod(0o600)


def tail(path: Path, lines: int = 120) -> str:
    try:
        return "\n".join(path.read_text(encoding="utf-8", errors="replace").splitlines()[-lines:])
    except OSError as exc:
        return f"<failed to read {path}: {exc}>"


def rpc_call(url: str, user: str, password: str, method: str, params=None):
    if params is None:
        params = []
    body = json.dumps({
        "jsonrpc": "2.0",
        "id": method,
        "method": method,
        "params": params,
    }).encode("utf-8")
    token = base64.b64encode(f"{user}:{password}".encode("utf-8")).decode("ascii")
    request = urllib.request.Request(
        url,
        data=body,
        headers={
            "Authorization": f"Basic {token}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=15) as response:
        payload = json.loads(response.read().decode("utf-8"))
    if payload.get("error"):
        raise RuntimeError(f"{method} failed: {payload['error']}")
    return payload.get("result")


def wait_for_rpc(node, timeout: float) -> None:
    deadline = time.time() + timeout
    last_error = None
    while time.time() < deadline:
        if node["proc"].poll() is not None:
            raise RuntimeError(
                f"{node['name']} exited before RPC was ready (code={node['proc'].returncode})\n"
                f"{tail(node['log'])}"
            )
        try:
            rpc_call(node["url"], node["user"], node["password"], "getblockcount")
            return
        except Exception as exc:  # noqa: BLE001
            last_error = exc
            time.sleep(0.5)
    raise RuntimeError(f"{node['name']} RPC not ready: {last_error}\n{tail(node['log'])}")


def wait_for_height(node, height: int, timeout: float) -> int:
    deadline = time.time() + timeout
    last_height = -1
    while time.time() < deadline:
        last_height = int(rpc_call(node["url"], node["user"], node["password"], "getblockcount"))
        if last_height >= height:
            return last_height
        time.sleep(1)
    raise RuntimeError(f"{node['name']} did not reach height {height}, last={last_height}\n{tail(node['log'])}")


def wait_for_peer(node, timeout: float) -> int:
    deadline = time.time() + timeout
    last_count = 0
    while time.time() < deadline:
        last_count = int(rpc_call(node["url"], node["user"], node["password"], "getconnectioncount"))
        if last_count >= 1:
            return last_count
        time.sleep(1)
    raise RuntimeError(f"{node['name']} has no peers, last={last_count}\n{tail(node['log'])}")


def wait_for_address_balance(node, address: str, min_angles: int, timeout: float) -> dict:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = rpc_call(node["url"], node["user"], node["password"], "getaddressbalance", [address])
        if int(last["balance_angles"]) >= min_angles:
            return last
        time.sleep(1)
    raise RuntimeError(f"{node['name']} balance for {address} below {min_angles}: {last}\n{tail(node['log'])}")


def stop_process(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=15)
        return
    except Exception:  # noqa: BLE001
        pass
    try:
        proc.terminate()
        proc.wait(timeout=10)
        return
    except Exception:  # noqa: BLE001
        pass
    proc.kill()
    proc.wait(timeout=10)


def start_node(args, root: Path, name: str, p2p_port: int, rpc_port: int, connect_to: int | None = None):
    node_dir = root / name
    node_dir.mkdir(parents=True, exist_ok=True)
    admin_pass = f"{name}-admin-pass"
    miner_pass = f"{name}-miner-pass"
    admin_file = node_dir / "rpc-admin.pass"
    miner_file = node_dir / "rpc-miner.pass"
    write_secret(admin_file, admin_pass)
    write_secret(miner_file, miner_pass)

    command = [
        str(args.node_binary),
        "--data-dir", str(node_dir / "data"),
        "--p2p-port", str(p2p_port),
        "--rpc-port", str(rpc_port),
        "--rpc",
        "--rpc-user", f"{name}-admin",
        "--rpc-pass-file", str(admin_file),
        "--rpc-miner-user", f"{name}-miner",
        "--rpc-miner-pass-file", str(miner_file),
        "--network", args.network,
        "--activation-manifest", str(args.activation_manifest),
        "--no-auto-connect",
        "--status-interval", "3600",
    ]
    if connect_to is not None:
        command.extend(["--connect", f"127.0.0.1:{connect_to}"])
    if args.debug:
        command.append("--debug")

    log_path = node_dir / "node.log"
    log_file = log_path.open("a", encoding="utf-8")
    env = os.environ.copy()
    env.setdefault("GEOCOIN_ALLOW_PLAINTEXT_WALLET", "1")
    proc = subprocess.Popen(
        command,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        cwd=str(root),
        env=env,
    )
    return {
        "name": name,
        "proc": proc,
        "log_file": log_file,
        "log": log_path,
        "url": f"http://127.0.0.1:{rpc_port}/",
        "user": f"{name}-admin",
        "password": admin_pass,
        "miner_user": f"{name}-miner",
        "miner_pass_file": miner_file,
        "p2p_port": p2p_port,
        "rpc_port": rpc_port,
        "data_dir": node_dir / "data",
    }


def mine_blocks(args, node, address: str, blocks: int) -> None:
    command = [
        str(args.miner_binary),
        "--url", node["url"],
        "--rpc-user", node["miner_user"],
        "--rpc-pass-file", str(node["miner_pass_file"]),
        "--address", address,
        "--blocks", str(blocks),
        "--timeout", str(args.timeout),
    ]
    if args.miner_kind == "gpu":
        command.extend([
            "--backend", args.gpu_backend,
            "--intensity", str(args.gpu_intensity),
            "--max-launch-attempts", str(args.gpu_max_launch_attempts),
            "--stats-json",
        ])
        if args.gpu_devices:
            command.extend(["--devices", args.gpu_devices])
    else:
        command.extend(["--threads", str(args.mining_threads)])

    completed = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=max(args.timeout * blocks + 30, 90),
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{args.miner_kind} miner failed\n"
            f"stdout:\n{completed.stdout[-4000:]}\n"
            f"stderr:\n{completed.stderr[-4000:]}\n"
            f"node log:\n{tail(node['log'])}"
        )


def run_builder(args, *builder_args) -> dict:
    completed = subprocess.run(
        [str(args.package_builder), *builder_args, "--network", args.network],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=60,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"package builder failed: {completed.stderr}")
    return json.loads(completed.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description="Two-node local multisig smoke test")
    parser.add_argument("--node-binary", required=True, type=Path)
    parser.add_argument("--miner-binary", required=True, type=Path)
    parser.add_argument("--miner-kind", choices=("gpu", "rpc-cpu"), default="gpu")
    parser.add_argument("--package-builder", required=True, type=Path)
    parser.add_argument("--activation-manifest", required=True, type=Path)
    parser.add_argument("--network", default="degree-test")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--coinbase-maturity", type=int, default=100)
    parser.add_argument("--mining-threads", type=int, default=1)
    parser.add_argument("--gpu-backend", default="auto", choices=("auto", "cuda", "hip", "opencl"))
    parser.add_argument("--gpu-devices", default="")
    parser.add_argument("--gpu-intensity", type=int, default=16777216)
    parser.add_argument("--gpu-max-launch-attempts", type=int, default=16777216)
    parser.add_argument("--multisig-m", type=int, default=2)
    parser.add_argument("--multisig-n", type=int, default=3)
    parser.add_argument("--keep-data-dir", action="store_true")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    if args.multisig_m <= 0 or args.multisig_n <= 0 or args.multisig_m > args.multisig_n or args.multisig_n > 15:
        parser.error("--multisig-m/--multisig-n must satisfy 1 <= m <= n <= 15")
    if args.coinbase_maturity < 1:
        parser.error("--coinbase-maturity must be >= 1")

    args.node_binary = args.node_binary.resolve()
    args.miner_binary = args.miner_binary.resolve()
    args.package_builder = args.package_builder.resolve()
    args.activation_manifest = args.activation_manifest.resolve()

    temp_dir = None
    if args.keep_data_dir:
        root = Path(tempfile.mkdtemp(prefix="geocoin-multisig-localnet-"))
    else:
        temp_dir = tempfile.TemporaryDirectory(prefix="geocoin-multisig-localnet-")
        root = Path(temp_dir.name)
    nodes = []
    summary = {"root": str(root)}

    try:
        ports = [find_free_port() for _ in range(4)]
        node1 = start_node(args, root, "node1", ports[0], ports[1])
        nodes.append(node1)
        wait_for_rpc(node1, args.timeout)

        node2 = start_node(args, root, "node2", ports[2], ports[3], connect_to=ports[0])
        nodes.append(node2)
        wait_for_rpc(node2, args.timeout)

        try:
            rpc_call(node1["url"], node1["user"], node1["password"], "addnode", [f"127.0.0.1:{ports[2]}", "onetry"])
        except Exception:
            pass
        peers1 = wait_for_peer(node1, args.timeout)
        peers2 = wait_for_peer(node2, args.timeout)

        wallet1 = rpc_call(node1["url"], node1["user"], node1["password"], "createwallet")
        wallet2 = rpc_call(node2["url"], node2["user"], node2["password"], "createwallet")
        mining_address = wallet1["address"]
        recipient = rpc_call(node2["url"], node2["user"], node2["password"], "getnewaddress")

        multisig_info = run_builder(
            args,
            "describe",
            "--m", str(args.multisig_m),
            "--n", str(args.multisig_n),
        )
        multisig_address = multisig_info["multisig_address"]

        initial_height = int(rpc_call(node1["url"], node1["user"], node1["password"], "getblockcount"))
        funding_blocks = max(3, args.coinbase_maturity)
        mine_blocks(args, node1, mining_address, funding_blocks)
        funded_height = wait_for_height(node1, initial_height + funding_blocks, args.timeout)
        wait_for_height(node2, funded_height, args.timeout)

        fund_amount_gc = 12.0
        fund_txid = rpc_call(
            node1["url"],
            node1["user"],
            node1["password"],
            "sendtoaddress",
            [multisig_address, fund_amount_gc, 0.000005],
        )
        mine_blocks(args, node1, mining_address, 1)
        fund_confirm_height = wait_for_height(node1, funded_height + 1, args.timeout)
        wait_for_height(node2, fund_confirm_height, args.timeout)

        fund_angles = 12 * 100000000
        balance1 = wait_for_address_balance(node1, multisig_address, fund_angles, args.timeout)
        balance2 = wait_for_address_balance(node2, multisig_address, fund_angles, args.timeout)

        utxos = rpc_call(node1["url"], node1["user"], node1["password"], "getaddressutxos", [multisig_address])
        if not utxos:
            raise RuntimeError("multisig address has no UTXOs after funding")
        utxos_path = root / "multisig-utxos.json"
        utxos_path.write_text(json.dumps(utxos), encoding="utf-8")

        package = run_builder(
            args,
            "build",
            "--utxos", str(utxos_path),
            "--recipient", recipient,
            "--amount-angles", str(100000000),
            "--fee-angles", str(100000),
            "--m", str(args.multisig_m),
            "--n", str(args.multisig_n),
        )

        commitment_accept = rpc_call(
            node1["url"],
            node1["user"],
            node1["password"],
            "testmempoolaccept",
            [package["commitments"]],
        )
        if not all(item.get("allowed") for item in commitment_accept):
            raise RuntimeError(f"commitments rejected by testmempoolaccept: {commitment_accept}")

        send_result = rpc_call(
            node1["url"],
            node1["user"],
            node1["password"],
            "sendmultisigpackage",
            [{"commitments": package["commitments"], "spend": package["spend"]}],
        )
        mempool = rpc_call(node1["url"], node1["user"], node1["password"], "getrawmempool")
        expected_txids = set(package["commitment_txids"] + [package["spend_txid"]])
        missing = expected_txids.difference(mempool)
        if missing:
            raise RuntimeError(f"multisig package txids missing from mempool: {sorted(missing)}")

        duplicate_rejected = False
        try:
            rpc_call(
                node1["url"],
                node1["user"],
                node1["password"],
                "sendmultisigpackage",
                [{"commitments": package["commitments"], "spend": package["spend"]}],
            )
        except Exception:
            duplicate_rejected = True
        if not duplicate_rejected:
            raise RuntimeError("duplicate multisig package was accepted")

        mine_blocks(args, node1, mining_address, 1)
        spend_height = wait_for_height(node1, fund_confirm_height + 1, args.timeout)
        wait_for_height(node2, spend_height, args.timeout)

        spend_tx1 = rpc_call(node1["url"], node1["user"], node1["password"], "getrawtransaction", [package["spend_txid"], 1])
        spend_tx2 = rpc_call(node2["url"], node2["user"], node2["password"], "getrawtransaction", [package["spend_txid"], 1])
        recipient_balance1 = wait_for_address_balance(node1, recipient, 100000000, args.timeout)
        recipient_balance2 = wait_for_address_balance(node2, recipient, 100000000, args.timeout)
        multisig_after = rpc_call(node1["url"], node1["user"], node1["password"], "getaddressbalance", [multisig_address])

        if int(spend_tx1.get("type")) != 2 or int(spend_tx2.get("type")) != 2:
            raise RuntimeError(f"confirmed tx is not MULTISIG_SPEND: node1={spend_tx1} node2={spend_tx2}")

        summary.update({
            "result": "ok",
            "peers": {"node1": peers1, "node2": peers2},
            "wallets": {"node1": wallet1, "node2": wallet2},
            "multisig": {"m": args.multisig_m, "n": args.multisig_n},
            "mining_address": mining_address,
            "recipient": recipient,
            "multisig_address": multisig_address,
            "fund_txid": fund_txid,
            "spend_txid": package["spend_txid"],
            "commitment_txids": package["commitment_txids"],
            "heights": {
                "initial": initial_height,
                "funded": funded_height,
                "fund_confirmed": fund_confirm_height,
                "spent": spend_height,
            },
            "balances": {
                "multisig_node1_before": balance1,
                "multisig_node2_before": balance2,
                "multisig_node1_after": multisig_after,
                "recipient_node1": recipient_balance1,
                "recipient_node2": recipient_balance2,
            },
            "send_result": send_result,
            "duplicate_rejected": duplicate_rejected,
        })
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except Exception as exc:  # noqa: BLE001
        summary["result"] = "failed"
        summary["error"] = str(exc)
        summary["logs"] = {node["name"]: tail(node["log"]) for node in nodes}
        print(json.dumps(summary, indent=2, sort_keys=True), file=sys.stderr)
        return 1
    finally:
        for node in nodes:
            try:
                rpc_call(node["url"], node["user"], node["password"], "stop")
            except Exception:
                pass
        for node in nodes:
            stop_process(node["proc"])
            node["log_file"].close()
        if args.keep_data_dir:
            print(f"kept data dir: {root}", file=sys.stderr)
        elif temp_dir is not None:
            temp_dir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
