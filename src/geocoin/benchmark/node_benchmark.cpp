#include "geocoin/benchmark/node_benchmark.hpp"

#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/encryption.hpp"
#include "geocoin/crypto/falcon512.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/storage/backend_support.hpp"
#include "geocoin/types.hpp"

#include <openssl/crypto.h>
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace geocoin::benchmark {

static uint64_t benchmark_u64_option(
    const json& options,
    const char* name,
    uint64_t default_value,
    uint64_t min_value,
    uint64_t max_value
) {
    if (!options.contains(name) || options[name].is_null()) {
        return default_value;
    }
    if (!options[name].is_number_integer() && !options[name].is_number_unsigned()) {
        throw BenchmarkException(BenchmarkErrorCode::INVALID_PARAMETER, std::string(name) + " must be an integer");
    }
    uint64_t value = options[name].get<uint64_t>();
    if (value < min_value || value > max_value) {
        std::ostringstream oss;
        oss << name << " must be in range [" << min_value << ", " << max_value << "]";
        throw BenchmarkException(BenchmarkErrorCode::INVALID_PARAMETER, oss.str());
    }
    return value;
}

static bool benchmark_bool_option(const json& options, const char* name, bool default_value) {
    if (!options.contains(name) || options[name].is_null()) {
        return default_value;
    }
    if (!options[name].is_boolean()) {
        throw BenchmarkException(BenchmarkErrorCode::INVALID_PARAMETER, std::string(name) + " must be boolean");
    }
    return options[name].get<bool>();
}

static std::string join_benchmark_operations(const std::vector<std::string>& operations) {
    std::ostringstream oss;
    for (size_t i = 0; i < operations.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << operations[i];
    }
    return oss.str();
}

static std::vector<std::string> benchmark_operations_option(
    const json& options,
    const std::vector<std::string>& defaults,
    const std::vector<std::string>& allowed
) {
    if (!options.contains("operations") || options["operations"].is_null()) {
        return defaults;
    }

    std::vector<std::string> selected;
    const auto add_operation = [&](const json& item) {
        if (!item.is_string()) {
            throw BenchmarkException(BenchmarkErrorCode::INVALID_PARAMETER, "operations entries must be strings");
        }
        const std::string op = item.get<std::string>();
        if (std::find(allowed.begin(), allowed.end(), op) == allowed.end()) {
            throw BenchmarkException(
                BenchmarkErrorCode::INVALID_PARAMETER,
                "Unsupported benchmark operation '" + op + "'. Allowed: " + join_benchmark_operations(allowed)
            );
        }
        if (std::find(selected.begin(), selected.end(), op) == selected.end()) {
            selected.push_back(op);
        }
    };

    const json& raw = options["operations"];
    if (raw.is_string()) {
        add_operation(raw);
    } else if (raw.is_array()) {
        for (const auto& item : raw) {
            add_operation(item);
        }
    } else {
        throw BenchmarkException(BenchmarkErrorCode::INVALID_PARAMETER, "operations must be a string or array of strings");
    }

    if (selected.empty()) {
        throw BenchmarkException(BenchmarkErrorCode::INVALID_PARAMETER, "operations must not be empty");
    }
    return selected;
}

static bool has_benchmark_operation(const std::vector<std::string>& operations, const char* operation) {
    return std::find(operations.begin(), operations.end(), operation) != operations.end();
}

static json benchmark_operations_json(const std::vector<std::string>& operations) {
    json out = json::array();
    for (const auto& op : operations) {
        out.push_back(op);
    }
    return out;
}

static double elapsed_ms(std::chrono::steady_clock::time_point start) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

static json summarize_latency_samples(
    std::vector<double> samples_ms,
    uint64_t bytes_processed,
    uint64_t extra_ops = 0
) {
    json out;
    const uint64_t ops = static_cast<uint64_t>(samples_ms.size()) + extra_ops;
    out["ops"] = ops;
    out["bytes"] = bytes_processed;
    if (samples_ms.empty()) {
        out["total_ms"] = 0.0;
        out["ops_sec"] = 0.0;
        out["mb_sec"] = 0.0;
        out["avg_ms"] = 0.0;
        out["p50_ms"] = 0.0;
        out["p95_ms"] = 0.0;
        out["p99_ms"] = 0.0;
        return out;
    }

    std::sort(samples_ms.begin(), samples_ms.end());
    const double total_ms = std::accumulate(samples_ms.begin(), samples_ms.end(), 0.0);
    const auto percentile = [&](double pct) {
        const double pos = pct * static_cast<double>(samples_ms.size() - 1);
        return samples_ms[static_cast<size_t>(std::ceil(pos))];
    };

    out["total_ms"] = total_ms;
    out["ops_sec"] = total_ms > 0.0 ? (static_cast<double>(ops) * 1000.0 / total_ms) : 0.0;
    out["mb_sec"] = total_ms > 0.0
        ? (static_cast<double>(bytes_processed) / 1024.0 / 1024.0) * 1000.0 / total_ms
        : 0.0;
    out["avg_ms"] = total_ms / static_cast<double>(samples_ms.size());
    out["p50_ms"] = percentile(0.50);
    out["p95_ms"] = percentile(0.95);
    out["p99_ms"] = percentile(0.99);
    return out;
}

template <typename Fn>
static json benchmark_loop(uint64_t iterations, uint64_t bytes_per_op, Fn&& fn) {
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iterations));
    for (uint64_t i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fn(i);
        samples.push_back(elapsed_ms(start));
    }
    return summarize_latency_samples(std::move(samples), iterations * bytes_per_op);
}

