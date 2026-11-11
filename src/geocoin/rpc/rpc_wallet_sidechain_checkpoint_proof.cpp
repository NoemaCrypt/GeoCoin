#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/consensus/sidechain_checkpoint.hpp"
#include "geocoin/utils/bytes.hpp"

namespace geocoin::rpc {

namespace {

Hash256 parse_checkpoint_sidechain_id(const json& request) {
    Hash256 sidechain_id{};
    bool found = false;
    if (request.contains("sidechain_id")) {
        if (!request["sidechain_id"].is_string()) {
            throw RPCException(RPC_INVALID_PARAMETER, "sidechain_id must be a string");
        }
        if (!hex_to_hash256_checked(
                request["sidechain_id"].get<std::string>(),
                sidechain_id)) {
            throw RPCException(
                RPC_INVALID_PARAMETER,
                "sidechain_id должен быть 32-байт hex"
            );
        }
        found = true;
    }
    if (request.contains("genesis_hash")) {
        if (!request["genesis_hash"].is_string()) {
            throw RPCException(RPC_INVALID_PARAMETER, "genesis_hash must be a string");
        }
        Hash256 genesis_hash{};
        if (!hex_to_hash256_checked(
                request["genesis_hash"].get<std::string>(),
                genesis_hash)) {
            throw RPCException(
                RPC_INVALID_PARAMETER,
                "genesis_hash должен быть 32-байт hex"
            );
        }
        if (found && sidechain_id != genesis_hash) {
            throw RPCException(
                RPC_INVALID_PARAMETER,
                "sidechain_id должен совпадать с genesis_hash"
            );
        }
        sidechain_id = genesis_hash;
        found = true;
    }
    if (!found) {
        throw RPCException(
            RPC_INVALID_PARAMETER,
            "sidechain_id или genesis_hash обязателен"
        );
    }
    return sidechain_id;
}

} // namespace

void RPCServer::register_wallet_sidechain_checkpoint_proof_methods() {

    // createcheckpointproofv2 - собрать proof_hex + checkpoint для batch v2
    register_method({
        .name = "createcheckpointproofv2",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект с полями sidechain_id/genesis_hash, state_root, checkpoint_height, total_supply, leaf_index, merkle_path");
            const Hash256 sidechain_id = parse_checkpoint_sidechain_id(p);

            if (!p.contains("state_root") || !p["state_root"].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "state_root обязателен (hex32)");
            }
            Hash256 state_root{};
            if (!hex_to_hash256_checked(p["state_root"].get<std::string>(), state_root)) {
                throw RPCException(RPC_INVALID_PARAMETER, "state_root должен быть 32-байт hex");
            }

            if (!p.contains("checkpoint_height")) {
                throw RPCException(RPC_INVALID_PARAMETER, "checkpoint_height обязателен");
            }
            const Height checkpoint_height = static_cast<Height>(required_u64_field(p, "checkpoint_height", "checkpoint_height обязателен"));
            if (checkpoint_height == 0 ||
                checkpoint_height > consensus::params::MAX_SIDECHAIN_CHECKPOINT_HEIGHT) {
                throw RPCException(RPC_INVALID_PARAMETER, "checkpoint_height вне допустимого диапазона");
            }

            if (!p.contains("total_supply")) {
                throw RPCException(RPC_INVALID_PARAMETER, "total_supply обязателен");
            }
            const uint64_t total_supply = required_u64_field(p, "total_supply", "total_supply обязателен");

            if (!p.contains("leaf_index")) {
                throw RPCException(RPC_INVALID_PARAMETER, "leaf_index обязателен");
            }
            const uint64_t leaf_index_u64 = required_u64_field(p, "leaf_index", "leaf_index обязателен");
            if (leaf_index_u64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
                throw RPCException(RPC_INVALID_PARAMETER, "leaf_index вне диапазона uint32");
            }
            const uint32_t leaf_index = static_cast<uint32_t>(leaf_index_u64);

            if (!p.contains("merkle_path") || !p["merkle_path"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "merkle_path обязателен (array hex32)");
            }
            std::vector<Hash256> merkle_path;
            for (const auto& h : p["merkle_path"]) {
                if (!h.is_string()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "merkle_path должен содержать hex-строки");
                }
                Hash256 sibling{};
                if (!hex_to_hash256_checked(h.get<std::string>(), sibling)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "элемент merkle_path должен быть 32-байт hex");
                }
                merkle_path.push_back(sibling);
            }
            if (merkle_path.size() > consensus::params::MAX_BATCH_CHECKPOINT_MERKLE_DEPTH) {
                throw RPCException(RPC_INVALID_PARAMETER, "merkle_path слишком длинный");
            }
            if (!core::merkle_leaf_index_in_range(merkle_path.size(), leaf_index)) {
                throw RPCException(RPC_INVALID_PARAMETER, "leaf_index не помещается в заданную глубину merkle_path");
            }

