#include "geocoin/nft/geodesy.hpp"
#include "geocoin/geography/wgs84.hpp"
#include "geodesy_internal.hpp"
#include <GeographicLib/Geodesic.hpp>
#include <GeographicLib/GeodesicLine.hpp>
#include <GeographicLib/Intersect.hpp>
#include <GeographicLib/PolygonArea.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

static_assert(GEOGRAPHICLIB_VERSION_MAJOR == 2 &&
              GEOGRAPHICLIB_VERSION_MINOR == 3 && GEOGRAPHICLIB_VERSION_PATCH == 0);
static_assert(GEOGRAPHICLIB_PRECISION == 2 && std::numeric_limits<double>::is_iec559);

namespace geocoin::nft::coordinates {
namespace {
using detail::NearestRounding;
using Line = GeographicLib::GeodesicLine;
using GeographicLib::Geodesic;
using GeographicLib::Intersect;
using GeographicLib::Math;

// Numerical tolerance for the pinned solver, in metres; substantially below
// the ~0.11 mm coordinate step at the equator. This is not a survey tolerance.
constexpr double kSolverToleranceM = 0.00001;
constexpr double kScale = geography::kNanodegreesPerDegree;

bool valid(const Point& p) {
    return geography::Wgs84Point{p.lat_ndeg, p.lon_ndeg}.is_valid();
}

std::pair<int64_t, int64_t> position_key(const Point& p) {
    return {p.lat_ndeg, std::abs(p.lat_ndeg) == geography::kMaxLatitude ? 0 :
        (p.lon_ndeg == geography::kMaxLongitude ? geography::kMinLongitude : p.lon_ndeg)};
}

Line edge(const Point& a, const Point& b) {
    return Geodesic::WGS84().InverseLine(a.lat_ndeg / kScale, a.lon_ndeg / kScale,
        b.lat_ndeg / kScale, b.lon_ndeg / kScale, Intersect::LineCaps);
}

// Intersect maintains mutable instrumentation counters. Never share an
// instance across RPC/mining/validation threads, even through a const reference.
Intersect& intersection_solver() {
    thread_local Intersect solver(Geodesic::WGS84());
    return solver;
}

struct Hit {
    double x;
    double y;
    int coincidence;
    bool intersects;
    bool valid;
};

Hit intersect(const Line& a, const Line& b) {
    int mode = 0, coincidence = 0;
    const auto p = intersection_solver().Segment(a, b, mode, &coincidence);
    const bool on_segments = std::isfinite(p.first) && std::isfinite(p.second) &&
        p.first >= -kSolverToleranceM && p.first <= a.Distance() + kSolverToleranceM &&
        p.second >= -kSolverToleranceM && p.second <= b.Distance() + kSolverToleranceM;
    return {p.first, p.second, coincidence, on_segments,
        std::isfinite(p.first) && std::isfinite(p.second)};
}

double coincident_overlap(const Line& a, const Line& b, const Hit& hit) {
    const double b_start = hit.x - hit.coincidence * hit.y;
    const double b_end = b_start + hit.coincidence * b.Distance();
    return std::min(a.Distance(), std::max(b_start, b_end)) -
        std::max(0.0, std::min(b_start, b_end));
}

std::vector<Line> edges(std::span<const Point> polygon) {
    std::vector<Line> result;
    result.reserve(polygon.size());
    for (size_t i = 0; i < polygon.size(); ++i)
        result.push_back(edge(polygon[i], polygon[(i + 1) % polygon.size()]));
    return result;
}

// Work in degrees without rounding derived edge positions back onto the
// nanodegree grid: that would move a point off its own geodesic boundary.
PointLocation locate(std::span<const Point> polygon, double lat, double lon,
                     double signed_area, const std::vector<Line>* prepared = nullptr) {
    if (polygon.size() < kMinRingVertices || signed_area == 0) return PointLocation::outside;
    double winding = 0;
    double previous_azimuth = 0, first_azimuth = 0;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto& p = polygon[i];
        if (!valid(p)) return PointLocation::outside;
        double distance, azimuth, end_azimuth;
        Geodesic::WGS84().Inverse(lat, lon, p.lat_ndeg / kScale, p.lon_ndeg / kScale,
            distance, azimuth, end_azimuth);
        if (distance <= kSolverToleranceM) return PointLocation::boundary;
        if (i == 0) first_azimuth = azimuth;
        else winding += Math::AngDiff(previous_azimuth, azimuth);
        previous_azimuth = azimuth;

        const auto line = prepared ? (*prepared)[i] : edge(p, polygon[(i + 1) % polygon.size()]);
        double along, azi, ignored;
        Geodesic::WGS84().Inverse(p.lat_ndeg / kScale, p.lon_ndeg / kScale,
            lat, lon, along, azi, ignored);
        if (along <= line.Distance() + kSolverToleranceM) {
            double edge_lat, edge_lon, separation;
            line.Position(std::min(along, line.Distance()), edge_lat, edge_lon);
            Geodesic::WGS84().Inverse(lat, lon, edge_lat, edge_lon, separation);
            if (separation <= kSolverToleranceM) return PointLocation::boundary;
        }
    }
    winding += Math::AngDiff(previous_azimuth, first_azimuth);
    // Azimuth increases clockwise. The sign also distinguishes the smaller
    // polygon from its antipodal winding, and is independent of ring order.
    return std::abs(winding) > 180 && winding * signed_area < 0 ?
        PointLocation::inside : PointLocation::outside;
}
} // namespace

