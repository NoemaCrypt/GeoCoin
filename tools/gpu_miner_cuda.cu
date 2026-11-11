#include "gpu_miner_common.hpp"

#if defined(GEOCOIN_GPU_ENABLE_CUDA) || defined(GEOCOIN_GPU_ENABLE_HIP)

#ifdef GEOCOIN_GPU_ENABLE_HIP
#include <hip/hip_runtime.h>
#else
#include <cuda_runtime.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>

#ifdef GEOCOIN_GPU_ENABLE_HIP
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#define GEOCOIN_GPU_BACKEND_NAME "hip"
#define Cuda Hip
#define CudaWork HipWork
#define kCudaWork kHipWork
#define make_cuda_work make_hip_work
#define list_cuda_devices list_hip_devices
#define mine_cuda mine_hip
#define cuda_error hip_error
#define check_cuda check_hip
#define cudaError_t hipError_t
#define cudaSuccess hipSuccess
#define cudaGetErrorString hipGetErrorString
#define cudaGetDeviceCount hipGetDeviceCount
#define cudaDeviceProp hipDeviceProp_t
#define cudaGetDeviceProperties hipGetDeviceProperties
#define cudaSetDevice hipSetDevice
#define cudaFuncSetCacheConfig hipFuncSetCacheConfig
#define cudaFuncCachePreferL1 hipFuncCachePreferL1
#define cudaMalloc hipMalloc
#define cudaFree hipFree
#define cudaMemcpy hipMemcpy
#define cudaMemcpyToSymbol hipMemcpyToSymbol
#define cudaMemcpyHostToDevice hipMemcpyHostToDevice
#define cudaMemcpyDeviceToHost hipMemcpyDeviceToHost
#define cudaGetLastError hipGetLastError
#else
#define GEOCOIN_GPU_BACKEND_NAME "cuda"
#endif

