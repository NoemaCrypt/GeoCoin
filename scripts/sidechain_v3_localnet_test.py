#!/usr/bin/env python3
"""Exhaustive sidechain-v3 test against real local GeoCoin nodes.

The scenario deliberately uses the public JSON-RPC surface, the real mempool,
block production and validation, authenticated P2P sessions, RocksDB restarts,
and a competing fork.  Blocks are submitted to every peer through its public
miner RPC so each live node independently executes full consensus validation;
this keeps sidechain coverage deterministic even when bulk P2P relay is under
separate test.  The only test-only component is ``sidechain_test_tool``, which
owns deterministic Falcon keys and produces the same signatures an external
sidechain signer would produce.
"""

import argparse
import base64
import hashlib
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path


COIN = 100_000_000
MIN_FEE_GC = 0.000005
INPUT_NETWORK_FEE = 50_000
CHECKPOINT_FEE = 5_000
REGISTRATION_FEE = 500
CHALLENGE_BLOCKS = 144
TARGET_BLOCK_SECONDS = 600

FINANCIAL_LOCK = 1_200_000_000
FINANCIAL_TOP_UP = 100_000_000
ACCOUNTING_LOCK = 800_000_000
UNREGISTERED_LOCK = 300_000_000
UNREGISTERED_TOP_UP = 50_000_000
UNREGISTERED_SPEND = 50_000_000
MINT_AMOUNT = 100_000_000
SLASH_AMOUNT = 50_000_000
FINANCIAL_FEE_BPS = 100


class RpcError(RuntimeError):
    def __init__(self, method: str, error: dict):
        self.method = method
        self.code = error.get("code")
        self.message = str(error.get("message", ""))
        super().__init__(f"{method} failed ({self.code}): {self.message}")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def record_coverage(coverage: list[str], label: str) -> None:
    coverage.append(label)
    print(f"[sidechain-v3] passed: {label}", flush=True)


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def write_secret(path: Path, value: str) -> None:
    path.write_text(value + "\n", encoding="utf-8")
    if os.name != "nt":
        path.chmod(0o600)


def tail(path: Path, lines: int = 160) -> str:
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
        headers={"Authorization": f"Basic {token}", "Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError as decode_error:
            raise RuntimeError(f"{method} HTTP {exc.code}: {raw}") from decode_error
    if payload.get("error"):
        raise RpcError(method, payload["error"])
    return payload.get("result")


def node_rpc(node: dict, method: str, params=None):
    return rpc_call(node["url"], node["user"], node["password"], method, params)


def node_miner_rpc(node: dict, method: str, params=None):
    return rpc_call(
        node["url"], node["miner_user"], node["miner_password"], method, params)


def expect_rpc_error(node: dict, method: str, params, contains: str | None = None) -> RpcError:
    try:
        node_rpc(node, method, params)
    except RpcError as exc:
        if contains is not None:
            check(contains.lower() in exc.message.lower(),
                  f"{method} error does not contain {contains!r}: {exc}")
        return exc
    raise AssertionError(f"{method} unexpectedly succeeded")


def wait_for_rpc(node: dict, timeout: float) -> None:
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
            time.sleep(0.35)
    raise RuntimeError(f"{node['name']} RPC not ready: {last_error}\n{tail(node['log'])}")


def wait_for_peer(node: dict, timeout: float) -> None:
    deadline = time.time() + timeout
    last_count = 0
    while time.time() < deadline:
        last_count = int(node_rpc(node, "getconnectioncount"))
        if last_count >= 1:
            return
        time.sleep(0.5)
    raise RuntimeError(f"{node['name']} has no peers, last={last_count}\n{tail(node['log'])}")


def wait_for_height(node: dict, height: int, timeout: float) -> int:
    deadline = time.time() + timeout
    last_height = -1
    while time.time() < deadline:
        last_height = int(node_rpc(node, "getblockcount"))
        if last_height >= height:
            return last_height
        time.sleep(0.5)
    raise RuntimeError(f"{node['name']} did not reach height {height}, last={last_height}\n{tail(node['log'])}")


def wait_for_matching_tip(nodes: list[dict], min_height: int, timeout: float) -> str:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        snapshot = []
        hashes = []
        ready = True
        for node in nodes:
            height = int(node_rpc(node, "getblockcount"))
            if height < min_height:
                ready = False
                snapshot.append({"node": node["name"], "height": height})
                continue
            block_hash = node_rpc(node, "getblockhash", [height])
            hashes.append((height, block_hash))
            snapshot.append({"node": node["name"], "height": height, "hash": block_hash})
        last = snapshot
        if ready and len(set(hashes)) == 1:
            return hashes[0][1]
        time.sleep(0.5)
    raise RuntimeError(f"nodes did not converge at >= {min_height}: {last}")


def wait_for_mempool_tx(node: dict, txid: str, timeout: float) -> None:
    deadline = time.time() + timeout
    last = []
    while time.time() < deadline:
        last = node_rpc(node, "getrawmempool")
        if txid in last:
            return
        time.sleep(0.25)
    raise RuntimeError(f"{node['name']} mempool lacks {txid}: {last}\n{tail(node['log'])}")


def stop_process(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=20)
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


def make_node(root: Path, name: str, p2p_port: int, rpc_port: int) -> dict:
    node_dir = root / name
    node_dir.mkdir(parents=True, exist_ok=True)
    admin_pass = f"{name}-admin-pass"
    miner_pass = f"{name}-miner-pass"
    admin_file = node_dir / "rpc-admin.pass"
    miner_file = node_dir / "rpc-miner.pass"
    write_secret(admin_file, admin_pass)
    write_secret(miner_file, miner_pass)
    return {
        "name": name,
        "proc": None,
        "log_file": None,
        "log": node_dir / "node.log",
        "url": f"http://127.0.0.1:{rpc_port}/",
        "user": f"{name}-admin",
        "password": admin_pass,
        "admin_file": admin_file,
        "miner_user": f"{name}-miner",
        "miner_password": miner_pass,
        "miner_pass_file": miner_file,
        "p2p_port": p2p_port,
        "rpc_port": rpc_port,
        "data_dir": node_dir / "data",
    }


def launch_node(args, root: Path, node: dict, connect_to: int | None = None) -> None:
    command = [
        str(args.node_binary),
        "--data-dir", str(node["data_dir"]),
        "--p2p-port", str(node["p2p_port"]),
        "--rpc-port", str(node["rpc_port"]),
        "--rpc",
        "--rpc-user", node["user"],
        "--rpc-pass-file", str(node["admin_file"]),
        "--rpc-miner-user", node["miner_user"],
        "--rpc-miner-pass-file", str(node["miner_pass_file"]),
        "--network", args.network,
        "--activation-manifest", str(args.activation_manifest),
        "--no-auto-connect",
        "--archive",
        "--status-interval", "3600",
    ]
    if connect_to is not None:
        command.extend(["--connect", f"127.0.0.1:{connect_to}"])
    if args.debug:
        command.append("--debug")
    log_file = node["log"].open("a", encoding="utf-8")
    env = os.environ.copy()
    env.setdefault("GEOCOIN_ALLOW_PLAINTEXT_WALLET", "1")
    env["GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW"] = "0"
    node["log_file"] = log_file
    node["proc"] = subprocess.Popen(
        command,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        cwd=str(root),
        env=env,
    )
    wait_for_rpc(node, args.timeout)


def stop_node(node: dict) -> None:
    if node.get("proc") is not None:
        stop_process(node["proc"])
    if node.get("log_file") is not None:
        node["log_file"].close()
        node["log_file"] = None


def connect_nodes(a: dict, b: dict, timeout: float) -> None:
    try:
        node_rpc(a, "addnode", [f"127.0.0.1:{b['p2p_port']}", "onetry"])
    except RpcError:
        pass
    wait_for_peer(a, timeout)
    wait_for_peer(b, timeout)


def ensure_connected(source: dict, peers: list[dict], timeout: float) -> None:
    for peer in peers:
        connect_nodes(source, peer, timeout)


def mine_blocks(args, node: dict, address: str, blocks: int) -> None:
    if blocks <= 0:
        return
    tip_height = int(node_rpc(node, "getblockcount"))
    tip = node_rpc(node, "getblockbyheight", [tip_height])
    first_timestamp = int(tip["time"]) + TARGET_BLOCK_SECONDS
    command = [
        str(args.miner_binary),
        "--url", node["url"],
        "--rpc-user", node["miner_user"],
        "--rpc-pass-file", str(node["miner_pass_file"]),
        "--address", address,
        "--blocks", str(blocks),
        "--timeout", str(args.timeout),
        "--threads", str(args.mining_threads),
        "--timestamp", str(first_timestamp),
        "--timestamp-step", str(TARGET_BLOCK_SECONDS),
    ]
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
            f"CPU miner failed for {node['name']}\nstdout:\n{completed.stdout[-5000:]}\n"
            f"stderr:\n{completed.stderr[-5000:]}\nnode log:\n{tail(node['log'])}"
        )


