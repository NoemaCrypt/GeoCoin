#include "geocoin/node/node_config_loader.hpp"
#include "geocoin/node/node_key.hpp"
#include "geocoin/node/node.hpp"
#include "geocoin/utils/secret_file.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

using namespace geocoin;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void test_pruned_retention_required() {
    node::NodeConfig config;
    config.node_type = node::NodeType::PRUNED;
    config.pruned_block_retention = 0;

    std::string error;
    check(!node::validate_node_config(config, error), "zero pruned retention is rejected");
    check(error.find("pruned-blocks") != std::string::npos, "retention error mentions pruned-blocks");
}

void test_pruned_archive_conflict() {
    node::NodeConfig config;
    config.node_type = node::NodeType::PRUNED;
    config.archive = true;

    std::string error;
    check(!node::validate_node_config(config, error), "pruned archive conflict is rejected");
    check(error.find("archive=1") != std::string::npos, "archive error mentions archive=1");
}

void test_pruned_index_delete_normalization() {
    node::NodeConfig config;
    config.node_type = node::NodeType::PRUNED;
    config.pruned_delete_old_blocks = false;
    config.pruned_delete_old_indexes = true;

    std::string error;
    check(node::validate_node_config(config, error), "valid pruned config is accepted");
    check(!config.pruned_delete_old_indexes, "index delete is disabled when block delete is disabled");
}

void test_full_config_allows_archive() {
    node::NodeConfig config;
    config.node_type = node::NodeType::FULL;
    config.archive = true;

    std::string error;
    check(node::validate_node_config(config, error), "full archive config is accepted");
}

