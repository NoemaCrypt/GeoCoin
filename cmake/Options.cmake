if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# The full loadnet harness is intentionally a separate Rust program. It is not
# part of the default C++ build, but can be built reproducibly through CMake.
find_program(GEOCOIN_CARGO_EXECUTABLE cargo)
if(GEOCOIN_CARGO_EXECUTABLE)
    add_custom_target(geocoin-loadnet-stress
        COMMAND "${GEOCOIN_CARGO_EXECUTABLE}" build
                --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/tools/loadnet-stress/Cargo.toml"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building the GeoCoin Rust loadnet stress harness"
        VERBATIM
    )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    set(THREADS_PREFER_PTHREAD_FLAG OFF)
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
    )
endif()

# Тестовые бинарники (и прочие эксперименты) должны собираться только по явному флагу.
option(BUILD_TESTS "Build GeoCoin tests/experimental binaries" OFF)
option(BUILD_FUZZERS "Build coverage-guided libFuzzer targets (Clang with ASan/UBSan)" OFF)
option(GEOCOIN_ENABLE_RPC_INTEGRATION_TESTS "Enable networked JSON-RPC integration tests" OFF)
option(GEOCOIN_ENABLE_CPU_MULTISIG_LOCALNET_TESTS "Enable multisig localnet smoke tests with the CPU RPC miner" OFF)
option(GEOCOIN_ENABLE_CPU_NFT_LOCALNET_TESTS "Enable NFT localnet smoke tests with the CPU RPC miner" OFF)
option(GEOCOIN_ENABLE_CPU_SIDECHAIN_LOCALNET_TESTS "Enable exhaustive sidechain-v3 localnet tests with the CPU RPC miner" OFF)
option(GEOCOIN_ENABLE_GPU_LOCALNET_TESTS "Enable hardware-dependent GPU localnet smoke tests" OFF)
option(BUILD_TOOLS "Build GeoCoin CLI/tools binaries" ON)
option(BUILD_NODE_CLI "Build GeoCoin node CLI" ON)
option(BUILD_MINER_CLI "Build GeoCoin miner CLI" ON)
option(BUILD_GPU_MINER "Build GeoCoin GPU miner CLI" OFF)
option(BUILD_GENESIS_CLI "Build developer-only GeoCoin genesis miner CLI" OFF)
option(BUILD_MULTISIG_CLI "Build GeoCoin multisig CLI" OFF)
option(BUILD_QT_GUI "Build Qt GUI" ON)
option(GEOCOIN_WINDOWS_STATIC_RELEASE "Build Windows release binaries with static third-party/runtime linkage" OFF)
option(GEOCOIN_HARDEN_BUILD "Enable supported compiler/linker exploit mitigations" ON)
set(GEOCOIN_SANITIZER "none" CACHE STRING "Runtime sanitizer: none, address-undefined, or thread")
set_property(CACHE GEOCOIN_SANITIZER PROPERTY STRINGS none address-undefined thread)

if(BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
       NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR
       NOT GEOCOIN_SANITIZER STREQUAL "address-undefined")
        message(FATAL_ERROR "BUILD_FUZZERS requires Clang C/C++ and GEOCOIN_SANITIZER=address-undefined")
    endif()
    # Instrument parser implementations too, not just LLVMFuzzerTestOneInput.
    add_compile_options(-fsanitize=fuzzer-no-link)
    add_link_options(-fsanitize=fuzzer-no-link)
endif()

