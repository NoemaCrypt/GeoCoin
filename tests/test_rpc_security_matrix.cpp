#include "geocoin/rpc/rpc_dispatch.hpp"
#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_sensitive_json.hpp"
#include "rpc_test_utils.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace geocoin::rpc {

struct RPCServerTestAccess {
    static const RPCCommandRegistry& registry(const RPCServer& server) {
        return server.command_registry_;
    }

    static json handle_call(RPCServer& server, const json& request, RPCAuthRole role) {
        return server.handle_rpc_call(request, role);
    }
};

} // namespace geocoin::rpc

namespace {

using geocoin::rpc::json;
using geocoin::rpc::RPCAuthRole;
using geocoin::rpc::RPCCommand;
using geocoin::rpc::RPCCommandProfile;
using geocoin::rpc::RPCCommandRegistry;
using geocoin::rpc::RPCException;
using geocoin::rpc::RPCServer;
using geocoin::rpc::RPCServerTestAccess;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int response_error_code(const json& response) {
    expect(response.is_object(), "RPC response must be an object");
    expect(response.contains("error") && response["error"].is_object(),
           "RPC response must contain an error object");
    return response["error"]["code"].get<int>();
}

template <typename Fn>
void expect_logic_error(Fn&& fn, const std::string& message) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    expect(rejected, message);
}

template <typename Fn>
void expect_rpc_error(geocoin::rpc::RPCErrorCode code,
                      Fn&& fn,
                      const std::string& message) {
    bool rejected = false;
    try {
        fn();
    } catch (const RPCException& error) {
        rejected = error.code() == code;
    }
    expect(rejected, message);
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value)
        : name_(name) {
        if (const char* old = std::getenv(name_.c_str())) {
            had_old_ = true;
            old_ = old;
        }
#ifdef _WIN32
        _putenv_s(name_.c_str(), value);
#else
        setenv(name_.c_str(), value, 1);
#endif
    }

    ~ScopedEnvVar() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), had_old_ ? old_.c_str() : "");
#else
        if (had_old_) {
            setenv(name_.c_str(), old_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

private:
    std::string name_;
    std::string old_;
    bool had_old_ = false;
};

json request_for(const std::string& method, int id = 1) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", json::array()}
    };
}

