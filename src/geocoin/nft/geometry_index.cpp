#include "geometry_index.hpp"
#include <algorithm>
#include <optional>

namespace geocoin::nft {
namespace {
std::optional<std::set<uint32_t>> tiles(const std::vector<coordinates::GeographicBox>& boxes) {
    std::set<uint32_t> result;
    for (const auto& box : boxes) {
        auto latitude = [](int64_t value) { return std::clamp<int64_t>((value + 90'000'000'000LL) / 1'000'000'000, 0, 179); };
        auto longitude = [](int64_t value) { return std::clamp<int64_t>((value + 180'000'000'000LL) / 1'000'000'000, 0, 359); };
        const auto south = latitude(box.south), north = latitude(box.north);
        const auto west = longitude(box.west), east = longitude(box.east);
        if ((north - south + 1) * (east - west + 1) > 4096) return {};
        for (auto lat = south; lat <= north; ++lat) for (auto lon = west; lon <= east; ++lon)
            result.insert(static_cast<uint32_t>(lat * 360 + lon));
        if (result.size() > 4096) return {};
    }
    return result;
}
}
void GeometryIndex::insert(const Hash256& id, const coordinates::Geometry& geometry) {
    erase(id);
    const auto selected = tiles(coordinates::geometry_boxes(geometry));
    auto& memberships = memberships_[id];
    if (!selected) { global_.insert(id); return; }
    for (const auto cell : *selected) { cells_[cell].insert(id); memberships.push_back(cell); }
}
void GeometryIndex::erase(const Hash256& id) {
    if (const auto it = memberships_.find(id); it != memberships_.end()) {
        for (const auto cell : it->second) {
            auto entry = cells_.find(cell);
            if (entry != cells_.end()) {
                entry->second.erase(id);
                if (entry->second.empty()) cells_.erase(entry);
            }
        }
        memberships_.erase(it);
    }
    global_.erase(id);
    std::lock_guard lock(cache_mutex_);
    if (const auto it = cache_.find(id); it != cache_.end()) {
        cache_bytes_ -= it->second.bytes;
        recent_.erase(it->second.position);
        cache_.erase(it);
    }
}
std::set<Hash256> GeometryIndex::candidates(const std::vector<coordinates::GeographicBox>& boxes) const {
    auto result = global_;
    const auto selected = tiles(boxes);
    if (!selected) {
        for (const auto& [id, membership] : memberships_) result.insert(id);
    } else {
        for (const auto cell : *selected)
            if (const auto it = cells_.find(cell); it != cells_.end()) result.insert(it->second.begin(), it->second.end());
    }
    return result;
}
std::shared_ptr<const coordinates::PreparedGeometry> GeometryIndex::prepare(
    const Hash256& id, const coordinates::Geometry& geometry) const {
    std::lock_guard lock(cache_mutex_);
    if (const auto it = cache_.find(id); it != cache_.end()) {
        recent_.splice(recent_.begin(), recent_, it->second.position);
        return it->second.value;
    }
    auto value = std::make_shared<const coordinates::PreparedGeometry>(geometry);
    const size_t bytes = value->memory_bytes();
    constexpr size_t budget = 32 * 1024 * 1024;
    if (bytes > budget) return value;
    while (!recent_.empty() && cache_bytes_ > budget - bytes) {
        const auto it = cache_.find(recent_.back());
        cache_bytes_ -= it->second.bytes;
        cache_.erase(it);
        recent_.pop_back();
    }
    recent_.push_front(id);
    cache_.emplace(id, Cached{value, bytes, recent_.begin()});
    cache_bytes_ += bytes;
    return value;
}
} // namespace geocoin::nft
