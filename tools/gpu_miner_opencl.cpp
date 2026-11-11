#include "gpu_miner_common.hpp"

#ifdef GEOCOIN_GPU_ENABLE_OPENCL

#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace geocoin::tools::gpu_miner {
namespace {

constexpr size_t kHeaderSize = 124;

#ifdef _WIN32
struct OpenClApi {
    using GetPlatformIDsFn = cl_int (CL_API_CALL *)(cl_uint, cl_platform_id*, cl_uint*);
    using GetDeviceIDsFn = cl_int (CL_API_CALL *)(cl_platform_id, cl_device_type, cl_uint, cl_device_id*, cl_uint*);
    using GetDeviceInfoFn = cl_int (CL_API_CALL *)(cl_device_id, cl_device_info, size_t, void*, size_t*);
    using CreateContextFn = cl_context (CL_API_CALL *)(const cl_context_properties*, cl_uint, const cl_device_id*,
        void (CL_CALLBACK *)(const char*, const void*, size_t, void*), void*, cl_int*);
    using CreateCommandQueueFn = cl_command_queue (CL_API_CALL *)(cl_context, cl_device_id, cl_command_queue_properties, cl_int*);
    using CreateProgramWithSourceFn = cl_program (CL_API_CALL *)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
    using BuildProgramFn = cl_int (CL_API_CALL *)(cl_program, cl_uint, const cl_device_id*, const char*,
        void (CL_CALLBACK *)(cl_program, void*), void*);
    using GetProgramBuildInfoFn = cl_int (CL_API_CALL *)(cl_program, cl_device_id, cl_program_build_info, size_t, void*, size_t*);
    using CreateKernelFn = cl_kernel (CL_API_CALL *)(cl_program, const char*, cl_int*);
    using CreateBufferFn = cl_mem (CL_API_CALL *)(cl_context, cl_mem_flags, size_t, void*, cl_int*);
    using EnqueueWriteBufferFn = cl_int (CL_API_CALL *)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
        const void*, cl_uint, const cl_event*, cl_event*);
    using SetKernelArgFn = cl_int (CL_API_CALL *)(cl_kernel, cl_uint, size_t, const void*);
    using EnqueueNDRangeKernelFn = cl_int (CL_API_CALL *)(cl_command_queue, cl_kernel, cl_uint, const size_t*,
        const size_t*, const size_t*, cl_uint, const cl_event*, cl_event*);
    using FinishFn = cl_int (CL_API_CALL *)(cl_command_queue);
    using EnqueueReadBufferFn = cl_int (CL_API_CALL *)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
        void*, cl_uint, const cl_event*, cl_event*);
    using ReleaseMemObjectFn = cl_int (CL_API_CALL *)(cl_mem);
    using ReleaseKernelFn = cl_int (CL_API_CALL *)(cl_kernel);
    using ReleaseProgramFn = cl_int (CL_API_CALL *)(cl_program);
    using ReleaseCommandQueueFn = cl_int (CL_API_CALL *)(cl_command_queue);
    using ReleaseContextFn = cl_int (CL_API_CALL *)(cl_context);

    HMODULE module = nullptr;
    bool attempted = false;
    std::string error;

    GetPlatformIDsFn p_clGetPlatformIDs = nullptr;
    GetDeviceIDsFn p_clGetDeviceIDs = nullptr;
    GetDeviceInfoFn p_clGetDeviceInfo = nullptr;
    CreateContextFn p_clCreateContext = nullptr;
    CreateCommandQueueFn p_clCreateCommandQueue = nullptr;
    CreateProgramWithSourceFn p_clCreateProgramWithSource = nullptr;
    BuildProgramFn p_clBuildProgram = nullptr;
    GetProgramBuildInfoFn p_clGetProgramBuildInfo = nullptr;
    CreateKernelFn p_clCreateKernel = nullptr;
    CreateBufferFn p_clCreateBuffer = nullptr;
    EnqueueWriteBufferFn p_clEnqueueWriteBuffer = nullptr;
    SetKernelArgFn p_clSetKernelArg = nullptr;
    EnqueueNDRangeKernelFn p_clEnqueueNDRangeKernel = nullptr;
    FinishFn p_clFinish = nullptr;
    EnqueueReadBufferFn p_clEnqueueReadBuffer = nullptr;
    ReleaseMemObjectFn p_clReleaseMemObject = nullptr;
    ReleaseKernelFn p_clReleaseKernel = nullptr;
    ReleaseProgramFn p_clReleaseProgram = nullptr;
    ReleaseCommandQueueFn p_clReleaseCommandQueue = nullptr;
    ReleaseContextFn p_clReleaseContext = nullptr;

    static OpenClApi& instance()
    {
        static OpenClApi api;
        return api;
    }

    bool load()
    {
        if (attempted) {
            return module != nullptr;
        }
        attempted = true;

        wchar_t exe_path[MAX_PATH]{};
        const DWORD size = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        if (size > 0 && size < MAX_PATH) {
            std::wstring local_path(exe_path, exe_path + size);
            const auto slash = local_path.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                local_path.resize(slash + 1);
                local_path += L"OpenCL.dll";
                module = LoadLibraryW(local_path.c_str());
            }
        }
        if (!module) {
            module = LoadLibraryW(L"OpenCL.dll");
        }
        if (!module) {
            error = "OpenCL loader OpenCL.dll was not found. Install a GPU driver with OpenCL support or use the bundled portable runtime.";
            return false;
        }

        return load_symbol(p_clGetPlatformIDs, "clGetPlatformIDs") &&
               load_symbol(p_clGetDeviceIDs, "clGetDeviceIDs") &&
               load_symbol(p_clGetDeviceInfo, "clGetDeviceInfo") &&
               load_symbol(p_clCreateContext, "clCreateContext") &&
               load_symbol(p_clCreateCommandQueue, "clCreateCommandQueue") &&
               load_symbol(p_clCreateProgramWithSource, "clCreateProgramWithSource") &&
               load_symbol(p_clBuildProgram, "clBuildProgram") &&
               load_symbol(p_clGetProgramBuildInfo, "clGetProgramBuildInfo") &&
               load_symbol(p_clCreateKernel, "clCreateKernel") &&
               load_symbol(p_clCreateBuffer, "clCreateBuffer") &&
               load_symbol(p_clEnqueueWriteBuffer, "clEnqueueWriteBuffer") &&
               load_symbol(p_clSetKernelArg, "clSetKernelArg") &&
               load_symbol(p_clEnqueueNDRangeKernel, "clEnqueueNDRangeKernel") &&
               load_symbol(p_clFinish, "clFinish") &&
               load_symbol(p_clEnqueueReadBuffer, "clEnqueueReadBuffer") &&
               load_symbol(p_clReleaseMemObject, "clReleaseMemObject") &&
               load_symbol(p_clReleaseKernel, "clReleaseKernel") &&
               load_symbol(p_clReleaseProgram, "clReleaseProgram") &&
               load_symbol(p_clReleaseCommandQueue, "clReleaseCommandQueue") &&
               load_symbol(p_clReleaseContext, "clReleaseContext");
    }

    template <typename T>
    bool load_symbol(T& target, const char* name)
    {
        const FARPROC symbol = GetProcAddress(module, name);
        if (!symbol) {
            error = std::string("OpenCL loader is missing required symbol: ") + name;
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        target = reinterpret_cast<T>(symbol);
        return true;
    }
};

