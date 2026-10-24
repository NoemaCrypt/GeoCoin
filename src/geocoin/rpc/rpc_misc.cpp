#include "geocoin/rpc/rpc_server.hpp"

namespace geocoin::rpc {

void RPCServer::register_misc_methods() {
    register_misc_control_methods();
    register_misc_diagnostics_methods();
    register_misc_signing_methods();
}

} // namespace geocoin::rpc
