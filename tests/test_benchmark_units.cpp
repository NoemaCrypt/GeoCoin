#include "geocoin/benchmark/node_benchmark.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using geocoin::benchmark::BenchmarkErrorCode;
using geocoin::benchmark::BenchmarkException;
using geocoin::benchmark::json;

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_benchmark_error(
    BenchmarkErrorCode code,
    const char* message,
    const std::function<void()>& fn
)
{
    try {
        fn();
    } catch (const BenchmarkException& e) {
        if (e.code() != code) {
            throw std::runtime_error(std::string(message) + ": wrong error code");
        }
        return;
    }
    throw std::runtime_error(message);
}

std::filesystem::path make_temp_dir(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path()
        / (name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

json minimal_options()
{
    return {
        {"iterations", 1},
        {"data_size", 8},
        {"pbkdf2_iterations", 1},
        {"falcon_iterations", 1},
    };
}

void test_list_and_dispatch()
{
    using namespace geocoin::benchmark;

    const json listed = list_benchmarks();
    expect(listed["mode"] == "cli", "benchmark mode is listed");
    expect(listed["suites"].contains("db"), "db suite is listed");
    expect(listed["suites"].contains("crypto"), "crypto suite is listed");
    expect(listed["suites"].contains("falcon"), "falcon suite is listed");

    const auto tmp = make_temp_dir("geocoin-benchmark-dispatch");
    const json crypto = run_benchmark(tmp, "compute", json{
        {"iterations", 1},
        {"data_size", 8},
        {"operations", "sha256"},
    });
    expect(crypto["suite"] == "crypto", "compute alias dispatches to crypto benchmark");
    expect(crypto.contains("elapsed_ms"), "dispatch adds elapsed time");
    expect(crypto.contains("sha256"), "selected crypto operation is present");

    const json falcon = run_benchmark(tmp, "falcon", json{
        {"falcon_iterations", 1},
    });
    expect(falcon["suite"] == "falcon", "falcon suite label is preserved");
    expect(falcon.contains("falcon512_score"), "falcon suite runs score by default");

    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "unknown suite rejects", [&] {
        (void)run_benchmark(tmp, "missing", json::object());
    });
    std::filesystem::remove_all(tmp);
}

void test_crypto_benchmark_operations_and_validation()
{
    using namespace geocoin::benchmark;

    json options = minimal_options();
    options["operations"] = json::array({
        "sha256",
        "double_sha256",
        "ripemd160",
        "pbkdf2",
        "aes_encrypt",
        "aes_decrypt",
        "falcon_public_key",
        "falcon_sign",
        "falcon_verify",
        "falcon_verify_packed",
        "falcon_recover_address",
        "falcon_score",
        "verify_address",
    });

    const json result = run_crypto_benchmark(options);
    expect(result["suite"] == "crypto", "crypto suite returns correct label");
    expect(result["iterations"] == 1, "crypto iterations option is honored");
    expect(result["data_size"] == 8, "crypto data size option is honored");
    expect(result["pbkdf2_iterations"] == 1, "pbkdf2 option is honored");
    expect(result["falcon_iterations"] == 1, "falcon iteration option is honored");
    expect(result["operations"].size() == 13, "all selected crypto operations are recorded");
    expect(result.contains("sha256"), "sha256 result exists");
    expect(result.contains("double_sha256"), "double sha256 result exists");
    expect(result.contains("ripemd160"), "ripemd160 result exists");
    expect(result.contains("pbkdf2_hmac_sha256"), "pbkdf2 result exists");
    expect(result.contains("aes256_gcm_encrypt"), "aes encrypt result exists");
    expect(result.contains("aes256_gcm_decrypt"), "aes decrypt result exists");
    expect(result.contains("falcon512_public_key_from_seed32"), "falcon public key result exists");
    expect(result.contains("falcon512_sign_digest32"), "falcon sign result exists");
    expect(result.contains("falcon512_verify_digest32"), "falcon verify result exists");
    expect(result.contains("falcon512_verify_packed_message"), "packed verify result exists");
    expect(result.contains("falcon512_recover_address"), "address recovery result exists");
    expect(result.contains("falcon512_score"), "falcon score result exists");
    expect(result.contains("verify_address"), "address verification result exists");
    expect(result.contains("sample_address"), "address benchmark exposes sample address");

    json slow = minimal_options();
    slow["operations"] = json::array({"pbkdf2", "aes_encrypt"});
    slow["include_slow"] = true;
    const json slow_result = run_crypto_benchmark(slow);
    expect(slow_result.contains("slow_profiles"), "slow crypto profiles are optional and explicit");

    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "integer type validation rejects", [&] {
        json bad = minimal_options();
        bad["iterations"] = "1";
        (void)run_crypto_benchmark(bad);
    });
    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "integer range validation rejects", [&] {
        json bad = minimal_options();
        bad["data_size"] = 0;
        (void)run_crypto_benchmark(bad);
    });
    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "bool validation rejects", [&] {
        json bad = minimal_options();
        bad["include_slow"] = "yes";
        (void)run_crypto_benchmark(bad);
    });
    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "operation type validation rejects", [&] {
        json bad = minimal_options();
        bad["operations"] = 42;
        (void)run_crypto_benchmark(bad);
    });
    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "operation entry validation rejects", [&] {
        json bad = minimal_options();
        bad["operations"] = json::array({"sha256", 42});
        (void)run_crypto_benchmark(bad);
    });
    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "empty operation list rejects", [&] {
        json bad = minimal_options();
        bad["operations"] = json::array();
        (void)run_crypto_benchmark(bad);
    });
    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "unknown operation rejects", [&] {
        json bad = minimal_options();
        bad["operations"] = "unknown";
        (void)run_crypto_benchmark(bad);
    });
}