bool ensure_opencl_available(std::string* error = nullptr)
{
    auto& api = OpenClApi::instance();
    const bool ok = api.load();
    if (!ok && error) {
        *error = api.error;
    }
    return ok;
}

#define clGetPlatformIDs OpenClApi::instance().p_clGetPlatformIDs
#define clGetDeviceIDs OpenClApi::instance().p_clGetDeviceIDs
#define clGetDeviceInfo OpenClApi::instance().p_clGetDeviceInfo
#define clCreateContext OpenClApi::instance().p_clCreateContext
#define clCreateCommandQueue OpenClApi::instance().p_clCreateCommandQueue
#define clCreateProgramWithSource OpenClApi::instance().p_clCreateProgramWithSource
#define clBuildProgram OpenClApi::instance().p_clBuildProgram
#define clGetProgramBuildInfo OpenClApi::instance().p_clGetProgramBuildInfo
#define clCreateKernel OpenClApi::instance().p_clCreateKernel
#define clCreateBuffer OpenClApi::instance().p_clCreateBuffer
#define clEnqueueWriteBuffer OpenClApi::instance().p_clEnqueueWriteBuffer
#define clSetKernelArg OpenClApi::instance().p_clSetKernelArg
#define clEnqueueNDRangeKernel OpenClApi::instance().p_clEnqueueNDRangeKernel
#define clFinish OpenClApi::instance().p_clFinish
#define clEnqueueReadBuffer OpenClApi::instance().p_clEnqueueReadBuffer
#define clReleaseMemObject OpenClApi::instance().p_clReleaseMemObject
#define clReleaseKernel OpenClApi::instance().p_clReleaseKernel
#define clReleaseProgram OpenClApi::instance().p_clReleaseProgram
#define clReleaseCommandQueue OpenClApi::instance().p_clReleaseCommandQueue
#define clReleaseContext OpenClApi::instance().p_clReleaseContext
#else
bool ensure_opencl_available(std::string* = nullptr)
{
    return true;
}
#endif