namespace geocoin::tools::gpu_miner {
namespace {

#ifdef GEOCOIN_GPU_ENABLE_HIP
#ifdef _WIN32
bool hip_runtime_available()
{
    static HMODULE module = []() -> HMODULE {
        if (HMODULE hip7 = LoadLibraryA("amdhip64_7.dll")) {
            return hip7;
        }
        return LoadLibraryA("amdhip64_6.dll");
    }();
    return module != nullptr;
}
#else
bool hip_runtime_available()
{
    const char* dxg_detection = std::getenv("HSA_ENABLE_DXG_DETECTION");
    const bool wants_dxg = dxg_detection != nullptr &&
        dxg_detection[0] != '\0' &&
        dxg_detection[0] != '0';
    if (!wants_dxg) {
        return true;
    }

    bool is_wsl = false;
    if (FILE* osrelease = std::fopen("/proc/sys/kernel/osrelease", "r")) {
        char buffer[256]{};
        if (std::fgets(buffer, sizeof(buffer), osrelease) != nullptr) {
            is_wsl = std::strstr(buffer, "Microsoft") != nullptr ||
                     std::strstr(buffer, "microsoft") != nullptr ||
                     std::strstr(buffer, "WSL") != nullptr;
        }
        std::fclose(osrelease);
    }
    if (!is_wsl) {
        return true;
    }

    void* rocdxg = dlopen("librocdxg.so", RTLD_LAZY | RTLD_LOCAL);
    if (!rocdxg) {
        rocdxg = dlopen("/opt/rocm/lib/librocdxg.so", RTLD_LAZY | RTLD_LOCAL);
    }
    if (!rocdxg) {
        return false;
    }
    dlclose(rocdxg);
    return true;
}
#endif
#endif

constexpr size_t kHeaderSize = 124;

#ifndef GEOCOIN_CUDA_THREADS_PER_BLOCK
#define GEOCOIN_CUDA_THREADS_PER_BLOCK 512
#endif

#ifndef GEOCOIN_CUDA_NONCES_PER_THREAD
#define GEOCOIN_CUDA_NONCES_PER_THREAD 1
#endif

#ifndef GEOCOIN_CUDA_MIN_BLOCKS_PER_SM
#define GEOCOIN_CUDA_MIN_BLOCKS_PER_SM 1
#endif

#ifndef GEOCOIN_CUDA_PERSISTENT_GRID
#define GEOCOIN_CUDA_PERSISTENT_GRID 0
#endif

#ifndef GEOCOIN_CUDA_PERSISTENT_BLOCKS_PER_SM
#define GEOCOIN_CUDA_PERSISTENT_BLOCKS_PER_SM 2
#endif

#ifndef GEOCOIN_CUDA_CONSTANT_WORK
#define GEOCOIN_CUDA_CONSTANT_WORK 0
#endif

#ifndef GEOCOIN_CUDA_IMMEDIATE_K
#define GEOCOIN_CUDA_IMMEDIATE_K 1
#endif

#ifndef GEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS
#define GEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS 0
#endif

#ifndef GEOCOIN_CUDA_SPECIALIZE_FINAL_LEN_BLOCK
#define GEOCOIN_CUDA_SPECIALIZE_FINAL_LEN_BLOCK 0
#endif

__device__ __forceinline__ uint32_t rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

__device__ __forceinline__ void store_be32(unsigned char* p, uint32_t v)
{
    p[0] = static_cast<unsigned char>((v >> 24) & 0xffu);
    p[1] = static_cast<unsigned char>((v >> 16) & 0xffu);
    p[2] = static_cast<unsigned char>((v >> 8) & 0xffu);
    p[3] = static_cast<unsigned char>(v & 0xffu);
}

#if !GEOCOIN_CUDA_IMMEDIATE_K
__device__ __constant__ uint32_t kSha256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};
#endif

template<int I>
__device__ __forceinline__ uint32_t sha256_k()
{
    static_assert(I >= 0 && I < 64, "SHA256 round constant index out of range");
#if GEOCOIN_CUDA_IMMEDIATE_K
    if constexpr (I == 0) return 0x428a2f98u;
    else if constexpr (I == 1) return 0x71374491u;
    else if constexpr (I == 2) return 0xb5c0fbcfu;
    else if constexpr (I == 3) return 0xe9b5dba5u;
    else if constexpr (I == 4) return 0x3956c25bu;
    else if constexpr (I == 5) return 0x59f111f1u;
    else if constexpr (I == 6) return 0x923f82a4u;
    else if constexpr (I == 7) return 0xab1c5ed5u;
    else if constexpr (I == 8) return 0xd807aa98u;
    else if constexpr (I == 9) return 0x12835b01u;
    else if constexpr (I == 10) return 0x243185beu;
    else if constexpr (I == 11) return 0x550c7dc3u;
    else if constexpr (I == 12) return 0x72be5d74u;
    else if constexpr (I == 13) return 0x80deb1feu;
    else if constexpr (I == 14) return 0x9bdc06a7u;
    else if constexpr (I == 15) return 0xc19bf174u;
    else if constexpr (I == 16) return 0xe49b69c1u;
    else if constexpr (I == 17) return 0xefbe4786u;
    else if constexpr (I == 18) return 0x0fc19dc6u;
    else if constexpr (I == 19) return 0x240ca1ccu;
    else if constexpr (I == 20) return 0x2de92c6fu;
    else if constexpr (I == 21) return 0x4a7484aau;
    else if constexpr (I == 22) return 0x5cb0a9dcu;
    else if constexpr (I == 23) return 0x76f988dau;
    else if constexpr (I == 24) return 0x983e5152u;
    else if constexpr (I == 25) return 0xa831c66du;
    else if constexpr (I == 26) return 0xb00327c8u;
    else if constexpr (I == 27) return 0xbf597fc7u;
    else if constexpr (I == 28) return 0xc6e00bf3u;
    else if constexpr (I == 29) return 0xd5a79147u;
    else if constexpr (I == 30) return 0x06ca6351u;
    else if constexpr (I == 31) return 0x14292967u;
    else if constexpr (I == 32) return 0x27b70a85u;
    else if constexpr (I == 33) return 0x2e1b2138u;
    else if constexpr (I == 34) return 0x4d2c6dfcu;
    else if constexpr (I == 35) return 0x53380d13u;
    else if constexpr (I == 36) return 0x650a7354u;
    else if constexpr (I == 37) return 0x766a0abbu;
    else if constexpr (I == 38) return 0x81c2c92eu;
    else if constexpr (I == 39) return 0x92722c85u;
    else if constexpr (I == 40) return 0xa2bfe8a1u;
    else if constexpr (I == 41) return 0xa81a664bu;
    else if constexpr (I == 42) return 0xc24b8b70u;
    else if constexpr (I == 43) return 0xc76c51a3u;
    else if constexpr (I == 44) return 0xd192e819u;
    else if constexpr (I == 45) return 0xd6990624u;
    else if constexpr (I == 46) return 0xf40e3585u;
    else if constexpr (I == 47) return 0x106aa070u;
    else if constexpr (I == 48) return 0x19a4c116u;
    else if constexpr (I == 49) return 0x1e376c08u;
    else if constexpr (I == 50) return 0x2748774cu;
    else if constexpr (I == 51) return 0x34b0bcb5u;
    else if constexpr (I == 52) return 0x391c0cb3u;
    else if constexpr (I == 53) return 0x4ed8aa4au;
    else if constexpr (I == 54) return 0x5b9cca4fu;
    else if constexpr (I == 55) return 0x682e6ff3u;
    else if constexpr (I == 56) return 0x748f82eeu;
    else if constexpr (I == 57) return 0x78a5636fu;
    else if constexpr (I == 58) return 0x84c87814u;
    else if constexpr (I == 59) return 0x8cc70208u;
    else if constexpr (I == 60) return 0x90befffau;
    else if constexpr (I == 61) return 0xa4506cebu;
    else if constexpr (I == 62) return 0xbef9a3f7u;
    else return 0xc67178f2u;
#else
    return kSha256[I];
#endif
}

#if GEOCOIN_CUDA_SPECIALIZE_FINAL_LEN_BLOCK
__device__ __constant__ uint32_t kSha256Final124W[64] = {
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x000003e0u,
    0x00000000u, 0x018c0000u, 0x00000000u, 0x800063f7u,
    0x00000000u, 0x3da53018u, 0x000003e0u, 0x3e00702au,
    0x03180000u, 0x361fd8dcu, 0x80012be6u, 0x17781a3au,
    0xee54c062u, 0x0e5f9b35u, 0xf6ff3075u, 0x4217b4e2u,
    0xb45c6babu, 0xae8b188au, 0xb803c2b2u, 0x5dd4c4abu,
    0x234a8a30u, 0xf0741169u, 0xa6250195u, 0x7d67e7cbu,
    0x1b0dd04du, 0x9520ab9fu, 0xa04f54d1u, 0x636a9ea1u,
    0xacbdd98du, 0x0a504a1au, 0xee41ae6fu, 0xe47c2e54u,
    0xf2ed904fu, 0x649130e8u, 0x01ad040bu, 0x8fa6854eu,
    0x19d375feu, 0xf0d0c316u, 0xbf8cd766u, 0xd76ff1c6u,
    0x78791da3u, 0x00393670u, 0xc67585c6u, 0xb3e1cea2u,
    0x37d0ee7bu, 0x52166680u, 0xef82ede7u, 0x42258859u
};
#endif

struct CudaWork {
    uint32_t midstate[8];
#if GEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS
    uint32_t tail_state_after_13[8];
#endif
    uint32_t tail_words[13];
    uint32_t target_words[8];
};

#if GEOCOIN_CUDA_CONSTANT_WORK
__device__ __constant__ CudaWork kCudaWork;
#endif

__host__ __device__ __forceinline__ uint32_t bswap32_fast(uint32_t x)
{
#if defined(__CUDA_ARCH__) && !defined(__HIP_DEVICE_COMPILE__)
    return __byte_perm(x, 0u, 0x0123u);
#else
    return ((x & 0x000000ffu) << 24) |
           ((x & 0x0000ff00u) << 8) |
           ((x & 0x00ff0000u) >> 8) |
           ((x & 0xff000000u) >> 24);
#endif
}

__device__ __forceinline__ void add_u64_words(uint32_t& lo, uint32_t& hi, uint32_t add_lo, uint32_t add_hi)
{
    const uint32_t old_lo = lo;
    lo += add_lo;
    hi += add_hi + (lo < old_lo ? 1u : 0u);
}

__device__ __forceinline__ uint32_t sha_ch(uint32_t x, uint32_t y, uint32_t z)
{
    return z ^ (x & (y ^ z));
}

__device__ __forceinline__ uint32_t sha_maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) | (z & (x ^ y));
}

__device__ __forceinline__ uint32_t sha_big0(uint32_t x)
{
    return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}

__device__ __forceinline__ uint32_t sha_big1(uint32_t x)
{
    return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}

__device__ __forceinline__ uint32_t sha_small0(uint32_t x)
{
    return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}

