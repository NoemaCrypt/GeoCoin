#include "rpc_nft_metadata.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/nft/geodesy.hpp"
#include "geocoin/nft/geometry_codec.hpp"
#include "geocoin/geography/wgs84.hpp"
#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/consensus/consensus.hpp"

namespace geocoin::rpc {
namespace {
Hash256 optional_hash256_field(
    const json& object,
    const std::string& field,
    const Hash256& default_value,
    const std::string& error_message
) {
    if (!object.is_object() || !object.contains(field) || object[field].is_null()) {
        return default_value;
    }
    if (!object[field].is_string()) {
        throw RPCException(RPC_INVALID_PARAMETER, error_message);
    }
    Hash256 out{};
    if (!hex_to_hash256_checked(object[field].get<std::string>(), out)) {
        throw RPCException(RPC_INVALID_PARAMETER, error_message);
    }
    return out;
}


} // namespace

nft::NFTMetadata parse_nft_metadata_from_rpc(
    const json& metadata_json,
    const nft::NFTMetadata& defaults
) {
    require_only_fields(metadata_json, {
        "name", "description", "image_uri", "content_hash", "geometry", "area_bits",
        "created_at", "block_height", "block_hash", "location_name", "creator", "tags", "cadastral"
    });
    nft::NFTMetadata metadata;
    metadata.name = metadata_json.value("name", "");
    metadata.description = metadata_json.value("description", "");
    metadata.image_uri = metadata_json.value("image_uri", "");
    metadata.content_hash = required_string_field(
        metadata_json,
        "content_hash",
        "metadata.content_hash is required"
    ).get<std::string>();
    Hash256 content_hash{};
    if (!hex_to_hash256_checked(metadata.content_hash, content_hash)) {
        throw RPCException(RPC_INVALID_PARAMETER, "metadata.content_hash must be a 32-byte hex string");
    }
    try {
        metadata.geometry = nft::coordinates::geometry_from_json(required_field(metadata_json, "geometry", "metadata.geometry is required"));
    } catch (const nft::coordinates::GeometryError& error) {
        throw RPCException(RPC_INVALID_PARAMETER, nft::coordinates::geometry_issue_to_json(error.issue).dump());
    }
    metadata.area_bits = optional_u64_field(metadata_json, "area_bits", 0, "metadata.area_bits must be an integer");
    metadata.created_at = optional_u64_field(
        metadata_json,
        "created_at",
        defaults.created_at,
        "metadata.created_at must be an integer"
    );
    metadata.block_height = optional_u64_field(
        metadata_json,
        "block_height",
        defaults.block_height,
        "metadata.block_height must be an integer"
    );
    metadata.block_hash = optional_hash256_field(
        metadata_json,
        "block_hash",
        defaults.block_hash,
        "metadata.block_hash must be a 32-byte hex string"
    );
    metadata.location_name = metadata_json.value("location_name", "");
    metadata.creator = metadata_json.value("creator", defaults.creator);

    if (metadata_json.contains("tags") && !metadata_json["tags"].is_null()) {
        if (!metadata_json["tags"].is_array()) {
            throw RPCException(RPC_INVALID_PARAMETER, "metadata.tags must be an array");
        }
        metadata.tags.clear();
        metadata.tags.reserve(metadata_json["tags"].size());
        for (const auto& tag_json : metadata_json["tags"]) {
            if (!tag_json.is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "metadata.tags must contain strings only");
            }
            metadata.tags.push_back(tag_json.get<std::string>());
        }
    }

