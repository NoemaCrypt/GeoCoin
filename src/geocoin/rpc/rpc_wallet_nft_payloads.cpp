#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "rpc_nft_metadata.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_nft_payload_methods() {

    // createnftcreatepayload - получить payload bytes для подписи на мобиле
    register_method({
        .name = "createnftcreatepayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект metadata + creator_address");
            if (!p.contains("metadata") || !p["metadata"].is_object()) {
                throw RPCException(RPC_INVALID_PARAMETER, "metadata обязателен");
            }
            if (!p.contains("creator_address")) {
                throw RPCException(RPC_INVALID_PARAMETER, "creator_address обязателен");
            }

            const Address creator(required_string_field(p, "creator_address", "creator_address is required").get<std::string>());
            if (!crypto::verify_address(creator)) {
                throw RPCException(RPC_INVALID_PARAMETER, "creator_address is invalid");
            }
            const auto meta = parse_nft_metadata_from_rpc(p["metadata"], nft::NFTMetadata{});

            nft::NFTCreationTransaction txp;
            txp.metadata = meta;
            txp.nft_id = meta.calculate_hash();
            txp.creator_address = creator;
            txp.royalty_percent = optional_u32_field(p, "royalty_percent", 0, "royalty_percent вне диапазона");
            if (txp.royalty_percent > 10000) {
                throw RPCException(RPC_INVALID_PARAMETER, "royalty_percent must be in range 0..10000");
            }
            txp.signature.clear();

            const bytes payload_to_sign = txp.serialize_for_signing();
            json out;
            out["nft_id"] = hash256_to_hex_string(txp.nft_id);
            out["creator_address"] = txp.creator_address.str();
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload_to_sign);
            return out;
        },
        .category = "nft",
        .help = "createnftcreatepayload {metadata:{...},creator_address:""GEO..."",royalty_percent:n}\n\nВозвращает payload bytes (hex) для подписи на клиенте.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "createnfttransferpayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект {nft_id,from,to,fee}");
            Hash256 nft_id;
            if (!p.contains("nft_id") || !hex_to_hash256_checked(p["nft_id"].get<std::string>(), nft_id)) {
                throw RPCException(RPC_INVALID_PARAMETER, "nft_id обязателен (hex)");
            }
            nft::NFTTransferTransaction txp;
            txp.nft_id = nft_id;
            txp.from = Address(p.value("from", std::string("")));
            txp.to = Address(p.value("to", std::string("")));
            txp.fee = optional_amount_field(p, "fee", 0, "fee вне диапазона");
            txp.signature.clear();

            const bytes payload_to_sign = txp.serialize_for_signing();
            json out;
            out["nft_id"] = hash256_to_hex_string(txp.nft_id);
            out["from"] = txp.from.str();
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload_to_sign);
            return out;
        },
        .category = "nft",
        .help = "createnfttransferpayload {nft_id:"".."",from:""GEO.."",to:""GEO.."",fee:n}\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    register_method({
        .name = "createnftburnpayload",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(params, 0, "Требуется объект {nft_id,owner,fee}");
            Hash256 nft_id;
            if (!p.contains("nft_id") || !hex_to_hash256_checked(p["nft_id"].get<std::string>(), nft_id)) {
                throw RPCException(RPC_INVALID_PARAMETER, "nft_id обязателен (hex)");
            }
            nft::NFTBurnTransaction txp;
            txp.nft_id = nft_id;
            txp.owner = Address(p.value("owner", std::string("")));
            txp.fee = optional_amount_field(p, "fee", 0, "fee вне диапазона");
            txp.signature.clear();

            const bytes payload_to_sign = txp.serialize_for_signing();
            json out;
            out["nft_id"] = hash256_to_hex_string(txp.nft_id);
            out["owner"] = txp.owner.str();
            out["payload_to_sign_hex"] = bytes_to_hex_string(payload_to_sign);
            return out;
        },
        .category = "nft",
        .help = "createnftburnpayload {nft_id:"".."",owner:""GEO.."",fee:n}\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

}

} // namespace geocoin::rpc
