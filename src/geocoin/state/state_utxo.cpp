#include "geocoin/state/state.hpp"
#include "geocoin/state/state_internal.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace geocoin::state {

namespace {

bool parse_utxo_storage_key(const std::string& key, UtxoKey& out) {
    constexpr size_t kPrefix = 5;
    constexpr size_t kHex = 64;
    if (key.size() < kPrefix + kHex + 2 || key.compare(0, kPrefix, "utxo:") != 0 ||
        key[kPrefix + kHex] != ':') {
        return false;
    }
    if (!utils::hex_to_hash256_checked(key.substr(kPrefix, kHex), out.tx_hash)) {
        return false;
    }
    uint32_t value = 0;
    bool saw_digit = false;
    for (size_t i = kPrefix + kHex + 1; i < key.size(); ++i) {
        const unsigned char digit = static_cast<unsigned char>(key[i]);
        if (digit < '0' || digit > '9') {
            return false;
        }
        saw_digit = true;
        const uint32_t next = value * 10u + static_cast<uint32_t>(digit - '0');
        if (next < value) {
            return false;
        }
        value = next;
    }
    if (!saw_digit) {
        return false;
    }
    out.vout = value;
    return true;
}

} // namespace

void ChainState::track_utxo_inserted_(const UTXO& utxo) {
    if (!utxo_balance_index_valid_ || utxo.value <= 0 || utxo.address.str().empty()) {
        return;
    }
    auto& bucket = utxo_balance_by_height_[utxo.height];
    Amount next_address = 0;
    Amount next_total = 0;
    Amount& current = bucket.by_address[utxo.address.str()];
    if (!checked_add_amount(current, utxo.value, next_address) ||
        !checked_add_amount(bucket.total, utxo.value, next_total)) {
        utxo_balance_index_valid_ = false;
        utxo_balance_by_height_.clear();
        return;
    }
    current = next_address;
    bucket.total = next_total;
}

void ChainState::track_utxo_erased_(const UTXO& utxo) {
    if (!utxo_balance_index_valid_ || utxo.value <= 0 || utxo.address.str().empty()) {
        return;
    }
    const auto height_it = utxo_balance_by_height_.find(utxo.height);
    if (height_it == utxo_balance_by_height_.end()) {
        utxo_balance_index_valid_ = false;
        utxo_balance_by_height_.clear();
        return;
    }
    auto& bucket = height_it->second;
    const auto address = utxo.address.str();
    const auto address_it = bucket.by_address.find(address);
    if (address_it == bucket.by_address.end() || address_it->second < utxo.value ||
        bucket.total < utxo.value) {
        utxo_balance_index_valid_ = false;
        utxo_balance_by_height_.clear();
        return;
    }
    address_it->second -= utxo.value;
    bucket.total -= utxo.value;
    if (address_it->second == 0) {
        bucket.by_address.erase(address_it);
    }
    if (bucket.by_address.empty()) {
        utxo_balance_by_height_.erase(height_it);
    }
}

void ChainState::adopt_utxo_memory_(const UTXO& utxo) {
    const UtxoKey key = utxo.memory_key();
    if (const auto existing = utxo_set_.find(key); existing != utxo_set_.end()) {
        track_utxo_erased_(existing->second);
    }
    utxo_set_.insert_or_assign(key, utxo);
    track_utxo_inserted_(utxo);
}

