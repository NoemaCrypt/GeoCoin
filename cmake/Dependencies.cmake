# Find dependencies
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
include(cmake/GeographicLib.cmake)
find_package(Boost 1.71 REQUIRED)

set(GEOCOIN_RUNTIME_DLLS "")
set(GEOCOIN_OPENSSL_RUNTIME_DLLS_RELEASE "")
set(GEOCOIN_OPENSSL_RUNTIME_DLLS_DEBUG "")
if(WIN32)
    if(NOT GEOCOIN_WINDOWS_STATIC_RELEASE)
        set(_geocoin_openssl_runtime_dirs_release)
        set(_geocoin_openssl_runtime_dirs_debug)

        if(DEFINED OPENSSL_ROOT_DIR AND OPENSSL_ROOT_DIR)
            list(APPEND _geocoin_openssl_runtime_dirs_release "${OPENSSL_ROOT_DIR}/bin")
            list(APPEND _geocoin_openssl_runtime_dirs_debug "${OPENSSL_ROOT_DIR}/debug/bin")
        endif()

        set(_geocoin_vcpkg_installed_dir "")
        if(DEFINED VCPKG_INSTALLED_DIR AND VCPKG_INSTALLED_DIR)
            set(_geocoin_vcpkg_installed_dir "${VCPKG_INSTALLED_DIR}")
        elseif(DEFINED _VCPKG_INSTALLED_DIR AND _VCPKG_INSTALLED_DIR)
            set(_geocoin_vcpkg_installed_dir "${_VCPKG_INSTALLED_DIR}")
        endif()
        if(_geocoin_vcpkg_installed_dir AND
           DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET)
            list(APPEND _geocoin_openssl_runtime_dirs_release
                "${_geocoin_vcpkg_installed_dir}/${VCPKG_TARGET_TRIPLET}/bin")
            list(APPEND _geocoin_openssl_runtime_dirs_debug
                "${_geocoin_vcpkg_installed_dir}/${VCPKG_TARGET_TRIPLET}/debug/bin")
        endif()

        # FindOpenSSL normally exposes import libraries rather than their DLLs.
        # Derive the sibling bin directory so both vcpkg OpenSSL 1.1 and 3.x
        # naming schemes can be staged without hard-coding a major version.
        foreach(_openssl_component CRYPTO SSL)
            set(_openssl_release_variable "OPENSSL_${_openssl_component}_LIBRARY_RELEASE")
            set(_openssl_general_variable "OPENSSL_${_openssl_component}_LIBRARY")
            if(DEFINED ${_openssl_release_variable} AND
               NOT "${${_openssl_release_variable}}" STREQUAL "")
                foreach(_openssl_library IN LISTS ${_openssl_release_variable})
                    get_filename_component(_openssl_library_dir "${_openssl_library}" DIRECTORY)
                    get_filename_component(_openssl_runtime_prefix "${_openssl_library_dir}" DIRECTORY)
                    list(APPEND _geocoin_openssl_runtime_dirs_release "${_openssl_runtime_prefix}/bin")
                endforeach()
            elseif(DEFINED ${_openssl_general_variable})
                # A single-config/custom FindOpenSSL result may expose only the
                # general variable. Treat an unqualified path as usable by both
                # configurations and honor optimized/debug list markers.
                set(_openssl_library_configuration "general")
                foreach(_openssl_library IN LISTS ${_openssl_general_variable})
                    if(_openssl_library MATCHES "^(optimized|debug|general)$")
                        set(_openssl_library_configuration "${_openssl_library}")
                        continue()
                    endif()
                    if(_openssl_library)
                        get_filename_component(_openssl_library_dir "${_openssl_library}" DIRECTORY)
                        get_filename_component(_openssl_runtime_prefix "${_openssl_library_dir}" DIRECTORY)
                        if(_openssl_library_configuration STREQUAL "debug")
                            list(APPEND _geocoin_openssl_runtime_dirs_debug "${_openssl_runtime_prefix}/bin")
                        elseif(_openssl_library_configuration STREQUAL "optimized")
                            list(APPEND _geocoin_openssl_runtime_dirs_release "${_openssl_runtime_prefix}/bin")
                        else()
                            list(APPEND _geocoin_openssl_runtime_dirs_release "${_openssl_runtime_prefix}/bin")
                            list(APPEND _geocoin_openssl_runtime_dirs_debug "${_openssl_runtime_prefix}/bin")
                        endif()
                    endif()
                endforeach()
            endif()
        endforeach()
        foreach(_openssl_library_variable OPENSSL_CRYPTO_LIBRARY_DEBUG OPENSSL_SSL_LIBRARY_DEBUG)
            if(DEFINED ${_openssl_library_variable})
                foreach(_openssl_library IN LISTS ${_openssl_library_variable})
                    if(_openssl_library)
                        get_filename_component(_openssl_library_dir "${_openssl_library}" DIRECTORY)
                        get_filename_component(_openssl_runtime_prefix "${_openssl_library_dir}" DIRECTORY)
                        list(APPEND _geocoin_openssl_runtime_dirs_debug "${_openssl_runtime_prefix}/bin")
                    endif()
                endforeach()
            endif()
        endforeach()

        list(REMOVE_DUPLICATES _geocoin_openssl_runtime_dirs_release)
        list(REMOVE_DUPLICATES _geocoin_openssl_runtime_dirs_debug)
        foreach(_openssl_runtime_dir IN LISTS _geocoin_openssl_runtime_dirs_release)
            if(IS_DIRECTORY "${_openssl_runtime_dir}")
                file(GLOB _geocoin_openssl_runtime_dlls CONFIGURE_DEPENDS
                    "${_openssl_runtime_dir}/libcrypto*.dll"
                    "${_openssl_runtime_dir}/libssl*.dll"
                )
                list(APPEND GEOCOIN_OPENSSL_RUNTIME_DLLS_RELEASE ${_geocoin_openssl_runtime_dlls})
            endif()
        endforeach()
        foreach(_openssl_runtime_dir IN LISTS _geocoin_openssl_runtime_dirs_debug)
            if(IS_DIRECTORY "${_openssl_runtime_dir}")
                file(GLOB _geocoin_openssl_runtime_dlls CONFIGURE_DEPENDS
                    "${_openssl_runtime_dir}/libcrypto*.dll"
                    "${_openssl_runtime_dir}/libssl*.dll"
                )
                list(APPEND GEOCOIN_OPENSSL_RUNTIME_DLLS_DEBUG ${_geocoin_openssl_runtime_dlls})
            endif()
        endforeach()
        list(REMOVE_DUPLICATES GEOCOIN_OPENSSL_RUNTIME_DLLS_RELEASE)
        list(REMOVE_DUPLICATES GEOCOIN_OPENSSL_RUNTIME_DLLS_DEBUG)

        # A toolchain may only install one configuration. Keep that build
        # usable while selecting debug/bin whenever both variants exist.
        if(NOT GEOCOIN_OPENSSL_RUNTIME_DLLS_DEBUG)
            set(GEOCOIN_OPENSSL_RUNTIME_DLLS_DEBUG ${GEOCOIN_OPENSSL_RUNTIME_DLLS_RELEASE})
        endif()
        if(NOT GEOCOIN_OPENSSL_RUNTIME_DLLS_RELEASE)
            set(GEOCOIN_OPENSSL_RUNTIME_DLLS_RELEASE ${GEOCOIN_OPENSSL_RUNTIME_DLLS_DEBUG})
        endif()

        if(NOT GEOCOIN_OPENSSL_RUNTIME_DLLS_RELEASE)
            message(WARNING
                "Dynamic Windows build did not locate OpenSSL runtime DLLs; "
                "portable packaging will require them to be discoverable from the build or vcpkg tree")
        endif()
    endif()
