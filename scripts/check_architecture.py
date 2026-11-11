#!/usr/bin/env python3
"""Enforce GeoCoin's module and public/private include boundaries (stdlib only)."""

import argparse
from pathlib import Path
import re
import sys


# Allowed directions, not an inferred graph: a new dependency needs review here.
# Core/consensus/NFT currently share value types. Blockchain orchestration is a
# separate module despite its historical location under core/.
ALLOWED = {
    "types": {"utils"},
    "geography": set(),
    "utils": {"types"},
    "crypto": {"types", "utils"},
    "core": {"types", "utils", "crypto", "consensus", "nft"},
    "chain": {"types", "core"},
    "consensus": {"types", "utils", "core", "chain", "crypto", "nft", "geography"},
    "nft": {"types", "utils", "crypto", "consensus", "geography"},
    "multisig": {"types", "core"},
    "storage": {"types", "utils", "core", "crypto", "consensus"},
    "state": {"types", "utils", "core", "crypto", "consensus", "nft", "multisig", "storage"},
    "blockchain": {"types", "utils", "core", "chain", "consensus", "state", "storage"},
    "services": {"types", "core", "consensus"},
    "network": {"types", "utils", "core", "chain", "crypto", "consensus", "state", "blockchain", "services"},
    "wallet": {"types", "utils", "core", "crypto", "consensus", "nft", "multisig", "state", "services"},
    "node": {"types", "utils", "core", "crypto", "consensus", "state", "storage", "blockchain", "network", "services"},
    "rpc": {"types", "utils", "core", "crypto", "consensus", "state", "storage", "blockchain", "network", "wallet", "node", "services", "nft", "geography"},
    "b2b": {"types", "utils", "crypto"},
    "benchmark": {"types", "crypto", "storage"},
}
NODE_API = {"geocoin/node/node_services.hpp", "geocoin/node/node_types.hpp"}
# RPC consumes NFT values and the stateless public geodesy API. Registry and
# validation internals remain behind the core/state/wallet services.
RPC_NFT_API = {"geocoin/nft/nft_types.hpp", "geocoin/nft/geodesy.hpp", "geocoin/nft/geometry_codec.hpp"}
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)


def module(path: str) -> str:
    parts = path.split("/")
    if parts[1] == "types.hpp":
        return "types"
    if parts[1] == "core" and parts[-1].startswith("blockchain"):
        return "blockchain"
    return parts[1]


def check(root: Path) -> list[str]:
    errors = []
    for base in (root / "include/geocoin", root / "src/geocoin"):
        if not base.is_dir():
            errors.append(f"Missing source directory: {base}")
            continue
        for source in sorted(base.rglob("*")):
            if source.suffix not in {".h", ".hpp", ".cpp"}:
                continue
            relative = source.relative_to(root).as_posix()
            owner = module(source.relative_to(base.parent).as_posix())
            if owner not in ALLOWED:
                errors.append(f"{relative}: module {owner!r} needs an explicit dependency policy")
                continue
            # Preserve line numbers while ignoring commented-out includes.
            content = re.sub(r'/\*.*?\*/', lambda m: '\n' * m[0].count('\n'),
                             source.read_text(encoding="utf-8"), flags=re.DOTALL)
            for match in INCLUDE.finditer(content):
                inc = match[1]
                line = content.count("\n", 0, match.start()) + 1
                location = f"{relative}:{line}"
                public = root / "include" / inc
                private = root / "src" / inc
                if not inc.startswith("geocoin/"):
                    # Catch relative paths that reach into another module's internals.
                    resolved = (source.parent / inc).resolve()
                    if not resolved.is_relative_to((root / "src/geocoin").resolve()):
                        continue
                    inc = resolved.relative_to((root / "src").resolve()).as_posix()
                    private = resolved
                    public = root / "include" / inc
                dependency = module(inc)
                if dependency != owner and dependency not in ALLOWED[owner]:
                    errors.append(f"{location}: forbidden dependency {owner} -> {dependency} ({inc})")
                if private.is_file() and not public.is_file():
                    if base == root / "include/geocoin":
                        errors.append(f"{location}: public header exposes implementation header {inc}")
                    elif dependency != owner:
                        errors.append(f"{location}: private header crosses module boundary: {inc}")
                if owner == "rpc" and dependency == "node" and inc not in NODE_API:
                    errors.append(f"{location}: RPC must depend on node_services/node_types, not {inc}")
                if owner == "rpc" and dependency == "nft" and inc not in RPC_NFT_API:
                    errors.append(f"{location}: RPC may use NFT value types only, not {inc}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root.resolve())
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Architecture boundaries passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