def submit_active_block_range(source: dict, target: dict, first_height: int,
                              last_height: int) -> None:
    for height in range(first_height, last_height + 1):
        source_hash = node_rpc(source, "getblockhash", [height])
        target_height = int(node_rpc(target, "getblockcount"))
        if target_height >= height:
            try:
                if node_rpc(target, "getblockhash", [height]) == source_hash:
                    continue
            except RpcError:
                pass
        raw_block = node_rpc(source, "getblockbyheight", [height, False])
        result = node_miner_rpc(target, "submitblock", [raw_block])
        if result is not None:
            target_height = int(node_rpc(target, "getblockcount"))
            active_matches = (
                target_height >= height
                and node_rpc(target, "getblockhash", [height]) == source_hash
            )
            check(active_matches, f"submitblock height {height} rejected by {target['name']}: {result}")


def mine_synced_blocks(args, node: dict, peers: list[dict], address: str, blocks: int) -> int:
    """Mine real blocks and make every live node validate them via submitblock."""
    ensure_connected(node, peers, args.timeout)
    first_height = int(node_rpc(node, "getblockcount")) + 1
    mine_blocks(args, node, address, blocks)
    final_height = first_height + blocks - 1
    for peer in peers:
        submit_active_block_range(node, peer, first_height, final_height)
    wait_for_matching_tip([node, *peers], final_height, args.timeout)
    return final_height


