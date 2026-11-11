#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/consensus/sidechain_rules.hpp"

namespace geocoin::rpc {

bool SyncDebugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}

bool is_printable_ascii(const std::string& value) {
    return consensus::is_printable_ascii(value);
}

} // namespace geocoin::rpc