__device__ __forceinline__ uint32_t sha_small1(uint32_t x)
{
    return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

#define GEOCOIN_SHA256_STEP(a, b, c, d, e, f, g, h, x, k) \
    do { \
        (h) += (k) + (x) + sha_big1(e) + sha_ch((e), (f), (g)); \
        (d) += (h); \
        (h) += sha_big0(a) + sha_maj((a), (b), (c)); \
    } while (0)

#define GEOCOIN_SHA256_EXPAND(x, y, z, w) (sha_small1(x) + (y) + sha_small0(z) + (w))

__device__ __forceinline__ void sha256_compress_words(
    uint32_t state[8],
    uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3,
    uint32_t w4, uint32_t w5, uint32_t w6, uint32_t w7,
    uint32_t w8, uint32_t w9, uint32_t w10, uint32_t w11,
    uint32_t w12, uint32_t w13, uint32_t w14, uint32_t w15)
{
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

#define GEOCOIN_ROUND_STEP(i) \
    do { \
        GEOCOIN_SHA256_STEP(a, b, c, d, e, f, g, h, w0, sha256_k<(i) + 0>()); \
        GEOCOIN_SHA256_STEP(h, a, b, c, d, e, f, g, w1, sha256_k<(i) + 1>()); \
        GEOCOIN_SHA256_STEP(g, h, a, b, c, d, e, f, w2, sha256_k<(i) + 2>()); \
        GEOCOIN_SHA256_STEP(f, g, h, a, b, c, d, e, w3, sha256_k<(i) + 3>()); \
        GEOCOIN_SHA256_STEP(e, f, g, h, a, b, c, d, w4, sha256_k<(i) + 4>()); \
        GEOCOIN_SHA256_STEP(d, e, f, g, h, a, b, c, w5, sha256_k<(i) + 5>()); \
        GEOCOIN_SHA256_STEP(c, d, e, f, g, h, a, b, w6, sha256_k<(i) + 6>()); \
        GEOCOIN_SHA256_STEP(b, c, d, e, f, g, h, a, w7, sha256_k<(i) + 7>()); \
        GEOCOIN_SHA256_STEP(a, b, c, d, e, f, g, h, w8, sha256_k<(i) + 8>()); \
        GEOCOIN_SHA256_STEP(h, a, b, c, d, e, f, g, w9, sha256_k<(i) + 9>()); \
        GEOCOIN_SHA256_STEP(g, h, a, b, c, d, e, f, w10, sha256_k<(i) + 10>()); \
        GEOCOIN_SHA256_STEP(f, g, h, a, b, c, d, e, w11, sha256_k<(i) + 11>()); \
        GEOCOIN_SHA256_STEP(e, f, g, h, a, b, c, d, w12, sha256_k<(i) + 12>()); \
        GEOCOIN_SHA256_STEP(d, e, f, g, h, a, b, c, w13, sha256_k<(i) + 13>()); \
        GEOCOIN_SHA256_STEP(c, d, e, f, g, h, a, b, w14, sha256_k<(i) + 14>()); \
        GEOCOIN_SHA256_STEP(b, c, d, e, f, g, h, a, w15, sha256_k<(i) + 15>()); \
    } while (0)

#define GEOCOIN_ROUND_EXPAND() \
    do { \
        w0 = GEOCOIN_SHA256_EXPAND(w14, w9, w1, w0); \
        w1 = GEOCOIN_SHA256_EXPAND(w15, w10, w2, w1); \
        w2 = GEOCOIN_SHA256_EXPAND(w0, w11, w3, w2); \
        w3 = GEOCOIN_SHA256_EXPAND(w1, w12, w4, w3); \
        w4 = GEOCOIN_SHA256_EXPAND(w2, w13, w5, w4); \
        w5 = GEOCOIN_SHA256_EXPAND(w3, w14, w6, w5); \
        w6 = GEOCOIN_SHA256_EXPAND(w4, w15, w7, w6); \
        w7 = GEOCOIN_SHA256_EXPAND(w5, w0, w8, w7); \
        w8 = GEOCOIN_SHA256_EXPAND(w6, w1, w9, w8); \
        w9 = GEOCOIN_SHA256_EXPAND(w7, w2, w10, w9); \
        w10 = GEOCOIN_SHA256_EXPAND(w8, w3, w11, w10); \
        w11 = GEOCOIN_SHA256_EXPAND(w9, w4, w12, w11); \
        w12 = GEOCOIN_SHA256_EXPAND(w10, w5, w13, w12); \
        w13 = GEOCOIN_SHA256_EXPAND(w11, w6, w14, w13); \
        w14 = GEOCOIN_SHA256_EXPAND(w12, w7, w15, w14); \
        w15 = GEOCOIN_SHA256_EXPAND(w13, w8, w0, w15); \
    } while (0)

    GEOCOIN_ROUND_STEP(0);
    GEOCOIN_ROUND_EXPAND();
    GEOCOIN_ROUND_STEP(16);
    GEOCOIN_ROUND_EXPAND();
    GEOCOIN_ROUND_STEP(32);
    GEOCOIN_ROUND_EXPAND();
    GEOCOIN_ROUND_STEP(48);

#undef GEOCOIN_ROUND_EXPAND
#undef GEOCOIN_ROUND_STEP

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

#if GEOCOIN_CUDA_SPECIALIZE_FINAL_LEN_BLOCK
__device__ __forceinline__ void sha256_compress_final_124_length_block(uint32_t state[8])
{
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

#define GEOCOIN_FINAL124_STEP(a, b, c, d, e, f, g, h, kw) \
    GEOCOIN_SHA256_STEP(a, b, c, d, e, f, g, h, 0u, kw)

    GEOCOIN_FINAL124_STEP(a, b, c, d, e, f, g, h, 0x428a2f98u);
    GEOCOIN_FINAL124_STEP(h, a, b, c, d, e, f, g, 0x71374491u);
    GEOCOIN_FINAL124_STEP(g, h, a, b, c, d, e, f, 0xb5c0fbcfu);
    GEOCOIN_FINAL124_STEP(f, g, h, a, b, c, d, e, 0xe9b5dba5u);
    GEOCOIN_FINAL124_STEP(e, f, g, h, a, b, c, d, 0x3956c25bu);
    GEOCOIN_FINAL124_STEP(d, e, f, g, h, a, b, c, 0x59f111f1u);
    GEOCOIN_FINAL124_STEP(c, d, e, f, g, h, a, b, 0x923f82a4u);
    GEOCOIN_FINAL124_STEP(b, c, d, e, f, g, h, a, 0xab1c5ed5u);
    GEOCOIN_FINAL124_STEP(a, b, c, d, e, f, g, h, 0xd807aa98u);
    GEOCOIN_FINAL124_STEP(h, a, b, c, d, e, f, g, 0x12835b01u);
    GEOCOIN_FINAL124_STEP(g, h, a, b, c, d, e, f, 0x243185beu);
    GEOCOIN_FINAL124_STEP(f, g, h, a, b, c, d, e, 0x550c7dc3u);
    GEOCOIN_FINAL124_STEP(e, f, g, h, a, b, c, d, 0x72be5d74u);
    GEOCOIN_FINAL124_STEP(d, e, f, g, h, a, b, c, 0x80deb1feu);
    GEOCOIN_FINAL124_STEP(c, d, e, f, g, h, a, b, 0x9bdc06a7u);
    GEOCOIN_FINAL124_STEP(b, c, d, e, f, g, h, a, 0xc19bf554u);
    GEOCOIN_FINAL124_STEP(a, b, c, d, e, f, g, h, 0xe49b69c1u);
    GEOCOIN_FINAL124_STEP(h, a, b, c, d, e, f, g, 0xf14a4786u);
    GEOCOIN_FINAL124_STEP(g, h, a, b, c, d, e, f, 0x0fc19dc6u);
    GEOCOIN_FINAL124_STEP(f, g, h, a, b, c, d, e, 0xa40d05c3u);
    GEOCOIN_FINAL124_STEP(e, f, g, h, a, b, c, d, 0x2de92c6fu);
    GEOCOIN_FINAL124_STEP(d, e, f, g, h, a, b, c, 0x8819b4c2u);
    GEOCOIN_FINAL124_STEP(c, d, e, f, g, h, a, b, 0x5cb0adbcu);
    GEOCOIN_FINAL124_STEP(b, c, d, e, f, g, h, a, 0xb4f9f904u);
    GEOCOIN_FINAL124_STEP(a, b, c, d, e, f, g, h, 0x9b565152u);
    GEOCOIN_FINAL124_STEP(h, a, b, c, d, e, f, g, 0xde519f49u);
    GEOCOIN_FINAL124_STEP(g, h, a, b, c, d, e, f, 0x300453aeu);
    GEOCOIN_FINAL124_STEP(f, g, h, a, b, c, d, e, 0xd6d19a01u);
    GEOCOIN_FINAL124_STEP(e, f, g, h, a, b, c, d, 0xb534cc55u);
    GEOCOIN_FINAL124_STEP(d, e, f, g, h, a, b, c, 0xe4072c7cu);
    GEOCOIN_FINAL124_STEP(c, d, e, f, g, h, a, b, 0xfdc993c6u);
    GEOCOIN_FINAL124_STEP(b, c, d, e, f, g, h, a, 0x5640de49u);
    GEOCOIN_FINAL124_STEP(a, b, c, d, e, f, g, h, 0xdc137630u);
    GEOCOIN_FINAL124_STEP(h, a, b, c, d, e, f, g, 0xdca639c2u);
    GEOCOIN_FINAL124_STEP(g, h, a, b, c, d, e, f, 0x053030aeu);
    GEOCOIN_FINAL124_STEP(f, g, h, a, b, c, d, e, 0xb10cd1beu);
    GEOCOIN_FINAL124_STEP(e, f, g, h, a, b, c, d, 0x8854fd84u);
    GEOCOIN_FINAL124_STEP(d, e, f, g, h, a, b, c, 0x66de1c24u);
    GEOCOIN_FINAL124_STEP(c, d, e, f, g, h, a, b, 0x27e7cac3u);
    GEOCOIN_FINAL124_STEP(b, c, d, e, f, g, h, a, 0x0fda1450u);
    GEOCOIN_FINAL124_STEP(a, b, c, d, e, f, g, h, 0xbdcdb8eeu);
    GEOCOIN_FINAL124_STEP(h, a, b, c, d, e, f, g, 0x3d3b11eau);
    GEOCOIN_FINAL124_STEP(g, h, a, b, c, d, e, f, 0x629ae041u);
    GEOCOIN_FINAL124_STEP(f, g, h, a, b, c, d, e, 0x2ad6f044u);
    GEOCOIN_FINAL124_STEP(e, f, g, h, a, b, c, d, 0x7e50c1a6u);
    GEOCOIN_FINAL124_STEP(d, e, f, g, h, a, b, c, 0xe0e9503eu);
    GEOCOIN_FINAL124_STEP(c, d, e, f, g, h, a, b, 0xe24fe3f4u);
    GEOCOIN_FINAL124_STEP(b, c, d, e, f, g, h, a, 0xf4e6cec4u);
    GEOCOIN_FINAL124_STEP(a, b, c, d, e, f, g, h, 0x0c925165u);
    GEOCOIN_FINAL124_STEP(h, a, b, c, d, e, f, g, 0x82c89cf0u);
    GEOCOIN_FINAL124_STEP(g, h, a, b, c, d, e, f, 0x28f57b57u);
    GEOCOIN_FINAL124_STEP(f, g, h, a, b, c, d, e, 0xc4574203u);
    GEOCOIN_FINAL124_STEP(e, f, g, h, a, b, c, d, 0x52ef82b1u);
    GEOCOIN_FINAL124_STEP(d, e, f, g, h, a, b, c, 0x3fa96d60u);
    GEOCOIN_FINAL124_STEP(c, d, e, f, g, h, a, b, 0x1b29a1b5u);
    GEOCOIN_FINAL124_STEP(b, c, d, e, f, g, h, a, 0x3f9e61b9u);
    GEOCOIN_FINAL124_STEP(a, b, c, d, e, f, g, h, 0xed08a091u);
    GEOCOIN_FINAL124_STEP(h, a, b, c, d, e, f, g, 0x78de99dfu);
    GEOCOIN_FINAL124_STEP(g, h, a, b, c, d, e, f, 0x4b3dfddau);
    GEOCOIN_FINAL124_STEP(f, g, h, a, b, c, d, e, 0x40a8d0aau);
    GEOCOIN_FINAL124_STEP(e, f, g, h, a, b, c, d, 0xc88fee75u);
    GEOCOIN_FINAL124_STEP(d, e, f, g, h, a, b, c, 0xf666d36bu);
    GEOCOIN_FINAL124_STEP(c, d, e, f, g, h, a, b, 0xae7c91deu);
    GEOCOIN_FINAL124_STEP(b, c, d, e, f, g, h, a, 0x0897014bu);

#undef GEOCOIN_FINAL124_STEP

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}
#endif

#if GEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS
__device__ __forceinline__ void sha256_compress_tail_from_round13(
#if !GEOCOIN_CUDA_CONSTANT_WORK
    const CudaWork* __restrict__ work,
#endif
    uint32_t state[8],
    uint32_t nonce_lo_be,
    uint32_t nonce_hi_be)
{
#if GEOCOIN_CUDA_CONSTANT_WORK
    uint32_t a = kCudaWork.tail_state_after_13[0];
    uint32_t b = kCudaWork.tail_state_after_13[1];
    uint32_t c = kCudaWork.tail_state_after_13[2];
    uint32_t d = kCudaWork.tail_state_after_13[3];
    uint32_t e = kCudaWork.tail_state_after_13[4];
    uint32_t f = kCudaWork.tail_state_after_13[5];
    uint32_t g = kCudaWork.tail_state_after_13[6];
    uint32_t h = kCudaWork.tail_state_after_13[7];

    uint32_t w0 = kCudaWork.tail_words[0];
    uint32_t w1 = kCudaWork.tail_words[1];
    uint32_t w2 = kCudaWork.tail_words[2];
    uint32_t w3 = kCudaWork.tail_words[3];
    uint32_t w4 = kCudaWork.tail_words[4];
    uint32_t w5 = kCudaWork.tail_words[5];
    uint32_t w6 = kCudaWork.tail_words[6];
    uint32_t w7 = kCudaWork.tail_words[7];
    uint32_t w8 = kCudaWork.tail_words[8];
    uint32_t w9 = kCudaWork.tail_words[9];
    uint32_t w10 = kCudaWork.tail_words[10];
    uint32_t w11 = kCudaWork.tail_words[11];
    uint32_t w12 = kCudaWork.tail_words[12];
#else
    uint32_t a = work->tail_state_after_13[0];
    uint32_t b = work->tail_state_after_13[1];
    uint32_t c = work->tail_state_after_13[2];
    uint32_t d = work->tail_state_after_13[3];
    uint32_t e = work->tail_state_after_13[4];
    uint32_t f = work->tail_state_after_13[5];
    uint32_t g = work->tail_state_after_13[6];
    uint32_t h = work->tail_state_after_13[7];

    uint32_t w0 = work->tail_words[0];
    uint32_t w1 = work->tail_words[1];
    uint32_t w2 = work->tail_words[2];
    uint32_t w3 = work->tail_words[3];
    uint32_t w4 = work->tail_words[4];
    uint32_t w5 = work->tail_words[5];
    uint32_t w6 = work->tail_words[6];
    uint32_t w7 = work->tail_words[7];
    uint32_t w8 = work->tail_words[8];
    uint32_t w9 = work->tail_words[9];
    uint32_t w10 = work->tail_words[10];
    uint32_t w11 = work->tail_words[11];
    uint32_t w12 = work->tail_words[12];
#endif
    uint32_t w13 = nonce_lo_be;
    uint32_t w14 = nonce_hi_be;
    uint32_t w15 = 0x80000000u;

    GEOCOIN_SHA256_STEP(d, e, f, g, h, a, b, c, w13, sha256_k<13>());
    GEOCOIN_SHA256_STEP(c, d, e, f, g, h, a, b, w14, sha256_k<14>());
    GEOCOIN_SHA256_STEP(b, c, d, e, f, g, h, a, w15, sha256_k<15>());

#define GEOCOIN_ROUND_STEP_FROM_16(i) \
    do { \
        GEOCOIN_SHA256_STEP(a, b, c, d, e, f, g, h, w0, sha256_k<(i) + 0>()); \
        GEOCOIN_SHA256_STEP(h, a, b, c, d, e, f, g, w1, sha256_k<(i) + 1>()); \
        GEOCOIN_SHA256_STEP(g, h, a, b, c, d, e, f, w2, sha256_k<(i) + 2>()); \
        GEOCOIN_SHA256_STEP(f, g, h, a, b, c, d, e, w3, sha256_k<(i) + 3>()); \
        GEOCOIN_SHA256_STEP(e, f, g, h, a, b, c, d, w4, sha256_k<(i) + 4>()); \
        GEOCOIN_SHA256_STEP(d, e, f, g, h, a, b, c, w5, sha256_k<(i) + 5>()); \
        GEOCOIN_SHA256_STEP(c, d, e, f, g, h, a, b, w6, sha256_k<(i) + 6>()); \
        GEOCOIN_SHA256_STEP(b, c, d, e, f, g, h, a, w7, sha256_k<(i) + 7>()); \
        GEOCOIN_SHA256_STEP(a, b, c, d, e, f, g, h, w8, sha256_k<(i) + 8>()); \
        GEOCOIN_SHA256_STEP(h, a, b, c, d, e, f, g, w9, sha256_k<(i) + 9>()); \
        GEOCOIN_SHA256_STEP(g, h, a, b, c, d, e, f, w10, sha256_k<(i) + 10>()); \
        GEOCOIN_SHA256_STEP(f, g, h, a, b, c, d, e, w11, sha256_k<(i) + 11>()); \
        GEOCOIN_SHA256_STEP(e, f, g, h, a, b, c, d, w12, sha256_k<(i) + 12>()); \
        GEOCOIN_SHA256_STEP(d, e, f, g, h, a, b, c, w13, sha256_k<(i) + 13>()); \
        GEOCOIN_SHA256_STEP(c, d, e, f, g, h, a, b, w14, sha256_k<(i) + 14>()); \
        GEOCOIN_SHA256_STEP(b, c, d, e, f, g, h, a, w15, sha256_k<(i) + 15>()); \
    } while (0)

#define GEOCOIN_ROUND_EXPAND_FROM_16() \
    do { \
        w0 = GEOCOIN_SHA256_EXPAND(w14, w9, w1, w0); \
        w1 = GEOCOIN_SHA256_EXPAND(w15, w10, w2, w1); \
        w2 = GEOCOIN_SHA256_EXPAND(w0, w11, w3, w2); \
        w3 = GEOCOIN_SHA256_EXPAND(w1, w12, w4, w3); \
        w4 = GEOCOIN_SHA256_EXPAND(w2, w13, w5, w4); \
        w5 = GEOCOIN_SHA256_EXPAND(w3, w14, w6, w5); \
        w6 = GEOCOIN_SHA256_EXPAND(w4, w15, w7, w6); \
        w7 = GEOCOIN_SHA256_EXPAND(w5, w0, w8, w7); \
        w8 = GEOCOIN_SHA256_EXPAND(w6, w1, w9, w8); \
        w9 = GEOCOIN_SHA256_EXPAND(w7, w2, w10, w9); \
        w10 = GEOCOIN_SHA256_EXPAND(w8, w3, w11, w10); \
        w11 = GEOCOIN_SHA256_EXPAND(w9, w4, w12, w11); \
        w12 = GEOCOIN_SHA256_EXPAND(w10, w5, w13, w12); \
        w13 = GEOCOIN_SHA256_EXPAND(w11, w6, w14, w13); \
        w14 = GEOCOIN_SHA256_EXPAND(w12, w7, w15, w14); \
        w15 = GEOCOIN_SHA256_EXPAND(w13, w8, w0, w15); \
    } while (0)

    GEOCOIN_ROUND_EXPAND_FROM_16();
    GEOCOIN_ROUND_STEP_FROM_16(16);
    GEOCOIN_ROUND_EXPAND_FROM_16();
    GEOCOIN_ROUND_STEP_FROM_16(32);
    GEOCOIN_ROUND_EXPAND_FROM_16();
    GEOCOIN_ROUND_STEP_FROM_16(48);

#undef GEOCOIN_ROUND_EXPAND_FROM_16
#undef GEOCOIN_ROUND_STEP_FROM_16

#if GEOCOIN_CUDA_CONSTANT_WORK
    state[0] = kCudaWork.midstate[0] + a;
    state[1] = kCudaWork.midstate[1] + b;
    state[2] = kCudaWork.midstate[2] + c;
    state[3] = kCudaWork.midstate[3] + d;
    state[4] = kCudaWork.midstate[4] + e;
    state[5] = kCudaWork.midstate[5] + f;
    state[6] = kCudaWork.midstate[6] + g;
    state[7] = kCudaWork.midstate[7] + h;
#else
    state[0] = work->midstate[0] + a;
    state[1] = work->midstate[1] + b;
    state[2] = work->midstate[2] + c;
    state[3] = work->midstate[3] + d;
    state[4] = work->midstate[4] + e;
    state[5] = work->midstate[5] + f;
    state[6] = work->midstate[6] + g;
    state[7] = work->midstate[7] + h;
#endif
}
#endif

__device__ __forceinline__ bool hash_words_meet_target(const uint32_t hash[8], const uint32_t target[8])
{
#pragma unroll 8
    for (int i = 0; i < 8; ++i) {
        if (hash[i] < target[i]) {
            return true;
        }
        if (hash[i] > target[i]) {
            return false;
        }
    }
    return true;
}

#undef GEOCOIN_SHA256_EXPAND
#undef GEOCOIN_SHA256_STEP

__global__ __launch_bounds__(GEOCOIN_CUDA_THREADS_PER_BLOCK, GEOCOIN_CUDA_MIN_BLOCKS_PER_SM) void geocoin_mine_kernel_fast(
#if !GEOCOIN_CUDA_CONSTANT_WORK
    const CudaWork* __restrict__ work,
#endif
    uint64_t base_nonce,
    uint64_t nonce_stride,
    uint64_t attempts,
    int* found,
    uint64_t* found_nonce,
    unsigned char* found_hash)
{
    const uint64_t tid = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const uint64_t total_threads = static_cast<uint64_t>(gridDim.x) * blockDim.x;

    const uint32_t stride_lo = static_cast<uint32_t>(nonce_stride);
    const uint32_t tid_lo = static_cast<uint32_t>(tid);
    const uint32_t tid_hi = static_cast<uint32_t>(tid >> 32);
    const uint32_t offset_lo = tid_lo * stride_lo;
    const uint32_t offset_hi = __umulhi(tid_lo, stride_lo) + tid_hi * stride_lo;
    uint32_t nonce_lo = static_cast<uint32_t>(base_nonce);
    uint32_t nonce_hi = static_cast<uint32_t>(base_nonce >> 32);
    add_u64_words(nonce_lo, nonce_hi, offset_lo, offset_hi);

    const uint64_t nonce_step64 = total_threads * nonce_stride;
    const uint32_t nonce_step_lo = static_cast<uint32_t>(nonce_step64);
    const uint32_t nonce_step_hi = static_cast<uint32_t>(nonce_step64 >> 32);

    const uint64_t attempt_step = total_threads * static_cast<uint64_t>(GEOCOIN_CUDA_NONCES_PER_THREAD);
    uint64_t attempt_index = tid;

    while (attempt_index < attempts) {
#pragma unroll
    for (uint32_t nonce_iter = 0; nonce_iter < GEOCOIN_CUDA_NONCES_PER_THREAD; ++nonce_iter) {
    if (attempt_index + static_cast<uint64_t>(nonce_iter) * total_threads >= attempts) {
        break;
    }
    const uint32_t nonce_lo_be = bswap32_fast(nonce_lo);
    const uint32_t nonce_hi_be = bswap32_fast(nonce_hi);

    uint32_t state[8];
#if GEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS
    sha256_compress_tail_from_round13(
#if GEOCOIN_CUDA_CONSTANT_WORK
#else
        work,
#endif
        state,
        nonce_lo_be,
        nonce_hi_be);
#else
    state[0] =
#if GEOCOIN_CUDA_CONSTANT_WORK
        kCudaWork.midstate[0];
#else
        work->midstate[0];
#endif
    state[1] =
#if GEOCOIN_CUDA_CONSTANT_WORK
        kCudaWork.midstate[1];
#else
        work->midstate[1];
#endif
    state[2] =
#if GEOCOIN_CUDA_CONSTANT_WORK
        kCudaWork.midstate[2];
#else
        work->midstate[2];
#endif
    state[3] =
#if GEOCOIN_CUDA_CONSTANT_WORK
        kCudaWork.midstate[3];
#else
        work->midstate[3];
#endif
    state[4] =
#if GEOCOIN_CUDA_CONSTANT_WORK
        kCudaWork.midstate[4];
#else
        work->midstate[4];
#endif
    state[5] =
#if GEOCOIN_CUDA_CONSTANT_WORK
        kCudaWork.midstate[5];
#else
        work->midstate[5];
#endif
    state[6] =
#if GEOCOIN_CUDA_CONSTANT_WORK
        kCudaWork.midstate[6];
#else
        work->midstate[6];
#endif
    state[7] =
#if GEOCOIN_CUDA_CONSTANT_WORK
        kCudaWork.midstate[7];
#else
        work->midstate[7];
#endif

    sha256_compress_words(
        state,
#if GEOCOIN_CUDA_CONSTANT_WORK
        kCudaWork.tail_words[0], kCudaWork.tail_words[1], kCudaWork.tail_words[2], kCudaWork.tail_words[3],
        kCudaWork.tail_words[4], kCudaWork.tail_words[5], kCudaWork.tail_words[6], kCudaWork.tail_words[7],
        kCudaWork.tail_words[8], kCudaWork.tail_words[9], kCudaWork.tail_words[10], kCudaWork.tail_words[11],
        kCudaWork.tail_words[12], nonce_lo_be, nonce_hi_be, 0x80000000u);
#else
        work->tail_words[0], work->tail_words[1], work->tail_words[2], work->tail_words[3],
        work->tail_words[4], work->tail_words[5], work->tail_words[6], work->tail_words[7],
        work->tail_words[8], work->tail_words[9], work->tail_words[10], work->tail_words[11],
        work->tail_words[12], nonce_lo_be, nonce_hi_be, 0x80000000u);
#endif
#endif

#if GEOCOIN_CUDA_SPECIALIZE_FINAL_LEN_BLOCK
    sha256_compress_final_124_length_block(state);
#else
    sha256_compress_words(
        state,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, static_cast<uint32_t>(kHeaderSize * 8u));
#endif

    uint32_t hash[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    sha256_compress_words(
        hash,
        state[0], state[1], state[2], state[3],
        state[4], state[5], state[6], state[7],
        0x80000000u, 0u, 0u, 0u, 0u, 0u, 0u, 256u);

    if (hash_words_meet_target(hash,
#if GEOCOIN_CUDA_CONSTANT_WORK
                               kCudaWork.target_words
#else
                               work->target_words
#endif
                               )) {
        if (atomicCAS(found, 0, 1) == 0) {
            *found_nonce = (static_cast<uint64_t>(nonce_hi) << 32) | nonce_lo;
#pragma unroll 8
            for (int i = 0; i < 8; ++i) {
                store_be32(found_hash + i * 4, hash[i]);
            }
        }
    }
    add_u64_words(nonce_lo, nonce_hi, nonce_step_lo, nonce_step_hi);
    }
    attempt_index += attempt_step;
    }
}

std::string cuda_error(cudaError_t error)
{
    return cudaGetErrorString(error);
}

void check_cuda(cudaError_t error, const char* operation)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string(operation) + " failed: " + cuda_error(error));
    }
}

