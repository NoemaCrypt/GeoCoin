#include "geocoin/state/state.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/sidechain_fee.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/storage/state_records.hpp"
#include "geocoin/utils/logger.hpp"

#include <chrono>
#include <charconv>
#include <limits>
#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace geocoin::state {

namespace {
constexpr const char* kActiveTxIndexPrefixLocal = "atx:";

bool starts_with_accessors(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool is_zero_hash_accessors(const Hash256& hash) {
    for (const auto byte : hash) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string query_prefix(const char* kind, const std::string& id) {
    return std::string(kind) + utils::bytes_to_hex(reinterpret_cast<const byte*>(id.data()), id.size()) + ":";
}

std::string query_ref_suffix(const IndexedTxRef& ref) {
    auto height = std::to_string(ref.height);
    height.insert(0, 20 - height.size(), '0');
    return height + ':' + utils::hash256_to_hex(ref.txid);
}

std::vector<IndexedTxRef> read_query_page(const Storage& storage, const std::string& prefix,
                                         size_t offset, size_t limit, size_t* total) {
    std::vector<IndexedTxRef> refs;
    for (const auto& key : storage.get_keys_page(prefix, offset, limit, total)) {
        const auto suffix = key.substr(prefix.size());
        IndexedTxRef ref;
        if (suffix.size() != 85 || suffix[20] != ':') {
            throw storage::ReadError(storage::ReadStatus::CORRUPT, "Invalid persisted query index");
        }
        const auto parsed = std::from_chars(suffix.data(), suffix.data() + 20, ref.height);
        if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + 20 ||
            !utils::hex_to_hash256_checked(suffix.substr(21), ref.txid)) {
            throw storage::ReadError(storage::ReadStatus::CORRUPT, "Invalid persisted query index");
        }
        refs.push_back(ref);
    }
    return refs;
}

constexpr const char* kQueryRefListPrefix = "qrefs:";

bytes query_tip(Height height, const Hash256& hash) {
    auto data = utils::encode_u64_le(height);
    data.insert(data.end(), hash.begin(), hash.end());
    return data;
}

void erase_tx_ref_from_map(
    std::unordered_map<std::string, std::vector<IndexedTxRef>>& index,
    const Hash256& txid
) {
    for (auto it = index.begin(); it != index.end();) {
        auto& refs = it->second;
        refs.erase(
            std::remove_if(refs.begin(), refs.end(), [&](const IndexedTxRef& ref) {
                return ref.txid == txid;
            }),
            refs.end()
        );
        if (refs.empty()) {
            it = index.erase(it);
        } else {
            ++it;
        }
    }
}
}

bool ChainState::active_tx_record_(const Hash256& txid, storage::ActiveTransactionRecord& record) const {
    if (!storage_) {
        const auto it = active_tx_index_.find(txid);
        if (it == active_tx_index_.end()) return false;
        record = it->second;
    } else {
        bytes marker;
        if (!storage_->get_metadata(std::string(kActiveTxIndexPrefixLocal) + hash256_to_hex(txid), marker)) return false;
        if (!storage::ActiveTransactionRecord::decode(marker, record)) {
            throw storage::ReadError(storage::ReadStatus::CORRUPT, "Invalid active transaction marker");
        }
    }
    if (record.height > current_height_ || is_zero_hash_accessors(record.block_hash) || record.transaction_count == 0) {
        throw storage::ReadError(storage::ReadStatus::CORRUPT, "Invalid active transaction marker");
    }
    return true;
}

void ChainState::rebuild_active_tx_index_from_active_chain() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    active_tx_index_.clear(); active_address_tx_index_.clear(); active_nft_tx_index_.clear();
    if (!block_storage_ || journal_recovery_required_) return;
    if (!storage_) {
        for (Height h = 0;; ++h) {
            core::Block block;
            if (block_storage_->get_block_by_height(h, block, false)) {
                const storage::ActiveTransactionRecord record{h, block.get_hash(), block.transactions.size()};
                for (const auto& tx : block.transactions) {
                    const Hash256 txid = tx.get_hash();
                    active_tx_index_[txid] = record;
                    index_query_refs_for_tx_(tx, h, {}, txid);
                }
            }
            if (h == current_height_) break;
        }
        return;
    }
    // Validate compact membership records; leave tx bodies and query history on disk.
    std::map<Height, std::pair<uint64_t, uint64_t>> counts;
    storage::BlockStorage::PrunedIndexAnchor anchor;
    const bool pruned = block_storage_->get_pruned_index_anchor(anchor);
    std::string after;
    for (;;) {
        const auto keys = storage_->get_keys_page(kActiveTxIndexPrefixLocal, 0, 1024, nullptr, after);
        for (const auto& key : keys) {
            bytes value; Hash256 txid{};
            storage::ActiveTransactionRecord record;
            if (!utils::hex_to_hash256_checked(key.substr(4), txid) ||
                !storage_->get_metadata(key, value) ||
                !storage::ActiveTransactionRecord::decode(value, record) ||
                record.height > current_height_ || is_zero_hash_accessors(record.block_hash)) {
                derived_state_storage_valid_ = false;
                continue;
            }
            const auto& [height, bound, expected] = record;
            Hash256 active{};
            const bool member = block_storage_->get_active_block_hash(height, active)
                ? active == bound
                : pruned && height <= anchor.height && (height < anchor.height || bound == anchor.hash);
            if (!member) { derived_state_storage_valid_ = false; continue; }
            auto& count = counts[height];
            if (count.first != 0 && count.second != expected) derived_state_storage_valid_ = false;
            ++count.first; count.second = expected;
        }
        if (keys.size() < 1024) break;
        after = keys.back();
    }
    if (is_zero_hash_accessors(current_tip_hash_)) return; // Fresh state, before genesis.
    if (counts.empty() || current_height_ != counts.size() - 1) derived_state_storage_valid_ = false;
    for (const auto& [height, count] : counts) {
        if (count.first != count.second) derived_state_storage_valid_ = false;
    }
    bytes tip;
    if (!storage_->get_metadata("query_index_tip", tip) || tip != query_tip(current_height_, current_tip_hash_)) {
        derived_state_storage_valid_ = false;
    }
}