bool ChainState::process_standard_utxo_effects_(
    const core::Transaction& tx,
    Height height,
    bool mutate,
    bool require_witness
) {
    const Hash256 tx_hash = tx.get_hash();
    Amount total_input = 0;
    std::set<std::pair<Hash256, uint32_t>> seen_inputs;
    std::vector<UTXO> inputs_to_spend;
    inputs_to_spend.reserve(tx.inputs.size());

    for (size_t i = 0; i < tx.inputs.size(); ++i) {
        const auto& input = tx.inputs[i];
        if (!seen_inputs.emplace(input.prev_tx_hash, input.vout).second) {
            return false;
        }

        const UTXO* utxo = find_utxo_unlocked_(input.prev_tx_hash, input.vout);
        if (!utxo || utxo->value < 0 || height < utxo->height) {
            return false;
        }

        const Height confirmations = height - utxo->height + 1;
        if (utxo->is_coinbase &&
            confirmations < consensus::Params().coinbase_maturity()) {
            return false;
        }

        if (require_witness) {
            if (i >= tx.witnesses.size()) {
                return false;
            }
            auto witness = tx.witnesses[i];
            if (witness.signature.empty() ||
                utils::is_null_hash256(witness.public_key_hash)) {
                return false;
            }
            if (!witness.public_key.empty()) {
                if (crypto::sha256(witness.public_key) != witness.public_key_hash) {
                    return false;
                }
            } else if (!get_witness_pubkey(witness.public_key_hash, witness.public_key) ||
                       witness.public_key.empty()) {
                return false;
            }

            Address signer_address;
            if (!crypto::address_from_public_key(witness.public_key, signer_address) ||
                signer_address != utxo->address) {
                return false;
            }
        }

        if (total_input > (std::numeric_limits<Amount>::max)() - utxo->value) {
            return false;
        }
        total_input += utxo->value;
        inputs_to_spend.push_back(*utxo);
    }

    Amount total_output = 0;
    std::vector<UTXO> outputs_to_add;
    outputs_to_add.reserve(tx.outputs.size());
    for (uint32_t i = 0; i < tx.outputs.size(); ++i) {
        const auto& output = tx.outputs[i];
        if (output.value < 0 ||
            total_output > (std::numeric_limits<Amount>::max)() - output.value) {
            return false;
        }
        total_output += output.value;

        UTXO utxo;
        utxo.tx_hash = tx_hash;
        utxo.vout = i;
        utxo.value = output.value;
        utxo.address = output.address;
        utxo.height = height;
        utxo.is_coinbase = false;
        if (utxo_set_.find(utxo.memory_key()) != utxo_set_.end()) {
            return false;
        }
        outputs_to_add.push_back(std::move(utxo));
    }
    if (total_input < total_output) {
        return false;
    }

    if (!mutate) {
        return true;
    }

    if (storage_) {
        bool storage_ok = true;
        try {
            for (const auto& utxo : inputs_to_spend) {
                if (!persist_utxo_record_(utxo, true)) {
                    storage_ok = false;
                    break;
                }
            }
            if (storage_ok) {
                for (const auto& utxo : outputs_to_add) {
                    if (!persist_utxo_record_(utxo, false)) {
                        storage_ok = false;
                        break;
                    }
                }
            }
        } catch (const std::exception& error) {
            GEO_LOG_ERROR(
                "chainstate",
                "UTXO persistence preparation failed: " << error.what()
            );
            storage_ok = false;
        } catch (...) {
            GEO_LOG_ERROR("chainstate", "UTXO persistence preparation failed");
            storage_ok = false;
        }

        if (!storage_ok) {
            bool restored = true;
            for (const auto& utxo : outputs_to_add) {
                try {
                    if (!persist_utxo_record_(utxo, true)) {
                        restored = false;
                    }
                } catch (...) {
                    restored = false;
                }
            }
            for (const auto& utxo : inputs_to_spend) {
                try {
                    if (!persist_utxo_record_(utxo, false)) {
                        restored = false;
                    }
                } catch (...) {
                    restored = false;
                }
            }
            if (!restored) {
                derived_state_storage_valid_ = false;
                state_transition_poisoned_ = true;
                GEO_LOG_ERROR(
                    "chainstate",
                    "Failed to restore UTXO persistence after a rejected transition"
                );
            }
            return false;
        }
    }

    for (const auto& utxo : inputs_to_spend) {
        track_utxo_erased_(utxo);
        utxo_set_.erase(utxo.memory_key());
    }
    for (const auto& utxo : outputs_to_add) {
        utxo_set_.emplace(utxo.memory_key(), utxo);
        track_utxo_inserted_(utxo);
    }
    return true;
}