geocoin::uint256 bits_to_target(uint32_t bits)
{
    const uint8_t exponent = static_cast<uint8_t>(bits >> 24);
    uint32_t mantissa = bits & 0x007fffff;
    const bool negative = (bits & 0x00800000) != 0;

    geocoin::uint256 target;
    target.data[0] = 0;
    target.data[1] = 0;
    target.data[2] = 0;
    target.data[3] = 0;

    if (exponent <= 3) {
        mantissa >>= 8 * (3 - exponent);
        target.data[0] = mantissa;
    } else {
        const int shift = 8 * (exponent - 3);
        const int word_index = shift / 64;
        const int bit_shift = shift % 64;
        if (word_index < 4) {
            target.data[word_index] |= static_cast<uint64_t>(mantissa) << bit_shift;
        }
        if (bit_shift > 64 - 24 && word_index + 1 < 4) {
            target.data[word_index + 1] |= static_cast<uint64_t>(mantissa) >> (64 - bit_shift);
        }
    }
    if (negative) {
        target.data[0] = target.data[1] = target.data[2] = target.data[3] = 0;
    }
    return target;
}

void target_to_be32(const geocoin::uint256& target, unsigned char out[32])
{
    for (int word = 3; word >= 0; --word) {
        const uint64_t value = target.data[word];
        const int offset = (3 - word) * 8;
        for (int i = 0; i < 8; ++i) {
            out[offset + i] = static_cast<unsigned char>((value >> ((7 - i) * 8)) & 0xffu);
        }
    }
}

