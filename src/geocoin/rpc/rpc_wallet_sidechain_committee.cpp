#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_committee_methods() {

    // createsidechaincommitteeupdatepayload - обновить комитет сайдчейна
    register_method({
        .name = "createsidechaincommitteeupdatepayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект с полями sidechain_id/genesis_hash, owner_address, committee_m, committee_members");

            if (!p.contains("committee_m")) {
                throw RPCException(RPC_INVALID_PARAMETER, "committee_m обязателен");
            }
            if (!p.contains("committee_members") || !p["committee_members"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "committee_members обязателен (array адресов)");
            }

            core::Transaction tx;
            const auto common = build_sidechain_tx_common(p, tx, "createsidechaincommitteeupdatepayload");

            const SidechainCommitteeFields committee = parse_sidechain_committee_from_rpc(p);

            tx.type = core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE;
            tx.sidechain_amount = 0;
            tx.sidechain_unlock_height = 0;
            tx.sidechain_checkpoint = Hash256{};
            tx.sidechain_committee_m = static_cast<uint8_t>(committee.m);
            tx.sidechain_committee_n = static_cast<uint8_t>(committee.n);
            tx.sidechain_committee_members = committee.members;
            tx.sidechain_committee_id = committee.committee_id;

            const bool validate = get_validate_flag(p, "createsidechaincommitteeupdatepayload");
            if (validate) {
                auto& chain_state = require_chain_state();
                state::SidechainRegistryEntry entry{};
                if (!chain_state.get_sidechain_registry(common.sidechain_id, entry)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain не найден");
                }
                if (entry.status != state::SidechainStatus::ACTIVE) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Sidechain находится в процессе decommission");
                }
                if (entry.owner.str() != common.owner_address) {
                    throw RPCException(RPC_INVALID_PARAMETER, "owner_address не совпадает с владельцем сайдчейна");
                }
            }

            bytes payload = core::build_sidechain_committee_update_payload_for_signing(tx);

            json out;
            out["tx_hex"] = bytes_to_hex_string(tx.serialize());
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload);
            out["sidechain_id"] = hash256_to_hex_string(common.sidechain_id);
            if (common.has_genesis) {
                out["genesis_hash"] = hash256_to_hex_string(common.genesis_hash);
            }
            out["owner_address"] = common.owner_address;
            out["committee_m"] = committee.m;
            out["committee_n"] = committee.n;
            out["committee_id"] = hash256_to_hex_string(tx.sidechain_committee_id);
            json members_json = json::array();
            for (const auto& a : tx.sidechain_committee_members) {
                members_json.push_back(a.str());
            }
            out["committee_members"] = members_json;
            out["timestamp"] = tx.timestamp;
            return out;
        },
        .category = "sidechain",
        .help = "createsidechaincommitteeupdatepayload {\"sidechain_id\":\"hex32\"|\"genesis_hash\":\"hex32\",\"owner_address\":\"GEO...\",\"committee_m\":n,\"committee_members\":[\"GEO...\"],\"validate\":true|false}\n\nСоздает UPDATE_SIDECHAIN_COMMITTEE и возвращает payload для подписи комитета.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    // finalizesidechaincommitteeupdate - подставить подписи комитета
    register_method({
        .name = "finalizesidechaincommitteeupdate",
        .method = [this](const json& params) -> json {
            return finalize_committee_signed_transaction(
                params,
                core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE,
                "Неверная hex-строка транзакции",
                "tx_hex не является UPDATE_SIDECHAIN_COMMITTEE"
            );
        },
        .category = "sidechain",
        .help = "finalizesidechaincommitteeupdate \"tx_hex\" [\"sig1_hex\",\"sig2_hex\"]\n\nВозвращает финальный tx_hex с подписями комитета.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    register_method({
        .name = "finalizesidechaincommitteeupdateandsubmit",
        .method = [this](const json& params) -> json {
            json finalized = finalize_committee_signed_transaction(
                params,
                core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE,
                "finalizesidechaincommitteeupdateandsubmit: неверный tx_hex",
                "finalizesidechaincommitteeupdateandsubmit: tx имеет неверный тип"
            );
            const std::string tx_hex = finalized["tx_hex"].get<std::string>();
            const std::string txid = submit_raw_transaction_hex_to_mempool(
                tx_hex,
                chain_state_,
                "finalizesidechaincommitteeupdateandsubmit",
                true,
                true,
                chain_mutations_
            );
            finalized["txid"] = txid;
            return finalized;
        },
        .category = "sidechain",
        .help = "finalizesidechaincommitteeupdateandsubmit \"tx_hex\" [\"sig1_hex\",\"sig2_hex\"]\n\nФинализирует UPDATE_SIDECHAIN_COMMITTEE подписями и отправляет в mempool. Возвращает tx_hex и txid.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"},
        .mutates = true,
        .side_effects = "submits a finalized sidechain committee update transaction to the local mempool"
    });

}
} // namespace geocoin::rpc
