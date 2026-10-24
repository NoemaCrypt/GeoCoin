#pragma once

#include "geocoin/nft/geometry.hpp"
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace geocoin::nft::coordinates {

inline constexpr char kGeodesyLibrary[] = "GeographicLib";
inline constexpr char kGeodesyVersion[] = "2.3.0";
inline constexpr uint32_t kGeodesyRulesVersion = 1;

struct GeometryIssue {
    std::string code;
    std::string message;
    size_t part = 0, ring = 0;
    std::optional<size_t> vertex, edge, other_edge;
};
struct GeometryValidation {
    std::optional<GeometryIssue> issue;
    explicit operator bool() const noexcept { return !issue; }
};

Geometry canonicalize_geometry(Geometry geometry);
GeometryValidation validate_geometry(const Geometry& geometry);

// Edges are unique shortest WGS84 geodesics. A ring describes the smaller
// region, regardless of winding; repeated closing vertices are not serialized.
struct PolygonMetrics {
    double signed_area_sqm;
    double perimeter_m;
    double area_sqm() const noexcept;
    // Quantize an existing measurement without traversing the geometry again.
    // Non-finite or overflowing areas have no integer representation.
    std::optional<uint64_t> area_sqcm() const;
};
std::optional<PolygonMetrics> measure_geometry(const Geometry& geometry);
std::optional<uint64_t> area_square_centimetres(const Geometry& geometry);

// Measurement alone does not assert that a polygon is simple.
std::optional<PolygonMetrics> measure_polygon(std::span<const Point> polygon);
bool is_simple_polygon(std::span<const Point> polygon);

enum class PointLocation { outside, boundary, inside };
PointLocation locate_point(std::span<const Point> polygon, const Point& point);

// Invalid coordinates return nullopt, never a made-up zero distance.
std::optional<double> geodesic_distance_m(const Point& a, const Point& b);

// Direct WGS84 geodesic. Azimuth is clockwise from north; output is rounded
// to signed nanodegrees and longitude is normalized into [-180,180].
std::optional<Point> geodesic_destination(
    const Point& origin, double azimuth_degrees, double distance_m);

// Interior overlap, including identical parcels. A shared edge/vertex alone
// does not consume a neighbouring cadastral parcel's slot.
bool polygon_interiors_overlap(std::span<const Point> a, std::span<const Point> b);

class PreparedGeometry {
public:
    explicit PreparedGeometry(const Geometry& geometry);
    ~PreparedGeometry();
    PointLocation locate(const Point& point) const;
    // If stop_above_m is finite, values above it may be conservative lower
    // bounds. Distances at/below the cutoff are always minimized to the edge.
    std::optional<double> distance_m(const Point& point,
        double stop_above_m = std::numeric_limits<double>::infinity()) const;
    const Geometry& geometry() const;
    const PolygonMetrics& metrics() const;
    size_t memory_bytes() const;
    bool overlaps(const PreparedGeometry& other) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool geometry_interiors_overlap(const Geometry& a, const Geometry& b);

// Conservative boxes; antimeridian intervals are split into separate boxes.
struct GeographicBox {
    int64_t south, west, north, east;
    bool intersects(const GeographicBox& other) const;
};
std::vector<GeographicBox> geometry_boxes(const Geometry& geometry);
std::vector<GeographicBox> radius_boxes(const Point& point, double radius_m);

} // namespace geocoin::nft::coordinates
