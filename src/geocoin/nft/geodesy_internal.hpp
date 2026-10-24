#pragma once

#include <cfenv>

namespace geocoin::nft::coordinates::detail {

// Keep every geodesic calculation in the same floating-point environment,
// including nested calls and exception unwinding through an import/export.
class NearestRounding {
public:
    NearestRounding() noexcept : previous_(std::fegetround()) {
        if (previous_ != FE_TONEAREST) std::fesetround(FE_TONEAREST);
    }
    ~NearestRounding() {
        if (previous_ != FE_TONEAREST && previous_ != -1) std::fesetround(previous_);
    }
    NearestRounding(const NearestRounding&) = delete;
    NearestRounding& operator=(const NearestRounding&) = delete;

private:
    int previous_;
};

} // namespace geocoin::nft::coordinates::detail
