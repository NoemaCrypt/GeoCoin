#pragma once

#include <array>
#include <cstdint>
#include <compare>

namespace geocoin::geography {

// WGS 84 geographic 2D coordinates: geodetic latitude, east-positive
// longitude, angular degrees. EPSG:4326 orders axes latitude/longitude;
// OGC:CRS84 and RFC 7946 GeoJSON order them longitude/latitude.
inline constexpr char kDatum[] = "WGS 84";
inline constexpr char kGeographicCrs[] = "EPSG:4326";
inline constexpr char kGeoJsonCrs[] = "OGC:CRS84";
inline constexpr int64_t kNanodegreesPerDegree = 1'000'000'000;
inline constexpr int64_t kMinLatitude = -90'000'000'000;
inline constexpr int64_t kMaxLatitude = 90'000'000'000;
inline constexpr int64_t kMinLongitude = -180'000'000'000;
inline constexpr int64_t kMaxLongitude = 180'000'000'000;

struct Wgs84Point {
    int64_t latitude_ndeg = 0;
    int64_t longitude_ndeg = 0;

    auto operator<=>(const Wgs84Point&) const = default;

    constexpr bool is_valid() const noexcept {
        return latitude_ndeg >= kMinLatitude && latitude_ndeg <= kMaxLatitude &&
               longitude_ndeg >= kMinLongitude && longitude_ndeg <= kMaxLongitude;
    }

    constexpr double latitude_degrees() const noexcept {
        return static_cast<double>(latitude_ndeg) / kNanodegreesPerDegree;
    }

    constexpr double longitude_degrees() const noexcept {
        return static_cast<double>(longitude_ndeg) / kNanodegreesPerDegree;
    }

    // RFC 7946 section 3.1.1. No altitude is invented for a 2D block marker.
    constexpr std::array<double, 2> geojson_position() const noexcept {
        return {longitude_degrees(), latitude_degrees()};
    }
};

} // namespace geocoin::geography