uint32_t host_rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

uint32_t host_load_be32(const unsigned char* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

constexpr uint32_t kHostSha256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

void host_sha256_transform(uint32_t state[8], const unsigned char block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = host_load_be32(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = host_rotr32(w[i - 15], 7) ^ host_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = host_rotr32(w[i - 2], 17) ^ host_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = host_rotr32(e, 6) ^ host_rotr32(e, 11) ^ host_rotr32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + ch + kHostSha256[i] + w[i];
        const uint32_t s0 = host_rotr32(a, 2) ^ host_rotr32(a, 13) ^ host_rotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

#if GEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS
uint32_t host_sha_ch(uint32_t x, uint32_t y, uint32_t z)
{
    return z ^ (x & (y ^ z));
}

uint32_t host_sha_maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) | (z & (x ^ y));
}

uint32_t host_sha_big0(uint32_t x)
{
    return host_rotr32(x, 2) ^ host_rotr32(x, 13) ^ host_rotr32(x, 22);
}

uint32_t host_sha_big1(uint32_t x)
{
    return host_rotr32(x, 6) ^ host_rotr32(x, 11) ^ host_rotr32(x, 25);
}

void host_sha256_step(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d,
                      uint32_t& e, uint32_t& f, uint32_t& g, uint32_t& h,
                      uint32_t x, uint32_t k)
{
    h += k + x + host_sha_big1(e) + host_sha_ch(e, f, g);
    d += h;
    h += host_sha_big0(a) + host_sha_maj(a, b, c);
}

void host_precompute_tail_round13(const uint32_t midstate[8], const uint32_t tail_words[13], uint32_t out[8])
{
    uint32_t a = midstate[0];
    uint32_t b = midstate[1];
    uint32_t c = midstate[2];
    uint32_t d = midstate[3];
    uint32_t e = midstate[4];
    uint32_t f = midstate[5];
    uint32_t g = midstate[6];
    uint32_t h = midstate[7];

    host_sha256_step(a, b, c, d, e, f, g, h, tail_words[0], kHostSha256[0]);
    host_sha256_step(h, a, b, c, d, e, f, g, tail_words[1], kHostSha256[1]);
    host_sha256_step(g, h, a, b, c, d, e, f, tail_words[2], kHostSha256[2]);
    host_sha256_step(f, g, h, a, b, c, d, e, tail_words[3], kHostSha256[3]);
    host_sha256_step(e, f, g, h, a, b, c, d, tail_words[4], kHostSha256[4]);
    host_sha256_step(d, e, f, g, h, a, b, c, tail_words[5], kHostSha256[5]);
    host_sha256_step(c, d, e, f, g, h, a, b, tail_words[6], kHostSha256[6]);
    host_sha256_step(b, c, d, e, f, g, h, a, tail_words[7], kHostSha256[7]);
    host_sha256_step(a, b, c, d, e, f, g, h, tail_words[8], kHostSha256[8]);
    host_sha256_step(h, a, b, c, d, e, f, g, tail_words[9], kHostSha256[9]);
    host_sha256_step(g, h, a, b, c, d, e, f, tail_words[10], kHostSha256[10]);
    host_sha256_step(f, g, h, a, b, c, d, e, tail_words[11], kHostSha256[11]);
    host_sha256_step(e, f, g, h, a, b, c, d, tail_words[12], kHostSha256[12]);

    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
    out[4] = e;
    out[5] = f;
    out[6] = g;
    out[7] = h;
}
#endif

CudaWork make_cuda_work(const std::vector<byte>& block_bytes, uint32_t bits)
{
    CudaWork work{};
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    host_sha256_transform(state, block_bytes.data());
    for (int i = 0; i < 8; ++i) {
        work.midstate[i] = state[i];
    }
    for (int i = 0; i < 13; ++i) {
        work.tail_words[i] = host_load_be32(block_bytes.data() + 64 + i * 4);
    }
#if GEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS
    host_precompute_tail_round13(work.midstate, work.tail_words, work.tail_state_after_13);
#endif

    unsigned char target_be[32]{};
    target_to_be32(bits_to_target(bits), target_be);
    for (int i = 0; i < 8; ++i) {
        work.target_words[i] = host_load_be32(target_be + i * 4);
    }
    return work;
}

} // namespace

