#include "geocoin/nft/geometry_codec.hpp"
#include "geodesy_internal.hpp"
#include "geometry_codec_internal.hpp"
#include <algorithm>
#include <limits>

namespace geocoin::nft::coordinates {
namespace {
using detail::Json;
using detail::geometry_error;
void object_fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object()) geometry_error("geometry_format", "Geometry value must be an object");
    for (auto it = value.begin(); it != value.end(); ++it)
        if (std::find(names.begin(), names.end(), it.key()) == names.end())
            geometry_error("unknown_geometry_field", "Unknown native geometry field");
}
int64_t integer(const Json& value) {
    if (value.is_number_unsigned()) {
        const auto n = value.get<uint64_t>();
        if (n > uint64_t(std::numeric_limits<int64_t>::max())) geometry_error("coordinate_range", "Coordinate exceeds int64");
        return static_cast<int64_t>(n);
    }
    if (!value.is_number_integer()) geometry_error("coordinate_type", "Native coordinates must be integer nanodegrees");
    return value.get<int64_t>();
}
Point read_native_position(const Json& raw) {
    object_fields(raw, {"lat_ndeg", "lon_ndeg"});
    if (!raw.contains("lat_ndeg") || !raw.contains("lon_ndeg"))
        geometry_error("position_format", "Both lat_ndeg and lon_ndeg are required");
    return {integer(raw["lat_ndeg"]), integer(raw["lon_ndeg"])};
}

Ring read_native_ring(const Json& array, size_t part, size_t index, size_t& count) {
    auto ring = detail::read_positions(array, part, index, read_native_position);
    if (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
    detail::count_vertices(ring, part, index, count);
    return ring;
}
} // namespace

Json geometry_issue_to_json(const GeometryIssue& issue) {
    Json out{{"code", issue.code}, {"message", issue.message}, {"part", issue.part}, {"ring", issue.ring}};
    if (issue.vertex) out["vertex"] = *issue.vertex;
    if (issue.edge) out["edge"] = *issue.edge;
    if (issue.other_edge) out["other_edge"] = *issue.other_edge;
    return out;
}
Json geometry_to_json(const Geometry& value) {
    const auto geometry = canonicalize_geometry(value);
    Json parts = Json::array();
    auto ring_json = [](const Ring& ring) {
        Json result = Json::array();
        for (const auto& p : ring) result.push_back({{"lat_ndeg", p.lat_ndeg}, {"lon_ndeg", p.lon_ndeg}});
        return result;
    };
    for (const auto& part : geometry.polygons) {
        Json holes = Json::array();
        for (const auto& hole : part.holes) holes.push_back(ring_json(hole));
        parts.push_back({{"exterior", ring_json(part.exterior)}, {"holes", holes}});
    }
    return {{"parts", parts}};
}
Geometry geometry_from_json(const Json& value) {
    const detail::NearestRounding rounding;
    object_fields(value, {"parts"});
    if (!value.contains("parts") || !value["parts"].is_array() || value["parts"].empty() || value["parts"].size() > kMaxGeometryParts)
        geometry_error("part_count", "Native geometry needs 1..16 parts");
    Geometry result; result.polygons.clear();
    size_t count = 0;
    for (const auto& raw : value["parts"]) {
        const size_t part = result.polygons.size();
        object_fields(raw, {"exterior", "holes"});
        if (!raw.contains("exterior")) geometry_error("missing_exterior", "Exterior ring is required", part);
        Polygon polygon;
        polygon.exterior = read_native_ring(raw["exterior"], part, 0, count);
        if (raw.contains("holes")) {
            if (!raw["holes"].is_array() || raw["holes"].size() > kMaxGeometryRings - 1) geometry_error("ring_count", "Invalid hole list", part);
            for (const auto& hole : raw["holes"])
                polygon.holes.push_back(read_native_ring(hole, part, polygon.holes.size() + 1, count));
        }
        result.polygons.push_back(std::move(polygon));
    }
    return detail::validated_geometry(std::move(result));
}
} // namespace geocoin::nft::coordinates
