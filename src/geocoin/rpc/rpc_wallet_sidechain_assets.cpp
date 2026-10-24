#include "geocoin/rpc/rpc_wallet_helpers.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_asset_methods() {
    register_wallet_sidechain_asset_issue_methods();
    register_wallet_sidechain_asset_mint_methods();
    register_wallet_sidechain_asset_redeem_methods();
    register_wallet_sidechain_asset_slash_methods();
}

} // namespace geocoin::rpc
