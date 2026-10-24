#include "geocoin/state/state.hpp"
#include "geocoin/state/state_sidechain_internal.hpp"

namespace geocoin::state {

bool ChainState::validate_sidechain_transaction_(const core::Transaction& tx) const {
    SidechainTransition transition;
    return prepare_sidechain_transition(*this, tx, current_height_ + 1, transition) &&
           (!transition.spends_inputs || verify_owned_utxo_inputs_(tx));
}

} // namespace geocoin::state
