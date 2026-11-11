#include "test_utils.hpp"
#include "geocoin/state/storage.hpp"

#include <chrono>
#include <iostream>
#include <map>
#include <random>

using namespace geocoin;
using test::expect;

namespace {
std::string numbered(const std::string& prefix, size_t value) {
    auto number = std::to_string(value);
    return prefix + std::string(8 - number.size(), '0') + number;
}

void check_pages(const state::Storage& storage, const std::map<std::string, bytes>& expected) {
    for (const std::string prefix : {"", "a", "addr:", "probe:", "utxo:", "missing:"}) {
        for (const std::string after : {"", "!", "addr:00000012", "probe:00000017", "zz"}) {
            std::vector<std::string> matching;
            for (const auto& [key, value] : expected) {
                if (key.starts_with(prefix) && (after.empty() || key > after)) matching.push_back(key);
            }
            for (size_t offset : {0u, 1u, 17u, 90u}) {
                for (size_t limit : {0u, 1u, 7u, 100u}) {
                    std::vector<std::string> page;
                    for (size_t i = offset; i < matching.size() && page.size() < limit; ++i) page.push_back(matching[i]);
                    size_t total = 999;
                    expect(storage.get_keys_page(prefix, offset, limit, &total, after) == page && total == matching.size(),
                           "counted page matches visible records after cursor");
                    expect(storage.get_keys_page(prefix, offset, limit, nullptr, after) == page,
                           "bounded page matches counted page");
                }
            }
        }
    }
    std::vector<std::string> all;
    for (const auto& [key, value] : expected) all.push_back(key);
    expect(storage.get_all_keys() == all, "complete scan is sorted and unique");
}

void contract(state::Storage& storage) {
    std::map<std::string, bytes> expected;
    storage.begin_batch();
    expect(storage.put_metadata("", {1}), "seed empty metadata key");
    expected[""] = {1};
    for (const std::string prefix : {"addr:", "probe:", "utxo:"}) {
        for (size_t i = 0; i < 30; ++i) {
            auto key = numbered(prefix, i);
            expect(storage.put_metadata(key, {1}), "seed metadata");
            expected[key] = {1};
        }
    }
    expect(storage.commit_batch(), "commit seed");
    check_pages(storage, expected);
    const auto committed = expected;
    storage.begin_batch();
    std::mt19937 random(741);
    for (size_t i = 0; i < 180; ++i) {
        const auto key = numbered(i % 2 ? "addr:" : "probe:", random() % 45);
        if (random() % 3 == 0) {
            expect(storage.delete_metadata(key), "stage removal");
            expected.erase(key);
        } else {
            expect(storage.put_metadata(key, {2}), "stage replacement/insertion");
            expected[key] = {2};
        }
    }
    check_pages(storage, expected);
    storage.abort_batch();
    check_pages(storage, committed);
}

void benchmark(state::Storage& storage, const std::string& backend) {
    storage.begin_batch();
    for (size_t i = 0; i < 10000; ++i) expect(storage.put_metadata(numbered("scan:", i), {1}), "benchmark seed");
    expect(storage.commit_batch(), "benchmark seed commit");
    const auto measure = [&](const char* phase) {
        const auto start = std::chrono::steady_clock::now();
        size_t returned = 0;
        for (int i = 0; i < 200; ++i) {
            const auto page = storage.get_keys_page("scan:", 0, 8, nullptr, numbered("scan:", 4000 + i));
            expect(page.size() == 8, "benchmark page length");
            returned += page.size();
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        std::cout << "SCAN backend=" << backend << " phase=" << phase
                  << " keys=10000 queries=200 page=8 returned=" << returned << " elapsed_us=" << elapsed << std::endl;
    };
    measure("committed");
    storage.begin_batch();
    expect(storage.put_metadata("scan:00005000", {2}), "benchmark pending replacement");
    measure("batch");
    storage.abort_batch();
}
}

int main(int argc, char** argv) {
    const auto directory = test::make_temp_dir("geocoin-scan");
    try {
        const bool measure = argc == 2 && std::string(argv[1]) == "--benchmark";
        expect(argc == 1 || measure, "usage: test_storage_scan [--benchmark]");
        {
            state::MemoryStorage memory;
            measure ? benchmark(memory, "memory") : contract(memory);
            state::RocksDBStorage single((directory / "single").string());
            expect(single.open(), "open single DB");
            measure ? benchmark(single, "rocks-single") : contract(single);
            state::RocksDBStorage split((directory / "chain").string(), (directory / "addresses").string());
            expect(split.open(), "open split DB");
            measure ? benchmark(split, "rocks-split") : contract(split);
        }
        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
