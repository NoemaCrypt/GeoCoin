#pragma once

#include "geocoin/rpc/rpc_types.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace geocoin::rpc {

/** Limits which command handlers exist on a particular RPC listener. */
enum class RPCCommandProfile {
    FULL,
    READ_ONLY,
    MINING_ONLY,
};

const char* rpc_command_profile_name(RPCCommandProfile profile) noexcept;

/**
 * Immutable-at-runtime command catalog used by the request dispatcher.
 *
 * Commands rejected by the selected profile are never retained, reducing the
 * callable surface of specialised listeners independently of authentication.
 */
class RPCCommandRegistry final {
public:
    using CommandMap = std::map<std::string, RPCCommand, std::less<>>;

    explicit RPCCommandRegistry(RPCCommandProfile profile = RPCCommandProfile::FULL)
        : profile_(profile) {}

    /** Validate and add a command. Returns false when its profile excludes it. */
    bool add(RPCCommand command);

    const RPCCommand* find(std::string_view name) const noexcept;
    const CommandMap& commands() const noexcept { return commands_; }
    size_t size() const noexcept { return commands_.size(); }
    RPCCommandProfile profile() const noexcept { return profile_; }
    std::string help(
        const std::string& method = "",
        std::optional<RPCAuthRole> viewer = std::nullopt
    ) const;

private:
    bool accepts(const RPCCommand& command) const;

    RPCCommandProfile profile_;
    CommandMap commands_;
};

}  // namespace geocoin::rpc