void ChainState::persist_active_tx_index_for_block(
    const core::Block& block,
    Height height,
    std::span<const TxUndo> tx_undos,
    std::span<const Hash256> txids) {
    if (tx_undos.size() != block.transactions.size() ||
        (!txids.empty() && txids.size() != block.transactions.size())) {
        throw std::logic_error("Query indexing requires undo for every transaction");
    }
    const auto block_hash = block.get_hash();
    const storage::ActiveTransactionRecord record{height, block_hash, block.transactions.size()};
    const auto marker = record.serialize();
    for (size_t i = 0; i < block.transactions.size(); ++i) {
        const auto& tx = block.transactions[i];
        const Hash256 txid = txids.empty() ? tx.get_hash() : txids[i];
        index_query_refs_for_tx_(tx, height, tx_undos[i].spent_utxos, txid);
        if (!storage_) active_tx_index_[txid] = record;
        if (storage_) {
            record_persistence_result_(
                storage_->put_metadata(
                    std::string(kActiveTxIndexPrefixLocal) + hash256_to_hex(txid),
                    marker),
                "persist active transaction marker"
            );
        }
    }
    if (storage_ && block_storage_) {
        for (size_t i = 0; i < block.transactions.size(); ++i) {
            const auto& tx = block.transactions[i];
            // Checkpoint transactions can be referenced by later fraud proofs
            // even after leaving the active branch; keep their independent copy.
            if (tx.type == core::TransactionType::SIDECHAIN_CHECKPOINT ||
                tx.type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) continue;
            const Hash256 txid = txids.empty() ? tx.get_hash() : txids[i];
            record_persistence_result_(storage_->delete_transaction(txid), "remove duplicate transaction body");
        }
    }
    if (storage_) record_persistence_result_(storage_->put_metadata("query_index_tip", query_tip(height, block_hash)), "persist query index tip");
}

