#pragma once

#include "geocoin/types.hpp"

#include <memory>

namespace geocoin::core { class Transaction; }

namespace geocoin::state {
class ChainState;

// Temporary card state for an ordered selection of admitted transactions.
// Uses ChainState's registry rules without writing to confirmed state.
class CardProjection {
public:
    CardProjection(const ChainState& state, Height height);
    ~CardProjection();

    // Witnesses must already be verified at admission. Failed candidates
    // leave the projection unchanged.
    bool try_apply(const core::Transaction& tx);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace geocoin::state
