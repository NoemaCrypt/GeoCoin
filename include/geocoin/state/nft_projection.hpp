#pragma once

#include <memory>

namespace geocoin::core { class Transaction; }

namespace geocoin::state {
class ChainState;

// Temporary registry for admitted NFT transitions and linked cadastral documents.
class NFTProjection {
public:
    explicit NFTProjection(const ChainState& state);
    ~NFTProjection();

    // Payloads and witnesses must already be validated. A failed candidate
    // leaves ownership, cadastral reservations and NFT identifiers unchanged.
    bool try_apply(const core::Transaction& tx);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace geocoin::state