            if (p.contains("verify") && !p["verify"].is_boolean()) {
                throw RPCException(RPC_INVALID_PARAMETER, "verify must be boolean");
            }
            const bool verify = p.value("verify", true);

            const Hash256 supply_leaf =
                consensus::sidechain_checkpoint::build_financial_supply_leaf_v2(
                sidechain_id,
                checkpoint_height,
                total_supply
            );
            if (verify && !core::verify_merkle_path(supply_leaf, merkle_path, state_root, leaf_index)) {
                throw RPCException(RPC_INVALID_PARAMETER, "merkle_path не соответствует state_root");
            }

            const Hash256 checkpoint =
                consensus::sidechain_checkpoint::build_financial_commitment_v2(
                sidechain_id,
                state_root,
                checkpoint_height,
                total_supply
            );
            bytes proof = consensus::sidechain_checkpoint::build_financial_proof_v2(
                state_root,
                checkpoint_height,
                total_supply,
                leaf_index,
                merkle_path
            );
            if (proof.size() > consensus::params::MAX_BATCH_CHECKPOINT_PROOF) {
                throw RPCException(RPC_INVALID_PARAMETER, "proof превышает лимит MAX_BATCH_CHECKPOINT_PROOF");
            }

            json out;
            out["sidechain_id"] = hash256_to_hex_string(sidechain_id);
            out["checkpoint"] = hash256_to_hex_string(checkpoint);
            out["state_root"] = hash256_to_hex_string(state_root);
            out["leaf_hash"] = hash256_to_hex_string(supply_leaf);
            out["proof_hex"] = bytes_to_hex_string(proof);
            out["proof_size"] = proof.size();
            out["merkle_depth"] = merkle_path.size();
            out["checkpoint_height"] = checkpoint_height;
            out["total_supply"] = total_supply;
            out["leaf_index"] = leaf_index;
            return out;
        },
        .category = "sidechain",
        .help = "createcheckpointproofv2 {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"state_root\":\"hex32\",\"checkpoint_height\":n,\"total_supply\":n,\"leaf_index\":n,\"merkle_path\":[\"hex32\",...],\"verify\":true|false}\n\nСобирает proof_hex для BATCH_SIDECHAIN_CHECKPOINT(v2) и вычисляет checkpoint.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "createaccountingcheckpointproofv1",
        .method = [this](const json& params) -> json {
            (void)this;
            const json& p = required_object_param(
                params,
                0,
                "Требуется объект с полями sidechain_id/genesis_hash, state_root, checkpoint_height, schema_hash"
            );

            const Hash256 sidechain_id = parse_checkpoint_sidechain_id(p);

            Hash256 state_root{};
            if (!p.contains("state_root") || !p["state_root"].is_string() ||
                !hex_to_hash256_checked(p["state_root"].get<std::string>(), state_root)) {
                throw RPCException(RPC_INVALID_PARAMETER, "state_root должен быть 32-байт hex");
            }

            const Height checkpoint_height = static_cast<Height>(required_u64_field(
                p,
                "checkpoint_height",
                "checkpoint_height обязателен"
            ));
            if (checkpoint_height == 0 ||
                checkpoint_height > consensus::params::MAX_SIDECHAIN_CHECKPOINT_HEIGHT) {
                throw RPCException(RPC_INVALID_PARAMETER, "checkpoint_height вне допустимого диапазона");
            }

            Hash256 schema_hash{};
            if (!p.contains("schema_hash") || !p["schema_hash"].is_string() ||
                !hex_to_hash256_checked(p["schema_hash"].get<std::string>(), schema_hash) ||
                utils::is_null_hash256(schema_hash)) {
                throw RPCException(RPC_INVALID_PARAMETER, "schema_hash должен быть ненулевым 32-байт hex");
            }

            const Hash256 checkpoint =
                consensus::sidechain_checkpoint::build_accounting_commitment_v1(
                sidechain_id,
                state_root,
                checkpoint_height,
                schema_hash
            );
            const bytes proof =
                consensus::sidechain_checkpoint::build_accounting_proof_v1(
                state_root,
                checkpoint_height,
                schema_hash
            );

            return json{
                {"sidechain_id", hash256_to_hex_string(sidechain_id)},
                {"sidechain_kind", "accounting"},
                {"checkpoint_policy_id", consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID},
                {"checkpoint_version", consensus::params::ACCOUNTING_CHECKPOINT_VERSION},
                {"checkpoint", hash256_to_hex_string(checkpoint)},
                {"state_root", hash256_to_hex_string(state_root)},
                {"schema_hash", hash256_to_hex_string(schema_hash)},
                {"checkpoint_height", checkpoint_height},
                {"proof_hex", bytes_to_hex_string(proof)},
                {"proof_size", proof.size()}
            };
        },
        .category = "sidechain",
        .help = "createaccountingcheckpointproofv1 {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"state_root\":\"hex32\",\"checkpoint_height\":n,\"schema_hash\":\"hex32\"}\n\nСобирает proof_hex и checkpoint для accounting-sidechain policy 3/version 1.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

}

} // namespace geocoin::rpc