std::vector<DeviceInfo> list_cuda_devices()
{
#ifdef GEOCOIN_GPU_ENABLE_HIP
    if (!hip_runtime_available()) {
        return {};
    }
#endif
    int count = 0;
    const cudaError_t rc = cudaGetDeviceCount(&count);
    if (rc != cudaSuccess) {
        return {};
    }

    std::vector<DeviceInfo> devices;
    devices.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, i) != cudaSuccess) {
            continue;
        }
        DeviceInfo info;
        info.id = i;
#ifdef GEOCOIN_GPU_ENABLE_HIP
        info.backend = Backend::Hip;
        info.vendor = "AMD";
#else
        info.backend = Backend::Cuda;
        info.vendor = "NVIDIA";
#endif
        info.name = prop.name;
        std::ostringstream detail;
#ifdef GEOCOIN_GPU_ENABLE_HIP
        if (prop.gcnArchName[0] != '\0') {
            detail << prop.gcnArchName << ", ";
        } else {
            detail << "HIP, ";
        }
#else
        detail << "sm_" << prop.major << prop.minor
               << ", ";
#endif
        detail << (static_cast<unsigned long long>(prop.totalGlobalMem) / (1024ull * 1024ull)) << " MiB";
        info.detail = detail.str();
        info.available = true;
        devices.push_back(std::move(info));
    }
    return devices;
}

