#include "geocoin/node/node.hpp"
#include "geocoin/utils/version.hpp"
#include "geocoin/node/node_key.hpp"
#include "geocoin/benchmark/node_benchmark.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/rpc_bind_policy.hpp"
#include "geocoin/utils/secret_file.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/network/mempool.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false);
}

std::string normalize_option_token(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string read_secret_from_file(const std::string& path, const char* label) {
    return geocoin::utils::read_secret_file(path, label);
}

uint16_t parse_port(const std::string& value, const char* flag) {
    int port = 0;
    try {
        port = std::stoi(value);
    } catch (...) {
        throw std::runtime_error(std::string("Invalid value for ") + flag + ": " + value);
    }
    if (port < 1 || port > 65535) {
        throw std::runtime_error(std::string("Port out of range for ") + flag + ": " + value);
    }
    return static_cast<uint16_t>(port);
}

size_t parse_size_option(const std::string& value, const char* flag) {
    try {
        return static_cast<size_t>(std::stoull(value));
    } catch (...) {
        throw std::runtime_error(std::string("Invalid value for ") + flag + ": " + value);
    }
}

uint32_t parse_u32_option(const std::string& value, const char* flag) {
    const size_t parsed = parse_size_option(value, flag);
    if (parsed > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error(std::string("Value out of range for ") + flag + ": " + value);
    }
    return static_cast<uint32_t>(parsed);
}

int parse_int_option(const std::string& value, const char* flag) {
    try {
        return std::stoi(value);
    } catch (...) {
        throw std::runtime_error(std::string("Invalid value for ") + flag + ": " + value);
    }
}

geocoin::benchmark::json parse_benchmark_operations(const std::string& raw) {
    geocoin::benchmark::json operations = geocoin::benchmark::json::array();
    std::stringstream ss(raw);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = normalize_option_token(item);
        if (!item.empty()) {
            operations.push_back(item);
        }
    }
    if (operations.empty()) {
        throw std::runtime_error("--benchmark-operations must not be empty");
    }
    return operations;
}

geocoin::benchmark::json run_cli_benchmarks(
    const std::filesystem::path& data_dir,
    const std::string& suite,
    const geocoin::benchmark::json& options
) {
    geocoin::benchmark::json result;
    result["mode"] = "benchmark";
    result["data_dir"] = data_dir.string();

    if (suite == "all") {
        result["suite"] = "all";
        result["db"] = geocoin::benchmark::run_benchmark(data_dir, "db", geocoin::benchmark::json::object());
        result["crypto"] = geocoin::benchmark::run_benchmark(data_dir, "crypto", geocoin::benchmark::json::object());
        return result;
    }

    if (suite != "db" && suite != "crypto" && suite != "compute" && suite != "falcon") {
        throw std::runtime_error("Invalid value for --benchmark-suite: " + suite + " (valid: db|crypto|falcon|all)");
    }

    result["result"] = geocoin::benchmark::run_benchmark(data_dir, suite, options);
    return result;
}

void load_optional_user_from_env(std::string& user, const char* env_name) {
    if (!user.empty()) {
        return;
    }
    if (const char* env = std::getenv(env_name)) {
        user = env;
    }
}

void load_secret_from_file_or_env(
    std::string& secret,
    const std::string& secret_file,
    const char* file_label,
    const char* env_name,
    bool& secret_from_cli
) {
    if (!secret_file.empty()) {
        secret = read_secret_from_file(secret_file, file_label);
        secret_from_cli = false;
        return;
    }
    if (secret.empty()) {
        if (const char* env = std::getenv(env_name)) {
            secret = env;
        }
    }
}

void warn_secret_from_cli(bool secret_from_cli, const char* flag, const char* preferred) {
    if (!secret_from_cli) {
        return;
    }
    std::cerr << "WARNING: " << flag
              << " is visible in process list/history. Prefer "
              << preferred << ".\n";
}

