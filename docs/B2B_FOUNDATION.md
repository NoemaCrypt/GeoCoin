# B2B Foundation v1

> Русская версия: [B2B_FOUNDATION_RU.md](B2B_FOUNDATION_RU.md)

This document fixes the contract between future GeoCoin B2B products and the
L1. The L1 stores identifiers, immutable hashes, statuses, corporate
quorum-policy, and settlement/checkpoint events. Personal data, terminal
logic, and HSM private keys stay in the product services.

Irreversible decisions: `adr/0001`–`adr/0004`. Cards on L1 carry no balances;
issuer recovery does not restore a sidechain deposit.

## 0. Post-quantum profile

GeoCoin critical signatures use Falcon-512. ECDSA, EdDSA and secp256k1 are not
supported signing schemes for the transaction/account layer.

P2P wire protocol v2 requires hybrid key establishment based on ML-KEM-768 and
X25519. X25519 is used only as part of the hybrid scheme together with
ML-KEM-768 and is never accepted as the only key-establishment mechanism.
Plaintext, classical-only and downgrade connection variants are rejected.

## 1. Corporate issuer policy

`geocoin::b2b::CorporateIssuerPolicy` defines four independent M-of-N roles:

- `mint` — issuing a card, token, or accounting unit;
- `freeze` — suspend/revoke and emergency blocking;
- `operations` — routine operational actions, including reactivation;
- `recovery` — policy replacement after key compromise or loss.

Each role's address list must be lexicographically sorted, contain no
duplicates, and consist of valid GEO addresses. The policy id is
`SHA-256("geocoin:b2b:issuer-policy:v1" || canonical_policy)`.

Authorization is signed over:

`"geocoin:b2b:authorization:v1" || policy_id || role || operation || object_id || nonce || expires_at`

The `nonce` is unique within the policy and is stored by the product backend.
`expires_at` is mandatory for network requests and limits authorization
reuse. Signatures are created by HSM keys or isolated signing services;
private keys are never handed to the web/mobile backend.

The L1 does not verify this payload and does not store nonces. Consensus
counts the quorum over the addresses of signed UTXO inputs belonging to the
required role (ADR 0001).

`CARD_PROGRAM_DEFINE` supports `corporate_policy.version=1` with four
independent M-of-N sets of GEO addresses. The L1 requires the recovery quorum
to create a program, `CARD_UPDATE_POLICY`; the mint quorum for `CARD_ISSUE`;
the freeze quorum for suspend/revoke; and the operations quorum for
reactivation. Each authorizing party signs one of the transaction inputs with
its HSM key.

`CARD_UPDATE_POLICY` keeps `issuer` and `scope_id` unchanged, while the
recovery quorum of the old policy can replace all roles and the compliance
commitment. This is an emergency replacement of issuer authorization: card
UIDs and statuses remain on the program. This is not a balance restoration
and not a sidechain-deposit recovery.

## 2. Limits and compliance

Universal consensus invariants on L1: a correct signature of the required
role, UID/public-key uniqueness, an allowed status transition, the collateral
limit, the sidechain issuance limit, and `max_active_cards` for a card
program. `compliance_policy_hash` anchors the version of the external policy.
Client policies are enforced by the B2B backend:

- daily/monthly limits and velocity;
- AML/KYC/risk score and sanctions lists;
- limits per terminal, employee, card, merchant, and region;
- case management and manual exception resolution.

Monetary/velocity limits cannot be an L1 invariant while L1 does not execute
card payments: they must be enforced by the sidechain or the authorization
backend. Every backend decision stores the policy id, policy version,
decision id, timestamp, authorizer set, and metadata hash. The L1 anchors the
decision hash, not personal data.

## 3. Canonical metadata hash

Metadata payloads are serialized before hashing using either the RFC 8785 JSON
Canonicalization Scheme or deterministic CBOR. The format is chosen in the
schema registry and is never mixed within one schema id.

`metadata_hash = SHA-256("geocoin:b2b:metadata:v1" || network_id || tenant_id || object_type || schema_id || schema_version || canonical_payload)`.

All strings are UTF-8 with a CompactSize length; `schema_version` is uint32 LE;
the payload is length-prefixed. `network_id` / `tenant_id` / `object_type` must
not be collapsed into a single `schema_id` (ADR 0002).

`schema_id` has a reverse-DNS form, for example `com.geocoin.card.metadata`.
Changing the meaning of a field requires a new schema version; previously
published versions are never modified. Binary documents, photos, and personal
data are stored off-chain, and the canonical payload includes their content
hash and encryption/key-reference.

## 4. Namespace for scope id

`scope_id = SHA-256("geocoin:b2b:scope:v1" || network_id || tenant_id || product_id || scope_name)`.

Each string is encoded as UTF-8 with a CompactSize length. `tenant_id` is a
stable, non-reusable organization identifier; do not use a person's name,
email, card number, or any other personal identifier in it. `scope_id` is
allowed in CARD programs and may coincide with a sidechain id only if the
product schema explicitly describes it.

## 5. Sidechain checkpoint v2

For financial L1: `policy_id=2`, `version=2`. For accounting: `policy_id=3`,
`version=1`. A batch item contains `sidechain_id`, `checkpoint`, sidechain
height, fee, committee id, M-of-N signatures, and the proof.

This is a federated model: L1 verifies the proof structure, the supply
constraint, checkpoint monotonicity, and the signatures of the registered
committee. Financial v2 proves that the supply leaf is included in the
claimed `state_root`; it does not prove the other leaves or VM correctness. A
conflict at the same sidechain height with two different checkpoint hashes is
grounds for `SLASH_DEPOSIT`, not for choosing the "true" state. The finality
of a checkpoint transaction on L1 is not the finality of sidechain execution
(ADR 0003).

L1 defines the checkpoint economics: at least once every 5 L1 blocks, a soft
30-block window with an increased min fee, paid from the deposit. Each
sidechain product separately defines:

- the finality algorithm and the minimum number of confirmations;
- the state schema and how the state root is obtained;
- the dispute, slash, and committee rotation procedure;
- the checkpoint period, retention, RPO/RTO, and fee budget;
- independent monitoring of supply, divergence, and signer availability.

Changing proof semantics or adding a new checkpoint policy is a separate
consensus upgrade. Ordinary B2B features on top of the current state root do
not require L1 changes.
