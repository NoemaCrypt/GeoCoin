#!/usr/bin/env python3
"""Run real Node shutdown scenarios in bounded, isolated processes."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    reports = []
    for scenario in ("config", "connect", "start", "drain", "connect-only", "seed-targets", "bootstrap"):
        with tempfile.TemporaryDirectory(prefix="geocoin-node-shutdown-") as directory:
            root = Path(directory)
            report_path = root / "report.json"
            try:
                process = subprocess.run(
                    [str(binary), scenario, str(root / "data"), str(report_path)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    timeout=10,
                    check=False,
                )
                log = process.stdout
                report = json.loads(report_path.read_text()) if report_path.exists() else {
                    "passed": 0, "failed": 1, "error": "child did not produce a report"
                }
                report["returncode"] = process.returncode
                if process.returncode and not report["failed"]:
                    report["failed"] = 1
            except subprocess.TimeoutExpired as error:
                output = error.stdout or b""
                log = output.decode(errors="replace") if isinstance(output, bytes) else output
                report = {"passed": 0, "failed": 1, "error": "shutdown deadlocked or exceeded 10 seconds"}
            report["scenario"] = scenario
            reports.append(report)
            print(f"{scenario}: {report['passed']} passed, {report['failed']} failed", flush=True)
            if report["failed"]:
                print(log[-6000:])
                if "error" in report:
                    print(report["error"])
            if args.output_dir:
                args.output_dir.mkdir(parents=True, exist_ok=True)
                (args.output_dir / f"{scenario}.log").write_text(log)
                (args.output_dir / f"{scenario}.json").write_text(json.dumps(report, indent=2) + "\n")
    result = {
        "scenarios": reports,
        "passed": sum(report["passed"] for report in reports),
        "failed": sum(report["failed"] for report in reports),
    }
    if args.output_dir:
        (args.output_dir / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Node shutdown: {result['passed']} passed, {result['failed']} failed")
    return int(result["failed"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
