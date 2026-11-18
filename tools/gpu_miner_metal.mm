#if defined(GEOCOIN_MACOS_ARM64) && defined(GEOCOIN_GPU_ENABLE_METAL)

#import <Metal/Metal.h>
#include "gpu_miner_apple_work.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace geocoin::tools::gpu_miner {
namespace {

using apple::host_rotr32;
using apple::host_sha256_step;
using apple::kHostSha256;

constexpr size_t kHeaderSize = 124;
constexpr uint64_t kMetalMaxLaunchAttempts = 16777216;
constexpr size_t kMetalScheduledWords = 16;

// Use the same work layout and full SHA-256d calculation as the OpenCL backend.
const char* kMetalKernel = R"MSL(
#include <metal_stdlib>
using namespace metal;

constant bool fixed_nonce_high [[function_constant(0)]];

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z) ((z) ^ ((x) & ((y) ^ (z))))
#define MAJ(x,y,z) (((x) & (y)) | ((z) & ((x) ^ (y))))
#define BIG0(x) (ROTR((x), 2) ^ ROTR((x), 13) ^ ROTR((x), 22))
#define BIG1(x) (ROTR((x), 6) ^ ROTR((x), 11) ^ ROTR((x), 25))
#define SMALL0(x) (ROTR((x), 7) ^ ROTR((x), 18) ^ ((x) >> 3))
#define SMALL1(x) (ROTR((x), 17) ^ ROTR((x), 19) ^ ((x) >> 10))
#define SHA256_STEP(a,b,c,d,e,f,g,h,x,k) do { \
    (h) += (k) + (x) + BIG1(e) + CH((e), (f), (g)); \
    (d) += (h); \
    (h) += BIG0(a) + MAJ((a), (b), (c)); \
} while (0)
#define SHA256_EXPAND(x,y,z,w) (SMALL1(x) + (y) + SMALL0(z) + (w))
#define ROUND_STEP(i) do { \
    SHA256_STEP(a,b,c,d,e,f,g,h,w0,K[(i)+0]); \
    SHA256_STEP(h,a,b,c,d,e,f,g,w1,K[(i)+1]); \
    SHA256_STEP(g,h,a,b,c,d,e,f,w2,K[(i)+2]); \
    SHA256_STEP(f,g,h,a,b,c,d,e,w3,K[(i)+3]); \
    SHA256_STEP(e,f,g,h,a,b,c,d,w4,K[(i)+4]); \
    SHA256_STEP(d,e,f,g,h,a,b,c,w5,K[(i)+5]); \
    SHA256_STEP(c,d,e,f,g,h,a,b,w6,K[(i)+6]); \
    SHA256_STEP(b,c,d,e,f,g,h,a,w7,K[(i)+7]); \
    SHA256_STEP(a,b,c,d,e,f,g,h,w8,K[(i)+8]); \
    SHA256_STEP(h,a,b,c,d,e,f,g,w9,K[(i)+9]); \
    SHA256_STEP(g,h,a,b,c,d,e,f,w10,K[(i)+10]); \
    SHA256_STEP(f,g,h,a,b,c,d,e,w11,K[(i)+11]); \
    SHA256_STEP(e,f,g,h,a,b,c,d,w12,K[(i)+12]); \
    SHA256_STEP(d,e,f,g,h,a,b,c,w13,K[(i)+13]); \
    SHA256_STEP(c,d,e,f,g,h,a,b,w14,K[(i)+14]); \
    SHA256_STEP(b,c,d,e,f,g,h,a,w15,K[(i)+15]); \
} while (0)
#define ROUND_EXPAND() do { \
    w0 = SHA256_EXPAND(w14, w9, w1, w0); \
    w1 = SHA256_EXPAND(w15, w10, w2, w1); \
    w2 = SHA256_EXPAND(w0, w11, w3, w2); \
    w3 = SHA256_EXPAND(w1, w12, w4, w3); \
    w4 = SHA256_EXPAND(w2, w13, w5, w4); \
    w5 = SHA256_EXPAND(w3, w14, w6, w5); \
    w6 = SHA256_EXPAND(w4, w15, w7, w6); \
    w7 = SHA256_EXPAND(w5, w0, w8, w7); \
    w8 = SHA256_EXPAND(w6, w1, w9, w8); \
    w9 = SHA256_EXPAND(w7, w2, w10, w9); \
    w10 = SHA256_EXPAND(w8, w3, w11, w10); \
    w11 = SHA256_EXPAND(w9, w4, w12, w11); \
    w12 = SHA256_EXPAND(w10, w5, w13, w12); \
    w13 = SHA256_EXPAND(w11, w6, w14, w13); \
    w14 = SHA256_EXPAND(w12, w7, w15, w14); \
    w15 = SHA256_EXPAND(w13, w8, w0, w15); \
} while (0)