static bytes deterministic_payload(size_t size) {
    bytes data(size);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<byte>((i * 131u + 17u) & 0xFFu);
    }
    return data;
}

static Hash256 deterministic_hash256() {
    Hash256 out{};
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<byte>((i * 29u + 11u) & 0xFFu);
    }
    return out;
}

static Hash256 benchmark_round_hash(Hash256 base, uint64_t round) {
    for (size_t i = 0; i < 8 && i < base.size(); ++i) {
        base[i] ^= static_cast<byte>((round >> (i * 8)) & 0xFFu);
    }
    return base;
}

json run_db_benchmark(const std::filesystem::path& data_dir, const json& options) {
    const uint64_t iterations = benchmark_u64_option(options, "iterations", 1000, 1, 100000);
    const uint64_t value_size = benchmark_u64_option(options, "value_size", 512, 1, 1024 * 1024);
    const uint64_t scan_repeats = benchmark_u64_option(options, "scan_repeats", 3, 1, 1000);
    const bool cleanup = benchmark_bool_option(options, "cleanup", true);
    const std::vector<std::string> operations = benchmark_operations_option(
        options,
        {"put", "get", "scan", "delete"},
        {"put", "get", "scan", "delete"}
    );
    const bool do_put = has_benchmark_operation(operations, "put");
    const bool do_get = has_benchmark_operation(operations, "get");
    const bool do_scan = has_benchmark_operation(operations, "scan");
    const bool do_delete = has_benchmark_operation(operations, "delete");

    const std::filesystem::path bench_path = data_dir / "bench" / "db";
    std::filesystem::create_directories(bench_path);

    rocksdb::Options db_options;
    db_options.create_if_missing = true;
    db_options.IncreaseParallelism();
    db_options.OptimizeLevelStyleCompaction();

    std::unique_ptr<rocksdb::DB> db;
    const rocksdb::Status open_status = storage::backend::open_rocksdb(
        db_options,
        bench_path.string(),
        db
    );
    if (!open_status.ok()) {
        throw BenchmarkException(BenchmarkErrorCode::DATABASE_ERROR, "Failed to open benchmark RocksDB: " + open_status.ToString());
    }

    const uint64_t run_id = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
    const std::string prefix = "bench:" + std::to_string(run_id) + ":";
    const std::string value(value_size, 'x');
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(iterations));
    for (uint64_t i = 0; i < iterations; ++i) {
        keys.push_back(prefix + std::to_string(i));
    }

    rocksdb::WriteOptions write_options;
    write_options.sync = false;
    rocksdb::ReadOptions read_options;
    uint64_t checksum = 0;

    const auto put_key = [&](uint64_t i) {
        const rocksdb::Status status = db->Put(write_options, keys[static_cast<size_t>(i)], value);
        if (!status.ok()) {
            throw BenchmarkException(BenchmarkErrorCode::DATABASE_ERROR, "RocksDB Put failed: " + status.ToString());
        }
    };

    const auto delete_key = [&](uint64_t i) {
        const rocksdb::Status status = db->Delete(write_options, keys[static_cast<size_t>(i)]);
        if (!status.ok()) {
            throw BenchmarkException(BenchmarkErrorCode::DATABASE_ERROR, "RocksDB Delete failed: " + status.ToString());
        }
    };

    const auto prepare_keys = [&]() {
        for (uint64_t i = 0; i < iterations; ++i) {
            put_key(i);
        }
    };

    json result;
    result["suite"] = "db";
    result["backend"] = "rocksdb";
    result["path"] = bench_path.string();
    result["iterations"] = iterations;
    result["value_size"] = value_size;
    result["scan_repeats"] = scan_repeats;
    result["cleanup"] = cleanup;
    result["operations"] = benchmark_operations_json(operations);

    bool keys_exist = false;
    if (!do_put && (do_get || do_scan || do_delete)) {
        prepare_keys();
        keys_exist = true;
    }

    if (do_put) {
        result["put"] = benchmark_loop(iterations, value_size, [&](uint64_t i) {
            put_key(i);
        });
        keys_exist = true;
    }

    std::string read_value;
    if (do_get) {
        result["get"] = benchmark_loop(iterations, value_size, [&](uint64_t i) {
            const rocksdb::Status status = db->Get(read_options, keys[static_cast<size_t>(i)], &read_value);
            if (!status.ok()) {
                throw BenchmarkException(BenchmarkErrorCode::DATABASE_ERROR, "RocksDB Get failed: " + status.ToString());
            }
            checksum += read_value.empty() ? 0 : static_cast<unsigned char>(read_value[0]);
        });
    }

    if (do_scan) {
        std::vector<double> scan_samples;
        scan_samples.reserve(static_cast<size_t>(scan_repeats));
        uint64_t scanned_rows = 0;
        for (uint64_t repeat = 0; repeat < scan_repeats; ++repeat) {
            const auto start = std::chrono::steady_clock::now();
            std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(read_options));
            uint64_t rows = 0;
            for (it->Seek(prefix); it->Valid(); it->Next()) {
                const std::string key = it->key().ToString();
                if (key.rfind(prefix, 0) != 0) {
                    break;
                }
                rows++;
                checksum += static_cast<unsigned char>(it->value().data()[0]);
            }
            if (!it->status().ok()) {
                throw BenchmarkException(BenchmarkErrorCode::DATABASE_ERROR, "RocksDB scan failed: " + it->status().ToString());
            }
            scanned_rows += rows;
            scan_samples.push_back(elapsed_ms(start));
        }
        json scan = summarize_latency_samples(
            std::move(scan_samples),
            scanned_rows * value_size,
            scanned_rows > scan_repeats ? scanned_rows - scan_repeats : 0
        );
        scan["rows"] = scanned_rows;
        scan["rows_per_sec"] = scan.value("ops_sec", 0.0);
        result["scan"] = std::move(scan);
    }

    if (do_delete) {
        result["delete"] = benchmark_loop(iterations, 0, [&](uint64_t i) {
            delete_key(i);
        });
        keys_exist = false;
    }

    if (!cleanup && do_delete) {
        prepare_keys();
        keys_exist = true;
    } else if (cleanup && keys_exist) {
        for (uint64_t i = 0; i < iterations; ++i) {
            delete_key(i);
        }
        keys_exist = false;
    }

    json properties = json::object();
    for (const char* prop : {
             "rocksdb.estimate-num-keys",
             "rocksdb.estimate-live-data-size",
             "rocksdb.total-sst-files-size",
             "rocksdb.num-immutable-mem-table"
         }) {
        std::string property_value;
        if (db->GetProperty(prop, &property_value)) {
            properties[prop] = property_value;
        }
    }

    result["properties"] = std::move(properties);
    result["checksum"] = checksum;
    return result;
}

