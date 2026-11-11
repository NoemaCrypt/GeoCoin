#include "geocoin/wallet/address_funds.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/utils/u64_math.hpp"

#include <limits>

namespace geocoin::wallet {

namespace {

bool add_amount(Amount& total, Amount value) {
    Amount next = 0;
    if (value < 0 || !checked_add_amount(total, value, next)) {
        return false;
    }
    total = next;
    return true;
}

AddressFunds invalid_funds() {
    AddressFunds funds;
    funds.valid = false;
    return funds;
}

} // namespace

bool utxo_is_spendable(const state::UTXO& utxo, Height chain_height) {
    if (utxo.value <= 0 || utxo.height > chain_height) {
        return false;
    }
    if (!utxo.is_coinbase) {
        return true;
    }
    const Height maturity = consensus::params::COINBASE_MATURITY;
    if (maturity == 0) {
        return true;
    }
    const Height delta = chain_height - utxo.height;
    if (delta == (std::numeric_limits<Height>::max)()) {
        return false;
    }
    return delta + 1 >= maturity;
}

size_t PendingSpendIndex::Hash::operator()(const Key& key) const noexcept {
    size_t value = 1469598103934665603ull;
    for (byte b : key.tx) {
        value ^= static_cast<size_t>(b);
        value *= 1099511628211ull;
    }
    value ^= static_cast<size_t>(key.vout);
    value *= 1099511628211ull;
    return value;
}

PendingSpendIndex::PendingSpendIndex(std::span<const core::Transaction> pending) {
    for (const auto& tx : pending) {
        if (tx.is_coinbase()) {
            continue;
        }
        for (const auto& input : tx.inputs) {
            spent_.insert(Key{input.prev_tx_hash, input.vout});
        }
    }
}

bool PendingSpendIndex::contains(const Hash256& tx_hash, uint32_t vout) const {
    return spent_.contains(Key{tx_hash, vout});
}

AddressFunds classify_address_funds(
    const state::ChainState& chain,
    const Address& address,
    std::span<const core::Transaction> pending
) {
    AddressFunds funds;
    const Height height = chain.get_height();
    if (height == (std::numeric_limits<Height>::max)()) {
        return invalid_funds();
    }

    const PendingSpendIndex reserved(pending);
    try {
        for (const auto& utxo : chain.get_utxos_for_address(address)) {
            if (utxo_is_spendable(utxo, height)) {
                if (!add_amount(funds.confirmed, utxo.value)) {
                    return invalid_funds();
                }
                if (!reserved.contains(utxo.tx_hash, utxo.vout) &&
                    !add_amount(funds.available, utxo.value)) {
                    return invalid_funds();
                }
                continue;
            }
            if (!utxo.is_coinbase || utxo.value <= 0 || utxo.height > height) {
                continue;
            }
            if (!add_amount(funds.immature, utxo.value)) {
                return invalid_funds();
            }
        }

        for (const auto& tx : pending) {
            if (tx.is_coinbase()) {
                continue;
            }
            for (const auto& output : tx.outputs) {
                if (output.address != address || output.value <= 0) {
                    continue;
                }
                if (!add_amount(funds.unconfirmed, output.value)) {
                    return invalid_funds();
                }
            }
        }
    } catch (const std::exception&) {
        return invalid_funds();
    } catch (...) {
        return invalid_funds();
    }
    return funds;
}

} // namespace geocoin::wallet
