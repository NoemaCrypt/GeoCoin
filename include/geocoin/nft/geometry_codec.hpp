#pragma once
#include "geocoin/nft/geodesy.hpp"
#include "nlohmann_json.hpp"
#include <stdexcept>

namespace geocoin::nft::coordinates {
class GeometryError : public std::invalid_argument {
public:
    GeometryIssue issue;
    explicit GeometryError(GeometryIssue value) : std::invalid_argument(value.message), issue(std::move(value)) {}
};
nlohmann::json geometry_to_json(const Geometry& geometry);
Geometry geometry_from_json(const nlohmann::json& value);
Geometry geometry_from_geojson(const nlohmann::json& value, double tolerance_m = 1.0);
// Derived RFC7946 display geometry. Geodesic arcs are subdivided to the
// requested tolerance, then cut at +/-180; signed native data stays separate.
nlohmann::json geometry_to_geojson(const Geometry& geometry, double tolerance_m = 1.0);
nlohmann::json geometry_issue_to_json(const GeometryIssue& issue);
} // namespace geocoin::nft::coordinates