bool validate_rpc_credentials(
    const std::string& rpc_user,
    const std::string& rpc_pass,
    const std::string& rpc_miner_user,
    const std::string& rpc_miner_pass,
    const std::string& rpc_readonly_user,
    const std::string& rpc_readonly_pass,
    std::string& error
) {
    if (rpc_user.empty() || rpc_pass.empty()) {
        error = "RPC enabled but --rpc-user and RPC password were not provided";
        return false;
    }
    if (rpc_miner_user.empty() != rpc_miner_pass.empty()) {
        error = "External miner RPC requires both --rpc-miner-user and --rpc-miner-pass";
        return false;
    }
    if (rpc_readonly_user.empty() != rpc_readonly_pass.empty()) {
        error = "Read-only RPC requires both --rpc-readonly-user and --rpc-readonly-pass";
        return false;
    }
    if (!geocoin::rpc::is_valid_rpc_credential_pair(rpc_user, rpc_pass) ||
        (!rpc_miner_user.empty() &&
         !geocoin::rpc::is_valid_rpc_credential_pair(rpc_miner_user, rpc_miner_pass)) ||
        (!rpc_readonly_user.empty() &&
         !geocoin::rpc::is_valid_rpc_credential_pair(
             rpc_readonly_user, rpc_readonly_pass))) {
        error = "RPC credentials contain control characters or exceed Basic Auth protocol limits";
        return false;
    }
    if (!rpc_miner_user.empty() &&
        (rpc_miner_user == rpc_user || rpc_miner_pass == rpc_pass)) {
        error = "External miner RPC username and password must each differ from admin RPC credentials";
        return false;
    }
    if (!rpc_readonly_user.empty() &&
        (rpc_readonly_user == rpc_user || rpc_readonly_pass == rpc_pass)) {
        error = "Read-only RPC username and password must each differ from admin RPC credentials";
        return false;
    }
    if (!rpc_miner_user.empty() && !rpc_readonly_user.empty() &&
        (rpc_miner_user == rpc_readonly_user || rpc_miner_pass == rpc_readonly_pass)) {
        error = "Read-only RPC username and password must each differ from external miner RPC credentials";
        return false;
    }

    return true;
}

