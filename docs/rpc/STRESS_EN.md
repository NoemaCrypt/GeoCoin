# RPC stress tests (EN)

**Date:** April 16, 2026

This repo includes a lightweight RPC load test with no extra dependencies: `scripts/rpc_stress.sh`.
It also includes a set of safe read-only scenarios: `scripts/rpc_stress_suite.sh`.

## What it measures

- success/failure counts (HTTP status)
- throughput (req/s based on wall time)
- latency percentiles p50/p95/p99 from `curl time_total` (successful responses only)

## How to run

1) Start the node with RPC enabled and set `rpc-user/rpc-pass` (see `docs/build-and-run.md`).
2) Run the stress test:

```bash
chmod +x scripts/rpc_stress.sh
chmod +x scripts/rpc_stress_suite.sh

export RPC_URL="http://127.0.0.1:13033/"
export RPC_USER="user"
export RPC_PASS="pass"

./scripts/rpc_stress.sh --url "$RPC_URL" \
  --method getblockcount --params '[]' \
  --requests 5000 --concurrency 50 --warmup 200
```

Run the read-only suite:

```bash
./scripts/rpc_stress_suite.sh --url "$RPC_URL" \
  --user "$RPC_USER" --pass "$RPC_PASS" \
  --requests 2000 --conc 50 --warmup 100
```

## Selftest (no node required)

To verify the scripts without a running node, use the `mock` runner (no sockets/network required):

```bash
./scripts/rpc_stress_suite.sh --user "$RPC_USER" --pass "$RPC_PASS" --selftest
```

Example for a chain query:

```bash
./scripts/rpc_stress.sh --url "$RPC_URL" \
  --method getblockhash --params '[100]' \
  --requests 2000 --concurrency 25
```

## Notes

- RPC request rate limiting is **off by default** (`rpc-rate-limit=0` in `geocoin.conf`, or `--rpc-rate-limit 0`). A non-zero value returns `429` when a client exceeds the 60-second budget.
- For heavy methods (`sendrawtransaction`, wallet flows), run separate tests with safeguards so you don't saturate mempool/disk.

## Next stress scenarios (dangerous)

For state-mutating stress tests, recommended guardrails:

- use a dedicated test `data-dir` (easy to wipe/recreate)
- run on `degree-test`
- require an explicit `ALLOW_DANGEROUS=1` flag in scripts (fail-closed by default)
- enforce limits (time, request count, mempool size)

Candidate scenarios:

- mempool admission (batch `sendrawtransaction` with valid txs)
- wallet send loop (`sendtoaddress`) with UTXO controls
- witness pruning (`prunenow`) + restore checks (`unprune`) on a pruned node