bool ChainState::apply_standard_utxo_effects_(
    const core::Transaction& tx,
    Height height,
    bool is_coinbase,
    consensus::ValidationState* validation,
    Amount* spent_input_total
) {
    const auto invalid = [validation](const char* reason) {
        if (validation) validation->invalid(consensus::RejectCode::INVALID, reason);
        return false;
    };
    const Hash256 tx_hash = tx.get_hash();
    Amount total_input = 0;
    std::set<std::pair<Hash256, uint32_t>> seen_inputs;
    std::vector<UTXO> inputs_to_spend;
    inputs_to_spend.reserve(tx.inputs.size());

    if (!is_coinbase) {
        for (const auto& input : tx.inputs) {
            if (!seen_inputs.emplace(input.prev_tx_hash, input.vout).second) {
                return invalid("bad-txns-inputs-duplicate");
            }
            const auto* utxo = find_utxo_unlocked_(input.prev_tx_hash, input.vout);
            if (!utxo) {
                return invalid("bad-txns-inputs-missing-or-spent");
            }
            if (height < utxo->height) {
                return invalid("bad-txns-inputs-from-future");
            }

            const Height COINBASE_MATURITY = consensus::Params().coinbase_maturity();
            Height confirmations = height - utxo->height + 1;
            if (utxo->is_coinbase && confirmations < COINBASE_MATURITY) {
                GEO_LOG_ERROR(
                    "chainstate",
                    "Coinbase not mature: height=" << height
                    << ", utxo_height=" << utxo->height
                    << ", confirmations=" << confirmations
                    << " (need " << COINBASE_MATURITY << ")"
                );
                return invalid("bad-txns-premature-spend-of-coinbase");
            }

            Amount next_input = 0;
            if (utxo->value < 0 || !checked_add_amount(total_input, utxo->value, next_input)) {
                return invalid("bad-txns-inputvalues-outofrange");
            }
            total_input = next_input;
            inputs_to_spend.push_back(*utxo);
        }
    }

    Amount total_output = 0;
    for (const auto& output : tx.outputs) {
        Amount next_output = 0;
        if (output.value < 0 || !checked_add_amount(total_output, output.value, next_output)) {
            return invalid("bad-txns-outputvalues-outofrange");
        }
        total_output = next_output;
    }

    if (!is_coinbase && total_input < total_output) {
        return invalid("bad-txns-in-belowout");
    }

    const auto restore_utxos = [&](size_t removed_inputs, size_t added_outputs) {
        bool restored = true;
        for (size_t i = added_outputs; i > 0; --i) {
            const uint32_t vout = static_cast<uint32_t>(i - 1);
            if (find_utxo_unlocked_(tx_hash, vout) && !remove_utxo(tx_hash, vout)) {
                restored = false;
            }
        }
        for (size_t i = 0; i < removed_inputs; ++i) {
            const auto& input = inputs_to_spend[i];
            if (find_utxo_unlocked_(input.tx_hash, input.vout)) {
                continue;
            }
            try {
                add_utxo(input);
            } catch (...) {
                restored = false;
            }
        }
        if (!restored) {
            derived_state_storage_valid_ = false;
            state_transition_poisoned_ = true;
            GEO_LOG_ERROR("chainstate", "Failed to restore UTXOs after transaction persistence failure");
        }
        return restored;
    };

    size_t removed_inputs = 0;
    for (size_t i = 0; i < inputs_to_spend.size(); ++i) {
        const auto& utxo = inputs_to_spend[i];
        if (!remove_utxo(utxo.tx_hash, utxo.vout)) {
            restore_utxos(removed_inputs, 0);
            return false;
        }
        ++removed_inputs;
    }

    size_t added_outputs = 0;
    for (uint32_t i = 0; i < tx.outputs.size(); i++) {
        const auto& output = tx.outputs[i];

        UTXO utxo;
        utxo.tx_hash = tx_hash;
        utxo.vout = i;
        utxo.value = output.value;
        utxo.address = output.address;
        utxo.height = height;
        utxo.is_coinbase = is_coinbase;

        try {
            add_utxo(utxo);
            ++added_outputs;
        } catch (const std::exception& error) {
            GEO_LOG_ERROR("chainstate", "Failed to add transaction output: " << error.what());
            restore_utxos(removed_inputs, added_outputs);
            return false;
        } catch (...) {
            GEO_LOG_ERROR("chainstate", "Failed to add transaction output");
            restore_utxos(removed_inputs, added_outputs);
            return false;
        }
    }

    if (!store_applied_transaction_(tx_hash, tx)) {
        restore_utxos(removed_inputs, added_outputs);
        return false;
    }
    if (spent_input_total) *spent_input_total = total_input;
    return true;
}