void test_complete_catalog_role_and_profile_matrix() {
    RPCServer full(RPCCommandProfile::FULL);
    RPCServer readonly(RPCCommandProfile::READ_ONLY);
    RPCServer mining(RPCCommandProfile::MINING_ONLY);

    const auto& full_registry = RPCServerTestAccess::registry(full);
    const auto& readonly_registry = RPCServerTestAccess::registry(readonly);
    const auto& mining_registry = RPCServerTestAccess::registry(mining);
    const auto& commands = full_registry.commands();

    expect(commands.size() == 142,
           "full RPC catalog changed without updating the audited role matrix");
    for (const auto* name : {"analyzegeometry", "getnftgeojson", "listblockgeography"}) {
        const auto& command = commands.at(name);
        expect(command.required_role == RPCAuthRole::READONLY && !command.requires_wallet &&
                   !command.mutates && readonly_registry.commands().count(name) == 1,
               std::string("geographic query must remain read-only: ") + name);
    }
    expect(!readonly_registry.commands().empty(), "read-only RPC profile must not be empty");
    expect(!mining_registry.commands().empty(), "mining RPC profile must not be empty");

    const std::array<RPCAuthRole, 4> all_roles = {
        RPCAuthRole::NONE,
        RPCAuthRole::READONLY,
        RPCAuthRole::ADMIN,
        RPCAuthRole::MINER
    };
    std::set<std::string> known_capabilities;
    for (const RPCAuthRole role : all_roles) {
        const auto capabilities = geocoin::rpc::rpc_role_capabilities(role);
        known_capabilities.insert(capabilities.begin(), capabilities.end());
    }

    size_t mutating_count = 0;
    size_t wallet_guard_count = 0;
    size_t denied_dispatches = 0;
    size_t reviewed_cross_role_commands = 0;

    for (const auto& [name, command] : commands) {
        expect(name == command.name, "registry key/name mismatch for " + name);
        expect(geocoin::rpc::is_valid_rpc_method_name(name),
               "invalid RPC method name reached the catalog: " + name);
        expect(static_cast<bool>(command.method), "missing handler for " + name);
        expect(!command.category.empty() && !command.help.empty() &&
                   !command.stability.empty() && !command.since.empty(),
               "incomplete metadata for " + name);
        expect(command.required_role != RPCAuthRole::NONE,
               "unauthenticated command reached the catalog: " + name);
        expect(full_registry.help(name) == command.help,
               "per-command help lookup mismatch for " + name);

        const auto capabilities = geocoin::rpc::rpc_command_capabilities(command);
        expect(!capabilities.empty(), "command has no effective capability: " + name);
        std::set<std::string> unique_capabilities;
        for (const auto& capability : capabilities) {
            expect(known_capabilities.count(capability) == 1,
                   "unknown capability " + capability + " on " + name);
            expect(unique_capabilities.insert(capability).second,
                   "duplicate effective capability on " + name);
        }

        expect(!geocoin::rpc::is_rpc_command_authorized(RPCAuthRole::NONE, command),
               "NONE role can authorize " + name);
        expect(geocoin::rpc::is_rpc_command_authorized(command.required_role, command),
               "declared role cannot authorize its own command: " + name);
        if (command.required_role == RPCAuthRole::ADMIN) {
            expect(!geocoin::rpc::is_rpc_command_authorized(RPCAuthRole::READONLY, command) &&
                       !geocoin::rpc::is_rpc_command_authorized(RPCAuthRole::MINER, command),
                   "admin command grants a lower-privilege role: " + name);
        }
        if (command.required_role == RPCAuthRole::MINER &&
            geocoin::rpc::is_rpc_command_authorized(RPCAuthRole::READONLY, command)) {
            expect(name == "getmempoolinfo" && !command.mutates &&
                       capabilities == std::vector<std::string>({"readonly", "miner"}),
                   "unreviewed miner/read-only role bridge: " + name);
            ++reviewed_cross_role_commands;
        }

        if (command.mutates) {
            ++mutating_count;
            expect(!command.side_effects.empty(),
                   "mutating command has no side-effect declaration: " + name);
            expect(!geocoin::rpc::is_rpc_command_authorized(RPCAuthRole::READONLY, command),
                   "read-only role can call mutating command: " + name);
        }

        const bool expected_readonly_profile =
            !command.mutates &&
            geocoin::rpc::is_rpc_command_authorized(RPCAuthRole::READONLY, command);
        const bool expected_mining_profile =
            command.required_role == RPCAuthRole::MINER &&
            geocoin::rpc::is_rpc_command_authorized(RPCAuthRole::MINER, command);
        expect((readonly_registry.find(name) != nullptr) == expected_readonly_profile,
               "read-only physical profile mismatch for " + name);
        expect((mining_registry.find(name) != nullptr) == expected_mining_profile,
               "mining physical profile mismatch for " + name);

        const json request = request_for(name);
        for (const RPCAuthRole role : all_roles) {
            const bool allowed = geocoin::rpc::is_rpc_command_authorized(role, command);
            if (!allowed) {
                const json response = RPCServerTestAccess::handle_call(full, request, role);
                expect(response_error_code(response) ==
                           geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE,
                       "dispatch authorization mismatch for method " + name +
                           " and role " + geocoin::rpc::rpc_role_name(role));
                ++denied_dispatches;
            } else if (command.requires_wallet) {
                const json response = RPCServerTestAccess::handle_call(full, request, role);
                expect(response_error_code(response) == geocoin::rpc::RPC_WALLET_ERROR,
                       "wallet guard did not run before handler for " + name);
                ++wallet_guard_count;
            }
        }
    }

    for (const auto& [name, command] : readonly_registry.commands()) {
        (void)command;
        expect(commands.count(name) == 1, "read-only profile invented command " + name);
    }
    for (const auto& [name, command] : mining_registry.commands()) {
        (void)command;
        expect(commands.count(name) == 1, "mining profile invented command " + name);
    }

    expect(mutating_count > 20, "mutating RPC matrix did not cover the expected surface");
    expect(wallet_guard_count > 0, "wallet precondition matrix did not execute");
    expect(denied_dispatches > commands.size(),
           "role-denial matrix did not cover every command broadly enough");
    expect(reviewed_cross_role_commands == 1,
           "reviewed cross-role command manifest changed");

    std::cout << "RPC catalog matrix: full=" << commands.size()
              << " readonly=" << readonly_registry.size()
              << " mining=" << mining_registry.size()
              << " mutating=" << mutating_count
              << " denied_dispatches=" << denied_dispatches << "\n";
}

