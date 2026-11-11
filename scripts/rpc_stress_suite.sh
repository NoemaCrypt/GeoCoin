#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
rpc_stress_suite.sh - Run a set of RPC stress scenarios

Usage:
  ./scripts/rpc_stress_suite.sh --url http://127.0.0.1:8778/ --user USER --pass PASS

Options:
  --url        RPC URL (default: http://127.0.0.1:8778/)
  --user       RPC username (or RPC_USER env)
  --pass       RPC password (or RPC_PASS env)
  --requests   Requests per scenario (default: 2000)
  --conc       Concurrency per scenario (default: 50)
  --timeout    Per-request curl max time seconds (default: 10)
  --warmup     Warmup requests per scenario (default: 100)
  --selftest   Start a local mock RPC server and run suite against it

Notes:
  - By default, this suite is read-only and safe.
  - For heavy/dangerous scenarios (wallet/mempool/pruning), see docs/rpc/STRESS_*.md for guidance.
USAGE
}

url="http://127.0.0.1:8778/"
user="${RPC_USER:-}"
pass="${RPC_PASS:-}"
requests=2000
conc=50
timeout_s=10
warmup=100
selftest=0

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
STRESS_SH="${SCRIPT_DIR}/rpc_stress.sh"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --url) url="$2"; shift 2 ;;
    --user) user="$2"; shift 2 ;;
    --pass) pass="$2"; shift 2 ;;
    --requests) requests="$2"; shift 2 ;;
    --conc) conc="$2"; shift 2 ;;
    --timeout) timeout_s="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --selftest) selftest=1; shift 1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ -z "$user" || -z "$pass" ]]; then
  echo "ERROR: missing --user/--pass (or RPC_USER/RPC_PASS env)" >&2
  exit 2
fi

mock_pid=""
cleanup() {
  if [[ -n "$mock_pid" ]]; then
    kill "$mock_pid" >/dev/null 2>&1 || true
    wait "$mock_pid" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

if [[ "$selftest" -eq 1 ]]; then
  # Selftest uses --runner mock (no sockets). Some environments disallow binding to local ports.
  url="http://127.0.0.1:0/"
fi

run() {
  local name="$1"
  local method="$2"
  local params="$3"
  echo
  echo "=== $name ==="
  "$STRESS_SH" \
    --url "$url" --user "$user" --pass "$pass" \
    --method "$method" --params "$params" \
    --requests "$requests" --concurrency "$conc" --timeout "$timeout_s" --warmup "$warmup" \
    $( [[ "$selftest" -eq 1 ]] && printf '%s' "--runner mock" )
}

run "Control: help" "help" "[]"
run "Chain: getblockcount" "getblockcount" "[]"
run "Chain: getblockhash(height=0)" "getblockhash" "[0]"
run "Chain: getblockchaininfo" "getblockchaininfo" "[]"
run "Network: getconnectioncount" "getconnectioncount" "[]"

echo
echo "Suite finished."