double PolygonMetrics::area_sqm() const noexcept { return std::abs(signed_area_sqm); }

std::optional<uint64_t> PolygonMetrics::area_sqcm() const {
    const NearestRounding rounding;
    const double value = std::round(area_sqm() * kSquareCentimetresPerSquareMetre);
    if (!(value >= 0 && value < static_cast<double>(std::numeric_limits<uint64_t>::max()))) return {};
    return static_cast<uint64_t>(value);
}

std::optional<double> geodesic_distance_m(const Point& a, const Point& b) {
    if (!valid(a) || !valid(b)) return std::nullopt;
    const NearestRounding rounding;
    double distance;
    Geodesic::WGS84().Inverse(a.lat_ndeg / kScale, a.lon_ndeg / kScale,
        b.lat_ndeg / kScale, b.lon_ndeg / kScale, distance);
    if (!std::isfinite(distance)) return std::nullopt;
    return distance;
}

std::optional<Point> geodesic_destination(const Point& origin, double azimuth, double distance) {
    if (!valid(origin) || !std::isfinite(azimuth) || !std::isfinite(distance) || distance < 0)
        return std::nullopt;
    const NearestRounding rounding;
    double lat, lon;
    Geodesic::WGS84().Direct(origin.lat_ndeg / kScale, origin.lon_ndeg / kScale,
        azimuth, distance, lat, lon);
    if (!std::isfinite(lat) || !std::isfinite(lon)) return std::nullopt;
    return Point{static_cast<int64_t>(std::llround(lat * kScale)),
        static_cast<int64_t>(std::llround(lon * kScale))};
}

std::optional<PolygonMetrics> measure_polygon(std::span<const Point> polygon) {
    if (polygon.size() < kMinRingVertices) return std::nullopt;
    const NearestRounding rounding;
    GeographicLib::PolygonArea accumulator(Geodesic::WGS84());
    for (const auto& p : polygon) {
        if (!valid(p)) return std::nullopt;
        accumulator.AddPoint(p.lat_ndeg / kScale, p.lon_ndeg / kScale);
    }
    PolygonMetrics result{};
    accumulator.Compute(false, true, result.perimeter_m, result.signed_area_sqm);
    if (!std::isfinite(result.perimeter_m) || !std::isfinite(result.signed_area_sqm))
        return std::nullopt;
    return result;
}