RPCCommand valid_unit_command() {
    RPCCommand command;
    command.name = "unit.guard";
    command.method = [](const json& params) { return params; };
    command.category = "unit";
    command.help = "unit.guard";
    command.required_role = RPCAuthRole::ADMIN;
    command.param_names = {"value"};
    command.capabilities = {"admin"};
    return command;
}

void test_registry_fail_closed_invariants() {
    auto reject = [](RPCCommand command) {
        RPCCommandRegistry registry;
        registry.add(std::move(command));
    };

    RPCCommand command = valid_unit_command();
    command.required_role = RPCAuthRole::NONE;
    expect_logic_error([&] { reject(command); }, "registry accepted NONE role");

    command = valid_unit_command();
    command.name = "unit bad";
    expect_logic_error([&] { reject(command); }, "registry accepted ambiguous method name");

    command = valid_unit_command();
    command.name = std::string(geocoin::rpc::MAX_RPC_METHOD_NAME_BYTES + 1, 'a');
    expect_logic_error([&] { reject(command); }, "registry accepted overlong method name");

    command = valid_unit_command();
    command.help.clear();
    expect_logic_error([&] { reject(command); }, "registry accepted incomplete metadata");

    command = valid_unit_command();
    command.capabilities = {"admin", "admin"};
    expect_logic_error([&] { reject(command); }, "registry accepted duplicate capabilities");

    command = valid_unit_command();
    command.capabilities = {"future_typo"};
    expect_logic_error([&] { reject(command); }, "registry accepted unknown capability");

    command = valid_unit_command();
    command.capabilities = {"admin", "readonly"};
    expect_logic_error([&] { reject(command); },
                       "registry accepted a readonly capability on an admin command");

    command = valid_unit_command();
    command.required_role = RPCAuthRole::MINER;
    command.capabilities = {"readonly"};
    expect_logic_error([&] { reject(command); },
                       "registry accepted capabilities excluding the declared role");

    command = valid_unit_command();
    command.required_role = RPCAuthRole::MINER;
    command.capabilities = {"miner", "admin"};
    expect_logic_error([&] { reject(command); },
                       "registry accepted an admin capability on a miner command");

    command = valid_unit_command();
    command.required_role = RPCAuthRole::READONLY;
    command.capabilities = {"readonly", "miner"};
    expect_logic_error([&] { reject(command); },
                       "registry accepted a miner capability on a read-only command");

    command = valid_unit_command();
    command.param_names = {"value", "value"};
    expect_logic_error([&] { reject(command); }, "registry accepted duplicate param names");

    command = valid_unit_command();
    command.mutates = true;
    command.side_effects.clear();
    expect_logic_error([&] { reject(command); },
                       "registry accepted an undocumented mutating command");

    command = valid_unit_command();
    command.required_role = RPCAuthRole::READONLY;
    command.capabilities = {"readonly"};
    command.mutates = true;
    command.side_effects = "changes state";
    expect_logic_error([&] { reject(command); }, "registry accepted read-only mutation");

    RPCCommandRegistry duplicate_registry;
    command = valid_unit_command();
    expect(duplicate_registry.add(command), "valid unit command was not registered");
    expect_logic_error([&] { duplicate_registry.add(command); },
                       "registry accepted duplicate method registration");

    RPCCommand unknown_category = valid_unit_command();
    unknown_category.capabilities.clear();
    unknown_category.category = "new_category_typo";
    const auto fallback = geocoin::rpc::rpc_command_capabilities(unknown_category);
    expect(fallback == std::vector<std::string>{"admin"} &&
               !geocoin::rpc::is_rpc_command_authorized(
                   RPCAuthRole::READONLY, unknown_category),
           "unknown category did not fail closed to the declared admin role");
}

