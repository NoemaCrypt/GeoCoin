#include "geocoin/rpc/rpc_server.hpp"

namespace geocoin::rpc {

void RPCServer::register_blockchain_methods() {
    register_blockchain_core_methods();
    register_blockchain_availability_methods();
    register_blockchain_sidechain_methods();
    register_blockchain_block_methods();
    register_blockchain_pruning_methods();
    register_blockchain_transaction_methods();
}

} // namespace geocoin::rpc