if(GEOCOIN_HARDEN_BUILD AND UNIX AND NOT APPLE AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Apply mitigations to every node/GUI/tool executable. PIE is enabled via
    # CMP0083; static libraries are compiled as PIC so they remain linkable.
    include(CheckPIESupported)
    check_pie_supported(OUTPUT_VARIABLE GEOCOIN_PIE_ERROR LANGUAGES C CXX)
    if(CMAKE_C_LINK_PIE_SUPPORTED AND CMAKE_CXX_LINK_PIE_SUPPORTED)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    else()
        message(WARNING "PIE is unavailable: ${GEOCOIN_PIE_ERROR}")
    endif()

    add_compile_options(
        $<$<COMPILE_LANGUAGE:C>:-fstack-protector-strong>
        $<$<COMPILE_LANGUAGE:CXX>:-fstack-protector-strong>
    )
    add_compile_definitions(
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:_FORTIFY_SOURCE=2>
    )
    add_link_options(
        -Wl,-z,relro
        -Wl,-z,now
        -Wl,-z,noexecstack
    )
endif()

if(NOT GEOCOIN_SANITIZER STREQUAL "none")
    if(MSVC)
        message(FATAL_ERROR "GEOCOIN_SANITIZER is supported only by GCC/Clang builds")
    elseif(GEOCOIN_SANITIZER STREQUAL "address-undefined")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    elseif(GEOCOIN_SANITIZER STREQUAL "thread")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    else()
        message(FATAL_ERROR "Invalid GEOCOIN_SANITIZER=${GEOCOIN_SANITIZER}")
    endif()
endif()
set(GEOCOIN_ENABLE_ROCKSDB "ON" CACHE STRING "RocksDB storage backend: ON is mandatory")
set_property(CACHE GEOCOIN_ENABLE_ROCKSDB PROPERTY STRINGS ON)
string(TOUPPER "${GEOCOIN_ENABLE_ROCKSDB}" _geocoin_enable_rocksdb)
if(NOT _geocoin_enable_rocksdb STREQUAL "ON")
    message(FATAL_ERROR "GEOCOIN_ENABLE_ROCKSDB=ON is mandatory; alternate storage backends are unsupported")
endif()
set(GEOCOIN_ENABLE_CUDA "AUTO" CACHE STRING "GPU miner CUDA backend: AUTO, ON, or OFF")
set_property(CACHE GEOCOIN_ENABLE_CUDA PROPERTY STRINGS AUTO ON OFF)
set(GEOCOIN_ENABLE_HIP "AUTO" CACHE STRING "GPU miner AMD HIP backend: AUTO, ON, or OFF")
set_property(CACHE GEOCOIN_ENABLE_HIP PROPERTY STRINGS AUTO ON OFF)
set(GEOCOIN_ENABLE_OPENCL "AUTO" CACHE STRING "GPU miner OpenCL backend: AUTO, ON, or OFF")
set_property(CACHE GEOCOIN_ENABLE_OPENCL PROPERTY STRINGS AUTO ON OFF)
set(GEOCOIN_CUDA_THREADS_PER_BLOCK "512" CACHE STRING "CUDA miner threads per block")
set(GEOCOIN_CUDA_NONCES_PER_THREAD "2" CACHE STRING "CUDA miner nonce attempts per CUDA thread")
set(GEOCOIN_CUDA_MIN_BLOCKS_PER_SM "2" CACHE STRING "CUDA miner launch_bounds minimum blocks per SM")
set(GEOCOIN_CUDA_PERSISTENT_GRID "OFF" CACHE STRING "CUDA miner persistent grid scheduler: ON or OFF")
set(GEOCOIN_CUDA_PERSISTENT_BLOCKS_PER_SM "2" CACHE STRING "CUDA miner persistent grid resident blocks per SM")
set(GEOCOIN_CUDA_CONSTANT_WORK "ON" CACHE STRING "CUDA miner work descriptor storage: ON or OFF")
set(GEOCOIN_CUDA_IMMEDIATE_K "ON" CACHE STRING "CUDA miner SHA256 round constants: ON for immediates, OFF for constant memory")
set(GEOCOIN_CUDA_MAXRREGCOUNT "" CACHE STRING "Optional CUDA miner max register count")
set(GEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS "ON" CACHE STRING "Precompute nonce-independent SHA256 tail rounds: ON or OFF")
set(GEOCOIN_CUDA_SPECIALIZE_FINAL_LEN_BLOCK "OFF" CACHE STRING "Use specialized final length SHA256 block: ON or OFF")
set(GEOCOIN_CUDA_EXTRA_NVCC_FLAGS "" CACHE STRING "Optional semicolon-separated extra nvcc flags for CUDA miner tuning")
set(GEOCOIN_HIP_ARCHITECTURES "gfx1030;gfx1031;gfx1032;gfx1034;gfx1035;gfx1100;gfx1101;gfx1102;gfx1103" CACHE STRING "HIP miner offload architectures, e.g. gfx1100;gfx1101 or native")
set(GEOCOIN_HIP_THREADS_PER_BLOCK "256" CACHE STRING "HIP miner threads per block")
set(GEOCOIN_HIP_NONCES_PER_THREAD "2" CACHE STRING "HIP miner nonce attempts per HIP thread")
set(GEOCOIN_HIP_MIN_BLOCKS_PER_SM "2" CACHE STRING "HIP miner launch_bounds minimum blocks per compute unit")
set(GEOCOIN_HIP_PERSISTENT_GRID "OFF" CACHE STRING "HIP miner persistent grid scheduler: ON or OFF")
set(GEOCOIN_HIP_PERSISTENT_BLOCKS_PER_SM "2" CACHE STRING "HIP miner persistent grid resident blocks per compute unit")
set(GEOCOIN_HIP_EXTRA_HIPCC_FLAGS "" CACHE STRING "Optional semicolon-separated extra hipcc flags for HIP miner tuning")

if(MSVC OR (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    if(GEOCOIN_WINDOWS_STATIC_RELEASE OR VCPKG_TARGET_TRIPLET MATCHES "static")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()

set(GEOCOIN_MSVC_STATIC_RUNTIME_LIB "")
if(WIN32 AND GEOCOIN_WINDOWS_STATIC_RELEASE AND MSVC)
    set(_geocoin_msvc_runtime_candidates)
    foreach(_geocoin_msvc_root
            "$ENV{VCToolsInstallDir}"
            "${CMAKE_GENERATOR_INSTANCE}/VC/Tools/MSVC"
            "$ENV{VSINSTALLDIR}/VC/Tools/MSVC"
            "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC"
            "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC"
            "D:/VS/VC/Tools/MSVC"
            "D:/Visual Studio/Community/VC/Tools/MSVC")
        if(_geocoin_msvc_root)
            file(TO_CMAKE_PATH "${_geocoin_msvc_root}" _geocoin_msvc_root)
            if(EXISTS "${_geocoin_msvc_root}/lib/x64/libcpmt.lib")
                list(APPEND _geocoin_msvc_runtime_candidates "${_geocoin_msvc_root}/lib/x64/libcpmt.lib")
            else()
                file(GLOB _geocoin_msvc_globbed_libcpmt
                    "${_geocoin_msvc_root}/*/lib/x64/libcpmt.lib")
                list(APPEND _geocoin_msvc_runtime_candidates ${_geocoin_msvc_globbed_libcpmt})
            endif()
        endif()
    endforeach()

    if(_geocoin_msvc_runtime_candidates)
        list(REMOVE_DUPLICATES _geocoin_msvc_runtime_candidates)
        set(_geocoin_msvc_best_version "0")
        foreach(_geocoin_msvc_candidate ${_geocoin_msvc_runtime_candidates})
            get_filename_component(_geocoin_msvc_arch_dir "${_geocoin_msvc_candidate}" DIRECTORY)
            get_filename_component(_geocoin_msvc_lib_dir "${_geocoin_msvc_arch_dir}" DIRECTORY)
            get_filename_component(_geocoin_msvc_version_dir "${_geocoin_msvc_lib_dir}" DIRECTORY)
            get_filename_component(_geocoin_msvc_version "${_geocoin_msvc_version_dir}" NAME)
            if(_geocoin_msvc_version VERSION_GREATER _geocoin_msvc_best_version)
                set(_geocoin_msvc_best_version "${_geocoin_msvc_version}")
                set(GEOCOIN_MSVC_STATIC_RUNTIME_LIB "${_geocoin_msvc_candidate}")
            endif()
        endforeach()
    endif()
endif()

# Настройка RPATH для работы и в build/ и в portable/
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_RPATH "")
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Устанавливаем выходные директории для исполняемых файлов
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR})
