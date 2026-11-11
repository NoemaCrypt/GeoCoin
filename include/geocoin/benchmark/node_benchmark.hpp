#pragma once

#include "nlohmann_json.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace geocoin::benchmark {

using json = nlohmann::json;

enum class BenchmarkErrorCode {
    INVALID_REQUEST,
    INVALID_PARAMETER,
    DATABASE_ERROR,
    INTERNAL_ERROR
};

class BenchmarkException : public std::runtime_error {
public:
    BenchmarkException(BenchmarkErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    BenchmarkErrorCode code() const { return code_; }

private:
    BenchmarkErrorCode code_;
};

json list_benchmarks();
json run_benchmark(const std::filesystem::path& data_dir, const std::string& suite, const json& options);
json run_db_benchmark(const std::filesystem::path& data_dir, const json& options);
json run_crypto_benchmark(const json& options);

}  // namespace geocoin::benchmark