void ChainState::erase_active_tx_index_for_block(const core::Block& block) {
    for (const auto& tx : block.transactions) {
        const Hash256 txid = tx.get_hash();
        erase_query_refs_for_tx_(txid);
        active_tx_index_.erase(txid);
        const std::string txid_hex = hash256_to_hex(txid);
        if (storage_) {
            record_persistence_result_(
                storage_->delete_metadata(std::string(kActiveTxIndexPrefixLocal) + txid_hex),
                "delete active transaction marker"
            );
        }
    }
    if (storage_) record_persistence_result_(storage_->put_metadata("query_index_tip",
        query_tip(current_height_ > 0 ? current_height_ - 1 : 0, block.header.prev_block_hash)), "revert query index tip");
}

void ChainState::index_query_refs_for_tx_(
    const core::Transaction& tx,
    Height height,
    std::span<const UTXO> spent_utxos,
    const Hash256& txid) {
    const IndexedTxRef ref{txid, height};

    std::set<std::string> address_keys;
    for (const auto& output : tx.outputs) {
        const std::string address = output.address.str();
        if (!address.empty()) {
            address_keys.insert(address);
        }
    }

    for (size_t i = 0; i < tx.inputs.size(); ++i) {
        const auto& input = tx.inputs[i];
        // These outputs were captured before applying the transaction, including
        // spends of earlier transactions in this block. Do not reload their bodies.
        if (i < spent_utxos.size() && spent_utxos[i].tx_hash == input.prev_tx_hash &&
            spent_utxos[i].vout == input.vout) {
            const auto& address = spent_utxos[i].address.str();
            if (!address.empty()) address_keys.insert(address);
            continue;
        }

        // Non-UTXO reference inputs and a rebuild without Storage have no spent
        // output in undo, so retain their historical transaction lookup.
        if (!block_storage_) continue;
        const auto previous_tx = get_transaction(input.prev_tx_hash);
        if (previous_tx && input.vout < previous_tx->outputs.size()) {
            const std::string address = previous_tx->outputs[input.vout].address.str();
            if (!address.empty()) {
                address_keys.insert(address);
            }
        }
    }

    const auto suffix = query_ref_suffix(ref);
    std::vector<std::string> forward_keys;
    const auto persist_ref = [&](const std::string& prefix) {
        forward_keys.push_back(prefix + suffix);
        record_persistence_result_(storage_->put_metadata(forward_keys.back(), {}), "persist query reference");
    };
    for (const auto& address : address_keys) {
        if (storage_) { persist_ref(query_prefix("qaddr:", address)); continue; }
        auto& refs = active_address_tx_index_[address];
        const bool exists = std::any_of(refs.begin(), refs.end(), [&](const IndexedTxRef& existing) {
            return existing.txid == txid;
        });
        if (!exists) {
            refs.push_back(ref);
        }
    }

    std::set<std::string> nft_keys;
    if (tx.type == core::TransactionType::NFT_CREATE && tx.nft_create.has_value() &&
        !is_zero_hash_accessors(tx.nft_create->nft_id)) {
        nft_keys.insert(hash256_to_hex(tx.nft_create->nft_id));
    } else if (tx.type == core::TransactionType::NFT_TRANSFER && tx.nft_transfer.has_value() &&
        !is_zero_hash_accessors(tx.nft_transfer->nft_id)) {
        nft_keys.insert(hash256_to_hex(tx.nft_transfer->nft_id));
    } else if (tx.type == core::TransactionType::NFT_BURN && tx.nft_burn.has_value() &&
        !is_zero_hash_accessors(tx.nft_burn->nft_id)) {
        nft_keys.insert(hash256_to_hex(tx.nft_burn->nft_id));
    } else if (tx.type == core::TransactionType::CADASTRAL_DOC && !is_zero_hash_accessors(tx.cadastral_nft_id)) {
        nft_keys.insert(hash256_to_hex(tx.cadastral_nft_id));
    }

    for (const auto& nft_id : nft_keys) {
        if (storage_) { persist_ref(query_prefix("qnft:", nft_id)); continue; }
        auto& refs = active_nft_tx_index_[nft_id];
        const bool exists = std::any_of(refs.begin(), refs.end(), [&](const IndexedTxRef& existing) {
            return existing.txid == txid;
        });
        if (!exists) {
            refs.push_back(ref);
        }
    }
    if (storage_ && !forward_keys.empty()) {
        record_persistence_result_(
            storage_->put_metadata(std::string(kQueryRefListPrefix) + hash256_to_hex(txid),
                                   storage::QueryReferenceList::serialize(forward_keys)),
            "persist query reference list");
    }
}

