#include "geocoin/state/nft_projection.hpp"
#include "geocoin/state/state.hpp"

namespace geocoin::state {
namespace {
struct NFTOverlay {
    // nullopt records a burn; new metadata is copied only for creations.
    std::map<Hash256, std::optional<Address>> owners;
    std::map<Hash256, nft::NFTMetadata> creations;
};
}

class NFTReadView : public nft::NFTRegistryView {
public:
    NFTReadView(const ChainState& state, const NFTOverlay& overlay)
        : state_(state), lock_(state.mutex_), overlay_(overlay) {}

    Address get_owner(const Hash256& id) const override {
        const auto it = overlay_.owners.find(id);
        return it == overlay_.owners.end() ? state_.nft_registry_.get_owner(id) : it->second.value_or(Address{});
    }

    nft::NFTMetadata get_metadata(const Hash256& id) const override {
        const auto owner = overlay_.owners.find(id);
        if (owner != overlay_.owners.end() && !owner->second) return {};
        const auto it = overlay_.creations.find(id);
        return it == overlay_.creations.end() ? state_.nft_registry_.get_metadata(id) : it->second;
    }

    bool exists(const Hash256& id) const override {
        const auto it = overlay_.owners.find(id);
        return it == overlay_.owners.end() ? state_.nft_registry_.exists(id) : it->second.has_value();
    }

    bool for_each_cadastral(const CadastralVisitor& visitor) const override {
        if (!state_.nft_registry_.for_each_cadastral([&](const Hash256& id, const nft::NFTMetadata& metadata) {
                const auto owner = overlay_.owners.find(id);
                if (owner != overlay_.owners.end() && !owner->second) return true;
                return visitor(id, metadata);
            })) return false;
        for (const auto& [id, metadata] : overlay_.creations) {
            if (metadata.cadastral && exists(id) && !visitor(id, metadata)) return false;
        }
        return true;
    }

    bool for_each_cadastral_candidate(const nft::NFTMetadata& metadata, const CadastralVisitor& visitor) const override {
        if (!state_.nft_registry_.for_each_cadastral_candidate(metadata, [&](const Hash256& id, const nft::NFTMetadata& existing) {
                const auto owner = overlay_.owners.find(id);
                if (owner != overlay_.owners.end() && !owner->second) return true;
                return visitor(id, existing);
            })) return false;
        for (const auto& [id, existing] : overlay_.creations)
            if (existing.cadastral && exists(id) && !visitor(id, existing)) return false;
        return true;
    }
    std::shared_ptr<const nft::coordinates::PreparedGeometry> prepared_geometry(const Hash256& id) const override {
        if (!exists(id)) return {};
        if (overlay_.creations.contains(id)) return NFTRegistryView::prepared_geometry(id);
        return state_.nft_registry_.prepared_geometry(id);
    }

private:
    const ChainState& state_;
    std::unique_lock<std::recursive_mutex> lock_;
    const NFTOverlay& overlay_;
};

struct NFTProjection::Impl {
    const ChainState& state;
    NFTOverlay overlay;
};

NFTProjection::NFTProjection(const ChainState& state)
    : impl_(std::make_unique<Impl>(Impl{state, {}})) {}

NFTProjection::~NFTProjection() = default;

bool NFTProjection::try_apply(const core::Transaction& tx) {
    NFTReadView view(impl_->state, impl_->overlay);
    if (tx.type == core::TransactionType::CADASTRAL_DOC) {
        if (!view.exists(tx.cadastral_nft_id) || view.get_owner(tx.cadastral_nft_id) != tx.cadastral_owner)
            return false;
        const auto metadata = view.get_metadata(tx.cadastral_nft_id);
        return metadata.cadastral && (tx.cadastral_number.empty() ||
            tx.cadastral_number == metadata.cadastral->cadastral_number);
    }
    if (tx.type == core::TransactionType::NFT_CREATE) {
        if (!tx.nft_create) return false;
        const auto& create = *tx.nft_create;
        // Burning releases cadastral land, but never a historical NFT id.
        if (impl_->overlay.creations.count(create.nft_id) > 0 ||
            impl_->state.has_nft_id_key(create.nft_id) ||
            !nft::NFTValidator::validate_creation_state(create, view)) return false;
        impl_->overlay.creations.emplace(create.nft_id, create.metadata);
        impl_->overlay.owners[create.nft_id] = create.creator_address;
        return true;
    }
    if (tx.type == core::TransactionType::NFT_TRANSFER) {
        if (!tx.nft_transfer || !nft::NFTValidator::validate_transfer_state(*tx.nft_transfer, view)) return false;
        const auto& transfer = *tx.nft_transfer;
        const bool burns = nft::NFTValidator::transfer_burns_nft(view.get_metadata(transfer.nft_id));
        impl_->overlay.owners[transfer.nft_id] = burns ? std::nullopt : std::optional<Address>(transfer.to);
        return true;
    }
    if (tx.type == core::TransactionType::NFT_BURN) {
        if (!tx.nft_burn || !nft::NFTValidator::validate_burn_state(*tx.nft_burn, view)) return false;
        impl_->overlay.owners[tx.nft_burn->nft_id] = std::nullopt;
        return true;
    }
    return false;
}
} // namespace geocoin::state
