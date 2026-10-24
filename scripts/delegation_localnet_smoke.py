#!/usr/bin/env python3
import argparse
import json
import sys
import tempfile
import time
from decimal import Decimal
from pathlib import Path

from multisig_localnet_smoke import (
    find_free_port,
    mine_blocks,
    rpc_call,
    run_builder,
    start_node,
    stop_process,
    tail,
    wait_for_address_balance,
    wait_for_height,
    wait_for_peer,
    wait_for_rpc,
)


COIN = 100_000_000
MIN_TX_FEE = 500


def participant_addresses(info: dict) -> list[str]:
    return [item["address"] for item in info["participants"]]


def sign_commitments(node, multisig_address: str, signing_hash_field: str, signing_hash: str, participants: list[dict], count: int):
    commitments = []
    for participant in participants[:count]:
        commitments.append(
            rpc_call(
                node["url"],
                node["user"],
                node["password"],
                "signanglecommitment",
                [{
                    "multisig_address": multisig_address,
                    signing_hash_field: signing_hash,
                    "private_key": participant["private_key_hex"],
                    "expected_signer_address": participant["address"],
                }],
            )
        )
    return commitments


def wait_for_mempool(node, expected_txids: set[str], timeout: float) -> list[str]:
    deadline = time.time() + timeout
    last_mempool = []
    while time.time() < deadline:
        last_mempool = rpc_call(node["url"], node["user"], node["password"], "getrawmempool")
        missing = expected_txids.difference(last_mempool)
        if not missing:
            return last_mempool
        time.sleep(0.5)
    raise RuntimeError(
        f"{node['name']} mempool missing txids: {sorted(expected_txids.difference(last_mempool))}\n"
        f"{tail(node['log'])}"
    )


def expect_rpc_error(node, method: str, params, label: str) -> str:
    try:
        rpc_call(node["url"], node["user"], node["password"], method, params)
    except Exception as exc:  # noqa: BLE001
        return str(exc)
    raise RuntimeError(f"{label} unexpectedly succeeded")


def mine_and_assert_protocol_reward_increases(args, node, mining_address: str, blocks: int) -> dict:
    def balances():
        fund = rpc_call(node["url"], node["user"], node["password"], "getfundbalance")
        pool = rpc_call(node["url"], node["user"], node["password"], "getinterestpool")
        return {"foundation": fund, "interest_pool": pool,
                "balance_angles": int(fund["balance_angles"]) + int(pool["balance_angles"])}

    before = balances()
    previous = before["balance_angles"]
    per_block = []
    for _ in range(blocks):
        prev_height = int(rpc_call(node["url"], node["user"], node["password"], "getblockcount"))
        mine_blocks(args, node, mining_address, 1)
        wait_for_height(node, prev_height + 1, args.timeout)
        current = balances()
        delta = current["balance_angles"] - previous
        if delta <= 0:
            raise RuntimeError(f"protocol reward did not increase after block {prev_height + 1}")
        per_block.append({"height": prev_height + 1, "delta_angles": delta,
                          "balance_angles": current["balance_angles"]})
        previous = current["balance_angles"]
    return {"before": before, "after": balances(),
            "delta_angles": previous - before["balance_angles"], "per_block": per_block}


