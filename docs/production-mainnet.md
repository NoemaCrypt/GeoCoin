# GeoCoin Production Mainnet Checklist

## Build Entry Points
- Windows: set `VCPKG_ROOT` to a path without `+`, then use `cmake --preset windows-clang-static-release-headless -S .` or `cmake --preset windows-clang-static-release-gui -S .`.
- Linux/WSL Ubuntu 22.04: run `./scripts/bootstrap-ubuntu-22.04.sh` for a headless node, or add `--with-gui` before `cmake --preset linux-release-gui -S .`. Headless configure is `cmake --preset linux-release-headless -S .`.
- `libgeo`: use the presets under `libgeo/CMakePresets.json`.

## Runtime Contracts
- `activation-manifest.json` is required in the network-scoped datadir (for production: `/var/lib/geocoin/degree`). If it is missing, GeoCoin creates a versioned production manifest with risky features disabled for local transaction admission and construction. This operator-controlled file never changes block validity.
- `geocoin-core --smoke-test -no-auto-connect` is the canonical GUI smoke contract.
- `geocoin-core --gui-e2e-test --data-dir <temp> -addnode=node1.geocoin.network:3030 -addnode=node2.geocoin.network:3030 -addnode=node3.geocoin.network:3030` is the Windows GUI full-node e2e contract.
- Wallet/admin RPC stays on loopback or behind a private reverse proxy.
- P2P wire protocol v2 is mandatory: ML-KEM-768 + X25519 establishes the
  channel and ChaCha20-Poly1305 protects every frame after `SECURE_REPLY`.
  Production builds must not add a plaintext or v1 downgrade switch.

## Pre-Launch Consensus Contract
- Mainnet uses one permanent ruleset from genesis: per-block LWMA and coinbase
  maturity `100` are never selected by an absolute block height.
- The committed mainnet launch genesis is timestamp `1790421975`
  (`2026-09-26 14:26:15 Europe/Minsk`), bits `0x1d00ffff`, nonce
  `2799726649`, hash
  `00000000e2c70b6a2e23dabee76868effa1d10aaf7a965cab05b888bb43a28b7`.
- The genesis foundation policy is the public `3-of-4` multisig
  `GEOM3PyazNssCYYzF3tW9YrsSwszLq45QXbAZv`.
- Testnet has its own genesis (`0x207fffff`, nonce `0`, hash
  `62583e09574792574d8cfce966f3281ab97eb7265f41cd14dcceab2efb32f138`)
  and cannot be used as a mainnet datadir.
- Genesis is the only hardcoded checkpoint before public launch. There is no
  checkpoint or compatibility exception for the discarded test chain.
- Datadirs and seed nodes created with the discarded launch rules are
  incompatible and must be reset instead of migrated by a height-based bypass.
- Every non-empty block database carries a consensus ruleset identifier. The
  node refuses databases with a missing or mismatched identifier.
- Before opening mining, start one node from an empty datadir and require
  height/hash/chainwork equality with at least two independently initialized seeds.

## Release Gate
- Windows clang static headless build passes.
- Windows clang static GUI build passes and `geocoin-core --smoke-test -no-auto-connect` exits with code `0`.
- Windows static package contains no project/Qt/OpenSSL/RocksDB/zlib/MSVC runtime DLLs.
- Linux headless build passes.
- Linux GUI build passes and the same smoke test runs with `QT_QPA_PLATFORM=offscreen` or `xvfb-run`.
- `ctest` passes for txid, config/manifest, and wallet/chainstate integration.
- Portable Linux and Windows bundles are rebuilt locally before publish.
- Release manifest and SBOM are generated.
- Security sign-off is complete before enabling `hierarchical_multisig`, `delegation`, `nft`, or `cadastral` in production.
- `test_secure_channel_units`, `test_peer_transport_units`, `test_peer_policy`,
  and the multi-node P2P cluster tests pass, including tamper, replay,
  transcript-binding, and plaintext-downgrade rejection.

## Seed Node Contract
- Public seed hosts should resolve to archive or full-history nodes.
- Open only `TCP 3030` publicly.
- Keep RPC on `127.0.0.1` only.
- Apply `deploy/firewall/geocoin-node-ufw.sh` on every public node.
- Provider security group must allow only `22/tcp` and `3030/tcp`; `3033/tcp` must fail raw external TCP connect.
- Confirm every ready entry in `getpeerinfo` reports
  `transport_encrypted=true`, `secure_wire_version=2`, and `secure_suite=1`.
- Recommended hostnames:
  - `seed.geocoin.network`
  - `seed1.geocoin.network`
  - `seed2.geocoin.network`

## Security Sign-Off Contract
- Local admission enforcement is strict: enabling risky transaction construction/relay requires either:
  - external audit sign-off (`external_audit.required=true` and `external_audit.signed_off=true`), or
  - internal dual-signoff (`internal_signoff.signed_off=true` with at least two distinct signers).
- Risky features: `hierarchical_multisig`, `delegation`, `nft`, `cadastral`.
- Consensus rules are identical on every node regardless of this local manifest; changing it cannot activate or deactivate a consensus rule.
- Before setting sign-off to true, keep an auditable package in release artifacts:
  - auditor identity and report id,
  - commit/tag hash audited,
  - scope (consensus/state/wallet/rpc/network),
  - signed date and findings status.
- For internal sign-off, use two maintainers and record both signer IDs.
- Suggested machine-readable template: `deploy/audit-signoff.template.json`.
