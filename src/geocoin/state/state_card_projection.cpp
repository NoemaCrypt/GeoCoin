#include "geocoin/state/card_projection.hpp"
#include "geocoin/state/state_card_internal.hpp"

#include "geocoin/utils/bytes.hpp"

namespace geocoin::state {
struct CardProjection::Impl {
    const ChainState& state;
    Height height;
    CardOverlay overlay;
};

CardProjection::CardProjection(const ChainState& state, Height height)
    : impl_(std::make_unique<Impl>(Impl{state, height, {}})) {}

CardProjection::~CardProjection() = default;

bool CardProjection::try_apply(const core::Transaction& tx) {
    CardReadView view(impl_->state, &impl_->overlay);
    CardTransition transition;
    if (!validate_card_transition(view, tx) ||
        !prepare_card_effects(view, tx, impl_->height, transition)) return false;

    // Publish only fully validated effects. A rejected candidate must not
    // reserve a UID, public key, capacity or a change of authority.
    if (transition.card) {
        const auto& card = *transition.card;
        size_t active = view.active_card_count(card.program_id);
        CardInstanceEntry previous;
        if (view.get_card(card.uid_hash, previous) && previous.status != 2) --active;
        if (card.status != 2) ++active;
        impl_->overlay.active_counts[utils::hash256_to_hex(card.program_id)] = active;
        const auto uid = utils::hash256_to_hex(card.uid_hash);
        impl_->overlay.pubkeys.insert(utils::hash256_to_hex(card.pubkey_hash));
        impl_->overlay.cards[uid] = std::move(*transition.card);
    }
    if (transition.program) {
        impl_->overlay.programs[utils::hash256_to_hex(tx.card_program_id)] = std::move(*transition.program);
    }
    return true;
}
} // namespace geocoin::state
