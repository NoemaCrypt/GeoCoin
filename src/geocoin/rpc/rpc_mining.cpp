#include "geocoin/rpc/rpc_server.hpp"

namespace geocoin::rpc {

void RPCServer::register_mining_methods() {
    register_mining_info_methods();
    register_mining_control_methods();
    register_mining_template_methods();
}

} // namespace geocoin::rpc
