#pragma once
#include "geocoin/nft/geodesy.hpp"
#include "geocoin/types.hpp"
#include <list>
#include <map>
#include <mutex>
#include <set>

namespace geocoin::nft {
// Derived RAM index. Consensus metadata remains the sole persisted geometry.
class GeometryIndex {
public:
    void insert(const Hash256& id, const coordinates::Geometry& geometry);
    void erase(const Hash256& id);
    std::set<Hash256> candidates(const std::vector<coordinates::GeographicBox>& boxes) const;
    std::shared_ptr<const coordinates::PreparedGeometry> prepare(const Hash256& id,
        const coordinates::Geometry& geometry) const;
private:
    std::map<uint32_t, std::set<Hash256>> cells_;
    std::map<Hash256, std::vector<uint32_t>> memberships_;
    std::set<Hash256> global_;
    struct Cached {
        std::shared_ptr<const coordinates::PreparedGeometry> value;
        size_t bytes;
        std::list<Hash256>::iterator position;
    };
    mutable std::mutex cache_mutex_;
    mutable size_t cache_bytes_ = 0;
    mutable std::list<Hash256> recent_;
    mutable std::map<Hash256, Cached> cache_;
};
} // namespace geocoin::nft
