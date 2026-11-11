#pragma once

#include "geocoin/wallet/wallet.hpp"
#include "geocoin/types.hpp"
#include "geocoin/crypto/geo_area.hpp"

#include <map>
#include <memory>
#include <string>

namespace geocoin::wallet {

inline bool checked_accumulate_nonnegative(Amount& total, Amount value) {
    Amount next_total = 0;
    if (value < 0 || !checked_add_amount(total, value, next_total)) {
        return false;
    }
    total = next_total;
    return true;
}

inline Address resolve_primary_address(
    const std::map<Address, KeyInfo>& keys,
    const Address& stored_primary
) {
    if (!stored_primary.empty() && keys.find(stored_primary) != keys.end()) {
        return stored_primary;
    }
    for (const auto& [address, info] : keys) {
        if (info.label == "default") {
            return address;
        }
    }
    if (keys.empty()) {
        return "";
    }
    return keys.begin()->first;
}

bool init_password_verifier_from_password(
    const std::string& password,
    bytes& salt_out,
    uint32_t& iterations_out,
    Hash256& verifier_out
);

inline void compact_standard_witnesses_with_cache(
    core::Transaction& tx,
    const std::shared_ptr<state::ChainState>& chain_state
) {
    if (!chain_state || tx.type != core::TransactionType::STANDARD) {
        return;
    }

    for (auto& w : tx.witnesses) {
        if (w.public_key.empty()) {
            continue;
        }
        const Hash256 h = crypto::sha256(w.public_key);
        w.public_key_hash = h;
        if (chain_state->has_witness_pubkey(h)) {
            w.public_key.clear();
        }
    }
}

bool verify_password_material(
    const std::string& password,
    bool has_password_hash,
    const Hash256& password_hash,
    const bytes& password_salt,
    uint32_t password_kdf_iterations
);

} // namespace geocoin::wallet
