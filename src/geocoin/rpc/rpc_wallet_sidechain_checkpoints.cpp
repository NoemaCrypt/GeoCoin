#include "geocoin/rpc/rpc_wallet_helpers.hpp"

namespace geocoin::rpc {

void RPCServer::register_wallet_sidechain_checkpoint_methods() {
    register_wallet_sidechain_checkpoint_single_methods();
    register_wallet_sidechain_checkpoint_proof_methods();
    register_wallet_sidechain_checkpoint_batch_methods();
}

} // namespace geocoin::rpc
