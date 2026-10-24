#pragma once

// Frozen historical consensus arithmetic. Required to validate blocks before
// the network WGS84 selection activation; never use it for post-activation work.

#include "geocoin/types.hpp"
#include <cstdint>

namespace geocoin::consensus::geo::historical {

// Координаты генезиса (ваш город), зафиксированы для консенсуса.
// Храним в микроградусах, чтобы избежать floating point в коде консенсуса.
constexpr int32_t GENESIS_LAT_U = 48851583;   // 48°51'05.7"N
constexpr int32_t GENESIS_LON_U = 8897278;    // 8°53'50.2"E

// Тег сетки (CELL = 0.1 градуса) в микроградусах
constexpr int32_t kCell = 100000;
constexpr uint32_t GENESIS_LAT_IDX = static_cast<uint32_t>((static_cast<int64_t>(GENESIS_LAT_U) + 90000000LL) / kCell);
constexpr uint32_t GENESIS_LON_IDX = static_cast<uint32_t>((static_cast<int64_t>(GENESIS_LON_U) + 180000000LL) / kCell);
constexpr uint64_t GENESIS_GEO_TAG = (static_cast<uint64_t>(GENESIS_LAT_IDX) << 32) | static_cast<uint64_t>(GENESIS_LON_IDX);

struct GeoTagResult {
    int32_t lat_u = 0;      // микроградусы
    int32_t lon_u = 0;      // микроградусы
    uint64_t geo_tag = 0;   // (lat_idx << 32) | lon_idx
};

inline GeoTagResult compute_geo_tag(uint32_t prev_timestamp, const Hash256& prev_hash) {
    if (prev_timestamp == 0U && prev_hash == Hash256{}) {
        GeoTagResult genesis_res;
        genesis_res.lat_u = GENESIS_LAT_U;
        genesis_res.lon_u = GENESIS_LON_U;
        genesis_res.geo_tag = GENESIS_GEO_TAG;
        return genesis_res;
    }

    const uint32_t target_time = prev_timestamp + 600U;
    const uint32_t time_of_day = target_time % 86400U;

    auto u64_le = [](const Hash256& h, size_t off) -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= (static_cast<uint64_t>(h[off + static_cast<size_t>(i)]) << (i * 8));
        }
        return v;
    };

    auto civil_from_days = [](int64_t z, int& y, unsigned& m, unsigned& d) {
        z += 719468;
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        d = doy - (153 * mp + 2) / 5 + 1;
        m = mp + (mp < 10 ? 3 : -9);
        y += (m <= 2);
    };

    auto day_of_year = [&](uint32_t ts) -> unsigned {
        const int64_t days = static_cast<int64_t>(ts / 86400U);
        int y = 0;
        unsigned m = 0;
        unsigned d = 0;
        civil_from_days(days, y, m, d);
        const bool leap = ((y % 4) == 0) && (((y % 100) != 0) || ((y % 400) == 0));
        static constexpr unsigned cum_norm[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
        static constexpr unsigned cum_leap[12] = {0,31,60,91,121,152,182,213,244,274,305,335};
        const unsigned idx = (m > 0 ? m - 1 : 0);
        const unsigned cum = leap ? cum_leap[idx] : cum_norm[idx];
        return cum + d;
    };

    const int32_t lon_base_u = static_cast<int32_t>((static_cast<uint64_t>(time_of_day) * 360000000ULL) / 86400ULL)
        - 180000000;

    const unsigned doy = day_of_year(target_time);
    auto solar_declination_u = [](unsigned doy_in) -> int32_t {
        // Кусочно-линейная аппроксимация субсолнечной широты:
        // день 80  ->   0.00° (мартовское равноденствие)
        // день 172 -> +23.44° (июньское солнцестояние)
        // день 266 ->   0.00° (сентябрьское равноденствие)
        // день 355 -> -23.44° (декабрьское солнцестояние)
        // Только детерминированная целочисленная математика (безопасно для консенсуса).
        constexpr int32_t kTiltU = 23440000;
        const unsigned d = ((doy_in - 1U) % 365U) + 1U;

        if (d >= 355U || d <= 80U) {
            const unsigned x = (d >= 355U) ? d : (d + 365U);
            return static_cast<int32_t>(
                -kTiltU + (static_cast<int64_t>(x - 355U) * kTiltU) / 90LL
            );
        }
        if (d <= 172U) {
            return static_cast<int32_t>(
                (static_cast<int64_t>(d - 80U) * kTiltU) / 92LL
            );
        }
        if (d <= 266U) {
            return static_cast<int32_t>(
                kTiltU - (static_cast<int64_t>(d - 172U) * kTiltU) / 94LL
            );
        }
        return static_cast<int32_t>(
            -(static_cast<int64_t>(d - 266U) * kTiltU) / 89LL
        );
    };

    const int32_t lat_base_u = solar_declination_u(doy);

    // Детерминированная псевдослучайная точка по освещенному полушарию (только целые).
    // ВАЖНО: реализация безопасна для консенсуса, без floating point и тригонометрии.
    auto splitmix64 = [](uint64_t& x) -> uint64_t {
        x += 0x9e3779b97f4a7c15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    };

    uint64_t seed = u64_le(prev_hash, 0) ^ (u64_le(prev_hash, 16) << 1) ^
                    (static_cast<uint64_t>(target_time) * 0x9e3779b97f4a7c15ULL);
    const uint64_t r_lon = splitmix64(seed);
    const uint64_t r_lat = splitmix64(seed);

    // Дневное полушарие вокруг субсолнечной долготы: смещение [-90°, +90°].
    // Диапазон 180,000,000 микроградусов.
    const int64_t lon_offset_u =
        static_cast<int64_t>(r_lon % 180000001ULL) - 90000000LL;

    // Случайная широта в допустимом диапазоне, смешанная с сезонной базовой широтой.
    const int64_t rand_lat_u =
        static_cast<int64_t>(r_lat % 170000001ULL) - 85000000LL;  // [-85°, +85°]
    int64_t lat_u64 = (static_cast<int64_t>(lat_base_u) + rand_lat_u) / 2;
    int64_t lon_u64 = static_cast<int64_t>(lon_base_u) + lon_offset_u;

    if (lat_u64 > 85000000LL) lat_u64 = 85000000LL;
    if (lat_u64 < -85000000LL) lat_u64 = -85000000LL;

    while (lon_u64 >= 180000000LL) lon_u64 -= 360000000LL;
    while (lon_u64 < -180000000LL) lon_u64 += 360000000LL;

    const int32_t lat_u = static_cast<int32_t>(lat_u64);
    const int32_t lon_u = static_cast<int32_t>(lon_u64);

    // Тег сетки (CELL = 0.1 градуса)
    const uint32_t lat_idx = static_cast<uint32_t>((static_cast<int64_t>(lat_u) + 90000000LL) / kCell);
    const uint32_t lon_idx = static_cast<uint32_t>((static_cast<int64_t>(lon_u) + 180000000LL) / kCell);

    const uint64_t geo_tag = (static_cast<uint64_t>(lat_idx) << 32) | static_cast<uint64_t>(lon_idx);

    GeoTagResult res;
    res.lat_u = lat_u;
    res.lon_u = lon_u;
    res.geo_tag = geo_tag;
    return res;
}

} // namespace geocoin::consensus::geo::historical