const char* kOpenClKernel = R"CLC(
#define ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
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

__constant uint K[64] = {
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

static void store_be32(__global uchar* p, uint v) {
    p[0] = (uchar)((v >> 24) & 0xffU);
    p[1] = (uchar)((v >> 16) & 0xffU);
    p[2] = (uchar)((v >> 8) & 0xffU);
    p[3] = (uchar)(v & 0xffU);
}

static void sha256_compress_words(
    uint state[8],
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

static void sha256_tail_from_round13(__global const uint* work, uint state[8], uint nonce_lo_be, uint nonce_hi_be)
{
    uint a = work[21], b = work[22], c = work[23], d = work[24];
    uint e = work[25], f = work[26], g = work[27], h = work[28];
    uint w0 = work[8], w1 = work[9], w2 = work[10], w3 = work[11];
    uint w4 = work[12], w5 = work[13], w6 = work[14], w7 = work[15];
    uint w8 = work[16], w9 = work[17], w10 = work[18], w11 = work[19];
    uint w12 = work[20], w13 = nonce_lo_be, w14 = nonce_hi_be, w15 = 0x80000000U;

    SHA256_STEP(d,e,f,g,h,a,b,c,w13,K[13]);
    SHA256_STEP(c,d,e,f,g,h,a,b,w14,K[14]);
    SHA256_STEP(b,c,d,e,f,g,h,a,w15,K[15]);
    ROUND_EXPAND();
    ROUND_STEP(16);
    ROUND_EXPAND();
    ROUND_STEP(32);
    ROUND_EXPAND();
    ROUND_STEP(48);

    state[0] = work[0] + a; state[1] = work[1] + b; state[2] = work[2] + c; state[3] = work[3] + d;
    state[4] = work[4] + e; state[5] = work[5] + f; state[6] = work[6] + g; state[7] = work[7] + h;
}

static uint bswap32(uint x) {
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

static int hash_words_meet_target(uint hash[8], __global const uint* target) {
    for (int i = 0; i < 8; ++i) {
        if (hash[i] < target[i]) return 1;
        if (hash[i] > target[i]) return 0;
    }
    return 1;
}

__kernel void geocoin_mine_fast(
    __global const uint* work,
    ulong base_nonce,
    ulong nonce_stride,
    ulong attempts,
    __global int* found,
    __global ulong* found_nonce,
    __global uchar* found_hash)
{
    const ulong tid = get_global_id(0);
    if (tid >= attempts || atomic_add(found, 0) != 0) return;

    const ulong nonce = base_nonce + tid * nonce_stride;
    const uint nonce_lo_be = bswap32((uint)nonce);
    const uint nonce_hi_be = bswap32((uint)(nonce >> 32));

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
        if (atomic_cmpxchg(found, 0, 1) == 0) {
            *found_nonce = nonce;
            for (int i = 0; i < 8; ++i) store_be32(found_hash + i * 4, hash[i]);
        }
    }
}
)CLC";

struct OpenClDevice {
    int id = -1;
    cl_platform_id platform = nullptr;
    cl_device_id device = nullptr;
    std::string name;
    std::string vendor;
};

std::string cl_device_string(cl_device_id device, cl_device_info info)
{
    size_t size = 0;
    clGetDeviceInfo(device, info, 0, nullptr, &size);
    std::string value(size, '\0');
    if (size > 0) {
        clGetDeviceInfo(device, info, size, value.data(), nullptr);
        if (!value.empty() && value.back() == '\0') {
            value.pop_back();
        }
    }
    return value;
}

std::vector<OpenClDevice> enumerate_opencl_devices()
{
    if (!ensure_opencl_available()) {
        return {};
    }

    cl_uint platform_count = 0;
    if (clGetPlatformIDs(0, nullptr, &platform_count) != CL_SUCCESS || platform_count == 0) {
        return {};
    }
    std::vector<cl_platform_id> platforms(platform_count);
    if (clGetPlatformIDs(platform_count, platforms.data(), nullptr) != CL_SUCCESS) {
        return {};
    }

    std::vector<OpenClDevice> out;
    int id = 0;
    for (cl_platform_id platform : platforms) {
        cl_uint device_count = 0;
        if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &device_count) != CL_SUCCESS || device_count == 0) {
            continue;
        }
        std::vector<cl_device_id> devices(device_count);
        if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, device_count, devices.data(), nullptr) != CL_SUCCESS) {
            continue;
        }
        for (cl_device_id device : devices) {
            OpenClDevice d;
            d.id = id++;
            d.platform = platform;
            d.device = device;
            d.name = cl_device_string(device, CL_DEVICE_NAME);
            d.vendor = cl_device_string(device, CL_DEVICE_VENDOR);
            out.push_back(std::move(d));
        }
    }
    return out;
}

