#include "geocoin/rpc/rpc_command_registry.hpp"

#include "geocoin/rpc/rpc_security.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace geocoin::rpc {

const char* rpc_command_profile_name(RPCCommandProfile profile) noexcept {
    switch (profile) {
        case RPCCommandProfile::FULL:
            return "full";
        case RPCCommandProfile::READ_ONLY:
            return "read-only";
        case RPCCommandProfile::MINING_ONLY:
            return "mining-only";
    }
    return "unknown";
}

bool RPCCommandRegistry::accepts(const RPCCommand& command) const {
    switch (profile_) {
        case RPCCommandProfile::FULL:
            return true;
        case RPCCommandProfile::READ_ONLY:
            return !command.mutates &&
                is_rpc_command_authorized(RPCAuthRole::READONLY, command);
        case RPCCommandProfile::MINING_ONLY:
            return command.required_role == RPCAuthRole::MINER &&
                is_rpc_command_authorized(RPCAuthRole::MINER, command);
    }
    return false;
}

bool RPCCommandRegistry::add(RPCCommand command) {
    if (!is_valid_rpc_method_name(command.name) || !command.method) {
        throw std::logic_error("RPC command must have a name and handler");
    }
    if (command.category.empty() || command.help.empty() ||
        command.stability.empty() || command.since.empty()) {
        throw std::logic_error("RPC command metadata is incomplete: " + command.name);
    }
    if (command.required_role == RPCAuthRole::NONE) {
        throw std::logic_error("RPC command cannot use the unauthenticated role: " + command.name);
    }
    if (command.mutates &&
        (command.required_role == RPCAuthRole::READONLY ||
         std::find(command.capabilities.begin(), command.capabilities.end(), "readonly") !=
             command.capabilities.end())) {
        throw std::logic_error("mutating RPC command cannot be readonly: " + command.name);
    }
    if (command.mutates && command.side_effects.empty()) {
        throw std::logic_error("mutating RPC command must describe side effects: " + command.name);
    }

    static const std::set<std::string, std::less<>> kKnownCapabilities = {
        "readonly", "wallet", "tx_submit", "tx_builder", "network_admin",
        "chain_admin", "diagnostics", "admin", "miner"
    };
    std::set<std::string, std::less<>> seen_capabilities;
    for (const auto& capability : command.capabilities) {
        if (kKnownCapabilities.count(capability) == 0 ||
            !seen_capabilities.insert(capability).second) {
            throw std::logic_error(
                "RPC command has an invalid or duplicate capability: " + command.name
            );
        }
    }
    if (!is_rpc_command_authorized(command.required_role, command)) {
        throw std::logic_error(
            "RPC command capabilities exclude its declared role: " + command.name
        );
    }
    if (command.required_role == RPCAuthRole::ADMIN &&
        (seen_capabilities.count("readonly") != 0 ||
         seen_capabilities.count("miner") != 0)) {
        throw std::logic_error(
            "admin RPC command cannot grant a lower-privilege capability: " + command.name
        );
    }
    if (command.required_role == RPCAuthRole::READONLY &&
        seen_capabilities.count("miner") != 0) {
        throw std::logic_error(
            "read-only RPC command cannot grant the miner role: " + command.name
        );
    }
    if (command.required_role == RPCAuthRole::MINER &&
        std::any_of(
            seen_capabilities.begin(),
            seen_capabilities.end(),
            [](const std::string& capability) {
                return capability != "miner" && capability != "readonly";
            })) {
        throw std::logic_error(
            "miner RPC command has a cross-role capability: " + command.name
        );
    }

    std::set<std::string, std::less<>> seen_params;
    for (const auto& param : command.param_names) {
        const bool valid = !param.empty() && param.size() <= 64 &&
            std::all_of(param.begin(), param.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
            });
        if (!valid || !seen_params.insert(param).second) {
            throw std::logic_error(
                "RPC command has an invalid or duplicate parameter name: " + command.name
            );
        }
    }
    if (!accepts(command)) {
        return false;
    }

    const std::string name = command.name;
    if (!commands_.emplace(name, std::move(command)).second) {
        throw std::logic_error("duplicate RPC method registration: " + name);
    }
    return true;
}

const RPCCommand* RPCCommandRegistry::find(std::string_view name) const noexcept {
    const auto it = commands_.find(name);
    return it == commands_.end() ? nullptr : &it->second;
}

std::string RPCCommandRegistry::help(
    const std::string& method,
    std::optional<RPCAuthRole> viewer
) const {
    const auto visible = [viewer](const RPCCommand& command) {
        if (!viewer.has_value() || *viewer == RPCAuthRole::ADMIN) {
            return true;
        }
        return is_rpc_command_authorized(*viewer, command);
    };

    if (!method.empty()) {
        const RPCCommand* command = find(method);
        if (!command || !visible(*command)) {
            return "Method not found: " + method;
        }
        return command->help;
    }

    std::ostringstream oss;
    std::vector<std::string> categories;
    std::set<std::string> seen_categories;
    for (const auto& [name, command] : commands_) {
        (void)name;
        if (!visible(command)) {
            continue;
        }
        if (seen_categories.insert(command.category).second) {
            categories.push_back(command.category);
        }
    }

    const std::vector<std::string> preferred_order = {
        "blockchain", "mempool", "network", "mining", "nft", "sidechain",
        "multisig", "txbuilder", "wallet", "geocoin", "util", "control"
    };
    std::vector<std::string> ordered_categories;
    for (const auto& category : preferred_order) {
        if (seen_categories.count(category) > 0) {
            ordered_categories.push_back(category);
        }
    }
    for (const auto& category : categories) {
        if (std::find(ordered_categories.begin(), ordered_categories.end(), category) ==
            ordered_categories.end()) {
            ordered_categories.push_back(category);
        }
    }

    for (size_t index = 0; index < ordered_categories.size(); ++index) {
        const auto& category = ordered_categories[index];
        if (index > 0) {
            oss << "\n";
        }
        std::string title = category.empty() ? "uncategorized" : category;
        if (!title.empty()) {
            title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
        }
        oss << "== " << title << " ==\n";
        for (const auto& [name, command] : commands_) {
            if (command.category != category || !visible(command)) {
                continue;
            }
            oss << name << " [role=" << rpc_role_name(command.required_role);
            const auto capabilities = rpc_command_capabilities(command);
            if (!capabilities.empty()) {
                oss << ", caps=";
                for (size_t capability_index = 0;
                     capability_index < capabilities.size();
                     ++capability_index) {
                    if (capability_index > 0) {
                        oss << "|";
                    }
                    oss << capabilities[capability_index];
                }
            }
            if (command.requires_wallet) {
                oss << ", wallet";
            }
            if (command.mutates) {
                oss << ", mutates";
            }
            if (!command.deprecation.empty()) {
                oss << ", deprecated";
            }
            oss << "]\n";
        }
    }
    return oss.str();
}

}  // namespace geocoin::rpc
