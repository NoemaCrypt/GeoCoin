#include "geocoin/rpc/rpc_server.hpp"
#include "rpc_geography.hpp"

#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/storage/backend_support.hpp"
#include <rocksdb/cache.h>
#if __has_include(<rocksdb/advanced_cache.h>)
#include <rocksdb/advanced_cache.h>
#endif
#include <rocksdb/write_buffer_manager.h>
#include "geocoin/consensus/geo_tag.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/utils/metrics.hpp"
#include <filesystem>
#include <limits>
#include <system_error>
#include <utility>

namespace geocoin::rpc {
namespace {

const char* storage_backend_to_string(storage::StorageBackend backend) {
    switch (backend) {
        case storage::StorageBackend::ROCKSDB:
            return "rocksdb";
    }
    return "unknown";
}

const char* storage_profile_to_string(storage::StorageProfile profile) {
    switch (profile) {
        case storage::StorageProfile::SAFE:
            return "safe";
        case storage::StorageProfile::FAST_SYNC:
            return "fast-sync";
        case storage::StorageProfile::UNSAFE_BENCHMARK:
            return "unsafe-benchmark";
    }
    return "unknown";
}

constexpr size_t kMaxDiagnosticDirectoryEntries = 100'000;

struct DirectoryUsage {
    uint64_t size_bytes = 0;
    size_t scanned_entries = 0;
    bool truncated = false;
};

DirectoryUsage directory_usage(const std::filesystem::path& path) {
    DirectoryUsage usage;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return usage;
    }

    std::filesystem::recursive_directory_iterator it(
        path,
        std::filesystem::directory_options::skip_permission_denied,
        ec
    );
    std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (++usage.scanned_entries > kMaxDiagnosticDirectoryEntries) {
            usage.truncated = true;
            break;
        }

        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec)) {
            const uint64_t file_size = static_cast<uint64_t>(it->file_size(entry_ec));
            if (!entry_ec) {
                if (file_size > (std::numeric_limits<uint64_t>::max)() - usage.size_bytes) {
                    usage.size_bytes = (std::numeric_limits<uint64_t>::max)();
                    usage.truncated = true;
                    break;
                }
                usage.size_bytes += file_size;
            }
        }
    }
    if (ec) {
        usage.truncated = true;
    }
    return usage;
}

json directory_info(const std::filesystem::path& path) {
    std::error_code ec;
    const DirectoryUsage usage = directory_usage(path);
    json out;
    out["path"] = path.string();
    out["exists"] = std::filesystem::exists(path, ec);
    out["size_bytes"] = usage.size_bytes;
    out["scanned_entries"] = usage.scanned_entries;
    out["truncated"] = usage.truncated;
    return out;
}

json block_storage_options_to_json(const storage::StorageOptions& opts) {
    json options;
    options["profile"] = storage_profile_to_string(opts.profile);
    options["write_buffer_mb"] = opts.write_buffer_mb;
    options["cache_mb"] = opts.cache_mb;
    options["max_background_jobs"] = opts.max_background_jobs;
    options["disable_wal_during_ibd"] = opts.disable_wal_during_ibd;
    options["memory_budget_scope"] = opts.shared_cache && opts.shared_write_buffer ? "all_four_databases" : "this_storage";
    if (opts.shared_cache) {
        options["cache_capacity_bytes"] = opts.shared_cache->GetCapacity();
        options["cache_usage_bytes"] = opts.shared_cache->GetUsage();
    }
    if (opts.shared_write_buffer) {
        options["memtable_budget_bytes"] = opts.shared_write_buffer->buffer_size();
        options["memtable_usage_bytes"] = opts.shared_write_buffer->memory_usage();
    }
    return options;
}

json block_storage_info_to_json(storage::BlockStorage& block_storage) {
    json out;
    out["backend"] = storage_backend_to_string(block_storage.get_backend());
    out["base_path"] = block_storage.get_base_path();
    out["chaindata_path"] = block_storage.get_chaindata_path();
    out["index_path"] = block_storage.get_index_path();
    out["open"] = block_storage.is_open();
    out["block_count"] = block_storage.get_block_count();
    out["block_count_meaning"] = "stored_block_bodies";
    out["size_bytes"] = block_storage.get_size();
    out["options"] = block_storage_options_to_json(block_storage.get_options());
    out["format_version"] = storage::backend::kFormatVersion;
    out["network"] = block_storage.get_options().network;
    out["genesis"] = block_storage.get_options().genesis_hex;
    return out;
}

} // namespace

