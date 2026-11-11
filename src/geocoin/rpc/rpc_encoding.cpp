#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/utils/bytes.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace geocoin::rpc {

bool hex_to_bytes_checked(const std::string& hex, bytes& out) {
    return utils::hex_to_bytes_checked(hex, out);
}

bytes parse_hex_bytes_or_throw(const std::string& hex, const std::string& error_message) {
    bytes out;
    if (!hex_to_bytes_checked(hex, out)) {
        throw RPCException(RPC_INVALID_PARAMETER, error_message);
    }
    return out;
}

std::string bytes_to_hex_string(const bytes& data) {
    return utils::bytes_to_hex(data);
}

std::string hash256_to_hex_string(const Hash256& h) {
    return utils::hash256_to_hex(h);
}

std::string uint256_to_hex_string(const uint256& value) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 3; i >= 0; --i) {
        oss << std::setw(16) << value.data[i];
    }
    return oss.str();
}

bool hex_to_hash256_checked(const std::string& hex, Hash256& out) {
    return utils::hex_to_hash256_checked(hex, out);
}

} // namespace geocoin::rpc
