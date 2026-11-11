#include "geocoin/nft/geometry_codec.hpp"
#include "geocoin/geography/wgs84.hpp"
#include "geodesy_internal.hpp"
#include "geometry_codec_internal.hpp"
#include <GeographicLib/Geodesic.hpp>
#include <GeographicLib/GeodesicLine.hpp>
// Avoid Boost's 1e7 rescaling grid: at global extent it would discard metre
// precision. This is display-only clipping of already validated native rings.
#define BOOST_GEOMETRY_NO_ROBUSTNESS
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/geometries.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace geocoin::nft::coordinates {
namespace {
using detail::Json;
using detail::geometry_error;
namespace bg = boost::geometry;
using XY = bg::model::d2::point_xy<long double>;
using FlatPolygon = bg::model::polygon<XY, false, true>;
using FlatMulti = bg::model::multi_polygon<FlatPolygon>;
constexpr double scale = geography::kNanodegreesPerDegree;

Point read_geojson_position(const Json& raw) {
    if (!raw.is_array() || raw.size() != 2 || !raw[0].is_number() || !raw[1].is_number())
        geometry_error("position_format", "GeoJSON positions must contain longitude and latitude only");
    const double lon = raw[0].get<double>(), lat = raw[1].get<double>();
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180)
        geometry_error("coordinate_range", "GeoJSON position outside WGS84 range");
    return {static_cast<int64_t>(std::llround(lat * scale)), static_cast<int64_t>(std::llround(lon * scale))};
}

Ring read_geojson_ring(const Json& array, size_t part, size_t index, size_t& count, double tolerance) {
    Ring ring = detail::read_positions(array, part, index, read_geojson_position);
    if (ring.size() < kMinRingVertices + 1 || ring.front() != ring.back())
        geometry_error("unclosed_ring", "GeoJSON linear rings must be closed and contain at least four positions", part, index);
    if (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
    if (ring.size() >= kMinRingVertices) {
        Ring refined;
        auto physical = [](Point point) {
            if (std::abs(point.lat_ndeg) == 90'000'000'000LL) point.lon_ndeg = 0;
            if (point.lon_ndeg == 180'000'000'000LL) point.lon_ndeg = -180'000'000'000LL;
            return point;
        };
        auto append = [&](Point point) {
            if (refined.empty() || physical(refined.back()) != physical(point)) refined.push_back(point);
            if (refined.size() + count > kMaxGeometryVertices) geometry_error("import_resolution", "GeoJSON approximation exceeds 256 vertices; increase tolerance_m", part, index);
        };
        for (size_t edge = 0; edge < ring.size(); ++edge) {
            const Point a = ring[edge], b = ring[(edge + 1) % ring.size()];
            if (physical(a) == physical(b)) continue;
            if (std::abs(a.lon_ndeg - b.lon_ndeg) > 180'000'000'000LL &&
                std::abs(a.lat_ndeg) != 90'000'000'000LL && std::abs(b.lat_ndeg) != 90'000'000'000LL)
                geometry_error("uncut_antimeridian", "Cut GeoJSON edges at the antimeridian before importing", part, index, edge);
            auto split = [&](auto&& self, Point start, Point finish, unsigned depth) -> void {
                const auto line = GeographicLib::Geodesic::WGS84().InverseLine(start.lat_ndeg / scale, start.lon_ndeg / scale,
                    finish.lat_ndeg / scale, finish.lon_ndeg / scale);
                double deviation = 0;
                for (double fraction : {0.25, 0.5, 0.75}) {
                    const double lat = (start.lat_ndeg + (finish.lat_ndeg - start.lat_ndeg) * fraction) / scale;
                    const double lon = (start.lon_ndeg + (finish.lon_ndeg - start.lon_ndeg) * fraction) / scale;
                    double along, projected_lat, projected_lon, distance;
                    GeographicLib::Geodesic::WGS84().Inverse(start.lat_ndeg / scale, start.lon_ndeg / scale, lat, lon, along);
                    line.Position(std::min(along, line.Distance()), projected_lat, projected_lon);
                    GeographicLib::Geodesic::WGS84().Inverse(lat, lon, projected_lat, projected_lon, distance);
                    deviation = std::max(deviation, distance);
                }
                if (deviation <= tolerance) append(start);
                else {
                    if (depth >= 32) geometry_error("import_convergence", "GeoJSON approximation did not converge", part, index, edge);
                    const Point middle{(start.lat_ndeg + finish.lat_ndeg) / 2, (start.lon_ndeg + finish.lon_ndeg) / 2};
                    self(self, start, middle, depth + 1); self(self, middle, finish, depth + 1);
                }
            };
            split(split, a, b, 0);
        }
        if (refined.size() > 1 && physical(refined.front()) == physical(refined.back())) refined.pop_back();
        ring = std::move(refined);
    }
    detail::count_vertices(ring, part, index, count);
    return ring;
}

double unwrap(double longitude, double reference) {
    while (longitude - reference > 180) longitude -= 360;
    while (longitude - reference < -180) longitude += 360;
    return longitude;
}
struct Position { double lon, lat; };
std::vector<Position> densify(const Ring& ring, double tolerance, size_t& budget) {
    std::vector<Position> result;
    if (ring.empty()) return result;
    result.push_back({ring.front().lon_ndeg / scale, ring.front().lat_ndeg / scale});
    for (size_t i = 0; i < ring.size(); ++i) {
        const auto& a = ring[i]; const auto& b = ring[(i + 1) % ring.size()];
        const auto line = GeographicLib::Geodesic::WGS84().InverseLine(a.lat_ndeg / scale, a.lon_ndeg / scale,
            b.lat_ndeg / scale, b.lon_ndeg / scale);
        const Position first = result.back();
        const Position last{unwrap(b.lon_ndeg / scale, first.lon), b.lat_ndeg / scale};
        auto split = [&](auto&& self, double sa, Position pa, double sb, Position pb, unsigned depth) -> void {
            if (++budget > 65536) geometry_error("export_limit", "GeoJSON exceeds 65536 display positions; increase tolerance_m");
            double deviation = 0;
            Position middle{};
            for (double fraction : {0.25, 0.5, 0.75}) {
                double lat, lon, distance;
                line.Position(sa + (sb - sa) * fraction, lat, lon);
                const double linear_lon = pa.lon + (pb.lon - pa.lon) * fraction;
                lon = unwrap(lon, linear_lon);
                GeographicLib::Geodesic::WGS84().Inverse(lat, lon,
                    pa.lat + (pb.lat - pa.lat) * fraction, linear_lon, distance);
                deviation = std::max(deviation, distance);
                if (fraction == 0.5) middle = {lon, lat};
            }
            if (depth >= 32) geometry_error("export_convergence", "GeoJSON subdivision did not reach the requested tolerance");
            if (std::abs(middle.lat) > 89.9999999999 && std::abs(pb.lon - pa.lon) > 90) {
                result.push_back({pa.lon, middle.lat}); result.push_back({pb.lon, middle.lat}); result.push_back(pb);
            } else if (deviation <= tolerance && sb - sa < 1'000'000) result.push_back(pb);
            else {
                self(self, sa, pa, (sa + sb) / 2, middle, depth + 1);
                self(self, (sa + sb) / 2, middle, sb, pb, depth + 1);
            }
        };
        split(split, 0, first, line.Distance(), last, 0);
    }
    if (std::abs(result.back().lon - result.front().lon) > 180) {
        const double pole = locate_point(ring, {90'000'000'000LL, 0}) == PointLocation::inside ? 90 : -90;
        result.push_back({result.back().lon, pole});
        result.push_back({result.front().lon, pole});
    }
    if (result.back().lon != result.front().lon || result.back().lat != result.front().lat)
        result.push_back(result.front());
    return result;
}
FlatPolygon flat_ring(const std::vector<Position>& positions, double shift = 0) {
    FlatPolygon polygon;
    for (const auto& p : positions) polygon.outer().push_back(XY(p.lon + shift, p.lat));
    bg::correct(polygon);
    return polygon;
}

Geometry read_geojson(const Json& value, size_t depth, double tolerance) {
    if (depth > 8) geometry_error("collection_depth", "GeoJSON collection nesting exceeds eight levels");
    if (!value.is_object() || !value.contains("type") || !value["type"].is_string() || value.contains("crs"))
        geometry_error("geojson_format", "Expected RFC7946 GeoJSON in WGS84 longitude/latitude");
    const auto type = value["type"].get<std::string>();
    if (type == "Feature") {
        if (!value.contains("geometry")) geometry_error("geojson_format", "Feature has no geometry");
        return read_geojson(value["geometry"], depth + 1, tolerance);
    }
    if (type == "FeatureCollection" || type == "GeometryCollection") {
        const char* key = type == "FeatureCollection" ? "features" : "geometries";
        if (!value.contains(key) || !value[key].is_array() || value[key].size() > kMaxGeometryParts)
            geometry_error("geojson_format", "Invalid GeoJSON collection");
        Geometry result; result.polygons.clear();
        for (const auto& child : value[key]) {
            auto imported = read_geojson(child, depth + 1, tolerance);
            result.polygons.insert(result.polygons.end(),
                std::make_move_iterator(imported.polygons.begin()), std::make_move_iterator(imported.polygons.end()));
        }
        return detail::validated_geometry(std::move(result));
    }
    if ((type != "Polygon" && type != "MultiPolygon") || !value.contains("coordinates") || !value["coordinates"].is_array())
        geometry_error("geojson_type", "Only Polygon and MultiPolygon are supported for parcels");
    Json parts = type == "Polygon" ? Json::array({value["coordinates"]}) : value["coordinates"];
    if (parts.empty() || parts.size() > kMaxGeometryParts) geometry_error("part_count", "GeoJSON needs 1..16 polygon parts");
    Geometry result; result.polygons.clear();
    size_t count = 0;
    for (const auto& raw : parts) {
        if (!raw.is_array() || raw.empty() || raw.size() > kMaxGeometryRings) geometry_error("ring_count", "GeoJSON polygon needs exterior and optional holes");
        Polygon polygon;
        for (size_t r = 0; r < raw.size(); ++r) {
            auto ring = read_geojson_ring(raw[r], result.polygons.size(), r, count, tolerance);
            if (r == 0) polygon.exterior = std::move(ring); else polygon.holes.push_back(std::move(ring));
        }
        result.polygons.push_back(std::move(polygon));
    }
    return detail::validated_geometry(std::move(result));
}
} // namespace

Geometry geometry_from_geojson(const Json& value, double tolerance) {
    const detail::NearestRounding rounding;
    if (!std::isfinite(tolerance) || tolerance < 0.001 || tolerance > 1000)
        geometry_error("import_tolerance", "tolerance_m must be between 0.001 and 1000 metres");
    return read_geojson(value, 0, tolerance);
}
Json geometry_to_geojson(const Geometry& value, double tolerance) {
    const detail::NearestRounding rounding;
    if (!std::isfinite(tolerance) || tolerance < 0.001 || tolerance > 1000)
        geometry_error("export_tolerance", "tolerance_m must be between 0.001 and 1000 metres");
    const auto geometry = detail::validated_geometry(value);
    Json output = Json::array();
    size_t budget = 0;
    for (const auto& part : geometry.polygons) {
        const auto exterior = densify(part.exterior, tolerance, budget);
        FlatMulti remaining; remaining.push_back(flat_ring(exterior));
        bg::model::box<XY> extent; bg::envelope(remaining, extent);
        const double centre = (bg::get<bg::min_corner, 0>(extent) + bg::get<bg::max_corner, 0>(extent)) / 2;
        for (const auto& hole : part.holes) {
            const auto positions = densify(hole, tolerance, budget);
            auto hole_polygon = flat_ring(positions);
            bg::model::box<XY> hole_extent; bg::envelope(hole_polygon, hole_extent);
            const double hole_centre = (bg::get<bg::min_corner, 0>(hole_extent) + bg::get<bg::max_corner, 0>(hole_extent)) / 2;
            const double alignment = 360 * std::round((centre - hole_centre) / 360);
            for (double shift : {alignment - 360, alignment, alignment + 360}) {
                FlatMulti next;
                bg::difference(remaining, flat_ring(positions, shift), next);
                remaining = std::move(next);
            }
        }
        const int first = static_cast<int>(std::floor((bg::get<bg::min_corner, 0>(extent) + 180) / 360));
        const int last = static_cast<int>(std::floor((bg::get<bg::max_corner, 0>(extent) + 180) / 360));
        for (int slab = first; slab <= last; ++slab) {
            FlatMulti clipped;
            bg::intersection(remaining, bg::model::box<XY>(XY(-180 + 360 * slab, -90), XY(180 + 360 * slab, 90)), clipped);
            for (auto& polygon : clipped) {
                bg::correct(polygon);
                if (std::abs(bg::area(polygon)) < 1e-20) continue;
                Json rings = Json::array();
                auto append = [&](const auto& ring) {
                    Json points = Json::array();
                    for (const auto& p : ring) {
                        const double lon = std::clamp(static_cast<double>(bg::get<0>(p)) - 360 * slab, -180.0, 180.0);
                        const double lat = std::clamp(static_cast<double>(bg::get<1>(p)), -90.0, 90.0);
                        points.push_back({std::round(lon * scale) / scale, std::round(lat * scale) / scale});
                    }
                    rings.push_back(std::move(points));
                };
                append(polygon.outer()); for (const auto& hole : polygon.inners()) append(hole);
                output.push_back(std::move(rings));
            }
        }
    }
    if (output.empty()) geometry_error("empty_export", "GeoJSON clipping produced no area");
    return output.size() == 1 ? Json{{"type", "Polygon"}, {"coordinates", output.front()}} :
        Json{{"type", "MultiPolygon"}, {"coordinates", output}};
}
} // namespace geocoin::nft::coordinates
