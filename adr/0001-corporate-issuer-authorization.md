# ADR 0001: Corporate issuer authorization on L1

## Status

Accepted.

## Context

Card programs on L1 are a registry of profile, issuance and status. They do not
hold spendable balances. A single `card_issuer` key is not a corporate control
plane: compromise or absence of that key freezes the program with no recovery
path that consensus can check.

Off-chain HSM policy, by itself, is not consensus. If mint/freeze/operations
exist only as a key-management document, a node still accepts any signature from
the issuer address.

## Decision

1. **Cards have no L1 balance.** `CARD_*` never credits, debits or custodians
   value. Recovery of card *value* is a sidechain or wallet problem. Recovery on
   L1 means restoring *issuer authorization* over the program and card status.

2. **On-chain policy is the source of truth.** `CardCorporatePolicy` version 1
   stores four independent M-of-N GEO-address sets: `mint`, `freeze`,
   `operations`, `recovery`. L1 rejects `CARD_*` unless enough *signed UTXO
   inputs* belong to the required role:

   | Transaction | Required role |
   | --- | --- |
   | `CARD_PROGRAM_DEFINE` | recovery of the *new* policy |
   | `CARD_ISSUE` | mint |
   | `CARD_UPDATE_STATUS` → suspend/revoke | freeze |
   | `CARD_UPDATE_STATUS` → reactivate | operations |
   | `CARD_UPDATE_POLICY` | recovery of the *current* policy |

3. **`CARD_UPDATE_POLICY` is emergency key rotation.** It may replace every
   role and `compliance_policy_hash`. It must not change `issuer` or
   `scope_id`. Existing card UIDs and status records stay attached to the
   program. This is not a sidechain deposit recovery and does not move funds.

4. **Version 0 is a single-key fallback.** Empty authorities plus an input from
   `card_issuer` remain valid for programs created before corporate policy.
   Version 1 cannot be mixed with empty authorities.

5. **Authorization blob is not an L1 object.**
   `geocoin:b2b:authorization:v1 || policy_id || role || operation || object_id || nonce || expires_at`
   is for product backends and HSMs. Chainstate does **not** parse this payload,
   does **not** store `nonce`, and does **not** enforce `expires_at`. Replay
   protection of off-chain approvals is the backend's job. L1 replay protection
   is the usual UTXO spend.

## Consequences

- A compromised mint set cannot rotate policy; only recovery can.
- Losing recovery keys loses the ability to rotate policy. That is an
  operational failure, not an L1 bug.
- Shipping a B2B card product that talks about "L1 card balances" contradicts
  this ADR.
