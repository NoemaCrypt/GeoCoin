#!/usr/bin/env python3
"""Export and compare the frozen geographic consensus v1 conformance corpus."""
import argparse
import json
from pathlib import Path
import subprocess


EXPECTED = {
    "rules_version": 1,
    "library_version": "2.3.0",
    "cases": 10020,
    "sha256": "9f617a74706e39f5874a9e0495a07d71b502ac8fe751cbd6da7242ff2cd25aaf",
}


def validate(record):
    if not isinstance(record, dict) or not isinstance(record.get("platform"), str) or not record["platform"]:
        raise ValueError("Missing fingerprint platform")
    for key, value in EXPECTED.items():
        if type(record.get(key)) is not type(value) or record[key] != value:
            raise ValueError(f"{record['platform']}: {key} differs: {record.get(key)!r}, expected {value!r}")
    return record["platform"]


def compare(directory, required):
    paths = sorted(directory.rglob("*.json"))
    if not paths:
        raise ValueError("No geographic fingerprints found")
    platforms = set()
    for path in paths:
        platform = validate(json.loads(path.read_text(encoding="utf-8")))
        if platform in platforms:
            raise ValueError(f"Duplicate fingerprint: {platform}")
        platforms.add(platform)
    if required and platforms != set(required):
        raise ValueError(f"Expected platforms {sorted(required)}, received {sorted(platforms)}")
    print(f"Geographic consensus v1 agrees on {len(platforms)} platforms: {', '.join(sorted(platforms))}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--binary", type=Path)
    mode.add_argument("--directory", type=Path)
    parser.add_argument("--platform")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-platform", action="append", default=[])
    args = parser.parse_args()
    try:
        if args.binary:
            if not args.platform or not args.output:
                parser.error("--binary requires --platform and --output")
            result = subprocess.run([str(args.binary.resolve()), "--fingerprint", args.platform],
                                    check=True, capture_output=True, text=True, timeout=120)
            record = json.loads(result.stdout)
            if validate(record) != args.platform:
                raise ValueError("Binary reported an unexpected platform")
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(record, sort_keys=True) + "\n", encoding="utf-8")
            print(f"{args.platform}: {record['sha256']}")
        else:
            compare(args.directory, args.require_platform)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Geographic conformance failed: {error}\n")


if __name__ == "__main__":
    main()
