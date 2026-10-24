#pragma once
#include "geocoin/types.hpp"
#include "geocoin/geography/wgs84.hpp"
#include "geocoin/consensus/geo_grid.hpp"
#include "geocoin/consensus/historical_geo_tag.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace geocoin::consensus::geo {
// Prelaunch GeoPoW v1: synthetic points on a global WGS84 grid. Selection
// probabilities are proportional to ellipsoidal cell areas. No GPS or solar
// observation is claimed. All selection arithmetic is integer-only.
inline constexpr unsigned kAlgorithmVersion = 1;
inline constexpr int64_t GENESIS_LAT_NDEG = 48'851'583'000LL;
inline constexpr int64_t GENESIS_LON_NDEG = 8'897'278'000LL;
inline constexpr int64_t kCell = 100'000'000;
inline constexpr uint32_t GENESIS_LAT_IDX = (GENESIS_LAT_NDEG + 90'000'000'000LL) / kCell;
inline constexpr uint32_t GENESIS_LON_IDX = (GENESIS_LON_NDEG + 180'000'000'000LL) / kCell;
inline constexpr uint64_t GENESIS_GEO_TAG = (uint64_t(GENESIS_LAT_IDX) << 32) | GENESIS_LON_IDX;
struct GeoTagResult {
    int64_t lat_ndeg = 0, lon_ndeg = 0;
    uint64_t geo_tag = 0;
    constexpr geography::Wgs84Point point() const { return {lat_ndeg, lon_ndeg}; }
};
struct GeoCell {
    geography::Wgs84Point southwest, northeast;
    constexpr geography::Wgs84Point center() const {
        return {(southwest.latitude_ndeg + northeast.latitude_ndeg) / 2,
                (southwest.longitude_ndeg + northeast.longitude_ndeg) / 2};
    }
    constexpr std::array<double, 4> geojson_bbox() const {
        return {southwest.longitude_degrees(), southwest.latitude_degrees(),
                northeast.longitude_degrees(), northeast.latitude_degrees()};
    }
    constexpr bool contains(geography::Wgs84Point p) const {
        return p.is_valid() && p.latitude_ndeg >= southwest.latitude_ndeg &&
            p.latitude_ndeg < northeast.latitude_ndeg && p.longitude_ndeg >= southwest.longitude_ndeg &&
            p.longitude_ndeg < northeast.longitude_ndeg;
    }
};
inline constexpr std::optional<GeoCell> decode_geo_tag(uint64_t tag) {
    const uint32_t row = static_cast<uint32_t>(tag >> 32), column = static_cast<uint32_t>(tag);
    if (row >= 1800 || column >= 3600) return {};
    const int64_t south = int64_t(row) * kCell - 90'000'000'000LL;
    const int64_t west = int64_t(column) * kCell - 180'000'000'000LL;
    return GeoCell{{south, west}, {south + kCell, west + kCell}};
}
inline constexpr std::optional<GeoTagResult> point_from_geo_tag(uint64_t tag) {
    const auto cell = decode_geo_tag(tag);
    if (!cell) return {};
    const auto centre = cell->center();
    return GeoTagResult{centre.latitude_ndeg, centre.longitude_ndeg, tag};
}
inline GeoTagResult compute_geo_tag(uint32_t parent_mtp, const Hash256& parent_hash) {
    if (parent_mtp == 0 && parent_hash == Hash256{})
        return {GENESIS_LAT_NDEG, GENESIS_LON_NDEG, GENESIS_GEO_TAG};
    uint64_t seed = uint64_t(parent_mtp) * 0x9e3779b97f4a7c15ULL;
    for (size_t i = 0; i < parent_hash.size(); ++i)
        seed = (seed ^ parent_hash[i]) * 0x100000001b3ULL;
    auto next = [&] {
        seed += 0x9e3779b97f4a7c15ULL;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    };
    const uint64_t ticket = next() >> 32;
    const auto it = std::upper_bound(kLatitudeCdf.begin(), kLatitudeCdf.end(), ticket);
    const uint32_t row = static_cast<uint32_t>(it - kLatitudeCdf.begin() - 1);
    constexpr uint64_t bound = 3600, threshold = uint64_t(-bound) % bound;
    uint64_t longitude;
    do { longitude = next(); } while (longitude < threshold);
    const uint64_t tag = (uint64_t(row) << 32) | (longitude % bound);
    return *point_from_geo_tag(tag);
}
// Height and the network activation schedule are mandatory: accepting either
// algorithm opportunistically would allow two different valid histories.
inline GeoTagResult compute_block_geo_tag(uint32_t parent_mtp, const Hash256& parent_hash,
                                          Height height, Height activation_height) {
    if (height >= activation_height) return compute_geo_tag(parent_mtp, parent_hash);
    const auto tag = historical::compute_geo_tag(parent_mtp, parent_hash);
    return {int64_t(tag.lat_u) * 1000, int64_t(tag.lon_u) * 1000, tag.geo_tag};
}

inline constexpr const char* selection_rule(Height height, Height activation_height) {
    return height == 0 ? "genesis" : height < activation_height ? "historical_solar" : "wgs84_area";
}
} // namespace geocoin::consensus::geo
