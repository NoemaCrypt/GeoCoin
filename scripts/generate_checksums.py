#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate a SHA256SUMS file for GeoCoin release assets.")
    parser.add_argument("output", help="Output file path")
    parser.add_argument("artifacts", nargs="+", help="Artifact files to hash")
    parser.add_argument(
        "--base-dir",
        default=None,
        help="Optional base directory used to store relative file names in the checksum list",
    )
    args = parser.parse_args()

    output = Path(args.output).resolve()
    artifacts = [Path(item).resolve() for item in args.artifacts]
    base_dir = Path(args.base_dir).resolve() if args.base_dir else None

    lines: list[str] = []
    for artifact in artifacts:
        if base_dir is not None:
            try:
                name = str(artifact.relative_to(base_dir))
            except ValueError:
                name = artifact.name
        else:
            name = artifact.name
        lines.append(f"{sha256_file(artifact)}  {name}")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
