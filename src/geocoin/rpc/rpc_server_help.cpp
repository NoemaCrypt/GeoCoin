#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_security.hpp"

namespace geocoin::rpc {

std::string RPCServer::get_help(const std::string& method) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return command_registry_.help(method, rpc_request_auth_role());
}


} // namespace geocoin::rpc
