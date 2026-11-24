# GeoCoin: Blockchain Capabilities (L1, Cadastre, Sidechains)

> Русская версия: [BLOCKCHAIN_CAPABILITIES_RU.md](BLOCKCHAIN_CAPABILITIES_RU.md)

## 1. What this network is

GeoCoin is a UTXO blockchain with PoW consensus (GeoPoW) focused on:

- payments and asset issuance;
- cadastral and georeferenced scenarios;
- launching user sidechains with economic anchoring to GeoCoin.

---

## 2. Base layer (L1)

### Consensus and security

- PoW with `TARGET_BLOCK_TIME = 600` seconds (10 minutes).
- Per-block dynamic difficulty (DAA) with a `DIFFICULTY_DAA_WINDOW = 60` window.
- Future block time limit: `MAX_FUTURE_BLOCK_TIME = 15 minutes`.
- `Median Time Past` check (11 blocks).
- Reorg depth limit: `MAX_REORG_DEPTH = 144`.
- Coinbase maturity: `COINBASE_MATURITY = 100`.

### Protocol economics

- Base emission: 50 GC/block, halving every 210,000 blocks.
- 90/10 model: miners receive 90%, the foundation — 10% (until the interest
  pool activation).
- After the `INTEREST_POOL_ACTIVATION_HEIGHT` height, the foundation share
  goes into the interest pool.
- Quarterly interest pool distribution: `INTEREST_DISTRIBUTION_INTERVAL = 13104`.

### DAA and the boundaries of the security model

The first block after genesis uses the initial difficulty. Afterwards the
difficulty is recalculated for every block over a rolling window of up to 60
blocks. The current implementation uses an LWMA-like calculation:

1. For neighboring blocks of the window, `Median Time Past` deltas are taken.
2. Each delta is clamped to the range from 1 second to 6 target intervals.
3. Newer intervals receive a larger weight, and the window targets are
   averaged.
4. The resulting target is constrained to a step of at most `×2` or `÷2` per
   block and to the overall PoW limit.

On mainnet there is no emergency difficulty reduction by timestamp: such a
trigger can be accelerated by an allowed timestamp shift. On testnet, a
minimal difficulty is allowed after a pause longer than two target intervals.

A new header must have a `timestamp > MTP` of the previous chain and be no
more than 15 minutes ahead of the local time of the receiving node. MTP
reduces the influence of a single manipulated timestamp, but it does not make
time trustworthy and does not fully eliminate time-warp risks — therefore the
intervals are additionally constrained inside the DAA.

The DAA stabilizes the average block interval as the total hashrate changes.
It does not equalize the income of large and small miners and does not protect
against hashrate concentration: the probability of finding a block remains
roughly proportional to the share of hashrate.

#### What actually limits chain rewrites

- Chain selection is performed by accumulated chainwork, with full
  verification of headers and blocks.
- Hard network checkpoints reject a mismatching hash at an explicitly
  specified height; currently only genesis is fixed in the parameters.
- The node keeps a local policy-checkpoint and, after a restart, must not
  select a fork diverging before it.
- Automatic reorg is limited to 144 blocks.

The last two points are subjective local policy, not consensus finality. In a
conflict they can stop the node or split nodes with different local
histories. A participant with the majority of hashrate is still able to
censor transactions, perform shallow reorgs, and build a heavier fork between
checkpoints. There is no absolute ("100%") protection against 51% in a
permissionless PoW system.

`allow-deep-reorg=1` is an emergency operator override: it disables the local
policy-checkpoint and the depth limit, but it does not allow bypassing hard
network checkpoints or consensus validation. In normal configuration it must
remain disabled.

To strengthen the network ahead of mainnet, independent miners and pool
diversification are needed, along with diverse P2P connections and eclipse
protection, monitoring of hashrate concentration/reorgs, and — if
cryptographic finality is required — a separate signed checkpoint/finality
mechanism with a predefined trust model and key rotation procedure.

The Interest Pool affects the distribution of part of the emission, but it
does not change the probability of finding a PoW block and is not a 51%
protection mechanism.

### Transaction model

- UTXO.
- Witness signature format (Falcon), with the ability to prune witnesses.
- Size and DoS limits in the consensus parameters (`MAX_BLOCK_SIZE`,
  `MAX_TX_SIZE`, inputs/outputs limits, etc.).

---

## 3. Cadastre and NFT

### CADASTRAL_DOC

- A `CADASTRAL_DOC` transaction anchors a cadastral document in L1.
- Document fields: hash, cadastral number, owner, and the full `nft_id` of
  the parcel.
- The related cadastral NFT must already exist; the document is signed by the
  current owner.
- Parcel geometry is independent of the block label.

