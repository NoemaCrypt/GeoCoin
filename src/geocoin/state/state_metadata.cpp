#include "geocoin/state/state.hpp"
#include "geocoin/state/storage_internal.hpp"
#include "geocoin/utils/logger.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <openssl/rand.h>

namespace geocoin::state {
namespace {

constexpr const char* kChainstateJournalKeyLocal = "chainstate_journal";
constexpr const char* kInterestPoolBalanceKeyLocal = "interest_pool_balance";
constexpr const char* kActiveTxIndexPrefixLocal = "atx:";
constexpr uint8_t kJournalVersionLocal = 1;
constexpr uint8_t kJournalPhaseRebuildLocal = 3;

bool starts_with_local(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool is_utxo_metadata_key_local(const std::string& key) {
    return key.rfind("utxo:", 0) == 0;
}

bool valid_index_epoch(const bytes& epoch) {
    return epoch.size() == 32 && std::all_of(epoch.begin(), epoch.end(), [](byte c) {
        return (c >= static_cast<byte>('0') && c <= static_cast<byte>('9')) ||
               (c >= static_cast<byte>('a') && c <= static_cast<byte>('f'));
    });
}

}  // namespace

std::string ChainState::idx_meta_key(const std::string& kind, const std::string& hex) const {
    // тип: ac/ms/dp/rd
    return "idx:" + idx_epoch_ + ":" + kind + ":" + hex;
}

std::string ChainState::generate_index_epoch() {
    // This namespace prevents stale anti-replay index keys from becoming live
    // again after a reset. Generate it with the process CSPRNG and fail closed.
    std::array<byte, 16> random_epoch{};
    if (RAND_bytes(random_epoch.data(), static_cast<int>(random_epoch.size())) != 1) {
        throw std::runtime_error("CSPRNG failure while creating chainstate index epoch");
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const byte value : random_epoch) {
        oss << std::setw(2) << static_cast<unsigned int>(value);
    }
    return oss.str();
}

void ChainState::load_index_epoch_from_storage() {
    if (!storage_) {
        if (idx_epoch_.empty()) {
            idx_epoch_ = generate_index_epoch();
        }
        return;
    }

    bytes epoch_bytes;
    const bool epoch_key_exists = storage_->has_metadata("idx_epoch");
    if (storage_->get_metadata("idx_epoch", epoch_bytes) && valid_index_epoch(epoch_bytes)) {
        idx_epoch_.assign(epoch_bytes.begin(), epoch_bytes.end());
        return;
    }

    // A missing/invalid epoch is normal only for a completely fresh state DB.
    // On an existing snapshot it would silently switch every persisted
    // anti-replay lookup to an empty namespace.
    const auto existing_keys = storage_->get_all_keys();
    if (epoch_key_exists || !existing_keys.empty()) {
        derived_state_storage_valid_ = false;
        GEO_LOG_ERROR(
            "chainstate",
            "Missing or invalid index epoch in non-empty state storage; rebuild required"
        );
        idx_epoch_.clear();
        return;
    }

    const std::string fresh_epoch = generate_index_epoch();
    if (!storage_->put_metadata(
            "idx_epoch",
            bytes(fresh_epoch.begin(), fresh_epoch.end()))) {
        throw std::runtime_error("Failed to persist chainstate index epoch");
    }
    idx_epoch_ = fresh_epoch;
}

void ChainState::reset_index_epoch() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Инвалидируем все старые idx:* через смену эпохи.
    const std::string fresh_epoch = generate_index_epoch();
    if (storage_ && !storage_->put_metadata(
            "idx_epoch",
            bytes(fresh_epoch.begin(), fresh_epoch.end()))) {
        throw std::runtime_error("Failed to persist replacement chainstate index epoch");
    }
    idx_epoch_ = fresh_epoch;
    angle_commitment_keys_.clear();
    multisig_spend_keys_.clear();
    protocol_authorization_keys_.clear();
    hierarchical_spend_keys_.clear();
    delegation_proof_keys_.clear();
    revoked_delegations_cache_.clear();
    nft_id_keys_.clear();
}

bool ChainState::write_state_journal(Height height, const Hash256& block_hash, uint8_t phase) {
    if (!storage_) {
        return false;
    }

    bytes data;
    data.reserve(1 + 1 + 8 + 32);
    data.push_back(kJournalVersionLocal);
    data.push_back(phase);
    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(height) >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), block_hash.begin(), block_hash.end());

    return storage_->put_metadata(kChainstateJournalKeyLocal, data);
}

bool ChainState::clear_state_journal() {
    if (!storage_) {
        return false;
    }
    return storage_->delete_metadata(kChainstateJournalKeyLocal);
}

bool ChainState::load_state_journal(JournalEntry& out) const {
    if (!storage_) {
        return false;
    }
    bytes data;
    if (!storage_->get_metadata(kChainstateJournalKeyLocal, data)) {
        return false;
    }
    if (data.size() != (1 + 1 + 8 + 32)) {
        return false;
    }

    out.version = data[0];
    out.phase = data[1];
    out.height = 0;
    size_t offset = 2;
    for (int i = 0; i < 8; i++) {
        out.height |= static_cast<uint64_t>(data[offset++]) << (i * 8);
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, out.block_hash.begin());
    return true;
}

