# ADR 0004: L1 invariants versus off-chain limits

## Status

Accepted.

## Context

Putting AML, velocity and per-client spend caps into L1 would freeze every
compliance change behind a consensus upgrade and still fail: L1 does not see
card payments, terminals or identities.

## Decision

L1 may enforce only **universal** invariants that every node can check from
chainstate:

- signature / role quorum (ADR 0001);
- unique `card_uid_hash` and `card_pubkey_hash`;
- legal status transitions (active ↔ suspended, either → revoked; revoked is
  terminal);
- `max_active_cards` on a program (`0` = unlimited);
- sidechain collateral, issuance cap and checkpoint supply leaf (ADR 0003);
- structural bounds (sizes, printable ASCII, non-null hashes).

L1 must **not** enforce:

- daily/monthly volume or velocity;
- AML/KYC, risk score, sanctions lists;
- per-terminal, per-employee, per-merchant or geo limits;
- case-management exceptions.

Those live in the product backend or sidechain. Each decision should record
policy id, policy version, decision id, timestamp, authorizer set and a
`metadata_hash` (ADR 0002). L1 may later commit that hash; it must not store
the personal data.

`compliance_policy_hash` on a card program is an opaque commitment to the
external policy pack. Changing it requires recovery quorum via
`CARD_UPDATE_POLICY`. L1 does not interpret the pack.

## Consequences

- "Hard L1 spending limits for corporate cards" is rejected until L1 actually
  settles card payments.
- Auditors get an on-chain policy/version/hash trail, not an on-chain AML
  engine.
