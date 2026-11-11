#pragma once

#include "geocoin/rpc/rpc_types.hpp"

namespace geocoin::rpc::detail {

/** Best-effort cleansing of every string value and object key in a JSON tree. */
void cleanse_json_strings(json& value) noexcept;

/** Cleanses JSON string storage when a request-processing scope exits. */
class ScopedJsonStringCleanser final {
public:
    explicit ScopedJsonStringCleanser(json& value) noexcept : value_(value) {}
    ~ScopedJsonStringCleanser();

    ScopedJsonStringCleanser(const ScopedJsonStringCleanser&) = delete;
    ScopedJsonStringCleanser& operator=(const ScopedJsonStringCleanser&) = delete;

private:
    json& value_;
};

} // namespace geocoin::rpc::detail