void ChainState::erase_query_refs_for_tx_(const Hash256& txid) {
    if (storage_) {
        const std::string list_key = std::string(kQueryRefListPrefix) + hash256_to_hex(txid);
        bytes listed;
        if (storage_->get_metadata(list_key, listed)) {
            std::vector<std::string> keys;
            if (!storage::QueryReferenceList::decode(listed, keys)) {
                throw storage::ReadError(storage::ReadStatus::CORRUPT, "Invalid query reference list");
            }
            const auto suffix = ":" + hash256_to_hex(txid);
            if (std::any_of(keys.begin(), keys.end(), [&](const std::string& key) { return !key.ends_with(suffix); }))
                throw storage::ReadError(storage::ReadStatus::CORRUPT, "Query reference belongs to another transaction");
            for (const auto& key : keys) {
                record_persistence_result_(storage_->delete_metadata(key), "delete query reference");
            }
            record_persistence_result_(storage_->delete_metadata(list_key), "delete query reference list");
            return;
        }
        return;
    }
    erase_tx_ref_from_map(active_address_tx_index_, txid);
    erase_tx_ref_from_map(active_nft_tx_index_, txid);
}

const UTXO* ChainState::find_utxo_unlocked_(const Hash256& tx_hash, uint32_t vout) const {
    const auto it = utxo_set_.find(utxo_memory_key(tx_hash, vout));

    if (it == utxo_set_.end()) {
        return nullptr;
    }

    return &it->second;
}


Amount ChainState::get_interest_pool_balance() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return interest_pool_balance_;
}

bool ChainState::create_interest_distribution_transaction(Height next_height, core::Transaction& out_tx) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!consensus::is_interest_distribution_due(next_height)) {
        return false;
    }

    std::vector<core::TxOutput> outputs;
    Amount distributed_total = 0;
    if (!build_interest_distribution_outputs_(next_height, outputs, distributed_total)) {
        return false;
    }

    core::Transaction tx;
    tx.version = 2;
    tx.type = core::TransactionType::INTEREST_DISTRIBUTION;
    tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    tx.outputs = std::move(outputs);

    out_tx = std::move(tx);
    return true;
}

