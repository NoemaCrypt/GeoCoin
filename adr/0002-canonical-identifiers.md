# ADR 0002: Canonical metadata hash and scope id

## Status

Accepted.

## Context

`card_metadata_hash` and `card_scope_id` are opaque 32-byte commitments on L1.
Without a domain separator, two tenants or two networks can publish the same
canonical JSON and collide in shared off-chain stores. Stuffing tenant into
`schema_id` is not enough: a later product will omit it.

## Decision

Strings are UTF-8 with CompactSize length. Empty strings are invalid.

### metadata_hash

Canonical payload is either RFC 8785 JCS or deterministic CBOR. One `schema_id`
picks one encoding and never mixes them.

```
metadata_hash = SHA-256(
    "geocoin:b2b:metadata:v1" ||
    network_id || tenant_id || object_type ||
    schema_id || schema_version || canonical_payload
)
```

`network_id`, `tenant_id`, `object_type` and `schema_id` are length-prefixed.
`schema_version` is uint32 LE. `canonical_payload` is length-prefixed bytes.

- `network_id` — chain/network name (`degree`, `testnet`, genesis-id string).
- `tenant_id` — stable org id. Not a name, email, PAN or other personal id.
- `object_type` — object class (`card.program`, `card.instance`, `card.decision`).
- `schema_id` — reverse-DNS schema (`com.geocoin.card.metadata`).
- Changing field meaning requires a new `schema_version`. Published versions are
  immutable.

`schema_id` must not be used as a substitute for `network_id` or `tenant_id`.
All four identity fields are hashed independently so identical payloads under
different tenants cannot match.

Personal data, photos and documents stay off-chain. The canonical payload may
hold their content hash and a key/reference, not the raw bytes.

L1 stores only the 32-byte hash. It does not verify schema or payload.

### scope_id

```
scope_id = SHA-256(
    "geocoin:b2b:scope:v1" ||
    network_id || tenant_id || product_id || scope_name
)
```

`scope_id` may equal a sidechain id only when a product schema says so
explicitly. Default is isolation: card program scope and sidechain id are
different objects.

## Consequences

- Helpers live in `geocoin::b2b`. Changing the formula is a consensus-adjacent
  product break even though L1 only sees the hash.
- Off-chain indexes must key by `network_id` + `tenant_id`, not by hash alone
  if they also store plaintext.
