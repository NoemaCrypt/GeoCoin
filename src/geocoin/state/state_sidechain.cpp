#include "geocoin/state/state.hpp"
#include "geocoin/state/state_internal.hpp"
#include "geocoin/state/state_sidechain_internal.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/sidechain_fee.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"

#include <set>
#include <utility>
#include <vector>

namespace geocoin::state {

bool ChainState::collect_spendable_inputs_(
    const core::Transaction& tx,
    Height height,
    Amount& total_input,
    std::vector<UTXO>& inputs_to_spend
) const {
    total_input = 0;
    std::set<std::pair<Hash256, uint32_t>> seen_inputs;
    inputs_to_spend.clear();
    inputs_to_spend.reserve(tx.inputs.size());
    for (const auto& input : tx.inputs) {
        const UTXO* utxo = find_utxo_unlocked_(input.prev_tx_hash, input.vout);
        if (!utxo || utxo->value < 0 || height < utxo->height ||
            !seen_inputs.emplace(input.prev_tx_hash, input.vout).second) {
            return false;
        }
        const Height confirmations = height - utxo->height + 1;
        if (utxo->is_coinbase && confirmations < consensus::Params().coinbase_maturity()) {
            return false;
        }
        Amount next_input = 0;
        if (!checked_add_amount(total_input, utxo->value, next_input)) {
            return false;
        }
        total_input = next_input;
        inputs_to_spend.push_back(*utxo);
    }
    return true;
}

bool ChainState::spend_collected_inputs_(const std::vector<UTXO>& inputs_to_spend) {
    for (size_t i = 0; i < inputs_to_spend.size(); ++i) {
        const auto& utxo = inputs_to_spend[i];
        if (!remove_utxo(utxo.tx_hash, utxo.vout)) {
            for (size_t restored = 0; restored < i; ++restored) {
                add_utxo(inputs_to_spend[restored]);
            }
            return false;
        }
    }
    return true;
}

bool ChainState::add_tx_outputs_as_utxos_(
    const core::Transaction& tx,
    const Hash256& tx_hash,
    Height height
) {
    size_t added = 0;
    for (uint32_t i = 0; i < tx.outputs.size(); ++i) {
        const auto& output = tx.outputs[i];
        UTXO utxo;
        utxo.tx_hash = tx_hash;
        utxo.vout = i;
        utxo.value = output.value;
        utxo.address = output.address;
        utxo.height = height;
        utxo.is_coinbase = false;
        try {
            add_utxo(utxo);
            ++added;
        } catch (...) {
            bool restored = true;
            for (size_t j = added; j > 0; --j) {
                if (!remove_utxo(tx_hash, static_cast<uint32_t>(j - 1))) {
                    restored = false;
                }
            }
            if (!restored) {
                derived_state_storage_valid_ = false;
                state_transition_poisoned_ = true;
            }
            return false;
        }
    }
    return true;
}

std::vector<Amount> ChainState::project_block_transaction_fees(
    const std::vector<core::Transaction>& transactions,
    size_t first_index,
    Height applying_height
) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<Amount> fees;
    if (first_index >= transactions.size()) return fees;
    fees.reserve(transactions.size() - first_index);

    std::map<Hash256, const core::Transaction*> earlier_transactions;
    SidechainOverlay overlay;
    for (size_t i = first_index; i < transactions.size(); ++i) {
        const auto& tx = transactions[i];
        if (tx.type == core::TransactionType::MINT_SIDECHAIN ||
            tx.type == core::TransactionType::REDEEM_SIDECHAIN) {
            SidechainRegistryEntry entry;
            Amount fee = 0;
            if (SidechainReadView(*this, &overlay).get_sidechain_registry(tx.sidechain_id, entry)) {
                fee = consensus::calc_sidechain_fee(tx.sidechain_amount, entry.fee_bps);
            }
            fees.push_back(fee >= 0 ? fee : 0);
        } else {
            fees.push_back(calculate_transaction_fee_(tx, &earlier_transactions));
        }

        if (core::is_sidechain_transition_type(tx.type)) {
            const SidechainReadView view(*this, &overlay);
            SidechainTransition transition;
            // An inapplicable transition leaves the overlay unchanged; the
            // block is rejected when that transaction is applied.
            if (prepare_sidechain_transition(view, tx, applying_height, transition)) {
                for (const auto& [id, entry] : transition.registries) {
                    SidechainRegistryEntry previous;
                    if (view.get_sidechain_registry(id, previous) &&
                        !previous.accounting_namespace.empty()) {
                        overlay.namespaces[previous.accounting_namespace] = false;
                    }
                    if (entry && !entry->accounting_namespace.empty()) {
                        overlay.namespaces[entry->accounting_namespace] = true;
                    }
                }
                for (auto& [id, deposit] : transition.deposits) overlay.deposits[id] = std::move(deposit);
                for (auto& [id, entry] : transition.registries) overlay.registries[id] = std::move(entry);
            }
        }
        earlier_transactions.emplace(tx.get_hash(), &tx);
    }
    return fees;
}

bool ChainState::apply_sidechain_transaction_(const core::Transaction& tx, Height height) {
    std::vector<UTXO> inputs_to_spend;
    std::optional<Amount> total_input;
    if (tx.type == core::TransactionType::LOCK_DEPOSIT ||
        tx.type == core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT) {
        total_input = 0;
        if (!collect_spendable_inputs_(tx, height, *total_input, inputs_to_spend)) return false;
    }
    SidechainTransition transition;
    if (!prepare_sidechain_transition(*this, tx, height, transition, total_input)) return false;
    if (transition.spends_inputs && !spend_collected_inputs_(inputs_to_spend)) return false;
    const Hash256 tx_hash = tx.get_hash();
    if (transition.creates_outputs && !add_tx_outputs_as_utxos_(tx, tx_hash, height)) {
        for (const auto& input : inputs_to_spend) add_utxo(input);
        return false;
    }
    for (auto& [id, deposit] : transition.deposits) {
        if (deposit) {
            sidechain_deposits_[hash256_to_hex(id)] = *deposit;
            persist_sidechain_deposit_to_storage(id, *deposit);
        } else {
            sidechain_deposits_.erase(hash256_to_hex(id));
            delete_sidechain_deposit_from_storage(id);
        }
    }
    for (auto& [id, entry] : transition.registries) {
        if (entry) {
            sidechain_registry_[hash256_to_hex(id)] = *entry;
            persist_sidechain_registry_to_storage(id, *entry);
        } else {
            sidechain_registry_.erase(hash256_to_hex(id));
            delete_sidechain_registry_from_storage(id);
        }
    }
    record_persistence_result_(store_applied_transaction_(tx_hash, tx), "persist sidechain transaction");
    return true;
}

} // namespace geocoin::state
