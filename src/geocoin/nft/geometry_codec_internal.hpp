#pragma once

#include "geocoin/nft/geometry_codec.hpp"

namespace geocoin::nft::coordinates::detail {

using Json = nlohmann::json;

[[noreturn]] inline void geometry_error(const char* code, const char* message,
    size_t part = 0, size_t ring = 0, std::optional<size_t> vertex = {}) {
    throw GeometryError({code, message, part, ring, vertex, {}, {}});
}

inline Geometry validated_geometry(Geometry geometry) {
    if (const auto result = validate_geometry(geometry); !result) throw GeometryError(*result.issue);
    return canonicalize_geometry(std::move(geometry));
}

// Both input formats report the caller's part/ring/vertex before any
// canonical reordering. Coordinate decoding remains specific to the format.
template<class DecodePoint>
Ring read_positions(const Json& array, size_t part, size_t ring, DecodePoint decode) {
    if (!array.is_array() || array.size() > kMaxGeometryVertices + 1)
        geometry_error("vertex_limit", "Ring exceeds 256 unique vertices", part, ring);
    Ring result;
    result.reserve(array.size());
    for (size_t vertex = 0; vertex < array.size(); ++vertex) {
        try {
            result.push_back(decode(array[vertex]));
        } catch (GeometryError& failure) {
            failure.issue.part = part;
            failure.issue.ring = ring;
            failure.issue.vertex = vertex;
            throw;
        }
    }
    return result;
}

inline void count_vertices(const Ring& ring, size_t part, size_t index, size_t& count) {
    count += ring.size();
    if (count > kMaxGeometryVertices)
        geometry_error("vertex_limit", "Geometry exceeds 256 unique vertices", part, index);
}

} // namespace geocoin::nft::coordinates::detail