void ChainState::clear() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    utxo_set_.clear();
    utxo_balance_by_height_.clear();
    utxo_balance_index_valid_ = true;
    current_height_ = 0;
    current_tip_hash_ = Hash256{};
    chain_position_loaded_from_storage_ = false;
    validation_block_time_.reset();
    interest_pool_balance_ = 0;
    reset_foundation_config_to_defaults_();
    undo_log_.clear();
    // Keep the shared object used by the verifier, but discard its old indexes.
    angle_store_->clear();
    {
        std::lock_guard<std::mutex> cache_lock(witness_cache_mutex_);
        witness_cache_.clear();
        witness_cache_order_.clear();
    }

    // Очищаем кэши/индексы в памяти. Персистентные индексы привязаны к эпохе.
    used_delegation_nonces_.clear();
    delegations_.clear();
    revoked_delegations_cache_.clear();
    angle_commitment_keys_.clear();
    multisig_spend_keys_.clear();
    protocol_authorization_keys_.clear();
    hierarchical_spend_keys_.clear();
    delegation_proof_keys_.clear();

    // Очищаем и NFT-состояние в памяти. Оно могло быть загружено из постоянного хранилища
    // при старте, но пересборка из блоков должна начинаться с чистого реестра в памяти.
    nft_registry_ = nft::NFTRegistryImpl();
    nft_id_keys_.clear();

    sidechain_deposits_.clear();
    sidechain_registry_.clear();
    card_programs_.clear();
    card_instances_.clear();
    card_pubkey_to_uid_.clear();
    card_active_counts_.clear();
    active_tx_index_.clear();
    active_address_tx_index_.clear();
    active_nft_tx_index_.clear();
    witness_pubkey_cache_.clear();
    witness_pubkey_refcounts_.clear();
    witness_pubkey_lru_.clear();
    witness_pubkey_lru_index_.clear();
    witness_pubkey_cache_hits_ = 0;
    witness_pubkey_cache_misses_ = 0;
    witness_pubkey_bytes_saved_ = 0;
    witness_pubkey_stats_dirty_events_ = 0;
    derived_state_storage_valid_ = true;
    state_transition_poisoned_ = false;
    transition_persistence_ok_ = true;
    transition_in_progress_ = false;
}

bool ChainState::has_block(const Hash256& block_hash) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (block_storage_) {
        return block_storage_->has_block(block_hash);
    }
    return false;
}

bool ChainState::get_block(const Hash256& block_hash, core::Block& block) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (block_storage_) {
        return block_storage_->get_block(block_hash, block);
    }
    return false;
}

bool ChainState::get_block_by_height(Height height, core::Block& block) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (block_storage_) {
        return block_storage_->get_block_by_height(height, block);
    }
    return false;
}

bool ChainState::get_active_block_hash(Height height, Hash256& hash) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return block_storage_ && block_storage_->get_active_block_hash(height, hash);
}

bool ChainState::is_block_body_pruned(Height height) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return block_storage_ && block_storage_->is_block_body_pruned(height);
}

bool ChainState::has_active_transaction(const Hash256& tx_hash) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!block_storage_) {
        return storage_ && storage_->has_transaction(tx_hash);
    }
    storage::ActiveTransactionRecord record;
    return active_tx_record_(tx_hash, record);
}

std::unique_ptr<core::Transaction> ChainState::get_transaction(const Hash256& tx_hash) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!block_storage_) {
        if (!storage_) {
            return nullptr;
        }
        core::Transaction tx;
        if (!storage_->get_transaction(tx_hash, tx)) {
            return nullptr;
        }
        return std::make_unique<core::Transaction>(std::move(tx));
    }

    storage::ActiveTransactionRecord record;
    if (!active_tx_record_(tx_hash, record)) return nullptr;

    // Archived values keep active transactions readable after body pruning.
    if (storage_) {
        core::Transaction stored_tx;
        if (storage_->get_transaction(tx_hash, stored_tx)) {
            return std::make_unique<core::Transaction>(std::move(stored_tx));
        }
    }

    // During a reorg the navigation index still describes the previous branch.
    // Use the block bound to this state, without another height-index read.
    core::Block block;
    if (!block_storage_->get_block(record.block_hash, block, true)) {
        throw storage::ReadError(storage::ReadStatus::CORRUPT, "Active transaction has neither a block body nor an archive record");
    }
    if (block.transactions.size() != record.transaction_count) {
        throw storage::ReadError(storage::ReadStatus::CORRUPT, "Active transaction count does not match its block");
    }
    for (const auto& tx : block.transactions) {
        if (tx.get_hash() == tx_hash) {
            return std::make_unique<core::Transaction>(tx);
        }
    }

    // Reads never repair or delete a persisted membership marker.
    throw storage::ReadError(storage::ReadStatus::CORRUPT, "Active transaction is absent from its bound block");
}

