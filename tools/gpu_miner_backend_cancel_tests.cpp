#include "gpu_miner_common.hpp"
#include <chrono>
#include <iostream>

int main()
{
    namespace gm = geocoin::tools::gpu_miner;
    gm::MiningJob job;
    job.cancel_requested = [] { return true; };
    const auto start = std::chrono::steady_clock::now();
    // All real implementations must return BEFORE runtime/device discovery or header validation.
    const auto cuda = gm::mine_cuda(job, {});
    const auto opencl = gm::mine_opencl(job, {});
    const auto hip = gm::mine_hip(job, {});
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (!cuda.cancelled || !opencl.cancelled || !hip.cancelled || ms >= 5000) return 1;
    std::cout << "Real CUDA/HIP/OpenCL pre-cancel paths passed in " << ms << "ms (no device initialization)\n";
    return 0;
}