constant uint K[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,
    0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,
    0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,
    0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,
    0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,
    0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,
    0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,
    0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,
    0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};

inline void store_be32(device uchar* p, uint v) {
    p[0] = (uchar)((v >> 24) & 0xffU);
    p[1] = (uchar)((v >> 16) & 0xffU);
    p[2] = (uchar)((v >> 8) & 0xffU);
    p[3] = (uchar)(v & 0xffU);
}

inline void sha256_compress_words(
    thread uint state[8],
    uint w0, uint w1, uint w2, uint w3,
    uint w4, uint w5, uint w6, uint w7,
    uint w8, uint w9, uint w10, uint w11,
    uint w12, uint w13, uint w14, uint w15)
{
    uint a = state[0], b = state[1], c = state[2], d = state[3];
    uint e = state[4], f = state[5], g = state[6], h = state[7];
    ROUND_STEP(0);
    ROUND_EXPAND();
    ROUND_STEP(16);
    ROUND_EXPAND();
    ROUND_STEP(32);
    ROUND_EXPAND();
    ROUND_STEP(48);
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

inline void sha256_tail_from_round13(constant uint* work, thread uint state[8], uint nonce_lo_be, uint nonce_hi_be)
{
    uint a = work[21], b = work[22], c = work[23], d = work[24];
    uint e = work[25], f = work[26], g = work[27], h = work[28];
    uint w0 = work[8], w1 = work[9], w2 = work[10], w3 = work[11];
    uint w4 = work[12], w5 = work[13], w6 = work[14], w7 = work[15];
    uint w8 = work[16], w9 = work[17], w10 = work[18], w11 = work[19];
    uint w12 = work[20], w13 = nonce_lo_be, w14 = nonce_hi_be, w15 = 0x80000000U;

    if (fixed_nonce_high) {
        // The host computed the nonce-independent part of round 13.
        c += w13;
        g += w13;
    } else {
        SHA256_STEP(d,e,f,g,h,a,b,c,w13,K[13]);
    }
    SHA256_STEP(c,d,e,f,g,h,a,b,w14,K[14]);
    SHA256_STEP(b,c,d,e,f,g,h,a,w15,K[15]);
    if (fixed_nonce_high) {
        // The first 16 expanded schedule words only depend on nonce_low
        // through the terms computed below. The host supplied the rest.
        constant uint* fixed = work + 37;
        w0 = fixed[0];
        w1 = fixed[1];
        w2 = fixed[2];
        w3 = fixed[3];
        w4 = SMALL1(w2) + w13 + fixed[4];
        w5 = fixed[5];
        w6 = SMALL1(w4) + fixed[6];
        w7 = fixed[7];
        w8 = SMALL1(w6) + fixed[8];
        w9 = fixed[9];
        w10 = SMALL1(w8) + fixed[10];
        w11 = SMALL1(w9) + w4 + fixed[11];
        w12 = SMALL1(w10) + SMALL0(w13) + fixed[12];
        w13 = SMALL1(w11) + w6 + w13 + fixed[13];
        w14 = SMALL1(w12) + fixed[14];
        w15 = SMALL1(w13) + w8 + fixed[15];
    } else {
        ROUND_EXPAND();
    }
    ROUND_STEP(16);
    ROUND_EXPAND();
    ROUND_STEP(32);
    ROUND_EXPAND();
    ROUND_STEP(48);

    state[0] = work[0] + a; state[1] = work[1] + b; state[2] = work[2] + c; state[3] = work[3] + d;
    state[4] = work[4] + e; state[5] = work[5] + f; state[6] = work[6] + g; state[7] = work[7] + h;
}

inline uint bswap32(uint x) {
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

inline bool hash_words_meet_target(const thread uint hash[8], constant uint* target) {
    for (int i = 0; i < 8; ++i) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true;
}

kernel void geocoin_mine_fast(
    constant uint* work [[buffer(0)]],
    constant ulong& base_nonce [[buffer(1)]],
    constant ulong& nonce_stride [[buffer(2)]],
    device atomic_int* found [[buffer(3)]],
    device ulong* found_nonce [[buffer(4)]],
    device uchar* found_hash [[buffer(5)]],
    uint tid [[thread_position_in_grid]])
{
    uint nonce_lo;
    uint nonce_hi;
    if (fixed_nonce_high) {
        // The host splits dispatches at the low-word boundary.
        nonce_lo = (uint)base_nonce + tid;
        nonce_hi = (uint)(base_nonce >> 32);
    } else {
        const ulong nonce = base_nonce + (ulong)tid * nonce_stride;
        nonce_lo = (uint)nonce;
        nonce_hi = (uint)(nonce >> 32);
    }
    const uint nonce_lo_be = bswap32(nonce_lo);
    const uint nonce_hi_be = bswap32(nonce_hi);

    uint state[8];
    sha256_tail_from_round13(work, state, nonce_lo_be, nonce_hi_be);
    sha256_compress_words(
        state,
        0U,0U,0U,0U,0U,0U,0U,0U,
        0U,0U,0U,0U,0U,0U,0U,992U);

    uint hash[8] = {
        0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U
    };
    sha256_compress_words(
        hash,
        state[0], state[1], state[2], state[3],
        state[4], state[5], state[6], state[7],
        0x80000000U,0U,0U,0U,0U,0U,0U,256U);

    if (hash_words_meet_target(hash, work + 29)) {
        if (atomic_exchange_explicit(found, 1, memory_order_relaxed) == 0) {
            *found_nonce = ((ulong)nonce_hi << 32) | nonce_lo;
            for (int i = 0; i < 8; ++i) store_be32(found_hash + i * 4, hash[i]);
        }
    }
}
)MSL";

struct MetalDevice {
    id<MTLDevice> device = nil;
    int id = -1;
    std::string name;
};

struct MetalPipeline {
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> state;
};

struct MetalDevicePipelines {
    id<MTLCommandQueue> queue;
    id<MTLLibrary> library;
    id<MTLComputePipelineState> contiguous;
    id<MTLComputePipelineState> strided;
};

std::string metal_error(NSError* error)
{
    const char* details = error.localizedDescription.UTF8String;
    return details ? details : "unknown Metal error";
}

MetalPipeline metal_pipeline(id<MTLDevice> device, bool contiguous)
{
    static std::mutex mutex;
    static std::unordered_map<uint64_t, MetalDevicePipelines> pipelines;
    const std::lock_guard lock(mutex);
    auto& cache = pipelines[device.registryID];
    if (!cache.queue) {
        cache.queue = [device newCommandQueue];
        if (!cache.queue) {
            throw std::runtime_error("could not create Metal command queue");
        }
    }
    NSError* error = nil;
    if (!cache.library) {
        MTLCompileOptions* options = [MTLCompileOptions new];
        if (@available(macOS 13.3, *)) {
            options.optimizationLevel = MTLLibraryOptimizationLevelSize;
        }
        cache.library = [device newLibraryWithSource:[NSString stringWithUTF8String:kMetalKernel]
                                            options:options error:&error];
        if (!cache.library) {
            throw std::runtime_error("Metal shader compile failed: " + metal_error(error));
        }
    }

    id<MTLComputePipelineState> __strong& state = contiguous ? cache.contiguous : cache.strided;
    if (state) {
        return {cache.queue, state};
    }
    MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
    [constants setConstantValue:&contiguous type:MTLDataTypeBool atIndex:0];
    id<MTLFunction> function = [cache.library newFunctionWithName:@"geocoin_mine_fast"
                                                 constantValues:constants error:&error];
    if (!function) {
        throw std::runtime_error("Metal kernel specialization failed: " + metal_error(error));
    }
    state = [device newComputePipelineStateWithFunction:function error:&error];
    if (!state) {
        throw std::runtime_error("Metal compute pipeline creation failed: " + metal_error(error));
    }
    return {cache.queue, state};
}

// Constant contributions to W[16..31] for a dispatch with the given high nonce
// word. The low word occupies W[13] and is still computed per thread.
void prepare_contiguous_work(const std::array<uint32_t, 37>& work, uint32_t nonce_high,
                             uint32_t* data)
{
    std::copy(work.begin(), work.end(), data);
    host_sha256_step(data[24], data[25], data[26], data[27],
                     data[28], data[21], data[22], data[23], 0, kHostSha256[13]);

    auto s0 = [](uint32_t x) { return host_rotr32(x, 7) ^ host_rotr32(x, 18) ^ (x >> 3); };
    auto s1 = [](uint32_t x) { return host_rotr32(x, 17) ^ host_rotr32(x, 19) ^ (x >> 10); };
    const uint32_t* w = work.data() + 8;
    const uint32_t w14 = __builtin_bswap32(nonce_high);
    constexpr uint32_t w15 = 0x80000000u;
    const uint32_t w16 = s1(w14) + w[9] + s0(w[1]) + w[0];
    const uint32_t w17 = s1(w15) + w[10] + s0(w[2]) + w[1];
    const uint32_t w18 = s1(w16) + w[11] + s0(w[3]) + w[2];
    const uint32_t w19 = s1(w17) + w[12] + s0(w[4]) + w[3];
    const uint32_t w21 = s1(w19) + w14 + s0(w[6]) + w[5];
    const uint32_t w23 = s1(w21) + w16 + s0(w[8]) + w[7];
    const uint32_t w25 = s1(w23) + w18 + s0(w[10]) + w[9];
    const std::array<uint32_t, kMetalScheduledWords> fixed = {{
        w16, w17, w18, w19,
        s0(w[5]) + w[4], w21, w15 + s0(w[7]) + w[6], w23,
        w17 + s0(w[9]) + w[8], w25, w19 + s0(w[11]) + w[10], s0(w[12]) + w[11],
        w21 + w[12], s0(w14), w23 + s0(w15) + w14, s0(w16) + w15
    }};
    std::copy(fixed.begin(), fixed.end(), data + work.size());
}

std::vector<MetalDevice> enumerate_metal_devices()
{
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    if (devices.count == 0) {
        id<MTLDevice> fallback = MTLCreateSystemDefaultDevice();
        if (fallback) {
            devices = @[ fallback ];
        }
    }
    std::vector<MetalDevice> out;
    int next_id = 0;
    for (id<MTLDevice> d in devices) {
        MetalDevice m;
        m.id = next_id++;
        m.device = d;
        m.name = d.name ? std::string(d.name.UTF8String) : std::string("Metal GPU");
        out.push_back(std::move(m));
    }
    return out;
}

DeviceInfo describe_device(const MetalDevice& d)
{
    DeviceInfo info;
    info.id = d.id;
    info.backend = Backend::Metal;
    info.name = d.name;
    info.vendor = "Apple";
    info.detail = "Metal GPU";
    info.available = true;
    return info;
}

}  // namespace