void test_db_benchmark_operations_and_validation()
{
    using namespace geocoin::benchmark;

    const auto tmp = make_temp_dir("geocoin-benchmark-db");
    const json base = {
        {"iterations", 2},
        {"value_size", 4},
        {"scan_repeats", 1},
    };

    json all_options = base;
    all_options["operations"] = json::array({"put", "get", "scan", "delete"});
    const json all = run_db_benchmark(tmp, all_options);
    expect(all["suite"] == "db", "db suite returns correct label");
    expect(all["backend"] == "rocksdb", "db backend is rocksdb");
    expect(all["iterations"] == 2, "db iterations option is honored");
    expect(all["value_size"] == 4, "db value size option is honored");
    expect(all.contains("put"), "db put result exists");
    expect(all.contains("get"), "db get result exists");
    expect(all.contains("scan"), "db scan result exists");
    expect(all.contains("delete"), "db delete result exists");
    expect(all["scan"]["rows"].get<uint64_t>() >= 2, "scan observes prepared rows");
    expect(all.contains("properties"), "db properties are collected");

    for (const std::string op : {"get", "scan", "delete"}) {
        json options = base;
        options["operations"] = op;
        const json result = run_db_benchmark(tmp, options);
        expect(result["operations"].size() == 1, "single db operation is recorded");
        expect(result["operations"][0] == op, "selected db operation is recorded");
        expect(result.contains(op), "single selected db operation runs");
    }

    json keep_after_delete = base;
    keep_after_delete["operations"] = "delete";
    keep_after_delete["cleanup"] = false;
    const json kept = run_db_benchmark(tmp, keep_after_delete);
    expect(kept["cleanup"] == false, "cleanup option is honored");
    expect(kept.contains("delete"), "delete benchmark runs with cleanup disabled");

    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "db integer type validation rejects", [&] {
        json bad = base;
        bad["value_size"] = "4";
        (void)run_db_benchmark(tmp, bad);
    });
    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "db integer range validation rejects", [&] {
        json bad = base;
        bad["scan_repeats"] = 0;
        (void)run_db_benchmark(tmp, bad);
    });
    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "db bool validation rejects", [&] {
        json bad = base;
        bad["cleanup"] = "no";
        (void)run_db_benchmark(tmp, bad);
    });
    expect_benchmark_error(BenchmarkErrorCode::INVALID_PARAMETER, "db operation validation rejects", [&] {
        json bad = base;
        bad["operations"] = "compact";
        (void)run_db_benchmark(tmp, bad);
    });
    std::filesystem::remove_all(tmp);
}

} // namespace

int main()
{
    try {
        test_list_and_dispatch();
        test_crypto_benchmark_operations_and_validation();
        test_db_benchmark_operations_and_validation();
        std::cout << "test_benchmark_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_benchmark_units failed: " << e.what() << "\n";
        return 1;
    }
}
