#pragma once

#include "geocoin/rpc/rpc_types.hpp"

namespace geocoin::rpc {

bool hex_to_bytes_checked(const std::string& hex, bytes& out);
bytes parse_hex_bytes_or_throw(const std::string& hex, const std::string& error_message);
std::string bytes_to_hex_string(const bytes& data);
std::string hash256_to_hex_string(const Hash256& h);
std::string uint256_to_hex_string(const uint256& value);
bool hex_to_hash256_checked(const std::string& hex, Hash256& out);

} // namespace geocoin::rpc
