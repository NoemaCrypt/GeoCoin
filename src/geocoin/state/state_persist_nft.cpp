#include "geocoin/state/state.hpp"
#include "geocoin/state/state_internal.hpp"

#include "geocoin/crypto/addresses.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"

#include <exception>
#include <string>
#include <utility>

namespace geocoin::state {

namespace {
template <typename Operation>
bool run_nft_storage_operation(const char* failure_message, Operation&& operation) {
    try {
        return std::forward<Operation>(operation)();
    } catch (const std::exception& error) {
        GEO_LOG_ERROR("chainstate", failure_message << ": " << error.what());
    } catch (...) {
        GEO_LOG_ERROR("chainstate", failure_message);
    }
    return false;
}
}

bool ChainState::persist_nft_owner_to_storage(const Hash256& nft_id, const Address& owner) {
    if (!storage_) {
        return true;
    }
    return run_nft_storage_operation("Failed to persist NFT owner", [&] {
        const std::string id_hex = hash256_to_hex(nft_id);
        const std::string key = "nft:owner:" + id_hex;
        const std::string owner_str = owner.str();
        return storage_->put_metadata(key, bytes(owner_str.begin(), owner_str.end()));
    });
}

bool ChainState::persist_nft_metadata_to_storage(const Hash256& nft_id, const nft::NFTMetadata& metadata) {
    if (!storage_) {
        return true;
    }
    return run_nft_storage_operation("Failed to persist NFT metadata", [&] {
        const std::string id_hex = hash256_to_hex(nft_id);
        const std::string key = "nft:meta:" + id_hex;
        return storage_->put_metadata(key, metadata.serialize());
    });
}

bool ChainState::delete_nft_from_storage(const Hash256& nft_id) {
    if (!storage_) {
        return true;
    }
    std::string nft_id_hex;
    const bool id_encoded = run_nft_storage_operation(
        "Failed to encode NFT id for deletion",
        [&] {
            nft_id_hex = hash256_to_hex(nft_id);
            return true;
        }
    );
    if (!id_encoded) {
        return false;
    }

    // Attempt both halves even if the first deletion fails.  This makes a
    // partial backend result observable to the caller while still removing as
    // much stale state as possible.
    const auto safe_delete = [&](const std::string& key) {
        return run_nft_storage_operation("Failed to delete NFT record", [&] {
            return storage_->delete_metadata(key);
        });
    };
    const bool owner_ok = safe_delete("nft:owner:" + nft_id_hex);
    const bool metadata_ok = safe_delete("nft:meta:" + nft_id_hex);
    return owner_ok && metadata_ok;
}

void ChainState::load_nft_registry_from_storage() {
    if (!storage_) {
        return;
    }

    nft_registry_ = nft::NFTRegistryImpl();
    nft_id_keys_.clear();

    const std::string owner_prefix = "nft:owner:";
    const std::string meta_prefix = "nft:meta:";
    auto keys = storage_->get_keys_with_prefix(owner_prefix);
    auto metadata_keys = storage_->get_keys_with_prefix(meta_prefix);
    keys.insert(keys.end(), metadata_keys.begin(), metadata_keys.end());
    bool corrupt = false;

    for (const auto& k : keys) {
        if (!metadata_key_starts_with(k, owner_prefix)) {
            continue;
        }
        const std::string id_hex = k.substr(owner_prefix.size());
        if (id_hex.size() != 64) {
            corrupt = true;

            continue;
        }

        Hash256 nft_id{};
        if (!utils::hex_to_hash256_checked(id_hex, nft_id) || hash256_to_hex(nft_id) != id_hex) {
            corrupt = true;

            continue;
        }

        bytes owner_bytes;
        bytes meta_bytes;
        if (!storage_->get_metadata(k, owner_bytes)) {
            corrupt = true;
            continue;
        }
        const std::string meta_key = meta_prefix + id_hex;
        if (!storage_->get_metadata(meta_key, meta_bytes)) {
            corrupt = true;

            continue;
        }

        const std::string owner_str(owner_bytes.begin(), owner_bytes.end());
        nft::NFTCreationTransaction create_tx;
        create_tx.nft_id = nft_id;
        create_tx.creator_address = Address(owner_str);
        create_tx.royalty_percent = 0;
        try {
            create_tx.metadata = nft::NFTMetadata::deserialize(meta_bytes);
        } catch (...) {
            corrupt = true;


            continue;
        }
        if (!crypto::verify_address(create_tx.creator_address) ||
            create_tx.metadata.calculate_hash() != nft_id) {
            corrupt = true;


            continue;
        }

        // register_nft задает начального владельца = creator_address.
        if (!nft_registry_.register_nft(create_tx)) {
            corrupt = true;


            continue;
        }

        // Заполняем индекс уникальности и в памяти.
        nft_id_keys_.insert(id_hex);

        // Гарантируем, что контекстный индекс уникальности существует для этой эпохи.
        if (!storage_->has_metadata(idx_meta_key("nft", id_hex))) {
            corrupt = true;
        }
    }

    // Metadata without its owner half is also an incomplete consensus record.
    for (const auto& k : keys) {
        if (!metadata_key_starts_with(k, meta_prefix)) {
            continue;
        }
        const std::string id_hex = k.substr(meta_prefix.size());
        Hash256 nft_id{};
        if (id_hex.size() != 64 ||
            !utils::hex_to_hash256_checked(id_hex, nft_id) ||
            hash256_to_hex(nft_id) != id_hex ||
            !storage_->has_metadata(owner_prefix + id_hex)) {
            corrupt = true;

        }
    }

    if (corrupt) {
        derived_state_storage_valid_ = false;
        GEO_LOG_ERROR("chainstate", "Corrupt NFT registry snapshot; rebuild required");
    }
}

} // namespace geocoin::state
