#include "geocoin/state/state.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/bytes.hpp"

#include <iterator>
#include <mutex>
#include <utility>

namespace geocoin::state {

Amount ChainState::get_delegation_total_spent(const Hash256& delegation_proof_txid) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string key = hash256_to_hex(delegation_proof_txid);
    auto it = delegations_.find(key);
    return (it == delegations_.end()) ? 0 : it->second.total_spent;
}

bool ChainState::has_angle_commitment_key(const Hash256& key) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string hex = hash256_to_hex(key);
    if (angle_commitment_keys_.count(hex) > 0) {
        return true;
    }
    if (storage_) {
        return storage_->has_metadata(idx_meta_key("ac", hex));
    }
    return false;
}

bool ChainState::has_multisig_spend_key(const Hash256& key) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string hex = hash256_to_hex(key);
    if (multisig_spend_keys_.count(hex) > 0) {
        return true;
    }
    if (storage_) {
        return storage_->has_metadata(idx_meta_key("ms", hex));
    }
    return false;
}

bool ChainState::has_protocol_authorization_key(const Hash256& key) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto hex = hash256_to_hex(key);
    return protocol_authorization_keys_.count(hex) > 0 ||
           (storage_ && storage_->has_metadata(idx_meta_key("pa", hex)));
}

bool ChainState::has_hierarchical_spend_key(const Hash256& key) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string hex = hash256_to_hex(key);
    if (hierarchical_spend_keys_.count(hex) > 0) {
        return true;
    }
    if (storage_) {
        return storage_->has_metadata(idx_meta_key("hs", hex));
    }
    return false;
}

bool ChainState::has_delegation_proof_key(const Hash256& key) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string hex = hash256_to_hex(key);
    if (delegation_proof_keys_.count(hex) > 0) {
        return true;
    }
    if (storage_) {
        return storage_->has_metadata(idx_meta_key("dp", hex));
    }
    return false;
}

bool ChainState::has_nft_id_key(const Hash256& nft_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string hex = hash256_to_hex(nft_id);
    if (nft_id_keys_.count(hex) > 0) {
        return true;
    }
    if (storage_) {
        return storage_->has_metadata(idx_meta_key("nft", hex));
    }
    return false;
}

bool ChainState::has_sidechain_deposit_key(const Hash256& sidechain_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string hex = hash256_to_hex(sidechain_id);
    if (sidechain_deposits_.count(hex) > 0) {
        return true;
    }
    if (storage_) {
        return storage_->has_metadata("scd:" + hex);
    }
    return false;
}

bool ChainState::has_sidechain_registry_key(const Hash256& sidechain_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string hex = hash256_to_hex(sidechain_id);
    if (sidechain_registry_.count(hex) > 0) {
        return true;
    }
    if (storage_) {
        return storage_->has_metadata("sc:reg:" + hex);
    }
    return false;
}

bool ChainState::has_accounting_namespace(const std::string& accounting_namespace) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (accounting_namespace.empty()) {
        return false;
    }
    for (const auto& [id_hex, entry] : sidechain_registry_) {
        (void)id_hex;
        if (entry.accounting_namespace == accounting_namespace) {
            return true;
        }
    }
    return false;
}

bool ChainState::has_witness_pubkey(const Hash256& pubkey_hash) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    bool cache_hit = false;
    return find_witness_pubkey_(pubkey_hash, cache_hit) != nullptr;
}

bool ChainState::get_witness_pubkey(const Hash256& pubkey_hash, bytes& out_pubkey) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    bool cache_hit = false;
    const auto* pub = find_witness_pubkey_(pubkey_hash, cache_hit);
    if (!pub) return false;
    out_pubkey = *pub;
    if (cache_hit) {
        ++witness_pubkey_cache_hits_;
        witness_pubkey_bytes_saved_ += static_cast<uint64_t>(pub->size());
    } else {
        ++witness_pubkey_cache_misses_;
    }
    ++witness_pubkey_stats_dirty_events_;
    persist_witness_pubkey_stats_to_storage(false);
    return true;
}

const bytes* ChainState::find_witness_pubkey_(const Hash256& pubkey_hash, bool& cache_hit) const {
    cache_hit = false;
    const std::string hex = hash256_to_hex(pubkey_hash);
    const uint64_t refcount = witness_refcount_get_(hex);
    if (refcount == 0) {
        if (storage_ && storage_->has_metadata("wpk:" + hex)) {
            throw storage::ReadError(storage::ReadStatus::CORRUPT, "Witness public key has no reference count");
        }
        return nullptr;
    }
    auto it = witness_pubkey_cache_.find(hex);
    if (it != witness_pubkey_cache_.end()) {
        witness_cache_touch_lru_(hex);
        cache_hit = true;
        return &it->second;
    }
    if (!storage_) {
        return nullptr;
    }

    bytes pub;
    if (!storage_->get_metadata("wpk:" + hex, pub) ||
        pub.size() != consensus::params::WITNESS_PUBKEY_SIZE || crypto::sha256(pub) != pubkey_hash) {
        throw storage::ReadError(storage::ReadStatus::CORRUPT, "Witness public key is missing or does not match its record");
    }
    // A refcount is cached only after its complete pair has been validated.
    witness_cache_put_(hex, std::move(pub));
    witness_pubkey_refcounts_[hex] = refcount;
    return &witness_pubkey_cache_.at(hex);
}

