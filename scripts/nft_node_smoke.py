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


COIN = 100000000
# Cover the full serialized NFT and Falcon witnesses at the default relay rate.
TX_FEE_ANGLES = 100000


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


def node_rpc(node, method: str, params=None):
    return rpc_call(node["url"], node["user"], node["password"], method, params)


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
            node_rpc(node, "getblockcount")
            return
        except Exception as exc:  # noqa: BLE001
            last_error = exc
            time.sleep(0.5)
    raise RuntimeError(f"{node['name']} RPC not ready: {last_error}\n{tail(node['log'])}")


def wait_for_peer(node, timeout: float) -> int:
    deadline = time.time() + timeout
    last_count = 0
    while time.time() < deadline:
        last_count = int(node_rpc(node, "getconnectioncount"))
        if last_count >= 1:
            return last_count
        time.sleep(1)
    raise RuntimeError(f"{node['name']} has no peers, last={last_count}\n{tail(node['log'])}")


def wait_for_height(node, height: int, timeout: float) -> int:
    deadline = time.time() + timeout
    last_height = -1
    while time.time() < deadline:
        last_height = int(node_rpc(node, "getblockcount"))
        if last_height >= height:
            return last_height
        time.sleep(1)
    raise RuntimeError(f"{node['name']} did not reach height {height}, last={last_height}\n{tail(node['log'])}")


def wait_for_matching_tip(nodes, height: int, timeout: float) -> str:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        snapshot = []
        hashes = []
        ready = True
        for node in nodes:
            block_count = int(node_rpc(node, "getblockcount"))
            if block_count < height:
                ready = False
                snapshot.append({"name": node["name"], "height": block_count})
                continue
            block_hash = node_rpc(node, "getblockhash", [height])
            hashes.append(block_hash)
            snapshot.append({"name": node["name"], "height": block_count, "hash": block_hash})
        last = snapshot
        if ready and len(set(hashes)) == 1:
            return hashes[0]
        time.sleep(1)
    raise RuntimeError(f"nodes did not agree at height {height}: {last}")


def wait_for_address_balance(node, address: str, min_angles: int, timeout: float) -> dict:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = node_rpc(node, "getaddressbalance", [address])
        if int(last["balance_angles"]) >= min_angles:
            return last
        time.sleep(1)
    raise RuntimeError(f"{node['name']} balance for {address} below {min_angles}: {last}\n{tail(node['log'])}")


def wait_for_wallet_balance(node, min_gc: float, timeout: float) -> float:
    deadline = time.time() + timeout
    last = 0.0
    while time.time() < deadline:
        last = float(node_rpc(node, "getbalance"))
        if last >= min_gc:
            return last
        time.sleep(1)
    raise RuntimeError(f"{node['name']} wallet balance below {min_gc} GC: {last}\n{tail(node['log'])}")


def wait_for_mempool_tx(node, txid: str, timeout: float) -> list:
    deadline = time.time() + timeout
    last = []
    while time.time() < deadline:
        last = node_rpc(node, "getrawmempool")
        if txid in last:
            return last
        time.sleep(0.5)
    raise RuntimeError(f"{node['name']} mempool does not contain {txid}: {last}\n{tail(node['log'])}")


def wait_for_nft_state(nodes, nft_id: str, exists: bool, timeout: float, owner: str | None = None) -> list:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        states = []
        ok = True
        for node in nodes:
            state = node_rpc(node, "getnft", [nft_id])
            states.append({"node": node["name"], "state": state})
            if bool(state.get("exists")) != exists:
                ok = False
            if exists and owner is not None and state.get("owner") != owner:
                ok = False
        last = states
        if ok:
            return states
        time.sleep(1)
    raise RuntimeError(f"NFT {nft_id} did not reach expected state exists={exists}, owner={owner}: {last}")


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
    if blocks <= 0:
        return
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


