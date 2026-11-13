#pragma once
#include "geocoin/types.hpp"
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace geocoin::network {
struct FeeEstimate {
    std::optional<uint64_t> rate_per_kb;
    uint32_t target = 0;
    size_t observations = 0;
};

// Local policy only. Samples come from transactions observed before inclusion,
// including transactions that are still waiting or were removed unconfirmed.
class FeeEstimator {
public:
    void configure_storage(std::string path, std::string genesis, Height height, const Hash256& tip);
    void transaction_added(const Hash256& txid, uint64_t fee, size_t size, Height height);
    void transaction_removed(const Hash256& txid);
    void block_connected(const Hash256& block, Height height, const std::vector<Hash256>& transactions);
    void reorganize(Height fork_height);
    void clear_pending();
    FeeEstimate estimate(uint32_t target, bool conservative) const;
private:
    struct Pending { uint64_t rate = 0; Height height = 0; };
    struct Sample { uint64_t rate = 0; Height height = 0; uint32_t waited = 0; bool confirmed = false; Hash256 block{}; };
    mutable std::mutex mutex_;
    std::map<Hash256, Pending> pending_;
    std::vector<Sample> samples_;
    Height height_ = 0;
    Hash256 tip_{};
    std::string path_, genesis_;
    void save_locked() const;
};
} // namespace geocoin::network