MiningResult mine_cuda(const MiningJob& job, const std::vector<int>& device_ids)
{
    MiningResult final_result;
    final_result.backend = GEOCOIN_GPU_BACKEND_NAME;
#ifdef GEOCOIN_GPU_ENABLE_HIP
    if (!hip_runtime_available()) {
#ifdef _WIN32
        final_result.error = "AMD HIP runtime amdhip64_7.dll or amdhip64_6.dll is not available";
#else
        final_result.error = "AMD HIP runtime is not available or WSL ROCDXG is not configured";
#endif
        return final_result;
    }
#endif

    if (job.block_bytes.size() < kHeaderSize) {
        final_result.error = "block template is shorter than the 124-byte GeoCoin header";
        return final_result;
    }

    std::vector<int> selected = device_ids;
    if (selected.empty()) {
        for (const auto& device : list_cuda_devices()) {
            selected.push_back(device.id);
        }
    }
    if (selected.empty()) {
        final_result.error = std::string("no ") + GEOCOIN_GPU_BACKEND_NAME + " devices available";
        return final_result;
    }
    const auto available_devices = list_cuda_devices();
    for (int selected_id : selected) {
        for (const auto& device : available_devices) {
            if (device.id == selected_id) {
                final_result.devices.push_back(device);
                break;
            }
        }
    }
    final_result.device_hashes.assign(final_result.devices.size(), 0);

    const uint64_t requested_per_launch = std::max<uint64_t>(1024, job.intensity);
    const uint64_t per_launch = job.max_launch_attempts > 0
        ? std::max<uint64_t>(1024, std::min<uint64_t>(requested_per_launch, job.max_launch_attempts))
        : requested_per_launch;
    const uint64_t nonce_stride = static_cast<uint64_t>(selected.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(job.timeout_sec);
    const CudaWork host_work = make_cuda_work(job.block_bytes, job.bits);

    std::atomic<bool> found{false};
    std::atomic<uint64_t> total_hashes{0};
    std::mutex result_mutex;
    std::mutex progress_mutex;
    auto next_progress = std::chrono::steady_clock::now();
    const auto progress_interval = std::chrono::milliseconds(
        std::max<uint32_t>(250, job.status_interval_ms));
    std::vector<uint64_t> live_device_hashes(final_result.devices.size(), 0);
    std::vector<std::thread> workers;

    for (size_t lane = 0; lane < selected.size(); ++lane) {
        workers.emplace_back([&, lane]() {
            const int device_id = selected[lane];
            uint64_t lane_hashes = 0;
            try {
                check_cuda(cudaSetDevice(device_id), "cudaSetDevice");
                cudaDeviceProp prop{};
                check_cuda(cudaGetDeviceProperties(&prop, device_id), "cudaGetDeviceProperties");
#ifdef GEOCOIN_GPU_ENABLE_HIP
                check_cuda(cudaFuncSetCacheConfig(reinterpret_cast<const void*>(geocoin_mine_kernel_fast),
                                                  cudaFuncCachePreferL1),
                           "hipFuncSetCacheConfig(geocoin_mine_kernel_fast)");
#else
                check_cuda(cudaFuncSetCacheConfig(geocoin_mine_kernel_fast, cudaFuncCachePreferL1),
                           "cudaFuncSetCacheConfig(geocoin_mine_kernel_fast)");
#endif
#if !GEOCOIN_CUDA_CONSTANT_WORK
                CudaWork* d_work = nullptr;
#endif
                unsigned char* d_hash = nullptr;
                int* d_found = nullptr;
                uint64_t* d_nonce = nullptr;
#if !GEOCOIN_CUDA_CONSTANT_WORK
                check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_work), sizeof(CudaWork)), "cudaMalloc(work)");
#endif
                check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_hash), 32), "cudaMalloc(hash)");
                check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_found), sizeof(int)), "cudaMalloc(found)");
                check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_nonce), sizeof(uint64_t)), "cudaMalloc(nonce)");
