#include "gpu_miner_common.hpp"
#include <chrono>
#include <iostream>

#ifdef GEOCOIN_MACOS_ARM64
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/bytes.hpp"
#include <string_view>
#include <utility>

namespace {
namespace gm = geocoin::tools::gpu_miner;

int test_apple_pow()
{
    gm::MiningJob job;
    job.block_bytes.resize(124);
    for (size_t i = 0; i < job.block_bytes.size(); ++i) {
        job.block_bytes[i] = static_cast<geocoin::byte>(i);
    }
    job.intensity = 1024;
    job.timeout_sec = 3;
    // Each range's first 1,024 nonces has a unique solution. The second
    // and third ranges cross the low-word and full-width nonce boundaries.
    struct Vector {
        uint64_t start;
        uint32_t bits;
        uint64_t expected_nonce;
        std::string_view expected_hex;
    };
    constexpr Vector vectors[] = {
        {0, 0x1f300000u, 0x10e,
         "0024ea3b9563206955bcd1eadd0280a1735c29a2b1b1307191476558e5394189"},
        {0xffffffffull, 0x1f100000u, 0x100000299ull,
         "000f7a5feba5ca805a97b85fce24c293b78b28f82d16fb04fb9e8f215be152b7"},
        {0xffffffffffffffc0ull, 0x1f300000u, 0x10e,
         "0024ea3b9563206955bcd1eadd0280a1735c29a2b1b1307191476558e5394189"},
    };
    for (const auto& vector : vectors) {
        job.start_nonce = vector.start;
        job.bits = vector.bits;
        auto expected_header = job.block_bytes;
        geocoin::utils::write_u64_le(expected_header, 116, vector.expected_nonce);
        const auto expected_hash = geocoin::crypto::double_sha256(expected_header);
        if (geocoin::utils::hash256_to_hex(expected_hash) != vector.expected_hex) return 1;

        for (const auto& [backend, result] : {
                std::pair{"Metal", gm::mine_metal(job, {0})},
                std::pair{"OpenCL", gm::mine_opencl(job, {0})}}) {
            if (!result.found || !result.error.empty() ||
                result.hash != expected_hash || result.nonce != vector.expected_nonce || result.device_id != 0) {
                std::cerr << backend << " proof-of-work vector failed: " << result.error
                          << ", nonce " << result.nonce << ", digest "
                          << geocoin::utils::hash256_to_hex(result.hash) << '\n';
                return 1;
            }
            std::cout << backend << " matched CPU SHA-256d at nonce " << result.nonce << '\n';
        }
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--apple-pow") return test_apple_pow();
    gm::MiningJob job;
    job.cancel_requested = [] { return true; };
    const auto start = std::chrono::steady_clock::now();
    const auto metal = gm::mine_metal(job, {});
    if (!metal.cancelled) return 1;
#ifdef GEOCOIN_GPU_ENABLE_OPENCL
    const auto opencl = gm::mine_opencl(job, {});
    if (!opencl.cancelled) return 1;
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (ms >= 5000) return 1;
    std::cout << "GPU pre-cancel paths passed in " << ms << "ms (no device initialization)\n";
    return 0;
}
#else
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
#endif