bool ChainState::verify_owned_utxo_inputs_(
    const core::Transaction& tx,
    Amount* actual_fee
) const {
    if (tx.inputs.empty() || tx.witnesses.size() != tx.inputs.size()) {
        return false;
    }

    core::Transaction tx_for_verify = tx;
    Amount total_input = 0;
    std::set<std::pair<Hash256, uint32_t>> seen_inputs;
    for (size_t i = 0; i < tx.inputs.size(); ++i) {
        const auto& input = tx.inputs[i];
        if (!seen_inputs.emplace(input.prev_tx_hash, input.vout).second) {
            return false;
        }
        const auto* utxo = find_utxo_unlocked_(input.prev_tx_hash, input.vout);
        if (!utxo || !checked_accumulate_nonnegative(utxo->value, total_input)) {
            return false;
        }

        auto& witness = tx_for_verify.witnesses[i];
        if (witness.signature.empty() || utils::is_null_hash256(witness.public_key_hash)) {
            return false;
        }
        if (!witness.public_key.empty()) {
            if (crypto::sha256(witness.public_key) != witness.public_key_hash) {
                return false;
            }
        } else if (!get_witness_pubkey(witness.public_key_hash, witness.public_key) ||
                   witness.public_key.empty()) {
            return false;
        }

        Address signer_address;
        if (!crypto::address_from_public_key(witness.public_key, signer_address) ||
            signer_address.str() != utxo->address.str()) {
            return false;
        }
    }

    if (!tx_for_verify.verify()) {
        return false;
    }

    if (actual_fee != nullptr) {
        Amount total_output = 0;
        if (!checked_sum_outputs(tx.outputs, total_output) ||
            total_input < total_output ||
            !checked_sub_amount(total_input, total_output, *actual_fee)) {
            return false;
        }
    }

    return true;
}

bool ChainState::store_applied_transaction_(const Hash256& tx_hash, const core::Transaction& tx) {
    if (!storage_) {
        return true;
    }
    try {
        return storage_->put_transaction(tx_hash, tx);
    } catch (const std::exception& error) {
        GEO_LOG_ERROR("chainstate", "Failed to persist applied transaction: " << error.what());
    } catch (...) {
        GEO_LOG_ERROR("chainstate", "Failed to persist applied transaction");
    }
    return false;
}

bool ChainState::record_persistence_result_(bool success, const char* operation) {
    if (success) {
        return true;
    }
    transition_persistence_ok_ = false;
    GEO_LOG_ERROR("chainstate", "State persistence failed: " << operation);
    return false;
}

bool ChainState::has_derived_state_snapshot() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!derived_state_storage_valid_) {
        return false;
    }
    if (current_height_ == 0) {
        return true;
    }
    // Минимальный эвристический признак: если UTXO-представление пустое,
    // скорее всего derived state отсутствует и нужен rebuild.
    return !utxo_set_.empty();
}

bool ChainState::has_utxo_count_snapshot() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!storage_) {
        return false;
    }
    bytes count_data;
    return storage_->get_metadata("chainstate_utxo_count", count_data) && count_data.size() == 8;
}

bool ChainState::persist_utxo_count_to_storage() {
    if (!storage_) {
        return true;
    }
    const uint64_t count = static_cast<uint64_t>(utxo_set_.size());
    bytes data(8);
    for (int i = 0; i < 8; ++i) {
        data[i] = static_cast<byte>((count >> (i * 8)) & 0xFF);
    }
    return storage_->put_metadata("chainstate_utxo_count", data);
}


Amount ChainState::get_balance(const Address& address) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Amount balance = 0;
    size_t offset = 0;
    constexpr size_t kBalanceScanChunk = 4096;
    for (;;) {
        const auto utxos = get_utxos_for_address_page(address, offset, kBalanceScanChunk);
        if (utxos.empty()) {
            break;
        }
        for (const auto& utxo : utxos) {
            Amount next = 0;
            if (utxo.value < 0 || !checked_add_amount(balance, utxo.value, next)) return 0;
            balance = next;
        }
        offset += utxos.size();
        if (utxos.size() < kBalanceScanChunk) {
            break;
        }
    }
    return balance;
}

std::vector<UTXO> ChainState::get_utxos_for_address(const Address& address) const {
    return get_utxos_for_address_page(
        address,
        0,
        (std::numeric_limits<size_t>::max)()
    );
}

