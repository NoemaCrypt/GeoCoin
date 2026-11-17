#pragma once

#include <cstddef>

namespace geocoin::node {
size_t cpu_mining_thread_limit();
size_t clamp_cpu_mining_threads(size_t threads);
} // namespace geocoin::node
