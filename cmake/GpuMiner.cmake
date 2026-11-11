set(_geocoin_gpu_sources
    tools/gpu_miner.cpp
    tools/gpu_miner_cuda_stub.cpp
    tools/gpu_miner_hip_stub.cpp
    tools/gpu_miner_opencl_stub.cpp
)
set(_geocoin_gpu_libs geocoin_crypto)
set(_geocoin_gpu_defs "")
set(_geocoin_gpu_include_dirs "")
set(_geocoin_gpu_has_cuda OFF)
set(_geocoin_gpu_has_hip OFF)

string(TOUPPER "${GEOCOIN_ENABLE_CUDA}" _geocoin_enable_cuda)
if(NOT _geocoin_enable_cuda MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "GEOCOIN_ENABLE_CUDA must be AUTO, ON, or OFF")
endif()
if(NOT _geocoin_enable_cuda STREQUAL "OFF")
    if(UNIX AND NOT DEFINED CUDAToolkit_ROOT AND NOT DEFINED ENV{CUDAToolkit_ROOT} AND NOT DEFINED ENV{CUDA_PATH} AND NOT DEFINED ENV{CUDA_HOME})
        foreach(_geocoin_cuda_root_candidate
                "/usr/local/cuda-13.1"
                "/usr/local/cuda-12.4"
                "/usr/local/cuda-12.5"
                "/usr/local/cuda-12.6"
                "/usr/local/cuda-12.8"
                "/usr/local/cuda")
            if(EXISTS "${_geocoin_cuda_root_candidate}/bin/nvcc")
                set(CUDAToolkit_ROOT "${_geocoin_cuda_root_candidate}" CACHE PATH "GeoCoin CUDA Toolkit root")
                break()
            endif()
        endforeach()
    endif()
    find_package(CUDAToolkit QUIET)
    find_program(GEOCOIN_CUDA_NVCC_EXECUTABLE
        NAMES nvcc nvcc.exe
        HINTS
            "${CUDAToolkit_ROOT}/bin"
            "${CUDAToolkit_BIN_DIR}"
            "$ENV{CUDA_PATH}/bin"
            "$ENV{CUDA_HOME}/bin"
            "/usr/local/cuda-13.1/bin"
            "/usr/local/cuda-12.4/bin"
            "/usr/local/cuda-12.5/bin"
            "/usr/local/cuda-12.6/bin"
            "/usr/local/cuda-12.8/bin"
            "/usr/local/cuda/bin"
    )
    if(CUDAToolkit_FOUND AND GEOCOIN_CUDA_NVCC_EXECUTABLE)
        if(CUDAToolkit_VERSION VERSION_LESS "12.0")
            if(_geocoin_enable_cuda STREQUAL "ON")
                message(FATAL_ERROR "GeoCoin CUDA GPU miner release requires CUDA Toolkit 12.0 or newer; found ${CUDAToolkit_VERSION}")
            else()
                message(WARNING "CUDA Toolkit ${CUDAToolkit_VERSION} is older than 12.0; CUDA GPU miner backend disabled")
            endif()
        else()
            set(_geocoin_cuda_arches "${CMAKE_CUDA_ARCHITECTURES}")
            if(_geocoin_cuda_arches STREQUAL "")
                set(_geocoin_cuda_arches "all")
            endif()
            set(_geocoin_cuda_gencode "")
            string(TOLOWER "${_geocoin_cuda_arches}" _geocoin_cuda_arches_lower)
            if(_geocoin_cuda_arches_lower STREQUAL "sm_all")
                set(_geocoin_cuda_arches_lower "all")
            elseif(_geocoin_cuda_arches_lower STREQUAL "sm_all_major")
                set(_geocoin_cuda_arches_lower "all-major")
            endif()
            if(_geocoin_cuda_arches_lower MATCHES "^(all|all-major|native)$")
                list(APPEND _geocoin_cuda_gencode "-arch=${_geocoin_cuda_arches_lower}")
            else()
                foreach(_geocoin_cuda_arch IN LISTS _geocoin_cuda_arches)
                    string(REGEX REPLACE "^(sm_|compute_)" "" _geocoin_cuda_arch_number "${_geocoin_cuda_arch}")
                    list(APPEND _geocoin_cuda_gencode "-gencode=arch=compute_${_geocoin_cuda_arch_number},code=sm_${_geocoin_cuda_arch_number}")
                endforeach()
                list(GET _geocoin_cuda_arches -1 _geocoin_cuda_ptx_arch)
                string(REGEX REPLACE "^(sm_|compute_)" "" _geocoin_cuda_ptx_arch "${_geocoin_cuda_ptx_arch}")
                list(APPEND _geocoin_cuda_gencode "-gencode=arch=compute_${_geocoin_cuda_ptx_arch},code=compute_${_geocoin_cuda_ptx_arch}")
            endif()

            set(_geocoin_cuda_include_args "")
            foreach(_geocoin_cuda_include
                    "${CMAKE_CURRENT_SOURCE_DIR}/include"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools")
                list(APPEND _geocoin_cuda_include_args "-I${_geocoin_cuda_include}")
            endforeach()
            foreach(_geocoin_cuda_include IN LISTS CUDAToolkit_INCLUDE_DIRS)
                list(APPEND _geocoin_cuda_include_args "-I${_geocoin_cuda_include}")
            endforeach()
            if(OPENSSL_INCLUDE_DIR)
                list(APPEND _geocoin_cuda_include_args "-I${OPENSSL_INCLUDE_DIR}")
            endif()
            if(OPENSSL_INCLUDE_DIRS)
                foreach(_geocoin_openssl_include IN LISTS OPENSSL_INCLUDE_DIRS)
                    list(APPEND _geocoin_cuda_include_args "-I${_geocoin_openssl_include}")
                endforeach()
            endif()

            if(WIN32)
                set(_geocoin_cuda_object "${CMAKE_CURRENT_BINARY_DIR}/gpu_miner_cuda.obj")
            else()
                set(_geocoin_cuda_object "${CMAKE_CURRENT_BINARY_DIR}/gpu_miner_cuda.o")
            endif()
            set(_geocoin_cuda_perf_flags -O3 --extra-device-vectorization)
            if(NOT GEOCOIN_CUDA_EXTRA_NVCC_FLAGS STREQUAL "")
                list(APPEND _geocoin_cuda_perf_flags ${GEOCOIN_CUDA_EXTRA_NVCC_FLAGS})
            endif()
            if(NOT GEOCOIN_CUDA_MAXRREGCOUNT STREQUAL "")
                list(APPEND _geocoin_cuda_perf_flags "--maxrregcount=${GEOCOIN_CUDA_MAXRREGCOUNT}")
            endif()
            add_custom_command(
                OUTPUT "${_geocoin_cuda_object}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}"
                COMMAND "${GEOCOIN_CUDA_NVCC_EXECUTABLE}"
                        -std=c++20
                        ${_geocoin_cuda_perf_flags}
                        -DGEOCOIN_GPU_ENABLE_CUDA=1
                        "-DGEOCOIN_CUDA_THREADS_PER_BLOCK=${GEOCOIN_CUDA_THREADS_PER_BLOCK}"
                        "-DGEOCOIN_CUDA_NONCES_PER_THREAD=${GEOCOIN_CUDA_NONCES_PER_THREAD}"
                        "-DGEOCOIN_CUDA_MIN_BLOCKS_PER_SM=${GEOCOIN_CUDA_MIN_BLOCKS_PER_SM}"
                        "-DGEOCOIN_CUDA_PERSISTENT_GRID=$<IF:$<STREQUAL:${GEOCOIN_CUDA_PERSISTENT_GRID},ON>,1,0>"
                        "-DGEOCOIN_CUDA_PERSISTENT_BLOCKS_PER_SM=${GEOCOIN_CUDA_PERSISTENT_BLOCKS_PER_SM}"
                        "-DGEOCOIN_CUDA_CONSTANT_WORK=$<IF:$<STREQUAL:${GEOCOIN_CUDA_CONSTANT_WORK},ON>,1,0>"
                        "-DGEOCOIN_CUDA_IMMEDIATE_K=$<IF:$<STREQUAL:${GEOCOIN_CUDA_IMMEDIATE_K},ON>,1,0>"
                        "-DGEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS=$<IF:$<STREQUAL:${GEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS},ON>,1,0>"
                        "-DGEOCOIN_CUDA_SPECIALIZE_FINAL_LEN_BLOCK=$<IF:$<STREQUAL:${GEOCOIN_CUDA_SPECIALIZE_FINAL_LEN_BLOCK},ON>,1,0>"
                        ${_geocoin_cuda_include_args}
                        ${_geocoin_cuda_gencode}
                        -c "${CMAKE_CURRENT_SOURCE_DIR}/tools/gpu_miner_cuda.cu"
                        -o "${_geocoin_cuda_object}"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gpu_miner_cuda.cu"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/gpu_miner_common.hpp"
                VERBATIM
                COMMENT "Compiling GeoCoin CUDA GPU miner backend with nvcc"
            )
            set_source_files_properties("${_geocoin_cuda_object}" PROPERTIES
                GENERATED TRUE
                EXTERNAL_OBJECT TRUE
            )
            list(APPEND _geocoin_gpu_sources "${_geocoin_cuda_object}")
            list(APPEND _geocoin_gpu_defs GEOCOIN_GPU_ENABLE_CUDA=1)
            set(_geocoin_gpu_has_cuda ON)
            if(TARGET CUDA::cudart_static)
                list(APPEND _geocoin_gpu_libs CUDA::cudart_static)
            else()
                list(APPEND _geocoin_gpu_libs CUDA::cudart)
            endif()
        endif()
    elseif(_geocoin_enable_cuda STREQUAL "ON")
        message(FATAL_ERROR "GEOCOIN_ENABLE_CUDA=ON but CUDA Toolkit/nvcc was not found")
    else()
        message(STATUS "CUDA Toolkit/nvcc was not found; CUDA GPU miner backend disabled")
    endif()
endif()

string(TOUPPER "${GEOCOIN_ENABLE_HIP}" _geocoin_enable_hip)
if(NOT _geocoin_enable_hip MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "GEOCOIN_ENABLE_HIP must be AUTO, ON, or OFF")
endif()
if(NOT _geocoin_enable_hip STREQUAL "OFF")
    set(_geocoin_hip_root_candidates
        "$ENV{HIP_PATH}"
        "$ENV{ROCM_PATH}"
    )
    set(_geocoin_hipcc_names hipcc)
    set(_geocoin_hipcc_hints
        "$ENV{HIP_PATH}/bin"
        "$ENV{ROCM_PATH}/bin"
    )
    if(WIN32)
        list(APPEND _geocoin_hip_root_candidates
            "C:/Program Files/AMD/ROCm/7.1"
            "C:/Program Files/AMD/ROCm/6.4"
            "C:/Program Files/AMD/ROCm/6.2"
            "C:/Program Files/AMD/ROCm")
        set(_geocoin_hipcc_names hipcc.bin.exe hipcc hipcc.bat)
        list(APPEND _geocoin_hipcc_hints
            "C:/Program Files/AMD/ROCm/7.1/bin"
            "C:/Program Files/AMD/ROCm/6.4/bin"
            "C:/Program Files/AMD/ROCm/6.2/bin"
            "C:/Program Files/AMD/ROCm/bin")
    else()
        list(APPEND _geocoin_hip_root_candidates "/opt/rocm")
        list(APPEND _geocoin_hipcc_hints "/opt/rocm/bin")
    endif()
    find_program(GEOCOIN_HIPCC_EXECUTABLE
        NAMES ${_geocoin_hipcc_names}
        HINTS ${_geocoin_hipcc_hints}
    )
    if(WIN32)
        get_filename_component(_geocoin_hipcc_name "${GEOCOIN_HIPCC_EXECUTABLE}" NAME)
    endif()
    if(WIN32 AND (_geocoin_hipcc_name STREQUAL "hipcc" OR GEOCOIN_HIPCC_EXECUTABLE MATCHES "\\.bat$"))
        get_filename_component(_geocoin_hipcc_dir "${GEOCOIN_HIPCC_EXECUTABLE}" DIRECTORY)
        set(_geocoin_hipcc_bin "${_geocoin_hipcc_dir}/hipcc.bin.exe")
        if(EXISTS "${_geocoin_hipcc_bin}")
            set(GEOCOIN_HIPCC_EXECUTABLE "${_geocoin_hipcc_bin}" CACHE FILEPATH "HIP compiler executable" FORCE)
        endif()
    endif()
    find_path(GEOCOIN_HIP_INCLUDE_DIR
        NAMES hip/hip_runtime.h
        HINTS ${_geocoin_hip_root_candidates}
        PATH_SUFFIXES include
    )
    find_library(GEOCOIN_HIP_LIBRARY
        NAMES amdhip64
        HINTS ${_geocoin_hip_root_candidates}
        PATH_SUFFIXES lib lib64
    )
    if(GEOCOIN_HIPCC_EXECUTABLE AND GEOCOIN_HIP_INCLUDE_DIR AND GEOCOIN_HIP_LIBRARY)
        set(_geocoin_hip_include_args "")
        foreach(_geocoin_hip_include
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/src"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools")
            list(APPEND _geocoin_hip_include_args "-I${_geocoin_hip_include}")
        endforeach()
        if(OPENSSL_INCLUDE_DIR)
            list(APPEND _geocoin_hip_include_args "-I${OPENSSL_INCLUDE_DIR}")
        endif()
        if(OPENSSL_INCLUDE_DIRS)
            foreach(_geocoin_openssl_include IN LISTS OPENSSL_INCLUDE_DIRS)
                list(APPEND _geocoin_hip_include_args "-I${_geocoin_openssl_include}")
            endforeach()
        endif()

        set(_geocoin_hip_arch_flags "")
        if(NOT GEOCOIN_HIP_ARCHITECTURES STREQUAL "")
            foreach(_geocoin_hip_arch IN LISTS GEOCOIN_HIP_ARCHITECTURES)
                list(APPEND _geocoin_hip_arch_flags "--offload-arch=${_geocoin_hip_arch}")
            endforeach()
        endif()

        if(WIN32)
            set(_geocoin_hip_object "${CMAKE_CURRENT_BINARY_DIR}/gpu_miner_hip.obj")
        else()
            set(_geocoin_hip_object "${CMAKE_CURRENT_BINARY_DIR}/gpu_miner_hip.o")
        endif()
        set(_geocoin_hip_command "${GEOCOIN_HIPCC_EXECUTABLE}")
        set(_geocoin_hip_perf_flags -O3)
        if(WIN32)
            list(APPEND _geocoin_hip_perf_flags
                -include stdint.h
                -include math.h)
        else()
            list(APPEND _geocoin_hip_perf_flags -fPIC)
        endif()
        if(NOT GEOCOIN_HIP_EXTRA_HIPCC_FLAGS STREQUAL "")
            list(APPEND _geocoin_hip_perf_flags ${GEOCOIN_HIP_EXTRA_HIPCC_FLAGS})
        endif()
        add_custom_command(
            OUTPUT "${_geocoin_hip_object}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}"
            COMMAND ${CMAKE_COMMAND} -E env "INCLUDE=" "CL="
                    ${_geocoin_hip_command}
                    -std=c++20
                    -x hip
                    ${_geocoin_hip_perf_flags}
                    -DGEOCOIN_GPU_ENABLE_HIP=1
                    "-DGEOCOIN_CUDA_THREADS_PER_BLOCK=${GEOCOIN_HIP_THREADS_PER_BLOCK}"
                    "-DGEOCOIN_CUDA_NONCES_PER_THREAD=${GEOCOIN_HIP_NONCES_PER_THREAD}"
                    "-DGEOCOIN_CUDA_MIN_BLOCKS_PER_SM=${GEOCOIN_HIP_MIN_BLOCKS_PER_SM}"
                    "-DGEOCOIN_CUDA_PERSISTENT_GRID=$<IF:$<STREQUAL:${GEOCOIN_HIP_PERSISTENT_GRID},ON>,1,0>"
                    "-DGEOCOIN_CUDA_PERSISTENT_BLOCKS_PER_SM=${GEOCOIN_HIP_PERSISTENT_BLOCKS_PER_SM}"
                    "-DGEOCOIN_CUDA_CONSTANT_WORK=$<IF:$<STREQUAL:${GEOCOIN_CUDA_CONSTANT_WORK},ON>,1,0>"
                    "-DGEOCOIN_CUDA_IMMEDIATE_K=$<IF:$<STREQUAL:${GEOCOIN_CUDA_IMMEDIATE_K},ON>,1,0>"
                    "-DGEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS=$<IF:$<STREQUAL:${GEOCOIN_CUDA_PRECOMPUTE_TAIL_ROUNDS},ON>,1,0>"
                    "-DGEOCOIN_CUDA_SPECIALIZE_FINAL_LEN_BLOCK=$<IF:$<STREQUAL:${GEOCOIN_CUDA_SPECIALIZE_FINAL_LEN_BLOCK},ON>,1,0>"
                    ${_geocoin_hip_include_args}
                    ${_geocoin_hip_arch_flags}
                    -c "${CMAKE_CURRENT_SOURCE_DIR}/tools/gpu_miner_cuda.cu"
                    -o "${_geocoin_hip_object}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gpu_miner_cuda.cu"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/gpu_miner_common.hpp"
            VERBATIM
            COMMENT "Compiling GeoCoin AMD HIP GPU miner backend with hipcc"
        )
        set_source_files_properties("${_geocoin_hip_object}" PROPERTIES
            GENERATED TRUE
            EXTERNAL_OBJECT TRUE
        )
        list(APPEND _geocoin_gpu_sources "${_geocoin_hip_object}")
        list(APPEND _geocoin_gpu_defs GEOCOIN_GPU_ENABLE_HIP=1)
        list(APPEND _geocoin_gpu_libs "${GEOCOIN_HIP_LIBRARY}")
        set(_geocoin_gpu_has_hip ON)
    elseif(_geocoin_enable_hip STREQUAL "ON")
        message(FATAL_ERROR "GEOCOIN_ENABLE_HIP=ON but HIP SDK/hipcc/amdhip64 was not found")
    else()
        message(STATUS "AMD HIP SDK/hipcc was not found; HIP GPU miner backend disabled")
    endif()
endif()

string(TOUPPER "${GEOCOIN_ENABLE_OPENCL}" _geocoin_enable_opencl)
if(NOT _geocoin_enable_opencl MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "GEOCOIN_ENABLE_OPENCL must be AUTO, ON, or OFF")
endif()
if(NOT _geocoin_enable_opencl STREQUAL "OFF")
    find_package(OpenCL QUIET)
    if(OpenCL_FOUND)
        list(APPEND _geocoin_gpu_sources tools/gpu_miner_opencl.cpp)
        list(APPEND _geocoin_gpu_defs GEOCOIN_GPU_ENABLE_OPENCL=1)
        if(WIN32)
            # Windows release must not hard-import OpenCL.dll. The backend
            # loads the ICD loader at runtime so CUDA/HIP enumeration still
            # works on machines without an OpenCL SDK/loader installed.
            list(APPEND _geocoin_gpu_include_dirs ${OpenCL_INCLUDE_DIRS})
        else()
            list(APPEND _geocoin_gpu_libs OpenCL::OpenCL)
        endif()
    elseif(_geocoin_enable_opencl STREQUAL "ON")
        message(FATAL_ERROR "GEOCOIN_ENABLE_OPENCL=ON but OpenCL headers/library were not found")
    else()
        message(STATUS "OpenCL was not found; OpenCL GPU miner backend disabled")
    endif()
endif()

add_executable(geocoin-gpu-miner ${_geocoin_gpu_sources})
target_link_libraries(geocoin-gpu-miner PRIVATE ${_geocoin_gpu_libs})
target_compile_definitions(geocoin-gpu-miner PRIVATE ${_geocoin_gpu_defs})
target_include_directories(geocoin-gpu-miner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools ${_geocoin_gpu_include_dirs})
if(_geocoin_gpu_has_cuda)
    set_target_properties(geocoin-gpu-miner PROPERTIES
        CUDA_RUNTIME_LIBRARY Static
    )
endif()
if(WIN32 AND _geocoin_gpu_has_hip)
    target_link_options(geocoin-gpu-miner PRIVATE "/DELAYLOAD:amdhip64_7.dll")
    target_link_options(geocoin-gpu-miner PRIVATE "/DELAYLOAD:amdhip64_6.dll")
    target_link_libraries(geocoin-gpu-miner PRIVATE delayimp)
endif()
geocoin_stage_runtime_dependencies(geocoin-gpu-miner)
install(TARGETS geocoin-gpu-miner DESTINATION bin)
