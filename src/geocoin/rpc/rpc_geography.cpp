#include "rpc_geography.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/blockchain.hpp"

namespace geocoin::rpc {
namespace {

json point_to_geojson(geography::Wgs84Point point) {
    return {{"type", "Point"}, {"coordinates", point.geojson_position()}};
}

} // namespace

json geo_tag_to_json(uint64_t tag, std::optional<consensus::geo::GeoTagResult> exact) {
    json result{
        {"datum", geography::kDatum},
        {"crs", geography::kGeoJsonCrs},
        {"coordinate_order", json::array({"longitude", "latitude"})},
        {"angular_unit", "degree"},
        {"integer_unit", "nanodegree"},
        {"algorithm", "GeoPoW"},
        {"algorithm_version", consensus::geo::kAlgorithmVersion},
        {"grid_step_degrees", static_cast<double>(consensus::geo::kCell) / geography::kNanodegreesPerDegree},
        {"status", "context_unavailable"},
        {"latitude_ndeg", nullptr},
        {"longitude_ndeg", nullptr},
        {"point", nullptr},
        {"cell", nullptr}
    };

    const auto cell = consensus::geo::decode_geo_tag(tag);
    if (!cell) {
        result["status"] = "invalid_tag";
        return result;
    }
    result["cell"] = {
        {"bbox", cell->geojson_bbox()},
        {"center", point_to_geojson(cell->center())}
    };
    result["point_kind"] = exact ? "derived_point" : "cell_center";
    if (!exact) exact = consensus::geo::point_from_geo_tag(tag);
    if (exact->geo_tag != tag || !cell->contains(exact->point())) {
        result["status"] = "tag_mismatch";
        return result;
    }
    result["status"] = "exact";
    result["latitude_ndeg"] = exact->lat_ndeg;
    result["longitude_ndeg"] = exact->lon_ndeg;
    result["point"] = point_to_geojson(exact->point());
    return result;
}

json geopow_rules_to_json(Height height) {
    const Height activation = consensus::Params().geopow_activation_height();
    return {{"algorithm_version", consensus::geo::kAlgorithmVersion},
            {"activation_height", activation},
            {"selection_rule", consensus::geo::selection_rule(height, activation)}};
}

json block_geography_to_json(const core::BlockHeader& header, const core::Blockchain& blockchain) {
    std::optional<consensus::geo::GeoTagResult> exact;
    if (header.prev_block_hash == Hash256{}) {
        if (header.get_hash() == consensus::Params().get_genesis_hash()) {
            exact = consensus::geo::compute_geo_tag(0, Hash256{});
        }
    } else {
        const auto parent = blockchain.get_block_position(header.prev_block_hash);
        if (parent.indexed) {
            exact = consensus::geo::compute_block_geo_tag(
                static_cast<uint32_t>(parent.median_time_past), header.prev_block_hash,
                parent.height + 1, consensus::Params().geopow_activation_height());
        }
    }
    return geo_tag_to_json(header.geo_tag, exact);
}

} // namespace geocoin::rpc
