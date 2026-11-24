# GeoCoin — technical documentation (overview)

> Русская версия: [TECHNICAL_OVERVIEW_RU.md](TECHNICAL_OVERVIEW_RU.md)

**Date:** April 16, 2026

This document is the "entry point" for developers: where things live, which
subsystems exist in the node, and which formats/contracts are considered
important.

The architecture page with the main data-flow and layer mapping:
[ARCHITECTURE.md](ARCHITECTURE.md).
RPC reference by section (RU/EN): `rpc/README_RU.md`, `rpc/README_EN.md`.

## Repository layout

- The repository root is the main C++ project (node, consensus, storage,
  RPC, Qt wallet).
- `libgeo/` — C SDK for external clients.

Quick start for building and running: [build-and-run.md](build-and-run.md).

## Node code structure (C++)

The main modules live in `../src/geocoin/`:

- `../src/geocoin/core/` — base structures (blocks/transactions/serialization).
- `../src/geocoin/consensus/` — consensus parameters and rules.
- `../src/geocoin/chain/` — chain handling (indexes, best-chain selection,
  links).
- `../src/geocoin/state/` — `ChainState`, block application, UTXO/accounting
  (depending on the model), background tasks.
- `../src/geocoin/storage/` — block/metadata storage (RocksDB), including
  witness pruning.
- `../src/geocoin/network/` — p2p, protocol, synchronization.
- `../src/geocoin/rpc/` — RPC surface of the node and the wallet.
- `../src/geocoin/crypto/` — signatures and cryptographic primitives
  (including Falcon-512 via liboqs/oqs/pqclean).
- `../src/geocoin/node/` — assembling subsystems into a runnable daemon
  (`geocoin-node`; `degree_node` is a compatible alias).
- `../src/geocoin/wallet/` — wallet (keys, accounts, transactions,
  RPC/GUI integration).
- `../src/geocoin/nft/` — NFT logic (if enabled by the activation manifest).

## Key formats and contracts

### Falcon-512 signature format

The signature packing format and size checks are implemented in:

- `../src/geocoin/crypto/signatures.cpp`

Implementation: `../src/geocoin/crypto/signatures.cpp`.

### Witness pruning

The mechanics of removing witness data for old blocks (pruned nodes), the
separate payload, and integrity verification:

- `../src/geocoin/storage/block_storage.cpp`

Implementation: `../src/geocoin/storage/block_storage.cpp`,
`../src/geocoin/state/state.cpp`.

## Build, run, operations

- Build/run: [build-and-run.md](build-and-run.md)
- Production checklist: [production-mainnet.md](production-mainnet.md)
- Seed runbook: [seed-node-runbook.md](seed-node-runbook.md)

## Related documents

- Blockchain capabilities (RU): [BLOCKCHAIN_CAPABILITIES_RU.md](BLOCKCHAIN_CAPABILITIES_RU.md)
- DAA and the boundaries of the PoW security model: [BLOCKCHAIN_CAPABILITIES_RU.md](BLOCKCHAIN_CAPABILITIES_RU.md)
- SDK (`libgeo`): [`../../libgeo/README.md`](../../libgeo/README.md)
- Holder dividends (EN/RU): [INTEREST_POOL.md](INTEREST_POOL.md),
  [INTEREST_POOL_RU.md](INTEREST_POOL_RU.md)
