# GeoCoin production testing

> Русская версия: [PRODUCTION_TESTING_RU.md](PRODUCTION_TESTING_RU.md)

## Status and boundaries of the claim

The local four-node run and the full CTest suite pass. This is strong
evidence that the current branch works, but it is not a mathematical "100%
guarantee" and not a mainnet readiness certificate. No single test suite can
produce such a statement: multi-host soak runs, long generative campaigns,
disk-fault drills, a cross-platform matrix, and an independent audit remain
before a public launch.

The isolated `degree-load` network deliberately uses a separate genesis and a
fixed trivial difficulty of `0x207fffff`. These parameters are for tests only
and must never appear in a production manifest or a mainnet datadir.

## How to run the full scenario

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON
cmake --build cmake-build-release \
  --target geocoin-node geocoin-rpc-miner geocoin-loadnet-stress \
  --parallel
cargo run --release --manifest-path tools/loadnet-stress/Cargo.toml -- \
  --node-binary ./cmake-build-release/geocoin-node \
  --miner-binary ./cmake-build-release/geocoin-rpc-miner \
  --work-dir ./loadnet-runs/full-1000-$(date +%Y%m%d-%H%M%S)
```

The run directory must be new or empty. The runner never overwrites an
existing datadir. Defaults: 4 nodes, 1,000 addresses, 16 worker threads, 2
load rounds, restart, `SIGKILL`, reorg, and a set of invalid P2P inputs.

A final `PASS` is allowed only if:

- every scenario and coverage gate is `true`;
- all unexpected errors are zero;
- every accepted transaction is confirmed;
- the tip, height, and mempool of all nodes have converged;
- all processes are alive and the components are healthy;
- regular P2P relay has converged without a single `p2p_repair`;
- the monitor observed no failure outside the explicitly marked
  restart/partition windows;
- the logs contain no unexplained `WARN`, `ERROR`, or `FATAL`;
- the transaction p99 and the maximum monitored RPC latency stay within the
  configured budgets;
- secret files are inaccessible to group/other, and canary searches find no
  RPC passwords in the logs.

Direct block repair is retained only for collecting extra diagnostics. If it
was needed, the network may still recover, but the verdict is `FAIL` anyway.

## Live scenarios actually executed

### Genesis, foundation, and initial distribution

- launching four separate archive nodes on random loopback ports;
- each node verifying the separate genesis and ruleset of the `degree-load`
  network;
- verifying a single `4-of-4` foundation and all four managing addresses;
- 104 blocks with a single CPU thread to reach maturity `100`;
- four independent fan-out transactions;
- creating, verifying uniqueness, persisting, and funding 1,000 addresses;
- verifying the fixed test difficulty on live blocks.

### RPC and privilege separation

- `readonly` reads the chain but cannot stop the node or modify the wallet;
- `miner` reads the mempool but has no wallet access;
- missing and wrong credentials return HTTP 401;
- a parse error reveals neither credentials nor a secret body;
- a batch exactly at the limit is accepted; a batch above the limit is
  rejected in full;
- excessive JSON depth is rejected;
- an empty batch, a scalar request, and a wrong JSON-RPC version are rejected;
- the JSON complexity limit stops an overly branched body before dispatch;
- a live raw-TCP HTTP corpus checks `GET` (405), a wrong path (404), DNS
  rebinding via `Host` (421), and a non-JSON `Content-Type` (415);
- duplicate `Content-Length`, `Host`, and `Authorization` headers, plus an
  invalid header name, are rejected before dispatch;
- additionally rejected: a repeated `Content-Type`, an empty `Host`, a folded
  header, an ambiguous request line, an unknown HTTP version, an overly long
  URI, a non-numeric `Content-Length`, and an unsupported chunked encoding;
- the combination `Transfer-Encoding + Content-Length` is rejected;
- a declared body of 999,999,999 bytes yields 413 before reading, and wrong
  authorization with an incomplete body yields 401 without waiting for the
  body;
- duplicate JSON object keys, including nested ones, and an object-valued
  JSON-RPC `id` are rejected without invoking the command;
- RPC connection limits and rate-limiting are off by default (`=0`); the live
  scenario holds four unfinished loopback connections and verifies that a
  fifth is accepted, while partial headers/bodies are closed on deadline;
- after releasing the stuck connections, RPC capacity fully recovers;
- after the entire set of invalid inputs the node answers healthy and keeps
  `Cache-Control: no-store` and `X-Content-Type-Options: nosniff`;
- 32-byte RPC passwords are generated from `OsRng`, stored in separate
  private files, and verified by a canary search across all logs.

### Transaction parsing and building

- an empty, odd-length, and non-hex raw transaction;
- a truncated header, a truncated witness, and extra trailing bytes;
- a non-canonical address, a wrong payload length, and a `GEO`/`GEOM` version
  mismatch;
- a wrong destination via both the wallet and the raw builder;
- empty inputs/outputs, a zero output, `vout` and `locktime` overflow;
- input and output counts above the consensus limit;
- duplicated inputs inside one transaction;
- a missing or already-spent UTXO;
- an attempt to sign with someone else's wallet;
- a corrupted Falcon signature.

### Monetary and policy boundaries

- insufficient balance and a fee below the minimum;
- an output below dust and an output exactly at the dust boundary;
- the sum of outputs exceeding the inputs;
- the sum of outputs exceeding `MAX_MONEY`;
- a self-transfer with a repeated destination;
- a valid fan-out to 33 outputs;
- exactly 1,000 outputs accepted and confirmed;
- 1,001 inputs/outputs rejected by the builder/consensus;
- the largest Falcon transaction that actually fits is accepted;
- the next input that pushes the signed transaction above 100,000 bytes is
  rejected even after the witness is added;
- `testmempoolaccept` at the exact batch limit does not touch the mempool;
- a batch above the limit is rejected;
- a rejected preflight leaves no side effects.

### Concurrency and load

- a mempool double-spend;
- two conflicting spends submitted simultaneously: exactly one winner;
- a concurrent duplicate storm stays idempotent;
- a single-origin transaction reaches all other nodes via P2P;
- replaying a confirmed boundary-valid transaction is rejected by every node;
- the replay of a confirmed boundary transaction is verified separately;
- two rounds of 1,000 signed transfers on 16 threads;
- a random recipient pattern and a hotspot pattern onto one address;
- continuous samples of height, tip, mempool, peer count, health, and RPC
  latency;
- bounded backpressure prevents the client from overloading the RPC
  connection limit itself and masking node/mempool behavior.

### P2P, sync, storage, and reorg

- a full mesh with three authenticated encrypted peers per node;
- self-driven relay of transactions and blocks without RPC repair;
- a graceful restart of a follower with wallet, UTXO, resync, and tip checks;
- a forced `SIGKILL` of a follower after durable writes;
- cold RocksDB/WAL recovery with checks of the previous tip, wallet, UTXO,
  and health;
- a real network split into `2 + 2`, two competing forks, and selection of
  the fork with more chainwork;
- UTXO/state rollback and returning a transaction from an orphaned block to
  the mempool;
- re-confirmation of the resurrected transaction on the winning fork;
- four independent pre-auth P2P cases on different nodes: unknown message
  type, a foreign network magic, a wrong fixed `CHALLENGE` length, and a
  mismatching checksum of a size-valid frame;
- each P2P case increments the exact policy counter, closes the connection,
  creates a temporary ban, and the node stays healthy with three
  authenticated honest peers;
- after every restart or split scenario, identical tip/height, an empty
  mempool, and live processes are verified.

## Additional automated suites

The full debug CTest suite contains 47 tests: consensus, transaction, wallet,
chainstate revert, persistence, storage, RPC auth/framing/payload, a
deterministic parser fuzz, secure-channel tamper/replay/downgrade, peer
policy, sync, node messages, GUI smoke, RPC localnet, P2P clusters on 2/3/4
nodes, and a full sidechain-v3 localnet.

### NFT, transactions, and state integrity

The NFT matrix covers all supported `CREATE`, `TRANSFER`, `BURN` transitions
and the cadastral `TRANSFER` with automatic object removal:

- an empty, wrong, and foreign-address signature;
- a changed recipient, fee, creator, royalty, metadata, and
  proof-of-location data after signing;
- a mismatch between the declared `TRANSFER`/`BURN` fee and the actual
  difference of UTXO inputs and outputs;
- a repeated input, a missing/future/immature/negative UTXO, overflow of the
  input or output sum, a negative output, an output sum above the inputs, and
  a new-UTXO key collision;
- a full `CREATE -> TRANSFER -> BURN` cycle inside one block, a clean
  rollback, re-applying the same block, and rolling back all three transitions
  when the last transaction is invalid;
- a permanent NFT id reservation after `BURN` and its correct removal when
  the `CREATE` itself is rolled back;
- separate behavior of ordinary geographic NFTs and cadastral parcels: an
  ordinary object does not reserve a cadastral number or land geometry;
- errors and exceptions when writing metadata, owner, the uniqueness index,
  the raw transaction, removing the input UTXO, and writing the output UTXO;
- a successful compensation with byte-equal UTXOs before/after the failure,
  and the `rebuild-required` mode if the backend did not confirm recovery;
- three sequential cold opens of a real RocksDB after `CREATE`, `TRANSFER`,
  and `BURN`, with checks of height, tip, owner, canonical metadata,
  reservation, transaction history, and the full UTXO snapshot.

GCC coverage of `src/geocoin/nft/nft_validator.cpp` after this matrix:
`329/329` executable source lines (`100.00%`), `98.81%` of branches executed,
and `62.26%` of branch directions taken at least once. The last two numbers
include compiler-generated standard-library and exception-handling paths, so
the claim is limited to exact line coverage and the explicitly listed
semantic conditions — not a promise to enumerate the infinite input-byte
space.

For `src/geocoin/core/transaction.cpp` the same fresh run gives `99.72%` of
source lines and `94.75%` of branches. Left uncovered: a defensive throw for
the case when the strict canonical parser unexpectedly returns a
non-canonical representation, an internal Falcon-signer failure after the
32-byte key check has already passed, and two compiler-generated
exception-only blocks. These paths cannot be reached correctly through a
public entry point without a special cryptographic backend substitution; they
are kept as protection against future changes, not hidden for the sake of a
formal percentage.

Separate ChainState regressions verify serialization of a concurrent
`revert_block`, early validation of the undo envelope/interest delta, a
`commit_batch` failure, runtime poison before rebuild, and degraded
readiness in `gethealth`. Separate process-crash tests terminate a child
process via `_exit()` without destructors and then reopen the real split
RocksDB databases:

- an unfinished async write keeps the recovery marker and triggers a rebuild;
- the transition from IBD to synchronous mode flushes the previous
  WAL-disabled records of both databases to disk before the marker is
  removed;
- the block body, the active tip, UTXOs before/after the transition, and the
  journal are verified after a cold open;
- UTXOs of an unfinished snapshot, old undo records, and epoch indexes are
  removed from the real split RocksDBs and are not loaded into memory;
- a partial metadata-cleanup failure does not remove the marker and puts the
  derived state into `rebuild-required`.

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j1
```