def main() -> int:
    parser = argparse.ArgumentParser(description="Two-node local delegation smoke test")
    parser.add_argument("--node-binary", required=True, type=Path)
    parser.add_argument("--miner-binary", required=True, type=Path)
    parser.add_argument("--miner-kind", choices=("gpu", "rpc-cpu"), default="gpu")
    parser.add_argument("--package-builder", required=True, type=Path)
    parser.add_argument("--activation-manifest", required=True, type=Path)
    parser.add_argument("--network", default="degree-test")
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--coinbase-maturity", type=int, default=100)
    parser.add_argument("--mining-threads", type=int, default=1)
    parser.add_argument("--gpu-backend", default="auto", choices=("auto", "cuda", "hip", "opencl"))
    parser.add_argument("--gpu-devices", default="")
    parser.add_argument("--gpu-intensity", type=int, default=16777216)
    parser.add_argument("--gpu-max-launch-attempts", type=int, default=16777216)
    parser.add_argument("--primary-m", type=int, default=5)
    parser.add_argument("--primary-n", type=int, default=5)
    parser.add_argument("--delegated-m", type=int, default=2)
    parser.add_argument("--delegated-n", type=int, default=3)
    parser.add_argument("--keep-data-dir", action="store_true")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    if args.coinbase_maturity < 1:
        parser.error("--coinbase-maturity must be >= 1")

    for label, m, n in (
        ("primary", args.primary_m, args.primary_n),
        ("delegated", args.delegated_m, args.delegated_n),
    ):
        if m <= 0 or n <= 0 or m > n or n > 15:
            parser.error(f"--{label}-m/--{label}-n must satisfy 1 <= m <= n <= 15")

    args.node_binary = args.node_binary.resolve()
    args.miner_binary = args.miner_binary.resolve()
    args.package_builder = args.package_builder.resolve()
    args.activation_manifest = args.activation_manifest.resolve()

    temp_dir = None
    if args.keep_data_dir:
        root = Path(tempfile.mkdtemp(prefix="geocoin-delegation-localnet-"))
    else:
        temp_dir = tempfile.TemporaryDirectory(prefix="geocoin-delegation-localnet-")
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
        recipient = rpc_call(node2["url"], node2["user"], node2["password"], "getnewaddress")

        primary = run_builder(
            args,
            "describe",
            "--m", str(args.primary_m),
            "--n", str(args.primary_n),
            "--first-seed", "101",
        )
        delegated = run_builder(
            args,
            "describe",
            "--m", str(args.delegated_m),
            "--n", str(args.delegated_n),
            "--first-seed", "151",
        )
        primary_address = primary["multisig_address"]
        delegated_address = delegated["multisig_address"]

        initial_height = int(rpc_call(node1["url"], node1["user"], node1["password"], "getblockcount"))
        funding_blocks = max(3, args.coinbase_maturity)
        foundation_reward = mine_and_assert_protocol_reward_increases(
            args,
            node1,
            primary_address,
            min(3, funding_blocks),
        )
        if funding_blocks > 3:
            mine_blocks(args, node1, primary_address, funding_blocks - 3)
        funded_height = wait_for_height(node1, initial_height + funding_blocks, args.timeout)
        wait_for_height(node2, funded_height, args.timeout)

        primary_balance = wait_for_address_balance(node1, primary_address, COIN, args.timeout)
        primary_balance_node2 = wait_for_address_balance(node2, primary_address, COIN, args.timeout)

        draft = rpc_call(
            node1["url"],
            node1["user"],
            node1["password"],
            "createdelegationproofdraft",
            [{
                "primary_multisig_address": primary_address,
                "delegated_multisig_address": delegated_address,
                "primary_m": args.primary_m,
                "primary_participants": participant_addresses(primary),
                "delegated_m": args.delegated_m,
                "delegated_participants": participant_addresses(delegated),
                "max_total_delegated": 2 * COIN,
                "max_per_transaction": COIN + 1_000_000,  # Transfer plus package fee.
                "max_percent_of_balance": 0,
                "valid_from": 0,
                "valid_until": 0,
                "nonce": 5001,
            }],
        )

        negative_short_primary = expect_rpc_error(
            node1,
            "finalizedelegationproof",
            [{
                "tx_hex": draft["tx_hex"],
                "commitments": ["00" * 32 for _ in range(args.primary_m - 1)],
            }],
            "finalizedelegationproof without primary quorum",
        )

        primary_commitments = sign_commitments(
            node1,
            primary_address,
            "delegation_hash",
            draft["delegation_hash"],
            primary["participants"],
            args.primary_m,
        )
        finalized_proof = rpc_call(
            node1["url"],
            node1["user"],
            node1["password"],
            "finalizedelegationproof",
            [{
                "tx_hex": draft["tx_hex"],
                "commitments": [item["commitment_hash"] for item in primary_commitments],
            }],
        )
        delegation_package = rpc_call(
            node1["url"],
            node1["user"],
            node1["password"],
            "senddelegationpackage",
            [{
                "commitments": [item["tx_hex"] for item in primary_commitments],
                "proof": finalized_proof["tx_hex"],
            }],
        )
        delegation_txids = set([item["txid"] for item in primary_commitments] + [finalized_proof["txid"]])
        wait_for_mempool(node1, delegation_txids, args.timeout)

        duplicate_delegation_rejected = False
        try:
            rpc_call(
                node1["url"],
                node1["user"],
                node1["password"],
                "senddelegationpackage",
                [{
                    "commitments": [item["tx_hex"] for item in primary_commitments],
                    "proof": finalized_proof["tx_hex"],
                }],
            )
        except Exception:
            duplicate_delegation_rejected = True
        if not duplicate_delegation_rejected:
            raise RuntimeError("duplicate delegation package was accepted")

        mine_blocks(args, node1, primary_address, 1)
        proof_height = wait_for_height(node1, funded_height + 1, args.timeout)
        wait_for_height(node2, proof_height, args.timeout)
        proof_info1 = rpc_call(node1["url"], node1["user"], node1["password"], "getdelegationproof", [finalized_proof["txid"]])
        proof_info2 = rpc_call(node2["url"], node2["user"], node2["password"], "getdelegationproof", [finalized_proof["txid"]])

        hierarchical_draft = rpc_call(
            node1["url"],
            node1["user"],
            node1["password"],
            "createhierarchicalspenddraft",
            [{
                "delegation_proof_txid": finalized_proof["txid"],
                "delegated_m": args.delegated_m,
                "delegated_participants": participant_addresses(delegated),
                "to_address": recipient,
                "amount": COIN,
                "fee": MIN_TX_FEE,
            }],
        )

        negative_short_delegated = expect_rpc_error(
            node1,
            "finalizehierarchicalspend",
            [{
                "tx_hex": hierarchical_draft["tx_hex"],
                "commitments": ["00" * 32 for _ in range(args.delegated_m - 1)],
            }],
            "finalizehierarchicalspend without delegated quorum",
        )

        delegated_commitments = sign_commitments(
            node1,
            delegated_address,
            "hierarchical_spend_hash",
            hierarchical_draft["hierarchical_spend_hash"],
            delegated["participants"],
            args.delegated_m,
        )
        finalized_spend = rpc_call(
            node1["url"],
            node1["user"],
            node1["password"],
            "finalizehierarchicalspend",
            [{
                "tx_hex": hierarchical_draft["tx_hex"],
                "commitments": [item["commitment_hash"] for item in delegated_commitments],
            }],
        )
        spend_package = rpc_call(
            node1["url"],
            node1["user"],
            node1["password"],
            "sendmultisigpackage",
            [{
                "commitments": [item["tx_hex"] for item in delegated_commitments],
                "spend": finalized_spend["tx_hex"],
            }],
        )
        spend_txids = set([item["txid"] for item in delegated_commitments] + [finalized_spend["txid"]])
        wait_for_mempool(node1, spend_txids, args.timeout)

        mine_blocks(args, node1, primary_address, 1)
        spend_height = wait_for_height(node1, proof_height + 1, args.timeout)
        wait_for_height(node2, spend_height, args.timeout)

        spend_tx1 = rpc_call(node1["url"], node1["user"], node1["password"], "getrawtransaction", [finalized_spend["txid"], 1])
        spend_tx2 = rpc_call(node2["url"], node2["user"], node2["password"], "getrawtransaction", [finalized_spend["txid"], 1])
        if int(spend_tx1.get("type")) != 2 or int(spend_tx2.get("type")) != 2:
            raise RuntimeError(f"confirmed hierarchical spend is not MULTISIG_SPEND: node1={spend_tx1} node2={spend_tx2}")

        recipient_balance1 = wait_for_address_balance(node1, recipient, COIN, args.timeout)
        recipient_balance2 = wait_for_address_balance(node2, recipient, COIN, args.timeout)
        proof_after1 = rpc_call(node1["url"], node1["user"], node1["password"], "getdelegationproof", [finalized_proof["txid"]])
        proof_after2 = rpc_call(node2["url"], node2["user"], node2["password"], "getdelegationproof", [finalized_proof["txid"]])
        expected_debit = COIN + int(hierarchical_draft["fee"])
        if any(Decimal(str(proof["total_spent"])) * COIN != expected_debit
               for proof in (proof_after1, proof_after2)):
            raise RuntimeError(f"delegation total_spent must include fee: node1={proof_after1} node2={proof_after2}")

        summary.update({
            "result": "ok",
            "peers": {"node1": peers1, "node2": peers2},
            "wallets": {"node1": wallet1, "node2": wallet2},
            "foundation_consensus": {
                "address": foundation_reward["after"]["foundation"]["address"],
                "reward_observed": foundation_reward,
            },
            "primary_fund": {
                "m": args.primary_m,
                "n": args.primary_n,
                "address": primary_address,
                "balance_after_funding_node1": primary_balance,
                "balance_after_funding_node2": primary_balance_node2,
            },
            "delegated_multisig": {
                "m": args.delegated_m,
                "n": args.delegated_n,
                "address": delegated_address,
            },
            "recipient": recipient,
            "transactions": {
                "delegation_proof_txid": finalized_proof["txid"],
                "hierarchical_spend_txid": finalized_spend["txid"],
                "primary_commitment_txids": [item["txid"] for item in primary_commitments],
                "delegated_commitment_txids": [item["txid"] for item in delegated_commitments],
            },
            "proof_before_spend": {"node1": proof_info1, "node2": proof_info2},
            "proof_after_spend": {"node1": proof_after1, "node2": proof_after2},
            "recipient_balances": {"node1": recipient_balance1, "node2": recipient_balance2},
            "heights": {
                "initial": initial_height,
                "funded": funded_height,
                "proof_confirmed": proof_height,
                "spend_confirmed": spend_height,
            },
            "send_results": {
                "delegation": delegation_package,
                "spend": spend_package,
            },
            "negative_checks": {
                "short_primary_quorum_rejected": negative_short_primary,
                "short_delegated_quorum_rejected": negative_short_delegated,
                "duplicate_delegation_package_rejected": duplicate_delegation_rejected,
            },
        })
        summary["connection_diagnostics"] = {
            node["name"]: rpc_call(node["url"], node["user"], node["password"], "getconnectiondiagnostics")
            for node in nodes
        }
        rate_rejects = {
            name: {reason: count for reason, count in diagnostics["policy_rejects"].items()
                   if reason.endswith("rate-limit") and count}
            for name, diagnostics in summary["connection_diagnostics"].items()
        }
        if any(rate_rejects.values()):
            raise RuntimeError(f"normal localnet traffic exceeded peer rate limits: {rate_rejects}")
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except Exception as exc:  # noqa: BLE001
        summary["result"] = "failed"
        summary["error"] = str(exc)
        summary["logs"] = {node["name"]: tail(node["log"]) for node in nodes}
        summary.setdefault("connection_diagnostics", {})
        for node in nodes:
            try:
                summary["connection_diagnostics"][node["name"]] = rpc_call(
                    node["url"], node["user"], node["password"], "getconnectiondiagnostics")
            except Exception as diagnostic_error:  # noqa: BLE001
                summary["connection_diagnostics"][node["name"]] = {"error": str(diagnostic_error)}
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
