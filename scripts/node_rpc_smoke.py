#!/usr/bin/env python3
import argparse
import base64
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path


DEFAULT_COINBASE_MATURITY = 100


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


def file_contains(path: Path, needle: str) -> bool:
    try:
        return needle in path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


def wait_for_rpc(
    url: str,
    user: str,
    password: str,
    timeout: float,
    proc: subprocess.Popen | None = None,
    log_path: Path | None = None,
):
    deadline = time.time() + timeout
    last_error = None
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            log_tail = f"\nNode log tail:\n{tail_file(log_path)}" if log_path is not None else ""
            raise RuntimeError(
                f"Node exited before RPC became ready (code={proc.returncode}).{log_tail}"
            )
        try:
            rpc_call(url, user, password, "help", [])
            if log_path is not None and not file_contains(log_path, "RPC server started"):
                last_error = RuntimeError(
                    "RPC endpoint answered before the launched node reported RPC server started"
                )
                time.sleep(1)
                continue
            if proc is not None and proc.poll() is not None:
                log_tail = f"\nNode log tail:\n{tail_file(log_path)}" if log_path is not None else ""
                raise RuntimeError(
                    f"Node exited before RPC became ready (code={proc.returncode}).{log_tail}"
                )
            return
        except Exception as exc:  # noqa: BLE001
            last_error = exc
            time.sleep(1)
    if proc is not None and proc.poll() is not None:
        log_tail = f"\nNode log tail:\n{tail_file(log_path)}" if log_path is not None else ""
        raise RuntimeError(
            f"Node exited before RPC became ready (code={proc.returncode}).{log_tail}"
        )
    raise RuntimeError(f"RPC did not become ready within {timeout}s: {last_error}")


def wait_for_height(url: str, user: str, password: str, target_height: int, timeout: float):
    deadline = time.time() + timeout
    last_height = None
    while time.time() < deadline:
        last_height = int(rpc_call(url, user, password, "getblockcount", []))
        if last_height >= target_height:
            return last_height
        time.sleep(1)
    raise RuntimeError(
        f"Target height {target_height} was not reached within {timeout}s (last height={last_height})"
    )


def resolve_external_miner(args):
    candidates = []
    if args.external_miner:
        candidates.append(args.external_miner)

    suffix = ".exe" if os.name == "nt" else ""
    candidates.append(args.node_binary.parent / f"geocoin-rpc-miner{suffix}")
    candidates.append(args.node_binary.parent.parent / "bin" / f"geocoin-rpc-miner{suffix}")

    for candidate in candidates:
        if candidate and candidate.exists():
            return candidate.resolve()
    return None


def read_secret_file(path: Path) -> str:
    with path.open("r", encoding="utf-8") as secret_file:
        secret = secret_file.readline().rstrip("\r\n")
    if not secret:
        raise RuntimeError(f"Secret file is empty: {path}")
    return secret


def write_private_secret_file(path: Path, value: str):
    path.write_text(f"{value}\n", encoding="utf-8")
    if os.name != "nt":
        path.chmod(0o600)


def run_external_miner(args, url: str, mining_address: str, blocks: int, summary: dict):
    miner = resolve_external_miner(args)
    if miner is None:
        raise RuntimeError("External mining requested, but geocoin-rpc-miner was not found")
    if blocks <= 0:
        return

    command = [
        str(miner),
        "--url",
        url,
        "--rpc-user",
        args.rpc_miner_user,
        "--rpc-pass-file",
        str(args.node_rpc_miner_pass_file),
        "--address",
        mining_address,
        "--blocks",
        str(blocks),
        "--threads",
        str(args.mining_threads),
        "--timeout",
        str(args.timeout),
    ]
    completed = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=max(args.timeout * blocks + 60, 120),
        check=False,
    )

    summary.setdefault("external_miner_runs", []).append({
        "miner": str(miner),
        "blocks": blocks,
        "returncode": completed.returncode,
        "stdout": completed.stdout[-4000:],
        "stderr": completed.stderr[-4000:],
    })
    if completed.returncode != 0:
        raise RuntimeError(
            f"External miner failed with code {completed.returncode}: {completed.stderr.strip()}"
        )


def mine_to_height(
    args,
    url: str,
    mining_address: str,
    target_height: int,
    timeout: float,
    summary: dict,
):
    current_height = int(rpc_call(url, args.rpc_user, args.rpc_pass, "getblockcount", []))
    if current_height >= target_height:
        return current_height

    use_external = False
    if args.mining_mode == "external":
        use_external = True
    elif args.mining_mode == "auto" and resolve_external_miner(args) is not None:
        use_external = True

    if use_external:
        run_external_miner(args, url, mining_address, target_height - current_height, summary)
        return wait_for_height(url, args.rpc_user, args.rpc_pass, target_height, timeout)

    internal_threads = args.mining_threads if args.mining_threads > 0 else max(1, os.cpu_count() or 1)
    try:
        rpc_call(url, args.rpc_user, args.rpc_pass, "setgenerate", [True, internal_threads])
        return wait_for_height(url, args.rpc_user, args.rpc_pass, target_height, timeout)
    finally:
        try:
            rpc_call(url, args.rpc_user, args.rpc_pass, "setgenerate", [False])
        except Exception:
            pass


