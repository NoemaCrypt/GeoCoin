#include "geocoin/state/state.hpp"
#include "geocoin/state/state_sidechain_internal.hpp"

namespace geocoin::state {

bool ChainState::validate_sidechain_transaction_(const core::Transaction& tx, bool relay_hardening) const {
    SidechainTransition transition;
    const SidechainReadView view = SidechainReadView(*this).with_hardening(relay_hardening);
    return prepare_sidechain_transition(view, tx, current_height_ + 1, transition) &&
           (!transition.spends_inputs || verify_owned_utxo_inputs_(tx));
}

} // namespace geocoin::state
