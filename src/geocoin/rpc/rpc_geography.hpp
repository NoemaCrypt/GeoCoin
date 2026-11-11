#pragma once

#include "geocoin/consensus/geo_tag.hpp"
#include "geocoin/rpc/rpc_types.hpp"

#include <optional>

namespace geocoin::core {
struct BlockHeader;
class Blockchain;
}

namespace geocoin::rpc {

// GeoJSON geometries use OGC:CRS84 (WGS 84 longitude, latitude in degrees).
// Post-activation blocks use the cell centre. Historical points require parent
// context; point_kind explicitly distinguishes them from a cell-centre fallback.
// The actual genesis point is supplied explicitly from its fixed constants.
json geo_tag_to_json(
    uint64_t tag,
    std::optional<consensus::geo::GeoTagResult> exact = std::nullopt
);

json geopow_rules_to_json(Height height);

json block_geography_to_json(const core::BlockHeader& header, const core::Blockchain& blockchain);

} // namespace geocoin::rpc