endif()

function(geocoin_stage_runtime_dependencies target_name)
    if(WIN32)
        foreach(_dll IN LISTS GEOCOIN_RUNTIME_DLLS)
            add_custom_command(
                TARGET ${target_name}
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_dll}"
                        $<TARGET_FILE_DIR:${target_name}>
                VERBATIM
            )
        endforeach()
        if(GEOCOIN_OPENSSL_RUNTIME_DLLS_RELEASE OR GEOCOIN_OPENSSL_RUNTIME_DLLS_DEBUG)
            add_custom_command(
                TARGET ${target_name}
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<IF:$<CONFIG:Debug>,${GEOCOIN_OPENSSL_RUNTIME_DLLS_DEBUG},${GEOCOIN_OPENSSL_RUNTIME_DLLS_RELEASE}>"
                        $<TARGET_FILE_DIR:${target_name}>
                COMMAND_EXPAND_LISTS
                VERBATIM
            )
        endif()
    endif()
endfunction()

# RocksDB (required storage backend)
add_library(geocoin_rocksdb INTERFACE)

find_package(RocksDB CONFIG QUIET)
find_package(rocksdb CONFIG QUIET)

if(TARGET RocksDB::rocksdb)
    target_link_libraries(geocoin_rocksdb INTERFACE RocksDB::rocksdb)
    set(_geocoin_rocksdb_check_libraries RocksDB::rocksdb)