json run_crypto_benchmark(const json& options) {
    const uint64_t iterations = benchmark_u64_option(options, "iterations", 100, 1, 100000);
    const uint64_t data_size = benchmark_u64_option(options, "data_size", 1024, 1, 1024 * 1024);
    const uint64_t pbkdf2_iterations = benchmark_u64_option(options, "pbkdf2_iterations", 10000, 1, 600000);
    const uint64_t falcon_iterations = benchmark_u64_option(
        options,
        "falcon_iterations",
        std::min<uint64_t>(iterations, 3),
        1,
        1000
    );
    const bool include_slow = benchmark_bool_option(options, "include_slow", false);
    const std::vector<std::string> operations = benchmark_operations_option(
        options,
        {
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
            "verify_address"
        },
        {
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
            "verify_address"
        }
    );
    const bool do_sha256 = has_benchmark_operation(operations, "sha256");
    const bool do_double_sha256 = has_benchmark_operation(operations, "double_sha256");
    const bool do_ripemd160 = has_benchmark_operation(operations, "ripemd160");
    const bool do_pbkdf2 = has_benchmark_operation(operations, "pbkdf2");
    const bool do_aes_encrypt = has_benchmark_operation(operations, "aes_encrypt");
    const bool do_aes_decrypt = has_benchmark_operation(operations, "aes_decrypt");
    const bool do_falcon_public_key = has_benchmark_operation(operations, "falcon_public_key");
    const bool do_falcon_sign = has_benchmark_operation(operations, "falcon_sign");
    const bool do_falcon_verify = has_benchmark_operation(operations, "falcon_verify");
    const bool do_falcon_verify_packed = has_benchmark_operation(operations, "falcon_verify_packed");
    const bool do_falcon_recover_address = has_benchmark_operation(operations, "falcon_recover_address");
    const bool do_falcon_score = has_benchmark_operation(operations, "falcon_score");
    const bool do_verify_address = has_benchmark_operation(operations, "verify_address");

    const bytes payload = deterministic_payload(static_cast<size_t>(data_size));
    const bytes private_key = deterministic_payload(32);
    const Hash256 msg = deterministic_hash256();
    const std::string password = "geocoin-benchmark-password";
    bytes salt(16, 0x42);
    uint64_t checksum = 0;

    json result;
    result["suite"] = "crypto";
    result["data_size"] = data_size;
    result["iterations"] = iterations;
    result["pbkdf2_iterations"] = pbkdf2_iterations;
    result["falcon_iterations"] = falcon_iterations;
    result["falcon_available"] = crypto::falcon512::available();
    result["operations"] = benchmark_operations_json(operations);

    if (do_sha256) {
        result["sha256"] = benchmark_loop(iterations, data_size, [&](uint64_t) {
            const Hash256 h = crypto::sha256(payload);
            checksum += h[0];
        });
    }
    if (do_double_sha256) {
        result["double_sha256"] = benchmark_loop(iterations, data_size, [&](uint64_t) {
            const Hash256 h = crypto::double_sha256(payload);
            checksum += h[0];
        });
    }
    if (do_ripemd160) {
        result["ripemd160"] = benchmark_loop(iterations, data_size, [&](uint64_t) {
            const Hash160 h = crypto::ripemd160(payload);
            checksum += h[0];
        });
    }

    const auto derive_benchmark_key = [&](int iteration_count) {
        bytes key;
        if (!crypto::derive_key_pbkdf2(password, salt, iteration_count, key)) {
            throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "PBKDF2 failed");
        }
        checksum += key.empty() ? 0 : key[0];
        OPENSSL_cleanse(key.data(), key.size());
    };

    if (do_pbkdf2) {
        result["pbkdf2_hmac_sha256"] = benchmark_loop(iterations, 32, [&](uint64_t) {
            derive_benchmark_key(static_cast<int>(pbkdf2_iterations));
        });
    }

    bytes encrypted_sample;
    const auto encrypt_sample = [&](int iteration_count) -> bytes {
        bytes encrypted;
        if (!crypto::encrypt_aes256(payload, password, encrypted, iteration_count)) {
            throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "AES-256-GCM encrypt failed");
        }
        return encrypted;
    };
    const auto prepare_encrypted_sample = [&]() {
        if (encrypted_sample.empty()) {
            encrypted_sample = encrypt_sample(static_cast<int>(pbkdf2_iterations));
        }
    };

    if (do_aes_encrypt) {
        result["aes256_gcm_encrypt"] = benchmark_loop(iterations, data_size, [&](uint64_t) {
            bytes encrypted = encrypt_sample(static_cast<int>(pbkdf2_iterations));
            checksum += encrypted.empty() ? 0 : encrypted[0];
            encrypted_sample = std::move(encrypted);
        });
    }
    if (do_aes_decrypt) {
        prepare_encrypted_sample();
        result["aes256_gcm_decrypt"] = benchmark_loop(iterations, data_size, [&](uint64_t) {
            bytes decrypted;
            if (!crypto::decrypt_aes256(encrypted_sample, password, decrypted, static_cast<int>(pbkdf2_iterations))) {
                throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "AES-256-GCM decrypt failed");
            }
            checksum += decrypted.empty() ? 0 : decrypted[0];
        });
    }

    bytes public_key;
    bytes signature;
    const auto derive_public_key = [&]() -> bytes {
        bytes pk;
        if (!crypto::falcon512::public_key_from_seed32(private_key, pk)) {
            throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Falcon public key derivation failed");
        }
        return pk;
    };
    const auto sign_digest = [&]() {
        bytes pk;
        bytes sig;
        if (!crypto::falcon512::sign_digest32(msg, private_key, pk, sig)) {
            throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Falcon sign failed");
        }
        public_key = std::move(pk);
        signature = std::move(sig);
    };
    const auto prepare_signature = [&]() {
        if (public_key.empty() || signature.empty()) {
            sign_digest();
        }
    };

    if (do_falcon_public_key) {
        result["falcon512_public_key_from_seed32"] = benchmark_loop(falcon_iterations, 32, [&](uint64_t) {
            bytes pk = derive_public_key();
            checksum += pk.empty() ? 0 : pk[0];
            public_key = std::move(pk);
        });
    }
    if (do_falcon_sign) {
        result["falcon512_sign_digest32"] = benchmark_loop(falcon_iterations, 32, [&](uint64_t) {
            sign_digest();
            checksum += signature.empty() ? 0 : signature[0];
        });
    }
    if (do_falcon_verify) {
        prepare_signature();
        result["falcon512_verify_digest32"] = benchmark_loop(iterations, 32, [&](uint64_t) {
            if (!crypto::falcon512::verify_digest32(msg, public_key, signature)) {
                throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Falcon verify failed");
            }
            checksum += signature.empty() ? 0 : signature[0];
        });
    }

    Signature packed_signature;
    const auto prepare_packed_signature = [&]() {
        if (packed_signature.empty()) {
            packed_signature = crypto::sign_recoverable_message(msg, private_key);
            if (packed_signature.empty()) {
                throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Packed Falcon signing failed");
            }
        }
    };
    if (do_falcon_verify_packed) {
        prepare_packed_signature();
        result["falcon512_verify_packed_message"] = benchmark_loop(iterations, 32, [&](uint64_t) {
            if (!crypto::verify_signed_message(msg, packed_signature)) {
                throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Packed Falcon message verify failed");
            }
            checksum += packed_signature.empty() ? 0 : packed_signature[0];
        });
    }
    if (do_falcon_recover_address) {
        prepare_packed_signature();
        Address recovered;
        result["falcon512_recover_address"] = benchmark_loop(iterations, 32, [&](uint64_t) {
            if (!crypto::recover_signer_address_from_signed_message(msg, packed_signature, recovered)) {
                throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Falcon address recovery failed");
            }
            checksum += recovered.str().empty() ? 0 : static_cast<unsigned char>(recovered.str()[0]);
        });
    }

    if (do_falcon_score) {
        json score = benchmark_loop(falcon_iterations, 32, [&](uint64_t round) {
            const Hash256 round_msg = benchmark_round_hash(msg, round);
            Signature packed = crypto::sign_recoverable_message(round_msg, private_key);
            if (packed.empty()) {
                throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Falcon score signing failed");
            }

            Address recovered;
            if (!crypto::recover_signer_address_from_signed_message(round_msg, packed, recovered)) {
                throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Falcon score recover/verify failed");
            }
            if (!crypto::verify_address(recovered)) {
                throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Falcon score address verify failed");
            }
            checksum += recovered.str().empty() ? 0 : static_cast<unsigned char>(recovered.str()[0]);
        });
        score["points"] = score.value("ops", falcon_iterations);
        score["points_sec"] = score.value("ops_sec", 0.0);
        score["avg_ms_per_point"] = score.value("avg_ms", 0.0);
        score["definition"] =
            "1 point = packed Falcon-512 sign + signer address recover/verify + address verify";
        result["falcon512_score"] = std::move(score);
    }

    if (do_verify_address) {
        Address identity_address;
        if (!crypto::derive_identity_address_from_private_key(private_key, identity_address)) {
            throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Address derivation failed");
        }
        result["verify_address"] = benchmark_loop(iterations, identity_address.str().size(), [&](uint64_t) {
            if (!crypto::verify_address(identity_address)) {
                throw BenchmarkException(BenchmarkErrorCode::INTERNAL_ERROR, "Address verify failed");
            }
            checksum += identity_address.str().empty() ? 0 : static_cast<unsigned char>(identity_address.str()[0]);
        });
        result["sample_address"] = identity_address.str();
    }

    if (include_slow && (do_pbkdf2 || do_aes_encrypt)) {
        json slow;
        for (int profile_iterations : {
                 crypto::kPbkdf2LegacyIterations,
                 crypto::kPbkdf2Iterations
             }) {
            const std::string key = std::to_string(profile_iterations);
            if (do_pbkdf2) {
                slow[key]["pbkdf2_hmac_sha256"] = benchmark_loop(1, 32, [&](uint64_t) {
                    derive_benchmark_key(profile_iterations);
                });
            }
            if (do_aes_encrypt) {
                slow[key]["aes256_gcm_encrypt"] = benchmark_loop(1, data_size, [&](uint64_t) {
                    bytes encrypted = encrypt_sample(profile_iterations);
                    checksum += encrypted.empty() ? 0 : encrypted[0];
                });
            }
        }
        result["slow_profiles"] = std::move(slow);
    }

    result["checksum"] = checksum;
    return result;
}