std::vector<UTXO> ChainState::get_utxos_for_address_page(
    const Address& address,
    size_t offset,
    size_t limit,
    size_t* total
) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<UTXO> result;
    if (limit == 0) {
        if (total) {
            *total = 0;
        }
        return result;
    }

    if (storage_) {
        const auto prefix = UTXO::address_index_prefix(address);
        const auto keys = storage_->get_keys_page(prefix, offset, limit, total);
        result.reserve(keys.size());
        for (const auto& key : keys) {
            if (key.size() < prefix.size()) {
                throw storage::ReadError(storage::ReadStatus::CORRUPT, "Address index disagrees with UTXO state");
            }
            UtxoKey parsed;
            if (!parse_utxo_storage_key(key.substr(prefix.size()), parsed)) {
                throw storage::ReadError(storage::ReadStatus::CORRUPT, "Address index disagrees with UTXO state");
            }
            const auto it = utxo_set_.find(parsed);
            if (it == utxo_set_.end() || it->second.address != address) {
                throw storage::ReadError(storage::ReadStatus::CORRUPT, "Address index disagrees with UTXO state");
            }
            result.push_back(it->second);
        }
        return result;
    }

    std::vector<UTXO> matching;
    for (const auto& [key, utxo] : utxo_set_) {
        if (utxo.address == address) {
            matching.push_back(utxo);
        }
    }
    if (total) {
        *total = matching.size();
    }
    if (offset >= matching.size()) {
        return result;
    }
    const size_t count = std::min(limit, matching.size() - offset);
    result.assign(
        matching.begin() + static_cast<std::ptrdiff_t>(offset),
        matching.begin() + static_cast<std::ptrdiff_t>(offset + count)
    );
    return result;
}

bool ChainState::persist_utxo_record_(const UTXO& utxo, bool erase) {
    if (!storage_) return true;
    const bool own_batch = !storage_->batch_active();
    if (own_batch) storage_->begin_batch();
    try {
        bool ok = true;
        const auto old = utxo_set_.find(utxo.memory_key());
        if (!erase && old != utxo_set_.end() && old->second.address != utxo.address) {
            ok = storage_->delete_metadata(old->second.get_address_index_key());
        }
        ok = ok && (erase
            ? storage_->delete_metadata(utxo.get_key()) && storage_->delete_metadata(utxo.get_address_index_key())
            : storage_->put_metadata(utxo.get_key(), utxo.serialize()) && storage_->put_metadata(utxo.get_address_index_key(), {}));
        if (own_batch) {
            if (ok) ok = storage_->commit_batch();
            if (!ok) storage_->abort_batch();
        }
        return ok;
    } catch (...) {
        if (own_batch) storage_->abort_batch();
        throw;
    }
}

Amount ChainState::get_total_supply() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Amount total = 0;
    for (const auto& [key, utxo] : utxo_set_) {
        Amount next = 0;
        if (utxo.value < 0 || !checked_add_amount(total, utxo.value, next)) return 0;
        total = next;
    }
    return total;
}

void ChainState::add_utxo(const UTXO& utxo) {
    // Validate and serialize before changing either representation.  The old
    // order inserted into memory first, so a serialization error left a
    // malformed UTXO live only in RAM and split the in-memory/disk state.
    (void)utxo.serialize();

    if (!persist_utxo_record_(utxo, false)) {
        throw std::runtime_error("failed to persist UTXO");
    }

    adopt_utxo_memory_(utxo);
}

bool ChainState::remove_utxo(const Hash256& tx_hash, uint32_t vout) {
    UTXO temp;
    temp.tx_hash = tx_hash;
    temp.vout = vout;

    const UtxoKey key = utxo_memory_key(tx_hash, vout);
    auto it = utxo_set_.find(key);

    if (it == utxo_set_.end()) {
        return false;
    }

    // Persist the deletion before changing the in-memory view.  Returning
    // success after a storage failure would make the UTXO spendable again
    // after restart even though it had already disappeared from RAM.
    if (!persist_utxo_record_(it->second, true)) {
        return false;
    }

    track_utxo_erased_(it->second);
    utxo_set_.erase(it);
    return true;
}

bool ChainState::get_utxo(const Hash256& tx_hash, uint32_t vout, UTXO& utxo) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    UTXO temp;
    temp.tx_hash = tx_hash;
    temp.vout = vout;

    const auto it = utxo_set_.find(utxo_memory_key(tx_hash, vout));

    if (it == utxo_set_.end()) {
        return false;
    }

    utxo = it->second;
    return true;
}

bool ChainState::has_utxo(const Hash256& tx_hash, uint32_t vout) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return find_utxo_unlocked_(tx_hash, vout) != nullptr;
}

} // namespace geocoin::state