def stop_process(proc: subprocess.Popen, allow_sigint: bool = True):
    if proc.poll() is not None:
        return

    if allow_sigint:
        try:
            if os.name == "nt":
                proc.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                proc.send_signal(signal.SIGINT)
        except Exception:  # noqa: BLE001
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


def start_node(args, data_dir: Path, log_path: Path):
    command = [
        str(args.node_binary),
        "--data-dir",
        str(data_dir),
        "--p2p-port",
        str(args.p2p_port),
        "--rpc-port",
        str(args.rpc_port),
        "--rpc",
        "--rpc-user",
        args.rpc_user,
        "--rpc-pass-file",
        str(args.node_rpc_pass_file),
        "--rpc-miner-user",
        args.rpc_miner_user,
        "--rpc-miner-pass-file",
        str(args.node_rpc_miner_pass_file),
        "--network",
        args.network,
        "--no-auto-connect",
        "--status-interval",
        "3600",
    ]

    if args.activation_manifest:
        command.extend(["--activation-manifest", str(args.activation_manifest)])
    if args.wallet_password:
        command.extend(["--wallet-password", args.wallet_password])
    if args.debug:
        command.append("--debug")

    log_file = open(log_path, "a", encoding="utf-8")
    env = os.environ.copy()
    if not args.wallet_password:
        env.setdefault("GEOCOIN_ALLOW_PLAINTEXT_WALLET", "1")
    proc = subprocess.Popen(
        command,
        cwd=str(data_dir),
        stdout=log_file,
        stderr=subprocess.STDOUT,
        env=env,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
    )
    return proc, log_file