    if (metadata_json.contains("cadastral") && !metadata_json["cadastral"].is_null()) {
        const json& cadastral_json = required_object_field(
            metadata_json,
            "cadastral",
            "metadata.cadastral must be an object"
        );
        nft::NFTMetadata::CadastralData cadastral;
        require_only_fields(cadastral_json, {
            "cadastral_number", "owner_name", "land_category", "permitted_use", "area_sqcm_official",
            "ownership_document_hash", "technical_plan_hash", "photo_hashes", "market_value_angles",
            "cadastral_value_angles", "last_valuation_date", "restrictions", "has_buildings", "building_count"
        });
        cadastral.cadastral_number = cadastral_json.value("cadastral_number", "");
        cadastral.owner_name = cadastral_json.value("owner_name", "");
        cadastral.land_category = cadastral_json.value("land_category", "");
        cadastral.permitted_use = cadastral_json.value("permitted_use", "");
        cadastral.area_sqcm_official = optional_u64_field(
            cadastral_json,
            "area_sqcm_official",
            0,
            "metadata.cadastral.area_sqcm_official must be an integer"
        );
        cadastral.ownership_document_hash = optional_hash256_field(
            cadastral_json,
            "ownership_document_hash",
            Hash256{},
            "metadata.cadastral.ownership_document_hash must be a 32-byte hex string"
        );
        cadastral.technical_plan_hash = optional_hash256_field(
            cadastral_json,
            "technical_plan_hash",
            Hash256{},
            "metadata.cadastral.technical_plan_hash must be a 32-byte hex string"
        );
        if (cadastral_json.contains("photo_hashes") && !cadastral_json["photo_hashes"].is_null()) {
            if (!cadastral_json["photo_hashes"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "metadata.cadastral.photo_hashes must be an array");
            }
            for (const auto& hash_json : cadastral_json["photo_hashes"]) {
                if (!hash_json.is_string()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "metadata.cadastral.photo_hashes must contain strings only");
                }
                Hash256 hash{};
                if (!hex_to_hash256_checked(hash_json.get<std::string>(), hash)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "metadata.cadastral.photo_hashes must contain 32-byte hex strings");
                }
                cadastral.photo_hashes.push_back(hash);
            }
        }
        cadastral.market_value = rpc_optional_amount_field(
            cadastral_json,
            "market_value_angles",
            0,
            "metadata.cadastral.market_value_angles must be a non-negative integer"
        );
        cadastral.cadastral_value = rpc_optional_amount_field(
            cadastral_json,
            "cadastral_value_angles",
            0,
            "metadata.cadastral.cadastral_value_angles must be a non-negative integer"
        );
        cadastral.last_valuation_date = optional_u64_field(
            cadastral_json,
            "last_valuation_date",
            0,
            "metadata.cadastral.last_valuation_date must be an integer"
        );
        if (cadastral_json.contains("restrictions") && !cadastral_json["restrictions"].is_null()) {
            if (!cadastral_json["restrictions"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "metadata.cadastral.restrictions must be an array");
            }
            for (const auto& restriction_json : cadastral_json["restrictions"]) {
                if (!restriction_json.is_string()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "metadata.cadastral.restrictions must contain strings only");
                }
                cadastral.restrictions.push_back(restriction_json.get<std::string>());
            }
        }
        cadastral.has_buildings = cadastral_json.value("has_buildings", false);
        cadastral.building_count = optional_u32_field(
            cadastral_json,
            "building_count",
            0,
            "metadata.cadastral.building_count must be an integer"
        );
        metadata.cadastral = std::move(cadastral);
    }

    if (!nft::metadata_within_consensus_limits(metadata)) {
        throw RPCException(
            RPC_INVALID_PARAMETER,
            "NFT metadata exceeds consensus field limits"
        );
    }

    return metadata;
}


json nft_metadata_to_json(const nft::NFTMetadata& metadata) {
    json md;
    md["name"] = metadata.name;
    md["description"] = metadata.description;
    md["image_uri"] = metadata.image_uri;
    md["content_hash"] = metadata.content_hash;

    md["geometry"] = nft::coordinates::geometry_to_json(metadata.geometry);
    md["area_bits"] = metadata.area_bits;
    md["created_at"] = metadata.created_at;
    md["block_height"] = metadata.block_height;
    md["block_hash"] = hash256_to_hex_string(metadata.block_hash);
    if (!metadata.location_name.empty()) {
        md["location_name"] = metadata.location_name;
    }
    if (!metadata.creator.empty()) {
        md["creator"] = metadata.creator;
    }
    if (!metadata.tags.empty()) {
        md["tags"] = metadata.tags;
    }
    if (metadata.cadastral.has_value()) {
        const auto& c = metadata.cadastral.value();
        json cj;
        cj["cadastral_number"] = c.cadastral_number;
        cj["owner_name"] = c.owner_name;
        cj["land_category"] = c.land_category;
        cj["permitted_use"] = c.permitted_use;
        cj["area_sqcm_official"] = c.area_sqcm_official;
        cj["ownership_document_hash"] = hash256_to_hex_string(c.ownership_document_hash);
        cj["technical_plan_hash"] = hash256_to_hex_string(c.technical_plan_hash);
        json photos = json::array();
        for (const auto& h : c.photo_hashes) {
            photos.push_back(hash256_to_hex_string(h));
        }
        cj["photo_hashes"] = std::move(photos);
        cj["market_value_angles"] = c.market_value;
        cj["cadastral_value_angles"] = c.cadastral_value;
        cj["last_valuation_date"] = c.last_valuation_date;
        cj["restrictions"] = c.restrictions;
        cj["has_buildings"] = c.has_buildings;
        cj["building_count"] = c.building_count;
        md["cadastral"] = std::move(cj);
    }
    return md;
}

json nft_geography_to_json(const nft::NFTMetadata& metadata) {
    const auto metrics = nft::coordinates::measure_geometry(metadata.geometry);
    json out = {
        {"datum", geography::kDatum},
        {"crs", geography::kGeographicCrs},
        {"coordinate_order", {"latitude", "longitude"}},
        {"integer_unit", "nanodegree"},
        {"edge_model", "shortest_wgs84_geodesic"},
        {"interior", "smaller_region"},
        {"library", nft::coordinates::kGeodesyLibrary},
        {"library_version", nft::coordinates::kGeodesyVersion},
        {"rules_version", nft::coordinates::kGeodesyRulesVersion},
        {"area_sqm", nullptr},
        {"perimeter_m", nullptr}
    };
    const auto validation = nft::coordinates::validate_geometry(metadata.geometry);
    out["valid"] = bool(validation);
    out["parts"] = metadata.geometry.polygons.size();
    if (!validation) out["issue"] = nft::coordinates::geometry_issue_to_json(*validation.issue);
    if (metrics) {
        out["area_sqcm"] = metrics->area_sqcm().value_or(0);
        out["area_sqm"] = metrics->area_sqm();
        out["perimeter_m"] = metrics->perimeter_m;
    }
    return out;
}

} // namespace geocoin::rpc
