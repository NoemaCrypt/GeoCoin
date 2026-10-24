#include "geocoin/state/sidechain_projection.hpp"
#include "geocoin/state/state_sidechain_internal.hpp"

#include <set>

namespace geocoin::state {
struct SidechainProjection::Impl {
    const ChainState& state;
    Height height;
    SidechainOverlay overlay;
    std::set<Hash256> authorizations;
};

SidechainProjection::SidechainProjection(const ChainState& state, Height height)
    : impl_(std::make_unique<Impl>(Impl{state, height, {}, {}})) {}

SidechainProjection::~SidechainProjection() = default;

bool SidechainProjection::try_apply(const core::Transaction& tx) {
    const auto authorization = tx.get_protocol_authorization_hash();
    if (authorization && (impl_->authorizations.count(*authorization) > 0 ||
                          impl_->state.has_protocol_authorization_key(*authorization))) return false;
    SidechainReadView view(impl_->state, &impl_->overlay);
    SidechainTransition transition;
    if (!prepare_sidechain_transition(view, tx, impl_->height, transition)) return false;

    // Validate the entire operation (including every item of a checkpoint
    // batch) before publishing any changes to subsequent candidates.
    for (const auto& [id, entry] : transition.registries) {
        SidechainRegistryEntry previous;
        if (view.get_sidechain_registry(id, previous) && !previous.accounting_namespace.empty()) {
            impl_->overlay.namespaces[previous.accounting_namespace] = false;
        }
        if (entry && !entry->accounting_namespace.empty()) {
            impl_->overlay.namespaces[entry->accounting_namespace] = true;
        }
    }
    for (auto& [id, deposit] : transition.deposits) impl_->overlay.deposits[id] = std::move(deposit);
    for (auto& [id, entry] : transition.registries) impl_->overlay.registries[id] = std::move(entry);
    if (authorization) impl_->authorizations.insert(*authorization);
    return true;
}
} // namespace geocoin::state
