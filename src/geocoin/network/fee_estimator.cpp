#include "geocoin/network/fee_estimator.hpp"
#include "geocoin/network/peer_file.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/u64_math.hpp"
#include "nlohmann_json.hpp"
#include <algorithm>
#include <fstream>
#include <set>

namespace geocoin::network {
namespace {
constexpr size_t kMaxSamples = 4096;
constexpr Height kHistoryBlocks = 2016;
uint32_t wait_blocks(Height current, Height added) {
    return static_cast<uint32_t>(std::min<Height>(current > added ? current - added : 1, 1000000));
}
}

void FeeEstimator::configure_storage(std::string path, std::string genesis, Height height, const Hash256& tip) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = std::move(path); genesis_ = std::move(genesis); height_ = height; tip_ = tip;
    pending_.clear(); samples_.clear();
    try {
        if (!std::filesystem::is_regular_file(path_) || std::filesystem::file_size(path_) > 2 * 1024 * 1024) return;
        std::ifstream input(path_, std::ios::binary);
        const auto data = nlohmann::json::parse(input);
        if (data.at("version") != 1 || data.at("genesis") != genesis_ || data.at("height") != height ||
            data.at("tip") != utils::hash256_to_hex(tip) || !data.at("samples").is_array() ||
            data.at("samples").size() > kMaxSamples) return;
        std::vector<Sample> loaded;
        for (const auto& item : data.at("samples")) {
            Sample sample;
            sample.rate = item.at("rate").get<uint64_t>(); sample.height = item.at("height").get<Height>();
            sample.waited = item.at("waited").get<uint32_t>(); sample.confirmed = item.at("confirmed").get<bool>();
            if (!utils::hex_to_hash256_checked(item.at("block").get<std::string>(), sample.block) ||
                sample.rate == 0 || sample.height > height || sample.waited == 0 || sample.waited > 1000000) return;
            loaded.push_back(sample);
        }
        samples_.swap(loaded);
    } catch (...) { samples_.clear(); }
}

void FeeEstimator::transaction_added(const Hash256& txid, uint64_t fee, size_t size, Height height) {
    if (fee == 0 || size == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto rate = utils::mul_div_u64_ceil(fee, 1000, size);
    if (rate != 0) pending_.try_emplace(txid, Pending{rate, height});
}

void FeeEstimator::transaction_removed(const Hash256& txid) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = pending_.find(txid);
    if (found == pending_.end()) return;
    samples_.push_back({found->second.rate, height_, wait_blocks(height_, found->second.height), false, {}});
    pending_.erase(found);
    if (samples_.size() > kMaxSamples) samples_.erase(samples_.begin(), samples_.end() - kMaxSamples);
}

void FeeEstimator::block_connected(const Hash256& block, Height height, const std::vector<Hash256>& transactions) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (height < height_ || (height == height_ && block == tip_)) return;
    height_ = height; tip_ = block;
    for (const auto& txid : transactions) {
        const auto found = pending_.find(txid);
        if (found == pending_.end()) continue;
        samples_.push_back({found->second.rate, height, wait_blocks(height, found->second.height), true, block});
        pending_.erase(found);
    }
    std::erase_if(samples_, [&](const auto& sample) { return height > sample.height && height - sample.height > kHistoryBlocks; });
    if (samples_.size() > kMaxSamples) samples_.erase(samples_.begin(), samples_.end() - kMaxSamples);
    save_locked();
}

void FeeEstimator::reorganize(Height fork_height) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(samples_, [&](const auto& sample) { return sample.height > fork_height; });
    pending_.clear(); height_ = fork_height; tip_ = {};
    save_locked();
}

void FeeEstimator::clear_pending() { std::lock_guard<std::mutex> lock(mutex_); pending_.clear(); }

FeeEstimate FeeEstimator::estimate(uint32_t target, bool conservative) const {
    std::lock_guard<std::mutex> lock(mutex_);
    FeeEstimate result{{}, target, 0};
    if (target == 0 || target > 1008) return result;
    struct Totals { size_t success = 0, total = 0; };
    std::map<uint64_t, Totals, std::greater<uint64_t>> buckets;
    std::set<Hash256> confirmed_blocks;
    for (const auto& sample : samples_) {
        if (!sample.confirmed && sample.waited < target) continue;
        auto& bucket = buckets[sample.rate]; ++bucket.total;
        if (sample.confirmed && sample.waited <= target) { ++bucket.success; confirmed_blocks.insert(sample.block); }
    }
    for (const auto& [txid, pending] : pending_) {
        (void)txid;
        if (height_ >= pending.height && height_ - pending.height >= target) ++buckets[pending.rate].total;
    }
    const size_t minimum = conservative ? 20 : 10;
    const size_t threshold = conservative ? 95 : 85;
    size_t success = 0, total = 0;
    if (confirmed_blocks.size() < 2) return result;
    for (const auto& [rate, bucket] : buckets) {
        success += bucket.success; total += bucket.total;
        if (total >= minimum && success * 100 >= threshold * total) result.rate_per_kb = rate;
    }
    result.observations = total;
    return result;
}

void FeeEstimator::save_locked() const {
    if (path_.empty()) return;
    try {
    nlohmann::json samples = nlohmann::json::array();
    for (const auto& sample : samples_) samples.push_back({{"rate", sample.rate}, {"height", sample.height},
        {"waited", sample.waited}, {"confirmed", sample.confirmed}, {"block", utils::hash256_to_hex(sample.block)}});
    const nlohmann::json document{{"version", 1}, {"genesis", genesis_}, {"height", height_},
        {"tip", utils::hash256_to_hex(tip_)}, {"samples", std::move(samples)}};
    // This is a replaceable policy cache, never consensus state or wallet data.
    (void)detail::write_peer_file(path_, [&](auto& output) { output << document.dump(); });
    } catch (...) { /* A replaceable policy cache must never fail a chain mutation. */ }
}
} // namespace geocoin::network