def confirmed_on_all(nodes, txid: str) -> list:
    out = []
    for node in nodes:
        tx = node_rpc(node, "getrawtransaction", [txid, 1])
        if tx.get("txid") != txid:
            raise RuntimeError(f"{node['name']} returned wrong tx for {txid}: {tx}")
        out.append({"node": node["name"], "type": tx.get("type")})
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Two-node local NFT lifecycle smoke test")
    parser.add_argument("--node-binary", required=True, type=Path)
    parser.add_argument("--miner-binary", required=True, type=Path)
    parser.add_argument("--miner-kind", choices=("gpu", "rpc-cpu"), default="gpu")
    parser.add_argument("--activation-manifest", required=True, type=Path)
    parser.add_argument("--network", default="degree-test")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--coinbase-maturity", type=int, default=100)
    parser.add_argument("--mining-threads", type=int, default=1)
    parser.add_argument("--gpu-backend", default="auto", choices=("auto", "cuda", "hip", "opencl"))
    parser.add_argument("--gpu-devices", default="")
    parser.add_argument("--gpu-intensity", type=int, default=16777216)
    parser.add_argument("--gpu-max-launch-attempts", type=int, default=16777216)
    parser.add_argument("--keep-data-dir", action="store_true")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()

    if args.miner_kind == "rpc-cpu" and args.mining_threads < 1:
        parser.error("--mining-threads must be >= 1")
    if args.coinbase_maturity < 1:
        parser.error("--coinbase-maturity must be >= 1")

    args.node_binary = args.node_binary.resolve()
    args.miner_binary = args.miner_binary.resolve()
    args.activation_manifest = args.activation_manifest.resolve()

    temp_dir = None
    if args.keep_data_dir:
        root = Path(tempfile.mkdtemp(prefix="geocoin-nft-localnet-"))
    else:
        temp_dir = tempfile.TemporaryDirectory(prefix="geocoin-nft-localnet-")
        root = Path(temp_dir.name)
    nodes = []
    summary = {
        "root": str(root),
        "miner_kind": args.miner_kind,
        "mining_threads": args.mining_threads if args.miner_kind == "rpc-cpu" else None,
    }

    try:
        ports = [find_free_port() for _ in range(4)]
        node1 = start_node(args, root, "node1", ports[0], ports[1])
        nodes.append(node1)
        wait_for_rpc(node1, args.timeout)

        node2 = start_node(args, root, "node2", ports[2], ports[3], connect_to=ports[0])
        nodes.append(node2)
        wait_for_rpc(node2, args.timeout)

        try:
            node_rpc(node1, "addnode", [f"127.0.0.1:{ports[2]}", "onetry"])
        except Exception:
            pass
        peers1 = wait_for_peer(node1, args.timeout)
        peers2 = wait_for_peer(node2, args.timeout)

        wallet1 = node_rpc(node1, "createwallet")
        wallet2 = node_rpc(node2, "createwallet")
        owner_address = wallet1["address"]
        recipient_address = wallet2["address"]

        initial_height = int(node_rpc(node1, "getblockcount"))
        funding_blocks = max(3, args.coinbase_maturity)
        mine_blocks(args, node1, owner_address, funding_blocks)
        funded_height = wait_for_height(node1, initial_height + funding_blocks, args.timeout)
        wait_for_matching_tip(nodes, funded_height, args.timeout)
        owner_balance = wait_for_wallet_balance(node1, 1.0, args.timeout)

        fund_txid = node_rpc(node1, "sendtoaddress", [recipient_address, 1.0, TX_FEE_ANGLES / COIN])
        wait_for_mempool_tx(node1, fund_txid, args.timeout)
        mine_blocks(args, node1, owner_address, 1)
        fund_height = wait_for_height(node1, funded_height + 1, args.timeout)
        wait_for_matching_tip(nodes, fund_height, args.timeout)
        recipient_balance = wait_for_address_balance(node2, recipient_address, COIN, args.timeout)

        metadata = {
            "name": "Node smoke NFT",
            "description": "Created, transferred, and burned through real local nodes",
            "image_uri": "ipfs://geocoin-node-smoke-nft",
            "content_hash": "a" * 64,
            "geometry": {"parts": [{"exterior": [
                {"lat_ndeg": 50_450_000_000, "lon_ndeg": 30_520_000_000},
                {"lat_ndeg": 50_450_100_000, "lon_ndeg": 30_520_000_000},
                {"lat_ndeg": 50_450_000_000, "lon_ndeg": 30_520_100_000},
            ], "holes": []}]},
            "location_name": "node-smoke",
            "tags": ["node", "smoke", "nft"],
        }

        create_result = node_rpc(node1, "createnft", [{
            "metadata": metadata,
            "royalty_percent": 250,
            "fee_angles": TX_FEE_ANGLES,
        }])
        nft_id = create_result["nft_id"]
        create_txid = create_result["txid"]
        wait_for_mempool_tx(node1, create_txid, args.timeout)
        mine_blocks(args, node1, owner_address, 1)
        create_height = wait_for_height(node1, fund_height + 1, args.timeout)
        wait_for_matching_tip(nodes, create_height, args.timeout)
        wait_for_nft_state(nodes, nft_id, True, args.timeout, owner=owner_address)

        transfer_result = node_rpc(node1, "transfernft", [{
            "nft_id": nft_id,
            "to": recipient_address,
            "fee_angles": TX_FEE_ANGLES,
        }])
        transfer_txid = transfer_result["txid"]
        wait_for_mempool_tx(node1, transfer_txid, args.timeout)
        mine_blocks(args, node1, owner_address, 1)
        transfer_height = wait_for_height(node1, create_height + 1, args.timeout)
        wait_for_matching_tip(nodes, transfer_height, args.timeout)
        wait_for_nft_state(nodes, nft_id, True, args.timeout, owner=recipient_address)

        burn_result = node_rpc(node2, "burnnft", [{
            "nft_id": nft_id,
            "fee_angles": TX_FEE_ANGLES,
        }])
        burn_txid = burn_result["txid"]
        wait_for_mempool_tx(node2, burn_txid, args.timeout)
        mine_blocks(args, node2, recipient_address, 1)
        burn_height = wait_for_height(node2, transfer_height + 1, args.timeout)
        wait_for_matching_tip(nodes, burn_height, args.timeout)
        wait_for_nft_state(nodes, nft_id, False, args.timeout)

        create_confirmed = confirmed_on_all(nodes, create_txid)
        transfer_confirmed = confirmed_on_all(nodes, transfer_txid)
        burn_confirmed = confirmed_on_all(nodes, burn_txid)
        history1 = node_rpc(node1, "getnftshistory", [nft_id, True])
        history2 = node_rpc(node2, "getnftshistory", [nft_id, True])
        if len(history1) < 3 or len(history2) < 3:
            raise RuntimeError(f"NFT history is incomplete: node1={history1}, node2={history2}")

        summary.update({
            "result": "ok",
            "peers": {"node1": peers1, "node2": peers2},
            "wallets": {"node1": wallet1, "node2": wallet2},
            "addresses": {
                "owner": owner_address,
                "recipient": recipient_address,
            },
            "balances": {
                "owner_wallet_after_initial_mine_gc": owner_balance,
                "recipient_after_funding": recipient_balance,
            },
            "nft_id": nft_id,
            "txids": {
                "fund": fund_txid,
                "create": create_txid,
                "transfer": transfer_txid,
                "burn": burn_txid,
            },
            "confirmed": {
                "create": create_confirmed,
                "transfer": transfer_confirmed,
                "burn": burn_confirmed,
            },
            "history_lengths": {
                "node1": len(history1),
                "node2": len(history2),
            },
            "heights": {
                "initial": initial_height,
                "funded": funded_height,
                "fund": fund_height,
                "create": create_height,
                "transfer": transfer_height,
                "burn": burn_height,
            },
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
                node_rpc(node, "stop")
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
