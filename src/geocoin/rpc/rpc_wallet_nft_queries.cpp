#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "rpc_nft_metadata.hpp"
#include "geocoin/nft/geometry_codec.hpp"

namespace geocoin::rpc {
namespace {
constexpr size_t kDefaultNftOwnerPage = 100;
constexpr size_t kMaxNftOwnerPage = 1000;
constexpr size_t kDefaultUtxoPage = 1000;
constexpr size_t kMaxUtxoPage = 5000;
}

void RPCServer::register_wallet_nft_query_methods() {

    register_method({
        .name = "analyzegeometry",
        .method = [](const json& params) -> json {
            try {
                const auto& value = required_param(params, 0, "geometry is required");
                const bool geojson = value.is_object() && value.contains("type");
                const auto geometry = geojson ? nft::coordinates::geometry_from_geojson(value, params.size() > 1 && !params[1].is_null() ? params[1].get<double>() : 1.0) :
                    nft::coordinates::geometry_from_json(value);
                const nft::coordinates::PreparedGeometry prepared(geometry);
                json result{{"valid", true}, {"geometry", nft::coordinates::geometry_to_json(geometry)},
                    {"rules_version", nft::coordinates::kGeodesyRulesVersion}, {"coordinate_unit", "nanodegree"},
                    {"area_sqcm", prepared.metrics().area_sqcm().value_or(0)},
                    {"area_sqm", prepared.metrics().area_sqm()}, {"perimeter_m", prepared.metrics().perimeter_m}};
                if (params.size() > 1 && !params[1].is_null()) {
                    const double tolerance = params[1].get<double>();
                    result["geojson"] = nft::coordinates::geometry_to_geojson(geometry, tolerance);
                }
                return result;
            } catch (const nft::coordinates::GeometryError& error) {
                return {{"valid", false}, {"issue", nft::coordinates::geometry_issue_to_json(error.issue)}};
            }
        },
        .category = "nft",
        .help = "analyzegeometry geometry ( tolerance_m )\nValidate native or GeoJSON Polygon/MultiPolygon geometry; return canonical nanodegrees, area and perimeter. Optional tolerance_m exports RFC7946 display geometry. Issue indices are zero based.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"geometry", "tolerance_m"}
    });
    register_method({
        .name = "getnftgeojson",
        .method = [this](const json& params) -> json {
            auto& state = require_chain_state();
            Hash256 id{};
            if (!hex_to_hash256_checked(required_param(params, 0, "nftid is required").get<std::string>(), id))
                throw RPCException(RPC_INVALID_PARAMETER, "Invalid nftid");
            if (!state.nft_exists(id)) throw RPCException(RPC_INVALID_PARAMETER, "NFT does not exist");
            const auto metadata = state.get_nft_metadata(id);
            const double tolerance = params.size() > 1 && !params[1].is_null() ? params[1].get<double>() : 1.0;
            try {
                return {{"type", "Feature"}, {"id", hash256_to_hex_string(id)},
                    {"geometry", nft::coordinates::geometry_to_geojson(metadata.geometry, tolerance)},
                    {"properties", {{"name", metadata.name}, {"display_tolerance_m", tolerance},
                        {"native_geometry", nft::coordinates::geometry_to_json(metadata.geometry)},
                        {"geography", nft_geography_to_json(metadata)}}}};
            } catch (const nft::coordinates::GeometryError& error) {
                throw RPCException(RPC_INVALID_PARAMETER, nft::coordinates::geometry_issue_to_json(error.issue).dump());
            }
        },
        .category = "nft",
        .help = "getnftgeojson nftid ( tolerance_m )\nExport parcel as an RFC7946 Feature. tolerance_m defaults to 1 metre. Native signed nanodegree geometry is included in properties for exact recovery.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"nftid", "tolerance_m"}
    });

    // getnft - Получить данные NFT по id
    register_method({
        .name = "getnft",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            const std::string nft_id_hex = required_param(params, 0, "Требуется nft_id (hex)").get<std::string>();
            Hash256 nft_id;
            if (!hex_to_hash256_checked(nft_id_hex, nft_id)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Неверный nft_id hex");
            }

            json result;
            result["nft_id"] = hash256_to_hex_string(nft_id);
            result["exists"] = chain_state.nft_exists(nft_id);
            if (!result["exists"].get<bool>()) {
                return result;
            }

            const auto owner = chain_state.get_nft_owner(nft_id);
            const auto meta = chain_state.get_nft_metadata(nft_id);

            result["owner"] = owner.str();
            result["metadata"] = nft_metadata_to_json(meta);
            result["geography"] = nft_geography_to_json(meta);
            return result;
        },
        .category = "nft",
        .help = "getnft \"nftid\"\n\nВозвращает owner, metadata и рассчитанную geography WGS84 (площадь м², периметр м).\n\nАргументы:\n1. \"nftid\" (string, required) hex (32 bytes)\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"nftid"}
    });

    // listnftsbyowner - Список NFT, принадлежащих адресу
    register_method({
        .name = "listnftsbyowner",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            Address owner(required_param(params, 0, "Требуется address").get<std::string>());
            const size_t offset = u64_to_size_checked(
                optional_u64_param(params, 1, 0, "offset must be a non-negative integer"),
                "offset is too large");
            const size_t limit = optional_bounded_size_param(
                params, 2, kDefaultNftOwnerPage, 1, kMaxNftOwnerPage, "limit");
            const auto ids = chain_state.get_owned_nfts_page(owner, offset, limit);

            json out = json::array();
            for (const auto& id : ids) {
                json item;
                item["nft_id"] = hash256_to_hex_string(id);
                const auto meta = chain_state.get_nft_metadata(id);
                item["name"] = meta.name;
                item["image_uri"] = meta.image_uri;
                out.push_back(item);
            }
            return out;
        },
        .category = "nft",
        .help = "listnftsbyowner \"address\" ( offset limit )\n\nВозвращает bounded NFT page для owner (default 100, maximum 1000).\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"address", "offset", "limit"}
    });

    // getutxos - Список UTXO для адреса (для мобильного выбора inputs)
    register_method({
        .name = "getutxos",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            Address addr(required_param(params, 0, "Требуется address").get<std::string>());
            const size_t offset = u64_to_size_checked(
                optional_u64_param(params, 1, 0, "offset must be a non-negative integer"),
                "offset is too large");
            const size_t limit = optional_bounded_size_param(
                params, 2, kDefaultUtxoPage, 1, kMaxUtxoPage, "limit");
            const auto utxos = chain_state.get_utxos_for_address_page(addr, offset, limit);
            json out = json::array();
            for (const auto& u : utxos) {
                json item;
                item["txid"] = hash256_to_hex_string(u.tx_hash);
                item["vout"] = u.vout;
                item["value"] = u.value;
                item["address"] = u.address.str();
                item["height"] = u.height;
                item["is_coinbase"] = u.is_coinbase;
                out.push_back(item);
            }
            return out;
        },
        .category = "blockchain",
        .help = "getutxos \"address\" ( offset limit )\n\nВозвращает bounded UTXO page для адреса (default 1000, maximum 5000).\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"address", "offset", "limit"},
        .deprecation = "Use getaddressutxos; getutxos is a legacy single-address alias."
    });

}

} // namespace geocoin::rpc
