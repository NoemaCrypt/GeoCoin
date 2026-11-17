"""Exercise offline CPU hashing and bounded stdin stop, without RPC or user data."""

import json
import subprocess
import sys
import time


def main():
    executable = sys.argv[1]
    threads = sys.argv[2] if len(sys.argv) > 2 else "2"
    command = [executable, "--benchmark", "--timeout", "30", "--threads", threads, "--control-stdin"]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.5)
        stopped_at = time.monotonic()
        output, diagnostics = process.communicate("stop\n", timeout=5)
        stop_seconds = time.monotonic() - stopped_at
    except BaseException:
        process.kill()
        process.communicate()
        raise
    if process.returncode:
        raise AssertionError(diagnostics)
    result = json.loads(output)
    assert result["result"] == "ok", result
    assert result["mode"] == "cpu-benchmark", result
    assert result["stop_reason"] == "stdin", result
    assert result["hashes"] > 0, result
    assert result["active_threads"] == 0, result
    assert result["spawned_threads"] == len(result["worker_bindings"]) > 0, result
    assert result["spawned_threads"] == min(int(threads) or result["allowed_threads"], result["allowed_threads"]), result
    assert sum(worker["hashes"] for worker in result["worker_bindings"]) == result["hashes"], result
    assert result["stop_latency_ms"] < 5000 and stop_seconds < 5, result
    print(f"test_cpu_rpc_benchmark: ok (workers={result['spawned_threads']}, "
          f"hashes={result['hashes']}, stop_ms={result['stop_latency_ms']})")


if __name__ == "__main__":
    main()