Critical C++ paths are additionally checked under AddressSanitizer and
UndefinedBehaviorSanitizer:

```bash
cmake -S . -B cmake-build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DGEOCOIN_SANITIZER=address-undefined
cmake --build cmake-build-asan --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir cmake-build-asan --output-on-failure
```

The Rust runner must pass formatting, lint without warnings, and unit tests:

```bash
cargo fmt --manifest-path tools/loadnet-stress/Cargo.toml -- --check
cargo clippy --manifest-path tools/loadnet-stress/Cargo.toml \
  --all-targets -- -D warnings
cargo test --manifest-path tools/loadnet-stress/Cargo.toml
```

ThreadSanitizer is a separate mandatory gate on a compatible Linux runner. If
GCC TSan exits before `main()` with `unexpected memory mapping`, the result
is counted as `BLOCKED`, not `PASS`; the test must be retried on a runner
without shadow-memory conflicts, if necessary under `setarch x86_64 -R`.

## Last confirmed run

Date: 2026-08-16. Artifact:
`loadnet-runs/full-1000-integrity-20260816-v2/report.json`.
Its SHA-256:
`7a349eac40b08ce5dbf45b18e54e5958e783172c62cba8d588b36bec400335a0`.

- verdict: `PASS`;
- 4 nodes, 1,000 addresses, 116 mined blocks;
- 89 unique live scenarios and 26 coverage gates passed;
- 2,010 valid-attempts, 2,009 accepted, 2,009 confirmed;
- 65 expected rejections, 0 unexpected transaction failures;
- 8,017 broadcast-copy observations;
- latency p50/p95/p99: `32/56/70 ms`;
- `p2p_repairs = 0`;
- SIGKILL recovery of node 2: `48,936 ms`;
- 239 monitor samples; 227 failures inside the explicitly marked
  restart/partition windows and 0 failures outside them;
