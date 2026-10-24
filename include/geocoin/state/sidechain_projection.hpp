#pragma once

#include "geocoin/types.hpp"

#include <memory>

namespace geocoin::core { class Transaction; }

namespace geocoin::state {
class ChainState;

// Temporary sidechain state for an ordered selection of admitted transactions.
// It uses the same transition rules as ChainState without writing to storage.
class SidechainProjection {
public:
    SidechainProjection(const ChainState& state, Height height);
    ~SidechainProjection();

    // Failed candidates leave the projection unchanged.
    bool try_apply(const core::Transaction& tx);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace geocoin::state