def tool_json(args, *tool_args: str) -> dict:
    completed = subprocess.run(
        [str(args.signing_tool), *tool_args],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=60,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"sidechain_test_tool failed: {' '.join(tool_args[:2])}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return json.loads(completed.stdout)


def key_info(args, seed: int) -> dict:
    return tool_json(args, "key-info", "--seed", str(seed))


def payload_signature(args, key: dict, payload_hex: str) -> str:
    result = tool_json(
        args,
        "sign-payload",
        "--private-key-hex", key["private_key_hex"],
        "--payload-hex", payload_hex,
    )
    check(result["signer"] == key["address"], "payload signature recovered a wrong signer")
    return result["signature_hex"]


def sign_inputs(args, key: dict, tx_hex: str) -> dict:
    return tool_json(
        args,
        "sign-inputs",
        "--private-key-hex", key["private_key_hex"],
        "--tx-hex", tx_hex,
    )


def owner_transaction(
    args,
    node: dict,
    create_method: str,
    request: dict,
    finalize_method: str,
    owner_key: dict,
    with_inputs: bool = False,
) -> tuple[dict, dict]:
    created = node_rpc(node, create_method, [request])
    signature = payload_signature(args, owner_key, created["payload_to_sign_hex"])
    finalized = node_rpc(node, finalize_method, [created["tx_hex"], signature])
    if with_inputs:
        finalized = sign_inputs(args, owner_key, finalized["tx_hex"])
    return created, finalized


def decommission_transaction(
    args,
    node: dict,
    request: dict,
    owner_key: dict,
    committee_keys: list[dict],
) -> tuple[dict, dict]:
    created = node_rpc(node, "createsidechaindecommissionpayload", [request])
    payload = created["payload_to_sign_hex"]
    owner_signature = payload_signature(args, owner_key, payload)
    committee_signatures = [payload_signature(args, key, payload) for key in committee_keys]
    finalized = node_rpc(
        node,
        "finalizesidechaindecommission",
        [created["tx_hex"], owner_signature, committee_signatures],
    )
    return created, finalized


def committee_transaction(
    args,
    node: dict,
    create_method: str,
    request: dict,
    finalize_method: str,
    signing_keys: list[dict],
) -> tuple[dict, dict]:
    created = node_rpc(node, create_method, [request])
    signatures = [payload_signature(args, key, created["payload_to_sign_hex"])
                  for key in signing_keys]
    finalized = node_rpc(node, finalize_method, [created["tx_hex"], signatures])
    return created, finalized


def batch_transaction(
    args,
    node: dict,
    items: list[dict],
    item_signing_keys: list[list[dict]],
) -> tuple[dict, dict]:
    created = node_rpc(node, "createbatchsidechaincheckpointpayload", [{"items": items}])
    check(len(created["payloads"]) == len(item_signing_keys), "batch payload count mismatch")
    signatures = []
    for payload, keys in zip(created["payloads"], item_signing_keys):
        signatures.append([payload_signature(args, key, payload["payload_to_sign_hex"])
                           for key in keys])
    finalized = node_rpc(
        node,
        "finalizebatchsidechaincheckpoint",
        [created["tx_hex"], signatures],
    )
    return created, finalized


def preflight(node: dict, tx_hex: str) -> dict:
    result = node_rpc(node, "testmempoolaccept", [[tx_hex]])
    check(isinstance(result, list) and len(result) == 1, f"bad testmempoolaccept result: {result}")
    return result[0]


def assert_preflight_allowed(node: dict, tx_hex: str, label: str) -> str:
    result = preflight(node, tx_hex)
    check(result.get("allowed") is True, f"{label} rejected: {result}")
    return result["txid"]


def assert_preflight_rejected(node: dict, tx_hex: str, label: str) -> dict:
    result = preflight(node, tx_hex)
    check(result.get("allowed") is False, f"{label} unexpectedly allowed: {result}")
    check(bool(result.get("reject_reason")), f"{label} rejection lacks reason: {result}")
    return result


def submit_and_confirm(
    args,
    node: dict,
    peers: list[dict],
    mining_address: str,
    tx_hex: str,
    label: str,
    confirmed_txids: list[str],
    blocks: int = 1,
) -> str:
    ensure_connected(node, peers, args.timeout)
    expected_txid = assert_preflight_allowed(node, tx_hex, label)
    txid = node_rpc(node, "sendrawtransaction", [tx_hex])
    check(txid == expected_txid, f"{label} sendrawtransaction txid mismatch")
    wait_for_mempool_tx(node, txid, args.timeout)
    before = int(node_rpc(node, "getblockcount"))
    mine_blocks(args, node, mining_address, blocks)
    height = before + blocks
    for peer in peers:
        submit_active_block_range(node, peer, before + 1, height)
    wait_for_matching_tip([node, *peers], height, args.timeout)
    for peer in [node, *peers]:
        verbose = node_rpc(peer, "getrawtransaction", [txid, 1])
        check(verbose.get("txid") == txid, f"{peer['name']} cannot read confirmed {label}")
    confirmed_txids.append(txid)
    return txid


def send_wallet_funding(args, node: dict, peers: list[dict], mining_address: str,
                        destination: str, amount_gc: float) -> str:
    ensure_connected(node, peers, args.timeout)
    txid = node_rpc(node, "sendtoaddress", [destination, amount_gc, MIN_FEE_GC])
    wait_for_mempool_tx(node, txid, args.timeout)
    before = int(node_rpc(node, "getblockcount"))
    mine_blocks(args, node, mining_address, 1)
    for peer in peers:
        submit_active_block_range(node, peer, before + 1, before + 1)
    wait_for_matching_tip([node, *peers], before + 1, args.timeout)
    return txid


def select_owner_utxo(node: dict, owner: str, required: int) -> dict:
    utxos = node_rpc(node, "getaddressutxos", [owner])
    candidates = [u for u in utxos if int(u["value"]) >= required]
    check(bool(candidates), f"no UTXO for {owner} covers {required}: {utxos}")
    return min(candidates, key=lambda item: (int(item["value"]), item["txid"], int(item["vout"])))


def funded_deposit_request(node: dict, sidechain_id: str, owner: str, amount: int,
                           unlock_height: int | None = None) -> dict:
    utxo = select_owner_utxo(node, owner, amount + INPUT_NETWORK_FEE)
    change = int(utxo["value"]) - amount - INPUT_NETWORK_FEE
    check(change >= 1_000, "deposit change would be dust")
    request = {
        "genesis_hash": sidechain_id,
        "owner_address": owner,
        "amount": amount,
        "inputs": [{"txid": utxo["txid"], "vout": int(utxo["vout"])}],
        "outputs": [{"address": owner, "value": change}],
        "validate": True,
    }
    if unlock_height is not None:
        request["unlock_height"] = unlock_height
    return request


def double_sha256(data: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def financial_checkpoint_proof(node: dict, sidechain_id: str, checkpoint_height: int,
                               total_supply: int, salt: str) -> dict:
    leaf = double_sha256(
        b"geocoin:sidechain:v2:supply_leaf"
        + bytes.fromhex(sidechain_id)
        + struct.pack("<Q", checkpoint_height)
        + struct.pack("<Q", total_supply)
    )
    sibling = hashlib.sha256(("sibling:" + salt).encode("utf-8")).digest()
    state_root = double_sha256(leaf + sibling)
    proof = node_rpc(node, "createcheckpointproofv2", [{
        "sidechain_id": sidechain_id,
        "state_root": state_root.hex(),
        "checkpoint_height": checkpoint_height,
        "total_supply": total_supply,
        "leaf_index": 0,
        "merkle_path": [sibling.hex()],
        "verify": True,
    }])
    check(proof["proof_hex"].startswith("53435032"), "financial proof lacks SCP2 magic")
    return proof


def accounting_checkpoint_proof(node: dict, sidechain_id: str, checkpoint_height: int,
                                schema_hash: str, salt: str) -> dict:
    state_root = hashlib.sha256(("accounting:" + salt).encode("utf-8")).hexdigest()
    proof = node_rpc(node, "createaccountingcheckpointproofv1", [{
        "sidechain_id": sidechain_id,
        "state_root": state_root,
        "checkpoint_height": checkpoint_height,
        "schema_hash": schema_hash,
    }])
    check(proof["proof_hex"].startswith("53434131"), "accounting proof lacks SCA1 magic")
    check(int(proof["proof_size"]) == 80, "accounting proof must be exactly 80 bytes")
    return proof


def checkpoint_item(sidechain_id: str, proof: dict, fee: int = CHECKPOINT_FEE,
                    version: int | None = None) -> dict:
    item = {
        "sidechain_id": sidechain_id,
        "checkpoint": proof["checkpoint"],
        "checkpoint_height": int(proof["checkpoint_height"]),
        "fee": fee,
        "proof_hex": proof["proof_hex"],
    }
    if version is not None:
        item["version"] = version
    return item


def assert_registry(node: dict, sidechain_id: str, expected: dict) -> dict:
    state = node_rpc(node, "getsidechaingenesis", [sidechain_id])
    for field, value in expected.items():
        check(state.get(field) == value,
              f"registry {sidechain_id} field {field}: got {state.get(field)!r}, expected {value!r}")
    return state


def restart_pair(args, root: Path, node1: dict, node2: dict) -> None:
    stop_node(node1)
    stop_node(node2)
    launch_node(args, root, node1)
    launch_node(args, root, node2)
    connect_nodes(node1, node2, args.timeout)
    height = max(int(node_rpc(node1, "getblockcount")), int(node_rpc(node2, "getblockcount")))
    wait_for_matching_tip([node1, node2], height, args.timeout)


def main() -> int:
    parser = argparse.ArgumentParser(description="Exhaustive live-node sidechain-v3 localnet test")
    parser.add_argument("--node-binary", required=True, type=Path)
    parser.add_argument("--miner-binary", required=True, type=Path)
    parser.add_argument("--signing-tool", required=True, type=Path)
    parser.add_argument("--activation-manifest", required=True, type=Path)
    parser.add_argument("--network", default="degree-test")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--coinbase-maturity", type=int, default=100)
    parser.add_argument("--mining-threads", type=int, default=1)
    parser.add_argument("--keep-data-dir", action="store_true")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()

    if args.mining_threads < 1:
        parser.error("--mining-threads must be >= 1")
    if args.coinbase_maturity < 1:
        parser.error("--coinbase-maturity must be >= 1")
    args.node_binary = args.node_binary.resolve()
    args.miner_binary = args.miner_binary.resolve()
    args.signing_tool = args.signing_tool.resolve()
    args.activation_manifest = args.activation_manifest.resolve()
    for binary in (args.node_binary, args.miner_binary, args.signing_tool):
        if not binary.is_file():
            parser.error(f"missing binary: {binary}")

    temp_dir = None
    if args.keep_data_dir:
        root = Path(tempfile.mkdtemp(prefix="geocoin-sidechain-v3-localnet-"))
    else:
        temp_dir = tempfile.TemporaryDirectory(prefix="geocoin-sidechain-v3-localnet-")
        root = Path(temp_dir.name)

    ports = [find_free_port() for _ in range(6)]
    node1 = make_node(root, "node1", ports[0], ports[1])
    node2 = make_node(root, "node2", ports[2], ports[3])
    node3 = make_node(root, "fork-node", ports[4], ports[5])
    all_nodes = [node1, node2, node3]
    confirmed_txids: list[str] = []
    coverage: list[str] = []
    summary = {
        "result": "failed",
        "root": str(root),
        "network": args.network,
        "mining_threads": args.mining_threads,
        "coverage": coverage,
        "transactions": confirmed_txids,
    }

    try:
        owner_fin = key_info(args, 10)
        owner_acc = key_info(args, 11)
        owner_unreg = key_info(args, 12)
        committee_fin_old = [key_info(args, seed) for seed in (40, 41, 42)]
        committee_fin_new = [key_info(args, seed) for seed in (50, 51, 52)]
        committee_acc = [key_info(args, seed) for seed in (60, 61, 62)]

        launch_node(args, root, node1)
        launch_node(args, root, node2)
        connect_nodes(node1, node2, args.timeout)
        launch_node(args, root, node3)
        connect_nodes(node1, node3, args.timeout)
        cluster_peers = [node2, node3]

        wallet1 = node_rpc(node1, "createwallet")
        wallet2 = node_rpc(node2, "createwallet")
        mining_address = wallet1["address"]
        recipient_address = wallet2["address"]

        initial_height = int(node_rpc(node1, "getblockcount"))
        mature_height = initial_height + args.coinbase_maturity
        mine_synced_blocks(
            args, node1, cluster_peers, mining_address, args.coinbase_maturity)
        wait_for_matching_tip([node1, *cluster_peers], mature_height, args.timeout)
        check(float(node_rpc(node1, "getbalance")) > 20.0, "mining wallet did not mature")

        for owner in (owner_fin, owner_acc, owner_unreg):
            send_wallet_funding(args, node1, cluster_peers, mining_address, owner["address"], 20.0)
        record_coverage(coverage, "real wallet funding and mature UTXOs")

        schema_hash = hashlib.sha256(b"geocoin-sidechain-v3-accounting-schema").hexdigest()
        wrong_schema_hash = hashlib.sha256(b"wrong-accounting-schema").hexdigest()
        fin_genesis = node_rpc(node1, "createsidechaingenesis", [{
            "version": 1,
            "timestamp": 1_700_000_001,
            "nonce": 101,
            "chain_name": "financial-live-v3",
            "meta": "live-node exhaustive test",
            "sidechain_kind": "financial",
        }])
        acc_genesis = node_rpc(node1, "createsidechaingenesis", [{
            "version": 2,
            "timestamp": 1_700_000_002,
            "nonce": 102,
            "chain_name": "accounting-live-v3",
            "meta": "live-node exhaustive test",
            "sidechain_kind": "accounting",
            "schema_hash": schema_hash,
        }])
        unreg_genesis = node_rpc(node1, "createsidechaingenesis", [{
            "version": 1,
            "timestamp": 1_700_000_003,
            "nonce": 103,
            "chain_name": "unregistered-deposit",
            "sidechain_kind": "financial",
        }])
        fin_id = fin_genesis["genesis_hash"]
        acc_id = acc_genesis["genesis_hash"]
        unreg_id = unreg_genesis["genesis_hash"]
        check(len({fin_id, acc_id, unreg_id}) == 3, "genesis IDs must be unique")
        record_coverage(coverage, "financial/accounting genesis profiles")

        unlock_height = int(node_rpc(node1, "getblockcount")) + 2
        deposits = [
            ("financial lock", fin_id, owner_fin, FINANCIAL_LOCK),
            ("accounting lock", acc_id, owner_acc, ACCOUNTING_LOCK),
            ("unregistered lock", unreg_id, owner_unreg, UNREGISTERED_LOCK),
        ]
        for label, sidechain_id, owner, amount in deposits:
            request = funded_deposit_request(
                node1, sidechain_id, owner["address"], amount, unlock_height)
            _, finalized = owner_transaction(
                args, node1, "createsidechainlockpayload", request,
                "finalizesidechainlock", owner, with_inputs=True)
            submit_and_confirm(
                args, node1, cluster_peers, mining_address, finalized["tx_hex"],
                label, confirmed_txids)
        record_coverage(coverage, "LOCK_DEPOSIT owner and UTXO witness signatures")

        unreg_topup_request = funded_deposit_request(
            node1, unreg_id, owner_unreg["address"], UNREGISTERED_TOP_UP)
        _, unreg_topup = owner_transaction(
            args, node1, "createsidechaintopuppayload", unreg_topup_request,
            "finalizesidechaintopup", owner_unreg, with_inputs=True)
        submit_and_confirm(
            args, node1, cluster_peers, mining_address, unreg_topup["tx_hex"],
            "unregistered top-up", confirmed_txids)

        unreg_spend_request = {
            "sidechain_id": unreg_id,
            "owner_address": owner_unreg["address"],
            "amount": UNREGISTERED_SPEND,
            "outputs": [{"address": recipient_address, "value": UNREGISTERED_SPEND}],
            "validate": True,
        }
        _, unreg_spend = owner_transaction(
            args, node1, "createsidechainspendpayload", unreg_spend_request,
            "finalizesidechainspend", owner_unreg)
        submit_and_confirm(
            args, node1, cluster_peers, mining_address, unreg_spend["tx_hex"],
            "unregistered partial spend", confirmed_txids)

        unreg_remaining = UNREGISTERED_LOCK + UNREGISTERED_TOP_UP - UNREGISTERED_SPEND
        unreg_unlock_request = {
            "sidechain_id": unreg_id,
            "owner_address": owner_unreg["address"],
            "outputs": [{"address": recipient_address, "value": unreg_remaining}],
            "validate": True,
        }
        _, unreg_unlock = owner_transaction(
            args, node1, "createsidechainunlockpayload", unreg_unlock_request,
            "finalizesidechainunlock", owner_unreg)
        submit_and_confirm(
            args, node1, cluster_peers, mining_address, unreg_unlock["tx_hex"],
            "unregistered final unlock", confirmed_txids)
        expect_rpc_error(node1, "createsidechainspendpayload", [unreg_spend_request], "Депозит")
        record_coverage(coverage, "TOP_UP/SPEND/UNLOCK unregistered deposit lifecycle")

        fin_issue_request = {
            "genesis_hash": fin_id,
            "owner_address": owner_fin["address"],
            "token_symbol": "LIV3",
            "multiplier": 100,
            "fee_bps": FINANCIAL_FEE_BPS,
            "max_multiplier": 200,
            "committee_m": 2,
            "committee_members": [key["address"] for key in committee_fin_old],
            "validate": True,
        }
        fin_issue_created, fin_issue = owner_transaction(
            args, node1, "createsidechainissuepayload", fin_issue_request,
            "finalizesidechainissue", owner_fin)
        submit_and_confirm(
            args, node1, cluster_peers, mining_address, fin_issue["tx_hex"],
            "financial registration", confirmed_txids)

        acc_issue_request = {
            "genesis_hash": acc_id,
            "owner_address": owner_acc["address"],
            "namespace": "land.registry.v1",
            "schema_hash": schema_hash,
            "committee_m": 2,
            "committee_members": [key["address"] for key in committee_acc],
            "validate": True,
        }
        acc_issue_created, acc_issue = owner_transaction(
            args, node1, "createaccountingsidechainpayload", acc_issue_request,
            "finalizeaccountingsidechain", owner_acc)
        submit_and_confirm(
            args, node1, cluster_peers, mining_address, acc_issue["tx_hex"],
            "accounting registration", confirmed_txids)

        fin_state = assert_registry(node1, fin_id, {
            "sidechain_kind": "financial",
            "status": "active",
            "owner": owner_fin["address"],
            "checkpoint_policy_id": 2,
            "checkpoint_version": 2,
            "token_symbol": "LIV3",
            "total_issued": 0,
            "multiplier": 100,
            "fee_bps": FINANCIAL_FEE_BPS,
            "max_multiplier": 200,
        })
        check(int(fin_state["issuance_limit"]) > MINT_AMOUNT, "financial issuance limit too small")
        assert_registry(node2, acc_id, {
            "sidechain_kind": "accounting",
            "status": "active",
            "owner": owner_acc["address"],
            "checkpoint_policy_id": 3,
            "checkpoint_version": 1,
            "namespace": "land.registry.v1",
            "schema_hash": schema_hash,
        })
        check(fin_issue_created["checkpoint_policy_id"] == 2, "financial RPC policy mismatch")
        check(acc_issue_created["checkpoint_policy_id"] == 3, "accounting RPC policy mismatch")
        record_coverage(coverage, "separate ISSUE_SIDECHAIN and REGISTER_ACCOUNTING_SIDECHAIN")

        active_topup_request = funded_deposit_request(
            node1, fin_id, owner_fin["address"], FINANCIAL_TOP_UP)
        _, active_topup = owner_transaction(
            args, node1, "createsidechaintopuppayload", active_topup_request,
            "finalizesidechaintopup", owner_fin, with_inputs=True)
        submit_and_confirm(
            args, node1, cluster_peers, mining_address, active_topup["tx_hex"],
            "active registered top-up", confirmed_txids)
        record_coverage(coverage, "TOP_UP_SIDECHAIN_DEPOSIT while active")

        expect_rpc_error(node1, "createsidechainmintpayload", [{
            "sidechain_id": acc_id,
            "owner_address": owner_acc["address"],
            "amount": 1,
            "validate": True,
        }], "financial")
        expect_rpc_error(node1, "createsidechainredeempayload", [{
            "sidechain_id": acc_id,
            "owner_address": owner_acc["address"],
            "amount": 1,
            "outputs": [{"address": recipient_address, "value": 1}],
            "validate": True,
        }], "financial")
        record_coverage(coverage, "accounting profile rejects mint and redeem")

        mint_request = {
            "sidechain_id": fin_id,
            "owner_address": owner_fin["address"],
            "amount": MINT_AMOUNT,
            "validate": True,
        }
        _, mint_tx = committee_transaction(
            args, node1, "createsidechainmintpayload", mint_request,
            "finalizesidechainmint", committee_fin_old[:2])
        submit_and_confirm(
            args, node1, cluster_peers, mining_address, mint_tx["tx_hex"],
            "financial mint", confirmed_txids)
        assert_registry(node2, fin_id, {"total_issued": MINT_AMOUNT})
        record_coverage(coverage, "financial MINT and deposit-backed issuance accounting")

        fin_proof = financial_checkpoint_proof(node1, fin_id, 1, MINT_AMOUNT, "mixed-batch")
        acc_proof = accounting_checkpoint_proof(node1, acc_id, 1, schema_hash, "mixed-batch")

        wrong_acc_proof = accounting_checkpoint_proof(
            node1, acc_id, 1, wrong_schema_hash, "wrong-schema")
        _, wrong_schema_batch = batch_transaction(
            args, node1,
            [checkpoint_item(acc_id, wrong_acc_proof)],
            [committee_acc[:2]],
        )
        assert_preflight_rejected(node1, wrong_schema_batch["tx_hex"], "wrong accounting schema")

        expect_rpc_error(node1, "createbatchsidechaincheckpointpayload", [{"items": [
            checkpoint_item(acc_id, acc_proof, version=2)
        ]}], "version")

        legacy_created = node_rpc(node1, "createsidechaincheckpointpayload", [{
            "sidechain_id": fin_id,
            "owner_address": owner_fin["address"],
            "checkpoint": hashlib.sha256(b"legacy-checkpoint").hexdigest(),
            "checkpoint_height": 1,
            "fee": CHECKPOINT_FEE,
            "validate": True,
        }])
        legacy_signatures = [payload_signature(args, key, legacy_created["payload_to_sign_hex"])
                             for key in committee_fin_old[:2]]
        legacy_final = node_rpc(
            node1, "finalizesidechaincheckpoint",
            [legacy_created["tx_hex"], legacy_signatures])
        assert_preflight_rejected(node1, legacy_final["tx_hex"], "legacy single checkpoint")

        _, mixed_batch = batch_transaction(
            args, node1,
            [checkpoint_item(fin_id, fin_proof), checkpoint_item(acc_id, acc_proof)],
            [committee_fin_old[:2], committee_acc[:2]],
        )
        mixed_checkpoint_height = int(node_rpc(node1, "getblockcount")) + 1
        submit_and_confirm(
            args, node1, cluster_peers, mining_address, mixed_batch["tx_hex"],
            "mixed financial/accounting checkpoint batch", confirmed_txids)

        too_frequent_proof = accounting_checkpoint_proof(
            node1, acc_id, 2, schema_hash, "too-frequent")
        expect_rpc_error(node1, "createbatchsidechaincheckpointpayload", [{"items": [
            checkpoint_item(acc_id, too_frequent_proof)
        ]}], "частый")
        record_coverage(coverage, "SCP2/SCA1 mixed batch, schema/version/legacy/interval rejection")

        update_request = {
            "sidechain_id": fin_id,
            "owner_address": owner_fin["address"],
            "committee_m": 2,
            "committee_members": [key["address"] for key in committee_fin_new],
            "validate": True,
        }
        update_created, update_tx = committee_transaction(
            args, node1, "createsidechaincommitteeupdatepayload", update_request,
            "finalizesidechaincommitteeupdate", committee_fin_old[:2])
        submit_and_confirm(
            args, node1, cluster_peers, mining_address, update_tx["tx_hex"],
            "committee rotation", confirmed_txids)

        redeem_request = {
            "sidechain_id": fin_id,
            "owner_address": owner_fin["address"],
            "amount": MINT_AMOUNT,
            "outputs": [{"address": recipient_address, "value": MINT_AMOUNT}],
            "validate": True,
        }
        redeem_created = node_rpc(node1, "createsidechainredeempayload", [redeem_request])
        old_signatures = [payload_signature(args, key, redeem_created["payload_to_sign_hex"])
                          for key in committee_fin_old[:2]]
        old_redeem = node_rpc(
            node1, "finalizesidechainredeem", [redeem_created["tx_hex"], old_signatures])
        assert_preflight_rejected(node1, old_redeem["tx_hex"], "old committee after rotation")
        new_signatures = [payload_signature(args, key, redeem_created["payload_to_sign_hex"])
                          for key in committee_fin_new[:2]]
        new_redeem = node_rpc(
            node1, "finalizesidechainredeem", [redeem_created["tx_hex"], new_signatures])
        submit_and_confirm(
            args, node1, cluster_peers, mining_address, new_redeem["tx_hex"],
            "redeem with rotated committee", confirmed_txids)
        assert_registry(node1, fin_id, {"total_issued": 0})
        check(update_created["committee_id"] == redeem_created["committee_id"],
              "redeem did not bind to rotated committee")
        record_coverage(coverage, "committee rotation invalidates old signatures; financial REDEEM")

        restart_pair(args, root, node1, node2)
        connect_nodes(node1, node3, args.timeout)
        wait_for_matching_tip(
            [node1, node2, node3], int(node_rpc(node1, "getblockcount")), args.timeout)
        assert_registry(node1, fin_id, {
            "sidechain_kind": "financial", "status": "active", "total_issued": 0,
        })
        assert_registry(node2, acc_id, {
            "sidechain_kind": "accounting", "status": "active", "schema_hash": schema_hash,
        })
        record_coverage(coverage, "RocksDB restart persistence for both profiles")

        current_height = int(node_rpc(node1, "getblockcount"))
        required_base = mixed_checkpoint_height + 4
        if current_height < required_base:
            mine_synced_blocks(
                args, node1, cluster_peers, mining_address, required_base - current_height)
            wait_for_matching_tip([node1, node2, node3], required_base, args.timeout)
        fork_base_height = int(node_rpc(node1, "getblockcount"))
        fork_base_hash = node_rpc(node1, "getblockhash", [fork_base_height])

        wait_for_matching_tip([node1, node2, node3], fork_base_height, args.timeout)
        stop_node(node3)

        acc_proof_a = accounting_checkpoint_proof(node1, acc_id, 2, schema_hash, "fork-A")
        _, batch_a = batch_transaction(
            args, node1, [checkpoint_item(acc_id, acc_proof_a)], [committee_acc[:2]])
        checkpoint_a_txid = submit_and_confirm(
            args, node1, [node2], mining_address, batch_a["tx_hex"],
            "fork checkpoint A", confirmed_txids)
        check(int(node_rpc(node1, "getblockcount")) == fork_base_height + 1,
              "fork A height mismatch")
        stop_node(node1)
        stop_node(node2)

        launch_node(args, root, node3)
        check(int(node_rpc(node3, "getblockcount")) == fork_base_height,
              "fork node did not remain at common base")
        check(node_rpc(node3, "getblockhash", [fork_base_height]) == fork_base_hash,
              "fork node common base hash mismatch")
        acc_proof_b = accounting_checkpoint_proof(node3, acc_id, 2, schema_hash, "fork-B")
        _, batch_b = batch_transaction(
            args, node3, [checkpoint_item(acc_id, acc_proof_b)], [committee_acc[:2]])
        checkpoint_b_txid = submit_and_confirm(
            args, node3, [], mining_address, batch_b["tx_hex"],
            "fork checkpoint B", confirmed_txids, blocks=1)
        check(int(node_rpc(node3, "getblockcount")) == fork_base_height + 1,
              "fork B height mismatch")

        launch_node(args, root, node1)
        launch_node(args, root, node2)
        connect_nodes(node1, node3, args.timeout)
        connect_nodes(node2, node3, args.timeout)
        # All three nodes first meet at equal height on different tips.  The
        # next block makes branch B strictly better; both targets receive the
        # full competing blocks through public submitblock and execute their
        # normal block-validation/reorg path.
        mine_blocks(args, node3, mining_address, 1)
        submit_active_block_range(
            node3, node1, fork_base_height + 1, fork_base_height + 2)
        submit_active_block_range(
            node3, node2, fork_base_height + 1, fork_base_height + 2)
        wait_for_matching_tip([node1, node2, node3], fork_base_height + 2, args.timeout)
        expect_rpc_error(node1, "getrawtransaction", [checkpoint_a_txid, 1], "не найдена")
        check(node_rpc(node1, "getrawtransaction", [checkpoint_b_txid, 1])["txid"] == checkpoint_b_txid,
              "canonical fork checkpoint B missing")

        connect_nodes(node1, node2, args.timeout)
        stop_node(node3)
        restart_pair(args, root, node1, node2)
        expect_rpc_error(node2, "getrawtransaction", [checkpoint_a_txid, 1], "не найдена")
        record_coverage(coverage, "real fork/reorg preserves hidden orphan evidence for slash")

        acc_decommission_request = {
            "sidechain_id": acc_id,
            "owner_address": owner_acc["address"],
            "validate": True,
        }
        acc_decommission_created, acc_decommission = decommission_transaction(
            args, node1, acc_decommission_request, owner_acc, committee_acc[:2])
        submit_and_confirm(
            args, node1, [node2], mining_address, acc_decommission["tx_hex"],
            "accounting decommission", confirmed_txids)
        acc_state = assert_registry(node2, acc_id, {
            "sidechain_kind": "accounting", "status": "decommissioning",
        })
        check(int(acc_state["challenge_end_height"]) ==
              int(acc_state["decommission_height"]) + CHALLENGE_BLOCKS,
              "accounting challenge period mismatch")

        early_acc_unlock = {
            "sidechain_id": acc_id,
            "owner_address": owner_acc["address"],
            "outputs": [{"address": recipient_address, "value": 1}],
            "validate": True,
        }
        expect_rpc_error(node1, "createsidechainunlockpayload", [early_acc_unlock], "Challenge")

        slash_request = {
            "sidechain_id": acc_id,
            "owner_address": owner_acc["address"],
            "amount": SLASH_AMOUNT,
            "outputs": [{"address": recipient_address, "value": SLASH_AMOUNT}],
            "fraud_ref_a": {"txid": checkpoint_a_txid},
            "fraud_ref_b": {"txid": checkpoint_b_txid},
            "validate": True,
        }
        slash_created, slash_tx = committee_transaction(
            args, node1, "createsidechainslashpayload", slash_request,
            "finalizesidechainslash", committee_acc[:2])
        check(slash_created["fraud_checkpoint_a"] == checkpoint_a_txid,
              "slash did not preserve fraud ref A")
        check(slash_created["fraud_checkpoint_b"] == checkpoint_b_txid,
              "slash did not preserve fraud ref B")
        submit_and_confirm(
            args, node1, [node2], mining_address, slash_tx["tx_hex"],
            "slash during challenge period", confirmed_txids)
        record_coverage(coverage, "orphan-vs-canonical equivocation SLASH during decommission")

        fin_decommission_request = {
            "sidechain_id": fin_id,
            "owner_address": owner_fin["address"],
            "validate": True,
        }
        fin_decommission_created, fin_decommission = decommission_transaction(
            args, node1, fin_decommission_request, owner_fin, committee_fin_new[:2])
        submit_and_confirm(
            args, node1, [node2], mining_address, fin_decommission["tx_hex"],
            "financial decommission", confirmed_txids)
        fin_state = assert_registry(node1, fin_id, {
            "sidechain_kind": "financial", "status": "decommissioning", "total_issued": 0,
        })

        blocked_topup = funded_deposit_request(
            node1, fin_id, owner_fin["address"], 1_000_000)
        expect_rpc_error(node1, "createsidechaintopuppayload", [blocked_topup], "закрываемого")
        expect_rpc_error(node1, "createsidechainmintpayload", [mint_request])
        post_decommission_proof = financial_checkpoint_proof(
            node1, fin_id, 2, 0, "decommission-checkpoint")
        post_decommission_checkpoint = node_rpc(node1, "createbatchsidechaincheckpointpayload", [{"items": [
            checkpoint_item(fin_id, post_decommission_proof)
        ]}])
        check("tx_hex" in post_decommission_checkpoint,
              "checkpoint during decommissioning should be allowed")
        early_fin_unlock = {
            "sidechain_id": fin_id,
            "owner_address": owner_fin["address"],
            "outputs": [{"address": recipient_address, "value": 1}],
            "validate": True,
        }
        expect_rpc_error(node1, "createsidechainunlockpayload", [early_fin_unlock], "Challenge")
        record_coverage(coverage, "active -> decommissioning transition blocks top-up/mint/early unlock, keeps checkpoints")

        target_unlock_height = max(
            int(fin_state["challenge_end_height"]),
            int(acc_state["challenge_end_height"]),
            unlock_height,
        )
        current_height = int(node_rpc(node1, "getblockcount"))
        empty_blocks = max(0, target_unlock_height - 1 - current_height)
        mine_synced_blocks(args, node1, [node2], mining_address, empty_blocks)
        wait_for_matching_tip([node1, node2], current_height + empty_blocks, args.timeout)

        operation_fee = max(500, (MINT_AMOUNT * FINANCIAL_FEE_BPS) // 10_000)
        financial_remaining = (
            FINANCIAL_LOCK + FINANCIAL_TOP_UP
            - REGISTRATION_FEE
            - operation_fee
            - CHECKPOINT_FEE
            - MINT_AMOUNT
            - operation_fee
        )
        accounting_remaining = (
            ACCOUNTING_LOCK
            - REGISTRATION_FEE
            - CHECKPOINT_FEE
            - CHECKPOINT_FEE
            - SLASH_AMOUNT
        )

        fin_unlock_request = {
            "sidechain_id": fin_id,
            "owner_address": owner_fin["address"],
            "outputs": [{"address": recipient_address, "value": financial_remaining}],
            "validate": True,
        }
        _, fin_unlock = owner_transaction(
            args, node1, "createsidechainunlockpayload", fin_unlock_request,
            "finalizesidechainunlock", owner_fin)
        submit_and_confirm(
            args, node1, [node2], mining_address, fin_unlock["tx_hex"],
            "financial final unlock", confirmed_txids)

        acc_unlock_request = {
            "sidechain_id": acc_id,
            "owner_address": owner_acc["address"],
            "outputs": [{"address": recipient_address, "value": accounting_remaining}],
            "validate": True,
        }
        _, acc_unlock = owner_transaction(
            args, node1, "createsidechainunlockpayload", acc_unlock_request,
            "finalizesidechainunlock", owner_acc)
        submit_and_confirm(
            args, node1, [node2], mining_address, acc_unlock["tx_hex"],
            "accounting final unlock", confirmed_txids)

        for node in (node1, node2):
            expect_rpc_error(node, "getsidechaingenesis", [fin_id], "не найден")
            expect_rpc_error(node, "getsidechaingenesis", [acc_id], "не найден")
        record_coverage(coverage, "144-block challenge and exact final deposit unlock/removal")

        restart_pair(args, root, node1, node2)
        for node in (node1, node2):
            expect_rpc_error(node, "getsidechaingenesis", [fin_id], "не найден")
            expect_rpc_error(node, "getsidechaingenesis", [acc_id], "не найден")
            for txid in confirmed_txids:
                if txid == checkpoint_a_txid:
                    continue
                tx = node_rpc(node, "getrawtransaction", [txid, 1])
                check(tx.get("txid") == txid, f"{node['name']} lost canonical transaction {txid}")
        record_coverage(coverage, "post-unlock restart and canonical transaction history")

        summary.update({
            "result": "passed",
            "final_height": int(node_rpc(node1, "getblockcount")),
            "financial_sidechain_id": fin_id,
            "accounting_sidechain_id": acc_id,
            "orphan_checkpoint_txid": checkpoint_a_txid,
            "canonical_checkpoint_txid": checkpoint_b_txid,
            "financial_remaining_unlocked": financial_remaining,
            "accounting_remaining_unlocked": accounting_remaining,
        })
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except Exception as exc:  # noqa: BLE001
        summary["error"] = str(exc)
        print(json.dumps(summary, indent=2, sort_keys=True), file=sys.stderr)
        for node in all_nodes:
            if node.get("log") is not None:
                print(f"\n===== {node['name']} log =====\n{tail(node['log'])}", file=sys.stderr)
        return 1
    finally:
        for node in all_nodes:
            stop_node(node)
        if temp_dir is not None:
            temp_dir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