- maximum successful monitored RPC latency: `1,974 ms` against a budget of
  `10,000 ms`;
- fatal errors: `0`;
- 456 `WARN` and 2 `ERROR` entries classified as expected negative, restart,
  and partition events; no unexplained findings.
- Release CTest after the latest fix: `47/47`, `162.79 s`; Rust
  fmt/clippy: clean, unit tests: `7/7`; ASan+UBSan critical subset: `4/4`,
  `20.20 s`, plus the previously confirmed mining/hash backend subset:
  `3/3`, `1.95 s`;
- the TSan target was built, but the runtime was blocked by the environment
  before `main()`: `FATAL: ThreadSanitizer: unexpected memory mapping`.

## Defects found and fixed

- the address decoder accepted payloads with extra bytes and did not bind
  the prefix to the version byte;
- mempool policy could let an invalid output address through via an
  exemption;
- the RPC builder narrowed a 64-bit `locktime` to `uint32_t`;
- the raw transaction size was checked before the Falcon witness but could
  exceed the limit after signing;
- private keys/intermediate signing buffers were not wiped on all exit paths;
- sync could request a child of the winning fork before the missing parents;
- `GETHEADERS` accepted a locator from a side branch as an anchor of the
  active chain;
- a transaction entering the mempool during disconnect/auth could be missed
  by the relay queue of the new peer;
