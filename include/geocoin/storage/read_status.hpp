#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace geocoin::storage {

enum class ReadStatus { FOUND, NOT_FOUND, CORRUPT, IO_ERROR };

class ReadError : public std::runtime_error {
public:
    ReadError(ReadStatus status, std::string detail)
        : std::runtime_error(std::move(detail)), status_(status) {}
    ReadStatus status() const noexcept { return status_; }
private:
    ReadStatus status_;
};

struct ReadResult {
    ReadStatus status;
    std::string detail;
    bool found() const noexcept { return status == ReadStatus::FOUND; }
};

} // namespace geocoin::storage
