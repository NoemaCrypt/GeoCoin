#include "geocoin/rpc/rpc_server.hpp"

#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/crypto/geo_area.hpp"

#include <limits>

namespace geocoin::rpc {

namespace {
constexpr size_t kMaxSidechainGenesisPayloadBytes = 64 * 1024;
constexpr size_t kMaxSidechainNameBytes = 128;
constexpr size_t kMaxSidechainMetadataBytes = 4096;

std::string optional_bounded_string(
    const json& object,
    const char* field,
    size_t max_size
) {
    if (!object.contains(field)) {
        return {};
    }
    if (!object[field].is_string()) {
        throw RPCException(RPC_INVALID_PARAMETER, std::string(field) + " должен быть строкой");
    }
    std::string value = object[field].get<std::string>();
    if (value.size() > max_size) {
        throw RPCException(RPC_INVALID_PARAMETER, std::string(field) + " слишком длинный");
    }
    return value;
}
} // namespace

void RPCServer::register_blockchain_sidechain_methods() {
    register_method({
        .name = "getsidechaingenesis",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();
            auto chain_state = blockchain.get_chain_state();
            if (!chain_state) {
                throw RPCException(RPC_INTERNAL_ERROR, "ChainState не инициализирован");
            }
            std::string id_hex = required_param(params, 0, "Требуется sidechain_id (hex)").get<std::string>();
            state::SidechainRegistryEntry entry;
            if (!chain_state->get_sidechain_registry_by_id_hex(id_hex, entry)) {
                throw RPCException(RPC_INVALID_PARAMETER, "sidechain_id не найден");
            }
            json out;
            out["sidechain_id"] = id_hex;
            out["genesis_hash"] = id_hex;
            out["sidechain_kind"] = core::sidechain_kind_to_string(entry.kind);
            out["status"] = state::sidechain_status_to_string(entry.status);
            out["owner"] = entry.owner.str();
            out["checkpoint_policy_id"] = entry.checkpoint_policy_id;
            out["checkpoint_version"] = entry.checkpoint_version;
            if (entry.status == state::SidechainStatus::DECOMMISSIONING) {
                out["decommission_height"] = entry.decommission_height;
                if (entry.decommission_height <=
                    std::numeric_limits<Height>::max() -
                        consensus::params::SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS) {
                    out["challenge_end_height"] = entry.decommission_height +
                        consensus::params::SIDECHAIN_DECOMMISSION_CHALLENGE_BLOCKS;
                } else {
                    out["challenge_end_height"] = nullptr;
                }
            }
            if (entry.kind == core::SidechainKind::FINANCIAL) {
                out["token_symbol"] = entry.token_symbol;
                out["issuance_limit"] = entry.issuance_limit;
                out["total_issued"] = entry.total_issued;
                out["multiplier"] = entry.multiplier;
                out["fee_bps"] = entry.fee_bps;
                out["max_multiplier"] = entry.max_multiplier;
            } else {
                out["namespace"] = entry.accounting_namespace;
                out["schema_hash"] = hash256_to_hex_string(entry.accounting_schema_hash);
            }
            return out;
        },
        .category = "blockchain",
        .help = "getsidechaingenesis \"sidechain_id\"\n\nВозвращает genesis_hash (равен sidechain_id) и базовую информацию.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"sidechain_id"}
    });