void test_dispatch_and_strict_json_boundaries() {
    std::atomic<unsigned> invocations{0};
    RPCCommand command = valid_unit_command();
    command.method = [&invocations](const json& params) {
        invocations.fetch_add(1, std::memory_order_relaxed);
        return params;
    };
    RPCCommandRegistry registry;
    expect(registry.add(command), "dispatch test command registration failed");

    const auto dispatch = [&](json request, RPCAuthRole role = RPCAuthRole::ADMIN) {
        return geocoin::rpc::dispatch_json_rpc_request(registry, request, role, false);
    };

    expect(response_error_code(dispatch({
               {"jsonrpc", "1.0"}, {"id", 1}, {"method", "unit.guard"}
           })) == geocoin::rpc::RPC_INVALID_REQUEST,
           "dispatch accepted an explicit non-2.0 protocol version");
    expect(response_error_code(dispatch({
               {"jsonrpc", "2.0"}, {"id", true}, {"method", "unit.guard"}
           })) == geocoin::rpc::RPC_INVALID_REQUEST,
           "dispatch accepted boolean id");
    expect(response_error_code(dispatch({
               {"jsonrpc", "2.0"}, {"id", json::object()}, {"method", "unit.guard"}
           })) == geocoin::rpc::RPC_INVALID_REQUEST,
           "dispatch accepted object id");
    expect(response_error_code(dispatch({
               {"jsonrpc", "2.0"}, {"id", std::string(257, 'i')},
               {"method", "unit.guard"}
           })) == geocoin::rpc::RPC_INVALID_REQUEST,
           "dispatch accepted overlong reflected id");
    expect(response_error_code(dispatch({
               {"jsonrpc", "2.0"}, {"id", 2}, {"method", "unit.guard\nstop"}
           })) == geocoin::rpc::RPC_INVALID_REQUEST,
           "dispatch accepted a control character in method name");
    expect(response_error_code(dispatch({
               {"jsonrpc", "2.0"}, {"id", 3}, {"method", "unit.guard"},
               {"params", json{{"value", 1}, {"extra", 2}}}
           })) == geocoin::rpc::RPC_INVALID_PARAMS,
           "dispatch accepted excess named parameters");

    const std::string hostile_key(200, 'x');
    const json unknown_param_response = dispatch({
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "unit.guard"},
        {"params", json{{hostile_key, 1}}}
    });
    expect(response_error_code(unknown_param_response) == geocoin::rpc::RPC_INVALID_PARAMS &&
               unknown_param_response.dump().find(hostile_key) == std::string::npos,
           "unknown named parameter was reflected into the error response");

    expect(response_error_code(dispatch(request_for("unit.guard"), RPCAuthRole::READONLY)) ==
               geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE,
           "read-only role crossed an admin dispatch boundary");
    expect(invocations.load(std::memory_order_relaxed) == 0,
           "invalid or unauthorized requests reached the handler");

    const json valid_response = dispatch({
        {"jsonrpc", "2.0"},
        {"id", "request-1"},
        {"method", "unit.guard"},
        {"params", json{{"value", 7}}}
    });
    expect(!valid_response.contains("error") && valid_response["result"] == json::array({7}) &&
               invocations.load(std::memory_order_relaxed) == 1,
           "valid named request did not reach the handler exactly once");

    expect_rpc_error(geocoin::rpc::RPC_INVALID_REQUEST, [] {
        (void)geocoin::rpc::parse_json_rpc_payload(
            R"({"jsonrpc":"2.0","method":"unit.guard","method":"stop"})"
        );
    }, "strict parser accepted duplicate top-level key");
    expect_rpc_error(geocoin::rpc::RPC_INVALID_REQUEST, [] {
        (void)geocoin::rpc::parse_json_rpc_payload(
            R"({"jsonrpc":"2.0","method":"unit.guard","params":{"value":1,"value":2}})"
        );
    }, "strict parser accepted duplicate nested key");

    std::string deep = "[";
    deep.append(80, '[');
    deep += "0";
    deep.append(81, ']');
    expect_rpc_error(geocoin::rpc::RPC_INVALID_REQUEST, [&] {
        (void)geocoin::rpc::parse_json_rpc_payload(deep);
    }, "strict parser accepted excessive nesting");

    std::string complex = "[";
    for (size_t i = 0; i < 50'100; ++i) {
        if (i > 0) {
            complex.push_back(',');
        }
        complex.push_back('0');
    }
    complex.push_back(']');
    expect_rpc_error(geocoin::rpc::RPC_INVALID_REQUEST, [&] {
        (void)geocoin::rpc::parse_json_rpc_payload(complex);
    }, "strict parser accepted excessive structural complexity");

    const std::string long_key(257, 'k');
    expect_rpc_error(geocoin::rpc::RPC_INVALID_REQUEST, [&] {
        (void)geocoin::rpc::parse_json_rpc_payload("{\"" + long_key + "\":1}");
    }, "strict parser accepted an overlong object key");

    const std::string secret = "STRICT_PARSE_SECRET";
    try {
        (void)geocoin::rpc::parse_json_rpc_payload("{\"value\":\"" + secret + "\"");
        expect(false, "strict parser accepted malformed JSON");
    } catch (const RPCException& error) {
        expect(error.code() == geocoin::rpc::RPC_PARSE_ERROR &&
                   std::string(error.what()) == "Parse error" &&
                   std::string(error.what()).find(secret) == std::string::npos,
               "parse error leaked a request fragment");
    }
}

