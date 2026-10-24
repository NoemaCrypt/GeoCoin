#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
rpc_stress.sh - JSON-RPC load test for GeoCoin node

Usage:
  ./scripts/rpc_stress.sh --url http://127.0.0.1:8778/ --user USER --pass PASS \
    --method getblockcount --params '[]' --requests 5000 --concurrency 50

Options:
  --url            RPC URL (default: http://127.0.0.1:8778/)
  --user           RPC username (or set RPC_USER env)
  --pass           RPC password (or set RPC_PASS env)
  --method         JSON-RPC method name (default: getblockcount)
  --params         JSON array string (default: [])
  --requests       Total requests to send (default: 1000)
  --concurrency    Parallel workers (default: 25)
  --timeout        Per-request curl max time seconds (default: 10)
  --warmup         Warmup request count (default: 0)
  --runner         Request runner: curl|mock (default: curl)

Output:
  - total/ok/fail
  - wall time and req/s
  - latency percentiles from curl time_total (p50/p95/p99)
USAGE
}

url="http://127.0.0.1:8778/"
user="${RPC_USER:-}"
pass="${RPC_PASS:-}"
method="getblockcount"
params="[]"
requests=1000
concurrency=25
timeout_s=10
warmup=0
runner="curl"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --url) url="$2"; shift 2 ;;
    --user) user="$2"; shift 2 ;;
    --pass) pass="$2"; shift 2 ;;
    --method) method="$2"; shift 2 ;;
    --params) params="$2"; shift 2 ;;
    --requests) requests="$2"; shift 2 ;;
    --concurrency) concurrency="$2"; shift 2 ;;
    --timeout) timeout_s="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --runner) runner="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ -z "$user" || -z "$pass" ]]; then
  echo "ERROR: missing --user/--pass (or RPC_USER/RPC_PASS env)" >&2
  exit 2
fi

if ! command -v curl >/dev/null 2>&1; then
  echo "ERROR: curl not found" >&2
  exit 2
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
out_file="$tmpdir/out.txt"

json_body=$(printf '{"jsonrpc":"2.0","id":1,"method":"%s","params":%s}' "$method" "$params")

run_batch() {
  local count="$1"
  local parallel="$2"
  seq 1 "$count" | xargs -P "$parallel" -n 1 bash -c '
    set -euo pipefail
    runner="$1"; url="$2"; auth="$3"; body="$4"; timeout_s="$5"
    if [[ "$runner" == "mock" ]]; then
      # Print one line: "<http_code> <time_total>"
      # Simulate a small and slightly jittery latency distribution.
      ms=$(( (RANDOM % 7) + 1 ))  # 1..7 ms
      time_total="$(awk -v ms="$ms" "BEGIN{printf \"%.4f\", ms/1000.0}")"
      echo "200 $time_total"
      exit 0
    fi

    # curl runner: keep stderr quiet to avoid xargs output interleaving.
    curl -s -u "$auth" -H "Content-Type: application/json" \
      --max-time "$timeout_s" \
      -o /dev/null -w "%{http_code} %{time_total}\n" \
      --data "$body" "$url" 2>/dev/null || echo "000 0"
  ' _ "$runner" "$url" "$user:$pass" "$json_body" "$timeout_s"
}

if [[ "$warmup" -gt 0 ]]; then
  echo "Warmup: $warmup requests..." >&2
  run_batch "$warmup" "$concurrency" >/dev/null
fi

start_ns="$(date +%s%N 2>/dev/null || true)"
if [[ -z "${start_ns}" ]]; then
  start_ns=""
fi

run_batch "$requests" "$concurrency" >"$out_file"

end_ns="$(date +%s%N 2>/dev/null || true)"
if [[ -z "${end_ns}" ]]; then
  end_ns=""
fi

total="$(wc -l <"$out_file" | tr -d ' ')"
ok="$(awk '$1=="200"{c++} END{print c+0}' "$out_file")"
fail="$((total - ok))"

wall_s="n/a"
reqps="n/a"
if [[ -n "$start_ns" && -n "$end_ns" ]]; then
  wall_ns="$((end_ns - start_ns))"
  if [[ "$wall_ns" -gt 0 ]]; then
    wall_s="$(awk -v ns="$wall_ns" 'BEGIN{printf "%.3f", ns/1e9}')"
    reqps="$(awk -v n="$total" -v ns="$wall_ns" 'BEGIN{printf "%.1f", n/(ns/1e9)}')"
  fi
fi

times_file="$tmpdir/times.txt"
awk '$1=="200"{print $2}' "$out_file" >"$times_file"
count_times="$(wc -l <"$times_file" | tr -d ' ')"

pct() {
  local p="$1"
  if [[ "$count_times" -le 0 ]]; then
    echo "n/a"
    return
  fi
  local idx
  idx="$(awk -v n="$count_times" -v p="$p" 'BEGIN{print int((p/100.0)*n + 0.999999)}')"
  if [[ "$idx" -lt 1 ]]; then idx=1; fi
  if [[ "$idx" -gt "$count_times" ]]; then idx="$count_times"; fi
  sort -n "$times_file" | awk -v i="$idx" 'NR==i{printf "%.4f", $1; exit}'
}

echo "RPC stress: method=$method params=$params"
echo "URL: $url"
echo "Requests: total=$total ok=$ok fail=$fail concurrency=$concurrency timeout=${timeout_s}s"
echo "Wall: ${wall_s}s  Throughput: ${reqps} req/s"
echo "Latency (curl time_total, ok-only): p50=$(pct 50)s p95=$(pct 95)s p99=$(pct 99)s"
