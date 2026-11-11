#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def display_path(path: Path, base_dir: Path | None) -> str:
    if base_dir is None:
        return str(path)
    try:
        return str(path.relative_to(base_dir))
    except ValueError:
        return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate a simple GeoCoin release manifest.")
    parser.add_argument("output", help="Path to the output JSON manifest")
    parser.add_argument("artifacts", nargs="+", help="Artifact files to include in the manifest")
    parser.add_argument(
        "--base-dir",
        default=None,
        help="Optional base directory used to store relative artifact paths in the manifest",
    )
    parser.add_argument("--source-commit", help="Exact tested Git commit used by every platform build")
    parser.add_argument("--release-tag", help="Release tag or manual artifact label")
    args = parser.parse_args()
    if args.source_commit and not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", args.source_commit):
        parser.error("--source-commit must be a full Git commit hash")

    output = Path(args.output).resolve()
    artifacts = [Path(item).resolve() for item in args.artifacts]
    base_dir = Path(args.base_dir).resolve() if args.base_dir else None

    manifest = {
        "schema_version": 1,
        "artifacts": [],
    }
    if args.source_commit:
        manifest["source_commit"] = args.source_commit
    if args.release_tag:
        manifest["release_tag"] = args.release_tag

    for artifact in artifacts:
        manifest["artifacts"].append(
            {
                "path": display_path(artifact, base_dir),
                "name": artifact.name,
                "size_bytes": artifact.stat().st_size,
                "sha256": sha256_file(artifact),
            }
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