void check_cl(cl_int error, const char* operation)
{
    if (error != CL_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with OpenCL error " + std::to_string(error));
    }
}

geocoin::uint256 bits_to_target(uint32_t bits)
{
    const uint8_t exponent = static_cast<uint8_t>(bits >> 24);
    uint32_t mantissa = bits & 0x007fffff;
    const bool negative = (bits & 0x00800000) != 0;
    geocoin::uint256 target;
    target.data[0] = target.data[1] = target.data[2] = target.data[3] = 0;
    if (exponent <= 3) {
        mantissa >>= 8 * (3 - exponent);
        target.data[0] = mantissa;
    } else {
        const int shift = 8 * (exponent - 3);
        const int word_index = shift / 64;
        const int bit_shift = shift % 64;
        if (word_index < 4) target.data[word_index] |= static_cast<uint64_t>(mantissa) << bit_shift;
        if (bit_shift > 64 - 24 && word_index + 1 < 4) {
            target.data[word_index + 1] |= static_cast<uint64_t>(mantissa) >> (64 - bit_shift);
        }
    }
    if (negative) target.data[0] = target.data[1] = target.data[2] = target.data[3] = 0;
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

std::array<uint32_t, 37> make_opencl_work(const std::vector<byte>& block_bytes, uint32_t bits)
{
    std::array<uint32_t, 37> work{};
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    host_sha256_transform(state, block_bytes.data());
    for (int i = 0; i < 8; ++i) {
        work[static_cast<size_t>(i)] = state[i];
    }
    for (int i = 0; i < 13; ++i) {
        work[8u + static_cast<size_t>(i)] = host_load_be32(block_bytes.data() + 64 + i * 4);
    }
    host_precompute_tail_round13(work.data(), work.data() + 8u, work.data() + 21u);

    unsigned char target_be[32]{};
    target_to_be32(bits_to_target(bits), target_be);
    for (int i = 0; i < 8; ++i) {
        work[29u + static_cast<size_t>(i)] = host_load_be32(target_be + i * 4);
    }
    return work;
}

} // namespace

std::vector<DeviceInfo> list_opencl_devices()
{
    std::vector<DeviceInfo> out;
    for (const auto& d : enumerate_opencl_devices()) {
        DeviceInfo info;
        info.id = d.id;
        info.backend = Backend::OpenCl;
        info.name = d.name;
        info.vendor = d.vendor;
        info.detail = "OpenCL GPU";
        info.available = true;
        out.push_back(std::move(info));
    }
    return out;
}