- an honest burst of `GETDATA`/`BLOCK` during normal sync was mistakenly
  rate-limited with a disconnect;
- relay sent INV per transaction and created an INV flood by itself;
- RPC `addnode ... onetry` did not actually bypass the accumulated reconnect
  backoff;
- the test runner masked a P2P relay failure via direct repair and could
  still award a `PASS`;
- the monitor counted all RPC failures of a once-restarted node as expected,
  even if the failure happened outside the chaos window;
- two different confirmed-replay scenarios shared the same ID;
- predictable test credentials, a short cold-start timeout, and the loss of
  `account_count` in a late-failure report were fixed.
- `ChainState::revert_block` and the public save/load of undo did not take
  the shared recursive mutex, allowing a race with apply/query;
- interest-pool underflow on revert was checked after the transactional
  mutations; now the entire undo envelope and delta are validated before
  them;
- a revert transaction/`commit_batch` failure could leave a partially
  modified in-memory state while the node still looked healthy; runtime
  state poison now blocks new transitions until a rebuild, and `gethealth`
  returns `rebuild-required`, `ready=false`, `status=degraded`;
- the recovery marker could be removed after a WAL-disabled commit before
  both split RocksDBs were actually on disk; removal is now possible only
  after a confirmed durable commit;
- the first synchronous commit after leaving IBD did not flush old
  WAL-disabled records; both databases are now explicitly flushed;
- rejecting the next block with a clean rollback could remove the marker of
  an earlier not-yet-durable commit; the marker is now preserved;
- a partial metadata-cleanup failure could remove the only sign of an
  unfinished recovery; the marker is now removed last and only after a
  successful cleanup;
- the rebuild cleared the height and specialized indexes but could leave
  UTXOs, undo records, and old epoch indexes of an unfinished snapshot; they
  are now recognized separately, removed before continuing, and verified
  after a cold open;
- the live runner checked only JSON-RPC over an HTTP client and a single P2P
  malformed header; raw HTTP framing, complexity/connection/deadline limits,
  and four independent P2P scenarios were added;
- the first checksum-check variant used a PING with an invalid payload and
  hit an earlier size check; the test was moved to a size-valid VERSION
  frame, so it now unambiguously exercises the checksum path.
- removing an NFT from the owner/tags indexes used a range from the found
  element to the end and could erase references to all following NFTs; now
  only the exact match is removed, and duplicate tag indexes are not created;
- NFT geometry used floating-point division and a platform-dependent wide
  type; area, edge intersections, boundary hits, and the cadastral tolerance
  are now computed with exact 128-bit integer arithmetic;
- an NFT polygon could be self-intersecting with a non-zero shoelace area; a
  mandatory simple-polygon check was added;
- the creator, royalty, metadata, and Proof-of-Location fields were not
  previously bound by one canonical signable representation; both modes now
  use the full `serialize_for_signing()`, and changing any field breaks the
  signature;
- duplicate tags, negative cadastral values, and contradictory
  `has_buildings/building_count` fields are now rejected before registration;
