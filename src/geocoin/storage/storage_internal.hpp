#pragma once

#include "geocoin/utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/cache.h>
#include <rocksdb/table.h>
#include <rocksdb/version.h>

namespace geocoin::storage::internal {

inline size_t mb_to_bytes(size_t mb) {
    constexpr size_t unit = 1024 * 1024;
    if (mb > (std::numeric_limits<size_t>::max)() / unit)
        throw std::invalid_argument("Database memory budget overflows byte size");
    return mb * unit;
}

inline std::string normalize_storage_value(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline void apply_block_cache_options(rocksdb::Options& options, size_t cache_mb) {
    if (cache_mb == 0) {
        return;
    }

    rocksdb::BlockBasedTableOptions table_options;
    table_options.block_cache = rocksdb::NewLRUCache(mb_to_bytes(cache_mb));
    table_options.cache_index_and_filter_blocks = true;
    table_options.pin_l0_filter_and_index_blocks_in_cache = true;
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
}

inline std::filesystem::path backend_marker_path(const std::string& db_path) {
    return std::filesystem::path(db_path).replace_extension(".backend");
}

inline bool directory_has_data(const std::string& db_path) {
    std::error_code ec;
    if (!std::filesystem::exists(db_path, ec)) {
        return false;
    }
    if (!std::filesystem::is_directory(db_path, ec)) {
        return true;
    }
    return !std::filesystem::is_empty(db_path, ec);
}

inline bool ensure_backend_marker(
    const std::string& db_path,
    const char* backend,
    const char* storage_label
) {
    const auto marker = backend_marker_path(db_path);
    std::error_code ec;
    if (std::filesystem::exists(marker, ec)) {
        std::ifstream in(marker);
        std::string existing;
        std::getline(in, existing);
        existing = normalize_storage_value(existing);
        if (existing == backend) {
            return true;
        }
        GEO_LOG_ERROR(
            "storage",
            storage_label << " backend mismatch for " << db_path << ": marker says '"
            << existing << "', requested '" << backend << "'"
        );
        return false;
    }

    if (directory_has_data(db_path)) {
        GEO_LOG_ERROR(
            "storage",
            "Refusing to open existing unmarked " << storage_label << " at " << db_path
            << " as " << backend << ". Use a fresh data directory."
        );
        return false;
    }

    std::filesystem::create_directories(marker.parent_path(), ec);
    std::ofstream out(marker, std::ios::out | std::ios::trunc);
    out << backend << "\n";
    out.flush();
    return out.good();
}

inline rocksdb::Status open_rocksdb(
    const rocksdb::Options& options,
    const std::string& path,
    std::unique_ptr<rocksdb::DB>& db
) {
#if defined(ROCKSDB_MAJOR) && ROCKSDB_MAJOR >= 11
    return rocksdb::DB::Open(options, path, &db);
#else
    rocksdb::DB* db_raw = nullptr;
    rocksdb::Status status = rocksdb::DB::Open(options, path, &db_raw);
    if (status.ok()) {
        db.reset(db_raw);
    }
    return status;
#endif
}

inline rocksdb::Status open_rocksdb_with_column_families(
    const rocksdb::DBOptions& options,
    const std::string& path,
    const std::vector<rocksdb::ColumnFamilyDescriptor>& column_families,
    std::vector<rocksdb::ColumnFamilyHandle*>* handles,
    std::unique_ptr<rocksdb::DB>& db
) {
#if defined(ROCKSDB_MAJOR) && ROCKSDB_MAJOR >= 11
    return rocksdb::DB::Open(options, path, column_families, handles, &db);
#else
    rocksdb::DB* db_raw = nullptr;
    rocksdb::Status status = rocksdb::DB::Open(options, path, column_families, handles, &db_raw);
    if (status.ok()) {
        db.reset(db_raw);
    }
    return status;
#endif
}

} // namespace geocoin::storage::internal
