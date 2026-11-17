#include "geocoin/node/cpu_topology.hpp"

namespace geocoin::node {
size_t cpu_mining_thread_limit() {
    return (std::max)(size_t{1}, cpu_mining_topology().capacity());
}

size_t clamp_cpu_mining_threads(size_t threads) {
    return (std::min)((std::max)(size_t{1}, threads), cpu_mining_thread_limit());
}
} // namespace geocoin::node
