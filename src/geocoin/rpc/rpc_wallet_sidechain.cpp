#include "geocoin/rpc/rpc_wallet_helpers.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_methods() {
    register_wallet_sidechain_asset_methods();
    register_wallet_sidechain_deposit_methods();
    register_wallet_sidechain_checkpoint_methods();
    register_wallet_sidechain_committee_methods();
    register_wallet_sidechain_decommission_methods();
}

} // namespace geocoin::rpc