namespace {
std::optional<GeometryIssue> ring_issue(std::span<const Point> polygon) {
    const auto fail = [](const char* code, const char* message,
                         std::optional<size_t> vertex = {}, std::optional<size_t> edge = {},
                         std::optional<size_t> other = {}) -> std::optional<GeometryIssue> {
        return GeometryIssue{code, message, 0, 0, vertex, edge, other};
    };
    if (polygon.size() < kMinRingVertices) return fail("too_few_vertices", "A ring needs at least three vertices");
    std::set<std::pair<int64_t, int64_t>> unique;
    for (size_t i = 0; i < polygon.size(); ++i) {
        if (!valid(polygon[i])) return fail("coordinate_range", "WGS84 coordinate outside its range", i);
        if (!unique.insert(position_key(polygon[i])).second)
            return fail("duplicate_position", "Two vertices represent the same physical position", i);
    }
    const auto lines = edges(polygon);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].Distance() <= kSolverToleranceM)
            return fail("short_edge", "Edge shorter than the numerical resolution", {}, i);
        if (lines[i].Arc() >= 180)
            return fail("ambiguous_edge", "Endpoints have no unique shortest geodesic", {}, i);
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        for (size_t j = i + 1; j < lines.size(); ++j) {
            const auto hit = intersect(lines[i], lines[j]);
            if (!hit.valid) return fail("solver_failure", "Geodesic intersection did not converge", {}, i, j);
            if (!hit.intersects) continue;
            const bool adjacent = j == i + 1 || (i == 0 && j + 1 == lines.size());
            if (!adjacent || (hit.coincidence &&
                coincident_overlap(lines[i], lines[j], hit) > kSolverToleranceM))
                return fail("self_intersection", "Ring edges intersect or touch", {}, i, j);
        }
    }
    const auto metrics = measure_polygon(polygon);
    if (!metrics || metrics->area_sqm() < 0.000001)
        return fail("zero_area", "Ring area below one square millimetre");
    if (Geodesic::WGS84().EllipsoidArea() / 2 - metrics->area_sqm() < 1.0)
        return fail("ambiguous_interior", "A half-world ring has no unique smaller interior");
    return {};
}
}

bool is_simple_polygon(std::span<const Point> polygon) {
    const NearestRounding rounding;
    return !ring_issue(polygon);
}

PointLocation locate_point(std::span<const Point> polygon, const Point& point) {
    if (!valid(point)) return PointLocation::outside;
    const NearestRounding rounding;
    const auto metrics = measure_polygon(polygon);
    if (!metrics) return PointLocation::outside;
    return locate(polygon, point.lat_ndeg / kScale, point.lon_ndeg / kScale,
        metrics->signed_area_sqm);
}