elseif(TARGET rocksdb::rocksdb)
    target_link_libraries(geocoin_rocksdb INTERFACE rocksdb::rocksdb)
    set(_geocoin_rocksdb_check_libraries rocksdb::rocksdb)
else()
    find_path(ROCKSDB_INCLUDE_DIR NAMES rocksdb/db.h)
    find_library(ROCKSDB_LIBRARY NAMES rocksdb)
    if(DEFINED ROCKSDB_INCLUDE_DIR)
        string(STRIP "${ROCKSDB_INCLUDE_DIR}" ROCKSDB_INCLUDE_DIR)
    endif()
    if(DEFINED ROCKSDB_LIBRARY)
        string(STRIP "${ROCKSDB_LIBRARY}" ROCKSDB_LIBRARY)
    endif()

    if(NOT ROCKSDB_INCLUDE_DIR OR NOT ROCKSDB_LIBRARY)
        message(FATAL_ERROR
            "RocksDB is required for GeoCoin.\n"
            "Linux/WSL Ubuntu: install librocksdb-dev, or use vcpkg manifest mode."
        )
    endif()

    target_include_directories(geocoin_rocksdb INTERFACE ${ROCKSDB_INCLUDE_DIR})
    target_link_libraries(geocoin_rocksdb INTERFACE ${ROCKSDB_LIBRARY})
    set(_geocoin_rocksdb_check_libraries "${ROCKSDB_LIBRARY}")
    set(_geocoin_rocksdb_check_includes "${ROCKSDB_INCLUDE_DIR}")
endif()

if(GEOCOIN_MSVC_STATIC_RUNTIME_LIB)
    target_link_libraries(geocoin_rocksdb INTERFACE "${GEOCOIN_MSVC_STATIC_RUNTIME_LIB}")
    list(APPEND _geocoin_rocksdb_check_libraries "${GEOCOIN_MSVC_STATIC_RUNTIME_LIB}")
endif()