- `UPDATE_FOUNDATION_ADDRESS` without a single signature passed the early
  `verify()` and was rejected only by the state-dependent check; an empty set
  is now rejected immediately, while the quorum and composition are still
  checked by ChainState;
- the NFT loader is now explicitly verified against owner-without-metadata,
  metadata-without-owner, a wrong owner address, corrupted bytes, and a
  mismatching canonical id; incomplete pairs are removed, and the snapshot is
  marked as requiring a rebuild;
- burn removes owner/metadata but keeps the permanent NFT id reservation;
  re-issuing the same canonical NFT is rejected, and a rollback restores the
  owner and metadata.
- the standalone NFT validator verified the owner of `TRANSFER`/`BURN` but
  not the cryptographic signature of the corresponding payload; the signature
  and signer address are now mandatory at both verification levels;
- an ordinary geographic NFT could erroneously block a cadastral parcel with
  intersecting geometry; cadastral number and boundary comparisons now run
  only between cadastral objects;
- NFT owner/metadata write results were ignored, and a UTXO could change in
  memory after a partial backend failure; persistent changes are now prepared
  before publication in memory, compensated on failure, and put the node into
  `rebuild-required` if exact recovery is not confirmed;
- a clean rollback removed the UTXO and NFT state but kept the raw
  transaction record, so a standalone ChainState could reject re-applying the
  same valid block; the raw index of ordinary/NFT transactions is now removed
  together with the rolled-back transaction. Checkpoint records are
  deliberately kept as history for verifying conflicting checkpoints after a
  reorganization and are not considered part of the active chain by public
  lookups.

## Open risks before production

1. The entire live run was executed on a single Linux host over loopback.
   There is no real WAN latency, packet loss, reordering, bandwidth caps,
   NAT, IPv6, or multi-region partition.
2. Cold recovery took 50.1 seconds even on a small test chain. RocksDB/WAL
   profiling, a recovery SLO, and validation at production volume are needed.
3. The local TSan run of wallet/mempool/sync now passes; the nightly job
   repeats it on a clean runner. Before release, the suite still needs long
   multithreaded node scenarios and the GUI.
4. Separate instrumented targets and a daily run with accumulated inputs are
   in place for P2P, RPC, raw transactions, blocks, and sidechain proofs. A
   longer continuous campaign is still required before release.
5. A sharp process termination between the writes of the two RocksDBs and a
   cold reopen are now verified automatically. Real `ENOSPC`, a read-only
   filesystem, corrupted SST/WAL, a power cut at different commit points, a
   slow disk, and file-descriptor exhaustion are still uncovered.
6. There is no chaos matrix of clock skew/NTP jumps, long link loss,
   half-open TCP, churn, and mass traffic from different IPs.
7. There is no 24/72-hour soak with continuous mining, reorgs, restarts, and
   millions of transactions; a short loopback test does not reveal leaks and
   time degradation.
8. An identical state snapshot is now verified after a 101-block replay on
   MemoryStorage, plain and split RocksDB, after a reorg and a restart;
   locally it matched in GCC and Clang. The full Windows and
   upgrade/rollback matrix has not been executed yet.
9. The logs are too noisy during expected negative tests, and some Russian
   RPC messages display as `????`. This hinders production alerting and
   forensic analysis.
10. Loadnet runs the wallet with explicitly allowed plaintext storage. For
    real funds, verified encryption-at-rest, backup/restore, rotation,
    hardware-backed key custody, and a ban on test-environment flags are
    needed.
11. An independent audit of consensus/state/wallet/RPC/P2P, a dependency/SBOM
    review, reproducible signed builds, and a responsible-disclosure plan are
    needed.

## Recommended release gates

- Every PR: full CTest, Rust fmt/clippy/tests, a whitespace patch check, and
  a secret scan.
- Nightly: a full 1,000-account loadnet, sidechain-v3/NFT/multisig localnet,
  ASan/UBSan, TSan, and three instrumented generative runs with artifacts.
- Weekly: a longer continuous campaign, multi-host `tc netem` chaos, and real
  disk-fault scenarios.
- Before a release candidate: a 72-hour soak, cross-platform consensus
  replay, fresh-node sync, upgrade/rollback, backup/restore, and a dependency
  scan.
- Before mainnet: resolve all critical/high findings of an independent
  audit, complete the `docs/production-mainnet.md` checklist, and archive a
  signed report package with the exact commit hash.