    register_method({
        .name = "createsidechaingenesis",
        .method = [this](const json& params) -> json {
            (void)this;
            const json& p = required_object_param(params, 0, "Требуется объект параметров");

            bytes payload;
            if (p.contains("payload_hex")) {
                require_only_fields(p, {"payload_hex"});
                if (!p["payload_hex"].is_string()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "payload_hex должен быть строкой");
                }
                const std::string payload_hex = p["payload_hex"].get<std::string>();
                if (payload_hex.size() > kMaxSidechainGenesisPayloadBytes * 2 ||
                    !hex_to_bytes_checked(payload_hex, payload)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "payload_hex неверный");
                }
            } else {
                require_only_fields(p, {
                    "version", "timestamp", "nonce", "chain_name", "meta",
                    "sidechain_kind", "schema_hash"
                });
                std::string sidechain_kind = "financial";
                if (p.contains("sidechain_kind")) {
                    sidechain_kind = required_string_field(
                        p,
                        "sidechain_kind",
                        "sidechain_kind должен быть строкой"
                    ).get<std::string>();
                }
                if (sidechain_kind != "financial" && sidechain_kind != "accounting") {
                    throw RPCException(RPC_INVALID_PARAMETER, "sidechain_kind должен быть financial или accounting");
                }
                const uint32_t version = u64_to_u32_checked(
                    optional_u64_field(
                        p,
                        "version",
                        sidechain_kind == "accounting" ? 2 : 1,
                        "version вне диапазона"
                    ),
                    "version вне диапазона"
                );
                Hash256 schema_hash{};
                if (p.contains("schema_hash")) {
                    if (!p["schema_hash"].is_string() ||
                        !hex_to_hash256_checked(p["schema_hash"].get<std::string>(), schema_hash)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "schema_hash должен быть 32-байт hex");
                    }
                }
                if (sidechain_kind == "accounting" &&
                    (version < 2 || schema_hash == Hash256{})) {
                    throw RPCException(RPC_INVALID_PARAMETER, "accounting genesis требует version >= 2 и schema_hash");
                }
                const uint64_t timestamp = optional_u64_field(
                    p, "timestamp", 0, "timestamp вне диапазона");
                const uint64_t nonce = optional_u64_field(
                    p, "nonce", 0, "nonce вне диапазона");
                const std::string chain_name = optional_bounded_string(
                    p, "chain_name", kMaxSidechainNameBytes);
                const std::string meta = optional_bounded_string(
                    p, "meta", kMaxSidechainMetadataBytes);

                payload.insert(payload.end(), {'S','C','G','E','N'});
                for (int i = 0; i < 4; i++) {
                    payload.push_back((version >> (i * 8)) & 0xFF);
                }
                for (int i = 0; i < 8; i++) {
                    payload.push_back((timestamp >> (i * 8)) & 0xFF);
                }
                for (int i = 0; i < 8; i++) {
                    payload.push_back((nonce >> (i * 8)) & 0xFF);
                }

                uint32_t name_len = static_cast<uint32_t>(chain_name.size());
                for (int i = 0; i < 4; i++) {
                    payload.push_back((name_len >> (i * 8)) & 0xFF);
                }
                payload.insert(payload.end(), chain_name.begin(), chain_name.end());

                uint32_t meta_len = static_cast<uint32_t>(meta.size());
                for (int i = 0; i < 4; i++) {
                    payload.push_back((meta_len >> (i * 8)) & 0xFF);
                }
                payload.insert(payload.end(), meta.begin(), meta.end());
                if (version >= 2) {
                    payload.push_back(
                        sidechain_kind == "accounting"
                            ? static_cast<byte>(core::SidechainKind::ACCOUNTING)
                            : static_cast<byte>(core::SidechainKind::FINANCIAL)
                    );
                    payload.insert(payload.end(), schema_hash.begin(), schema_hash.end());
                }
            }

            Hash256 h = crypto::sha256(payload);
            json out;
            out["genesis_hash"] = hash256_to_hex_string(h);
            out["payload_hex"] = bytes_to_hex_string(payload);
            if (!p.contains("payload_hex")) {
                out["sidechain_kind"] = p.value("sidechain_kind", "financial");
                if (p.contains("schema_hash")) {
                    out["schema_hash"] = p["schema_hash"];
                }
            }
            return out;
        },
        .category = "blockchain",
        .help = "createsidechaingenesis {\"chain_name\":\"...\",\"meta\":\"...\",\"timestamp\":n,\"nonce\":n,\"version\":n}\nили\ncreatesidechaingenesis {\"payload_hex\":\"...\"}\n\nВозвращает genesis_hash (SHA256 payload).\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"request"}
    });
}

} // namespace geocoin::rpc
