#include "geocoin/rpc/rpc_dispatch.hpp"
#include "geocoin/rpc/rpc_protocol.hpp"

#include <exception>
#include <new>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string payload(reinterpret_cast<const char*>(data), size);
    std::string user, password;
    (void)geocoin::rpc::parse_basic_auth(payload, user, password);
    try {
        (void)geocoin::rpc::parse_json_rpc_payload(payload);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
    }
    return 0;
}
