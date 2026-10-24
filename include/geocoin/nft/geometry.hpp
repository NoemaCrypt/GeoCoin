#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace geocoin::nft::coordinates {

// Native geometry v1 limits, shared by wire decoding, validation and import.
inline constexpr size_t kMinRingVertices = 3;
inline constexpr size_t kMaxGeometryParts = 16;
inline constexpr size_t kMaxGeometryRings = 64;
inline constexpr size_t kMaxGeometryVertices = 256;
inline constexpr uint64_t kSquareCentimetresPerSquareMetre = 10'000;

struct Point {
    // WGS 84 / EPSG:4326, latitude then longitude, signed nanodegrees.
    // RFC 7946 GeoJSON uses the opposite coordinate order.
    int64_t lat_ndeg;
    int64_t lon_ndeg;
    auto operator<=>(const Point&) const = default;
};

using Ring = std::vector<Point>;

struct Polygon {
    Ring exterior;
    std::vector<Ring> holes;
    auto operator<=>(const Polygon&) const = default;
};

struct Geometry {
    std::vector<Polygon> polygons{Polygon{}};
    auto operator<=>(const Geometry&) const = default;
};

} // namespace geocoin::nft::coordinates
