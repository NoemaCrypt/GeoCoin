#include "geocoin/rpc/rpc_wallet_helpers.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_multisig_methods() {
    register_wallet_multisig_package_methods();
    register_wallet_multisig_delegation_methods();
    register_wallet_multisig_legacy_methods();
}

} // namespace geocoin::rpc