bool polygon_interiors_overlap(std::span<const Point> a, std::span<const Point> b) {
    const NearestRounding rounding;
    const auto am = measure_polygon(a), bm = measure_polygon(b);
    if (!am || !bm || am->area_sqm() == 0 || bm->area_sqm() == 0) return false;
    const auto ae = edges(a), be = edges(b);
    std::vector<std::vector<double>> cuts(ae.size());
    for (size_t i = 0; i < ae.size(); ++i) {
        cuts[i] = {0, ae[i].Distance()};
        for (size_t j = 0; j < be.size(); ++j) {
            const auto hit = intersect(ae[i], be[j]);
            if (!hit.valid) return true;
            if (!hit.intersects) continue;
            if (hit.coincidence) {
                // Coincident boundaries overlap only when both polygon
                // interiors are on the same side of the shared geodesic.
                if (coincident_overlap(ae[i], be[j], hit) > kSolverToleranceM &&
                    am->signed_area_sqm * bm->signed_area_sqm * hit.coincidence > 0)
                    return true;
            } else {
                const bool a_interior = hit.x > kSolverToleranceM &&
                    hit.x < ae[i].Distance() - kSolverToleranceM;
                const bool b_interior = hit.y > kSolverToleranceM &&
                    hit.y < be[j].Distance() - kSolverToleranceM;
                if (a_interior && b_interior) return true;
                cuts[i].push_back(std::clamp(hit.x, 0.0, ae[i].Distance()));
            }
        }
    }
    for (const auto& p : a)
        if (locate(b, p.lat_ndeg / kScale, p.lon_ndeg / kScale, bm->signed_area_sqm) == PointLocation::inside)
            return true;
    for (const auto& p : b)
        if (locate(a, p.lat_ndeg / kScale, p.lon_ndeg / kScale, am->signed_area_sqm) == PointLocation::inside)
            return true;
    // Crossings at vertices can put an edge interior inside the other parcel
    // while all serialized vertices remain on its boundary.
    for (size_t i = 0; i < ae.size(); ++i) {
        auto& positions = cuts[i];
        std::sort(positions.begin(), positions.end());
        for (size_t j = 1; j < positions.size(); ++j) {
            if (positions[j] - positions[j - 1] <= kSolverToleranceM) continue;
            double lat, lon;
            ae[i].Position((positions[j] + positions[j - 1]) / 2, lat, lon);
            if (locate(b, lat, lon, bm->signed_area_sqm) == PointLocation::inside) return true;
        }
    }
    return false;
}

namespace {
void canonical_ring(Ring& ring, bool hole) {
    for (auto& point : ring) {
        if (!valid(point)) continue;
        const auto key = position_key(point);
        point = {key.first, key.second};
    }
    while (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
    if (ring.empty()) return;
    const auto metrics = measure_polygon(ring);
    if (metrics && metrics->signed_area_sqm != 0 &&
        (metrics->signed_area_sqm < 0) != hole) std::reverse(ring.begin(), ring.end());
    std::rotate(ring.begin(), std::min_element(ring.begin(), ring.end()), ring.end());
}
std::vector<const Ring*> rings(const Polygon& polygon) {
    std::vector<const Ring*> result{&polygon.exterior};
    for (const auto& hole : polygon.holes) result.push_back(&hole);
    return result;
}
bool rings_touch(const Ring& a, const Ring& b) {
    const auto ae = edges(a), be = edges(b);
    for (const auto& x : ae) for (const auto& y : be) {
        const auto hit = intersect(x, y);
        if (!hit.valid || hit.intersects) return true;
    }
    return false;
}
double edge_distance(const Line& line, const Point& point, double stop_above) {
    auto distance = [&](double position) {
        double lat, lon, value;
        line.Position(position, lat, lon);
        Geodesic::WGS84().Inverse(lat, lon, point.lat_ndeg / kScale, point.lon_ndeg / kScale, value);
        return value;
    };
    const double length = line.Distance();
    const double first = distance(0), last = distance(length);
    // Triangle inequality provides a cheap conservative exclusion for a
    // radius query; it cannot drop a point close to the middle of an edge.
    const double lower = std::max(0.0, (first + last - length) / 2);
    if (lower > stop_above) return lower;
    double best = std::min(first, last);
    const size_t count = std::max<size_t>(1, static_cast<size_t>(std::ceil(length / 1'000'000)));
    constexpr double ratio = 0.6180339887498948482;
    for (size_t part = 0; part < count; ++part) {
        double a = length * part / count, b = length * (part + 1) / count;
        double c = b - ratio * (b - a), d = a + ratio * (b - a);
        double fc = distance(c), fd = distance(d);
        for (unsigned i = 0; i < 80 && b - a > 0.000001; ++i) {
            if (fc < fd) { b = d; d = c; fd = fc; c = b - ratio * (b - a); fc = distance(c); }
            else { a = c; c = d; fc = fd; d = a + ratio * (b - a); fd = distance(d); }
        }
        best = std::min({best, fc, fd});
    }
    return best;
}
}

Geometry canonicalize_geometry(Geometry geometry) {
    const NearestRounding rounding;
    for (auto& part : geometry.polygons) {
        canonical_ring(part.exterior, false);
        for (auto& hole : part.holes) canonical_ring(hole, true);
        std::sort(part.holes.begin(), part.holes.end());
    }
    std::sort(geometry.polygons.begin(), geometry.polygons.end());
    return geometry;
}

namespace {
void accumulate_metrics(PolygonMetrics& total, const PolygonMetrics& ring, bool hole) {
    if (hole) total.signed_area_sqm -= ring.area_sqm();
    else total.signed_area_sqm += ring.area_sqm();
    total.perimeter_m += ring.perimeter_m;
}
bool usable_metrics(const PolygonMetrics& metrics) {
    return metrics.signed_area_sqm >= 0 && std::isfinite(metrics.perimeter_m);
}
}

std::optional<PolygonMetrics> measure_geometry(const Geometry& geometry) {
    const NearestRounding rounding;
    PolygonMetrics result{};
    if (geometry.polygons.empty()) return {};
    for (const auto& part : geometry.polygons) {
        auto exterior = measure_polygon(part.exterior);
        if (!exterior) return {};
        accumulate_metrics(result, *exterior, false);
        for (const auto& hole : part.holes) {
            auto metrics = measure_polygon(hole);
            if (!metrics) return {};
            accumulate_metrics(result, *metrics, true);
        }
    }
    if (!usable_metrics(result)) return {};
    return result;
}

std::optional<uint64_t> area_square_centimetres(const Geometry& geometry) {
    const auto metrics = measure_geometry(geometry);
    return metrics ? metrics->area_sqcm() : std::nullopt;
}

struct PreparedGeometry::Impl {
    struct PreparedRing { const Ring* points; std::vector<Line> lines; double area; };
    Geometry geometry;
    PolygonMetrics metrics{};
    std::vector<std::vector<PreparedRing>> parts;

