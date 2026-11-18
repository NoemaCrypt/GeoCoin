cmake_minimum_required(VERSION 3.15)
project(GeoCoin VERSION 0.1.3 LANGUAGES C CXX)

set(GEOCOIN_MACOS_ARM64 OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND
    (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
     (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")))
    set(GEOCOIN_MACOS_ARM64 ON)
    add_compile_definitions(GEOCOIN_MACOS_ARM64=1)
endif()

# Keep policy/options before dependencies; each included module uses root scope.
include(cmake/Options.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Libraries.cmake)
include(cmake/Tools.cmake)

if(BUILD_QT_GUI)
    if(GEOCOIN_MACOS_ARM64)
        add_subdirectory(src/macos)
    else()
        add_subdirectory(src/qt)
    endif()
endif()

if(BUILD_TESTS)
    include(cmake/Tests.cmake)
endif()

if(BUILD_FUZZERS)
    include(cmake/Fuzzers.cmake)
endif()
