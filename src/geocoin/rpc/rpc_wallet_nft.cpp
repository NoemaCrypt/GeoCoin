#include "geocoin/rpc/rpc_wallet_helpers.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_nft_methods() {
    register_wallet_nft_query_methods();
    register_wallet_nft_payload_methods();
    register_wallet_nft_finalize_methods();
    register_wallet_nft_transaction_methods();
}

} // namespace geocoin::rpc