# Check the selected headers and library together, before building the node.
# Older distro packages lack write stalls and can also fail in db.h under C++20.
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES ${_geocoin_rocksdb_check_libraries})
set(CMAKE_REQUIRED_INCLUDES ${_geocoin_rocksdb_check_includes})
unset(GEOCOIN_ROCKSDB_HAS_REQUIRED_API CACHE)
check_cxx_source_compiles([[
    #include <memory>
    #include <rocksdb/db.h>
    #include <rocksdb/write_buffer_manager.h>
    int main() {
        rocksdb::Options options;
        options.write_buffer_manager =
            std::make_shared<rocksdb::WriteBufferManager>(1048576, nullptr, true);
        return options.write_buffer_manager->enabled() ? 0 : 1;
    }
]] GEOCOIN_ROCKSDB_HAS_REQUIRED_API)
cmake_pop_check_state()
if(NOT GEOCOIN_ROCKSDB_HAS_REQUIRED_API)
    message(FATAL_ERROR
        "The selected RocksDB cannot build GeoCoin's C++20 storage API.\n"
        "Use RocksDB >= 6.29.5 with WriteBufferManager write-stall support.\n"
        "Linux: run bash scripts/bootstrap-linux.sh --profile dev; "
        "it installs a pinned compatible release when the distro package is too old.\n"
        "For custom installs, check CMAKE_PREFIX_PATH and RocksDB_DIR/rocksdb_DIR."
    )
endif()

set(GEOCOIN_OPTIONAL_MATH_LIBS "")
if(NOT WIN32)
    list(APPEND GEOCOIN_OPTIONAL_MATH_LIBS m)
endif()

# liboqs (real Falcon-512 backend)
# КРИТИЧНО: Версия liboqs должна быть жестко зафиксирована для воспроизводимости криптографии.
set(LIBOQS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/liboqs")
if(NOT EXISTS "${LIBOQS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "liboqs not found in ${LIBOQS_DIR}. "
                        "Clone fixed version with: "
                        "git clone --depth 1 --branch 0.15.0 https://github.com/open-quantum-safe/liboqs.git ${LIBOQS_DIR}")
endif()

file(READ "${LIBOQS_DIR}/CMakeLists.txt" LIBOQS_CMAKE_CONTENT)
string(REGEX MATCH "set\\(OQS_VERSION_MAJOR[ \t]+([0-9]+)\\)" _OQS_MAJOR_MATCH "${LIBOQS_CMAKE_CONTENT}")
set(LIBOQS_VERSION_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "set\\(OQS_VERSION_MINOR[ \t]+([0-9]+)\\)" _OQS_MINOR_MATCH "${LIBOQS_CMAKE_CONTENT}")
set(LIBOQS_VERSION_MINOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "set\\(OQS_VERSION_PATCH[ \t]+([0-9]+)\\)" _OQS_PATCH_MATCH "${LIBOQS_CMAKE_CONTENT}")
set(LIBOQS_VERSION_PATCH "${CMAKE_MATCH_1}")

if(NOT LIBOQS_VERSION_MAJOR STREQUAL "0" OR
   NOT LIBOQS_VERSION_MINOR STREQUAL "15" OR
   NOT LIBOQS_VERSION_PATCH STREQUAL "0")
    message(FATAL_ERROR "Unsupported liboqs version in ${LIBOQS_DIR}. "
                        "Required: 0.15.0, found: ${LIBOQS_VERSION_MAJOR}.${LIBOQS_VERSION_MINOR}.${LIBOQS_VERSION_PATCH}")
endif()

set(OQS_BUILD_ONLY_LIB ON CACHE BOOL "" FORCE)
set(OQS_MINIMAL_BUILD "KEM_ml_kem_768;SIG_falcon_512" CACHE STRING "" FORCE)
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    # liboqs leaves this cached OFF after a minimal build. Enable its supported
    # AVX2/BMI2/POPCNT backend, retaining runtime dispatch for older CPUs.
    set(OQS_ENABLE_KEM_ml_kem_768_x86_64 ON CACHE BOOL "" FORCE)
elseif(WIN32)
    # liboqs 0.15.0's x86_64 ML-KEM assembly uses the System V ABI. Windows
    # must use the portable implementation, as required by alg_support.cmake.
    set(OQS_ENABLE_KEM_ml_kem_768_x86_64 OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(liboqs EXCLUDE_FROM_ALL)