    PointLocation locate(double latitude, double longitude) const {
        for (const auto& part : parts) {
            const auto location = [&](const PreparedRing& ring) {
                return coordinates::locate(*ring.points, latitude, longitude, ring.area, &ring.lines);
            };
            const auto outer = location(part.front());
            if (outer == PointLocation::boundary) return outer;
            if (outer == PointLocation::outside) continue;
            bool excluded = false;
            for (size_t i = 1; i < part.size(); ++i) {
                const auto hole = location(part[i]);
                if (hole == PointLocation::boundary) return hole;
                excluded = excluded || hole == PointLocation::inside;
            }
            if (!excluded) return PointLocation::inside;
        }
        return PointLocation::outside;
    }
};
PreparedGeometry::PreparedGeometry(const Geometry& geometry) : impl_(std::make_unique<Impl>()) {
    const NearestRounding rounding;
    impl_->geometry = canonicalize_geometry(geometry);
    PolygonMetrics total{};
    bool measurable = !impl_->geometry.polygons.empty();
    for (const auto& part : impl_->geometry.polygons) {
        std::vector<Impl::PreparedRing> prepared;
        for (const auto* ring : rings(part)) {
            const auto metrics = measure_polygon(*ring);
            if (metrics) accumulate_metrics(total, *metrics, !prepared.empty());
            else measurable = false;
            prepared.push_back({ring, metrics ? edges(*ring) : std::vector<Line>{},
                metrics ? metrics->signed_area_sqm : 0});
        }
        impl_->parts.push_back(std::move(prepared));
    }
    if (measurable && usable_metrics(total)) impl_->metrics = total;
}
PreparedGeometry::~PreparedGeometry() = default;
const Geometry& PreparedGeometry::geometry() const { return impl_->geometry; }
const PolygonMetrics& PreparedGeometry::metrics() const { return impl_->metrics; }
size_t PreparedGeometry::memory_bytes() const {
    size_t result = sizeof(*this) + sizeof(Impl);
    for (const auto& part : impl_->parts) for (const auto& ring : part)
        result += sizeof(ring) + ring.points->capacity() * sizeof(Point) + ring.lines.capacity() * sizeof(Line);
    return result;
}
PointLocation PreparedGeometry::locate(const Point& point) const {
    if (!valid(point)) return PointLocation::outside;
    const NearestRounding rounding;
    return impl_->locate(point.lat_ndeg / kScale, point.lon_ndeg / kScale);
}
std::optional<double> PreparedGeometry::distance_m(const Point& point, double stop_above) const {
    if (!valid(point) || std::isnan(stop_above) || stop_above < 0) return {};
    const NearestRounding rounding;
    if (locate(point) != PointLocation::outside) return 0;
    double best = std::numeric_limits<double>::infinity();
    for (const auto& part : impl_->parts) for (const auto& ring : part)
        for (const auto& line : ring.lines)
            best = std::min(best, edge_distance(line, point, std::min(best, stop_above)));
    return std::isfinite(best) ? std::optional<double>(best) : std::nullopt;
}

bool PreparedGeometry::overlaps(const PreparedGeometry& other) const {
    const NearestRounding rounding;
    const auto& a = impl_->parts;
    const auto& b = other.impl_->parts;
    for (const auto& apart : a) for (const auto& aring : apart) {
        for (const auto& point : *aring.points)
            if (other.locate(point) == PointLocation::inside) return true;
        for (const auto& line : aring.lines) {
            std::vector<double> cuts{0, line.Distance()};
            for (const auto& bpart : b) for (const auto& bring : bpart) for (const auto& bl : bring.lines) {
                const auto hit = intersect(line, bl);
                if (!hit.valid) return true;
                if (!hit.intersects) continue;
                if (hit.coincidence) {
                    if (hit.coincidence > 0 && coincident_overlap(line, bl, hit) > kSolverToleranceM) return true;
                } else {
                    if (hit.x > kSolverToleranceM && hit.x < line.Distance() - kSolverToleranceM &&
                        hit.y > kSolverToleranceM && hit.y < bl.Distance() - kSolverToleranceM) return true;
                    cuts.push_back(std::clamp(hit.x, 0.0, line.Distance()));
                }
            }
            std::sort(cuts.begin(), cuts.end());
            for (size_t i = 1; i < cuts.size(); ++i) {
                if (cuts[i] - cuts[i - 1] <= kSolverToleranceM) continue;
                double lat, lon;
                line.Position((cuts[i] + cuts[i - 1]) / 2, lat, lon);
                // Keep the midpoint off the integer input grid during the
                // boundary test, as for the single-ring solver above.
                if (other.impl_->locate(lat, lon) == PointLocation::inside) return true;
            }
        }
    }
    for (const auto& part : b) for (const auto& ring : part) for (const auto& point : *ring.points)
        if (locate(point) == PointLocation::inside) return true;
    return false;
}
bool geometry_interiors_overlap(const Geometry& a, const Geometry& b) {
    return PreparedGeometry(a).overlaps(PreparedGeometry(b));
}

GeometryValidation validate_geometry(const Geometry& geometry) {
    const NearestRounding rounding;
    auto fail = [](const char* code, const char* message, size_t part = 0, size_t ring = 0) {
        return GeometryValidation{GeometryIssue{code, message, part, ring, {}, {}, {}}};
    };
    if (geometry.polygons.empty() || geometry.polygons.size() > kMaxGeometryParts)
        return fail("part_count", "Geometry needs 1..16 polygon parts");
    size_t vertices = 0, ring_count = 0;
    for (size_t p = 0; p < geometry.polygons.size(); ++p) {
        const auto& part = geometry.polygons[p];
        const auto all_rings = rings(part);
        for (size_t r = 0; r < all_rings.size(); ++r) {
            vertices += all_rings[r]->size();
            if (vertices > kMaxGeometryVertices || ++ring_count > kMaxGeometryRings)
                return fail("vertex_limit", "Geometry exceeds 256 vertices or 64 rings", p, r);
            if (auto issue = ring_issue(*all_rings[r])) {
                issue->part = p; issue->ring = r;
                return {issue};
            }
        }
        for (size_t h = 0; h < part.holes.size(); ++h) {
            if (locate_point(part.exterior, part.holes[h].front()) != PointLocation::inside ||
                rings_touch(part.exterior, part.holes[h]))
                return fail("hole_outside", "A hole must be strictly inside its exterior", p, h + 1);
            for (size_t j = 0; j < h; ++j)
                if (rings_touch(part.holes[h], part.holes[j]) ||
                    polygon_interiors_overlap(part.holes[h], part.holes[j]))
                    return fail("holes_overlap", "Holes must be disjoint and cannot touch", p, h + 1);
        }
        for (size_t j = 0; j < p; ++j) {
            const Geometry a{{part}}, b{{geometry.polygons[j]}};
            if (geometry_interiors_overlap(a, b))
                return fail("parts_overlap", "Polygon parts must have disjoint interiors", p);
        }
    }
    return {};
}

bool GeographicBox::intersects(const GeographicBox& other) const {
    return south <= other.north && north >= other.south && west <= other.east && east >= other.west;
}
std::vector<GeographicBox> radius_boxes(const Point& point, double radius) {
    if (!valid(point) || !std::isfinite(radius) || radius >= 1'000'000)
        return {{-90'000'000'000LL, -180'000'000'000LL, 90'000'000'000LL, 180'000'000'000LL}};
    const NearestRounding rounding;
    // Every WGS84 meridional and prime-vertical radius is >= this value.
    // Using it gives an enclosing spherical cap, not an underestimated box.
    const double angle = std::max(radius, 0.0) / 6335439.0;
    const double radians = std::acos(-1) / 180;
    const double lat = point.lat_ndeg / kScale, lon = point.lon_ndeg / kScale;
    const int64_t south = std::max<int64_t>(-90'000'000'000LL,
        static_cast<int64_t>(std::floor((lat - angle / radians) * kScale)) - 2);
    const int64_t north = std::min<int64_t>(90'000'000'000LL,
        static_cast<int64_t>(std::ceil((lat + angle / radians) * kScale)) + 2);
    if (south == -90'000'000'000LL || north == 90'000'000'000LL)
        return {{south, -180'000'000'000LL, north, 180'000'000'000LL}};
    const double delta = std::asin(std::min(1.0, std::sin(angle) / std::cos(lat * radians))) / radians;
    int64_t west = static_cast<int64_t>(std::floor((lon - delta) * kScale)) - 2;
    int64_t east = static_cast<int64_t>(std::ceil((lon + delta) * kScale)) + 2;
    if (west < -180'000'000'000LL)
        return {{south, west + 360'000'000'000LL, north, 180'000'000'000LL},
                {south, -180'000'000'000LL, north, east}};
    if (east > 180'000'000'000LL)
        return {{south, west, north, 180'000'000'000LL},
                {south, -180'000'000'000LL, north, east - 360'000'000'000LL}};
    return {{south, west, north, east}};
}
std::vector<GeographicBox> geometry_boxes(const Geometry& geometry) {
    std::vector<GeographicBox> result;
    for (const auto& part : geometry.polygons) {
        if (part.exterior.empty()) continue;
        const auto metrics = measure_polygon(part.exterior);
        const auto boxes = radius_boxes(part.exterior.front(), metrics ? metrics->perimeter_m / 2 :
            std::numeric_limits<double>::infinity());
        result.insert(result.end(), boxes.begin(), boxes.end());
    }
    return result;
}

} // namespace geocoin::nft::coordinates
