#pragma once

#include <cstdint>

#if !defined(__SIZEOF_INT128__)
#error GeoCoin NFT geometry requires a compiler with __int128
#endif

namespace geocoin {

using int128_t = __int128;
using uint128_t = unsigned __int128;

inline long double to_long_double(int128_t value) {
    return static_cast<long double>(value);
}

} // namespace geocoin