std::vector<DeviceInfo> list_metal_devices()
{
    std::vector<DeviceInfo> out;
    for (const auto& d : enumerate_metal_devices()) {
        out.push_back(describe_device(d));
    }
    return out;
}

MiningResult mine_metal(const MiningJob& job, const std::vector<int>& device_ids)
{
    MiningResult final_result;
    final_result.backend = "metal";
    if (job_cancelled(job)) { final_result.cancelled = true; return final_result; }

    auto all_devices = enumerate_metal_devices();
    std::vector<int> seen;
    for (int want_id : device_ids) {
        if (std::find(seen.begin(), seen.end(), want_id) != seen.end() ||
            std::none_of(all_devices.begin(), all_devices.end(), [want_id](const MetalDevice& d) { return d.id == want_id; })) {
            final_result.error = "invalid or duplicate Metal device id: " + std::to_string(want_id);
            return final_result;
        }
        seen.push_back(want_id);
    }
    if (!device_ids.empty()) {
        all_devices.erase(std::remove_if(all_devices.begin(), all_devices.end(), [&](const MetalDevice& d) {
            return std::find(device_ids.begin(), device_ids.end(), d.id) == device_ids.end();
        }), all_devices.end());
    }
    if (all_devices.empty()) {
        final_result.error = "no Metal GPU devices available";
        return final_result;
    }
    if (job.block_bytes.size() < kHeaderSize) {
        final_result.error = "block template is shorter than the 124-byte GeoCoin header";
        return final_result;
    }
    for (const auto& device : all_devices) {
        final_result.devices.push_back(describe_device(device));
    }
    final_result.device_hashes.assign(final_result.devices.size(), 0);

    const uint64_t nonce_stride = static_cast<uint64_t>(all_devices.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(job.timeout_sec);
    const auto work = apple::make_work(job.block_bytes, job.bits);

    std::atomic<bool> found{false};
    std::atomic<uint64_t> total_hashes{0};
    std::mutex result_mutex;
    std::mutex progress_mutex;
    auto next_progress = std::chrono::steady_clock::now();
    const auto progress_interval = std::chrono::milliseconds(
        std::max<uint32_t>(250, job.status_interval_ms));
    std::vector<uint64_t> live_device_hashes(final_result.devices.size(), 0);
    std::vector<std::thread> workers;

    for (size_t lane = 0; lane < all_devices.size(); ++lane) {
        workers.emplace_back([&, lane]() {
            const MetalDevice device = all_devices[lane];
            uint64_t lane_hashes = 0;
            @autoreleasepool {
                try {
                    if (job_cancelled(job)) return;
                    id<MTLDevice> mtl_device = device.device;
                    const bool contiguous = nonce_stride == 1;
                    const MetalPipeline pipeline = metal_pipeline(mtl_device, contiguous);
                    const NSUInteger simdgroups = contiguous ? 4 : 8;
                    const NSUInteger threads_per_group = std::min(pipeline.state.threadExecutionWidth * simdgroups,
                                                                  pipeline.state.maxTotalThreadsPerThreadgroup);

                    const MTLResourceOptions storage = MTLResourceStorageModeShared;
                    const size_t work_words = work.size() + (contiguous ? kMetalScheduledWords : 0);
                    id<MTLBuffer> work_buf = [mtl_device newBufferWithLength:work_words * sizeof(uint32_t)
                                                                    options:storage];
                    id<MTLBuffer> found_buf = [mtl_device newBufferWithLength:sizeof(int32_t) options:storage];
                    id<MTLBuffer> nonce_buf = [mtl_device newBufferWithLength:sizeof(uint64_t) options:storage];
                    id<MTLBuffer> hash_buf = [mtl_device newBufferWithLength:32 options:storage];
                    if (!work_buf || !found_buf || !nonce_buf || !hash_buf) {
                        throw std::runtime_error("could not allocate Metal mining buffers");
                    }
                    if (!contiguous) {
                        std::memcpy(work_buf.contents, work.data(), work.size() * sizeof(uint32_t));
                    }
                    *reinterpret_cast<int32_t*>(found_buf.contents) = 0;

                    LaunchSizer sizing(job);
                    NonceCursor nonces(job.start_nonce, lane, nonce_stride);
                    constexpr uint64_t nonce_low_range = 1ull << 32;
                    uint64_t prepared_nonce_high = nonce_low_range;

                    while (!found.load(std::memory_order_relaxed) &&
                           !job_cancelled(job) &&
                           std::chrono::steady_clock::now() < deadline) {
                        uint64_t per_launch = std::min(sizing.attempts(), kMetalMaxLaunchAttempts);
                        if (contiguous) {
                            const uint64_t nonce_high = nonces.next() >> 32;
                            if (nonce_high != prepared_nonce_high) {
                                prepare_contiguous_work(work, static_cast<uint32_t>(nonce_high),
                                                        static_cast<uint32_t*>(work_buf.contents));
                                prepared_nonce_high = nonce_high;
                            }
                            const uint64_t remaining = nonce_low_range - static_cast<uint32_t>(nonces.next());
                            per_launch = std::min(per_launch, remaining);
                        }
                        const uint64_t base_nonce = nonces.take(per_launch);

                        const auto launch_start = std::chrono::steady_clock::now();
                        @autoreleasepool {
                            id<MTLCommandBuffer> command_buffer = [pipeline.queue commandBuffer];
                            id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
                            if (!encoder) {
                                throw std::runtime_error("could not create Metal compute command encoder");
                            }
                            [encoder setComputePipelineState:pipeline.state];
                            [encoder setBuffer:work_buf offset:0 atIndex:0];
                            [encoder setBytes:&base_nonce length:sizeof(base_nonce) atIndex:1];
                            [encoder setBytes:&nonce_stride length:sizeof(nonce_stride) atIndex:2];
                            [encoder setBuffer:found_buf offset:0 atIndex:3];
                            [encoder setBuffer:nonce_buf offset:0 atIndex:4];
                            [encoder setBuffer:hash_buf offset:0 atIndex:5];
                            [encoder dispatchThreads:MTLSizeMake(per_launch, 1, 1)
                               threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
                            [encoder endEncoding];
                            [command_buffer commit];
                            [command_buffer waitUntilCompleted];
                            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                                throw std::runtime_error("Metal command failed: " + metal_error(command_buffer.error));
                            }
                        }
                        sizing.completed(per_launch, std::chrono::steady_clock::now() - launch_start);
                        lane_hashes += per_launch;
                        const uint64_t total_after =
                            total_hashes.fetch_add(per_launch, std::memory_order_relaxed) + per_launch;

                        if (job.progress_callback) {
                            std::lock_guard<std::mutex> progress_lock(progress_mutex);
                            const auto now = std::chrono::steady_clock::now();
                            if (lane < live_device_hashes.size()) {
                                live_device_hashes[lane] = lane_hashes;
                            }
                            if (now >= next_progress) {
                                next_progress = now + progress_interval;
                                job.progress_callback("metal",
                                                      total_after,
                                                      final_result.devices,
                                                      live_device_hashes);
                            }
                        }

                        if (*reinterpret_cast<int32_t*>(found_buf.contents) != 0) {
                            bool expected = false;
                            if (found.compare_exchange_strong(expected, true)) {
                                std::lock_guard<std::mutex> lock(result_mutex);
                                final_result.found = true;
                                final_result.device_id = device.id;
                                final_result.nonce = *reinterpret_cast<uint64_t*>(nonce_buf.contents);
                                std::memcpy(final_result.hash.data(), hash_buf.contents, 32);
                            }
                            break;
                        }
                    }
                } catch (const std::exception& e) {
                    found.store(true, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(result_mutex);
                    final_result.error = e.what();
                }
            }
            if (lane < final_result.device_hashes.size()) {
                final_result.device_hashes[lane] = lane_hashes;
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    final_result.hashes = total_hashes.load(std::memory_order_relaxed);
    final_result.cancelled = job_cancelled(job);
    if (!final_result.found && !final_result.cancelled && final_result.error.empty()) {
        final_result.timed_out = true;
        final_result.error = "Metal mining timeout after " + std::to_string(job.timeout_sec) +
            "s, hashes=" + std::to_string(final_result.hashes);
    }
    return final_result;
}

}  // namespace geocoin::tools::gpu_miner

#endif  // GEOCOIN_GPU_ENABLE_METAL