def main():
    parser = argparse.ArgumentParser(description="GeoCoin node/RPC smoke test")
    parser.add_argument("--node-binary", required=True, type=Path)
    parser.add_argument("--activation-manifest", type=Path)
    parser.add_argument("--network", default="degree-test")
    parser.add_argument("--rpc-host", default="127.0.0.1")
    parser.add_argument("--rpc-port", type=int, default=19445)
    parser.add_argument("--p2p-port", type=int, default=19444)
    parser.add_argument("--rpc-user", default="smoke")
    parser.add_argument("--rpc-pass", default="smoke-pass")
    parser.add_argument("--rpc-pass-file", type=Path)
    parser.add_argument("--rpc-miner-user", default="miner")
    parser.add_argument("--rpc-miner-pass", default="smoke-miner-pass")
    parser.add_argument("--rpc-miner-pass-file", type=Path)
    parser.add_argument("--wallet-password", default="")
    parser.add_argument("--mined-blocks", type=int, default=4)
    parser.add_argument("--coinbase-maturity", type=int, default=DEFAULT_COINBASE_MATURITY)
    parser.add_argument("--confirm-blocks", type=int, default=1)
    parser.add_argument("--mining-threads", type=int, default=0)
    parser.add_argument("--mining-mode", choices=["auto", "internal", "external"], default="auto")
    parser.add_argument("--external-miner", type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--amount", type=float, default=1.0)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--data-dir", type=Path)
    args = parser.parse_args()

    args.node_binary = args.node_binary.resolve()
    if args.external_miner:
        args.external_miner = args.external_miner.resolve()
    if args.activation_manifest:
        args.activation_manifest = args.activation_manifest.resolve()
    if args.rpc_pass_file:
        args.rpc_pass_file = args.rpc_pass_file.resolve()
        args.rpc_pass = read_secret_file(args.rpc_pass_file)
    if args.rpc_miner_pass_file:
        args.rpc_miner_pass_file = args.rpc_miner_pass_file.resolve()
        args.rpc_miner_pass = read_secret_file(args.rpc_miner_pass_file)

    temp_dir_obj = None
    if args.data_dir is None:
        temp_dir_obj = tempfile.TemporaryDirectory(prefix="geocoin-smoke-")
        data_dir = Path(temp_dir_obj.name)
    else:
        data_dir = args.data_dir.resolve()
        data_dir.mkdir(parents=True, exist_ok=True)

    if args.rpc_pass_file:
        args.node_rpc_pass_file = args.rpc_pass_file
    else:
        args.node_rpc_pass_file = data_dir / "rpc-admin.pass"
        write_private_secret_file(args.node_rpc_pass_file, args.rpc_pass)

    if args.rpc_miner_user == args.rpc_user and args.rpc_miner_pass == args.rpc_pass:
        raise RuntimeError("Miner RPC credentials must differ from admin RPC credentials")

    if args.rpc_miner_pass_file:
        args.node_rpc_miner_pass_file = args.rpc_miner_pass_file
    else:
        args.node_rpc_miner_pass_file = data_dir / "rpc-miner.pass"
        write_private_secret_file(args.node_rpc_miner_pass_file, args.rpc_miner_pass)

    log_path = data_dir / "geocoin-node.log"
    url = f"http://{args.rpc_host}:{args.rpc_port}/"
    summary = {
        "node_binary": str(args.node_binary),
        "network": args.network,
        "data_dir": str(data_dir),
        "log_path": str(log_path),
    }

    proc = None
    log_file = None
    try:
        proc, log_file = start_node(args, data_dir, log_path)
        wait_for_rpc(url, args.rpc_user, args.rpc_pass, args.timeout, proc=proc, log_path=log_path)

        wallet_info = rpc_call(url, args.rpc_user, args.rpc_pass, "createwallet", [args.wallet_password] if args.wallet_password else [])
        mining_address = wallet_info["address"]
        recipient_address = rpc_call(url, args.rpc_user, args.rpc_pass, "getnewaddress", [])
        initial_height = int(rpc_call(url, args.rpc_user, args.rpc_pass, "getblockcount", []))
        spendable_mined_blocks = max(args.mined_blocks, args.coinbase_maturity)

        mined_height = mine_to_height(
            args,
            url,
            mining_address,
            initial_height + spendable_mined_blocks,
            args.timeout,
            summary,
        )

        balance = float(rpc_call(url, args.rpc_user, args.rpc_pass, "getbalance", []))
        if balance <= 0:
            raise RuntimeError(f"Wallet balance did not increase after mining (balance={balance})")

        txid = rpc_call(url, args.rpc_user, args.rpc_pass, "sendtoaddress", [recipient_address, args.amount])

        confirmed_height = mine_to_height(
            args,
            url,
            mining_address,
            mined_height + args.confirm_blocks,
            args.timeout,
            summary,
        )

        tx_info = rpc_call(url, args.rpc_user, args.rpc_pass, "getrawtransaction", [txid, 1])
        recipient_balance = rpc_call(url, args.rpc_user, args.rpc_pass, "getaddressbalance", [recipient_address])
        if float(recipient_balance["balance"]) < args.amount:
            raise RuntimeError(
                f"Recipient balance too low after confirmation: {recipient_balance['balance']}"
            )

        summary.update({
            "wallet_address": mining_address,
            "recipient_address": recipient_address,
            "initial_height": initial_height,
            "requested_mined_blocks": args.mined_blocks,
            "spendable_mined_blocks": spendable_mined_blocks,
            "mined_height": mined_height,
            "confirmed_height": confirmed_height,
            "balance_after_mining": balance,
            "txid": txid,
            "tx_type": tx_info.get("type"),
            "recipient_balance": recipient_balance,
            "mining_mode": args.mining_mode,
        })

        try:
            rpc_call(url, args.rpc_user, args.rpc_pass, "stop", [])
        except Exception:
            pass
        stop_process(proc)
        log_file.close()
        proc = None
        log_file = None

        proc, log_file = start_node(args, data_dir, log_path)
        wait_for_rpc(url, args.rpc_user, args.rpc_pass, args.timeout)

        restart_height = int(rpc_call(url, args.rpc_user, args.rpc_pass, "getblockcount", []))
        restart_tx_info = rpc_call(url, args.rpc_user, args.rpc_pass, "getrawtransaction", [txid, 1])
        restart_recipient_balance = rpc_call(url, args.rpc_user, args.rpc_pass, "getaddressbalance", [recipient_address])

        if restart_height < confirmed_height:
            raise RuntimeError(
                f"Height regressed after restart ({restart_height} < {confirmed_height})"
            )
        if restart_tx_info.get("txid") != txid:
            raise RuntimeError("Transaction lookup changed after restart")
        if float(restart_recipient_balance["balance"]) < args.amount:
            raise RuntimeError("Recipient balance was not preserved after restart")

        summary.update({
            "restart_height": restart_height,
            "restart_recipient_balance": restart_recipient_balance,
            "result": "ok",
        })

        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except Exception as exc:  # noqa: BLE001
        summary["result"] = "failed"
        summary["error"] = str(exc)
        print(json.dumps(summary, indent=2, sort_keys=True), file=sys.stderr)
        return 1
    finally:
        if log_file is not None:
            log_file.close()
        if proc is not None:
            stop_process(proc)
        if temp_dir_obj is not None:
            temp_dir_obj.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