void test_sensitive_json_cleanup() {
    json sensitive_object = {
        {"PRIVATE_KEY_AS_OBJECT_KEY", "private-value"},
        {"nested", json{{"secret", "nested-value"}}}
    };
    geocoin::rpc::detail::cleanse_json_strings(sensitive_object);
    expect(sensitive_object.is_object() && sensitive_object.empty(),
           "sensitive JSON cleanup retained object keys or values");

    json sensitive_array = json::array({
        "array-secret",
        json{{"nested-secret-key", "nested-secret-value"}}
    });
    geocoin::rpc::detail::cleanse_json_strings(sensitive_array);
    expect(sensitive_array[0].is_string() && sensitive_array[0].get<std::string>().empty() &&
               sensitive_array[1].is_object() && sensitive_array[1].empty(),
           "sensitive JSON cleanup retained nested array data");
}

void test_credential_configuration_matrix() {
    const auto fresh_port = [] { return geocoin::test::find_free_port(); };

    {
        RPCServer server;
        expect(!server.start("127.0.0.1", fresh_port(), "", ""),
               "RPC server accepted missing admin credentials");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "admin", "admin-pass", "miner", ""
               ),
               "RPC server accepted a miner username without password");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "admin", "admin-pass",
                   "", "miner-pass"
               ),
               "RPC server accepted a miner password without username");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "admin", "admin-pass",
                   "", "", "reader", ""
               ),
               "RPC server accepted a read-only username without password");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "admin", "admin-pass",
                   "", "", "", "reader-pass"
               ),
               "RPC server accepted a read-only password without username");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "same", "same-pass",
                   "same", "same-pass"
               ),
               "RPC server accepted identical admin/miner credentials");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "same", "same-pass",
                   "", "", "same", "same-pass"
               ),
               "RPC server accepted identical admin/read-only credentials");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "admin", "admin-pass",
                   "same", "same-pass", "same", "same-pass"
               ),
               "RPC server accepted identical miner/read-only credentials");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "shared", "admin-pass",
                   "shared", "miner-pass"
               ),
               "RPC server accepted the same username for different roles");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "admin", "shared-pass",
                   "miner", "shared-pass"
               ),
               "RPC server accepted the same password for different roles");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), std::string(129, 'u'), "admin-pass"
               ),
               "RPC server accepted credentials that cannot be represented by Basic Auth");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "admin", "bad\npassword"
               ),
               "RPC server accepted control characters in credentials");
    }
    {
        RPCServer server;
        expect(!server.start(
                   "127.0.0.1", fresh_port(), "admin:shadow", "admin-pass"
               ),
               "RPC server accepted a Basic Auth username containing a colon");
    }
}

struct Credential {
    RPCAuthRole role;
    const char* user;
    const char* password;
};

