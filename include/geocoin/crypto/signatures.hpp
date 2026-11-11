#pragma once

#include "geocoin/types.hpp"
namespace geocoin::crypto {

Signature sign_transaction_input(
    const Hash256& tx_hash,
    size_t input_index,
    const bytes& private_key
);

bool sign_transaction_input_detached(
    const Hash256& tx_hash,
    size_t input_index,
    const bytes& private_key,
    bytes& out_public_key,
    Signature& out_signature
);

Signature sign_angle_commitment(
    const Hash256& spending_tx_hash,
    const bytes& private_key
);

Signature sign_recoverable_message(
    const Hash256& msg32,
    const bytes& private_key
);

bool verify_transaction_input(
    const Hash256& tx_hash,
    size_t input_index,
    const Signature& signature
);

bool verify_transaction_input_detached(
    const Hash256& tx_hash,
    size_t input_index,
    const bytes& public_key,
    const Signature& signature
);

bool verify_angle_commitment_signature(
    const Hash256& spending_tx_hash,
    const Signature& signature
);

bool verify_signed_message(
    const Hash256& msg32,
    const Signature& signature
);

// Relay policy: true when a raw or packed ("F512") Falcon signature uses the
// minimal encoding. An empty value carries no signature and is accepted.
bool has_canonical_signature_encoding(const Signature& signature);

bool recover_signer_address_from_signed_message(
    const Hash256& msg32,
    const Signature& signature,
    Address& out_address
);

bool recover_signer_address_from_transaction_input(
    const Hash256& tx_hash,
    size_t input_index,
    const Signature& signature,
    Address& out_address
);

bool address_from_public_key(
    const bytes& public_key,
    Address& out_address
);

bool derive_identity_address_from_private_key(
    const bytes& private_key,
    Address& out_address
);

bool recover_signer_address_from_angle_commitment(
    const Hash256& spending_tx_hash,
    const Signature& signature,
    Address& out_address
);

} // namespace geocoin::crypto
