#pragma once

#include "geocoin/core/transaction.hpp"
#include "geocoin/state/state.hpp"

#include <span>
#include <unordered_set>

namespace geocoin::wallet {

struct AddressFunds {
    Amount confirmed = 0;
    Amount available = 0;
    Amount unconfirmed = 0;
    Amount immature = 0;
    bool valid = true;
};

// Chain outputs a transaction may spend: positive value, already in the
// active chain, and a coinbase that has reached maturity.
bool utxo_is_spendable(const state::UTXO& utxo, Height chain_height);

// Outpoints consumed by transactions that are not yet in the chain.
class PendingSpendIndex {
public:
    PendingSpendIndex() = default;
    explicit PendingSpendIndex(std::span<const core::Transaction> pending);

    bool contains(const Hash256& tx_hash, uint32_t vout) const;

private:
    struct Key {
        Hash256 tx{};
        uint32_t vout = 0;
        bool operator==(const Key& other) const {
            return vout == other.vout && tx == other.tx;
        }
    };
    struct Hash {
        size_t operator()(const Key& key) const noexcept;
    };
    std::unordered_set<Key, Hash> spent_;
};

// One classification for the wallet, the GUI, and coin selection.
// `confirmed` is every spendable chain output. `available` is that set minus
// outputs already consumed by `pending`. Incoming pending outputs, including
// change, stay in `unconfirmed` until they are in the chain.
AddressFunds classify_address_funds(
    const state::ChainState& chain,
    const Address& address,
    std::span<const core::Transaction> pending = {});

} // namespace geocoin::wallet
