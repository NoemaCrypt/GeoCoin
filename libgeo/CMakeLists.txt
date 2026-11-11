cmake_minimum_required(VERSION 3.15)
project(libgeo VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LIBGEO_BUILD_SHARED "Build shared libgeo" ON)
option(LIBGEO_BUILD_STATIC "Build static libgeo" OFF)

find_package(OpenSSL REQUIRED)

set(GEOCOIN_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_QT_GUI OFF CACHE BOOL "" FORCE)
add_subdirectory(${GEOCOIN_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/geocoin EXCLUDE_FROM_ALL)

set(LIBGEO_PUBLIC_HEADERS
    include/libgeo/libgeo.h
)

set(LIBGEO_SOURCES
    src/libgeo.cpp
)

if(LIBGEO_BUILD_SHARED)
    add_library(geo SHARED ${LIBGEO_SOURCES} ${LIBGEO_PUBLIC_HEADERS})
    target_compile_definitions(geo PRIVATE LIBGEO_EXPORTS)
    target_include_directories(geo
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${GEOCOIN_ROOT}/include
    )
    target_link_libraries(geo PRIVATE geocoin_core geocoin_crypto geocoin_wallet OpenSSL::Crypto)
    set_target_properties(geo PROPERTIES OUTPUT_NAME "geo")
    if(WIN32)
        set_target_properties(geo PROPERTIES PREFIX "" IMPORT_PREFIX "" IMPORT_SUFFIX ".lib")
    endif()
endif()

if(LIBGEO_BUILD_STATIC)
    add_library(geo_static STATIC ${LIBGEO_SOURCES} ${LIBGEO_PUBLIC_HEADERS})
    target_include_directories(geo_static
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${GEOCOIN_ROOT}/include
    )
    target_link_libraries(geo_static PRIVATE geocoin_core geocoin_crypto geocoin_wallet OpenSSL::Crypto)
    set_target_properties(geo_static PROPERTIES OUTPUT_NAME "geo")
endif()

install(FILES ${LIBGEO_PUBLIC_HEADERS} DESTINATION include/libgeo)
if(TARGET geo)
    install(TARGETS geo
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()
if(TARGET geo_static)
    install(TARGETS geo_static
        ARCHIVE DESTINATION lib
    )
endif()