### NFT layer

- Types: `NFT_CREATE`, `NFT_TRANSFER`, `NFT_BURN`.
- WGS84 geometry in int64 nanodegrees: multiple parts and holes, canonical
  bytes.
- GeographicLib 2.3.0: geodesic areas, distances to edges, and
  intersections.
- The official area is stored in cm²; the former comparison tolerance was 10%.
- GeoJSON, diagnostics, and preview in Qt; spatial index and cache.
- Metadata validation, including cadastral fields.
- Uniqueness checks (e.g. `cadastral_number`) and geometric correctness (by
  the NFT validator).
- Suitable for tokenizing objects (land, rights, records, digital documents).

---

## 4. Multisignature and delegation

- `ANGLE_COMMITMENT` + `MULTISIG_SPEND` for M-of-N scenarios.
- Hierarchical mode via `DELEGATION_PROOF` / `REVOKE_DELEGATION`.
- Delegation limits: by time, by amount, by % of balance, by nonce.
- Checks are factored into `MultisigVerifier`; state-dependent constraints
  live in `ChainState`.

---

## 5. Sidechains

GeoCoin supports the model: **a separate sidechain chain + collateral and
registration rules on L1**. The registry is split into two incompatible
profiles:

- `financial`: tokenomics, emission cap, `MINT_SIDECHAIN` /
  `REDEEM_SIDECHAIN`, checkpoint policy/version `2/2`;
- `accounting`: `namespace` and `schema_hash`, without emission and
  redemption, checkpoint policy/version `3/1`.

### Lifecycle

- `LOCK_DEPOSIT` — locking the collateral.
- `TOP_UP_SIDECHAIN_DEPOSIT` — topping up an existing deposit.
- `ISSUE_SIDECHAIN` — registering a financial sidechain.
- `REGISTER_ACCOUNTING_SIDECHAIN` — registering an accounting sidechain.
- `MINT_SIDECHAIN` / `REDEEM_SIDECHAIN` — issuance and redemption within the
  limit.
- `BATCH_SIDECHAIN_CHECKPOINT` — batch checkpoints into L1.
- `UPDATE_SIDECHAIN_COMMITTEE` — committee change.
- `SLASH_DEPOSIT` — deposit confiscation upon proven fraud.
- `DECOMMISSION_SIDECHAIN` — start of shutdown while keeping the
  slashable registry.
- `UNLOCK_DEPOSIT` — unlocking after `unlock_height` and a 144-block
  challenge period; removes the deposit and the registry.

### What is verified on L1

- A financial proof `SCP2` verifies supply and policy/version `2/2`.
- An accounting proof `SCA1` anchors the state root to the registered
  `schema_hash` and policy/version `3/1`, without a supply field.
- A mandatory `proof` for both batch checkpoint profiles.
- Size/frequency constraints (`MAX_BATCH_CHECKPOINT_PROOF`,
  `MIN_SIDECHAIN_CHECKPOINT_INTERVAL`).
- M-of-N committee signatures.
- Collateral and fee controls.

### What a sidechain operator can choose

- The internal rules of their own sidechain node (consensus / execution /
  state format).
- L1 validates only the "anchor" (checkpoint + proof + committee policy), not
  the entire sidechain runtime.

---

## 6. User capabilities

For a user (wallet/RPC/CLI):

- sending and receiving regular transactions;
- creating and managing multisig transactions;
- NFT operations;
- cadastral records;
- sidechain deposit and checkpoint operations (via the RPC interface);
- node and metrics monitoring (including `getmetrics`).

For an operator/organization:

- running their own sidechain under their own regulations;
- economic anchoring to GeoCoin through the deposit model;
- configuring a committee signing model and governance procedures.

---

## 7. Application areas

- State and municipal registries (cadastre, land records, rights archives).
- Corporate registries of rights and assets (object tokenization).
- Fintech infrastructure with collateralized sidechains.
- B2B document-flow automation with on-chain anchoring.
- Services that need a verifiable change history and independent audit.

---

## 8. What to consider before production

- Enabling "risky" modules (hierarchical multisig, delegation, NFT,
  cadastral) must be controlled by the `ActivationManifest`.
- A mainnet release requires release discipline: a fixed rules version, a
  test run, metrics/alerts, and a rollback plan.
- For sidechains it is critical to define governance upfront (who signs
  checkpoints and how, how committee rotation proceeds, how disputes/slashing
  are handled).

---

## 9. Brief summary

GeoCoin already covers not just a "coin" but an entire platform:

- a resilient L1 with a managed economy;
- legally meaningful cadastral and NFT scenarios;
- sidechains with collateral and controlled risks;
- an architecture fit for real deployment in state and corporate
  environments.