WitnessPubkeyStats ChainState::get_witness_pubkey_stats() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    persist_witness_pubkey_stats_to_storage(true);
    WitnessPubkeyStats out;
    out.total_keys = static_cast<uint64_t>(witness_pubkey_refcounts_.size());
    out.cache_hits = witness_pubkey_cache_hits_;
    out.cache_misses = witness_pubkey_cache_misses_;
    out.bytes_saved = witness_pubkey_bytes_saved_;
    return out;
}

void ChainState::witness_cache_touch_lru_(const std::string& hex) const {
    auto it = witness_pubkey_lru_index_.find(hex);
    if (it == witness_pubkey_lru_index_.end()) {
        return;
    }
    witness_pubkey_lru_.splice(witness_pubkey_lru_.end(), witness_pubkey_lru_, it->second);
    it->second = std::prev(witness_pubkey_lru_.end());
}

void ChainState::witness_cache_put_(const std::string& hex, bytes pubkey) const {
    const auto [it, inserted] = witness_pubkey_cache_.try_emplace(hex, std::move(pubkey));
    if (inserted) {
        witness_pubkey_lru_.push_back(hex);
        witness_pubkey_lru_index_[hex] = std::prev(witness_pubkey_lru_.end());
    } else {
        it->second = std::move(pubkey);
        witness_cache_touch_lru_(hex);
    }
    witness_cache_evict_if_needed_();
}

void ChainState::witness_cache_evict_if_needed_() const {
    while (witness_pubkey_cache_.size() > MAX_WITNESS_CACHE_SIZE && !witness_pubkey_lru_.empty()) {
        const std::string oldest = witness_pubkey_lru_.front();
        witness_pubkey_lru_.pop_front();
        witness_pubkey_lru_index_.erase(oldest);
        witness_pubkey_cache_.erase(oldest);
    }
}

uint64_t ChainState::witness_refcount_get_(const std::string& hex) const {
    auto it = witness_pubkey_refcounts_.find(hex);
    if (it != witness_pubkey_refcounts_.end()) {
        return it->second;
    }
    if (!storage_) {
        return 0;
    }
    bytes raw;
    uint64_t count = 0;
    if (!storage_->get_metadata("wpkref:" + hex, raw)) return 0;
    if (!utils::decode_u64_le_exact(raw, count) || count == 0) {
        throw storage::ReadError(storage::ReadStatus::CORRUPT, "Invalid witness public key reference count");
    }
    return count;
}

void ChainState::witness_refcount_set_(const std::string& hex, uint64_t count) {
    if (count == 0) {
        witness_pubkey_refcounts_.erase(hex);
        if (storage_) {
            record_persistence_result_(
                storage_->delete_metadata("wpkref:" + hex),
                "delete witness public key refcount"
            );
        }
        return;
    }
    witness_pubkey_refcounts_[hex] = count;
    if (storage_) {
        record_persistence_result_(
            storage_->put_metadata("wpkref:" + hex, utils::encode_u64_le(count)),
            "persist witness public key refcount"
        );
    }
}

bool ChainState::witness_refcount_decrement_(const std::string& hex) {
    const uint64_t current = witness_refcount_get_(hex);
    if (current == 0) {
        return false;
    }
    const uint64_t next = current - 1;
    witness_refcount_set_(hex, next);
    return next == 0;
}

bool ChainState::get_sidechain_registry_by_id_hex(const std::string& sidechain_id_hex, SidechainRegistryEntry& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Hash256 id{};
    if (!utils::hex_to_hash256_checked(sidechain_id_hex, id)) {
        return false;
    }
    return get_sidechain_registry(id, out);
}

Address ChainState::get_foundation_address() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return foundation_config_.address;
}

FoundationConfig ChainState::get_foundation_config() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return foundation_config_;
}

std::vector<Hash256> ChainState::get_owned_nfts(const Address& owner) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return nft_registry_.get_owned_nfts(owner);
}

std::vector<Hash256> ChainState::get_owned_nfts_page(
    const Address& owner,
    size_t offset,
    size_t limit,
    size_t* total_out
) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return nft_registry_.get_owned_nfts_page(owner, offset, limit, total_out);
}

Address ChainState::get_nft_owner(const Hash256& nft_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return nft_registry_.get_owner(nft_id);
}

nft::NFTMetadata ChainState::get_nft_metadata(const Hash256& nft_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return nft_registry_.get_metadata(nft_id);
}

bool ChainState::nft_exists(const Hash256& nft_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return nft_registry_.exists(nft_id);
}

bool ChainState::has_revoke_delegation_key(const Hash256& delegation_proof_txid) const {
    return is_delegation_revoked(delegation_proof_txid);
}

bool ChainState::is_delegation_revoked(const Hash256& delegation_proof_txid) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string key = hash256_to_hex(delegation_proof_txid);
    if (revoked_delegations_cache_.count(key) > 0) {
        return true;
    }
    if (storage_) {
        return storage_->has_metadata(idx_meta_key("rd", key));
    }
    return false;
}

}  // namespace geocoin::state