std::unique_ptr<core::Transaction> ChainState::get_transaction_for_fraud_proof(
    const Hash256& tx_hash
) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!storage_) {
        return nullptr;
    }
    core::Transaction tx;
    if (!storage_->get_transaction(tx_hash, tx)) {
        return nullptr;
    }
    if (tx.type != core::TransactionType::SIDECHAIN_CHECKPOINT &&
        tx.type != core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
        return nullptr;
    }
    return std::make_unique<core::Transaction>(std::move(tx));
}

std::vector<IndexedTxRef> ChainState::get_address_txids(const Address& address) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (storage_) return read_query_page(*storage_, query_prefix("qaddr:", address.str()), 0, (std::numeric_limits<size_t>::max)(), nullptr);
    auto it = active_address_tx_index_.find(address.str());
    if (it == active_address_tx_index_.end()) {
        return {};
    }
    return it->second;
}

std::vector<IndexedTxRef> ChainState::get_address_txids_page(
    const Address& address,
    size_t offset,
    size_t limit,
    size_t* total
) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (storage_) return read_query_page(*storage_, query_prefix("qaddr:", address.str()), offset, limit, total);
    const auto it = active_address_tx_index_.find(address.str());
    if (it == active_address_tx_index_.end()) {
        if (total) *total = 0;
        return {};
    }
    if (total) *total = it->second.size();
    if (limit == 0 || offset >= it->second.size()) {
        return {};
    }
    const size_t count = std::min(limit, it->second.size() - offset);
    return std::vector<IndexedTxRef>(
        it->second.begin() + static_cast<std::ptrdiff_t>(offset),
        it->second.begin() + static_cast<std::ptrdiff_t>(offset + count)
    );
}

std::vector<IndexedTxRef> ChainState::get_nft_txids(const Hash256& nft_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (storage_) return read_query_page(*storage_, query_prefix("qnft:", hash256_to_hex(nft_id)), 0, (std::numeric_limits<size_t>::max)(), nullptr);
    auto it = active_nft_tx_index_.find(hash256_to_hex(nft_id));
    if (it == active_nft_tx_index_.end()) {
        return {};
    }
    return it->second;
}

std::vector<IndexedTxRef> ChainState::get_nft_txids_page(
    const Hash256& nft_id,
    size_t offset,
    size_t limit,
    size_t* total
) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (storage_) return read_query_page(*storage_, query_prefix("qnft:", hash256_to_hex(nft_id)), offset, limit, total);
    const auto it = active_nft_tx_index_.find(hash256_to_hex(nft_id));
    if (it == active_nft_tx_index_.end()) {
        if (total) *total = 0;
        return {};
    }
    if (total) *total = it->second.size();
    if (limit == 0 || offset >= it->second.size()) {
        return {};
    }
    const size_t count = std::min(limit, it->second.size() - offset);
    return std::vector<IndexedTxRef>(
        it->second.begin() + static_cast<std::ptrdiff_t>(offset),
        it->second.begin() + static_cast<std::ptrdiff_t>(offset + count)
    );
}


Amount ChainState::calculate_transaction_fee(const core::Transaction& tx) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return calculate_transaction_fee_(tx);
}

