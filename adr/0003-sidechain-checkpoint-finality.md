# ADR 0003: Sidechain checkpoint proof, conflicts and finality

## Status

Accepted.

## Context

L1 does not execute sidechain runtime. A checkpoint is a committee-attested
commitment plus a structured proof. If the product claims "L1 proves the
sidechain is correct", that claim is false and must not ship.

## Decision

### What the proof proves

Financial sidechains (`policy_id=2`, `version=2`):

- Proof magic `SCP2`.
- `state_root` is a Merkle root. The leaf at `leaf_index` is
  `SHA-256d("geocoin:sidechain:v2:supply_leaf" || sidechain_id || height || total_supply)`.
- `checkpoint` must equal
  `SHA-256d("geocoin:sidechain:v2:checkpoint_commitment" || sidechain_id || state_root || height || total_supply)`.
- `total_supply` must equal L1 `total_issued` and must not exceed `issuance_limit`.

This proves: the committee published a state root that *includes* the
L1-known supply leaf. It does **not** prove other leaves, transaction
ordering, or that the sidechain VM executed correctly.

Accounting sidechains (`policy_id=3`, `version=1`):

- Proof magic `SCA1`.
- `checkpoint` commits `state_root`, height and registered `schema_hash`.
- No supply leaf. L1 proves structure and schema binding, not business data.

Changing proof semantics is a consensus upgrade, not a product toggle.

### Who attests execution

The registered committee (M-of-N over the checkpoint payload). L1 checks
signature count and membership. L1 does not re-run the sidechain, does not
interpret `state_root` beyond the rules above, and does not decide disputes
about off-chain history.

Independent operators must monitor supply, divergence and missing signers.

### Conflicts

L1 rejects:

- a checkpoint hash equal to the last stored hash;
- `checkpoint_height` ≤ last sidechain checkpoint height (must be strictly
  monotonic);
- L1 height closer than `MIN_SIDECHAIN_CHECKPOINT_INTERVAL` (5 blocks) to the
  previous checkpoint *on L1*.

Two different checkpoint hashes at the **same** sidechain height, already
included in L1, are a fraud signal. `SLASH_DEPOSIT` may spend the deposit when
a fraud proof names those two L1 checkpoint transactions. Slash does not
reconstruct the "true" sidechain state; it punishes equivocation.

Fork choice and reorg of the sidechain itself are product rules. After L1
accepts a higher height, L1 will not roll the checkpoint back except by L1
reorg of the checkpoint transaction.

### Finality

| Layer | Meaning |
| --- | --- |
| L1 checkpoint tx | The commitment is in GeoCoin history under GeoCoin finality. |
| Sidechain execution | Not implied. Product defines confirmations, challenge window and RPO/RTO. |

Decommission keeps the deposit slashable for
`SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS` (144). That window is for L1 fraud
proofs, not a generic sidechain finality gadget.

### Economics

Fee is paid from the sidechain deposit, not as a normal tx output.

- Weight fee: `ceil(weight * FEE_RATE_PER_KB / 1000)`, at least `MIN_TX_FEE`.
- Frequency: below `SIDECHAIN_CHECKPOINT_SOFT_INTERVAL` (30 L1 blocks) the
  minimum fee rises as `MIN_TX_FEE * ceil(soft / delta)`, capped at
  `MAX_SIDECHAIN_CHECKPOINT_FEE_MULTIPLIER` (20).

Products choose how often to checkpoint inside that envelope. L1 will not
accept a free/zero-fee checkpoint.

## Consequences

- B2B copy must say "committee-attested state root", never "L1-verified
  sidechain execution".
- New proof types or extra verified leaves need a new `policy_id`/`version`.
