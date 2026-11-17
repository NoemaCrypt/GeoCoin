#include "geocoin/state/storage.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/utils/filesystem.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

static std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    const fs::path root = fs::absolute("out/test-storage-open") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        // UTF-8 Cyrillic plus spaces, independent of the build host's codepage.
        fs::path directory = root / fs::u8path("\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82 wallet data");
        for (int i = 0; i < 5; ++i) directory /= std::string(48, 'a' + i);
        directory = geocoin::utils::path_from_utf8(utf8(directory));
        const auto database = utf8(directory / "state");
        fs::create_directories(directory);
        {
            geocoin::state::RocksDBStorage first(database);
            if (!first.open()) throw std::runtime_error(first.last_open_error());
            check(first.put_metadata("probe", geocoin::bytes{1, 2, 3}), "Initial metadata write failed");
            std::string error;
            auto second = geocoin::state::StorageFactory::create_split(
                geocoin::state::StorageFactory::Type::ROCKSDB, database, "", {}, &error);
            check(!second, "Second owner incorrectly acquired an open database");
            check(error.find(database) != std::string::npos &&
                  (error.find("lock") != std::string::npos || error.find("LOCK") != std::string::npos),
                  "Busy database lost original path/lock status");
        }
        for (int i = 0; i < 3; ++i) {
            geocoin::state::RocksDBStorage reopened(database);
            if (!reopened.open()) throw std::runtime_error(reopened.last_open_error());
            geocoin::bytes value;
            check(reopened.get_metadata("probe", value) && value == geocoin::bytes({1, 2, 3}),
                  "Restart lost persisted metadata");
        }
        {
            geocoin::storage::BlockStorage blocks(utf8(directory / "node"));
            if (!blocks.is_open()) throw std::runtime_error(blocks.last_open_error());
        }
        const auto file = root / "not-a-directory";
        { std::ofstream output(file); output << "sentinel"; }
        geocoin::state::RocksDBStorage invalid(utf8(file / "state"));
        check(!invalid.open() && !invalid.last_open_error().empty(), "Invalid path lacks diagnostics");
        // Only this test-owned directory, after every database owner is destroyed.
#ifdef _WIN32
        fs::remove_all(fs::path(L"\\\\?\\" + root.native()));
#else
        fs::remove_all(root);
#endif
        std::cout << "Storage lock, UTF-8/long paths and reopen diagnostics passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