void test_live_listener_role_matrix_and_concurrency() {
    ScopedEnvVar generous_limit("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "10000");
    RPCServer server;
    const uint16_t port = geocoin::test::find_free_port();
    expect(server.start(
               "127.0.0.1",
               port,
               "matrix-admin",
               "matrix-admin-pass",
               "matrix-miner",
               "matrix-miner-pass",
               "matrix-readonly",
               "matrix-readonly-pass"
           ),
           "live matrix RPC listener failed to start");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    const std::array<Credential, 3> credentials = {{
        {RPCAuthRole::ADMIN, "matrix-admin", "matrix-admin-pass"},
        {RPCAuthRole::MINER, "matrix-miner", "matrix-miner-pass"},
        {RPCAuthRole::READONLY, "matrix-readonly", "matrix-readonly-pass"}
    }};

    size_t live_denials = 0;
    const auto& commands = RPCServerTestAccess::registry(server).commands();
    for (const auto& [name, command] : commands) {
        for (const auto& credential : credentials) {
            if (geocoin::rpc::is_rpc_command_authorized(credential.role, command)) {
                continue;
            }
            const auto response = geocoin::test::send_rpc(
                port,
                credential.user,
                credential.password,
                name
            );
            expect(response.status == 200 &&
                       response_error_code(response.body) ==
                           geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE,
                   "live role boundary mismatch for " + name + " / " +
                       geocoin::rpc::rpc_role_name(credential.role));
            ++live_denials;
        }
    }
    expect(live_denials > commands.size(),
           "live listener did not exercise enough denied role/command pairs");

    std::atomic<unsigned> concurrent_failures{0};
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (unsigned request = 0; request < 16; ++request) {
                    const auto response = geocoin::test::send_rpc(
                        port,
                        "matrix-readonly",
                        "matrix-readonly-pass",
                        "help",
                        json::array({"getrpcinfo"})
                    );
                    if (response.status != 200 || response.body.contains("error")) {
                        concurrent_failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                (void)worker;
                concurrent_failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    expect(concurrent_failures.load(std::memory_order_relaxed) == 0,
           "concurrent authenticated read-only RPC requests were unstable");
    server.stop();

    RPCServer readonly_server(RPCCommandProfile::READ_ONLY);
    const uint16_t readonly_port = geocoin::test::find_free_port();
    expect(readonly_server.start(
               "127.0.0.1",
               readonly_port,
               "ro-admin",
               "ro-admin-pass",
               "ro-miner",
               "ro-miner-pass",
               "ro-user",
               "ro-user-pass"
           ),
           "read-only profile listener failed to start");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto filtered_stop = geocoin::test::send_rpc(
        readonly_port, "ro-admin", "ro-admin-pass", "stop"
    );
    expect(response_error_code(filtered_stop.body) == geocoin::rpc::RPC_METHOD_NOT_FOUND,
           "read-only listener retained the stop handler");
    const auto readonly_help = geocoin::test::send_rpc(
        readonly_port, "ro-user", "ro-user-pass", "help"
    );
    expect(!readonly_help.body.contains("error"),
           "read-only listener rejected its intended credential");
    readonly_server.stop();

    RPCServer mining_server(RPCCommandProfile::MINING_ONLY);
    const uint16_t mining_port = geocoin::test::find_free_port();
    expect(mining_server.start(
               "127.0.0.1",
               mining_port,
               "mine-admin",
               "mine-admin-pass",
               "mine-user",
               "mine-user-pass"
           ),
           "mining profile listener failed to start");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto filtered_help = geocoin::test::send_rpc(
        mining_port, "mine-user", "mine-user-pass", "help"
    );
    expect(response_error_code(filtered_help.body) == geocoin::rpc::RPC_METHOD_NOT_FOUND,
           "mining listener retained control/help handlers");
    const auto mining_template = geocoin::test::send_rpc(
        mining_port, "mine-user", "mine-user-pass", "getblocktemplate"
    );
    const int mining_error = response_error_code(mining_template.body);
    expect(mining_error != geocoin::rpc::RPC_METHOD_NOT_FOUND &&
               mining_error != geocoin::rpc::RPC_FORBIDDEN_BY_SAFE_MODE,
           "mining listener did not route an allowed miner command to its handler");
    mining_server.stop();

    std::cout << "Live RPC role denials checked: " << live_denials << "\n";
}

} // namespace

int main() {
    try {
        test_complete_catalog_role_and_profile_matrix();
        test_registry_fail_closed_invariants();
        test_dispatch_and_strict_json_boundaries();
        test_sensitive_json_cleanup();
        test_credential_configuration_matrix();
        test_live_listener_role_matrix_and_concurrency();
        std::cout << "test_rpc_security_matrix: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_rpc_security_matrix failed: " << error.what() << "\n";
        return 1;
    }
}
