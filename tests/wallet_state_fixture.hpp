#pragma once

#include "geocoin/state/state.hpp"

namespace geocoin::state {

// Wallet boundary/builder fixtures may need synthetic balances or a stored
// unsigned draft. Keep that setup explicit and outside the production API.
struct ChainStateTestAccess {
    static void seed_funding(ChainState& state, const core::Transaction& tx, Height height) {
        for (uint32_t i = 0; i < tx.outputs.size(); ++i) {
            UTXO utxo;
            utxo.tx_hash = tx.get_hash();
            utxo.vout = i;
            utxo.value = tx.outputs[i].value;
            utxo.address = tx.outputs[i].address;
            utxo.height = height;
            utxo.is_coinbase = true;
            state.add_utxo(utxo);
        }
    }
    static bool store_delegation(ChainState& state, const core::Transaction& tx, Height height) {
        return state.apply_delegation_proof_(tx, height);
    }
};

} // namespace geocoin::state