#if GEOCOIN_CUDA_CONSTANT_WORK
                check_cuda(cudaMemcpyToSymbol(kCudaWork, &host_work, sizeof(CudaWork)), "cudaMemcpyToSymbol(work)");
#else
                check_cuda(cudaMemcpy(d_work, &host_work, sizeof(CudaWork), cudaMemcpyHostToDevice), "cudaMemcpy(work)");
#endif
                int zero = 0;
                uint64_t zero_nonce = 0;
                check_cuda(cudaMemcpy(d_found, &zero, sizeof(int), cudaMemcpyHostToDevice), "cudaMemcpy(found=0)");
                check_cuda(cudaMemcpy(d_nonce, &zero_nonce, sizeof(uint64_t), cudaMemcpyHostToDevice), "cudaMemcpy(nonce=0)");

                const int threads = GEOCOIN_CUDA_THREADS_PER_BLOCK;
                const uint64_t attempts_per_lane = std::max<uint64_t>(threads, per_launch);
                const uint64_t attempts_per_block =
                    static_cast<uint64_t>(threads) * static_cast<uint64_t>(GEOCOIN_CUDA_NONCES_PER_THREAD);
#if GEOCOIN_CUDA_PERSISTENT_GRID
                const uint64_t resident_blocks = std::max<uint64_t>(
                    1,
                    static_cast<uint64_t>(std::max(1, prop.multiProcessorCount)) *
                        static_cast<uint64_t>(std::max(1, GEOCOIN_CUDA_PERSISTENT_BLOCKS_PER_SM)));
                const uint64_t launch_attempts =
                    ((attempts_per_lane + attempts_per_block - 1) / attempts_per_block) * attempts_per_block;
                const uint64_t blocks = resident_blocks;
#else
                const uint64_t blocks = (attempts_per_lane + attempts_per_block - 1) / attempts_per_block;
                const uint64_t launch_attempts = blocks * attempts_per_block;
#endif
                uint64_t chunk = 0;

                while (!found.load(std::memory_order_relaxed) &&
                       std::chrono::steady_clock::now() < deadline) {
                    const uint64_t base_nonce = job.start_nonce + static_cast<uint64_t>(lane) +
                        chunk++ * launch_attempts * nonce_stride;
                    geocoin_mine_kernel_fast<<<static_cast<unsigned int>(blocks), threads>>>(
#if !GEOCOIN_CUDA_CONSTANT_WORK
                        d_work,
#endif
                        base_nonce,
                        nonce_stride,
                        launch_attempts,
                        d_found,
                        d_nonce,
                        d_hash);
                    check_cuda(cudaGetLastError(), "geocoin_mine_kernel_fast");
                    lane_hashes += launch_attempts;
                    const uint64_t total_after =
                        total_hashes.fetch_add(launch_attempts, std::memory_order_relaxed) + launch_attempts;

                    if (job.progress_callback) {
                        const auto now = std::chrono::steady_clock::now();
                        std::lock_guard<std::mutex> progress_lock(progress_mutex);
                        if (lane < live_device_hashes.size()) {
                            live_device_hashes[lane] = lane_hashes;
                        }
                        if (now >= next_progress) {
                            next_progress = now + progress_interval;
                            job.progress_callback(GEOCOIN_GPU_BACKEND_NAME,
                                                  total_after,
                                                  final_result.devices,
                                                  live_device_hashes);
                        }
                    }

                    int host_found = 0;
                    check_cuda(cudaMemcpy(&host_found, d_found, sizeof(int), cudaMemcpyDeviceToHost), "cudaMemcpy(found)");
                    if (host_found != 0) {
                        bool expected = false;
                        if (found.compare_exchange_strong(expected, true)) {
                            std::lock_guard<std::mutex> lock(result_mutex);
                            final_result.found = true;
                            final_result.device_id = device_id;
                            check_cuda(cudaMemcpy(&final_result.nonce, d_nonce, sizeof(uint64_t), cudaMemcpyDeviceToHost), "cudaMemcpy(nonce)");
                            check_cuda(cudaMemcpy(final_result.hash.data(), d_hash, 32, cudaMemcpyDeviceToHost), "cudaMemcpy(hash)");
                        }
                        break;
                    }
                }

#if !GEOCOIN_CUDA_CONSTANT_WORK
                cudaFree(d_work);
#endif
                cudaFree(d_hash);
                cudaFree(d_found);
                cudaFree(d_nonce);
            } catch (const std::exception& e) {
                bool expected = false;
                if (found.compare_exchange_strong(expected, true)) {
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
    if (!final_result.found && final_result.error.empty()) {
        final_result.error = std::string(GEOCOIN_GPU_BACKEND_NAME) + " mining timeout after " + std::to_string(job.timeout_sec) +
            "s, hashes=" + std::to_string(final_result.hashes);
    }
    return final_result;
}

} // namespace geocoin::tools::gpu_miner

#endif // defined(GEOCOIN_GPU_ENABLE_CUDA) || defined(GEOCOIN_GPU_ENABLE_HIP)