std::filesystem::path write_temp_config(const std::string& body) {
    const auto path = std::filesystem::temp_directory_path() /
        ("geocoin-node-config-loader-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".conf");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    return path;
}

void test_storage_config_file_values() {
    const auto path = write_temp_config(
        "db-profile=fast-sync\n"
        "db-write-buffer-mb= 64 \n"
        "db-cache-mb= 128 \n"
        "db-max-background-jobs=+4\n"
        "db-unsafe-disable-wal-during-ibd=1\n"
        "node-type=pruned\n"
        "pruned-blocks=42\n"
        "pruned-keep-old-blocks=1\n"
        "pruned-delete-old-indexes=1\n"
    );

    node::NodeConfig config;
    node::apply_storage_config_from_file(config, path.string());

    check(config.db_profile == "fast-sync", "db-profile is loaded from file");
    check(config.db_write_buffer_mb == 64, "db-write-buffer-mb is loaded from file");
    check(config.db_cache_mb == 128, "db-cache-mb is loaded from file");
    check(config.db_max_background_jobs == 4, "db-max-background-jobs is loaded from file");
    check(config.db_unsafe_disable_wal_during_ibd, "unsafe WAL flag is loaded from file");
    check(config.node_type == node::NodeType::PRUNED, "node-type is loaded from file");
    check(config.pruned_block_retention == 42, "pruned-blocks is loaded from file");
    check(!config.pruned_delete_old_blocks, "pruned-keep-old-blocks flips delete flag");

    std::string error;
    check(node::validate_node_config(config, error), "loaded pruned config validates");
    check(!config.pruned_delete_old_indexes, "index delete is normalized when old blocks are kept");

    std::filesystem::remove(path);
}

void test_storage_config_false_aliases_and_explicit_overrides() {
    const auto path = write_temp_config(
        "db-profile=unsafe-benchmark\n"
        "db-write-buffer-mb=999\n"
        "db-cache-mb=777\n"
        "db-max-background-jobs=+5\n"
        "db-unsafe-disable-wal-during-ibd= off \n"
        "node-type= FULL \n"
        "pruned-delete-old-blocks=no\n"
        "pruned-keep-old-indexes=yes\n"
        "db-backend=rocksdb\n"
    );

    node::NodeConfig config;
    config.db_profile = "safe";
    config.db_write_buffer_mb = 11;
    config.db_cache_mb = 22;
    config.db_max_background_jobs = 33;
    config.db_unsafe_disable_wal_during_ibd = true;
    config.db_profile_explicit = true;
    config.db_write_buffer_explicit = true;
    config.db_cache_explicit = true;
    config.db_max_background_jobs_explicit = true;
    config.db_unsafe_wal_explicit = true;

    node::apply_storage_config_from_file(config, path.string());

    check(config.db_backend == "rocksdb", "supported db-backend is accepted");
    check(config.db_profile == "safe", "explicit db-profile is not overwritten");
    check(config.db_write_buffer_mb == 11, "explicit write buffer is not overwritten");
    check(config.db_cache_mb == 22, "explicit cache is not overwritten");
    check(config.db_max_background_jobs == 33, "explicit background jobs is not overwritten");
    check(config.db_unsafe_disable_wal_during_ibd, "explicit WAL flag is not overwritten");
    check(config.node_type == node::NodeType::FULL, "node-type parser accepts spaced uppercase full");
    check(!config.pruned_delete_old_blocks, "boolean no disables old block deletion");
    check(!config.pruned_delete_old_indexes, "keep old indexes disables old index deletion");

    std::filesystem::remove(path);
}

void test_invalid_config_values_are_rejected() {
    const auto invalid_node_type = write_temp_config("node-type=archive\n");
    node::NodeConfig bad_type;
    bool threw = false;
    try {
        node::apply_storage_config_from_file(bad_type, invalid_node_type.string());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "invalid node-type throws");
    std::filesystem::remove(invalid_node_type);

    const auto path = write_temp_config(
        "db-write-buffer-mb=\n"
        "db-cache-mb=184467440737095516160\n"
        "db-profile=\n"
        "db-max-background-jobs=2147483648\n"
        "db-unsafe-disable-wal-during-ibd=maybe\n"
        "node-type=pruned\n"
        "pruned-blocks=184467440737095516160\n"
    );

    node::NodeConfig config;
    threw = false;
    try {
        node::apply_storage_config_from_file(config, path.string());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "invalid known config values fail fast");

    std::filesystem::remove(path);

    const auto legacy_backend = write_temp_config("db-backend=leveldb\n");
    threw = false;
    try {
        node::apply_storage_config_from_file(config, legacy_backend.string());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "unsupported legacy db-backend is rejected");
    std::filesystem::remove(legacy_backend);
}

void test_default_config_and_node_type_names() {
    const auto existing_path = write_temp_config("keep-this=1\n");
    node::write_default_node_config_if_missing(existing_path.string());
    std::ifstream existing_in(existing_path, std::ios::binary);
    std::string existing_body;
    std::getline(existing_in, existing_body);
    check(existing_body == "keep-this=1", "existing default config file is not overwritten");
    std::filesystem::remove(existing_path);

    const auto missing_path = std::filesystem::temp_directory_path() /
        ("geocoin-node-config-default-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".conf");
    std::filesystem::remove(missing_path);
    node::write_default_node_config_if_missing(missing_path.string());
    std::ifstream generated_in(missing_path, std::ios::binary);
    std::string generated_body(
        (std::istreambuf_iterator<char>(generated_in)),
        std::istreambuf_iterator<char>()
    );
    check(generated_body.find("GeoCoin configuration") != std::string::npos,
          "missing default config file is generated");
    check(generated_body.find("rpc-rate-limit=0") != std::string::npos,
          "default config disables RPC request rate limit");
    check(generated_body.find("rpc-max-connections=0") != std::string::npos,
          "default config disables RPC connection cap");
    check(generated_body.find("rpc-max-connections-per-ip=0") != std::string::npos,
          "default config disables per-IP RPC connection cap");
    std::filesystem::remove(missing_path);

    const node::NodeConfig defaults;
    check(defaults.rpc_rate_limit_max_requests_per_window == 0,
          "NodeConfig defaults to unlimited RPC request rate");
    check(defaults.rpc_max_connections == 0, "NodeConfig defaults to unlimited RPC connections");
    check(defaults.rpc_max_connections_per_ip == 0,
          "NodeConfig defaults to unlimited per-IP RPC connections");

    check(std::string(node::node_type_name(node::NodeType::FULL)) == "full",
          "full node type name is stable");
    check(std::string(node::node_type_name(node::NodeType::PRUNED)) == "pruned",
          "pruned node type name is stable");
    check(std::string(node::node_type_name(static_cast<node::NodeType>(99))) == "unknown",
          "unknown node type name is guarded");
}

void test_runtime_config_file_values() {
    const auto path = write_temp_config(
        "addnode=127.0.0.1:19000\n"
        "addnode=127.0.0.1:19001\n"
        "archive=1\n"
        "no-auto-connect=1\n"
        "activation-manifest=custom-activation.json\n"
        "policy-checkpoint-interval=55\n"
        "policy-checkpoint-confirmations=12\n"
        "allow-deep-reorg=1\n"
        "rpc-rate-limit=120\n"
        "rpc-max-connections=32\n"
        "rpc-max-connections-per-ip=8\n"
    );

    node::NodeConfig config;
    node::apply_runtime_config_from_file(config, path.string());

    check(config.seed_nodes.size() == 2, "addnode values are loaded from file");
    check(config.archive, "archive is loaded from runtime config");
    check(!config.enable_auto_connect, "no-auto-connect disables auto-connect");
    check(config.activation_manifest_path == "custom-activation.json", "activation manifest path is loaded");
    check(config.policy_checkpoint_interval == 55, "policy checkpoint interval is loaded");
    check(config.policy_checkpoint_confirmations == 12, "policy checkpoint confirmations are loaded");
    check(config.allow_deep_reorg, "allow-deep-reorg is loaded");
    check(config.rpc_rate_limit_max_requests_per_window == 120, "rpc-rate-limit is loaded");
    check(config.rpc_max_connections == 32, "rpc-max-connections is loaded");
    check(config.rpc_max_connections_per_ip == 8, "rpc-max-connections-per-ip is loaded");

    std::filesystem::remove(path);

    const auto unlimited_path = write_temp_config(
        "rpc-rate-limit=0\n"
        "rpc-max-connections=0\n"
        "rpc-max-connections-per-ip=0\n"
    );
    node::NodeConfig unlimited;
    unlimited.rpc_rate_limit_max_requests_per_window = 120;
    unlimited.rpc_max_connections = 16;
    unlimited.rpc_max_connections_per_ip = 4;
    node::apply_runtime_config_from_file(unlimited, unlimited_path.string());
    check(unlimited.rpc_rate_limit_max_requests_per_window == 0,
          "rpc-rate-limit=0 stays unlimited instead of falling back");
    check(unlimited.rpc_max_connections == 0, "rpc-max-connections=0 stays unlimited");
    check(unlimited.rpc_max_connections_per_ip == 0, "rpc-max-connections-per-ip=0 stays unlimited");
    std::filesystem::remove(unlimited_path);
}

void test_runtime_config_preserves_explicit_and_absolute_paths() {
    const auto path = write_temp_config(
        "archive=1\n"
        "no-auto-connect=0\n"
        "activation-manifest=/tmp/geocoin-activation.json\n"
        "policy-checkpoint-interval=64\n"
        "policy-checkpoint-confirmations=12\n"
        "allow-deep-reorg=off\n"
        "rpc-rate-limit=99\n"
        "rpc-max-connections=16\n"
        "rpc-max-connections-per-ip=4\n"
    );

    node::NodeConfig config;
    config.archive = true;
    config.enable_auto_connect = true;
    config.activation_manifest_path = "already-set.json";
    config.policy_checkpoint_interval = 10;
    config.policy_checkpoint_confirmations = 11;
    config.allow_deep_reorg = true;
    config.rpc_rate_limit_max_requests_per_window = 7;
    config.rpc_rate_limit_explicit = true;

    node::apply_runtime_config_from_file(config, path.string());

    check(config.archive, "archive stays enabled when already explicit true");
    check(config.enable_auto_connect, "no-auto-connect=0 keeps auto-connect enabled");
    check(config.activation_manifest_path == "already-set.json",
          "existing activation manifest path is preserved");
    check(config.policy_checkpoint_interval == 64,
          "valid policy checkpoint interval is applied");
    check(config.policy_checkpoint_confirmations == 12,
          "valid policy checkpoint confirmations are applied");
    check(!config.allow_deep_reorg, "allow-deep-reorg=off disables deep reorg");
    check(config.rpc_rate_limit_max_requests_per_window == 7,
          "explicit rpc-rate-limit is not overwritten by config file");
    check(config.rpc_max_connections == 16, "rpc-max-connections is loaded when not explicit");
    check(config.rpc_max_connections_per_ip == 4, "rpc-max-connections-per-ip is loaded when not explicit");

    node::NodeConfig relative;
    relative.data_dir = "/var/lib/geocoin";
    check(node::resolve_activation_manifest_path(relative) ==
              "/var/lib/geocoin/activation-manifest.json",
          "default activation manifest resolves under data dir");
    relative.activation_manifest_path = "rollout.json";
    check(node::resolve_activation_manifest_path(relative) == "/var/lib/geocoin/rollout.json",
          "relative activation manifest resolves under data dir");
    relative.activation_manifest_path = "/etc/geocoin/rollout.json";
    check(node::resolve_activation_manifest_path(relative) == "/etc/geocoin/rollout.json",
          "absolute activation manifest path is preserved");
    relative.activation_manifest_path = "C:\\geocoin\\rollout.json";
    check(node::resolve_activation_manifest_path(relative) == "C:\\geocoin\\rollout.json",
          "drive-style activation manifest path is preserved");

    std::filesystem::remove(path);
}

void test_cpu_mining_thread_limits() {
    const uint32_t limit = node::cpu_mining_thread_limit();
    check(limit >= 1, "CPU mining thread limit is at least one thread");
    check(limit <= node::MAX_CPU_MINING_THREADS, "CPU mining thread limit respects the global hard cap");
    check(node::clamp_cpu_mining_threads(0) == 1, "zero requested mining threads clamps to one");
    check(node::clamp_cpu_mining_threads(limit + 1024) == limit, "oversized mining thread request clamps to runtime limit");
}

void test_strict_numeric_config_values() {
    for (const auto& [body, label] : std::vector<std::pair<std::string, std::string>>{
             {"db-write-buffer-mb=64junk\n", "db-write-buffer-mb trailing junk"},
             {"db-cache-mb=1 2\n", "db-cache-mb internal whitespace"},
             {"db-max-background-jobs=4x\n", "db-max-background-jobs trailing junk"},
             {"pruned-blocks=42tail\n", "pruned-blocks trailing junk"},
         }) {
        const auto storage_path = write_temp_config(body);
        node::NodeConfig storage_config;
        bool rejected = false;
        try {
            node::apply_storage_config_from_file(storage_config, storage_path.string());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, label + " is rejected with a hard error");
        std::filesystem::remove(storage_path);
    }

    for (const auto& [body, label] : std::vector<std::pair<std::string, std::string>>{
             {"policy-checkpoint-interval=55junk\n", "policy-checkpoint-interval trailing junk"},
             {"policy-checkpoint-confirmations=1 2\n", "policy-checkpoint-confirmations whitespace"},
             {"policy-checkpoint-confirmations=-1\n", "negative checkpoint confirmations"},
             {"rpc-rate-limit=12x\n", "rpc-rate-limit trailing junk"},
             {"rpc-max-connections=1 2\n", "rpc-max-connections whitespace"},
         }) {
        const auto runtime_path = write_temp_config(body);
        node::NodeConfig runtime_config;
        bool rejected = false;
        try {
            node::apply_runtime_config_from_file(runtime_config, runtime_path.string());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, label + " is rejected with a hard error");
        std::filesystem::remove(runtime_path);
    }
}

void test_unknown_duplicate_and_versioned_config_are_rejected() {
    const auto unknown_path = write_temp_config("config-version=1\ntotally-unknown-option=1\n");
    node::NodeConfig unknown_config;
    bool unknown_threw = false;
    try {
        node::apply_storage_config_from_file(unknown_config, unknown_path.string());
    } catch (const std::runtime_error&) {
        unknown_threw = true;
    }
    check(unknown_threw, "unknown geocoin.conf options are rejected instead of ignored");
    std::filesystem::remove(unknown_path);

    const auto legacy_path = write_temp_config(
        "config-version=1\n"
        "network=degree\n"
        "rpcbind=127.0.0.1\n"
        "rpcport=3033\n"
        "listen=1\n"
        "port=3030\n"
    );
    node::NodeConfig legacy_config;
    node::apply_storage_config_from_file(legacy_config, legacy_path.string());
    node::apply_runtime_config_from_file(legacy_config, legacy_path.string());
    check(legacy_config.network == "degree" && legacy_config.p2p_port == 0 && legacy_config.rpc_port == 0,
          "published v1 runtime-only keys remain accepted but cannot override CLI/runtime state");
    std::filesystem::remove(legacy_path);

    const auto duplicate_path = write_temp_config("archive=0\narchive=1\n");
    node::NodeConfig duplicate_config;
    bool duplicate_threw = false;
    try {
        node::apply_runtime_config_from_file(duplicate_config, duplicate_path.string());
    } catch (const std::runtime_error&) {
        duplicate_threw = true;
    }
    check(duplicate_threw, "duplicate singleton geocoin.conf options are rejected");
    std::filesystem::remove(duplicate_path);

    const auto version_path = write_temp_config("config-version=2\n");
    node::NodeConfig version_config;
    bool version_threw = false;
    try {
        node::apply_storage_config_from_file(version_config, version_path.string());
    } catch (const std::runtime_error&) {
        version_threw = true;
    }
    check(version_threw, "unsupported geocoin.conf schema versions are rejected");
    std::filesystem::remove(version_path);
}

void test_node_key_secure_lifecycle() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const auto root = std::filesystem::temp_directory_path() / ("geocoin-node-key-" + suffix);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const bytes first = node::load_or_create_node_key(root);
    const bytes second = node::load_or_create_node_key(root);
    check(first.size() == 32, "node key generator returns exactly 32 bytes");
    check(first == second, "existing node identity key is loaded without rotation");
    check(std::filesystem::file_size(root / "node.key", ec) == 32 && !ec,
          "persisted node identity key has exact size");
#if !defined(_WIN32)
    struct stat st {};
    check(::lstat((root / "node.key").c_str(), &st) == 0 &&
              S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600,
          "node identity key is a regular owner-only file");
#endif
    std::filesystem::remove_all(root, ec);

    const auto malformed = std::filesystem::temp_directory_path() /
        ("geocoin-node-key-malformed-" + suffix);
    std::filesystem::create_directories(malformed, ec);
    {
        std::ofstream out(malformed / "node.key", std::ios::binary | std::ios::trunc);
        out << "bad";
    }
    bool malformed_threw = false;
    try {
        (void)node::load_or_create_node_key(malformed);
    } catch (const std::runtime_error&) {
        malformed_threw = true;
    }
    check(malformed_threw, "malformed node key is rejected instead of rotated");
    check(std::filesystem::file_size(malformed / "node.key", ec) == 3 && !ec,
          "malformed node key is not overwritten");
    std::filesystem::remove_all(malformed, ec);

#if !defined(_WIN32)
    const auto permissive_root = std::filesystem::temp_directory_path() /
        ("geocoin-node-key-permissive-" + suffix);
    std::filesystem::create_directories(permissive_root, ec);
    const auto permissive_key = permissive_root / "node.key";
    {
        std::ofstream out(permissive_key, std::ios::binary | std::ios::trunc);
        out << std::string(32, 'p');
    }
    check(::chmod(permissive_key.c_str(), 0644) == 0,
          "permissive node key fixture is made group/world-readable");
    bool permissive_threw = false;
    try {
        (void)node::load_or_create_node_key(permissive_root);
    } catch (const std::runtime_error&) {
        permissive_threw = true;
    }
    check(permissive_threw,
          "existing group/world-readable node identity key is rejected");
    struct stat permissive_stat {};
    check(::lstat(permissive_key.c_str(), &permissive_stat) == 0 &&
              (permissive_stat.st_mode & 0777) == 0644,
          "loader does not hide a prior node-key exposure by silently chmodding it");
    std::filesystem::remove_all(permissive_root, ec);

    const auto symlink_root = std::filesystem::temp_directory_path() /
        ("geocoin-node-key-symlink-" + suffix);
    std::filesystem::create_directories(symlink_root, ec);
    const auto target = symlink_root / "target";
    {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out << std::string(32, 'x');
    }
    std::filesystem::create_symlink(target, symlink_root / "node.key", ec);
    check(!ec, "node key symlink fixture is created");
    bool symlink_threw = false;
    try {
        (void)node::load_or_create_node_key(symlink_root);
    } catch (const std::runtime_error&) {
        symlink_threw = true;
    }
    check(symlink_threw, "node key symlink is rejected");
    std::filesystem::remove_all(symlink_root, ec);
#endif
}

void test_secret_file_security() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const auto root = std::filesystem::temp_directory_path() /
        ("geocoin-secret-file-" + suffix);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const auto secret_path = root / "rpc.pass";
    {
        std::ofstream out(secret_path, std::ios::binary | std::ios::trunc);
        out << "unit-secret\n";
    }
#if !defined(_WIN32)
    check(::chmod(secret_path.c_str(), 0600) == 0, "secret fixture permissions are restricted");
#endif
    check(utils::read_secret_file(secret_path, "test secret") == "unit-secret",
          "private one-line secret file is accepted");

#if !defined(_WIN32)
    check(::chmod(secret_path.c_str(), 0644) == 0, "insecure secret fixture permissions are set");
    bool public_threw = false;
    try {
        (void)utils::read_secret_file(secret_path, "test secret");
    } catch (const std::runtime_error&) {
        public_threw = true;
    }
    check(public_threw, "group/world-readable secret file is rejected");
    check(::chmod(secret_path.c_str(), 0600) == 0, "secret fixture permissions are restored");

    const auto link_path = root / "rpc-link.pass";
    std::filesystem::create_symlink(secret_path, link_path, ec);
    check(!ec, "secret symlink fixture is created");
    bool symlink_threw = false;
    try {
        (void)utils::read_secret_file(link_path, "test secret");
    } catch (const std::runtime_error&) {
        symlink_threw = true;
    }
    check(symlink_threw, "secret file symlink is rejected");
#endif

    {
        std::ofstream out(secret_path, std::ios::binary | std::ios::trunc);
        out << "first\nsecond\n";
    }
#if !defined(_WIN32)
    (void)::chmod(secret_path.c_str(), 0600);
#endif
    bool multiline_threw = false;
    try {
        (void)utils::read_secret_file(secret_path, "test secret");
    } catch (const std::runtime_error&) {
        multiline_threw = true;
    }
    check(multiline_threw, "multi-line secret file is rejected");
    std::filesystem::remove_all(root, ec);
}

}  // namespace

int main() {
    test_pruned_retention_required();
    test_pruned_archive_conflict();
    test_pruned_index_delete_normalization();
    test_full_config_allows_archive();
    test_storage_config_file_values();
    test_storage_config_false_aliases_and_explicit_overrides();
    test_invalid_config_values_are_rejected();
    test_default_config_and_node_type_names();
    test_runtime_config_file_values();
    test_runtime_config_preserves_explicit_and_absolute_paths();
    test_cpu_mining_thread_limits();
    test_strict_numeric_config_values();
    test_unknown_duplicate_and_versioned_config_are_rejected();
    test_node_key_secure_lifecycle();
    test_secret_file_security();

    if (failures == 0) {
        std::cout << "Node config loader tests passed.\n";
        return 0;
    }

    std::cerr << failures << " node config loader test(s) failed.\n";
    return 1;
}