void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "Options:\n"
        << "  --data-dir <path>      Data directory (default: ./data)\n"
        << "  --p2p-port <port>      P2P port override\n"
        << "  --no-p2p               Disable P2P listener (RPC-only mode)\n"
        << "  --rpc-port <port>      RPC port override\n"
        << "  --rpc                 Enable RPC server (requires --rpc-user and a password)\n"
        << "  --rpc-bind <ip>        RPC bind host (default: 127.0.0.1)\n"
        << "                         Non-loopback binds require GEOCOIN_ALLOW_PUBLIC_RPC_BIND=1\n"
        << "  --rpc-user <user>      RPC username\n"
        << "  --rpc-pass <pass>      RPC password (prefer --rpc-pass-file)\n"
        << "  --rpc-pass-file <path> Read RPC password from file (first line)\n"
        << "  --rpc-miner-user <user>      External miner RPC username\n"
        << "  --rpc-miner-pass <pass>      External miner RPC password\n"
        << "  --rpc-miner-pass-file <path> Read external miner RPC password from file\n"
        << "  --rpc-readonly-user <user>   Read-only RPC username for trusted monitoring clients\n"
        << "  --rpc-readonly-pass <pass>   Read-only RPC password\n"
        << "  --rpc-readonly-pass-file <path> Read read-only RPC password from file\n"
        << "  --rpc-rate-limit <n>   RPC requests per client per 60s (0 = unlimited, default)\n"
        << "  --rpc-max-connections <n> Concurrent RPC TCP connections (0 = unlimited, default)\n"
        << "  --rpc-max-connections-per-ip <n> Concurrent RPC connections per client IP (0 = unlimited)\n"
        << "  --benchmark           Run standalone benchmark mode and exit (no P2P/RPC runtime)\n"
        << "  --benchmark-suite <name> db|crypto|falcon|all (default: all)\n"
        << "  --benchmark-operations <csv> Operation list for selected suite\n"
        << "  --benchmark-iterations <n> Operation iterations override\n"
        << "  --benchmark-value-size <bytes> DB value size override\n"
        << "  --benchmark-scan-repeats <n> DB scan repeats override\n"
        << "  --benchmark-data-size <bytes> Crypto data size override\n"
        << "  --benchmark-pbkdf2-iterations <n> PBKDF2 iterations override\n"
        << "  --benchmark-falcon-iterations <n> Falcon key/sign iterations override\n"
        << "  --benchmark-include-slow  Include selected slow crypto profiles\n"
        << "  --benchmark-output <path> Write benchmark JSON result to file\n"
        << "  --network <id>         Network id (degree|degree-test|degree-load, default: degree)\n"
        << "  --node-type <type>     Node type: full|pruned (default: full)\n"
        << "  --pruned-blocks <n>    Recent block window for pruned node (default: 1000)\n"
        << "  --pruned-keep-old-blocks  Do not delete old block bodies in pruned mode\n"
        << "  --pruned-keep-old-indexes Do not delete old block indexes in pruned mode\n"
        << "  --db-profile <name>    RocksDB profile: safe|fast-sync|unsafe-benchmark (default: safe)\n"
        << "  --db-write-buffer-mb <mb>    DB write buffer override (0 = backend default)\n"
        << "  --db-cache-mb <mb>     DB cache override/reserved (0 = backend default)\n"
        << "  --db-max-background-jobs <n> DB background job override (0 = backend default)\n"
        << "  --db-unsafe-disable-wal-during-ibd  Benchmark only: disable WAL during IBD\n"
        << "  --addnode <ip:port>    Add persistent peer (repeatable)\n"
        << "  --connect <ip:port>    Connect-only persistent peer (repeatable)\n"
        << "  --status-interval <s>  Status print interval in seconds (default: 30)\n"
        << "  --activation-manifest <path>  Activation manifest path (default: <data-dir>/activation-manifest.json)\n"
        << "  --wallet-password <password>  Wallet password for loading encrypted wallet.dat\n"
        << "  --wallet-password-file <path> Read wallet password from file (first line)\n"
        << "  --audit-log <path>     Write operational/audit logs to file\n"
        << "  --mining-sha256 <mode> Mining SHA256 backend: auto|native|legacy|openssl (default: auto)\n"
        << "  --debug                Enable verbose DEBUG logging\n"
        << "  --archive              Disable witness pruning (store full history)\n"
        << "  --no-auto-connect      Disable seed auto-connect\n"
        << "  --help                 Show this help\n";
}
}  // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    const std::string program_name = (argc > 0 && argv[0] != nullptr)
        ? std::filesystem::path(argv[0]).filename().string()
        : std::string("geocoin-node");

    try {
        geocoin::node::NodeConfig config;
        int status_interval_sec = 30;
        bool debug_mode = false;
        bool enable_rpc = false;
        bool benchmark_mode = false;
        std::string benchmark_suite = "all";
        std::string benchmark_operations;
        std::string benchmark_output_path;
        geocoin::benchmark::json benchmark_options = geocoin::benchmark::json::object();
        std::string rpc_bind = "127.0.0.1";
        std::string rpc_user;
        std::string rpc_pass;
        std::string rpc_pass_file;
        std::string rpc_miner_user;
        std::string rpc_miner_pass;
        std::string rpc_miner_pass_file;
        std::string rpc_readonly_user;
        std::string rpc_readonly_pass;
        std::string rpc_readonly_pass_file;
        std::string wallet_password;
        std::string wallet_password_file;
        std::string audit_log_path;
        bool rpc_pass_from_cli = false;
        bool rpc_miner_pass_from_cli = false;
        bool rpc_readonly_pass_from_cli = false;
        bool wallet_pass_from_cli = false;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];

            if (arg == "--version" || arg == "-version") {
                std::cout << "GeoCoin " << geocoin::version::APPLICATION
                          << " (RPC API " << geocoin::version::RPC_API << ")\n";
                return 0;
            }

            if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                return 0;
            }
            if (arg == "--data-dir" && i + 1 < argc) {
                config.data_dir = argv[++i];
                continue;
            }
            if (arg == "--p2p-port" && i + 1 < argc) {
                config.p2p_port = parse_port(argv[++i], "--p2p-port");
                continue;
            }
            if (arg == "--no-p2p") {
                config.enable_p2p_listen = false;
                config.enable_auto_connect = false;
                continue;
            }
            if (arg == "--rpc-port" && i + 1 < argc) {
                config.rpc_port = parse_port(argv[++i], "--rpc-port");
                continue;
            }
            if (arg == "--rpc") {
                enable_rpc = true;
                continue;
            }
            if (arg == "--rpc-bind" && i + 1 < argc) {
                rpc_bind = argv[++i];
                continue;
            }
            if (arg == "--rpc-user" && i + 1 < argc) {
                rpc_user = argv[++i];
                continue;
            }
            if (arg == "--rpc-pass" && i + 1 < argc) {
                rpc_pass = argv[++i];
                rpc_pass_from_cli = true;
                continue;
            }
            if (arg == "--rpc-pass-file" && i + 1 < argc) {
                rpc_pass_file = argv[++i];
                continue;
            }
            if (arg == "--rpc-miner-user" && i + 1 < argc) {
                rpc_miner_user = argv[++i];
                continue;
            }
            if (arg == "--rpc-miner-pass" && i + 1 < argc) {
                rpc_miner_pass = argv[++i];
                rpc_miner_pass_from_cli = true;
                continue;
            }
            if (arg == "--rpc-miner-pass-file" && i + 1 < argc) {
                rpc_miner_pass_file = argv[++i];
                continue;
            }
            if (arg == "--rpc-readonly-user" && i + 1 < argc) {
                rpc_readonly_user = argv[++i];
                continue;
            }
            if (arg == "--rpc-readonly-pass" && i + 1 < argc) {
                rpc_readonly_pass = argv[++i];
                rpc_readonly_pass_from_cli = true;
                continue;
            }
            if (arg == "--rpc-readonly-pass-file" && i + 1 < argc) {
                rpc_readonly_pass_file = argv[++i];
                continue;
            }
            if (arg == "--rpc-rate-limit" && i + 1 < argc) {
                config.rpc_rate_limit_max_requests_per_window =
                    parse_u32_option(argv[++i], "--rpc-rate-limit");
                config.rpc_rate_limit_explicit = true;
                continue;
            }
            if (arg == "--rpc-max-connections" && i + 1 < argc) {
                config.rpc_max_connections =
                    parse_size_option(argv[++i], "--rpc-max-connections");
                config.rpc_max_connections_explicit = true;
                continue;
            }
            if (arg == "--rpc-max-connections-per-ip" && i + 1 < argc) {
                config.rpc_max_connections_per_ip =
                    parse_size_option(argv[++i], "--rpc-max-connections-per-ip");
                config.rpc_max_connections_per_ip_explicit = true;
                continue;
            }
            if (arg == "--benchmark") {
                benchmark_mode = true;
                continue;
            }
            if (arg == "--benchmark-suite" && i + 1 < argc) {
                benchmark_suite = normalize_option_token(argv[++i]);
                continue;
            }
            if (arg == "--benchmark-operations" && i + 1 < argc) {
                benchmark_operations = argv[++i];
                continue;
            }
            if (arg == "--benchmark-iterations" && i + 1 < argc) {
                benchmark_options["iterations"] = parse_size_option(argv[++i], "--benchmark-iterations");
                continue;
            }
            if (arg == "--benchmark-value-size" && i + 1 < argc) {
                benchmark_options["value_size"] = parse_size_option(argv[++i], "--benchmark-value-size");
                continue;
            }
            if (arg == "--benchmark-scan-repeats" && i + 1 < argc) {
                benchmark_options["scan_repeats"] = parse_size_option(argv[++i], "--benchmark-scan-repeats");
                continue;
            }
            if (arg == "--benchmark-data-size" && i + 1 < argc) {
                benchmark_options["data_size"] = parse_size_option(argv[++i], "--benchmark-data-size");
                continue;
            }
            if (arg == "--benchmark-pbkdf2-iterations" && i + 1 < argc) {
                benchmark_options["pbkdf2_iterations"] = parse_size_option(argv[++i], "--benchmark-pbkdf2-iterations");
                continue;
            }
            if (arg == "--benchmark-falcon-iterations" && i + 1 < argc) {
                benchmark_options["falcon_iterations"] = parse_size_option(argv[++i], "--benchmark-falcon-iterations");
                continue;
            }
            if (arg == "--benchmark-include-slow") {
                benchmark_options["include_slow"] = true;
                continue;
            }
            if (arg == "--benchmark-output" && i + 1 < argc) {
                benchmark_output_path = argv[++i];
                continue;
            }
            if (arg == "--network" && i + 1 < argc) {
                config.network = argv[++i];
                continue;
            }
            if (arg == "--node-type" && i + 1 < argc) {
                const std::string raw_value = argv[++i];
                const std::string value = normalize_option_token(raw_value);
                if (value == "full") {
                    config.node_type = geocoin::node::NodeType::FULL;
                } else if (value == "pruned") {
                    config.node_type = geocoin::node::NodeType::PRUNED;
                } else {
                    throw std::runtime_error(
                        "Invalid value for --node-type: " + raw_value + " (valid: full|pruned)"
                    );
                }
                config.node_type_explicit = true;
                continue;
            }
            if (arg == "--pruned-blocks" && i + 1 < argc) {
                config.pruned_block_retention = static_cast<geocoin::Height>(
                    parse_size_option(argv[++i], "--pruned-blocks")
                );
                config.pruned_block_retention_explicit = true;
                continue;
            }
            if (arg == "--pruned-keep-old-blocks") {
                config.pruned_delete_old_blocks = false;
                config.pruned_delete_old_blocks_explicit = true;
                continue;
            }
            if (arg == "--pruned-keep-old-indexes") {
                config.pruned_delete_old_indexes = false;
                config.pruned_delete_old_indexes_explicit = true;
                continue;
            }
            if (arg == "--db-profile" && i + 1 < argc) {
                config.db_profile = argv[++i];
                config.db_profile_explicit = true;
                continue;
            }
            if (arg == "--db-write-buffer-mb" && i + 1 < argc) {
                config.db_write_buffer_mb = parse_size_option(argv[++i], "--db-write-buffer-mb");
                config.db_write_buffer_explicit = true;
                continue;
            }
            if (arg == "--db-cache-mb" && i + 1 < argc) {
                config.db_cache_mb = parse_size_option(argv[++i], "--db-cache-mb");
                config.db_cache_explicit = true;
                continue;
            }
            if (arg == "--db-max-background-jobs" && i + 1 < argc) {
                config.db_max_background_jobs = parse_int_option(argv[++i], "--db-max-background-jobs");
                config.db_max_background_jobs_explicit = true;
                continue;
            }
            if (arg == "--db-unsafe-disable-wal-during-ibd") {
                config.db_unsafe_disable_wal_during_ibd = true;
                config.db_unsafe_wal_explicit = true;
                continue;
            }
            if (arg == "--addnode" && i + 1 < argc) {
                config.seed_nodes.emplace_back(argv[++i]);
                continue;
            }
            if (arg == "--connect" && i + 1 < argc) {
                config.connect_nodes.emplace_back(argv[++i]);
                continue;
            }
            if (arg == "--status-interval" && i + 1 < argc) {
                int parsed = 0;
                try {
                    parsed = std::stoi(argv[++i]);
                } catch (...) {
                    throw std::runtime_error("Invalid value for --status-interval");
                }
                if (parsed < 1 || parsed > 3600) {
                    throw std::runtime_error("--status-interval must be in range [1..3600]");
                }
                status_interval_sec = parsed;
                continue;
            }
            if (arg == "--activation-manifest" && i + 1 < argc) {
                config.activation_manifest_path = argv[++i];
                continue;
            }
            if (arg == "--wallet-password" && i + 1 < argc) {
                wallet_password = argv[++i];
                wallet_pass_from_cli = true;
                continue;
            }
            if (arg == "--wallet-password-file" && i + 1 < argc) {
                wallet_password_file = argv[++i];
                continue;
            }
            if (arg == "--audit-log" && i + 1 < argc) {
                audit_log_path = argv[++i];
                continue;
            }
            if (arg == "--mining-sha256" && i + 1 < argc) {
                config.mining_sha256_backend = argv[++i];
                continue;
            }
            if (arg == "--debug") {
                debug_mode = true;
                continue;
            }
            if (arg == "--no-auto-connect") {
                config.enable_auto_connect = false;
                continue;
            }
            if (arg == "--archive") {
                config.archive = true;
                continue;
            }

            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }

        load_secret_from_file_or_env(
            rpc_pass,
            rpc_pass_file,
            "RPC password",
            "GEOCOIN_RPC_PASS",
            rpc_pass_from_cli
        );
        load_optional_user_from_env(rpc_miner_user, "GEOCOIN_RPC_MINER_USER");
        load_secret_from_file_or_env(
            rpc_miner_pass,
            rpc_miner_pass_file,
            "external miner RPC password",
            "GEOCOIN_RPC_MINER_PASS",
            rpc_miner_pass_from_cli
        );
        load_optional_user_from_env(rpc_readonly_user, "GEOCOIN_RPC_READONLY_USER");
        load_secret_from_file_or_env(
            rpc_readonly_pass,
            rpc_readonly_pass_file,
            "read-only RPC password",
            "GEOCOIN_RPC_READONLY_PASS",
            rpc_readonly_pass_from_cli
        );
        load_secret_from_file_or_env(
            wallet_password,
            wallet_password_file,
            "wallet password",
            "GEOCOIN_WALLET_PASS",
            wallet_pass_from_cli
        );

        if (const char* env = std::getenv("GEOCOIN_MINING_SHA256")) {
            config.mining_sha256_backend = env;
        }

        warn_secret_from_cli(
            rpc_pass_from_cli,
            "--rpc-pass",
            "--rpc-pass-file or GEOCOIN_RPC_PASS"
        );
        warn_secret_from_cli(
            rpc_miner_pass_from_cli,
            "--rpc-miner-pass",
            "--rpc-miner-pass-file or GEOCOIN_RPC_MINER_PASS"
        );
        warn_secret_from_cli(
            rpc_readonly_pass_from_cli,
            "--rpc-readonly-pass",
            "--rpc-readonly-pass-file or GEOCOIN_RPC_READONLY_PASS"
        );
        warn_secret_from_cli(
            wallet_pass_from_cli,
            "--wallet-password",
            "--wallet-password-file or GEOCOIN_WALLET_PASS"
        );

        if (!config.connect_nodes.empty()) {
            config.enable_auto_connect = true;
        }
        if (debug_mode && status_interval_sec == 30) {
            status_interval_sec = 5;
        }

        config.data_dir = std::filesystem::absolute(config.data_dir).string();
        std::filesystem::create_directories(config.data_dir);
        geocoin::wallet::Wallet::set_default_wallet_path(config.data_dir + "/wallet.dat");

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        // По умолчанию тихий режим: только WARN/ERROR.
        geocoin::utils::Logger::instance().set_level(geocoin::utils::LogLevel::WARN);
        if (debug_mode) {
            geocoin::utils::Logger::instance().set_level(geocoin::utils::LogLevel::DEBUG);
#if defined(_WIN32)
            _putenv_s("GEOCOIN_SYNC_DEBUG", "1");
#else
            setenv("GEOCOIN_SYNC_DEBUG", "1", 1);
#endif
        }

        if (!audit_log_path.empty()) {
            if (!geocoin::utils::Logger::instance().set_output_file(audit_log_path)) {
                std::cerr << "Failed to open audit log file: " << audit_log_path << "\n";
                return 1;
            }
            GEO_LOG_INFO("audit", program_name << " audit log enabled path=" << audit_log_path);
        }

        if (benchmark_mode) {
            if (!benchmark_operations.empty()) {
                if (benchmark_suite == "all") {
                    throw std::runtime_error("--benchmark-operations requires --benchmark-suite db|crypto");
                }
                benchmark_options["operations"] = parse_benchmark_operations(benchmark_operations);
            }

            geocoin::benchmark::json result = run_cli_benchmarks(
                std::filesystem::path(config.data_dir),
                benchmark_suite,
                benchmark_options
            );
            const std::string output = result.dump(2);
            std::cout << output << "\n";
            if (!benchmark_output_path.empty()) {
                std::ofstream out(benchmark_output_path, std::ios::binary | std::ios::trunc);
                if (!out.good()) {
                    std::cerr << "Failed to open benchmark output file: " << benchmark_output_path << "\n";
                    return 1;
                }
                out << output << "\n";
            }
            return 0;
        }

        uint16_t rpc_port = config.rpc_port;
        if (rpc_port == 0) {
            if (!geocoin::consensus::SelectParamsByNetworkId(config.network)) {
                throw std::runtime_error("Invalid network: " + config.network + " (valid: degree, degree-test, degree-load)");
            }
            rpc_port = geocoin::consensus::Params().rpc_port();
        }
        if (enable_rpc) {
            std::string rpc_credentials_error;
            if (!validate_rpc_credentials(
                    rpc_user,
                    rpc_pass,
                    rpc_miner_user,
                    rpc_miner_pass,
                    rpc_readonly_user,
                    rpc_readonly_pass,
                    rpc_credentials_error
                )) {
                std::cerr << rpc_credentials_error << "\n";
                return 1;
            }
            if (!geocoin::utils::is_loopback_rpc_bind_host(rpc_bind)) {
                if (!geocoin::utils::allow_public_rpc_bind_from_env()) {
                    std::cerr
                        << "ERROR: refusing non-loopback RPC bind " << rpc_bind << ":" << rpc_port
                        << ". Set GEOCOIN_ALLOW_PUBLIC_RPC_BIND=1 only behind an audited private firewall.\n";
                    return 1;
                }
                std::cerr
                    << "WARNING: RPC is exposed on " << rpc_bind << ":" << rpc_port
                    << ". Put it behind an audited private firewall/TLS proxy and do not expose wallet/admin RPC publicly.\n";
            }
        }

        auto node = std::make_shared<geocoin::node::Node>(config);

        geocoin::bytes node_key = geocoin::node::load_or_create_node_key(config.data_dir);
        geocoin::utils::ScopedContainerCleanser<geocoin::bytes> nodeKeyCleanser(node_key);
        auto wallet = std::make_shared<geocoin::wallet::Wallet>();

        const std::string wallet_path = geocoin::wallet::Wallet::get_default_wallet_path();
        if (std::filesystem::exists(wallet_path)) {
            if (!wallet->load_from_file(wallet_path, wallet_password)) {
                std::cerr << "Failed to load wallet from " << wallet_path << "\n";
                return 1;
            }
            std::cout << "Wallet loaded: " << wallet_path << "\n";
        } else {
            std::cout << "Wallet file not found, wallet RPC is ready for createwallet at " << wallet_path << "\n";
        }

        const auto start_result = node->start_with_result(node_key);
        if (!start_result.ok()) {
            std::cerr
                << "Failed to start node ["
                << geocoin::node::node_start_failure_name(start_result.failure)
                << "]: " << start_result.message << "\n";
            if (start_result.chain_data_may_need_attention()) {
                std::cerr
                    << "Chain data was left unchanged; inspect logs and back it up before any explicit recovery.\n";
            }
            return 1;
        }

        geocoin::rpc::RPCServer rpc_server;
        if (enable_rpc) {
            std::cout << "RPC enabled: bind=" << rpc_bind << ":" << rpc_port << "\n";
            const auto components = node->component_handle();
            const auto runtime_cfg = node->get_config();
            geocoin::rpc::RPCServerContext rpc_context;
            rpc_context.blockchain = components.blockchain;
            rpc_context.wallet = wallet;
            rpc_context.chain_state = components.chain_state;
            rpc_context.mempool = components.mempool;
            rpc_context.peer_manager = components.peer_manager;
            rpc_context.node_services = components.services;
            rpc_context.chain_mutations = rpc_context.node_services.mutations;
            rpc_context.rpc_rate_limit_max_requests_per_window =
                runtime_cfg.rpc_rate_limit_max_requests_per_window;
            rpc_context.rpc_max_connections = runtime_cfg.rpc_max_connections;
            rpc_context.rpc_max_connections_per_ip = runtime_cfg.rpc_max_connections_per_ip;
            if (!rpc_server.set_context(std::move(rpc_context))) {
                std::cerr << "Failed to configure RPC server context\n";
                node->stop();
                return 1;
            }
            rpc_server.set_shutdown_callback([]() {
                g_running.store(false);
            });
            if (!rpc_server.start(
                    rpc_bind,
                    rpc_port,
                    rpc_user,
                    rpc_pass,
                    rpc_miner_user,
                    rpc_miner_pass,
                    rpc_readonly_user,
                    rpc_readonly_pass
                )) {
                std::cerr << "Failed to start RPC server\n";
                node->stop();
                return 1;
            }
            std::cout << "RPC server started\n";
        }

        std::cout << program_name << " started. Press Ctrl+C to stop.\n";
        std::atomic<bool> wallet_worker_stop{false};
        std::condition_variable wallet_worker_cv;
        std::mutex wallet_worker_mutex;
        const auto wallet_components = node->component_handle();
        std::jthread wallet_worker([&](std::stop_token stop) {
            std::stop_callback on_stop(stop, [&] {
                wallet_worker_stop.store(true);
                wallet->stop_scans();
                wallet_worker_cv.notify_all();
            });
            auto next_rebroadcast = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(wallet_worker_mutex);
            while (!wallet_worker_stop.load()) {
                lock.unlock();
                try {
                    const auto pending = wallet_components.mempool->get_all_transactions();
                    const auto result = wallet->synchronize(wallet_components.chain_state, pending);
                    if (!result.complete() && !wallet_worker_stop.load())
                        GEO_LOG_WARN("wallet", "Headless wallet scan incomplete: " << result.error);
                    if (result.complete() && std::chrono::steady_clock::now() >= next_rebroadcast) {
                        next_rebroadcast = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                        for (const auto& tx : wallet->get_transactions()) {
                            if (wallet_worker_stop.load()) break;
                            if (tx.broadcast && !tx.is_coinbase && tx.block_hash == geocoin::Hash256{} &&
                                !wallet_components.mempool->has_transaction(tx.tx_hash))
                                (void)wallet_components.services.mutations->admit_transaction(tx.tx);
                        }
                    }
                } catch (const std::exception& error) {
                    GEO_LOG_ERROR("wallet", "Headless wallet update failed: " << error.what());
                }
                lock.lock();
                wallet_worker_cv.wait_for(lock, std::chrono::seconds(1), [&] { return wallet_worker_stop.load(); });
            }
        });
        auto next_status_at = std::chrono::steady_clock::now() + std::chrono::seconds(status_interval_sec);
        bool runtime_failed = false;
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!g_running.load()) {
                break;
            }
            // Node::is_running() turns false when the background runtime dies
            // (for example an unhandled exception in the update loop). Keeping
            // the process alive would leave a zombie that answers RPC but can
            // no longer sync or accept blocks; exit non-zero so a supervisor
            // restarts it.
            if (!node->is_running()) {
                std::cerr << program_name
                          << ": node runtime stopped unexpectedly; shutting down\n";
                runtime_failed = true;
                break;
            }
            auto now = std::chrono::steady_clock::now();
            if (now < next_status_at) {
                continue;
            }
            next_status_at = now + std::chrono::seconds(status_interval_sec);
            const auto status = node->get_status();
            std::cout
                << "height=" << status.current_height
                << " peers=" << status.connected_peers
                << " mempool=" << status.mempool_size
                << " syncing=" << (status.syncing ? "yes" : "no")
                << "\n";
        }

        rpc_server.stop();
        wallet_worker_stop.store(true);
        wallet->stop_scans();
        wallet_worker.request_stop();
        wallet_worker_cv.notify_all();
        wallet_worker.join();
        node->stop();
        std::cout << program_name << " stopped\n";
        return runtime_failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << program_name << " error: " << e.what() << "\n";
        return 1;
    }
}
