#include "geocoin/rpc/rpc_server.hpp"

namespace geocoin::rpc {

void RPCServer::register_builtin_modules() {
    register_blockchain_methods();
    register_wallet_methods();
    register_mempool_methods();
    register_mining_methods();
    register_network_methods();
    register_misc_methods();
}

}  // namespace geocoin::rpc
