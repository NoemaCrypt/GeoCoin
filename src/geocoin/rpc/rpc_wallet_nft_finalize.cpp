#include "geocoin/rpc/rpc_wallet_helpers.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_nft_finalize_methods() {

    // finalize... - подставить подпись payload в raw tx
    register_method({
        .name = "finalizenftcreate",
        .method = [this](const json& params) -> json {
            return finalize_nft_transaction_signature(
                params,
                core::TransactionType::NFT_CREATE,
                "NFT_CREATE"
            );
        },
        .category = "nft",
        .help = "finalizenftcreate \"tx_hex\" \"signature_hex\"\n\nВозвращает финальный tx_hex с подписью payload.\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    register_method({
        .name = "finalizenfttransfer",
        .method = [this](const json& params) -> json {
            return finalize_nft_transaction_signature(
                params,
                core::TransactionType::NFT_TRANSFER,
                "NFT_TRANSFER"
            );
        },
        .category = "nft",
        .help = "finalizenfttransfer \"tx_hex\" \"signature_hex\"\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

    register_method({
        .name = "finalizenftburn",
        .method = [this](const json& params) -> json {
            return finalize_nft_transaction_signature(
                params,
                core::TransactionType::NFT_BURN,
                "NFT_BURN"
            );
        },
        .category = "nft",
        .help = "finalizenftburn \"tx_hex\" \"signature_hex\"\n",
        .requires_wallet = false,
        .param_names = {"tx_hex", "signature_hex"}
    });

}

} // namespace geocoin::rpc
