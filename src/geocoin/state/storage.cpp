#include "geocoin/state/storage.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/storage/backend_support.hpp"

#include <filesystem>

namespace geocoin::state {

std::unique_ptr<Storage> StorageFactory::create(
    Type type,
    const std::string& path,
    StorageOptions options
) {
    return create_split(type, path, "", options);
}

std::unique_ptr<Storage> StorageFactory::create_split(
    Type type,
    const std::string& chaindata_path,
    const std::string& addresses_path,
    StorageOptions options
) {
    switch (type) {
        case Type::MEMORY:
            return std::make_unique<MemoryStorage>();
        case Type::ROCKSDB: {
            if (!chaindata_path.empty()) {
                std::filesystem::create_directories(std::filesystem::path(chaindata_path).parent_path());
            }
            if (!addresses_path.empty()) {
                std::filesystem::create_directories(std::filesystem::path(addresses_path).parent_path());
            }
            auto storage = std::make_unique<RocksDBStorage>(chaindata_path, addresses_path, options);
            if (!storage->open()) {
                GEO_LOG_ERROR("storage", "Failed to open RocksDB storage at: " << chaindata_path);
                return nullptr;
            }
            return storage;
        }

        default:
            return nullptr;
    }
}

std::optional<StorageFactory::Type> StorageFactory::parse_type(const std::string& value) {
    const std::string normalized = storage::backend::normalize_storage_value(value);
    if (normalized == "memory" || normalized == "mem") {
        return Type::MEMORY;
    }
    if (normalized == "rocksdb" || normalized == "rocks") {
        return Type::ROCKSDB;
    }
    return std::nullopt;
}

std::optional<StorageProfile> StorageFactory::parse_profile(const std::string& value) {
    const std::string normalized = storage::backend::normalize_storage_value(value);
    if (normalized == "safe") {
        return StorageProfile::SAFE;
    }
    if (normalized == "fast-sync" || normalized == "fastsync" || normalized == "fast") {
        return StorageProfile::FAST_SYNC;
    }
    if (normalized == "unsafe-benchmark" || normalized == "unsafe" || normalized == "benchmark") {
        return StorageProfile::UNSAFE_BENCHMARK;
    }
    return std::nullopt;
}

const char* StorageFactory::type_name(Type type) {
    switch (type) {
        case Type::MEMORY:
            return "memory";
        case Type::ROCKSDB:
            return "rocksdb";
        default:
            return "unknown";
    }
}

const char* StorageFactory::profile_name(StorageProfile profile) {
    switch (profile) {
        case StorageProfile::SAFE:
            return "safe";
        case StorageProfile::FAST_SYNC:
            return "fast-sync";
        case StorageProfile::UNSAFE_BENCHMARK:
            return "unsafe-benchmark";
        default:
            return "unknown";
    }
}

bool StorageFactory::rocksdb_available() {
    return true;
}

} // namespace geocoin::state