Amount ChainState::calculate_transaction_fee_(const core::Transaction& tx,
    const std::map<Hash256, const core::Transaction*>* earlier_transactions) const {
    // Coinbase транзакции не имеют комиссии
    if (tx.is_coinbase()) {
        return 0;
    }

    if (tx.type == core::TransactionType::SPEND_DEPOSIT ||
        tx.type == core::TransactionType::UNLOCK_DEPOSIT ||
        tx.type == core::TransactionType::ISSUE_SIDECHAIN ||
        tx.type == core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN ||
        tx.type == core::TransactionType::MINT_SIDECHAIN ||
        tx.type == core::TransactionType::REDEEM_SIDECHAIN ||
        tx.type == core::TransactionType::SIDECHAIN_CHECKPOINT ||
        tx.type == core::TransactionType::SLASH_DEPOSIT ||
        tx.type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT ||
        tx.type == core::TransactionType::INTEREST_DISTRIBUTION) {
        if (tx.type == core::TransactionType::ISSUE_SIDECHAIN) {
            const Amount fee = consensus::calc_sidechain_fee(0, tx.sidechain_fee_bps);
            return fee >= 0 ? fee : 0;
        }
        if (tx.type == core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN) {
            return consensus::calc_sidechain_fee(0, 0);
        }
        if (tx.type == core::TransactionType::MINT_SIDECHAIN ||
            tx.type == core::TransactionType::REDEEM_SIDECHAIN) {
            SidechainRegistryEntry entry;
            if (get_sidechain_registry(tx.sidechain_id, entry)) {
                const Amount fee = consensus::calc_sidechain_fee(tx.sidechain_amount, entry.fee_bps);
                return fee >= 0 ? fee : 0;
            }
            return 0;
        }
        if (tx.type == core::TransactionType::SIDECHAIN_CHECKPOINT) {
            return tx.sidechain_amount >= 0 ? tx.sidechain_amount : 0;
        }
        if (tx.type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
            Amount total = 0;
            for (const auto& item : tx.sidechain_batch_checkpoints) {
                Amount next_total = 0;
                if (item.fee < 0 || !checked_add_amount(total, item.fee, next_total)) {
                    return 0;
                }
                total = next_total;
            }
            return total;
        }
        return 0;
    }

    Amount total_input = 0;
    Amount total_output = 0;

    // Read only the amount; copying an entire UTXO also copies its address.
    for (const auto& input : tx.inputs) {
        const Amount* value = nullptr;
        if (earlier_transactions) {
            const auto previous = earlier_transactions->find(input.prev_tx_hash);
            if (previous != earlier_transactions->end()) {
                if (input.vout >= previous->second->outputs.size()) return 0;
                value = &previous->second->outputs[input.vout].value;
            }
        }
        if (!value) {
            const auto* utxo = find_utxo_unlocked_(input.prev_tx_hash, input.vout);
            if (!utxo) return 0;
            value = &utxo->value;
        }

        Amount next_total = 0;
        if (*value < 0 || !checked_add_amount(total_input, *value, next_total)) {
            return 0;
        }
        total_input = next_total;
    }

    // Вычисляем сумму выходов
    for (const auto& output : tx.outputs) {
        Amount next_total = 0;
        if (output.value < 0 || !checked_add_amount(total_output, output.value, next_total)) {
            return 0;
        }
        total_output = next_total;
    }

    if (tx.type == core::TransactionType::LOCK_DEPOSIT ||
        tx.type == core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT) {
        Amount required = 0;
        if (tx.sidechain_amount < 0 ||
            !checked_add_amount(total_output, tx.sidechain_amount, required)) {
            return 0;
        }
        if (total_input > required) {
            Amount fee = 0;
            return checked_sub_amount(total_input, required, fee) ? fee : 0;
        }
        return 0;
    }

    // Комиссия = входы - выходы
    if (total_input > total_output) {
        Amount fee = 0;
        return checked_sub_amount(total_input, total_output, fee) ? fee : 0;
    }
    return 0;
}

void ChainState::add_angle_commitment_to_store(const core::Transaction& tx) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (tx.type == core::TransactionType::ANGLE_COMMITMENT && angle_store_) {
        angle_store_->add_commitment(tx);
    }
}

void ChainState::remove_mempool_angle_commitment_from_store(const Hash256& tx_hash) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!angle_store_ || transition_in_progress_) {
        return;
    }

    // Block application owns this mutex for the whole transition and retires
    // confirmed same-block envelopes before releasing it. Cleanup of a pending
    // envelope therefore needs no historical transaction or storage read.
    angle_store_->remove_commitment(tx_hash);
}

} // namespace geocoin::state
