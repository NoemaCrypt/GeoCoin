# ADR 0006: Transaction hardening soft fork

Status: implemented; activation height pending deployment confirmation.

## Context

A review of transaction validation found encodings that consensus accepted
although no honest wallet, RPC builder, GUI flow or C SDK produces them:

- Falcon-512 verification also accepts a compressed signature zero-padded to its
  fixed 666-byte size. The txid commits to signature bytes (`signature_hash` for
  witnesses, the bytes themselves for in-body signatures), so any relayer or
  miner could re-encode a transaction it did not sign and change its txid.
  Unconfirmed children of the original txid then become invalid.
- Sidechain committee and foundation signature lists skipped entries that failed
  to verify, repeated a signer or came from a non-member. A third party could
  append such entries, again changing the txid.
- `REVOKE_DELEGATION` change had no dust or script check, unlike
  `DELEGATION_PROOF` change.
- State application counted the spending block as a confirmation, so a coinbase
  became spendable after 99 blocks, while the wallet and mempool require 100.
- `SLASH_DEPOSIT` resolved fraud evidence from node-local storage, which retains
  checkpoints from disconnected blocks. Two conflicting checkpoints for one
  height cannot both be on the active chain, so valid evidence needs an orphaned
  checkpoint that only some nodes have seen. Such a block splits the network.

## Decision

From `ChainParams::transaction_hardening_activation_height()` blocks must satisfy:

| Rule | Reject reason / effect |
| --- | --- |
| Every Falcon signature uses the minimal encoding (witnesses, `ANGLE_COMMITMENT` inputs, NFT, sidechain owner/committee/batch and foundation fields). Coinbase witness bytes are exempt. | `bad-txns-noncanonical-signature` |
| `REVOKE_DELEGATION` change is at least `DUST_LIMIT` and has no script. | `bad-revoke-delegation` |
| Committee and foundation lists contain only distinct, verifying member signatures (more than the quorum is still allowed). | state validation failure |
| A coinbase output is spendable once the spending height is at least `coinbase_maturity()` blocks above it. | state validation failure |
| `SLASH_DEPOSIT` evidence must be active-chain transactions. | state validation failure |

Each rule only rejects what was previously accepted, so this is a soft fork.
Blocks below the activation height keep the original rules and the existing chain
stays valid on every node; no stored data or serialization changes.

Relay enforces all rules immediately, independent of height. Nothing admitted to
the mempool before activation can therefore invalidate a block template after it.

Activation heights: mainnet and testnet use a fixed future height set in
`chainparams.cpp`; the isolated load network enforces the rules from height 1.

## Consequences

- All nodes and miners must upgrade before the activation height. A miner on old
  software could otherwise produce a block that upgraded nodes reject.
- Slashing is effectively unavailable until a later protocol change carries both
  conflicting checkpoint items and their committee signatures inline in
  `SLASH_DEPOSIT`. That change alters the transaction format and is out of scope.
- Not part of this fork, because they would restrict legitimate existing use or
  change economics: `CARD_PROGRAM_DEFINE` issuer authentication (see ADR 0001),
  recomputing sidechain issuance limits after fee debits, network-bound signed
  authorizations, and interest distribution with more than 1000 recipients.