bool ChainState::purge_state_metadata_for_rebuild() {
    if (!storage_) {
        return false;
    }
    bool success = true;
    const auto keys = storage_->get_all_keys();
    for (const auto& key : keys) {
        if (key == "height" ||
            key == "chainstate_height" ||
            key == "chainstate_tip_hash" ||
            key == "chainstate_utxo_count" ||
            key == "foundation_cfg" ||
            key == kInterestPoolBalanceKeyLocal) {
            success = storage_->delete_metadata(key) && success;
            continue;
        }
        if (starts_with_local(key, "addr:utxo:") || starts_with_local(key, "qaddr:") ||
            starts_with_local(key, "qnft:") || starts_with_local(key, "qrefs:") || key == "query_index_tip" ||
            starts_with_local(key, "nft:") ||
            starts_with_local(key, "scd:") ||
            starts_with_local(key, "sc:reg:") ||
            starts_with_local(key, "delegation:") ||
            starts_with_local(key, "card:prog:") ||
            starts_with_local(key, "card:uid:") ||
            starts_with_local(key, "undo:") ||
            starts_with_local(key, "idx:") ||
            starts_with_local(key, kActiveTxIndexPrefixLocal) ||
            starts_with_local(key, "wpk:") ||
            starts_with_local(key, "wpkref:") ||
            key == "wpk_stats" ||
            is_utxo_metadata_key_local(key)) {
            success = storage_->delete_metadata(key) && success;
            continue;
        }
    }
    return success;
}

bool ChainState::recover_from_journal_if_needed() {
    journal_recovery_required_ = storage_ &&
        (storage_->has_metadata(kChainstateJournalKeyLocal) ||
         storage_->has_metadata("chainstate_rebuild_pending") || storage_->has_incomplete_split_commit());
    if (journal_recovery_required_) {
        derived_state_storage_valid_ = false;
        GEO_LOG_WARN("chainstate", "Pending state recovery; preserving snapshot and journal until history is verified");
    }
    return journal_recovery_required_;
}

bool ChainState::prepare_rebuild() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (storage_) {
        if (!storage_->create_recovery_checkpoint() ||
            !storage_->put_metadata("chainstate_rebuild_pending", bytes{1})) return false;
        storage_->begin_batch();
        if (!purge_state_metadata_for_rebuild() || !storage_->commit_batch()) {
            storage_->abort_batch();
            derived_state_storage_valid_ = false;
            return false;
        }
    }
    clear();
    journal_recovery_required_ = false;
    chain_position_loaded_from_storage_ = false;
    reset_index_epoch();
    return true;
}

bool ChainState::finish_rebuild() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!has_derived_state_snapshot()) return false;
    if (storage_) {
        if (!clear_state_journal() || !storage_->delete_metadata("chainstate_rebuild_pending") ||
            !storage_->delete_metadata(kSplitCommitPendingKey)) return false;
        storage_->discard_recovery_checkpoint();
    }
    journal_recovery_required_ = false;
    return true;
}

bool ChainState::recover_from_storage_mismatch() {
    if (!storage_ || !block_storage_) {
        return false;
    }

    Height stored_height = 0;
    bool has_height = false;
    bytes height_data;
    if (storage_->get_metadata("chainstate_height", height_data) && height_data.size() == 8) {
        stored_height = 0;
        for (int i = 0; i < 8; i++) {
            stored_height |= (uint64_t)height_data[i] << (i * 8);
        }
        has_height = true;
    }

    const Height best_height = block_storage_->get_best_height();
    bool mismatch = false;

    if (has_height && best_height < stored_height) {
        mismatch = true;
    }

    bytes tip_hash_data;
    if (storage_->get_metadata("chainstate_tip_hash", tip_hash_data) && tip_hash_data.size() == 32) {
        Hash256 tip_hash;
        std::memcpy(tip_hash.data(), tip_hash_data.data(), 32);
        if (!block_storage_->has_block(tip_hash)) {
            mismatch = true;
        }
    }

    if (!mismatch) {
        return false;
    }

    GEO_LOG_WARN("chainstate", "Detected storage mismatch, forcing rebuild from BlockStorage");
    journal_recovery_required_ = true;
    if (!write_state_journal(
            stored_height,
            current_tip_hash_,
            kJournalPhaseRebuildLocal)) {
        derived_state_storage_valid_ = false;
        GEO_LOG_ERROR("chainstate", "Failed to persist storage-mismatch rebuild journal");
        return true;
    }
    derived_state_storage_valid_ = false;
    // Blockchain chooses a recoverable history before replacing any state.

    return true;
}

std::string ChainState::hash256_to_hex(const Hash256& h) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : h) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

}  // namespace geocoin::state