json list_benchmarks() {
    json result;
    result["mode"] = "cli";
    result["enable_flag"] = "--benchmark";
    result["suites"] = {
        {
            "db",
            {
                {"description", "RocksDB put/get/scan/delete benchmark in data_dir/bench/db"},
                {"writes_chain_db", false},
                {"options", {
                    {"iterations", "1..100000, default 1000"},
                    {"value_size", "1..1048576 bytes, default 512"},
                    {"scan_repeats", "1..1000, default 3"},
                    {"cleanup", "boolean, default true"},
                    {"operations", "string or array: put|get|scan|delete; default all"}
                }}
            }
        },
        {
            "crypto",
            {
                {"description", "Hash, PBKDF2/AES-256-GCM and Falcon-512 benchmark"},
                {"writes_chain_db", false},
                {"options", {
                    {"iterations", "1..100000, default 100"},
                    {"data_size", "1..1048576 bytes, default 1024"},
                    {"pbkdf2_iterations", "1..600000, default 10000"},
                    {"falcon_iterations", "1..1000, default min(iterations, 3)"},
                    {"include_slow", "boolean, default false; adds selected slow PBKDF2/AES profiles"},
                    {"operations", "string or array: sha256|double_sha256|ripemd160|pbkdf2|aes_encrypt|aes_decrypt|falcon_public_key|falcon_sign|falcon_verify|falcon_verify_packed|falcon_recover_address|falcon_score|verify_address; default all"}
                }}
            }
        },
        {
            "falcon",
            {
                {"description", "Falcon-512 score benchmark"},
                {"writes_chain_db", false},
                {"score", "falcon512_score.points_sec"},
                {"options", {
                    {"falcon_iterations", "1..1000, default 20 for suite falcon"},
                    {"operations", "default falcon_score; can use any Falcon operations from crypto suite"}
                }}
            }
        }
    };
    return result;
}

json run_benchmark(const std::filesystem::path& data_dir, const std::string& suite, const json& options) {
    const auto started = std::chrono::steady_clock::now();
    json result;
    if (suite == "db") {
        result = run_db_benchmark(data_dir, options);
    } else if (suite == "crypto" || suite == "compute") {
        result = run_crypto_benchmark(options);
    } else if (suite == "falcon") {
        json falcon_options = options;
        if (!falcon_options.contains("operations") || falcon_options["operations"].is_null()) {
            falcon_options["operations"] = json::array({"falcon_score"});
        }
        if (!falcon_options.contains("falcon_iterations") || falcon_options["falcon_iterations"].is_null()) {
            falcon_options["falcon_iterations"] = 20;
        }
        result = run_crypto_benchmark(falcon_options);
        result["suite"] = "falcon";
    } else {
        throw BenchmarkException(BenchmarkErrorCode::INVALID_PARAMETER, "Unknown benchmark suite: " + suite);
    }
    result["elapsed_ms"] = elapsed_ms(started);
    return result;
}

}  // namespace geocoin::benchmark
