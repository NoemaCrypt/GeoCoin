#include "geocoin/rpc/rpc_wallet_helpers.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_methods() {
    register_wallet_basic_methods();
    register_wallet_nft_methods();
    register_wallet_sidechain_methods();
    register_wallet_multisig_methods();
}

void RPCServer::register_wallet_basic_methods() {
    register_wallet_basic_account_methods();
    register_wallet_basic_utxo_methods();
    register_wallet_basic_transaction_methods();
    register_wallet_txbuilder_methods();
}

} // namespace geocoin::rpc
