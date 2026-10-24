#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_asset_issue_methods() {

    // createsidechainissuepayload - создать ISSUE_SIDECHAIN с автозаполнением fee_bps/max_multiplier
    register_method({
        .name = "createsidechainissuepayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект с полями sidechain_id, owner_address, token_symbol, multiplier");

            if (!p.contains("genesis_hash") || !p["genesis_hash"].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "genesis_hash обязателен (hex 32 bytes)");
            }
            if (!p.contains("owner_address") || !p["owner_address"].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "owner_address обязателен");
            }
            if (!p.contains("token_symbol") || !p["token_symbol"].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "token_symbol обязателен");
            }
            if (!p.contains("multiplier")) {
                throw RPCException(RPC_INVALID_PARAMETER, "multiplier обязателен");
            }
            if (!p.contains("committee_m")) {
                throw RPCException(RPC_INVALID_PARAMETER, "committee_m обязателен");
            }
            if (!p.contains("committee_members") || !p["committee_members"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "committee_members обязателен (array адресов)");
            }

            core::Transaction tx;
            const auto common = build_sidechain_tx_common(p, tx, "createsidechainissuepayload");
            if (!common.has_genesis) {
                throw RPCException(RPC_INVALID_PARAMETER, "genesis_hash обязателен (hex 32 bytes)");
            }
            const std::string symbol = p["token_symbol"].get<std::string>();
            const uint32_t multiplier = required_u32_field(p, "multiplier", "multiplier обязателен");

            uint32_t fee_bps = 0;
            if (p.contains("fee_bps")) {
                fee_bps = optional_u32_field(p, "fee_bps", 0, "fee_bps вне диапазона");
            }
            uint32_t max_multiplier = 0;
            if (p.contains("max_multiplier")) {
                max_multiplier = optional_u32_field(p, "max_multiplier", 0, "max_multiplier вне диапазона");
            }
            if (max_multiplier == 0) {
                max_multiplier = multiplier;
            }
            uint8_t checkpoint_policy_id =
                consensus::params::FINANCIAL_CHECKPOINT_POLICY_ID;
            uint8_t checkpoint_version =
                consensus::params::FINANCIAL_CHECKPOINT_VERSION;
            if (p.contains("checkpoint_policy_id")) {
                checkpoint_policy_id = optional_u8_field(p, "checkpoint_policy_id", checkpoint_policy_id, "checkpoint_policy_id вне диапазона");
            }
            if (p.contains("checkpoint_version")) {
                checkpoint_version = optional_u8_field(p, "checkpoint_version", checkpoint_version, "checkpoint_version вне диапазона");
            }

            if (symbol.empty() || symbol.size() > consensus::params::MAX_TOKEN_SYMBOL) {
                throw RPCException(RPC_INVALID_PARAMETER, "token_symbol имеет неверную длину");
            }
            if (!is_printable_ascii(symbol)) {
                throw RPCException(RPC_INVALID_PARAMETER, "token_symbol содержит непечатные символы");
            }
            if (multiplier < consensus::params::MIN_SIDECHAIN_MULTIPLIER) {
                throw RPCException(
                    RPC_INVALID_PARAMETER,
                    "multiplier должен быть >= " + std::to_string(consensus::params::MIN_SIDECHAIN_MULTIPLIER)
                );
            }
            if (max_multiplier < consensus::params::MIN_SIDECHAIN_MULTIPLIER ||
                max_multiplier < multiplier ||
                max_multiplier > consensus::params::MAX_SIDECHAIN_MULTIPLIER) {
                throw RPCException(RPC_INVALID_PARAMETER, "max_multiplier вне диапазона");
            }
            if (fee_bps > consensus::params::MAX_SIDECHAIN_FEE_BPS) {
                throw RPCException(RPC_INVALID_PARAMETER, "fee_bps вне диапазона");
            }

            const SidechainCommitteeFields committee = parse_sidechain_committee_from_rpc(p);

            tx.type = core::TransactionType::ISSUE_SIDECHAIN;
            tx.sidechain_amount = 0;
            tx.sidechain_unlock_height = 0;
            tx.sidechain_checkpoint = common.genesis_hash;
            tx.sidechain_token_symbol = symbol;
            tx.sidechain_multiplier = multiplier;
            tx.sidechain_fee_bps = fee_bps;
            tx.sidechain_max_multiplier = max_multiplier;
            tx.sidechain_checkpoint_policy_id = checkpoint_policy_id;
            tx.sidechain_checkpoint_version = checkpoint_version;
            tx.sidechain_committee_m = static_cast<uint8_t>(committee.m);
            tx.sidechain_committee_n = static_cast<uint8_t>(committee.n);
            tx.sidechain_committee_members = committee.members;
            tx.sidechain_committee_id = committee.committee_id;

            const bool validate = get_validate_flag(p, "createsidechainissuepayload");
            if (validate) {
                auto& chain_state = require_chain_state();
                if (chain_state.has_sidechain_registry_key(common.sidechain_id)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain уже существует");
                }
                state::SidechainDeposit deposit{};
                if (!chain_state.get_sidechain_deposit(common.sidechain_id, deposit)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                }
                if (deposit.owner.str() != common.owner_address) {
                    throw RPCException(RPC_INVALID_PARAMETER, "owner_address не совпадает с владельцем депозита");
                }
                const Amount fee = calc_sidechain_fee_rpc(0, fee_bps);
                if (deposit.amount < fee) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Недостаточно средств в депозите для комиссии ISSUE");
                }
            }

            bytes payload = core::build_sidechain_payload_for_signing(tx);

            json out;
            out["tx_hex"] = bytes_to_hex_string(tx.serialize());
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload);
            out["sidechain_id"] = hash256_to_hex_string(common.sidechain_id);
            out["genesis_hash"] = hash256_to_hex_string(common.genesis_hash);
            out["owner_address"] = common.owner_address;
            out["token_symbol"] = symbol;
            out["multiplier"] = multiplier;
            out["fee_bps"] = fee_bps;
            out["max_multiplier"] = max_multiplier;
            out["committee_m"] = committee.m;
            out["committee_n"] = committee.n;
            out["committee_id"] = hash256_to_hex_string(tx.sidechain_committee_id);
            out["checkpoint_policy_id"] = checkpoint_policy_id;
            out["checkpoint_version"] = checkpoint_version;
            json members_json = json::array();
            for (const auto& a : tx.sidechain_committee_members) {
                members_json.push_back(a.str());
            }
            out["committee_members"] = members_json;
            out["timestamp"] = tx.timestamp;
            return out;
        },
        .category = "sidechain",
        .help = "createsidechainissuepayload {\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"token_symbol\":\"SYM\",\"multiplier\":n,\"fee_bps\":n,\"max_multiplier\":n,\"committee_m\":n,\"committee_members\":[\"GEO...\"],\"checkpoint_policy_id\":n?,\"checkpoint_version\":n?,\"validate\":true|false}\n\nСоздает ISSUE_SIDECHAIN. sidechain_id = genesis_hash. validate по умолчанию = true.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    // finalizesidechainissue - подставить подпись в ISSUE_SIDECHAIN
    register_method({
        .name = "finalizesidechainissue",
        .method = [this](const json& params) -> json {
            return finalize_sidechain_tx(params, core::TransactionType::ISSUE_SIDECHAIN, "finalizesidechainissue");
        },
        .category = "sidechain",
        .help = "finalizesidechainissue \"tx_hex\" \"signature_hex\"\n\nВозвращает финальный tx_hex с подписью для ISSUE_SIDECHAIN.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    register_method({
        .name = "finalizesidechainissueandsubmit",
        .method = [this](const json& params) -> json {
            json finalized = finalize_sidechain_tx(params, core::TransactionType::ISSUE_SIDECHAIN, "finalizesidechainissueandsubmit");
            const std::string tx_hex = finalized["tx_hex"].get<std::string>();
            const std::string txid = submit_raw_transaction_hex_to_mempool(
                tx_hex,
                chain_state_,
                "finalizesidechainissueandsubmit",
                true,
                true,
                chain_mutations_
            );
            finalized["txid"] = txid;
            return finalized;
        },
        .category = "sidechain",
        .help = "finalizesidechainissueandsubmit \"tx_hex\" \"signature_hex\"\n\nФинализирует ISSUE_SIDECHAIN подписью и отправляет транзакцию в mempool. Возвращает tx_hex и txid.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"},
        .mutates = true,
        .side_effects = "submits a finalized sidechain issue transaction to the local mempool"
    });

    register_method({
        .name = "createaccountingsidechainpayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(
                params,
                0,
                "Требуется объект с genesis_hash, owner_address, namespace, schema_hash и committee"
            );
            if (!p.contains("genesis_hash") || !p["genesis_hash"].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "genesis_hash обязателен (hex 32 bytes)");
            }
            const std::string sidechain_namespace = required_string_field(
                p,
                "namespace",
                "namespace обязателен"
            ).get<std::string>();
            if (sidechain_namespace.empty() ||
                sidechain_namespace.size() > consensus::params::MAX_SIDECHAIN_NAMESPACE ||
                !is_printable_ascii(sidechain_namespace)) {
                throw RPCException(RPC_INVALID_PARAMETER, "namespace имеет неверный формат");
            }
            Hash256 schema_hash{};
            const std::string schema_hex = required_string_field(
                p,
                "schema_hash",
                "schema_hash обязателен"
            ).get<std::string>();
            if (!hex_to_hash256_checked(schema_hex, schema_hash) || schema_hash == Hash256{}) {
                throw RPCException(RPC_INVALID_PARAMETER, "schema_hash должен быть ненулевым 32-байт hex");
            }

            core::Transaction tx;
            const auto common = build_sidechain_tx_common(
                p,
                tx,
                "createaccountingsidechainpayload"
            );
            if (!common.has_genesis) {
                throw RPCException(RPC_INVALID_PARAMETER, "genesis_hash обязателен");
            }
            const SidechainCommitteeFields committee = parse_sidechain_committee_from_rpc(p);
            tx.type = core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN;
            tx.sidechain_amount = 0;
            tx.sidechain_unlock_height = 0;
            tx.sidechain_checkpoint = common.genesis_hash;
            tx.sidechain_namespace = sidechain_namespace;
            tx.sidechain_schema_hash = schema_hash;
            tx.sidechain_checkpoint_policy_id =
                consensus::params::ACCOUNTING_CHECKPOINT_POLICY_ID;
            tx.sidechain_checkpoint_version =
                consensus::params::ACCOUNTING_CHECKPOINT_VERSION;
            tx.sidechain_committee_m = static_cast<uint8_t>(committee.m);
            tx.sidechain_committee_n = static_cast<uint8_t>(committee.n);
            tx.sidechain_committee_members = committee.members;
            tx.sidechain_committee_id = committee.committee_id;

            if (get_validate_flag(p, "createaccountingsidechainpayload")) {
                auto& chain_state = require_chain_state();
                if (chain_state.has_sidechain_registry_key(common.sidechain_id)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain уже существует");
                }
                state::SidechainDeposit deposit{};
                if (!chain_state.get_sidechain_deposit(common.sidechain_id, deposit)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Депозит не найден для sidechain_id");
                }
                if (deposit.owner.str() != common.owner_address) {
                    throw RPCException(RPC_INVALID_PARAMETER, "owner_address не совпадает с владельцем депозита");
                }
                if (deposit.amount < calc_sidechain_fee_rpc(0, 0)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Недостаточно депозита для комиссии регистрации");
                }
                if (chain_state.has_accounting_namespace(sidechain_namespace)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "namespace уже занят другим accounting sidechain");
                }
            }

            const bytes payload = core::build_sidechain_payload_for_signing(tx);
            json members = json::array();
            for (const auto& member : committee.members) {
                members.push_back(member.str());
            }
            return json{
                {"tx_hex", bytes_to_hex_string(tx.serialize())},
                {"payload_to_sign_hex", bytes_to_hex_string(payload)},
                {"sidechain_id", hash256_to_hex_string(common.sidechain_id)},
                {"genesis_hash", hash256_to_hex_string(common.genesis_hash)},
                {"owner_address", common.owner_address},
                {"sidechain_kind", "accounting"},
                {"namespace", sidechain_namespace},
                {"schema_hash", schema_hex},
                {"committee_m", committee.m},
                {"committee_n", committee.n},
                {"committee_id", hash256_to_hex_string(committee.committee_id)},
                {"committee_members", members},
                {"checkpoint_policy_id", tx.sidechain_checkpoint_policy_id},
                {"checkpoint_version", tx.sidechain_checkpoint_version},
                {"timestamp", tx.timestamp}
            };
        },
        .category = "sidechain",
        .help = "createaccountingsidechainpayload {\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"namespace\":\"registry\",\"schema_hash\":\"hex32\",\"committee_m\":n,\"committee_members\":[\"GEO...\"],\"validate\":true|false}\n\nСоздает REGISTER_ACCOUNTING_SIDECHAIN с checkpoint policy 3/1.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "finalizeaccountingsidechain",
        .method = [this](const json& params) -> json {
            return finalize_sidechain_tx(
                params,
                core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN,
                "finalizeaccountingsidechain"
            );
        },
        .category = "sidechain",
        .help = "finalizeaccountingsidechain \"tx_hex\" \"signature_hex\"\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    register_method({
        .name = "finalizeaccountingsidechainandsubmit",
        .method = [this](const json& params) -> json {
            json finalized = finalize_sidechain_tx(
                params,
                core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN,
                "finalizeaccountingsidechainandsubmit"
            );
            finalized["txid"] = submit_raw_transaction_hex_to_mempool(
                finalized["tx_hex"].get<std::string>(),
                chain_state_,
                "finalizeaccountingsidechainandsubmit",
                true,
                true,
                chain_mutations_
            );
            return finalized;
        },
        .category = "sidechain",
        .help = "finalizeaccountingsidechainandsubmit \"tx_hex\" \"signature_hex\"\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"},
        .mutates = true,
        .side_effects = "submits an accounting sidechain registration to the local mempool"
    });

}

} // namespace geocoin::rpc