MiningResult mine_opencl(const MiningJob& job, const std::vector<int>& device_ids)
{
    MiningResult final_result;
    final_result.backend = "opencl";
    std::string loader_error;
    if (!ensure_opencl_available(&loader_error)) {
        final_result.error = loader_error;
        return final_result;
    }

    if (job.block_bytes.size() < kHeaderSize) {
        final_result.error = "block template is shorter than the 124-byte GeoCoin header";
        return final_result;
    }

    auto all_devices = enumerate_opencl_devices();
    if (!device_ids.empty()) {
        all_devices.erase(std::remove_if(all_devices.begin(), all_devices.end(), [&](const OpenClDevice& d) {
            return std::find(device_ids.begin(), device_ids.end(), d.id) == device_ids.end();
        }), all_devices.end());
    }
    if (all_devices.empty()) {
        final_result.error = "no OpenCL GPU devices available";
        return final_result;
    }
    for (const auto& device : all_devices) {
        final_result.devices.push_back({
            device.id,
            Backend::OpenCl,
            device.name,
            device.vendor,
            "OpenCL GPU",
            true,
        });
    }
    final_result.device_hashes.assign(final_result.devices.size(), 0);

    const uint64_t requested_per_launch = std::max<uint64_t>(1024, job.intensity);
    const uint64_t per_launch = job.max_launch_attempts > 0
        ? std::max<uint64_t>(1024, std::min<uint64_t>(requested_per_launch, job.max_launch_attempts))
        : requested_per_launch;
    const uint64_t nonce_stride = static_cast<uint64_t>(all_devices.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(job.timeout_sec);
    const auto work = make_opencl_work(job.block_bytes, job.bits);

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
            const auto device = all_devices[lane];
            uint64_t lane_hashes = 0;
            try {
                cl_int rc = CL_SUCCESS;
                cl_context context = clCreateContext(nullptr, 1, &device.device, nullptr, nullptr, &rc);
                check_cl(rc, "clCreateContext");
                cl_command_queue queue = clCreateCommandQueue(context, device.device, 0, &rc);
                check_cl(rc, "clCreateCommandQueue");
                const char* source = kOpenClKernel;
                const size_t source_len = std::strlen(kOpenClKernel);
                cl_program program = clCreateProgramWithSource(context, 1, &source, &source_len, &rc);
                check_cl(rc, "clCreateProgramWithSource");
                rc = clBuildProgram(program, 1, &device.device, "", nullptr, nullptr);
                if (rc != CL_SUCCESS) {
                    size_t log_size = 0;
                    clGetProgramBuildInfo(program, device.device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
                    std::string log(log_size, '\0');
                    if (log_size > 0) {
                        clGetProgramBuildInfo(program, device.device, CL_PROGRAM_BUILD_LOG, log_size, log.data(), nullptr);
                    }
                    throw std::runtime_error("clBuildProgram failed: " + log);
                }
                cl_kernel kernel = clCreateKernel(program, "geocoin_mine_fast", &rc);
                check_cl(rc, "clCreateKernel");

                cl_mem d_work = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                    work.size() * sizeof(uint32_t), const_cast<uint32_t*>(work.data()), &rc);
                check_cl(rc, "clCreateBuffer(work)");
                cl_mem d_found = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(cl_int), nullptr, &rc);
                check_cl(rc, "clCreateBuffer(found)");
                cl_mem d_nonce = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(cl_ulong), nullptr, &rc);
                check_cl(rc, "clCreateBuffer(nonce)");
                cl_mem d_hash = clCreateBuffer(context, CL_MEM_READ_WRITE, 32, nullptr, &rc);
                check_cl(rc, "clCreateBuffer(hash)");

                const size_t local = 256;
                const size_t global = static_cast<size_t>(((per_launch + local - 1) / local) * local);
                uint64_t chunk = 0;
                cl_int zero = 0;
                cl_ulong zero_nonce = 0;
                check_cl(clEnqueueWriteBuffer(queue, d_found, CL_TRUE, 0, sizeof(zero), &zero, 0, nullptr, nullptr), "clEnqueueWriteBuffer(found)");
                check_cl(clEnqueueWriteBuffer(queue, d_nonce, CL_TRUE, 0, sizeof(zero_nonce), &zero_nonce, 0, nullptr, nullptr), "clEnqueueWriteBuffer(nonce)");

                while (!found.load(std::memory_order_relaxed) &&
                       std::chrono::steady_clock::now() < deadline) {
                    const cl_ulong base_nonce = static_cast<cl_ulong>(job.start_nonce + static_cast<uint64_t>(lane) +
                        chunk++ * per_launch * nonce_stride);
                    const cl_ulong stride = static_cast<cl_ulong>(nonce_stride);
                    const cl_ulong attempts = static_cast<cl_ulong>(per_launch);

                    check_cl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_work), "clSetKernelArg(work)");
                    check_cl(clSetKernelArg(kernel, 1, sizeof(base_nonce), &base_nonce), "clSetKernelArg(base_nonce)");
                    check_cl(clSetKernelArg(kernel, 2, sizeof(stride), &stride), "clSetKernelArg(stride)");
                    check_cl(clSetKernelArg(kernel, 3, sizeof(attempts), &attempts), "clSetKernelArg(attempts)");
                    check_cl(clSetKernelArg(kernel, 4, sizeof(cl_mem), &d_found), "clSetKernelArg(found)");
                    check_cl(clSetKernelArg(kernel, 5, sizeof(cl_mem), &d_nonce), "clSetKernelArg(nonce)");
                    check_cl(clSetKernelArg(kernel, 6, sizeof(cl_mem), &d_hash), "clSetKernelArg(hash)");
                    check_cl(clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global, &local, 0, nullptr, nullptr), "clEnqueueNDRangeKernel");
                    check_cl(clFinish(queue), "clFinish");
                    lane_hashes += per_launch;
                    const uint64_t total_after =
                        total_hashes.fetch_add(per_launch, std::memory_order_relaxed) + per_launch;

                    if (job.progress_callback) {
                        const auto now = std::chrono::steady_clock::now();
                        std::lock_guard<std::mutex> progress_lock(progress_mutex);
                        if (lane < live_device_hashes.size()) {
                            live_device_hashes[lane] = lane_hashes;
                        }
                        if (now >= next_progress) {
                            next_progress = now + progress_interval;
                            job.progress_callback("opencl",
                                                  total_after,
                                                  final_result.devices,
                                                  live_device_hashes);
                        }
                    }

                    cl_int host_found = 0;
                    check_cl(clEnqueueReadBuffer(queue, d_found, CL_TRUE, 0, sizeof(host_found), &host_found, 0, nullptr, nullptr), "clEnqueueReadBuffer(found)");
                    if (host_found != 0) {
                        bool expected = false;
                        if (found.compare_exchange_strong(expected, true)) {
                            std::lock_guard<std::mutex> lock(result_mutex);
                            final_result.found = true;
                            final_result.device_id = device.id;
                            check_cl(clEnqueueReadBuffer(queue, d_nonce, CL_TRUE, 0, sizeof(cl_ulong), &final_result.nonce, 0, nullptr, nullptr), "clEnqueueReadBuffer(nonce)");
                            check_cl(clEnqueueReadBuffer(queue, d_hash, CL_TRUE, 0, 32, final_result.hash.data(), 0, nullptr, nullptr), "clEnqueueReadBuffer(hash)");
                        }
                        break;
                    }
                }

                clReleaseMemObject(d_work);
                clReleaseMemObject(d_found);
                clReleaseMemObject(d_nonce);
                clReleaseMemObject(d_hash);
                clReleaseKernel(kernel);
                clReleaseProgram(program);
                clReleaseCommandQueue(queue);
                clReleaseContext(context);
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
        final_result.error = "OpenCL mining timeout after " + std::to_string(job.timeout_sec) +
            "s, hashes=" + std::to_string(final_result.hashes);
    }
    return final_result;
}

} // namespace geocoin::tools::gpu_miner

#endif // GEOCOIN_GPU_ENABLE_OPENCL