void RPCServer::register_misc_diagnostics_methods() {
    register_method({
        .name = "powdeterminism",
        .method = [this](const json& params) -> json {
            (void)this;
            (void)params;

            core::BlockHeader header;
            header.version = 1;
            header.timestamp = 1700000000U;
            header.bits = 0x1e0fffff;
            header.nonce = 123456789ULL;
            header.prev_block_hash = {};
            header.merkle_root = {};

            const uint32_t prev_timestamp = 0U;
            const Hash256 prev_hash = {};

            const auto tag = consensus::geo::compute_geo_tag(prev_timestamp, prev_hash);
            header.geo_tag = tag.geo_tag;
            Hash256 pow_hash = consensus::ProofOfWork::compute_pow_hash(header, prev_timestamp, prev_hash, 0);

            json result;
            result["header"] = {
                {"version", header.version},
                {"timestamp", header.timestamp},
                {"bits", header.bits},
                {"nonce", header.nonce},
                {"geo_tag", header.geo_tag},
                {"prev_block_hash", hash256_to_hex_string(header.prev_block_hash)},
                {"merkle_root", hash256_to_hex_string(header.merkle_root)}
            };

            result["geo_tag"] = tag.geo_tag;
            result["latitude_ndeg"] = tag.lat_ndeg;
            result["longitude_ndeg"] = tag.lon_ndeg;
            result["geography"] = geo_tag_to_json(tag.geo_tag, tag);
            result["pow_hash"] = hash256_to_hex_string(pow_hash);
            return result;
        },
        .category = "util",
        .help = "powdeterminism\n\nSelf-check детерминизма GeoPoW. geography использует WGS 84 / OGC:CRS84, GeoJSON [долгота,широта] в градусах и целочисленные наноградусы.\n\nРезультат:\n{\n  \"header\": { ... },\n  \"geo_tag\": n,\n  \"latitude_ndeg\": n,\n  \"longitude_ndeg\": n,\n  \"pow_hash\": \"...\"\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "getmetrics",
        .method = [this](const json& params) -> json {
            (void)this;
            (void)params;

            json result;
            auto& registry = utils::MetricsRegistry::instance();

            json gauges = json::object();
            for (const auto& [key, value] : registry.snapshot_gauges()) {
                gauges[key] = value;
            }

            json counters = json::object();
            for (const auto& [key, value] : registry.snapshot_counters()) {
                counters[key] = value;
            }

            result["gauges"] = std::move(gauges);
            result["counters"] = std::move(counters);
            return result;
        },
        .category = "util",
        .help = "getmetrics\n\nВозвращает все метрики ноды.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });

    register_method({
        .name = "getstorageinfo",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& node = require_node_info();

            const auto cfg = node.get_config();
            const std::filesystem::path data_dir = cfg.data_dir;
            json result;
            result["backend"] = cfg.db_backend;
            result["profile"] = cfg.db_profile;
            result["data_dir"] = data_dir.string();

            json directories;
            directories["data"] = directory_info(data_dir);
            directories["chaindata"] = directory_info(data_dir / "chaindata");
            directories["addresses"] = directory_info(data_dir / "addresses");
            directories["index"] = directory_info(data_dir / "index");
            json databases;
            for (const auto* relative : {"chaindata/blocks", "index/blocks", "chaindata/state", "addresses/state"}) {
                databases[relative] = directory_info(data_dir / relative);
            }
            result["databases"] = std::move(databases);
            result["directories"] = std::move(directories);

            if (blockchain_) {
                auto block_storage = blockchain_->get_block_storage();
                if (block_storage) {
                    result["block_storage"] = block_storage_info_to_json(*block_storage);
                }
            }
            return result;
        },
        .category = "util",
        .help = "getstorageinfo\n\nВозвращает layout и размеры data-dir: chaindata, addresses, index и block storage.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::ADMIN,
        .capabilities = {"diagnostics"}
    });

    register_method({
        .name = "getdbstats",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& node = require_node_info();

            const auto cfg = node.get_config();
            const std::filesystem::path data_dir = cfg.data_dir;
            const auto to_mb = [](uint64_t bytes) {
                return static_cast<double>(bytes) / 1024.0 / 1024.0;
            };

            json dirs;
            dirs["data"] = directory_info(data_dir);
            dirs["chaindata"] = directory_info(data_dir / "chaindata");
            dirs["chaindata_blocks"] = directory_info(data_dir / "chaindata" / "blocks");
            dirs["chaindata_state"] = directory_info(data_dir / "chaindata" / "state");
            dirs["addresses"] = directory_info(data_dir / "addresses");
            dirs["addresses_state"] = directory_info(data_dir / "addresses" / "state");
            dirs["index"] = directory_info(data_dir / "index");
            dirs["index_blocks"] = directory_info(data_dir / "index" / "blocks");

            json sizes;
            for (const auto& [name, info] : dirs.items()) {
                const uint64_t bytes = info.value("size_bytes", 0ULL);
                sizes[name] = {
                    {"bytes", bytes},
                    {"mb", to_mb(bytes)}
                };
            }

            json result;
            result["backend"] = cfg.db_backend;
            result["profile"] = cfg.db_profile;
            result["data_dir"] = data_dir.string();
            result["directories"] = std::move(dirs);
            result["sizes"] = std::move(sizes);
            result["rocksdb"] = {
                {"enabled", cfg.db_backend == "rocksdb"},
                {"layout", json::array({"addresses", "chaindata", "index"})}
            };

            if (blockchain_) {
                auto block_storage = blockchain_->get_block_storage();
                if (block_storage) {
                    json bs = block_storage_info_to_json(*block_storage);
                    bs["size_mb"] = to_mb(block_storage->get_size());
                    result["block_storage"] = std::move(bs);
                }
            }

            return result;
        },
        .category = "util",
        .help = "getdbstats\n\nReturns normalized RocksDB/storage layout, directory sizes and active DB options.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::ADMIN,
        .capabilities = {"diagnostics"}
    });

    register_method({
        .name = "getpruninginfo",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& node = require_node_info();

            const auto pruned = node.get_pruned_data_info();
            json result;
            result["node_type"] = pruned.enabled ? "pruned" : "full";
            result["pruned"] = pruned.enabled;
            result["retention_blocks"] = pruned.retention;
            result["first_available_height"] = pruned.first_available_height;
            result["tip_height"] = pruned.tip_height;

            const auto cfg = node.get_config();
            result["delete_old_blocks"] = cfg.pruned_delete_old_blocks;
            result["delete_old_indexes"] = cfg.pruned_delete_old_indexes;

            if (chain_state_) {
                result["archive"] = chain_state_->is_archive_mode();
                result["witness_prune_depth"] = chain_state_->get_prune_depth();
                Height witness_pruned_height = 0;
                if (chain_state_->get_witness_pruned_height(witness_pruned_height)) {
                    result["witness_pruned_height"] = witness_pruned_height;
                }
                storage::BlockStorage::PruningStats stats;
                if (chain_state_->get_pruning_stats(stats)) {
                    json s;
                    s["total_blocks"] = stats.total_blocks;
                    s["pruned_blocks"] = stats.pruned_blocks;
                    s["body_bytes_reduced"] = stats.body_bytes_reduced;
                    s["bytes_total"] = stats.bytes_total;
                    s["body_reduction_percent"] = stats.bytes_total == 0
                        ? 0.0
                        : (static_cast<double>(stats.body_bytes_reduced) * 100.0 /
                           static_cast<double>(stats.bytes_total));
                    result["witness_pruning_stats"] = std::move(s);
                }
            }
            return result;
        },
        .category = "blockchain",
        .help = "getpruninginfo\n\nВозвращает настройки и фактическое окно доступной истории Full/Pruned-ноды.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY
    });
}

} // namespace geocoin::rpc
