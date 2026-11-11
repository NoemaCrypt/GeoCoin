# Geometry used by consensus must use the same source and precision everywhere.
# Build the pinned upstream library directly, without its tools, data downloads,
# documentation, tests, or global BUILD_SHARED_LIBS/BUILD_TESTING options.
set(GEOCOIN_GEOGRAPHICLIB_REVISION "849284b9620b2ee4f0b9841b6822c67f9d0fa0c2")
set(_geocoin_geographiclib_dir "${CMAKE_CURRENT_SOURCE_DIR}/geographiclib")
if(NOT EXISTS "${_geocoin_geographiclib_dir}/.git")
    message(FATAL_ERROR "Initialize pinned GeographicLib 2.3.0 with: git submodule update --init --recursive")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_geocoin_geographiclib_dir}" rev-parse HEAD
    RESULT_VARIABLE _geocoin_geo_result OUTPUT_VARIABLE _geocoin_geo_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _geocoin_geo_result EQUAL 0 OR
   NOT _geocoin_geo_revision STREQUAL GEOCOIN_GEOGRAPHICLIB_REVISION)
    message(FATAL_ERROR "GeographicLib must be at ${GEOCOIN_GEOGRAPHICLIB_REVISION} (2.3.0); run git submodule update --init --recursive")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_geocoin_geographiclib_dir}" status --porcelain --untracked-files=all
    RESULT_VARIABLE _geocoin_geo_result OUTPUT_VARIABLE _geocoin_geo_changes
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _geocoin_geo_result EQUAL 0 OR _geocoin_geo_changes)
    message(FATAL_ERROR "The pinned GeographicLib checkout must be clean")
endif()

# Recheck the pin when an existing build sees a different checkout or edited
# source, instead of relying on developers to rerun configuration manually.
file(GLOB_RECURSE _geocoin_geo_inputs CONFIGURE_DEPENDS
    "${_geocoin_geographiclib_dir}/src/*.cpp"
    "${_geocoin_geographiclib_dir}/src/*.hh"
    "${_geocoin_geographiclib_dir}/include/*.hpp"
    "${_geocoin_geographiclib_dir}/include/*.in")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_geocoin_geographiclib_dir}" rev-parse --absolute-git-dir
    OUTPUT_VARIABLE _geocoin_geo_git_dir OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _geocoin_geo_result)
if(NOT _geocoin_geo_result EQUAL 0)
    message(FATAL_ERROR "Cannot resolve pinned GeographicLib git directory")
endif()
list(APPEND _geocoin_geo_inputs "${_geocoin_geo_git_dir}/HEAD" "${_geocoin_geo_git_dir}/index")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_geocoin_geo_inputs} "${_geocoin_geographiclib_dir}/CMakeLists.txt")

function(geocoin_build_geographiclib source_dir)
    set(PROJECT_VERSION "2.3")
    set(PROJECT_VERSION_MAJOR 2)
    set(PROJECT_VERSION_MINOR 3)
    set(PROJECT_VERSION_PATCH 0)
    set(GEOGRAPHICLIB_DATA "") # No external datasets are used by WGS84 geodesy.
    set(GEOGRAPHICLIB_PRECISION 2) # IEEE 754 binary64, including Windows.
    set(GEOGRAPHICLIB_LIB_TYPE_VAL 0)
    include(CheckTypeSize)
    check_type_size("long double" GEOCOIN_LONG_DOUBLE_SIZE LANGUAGE CXX)
    set(GEOGRAPHICLIB_HAVE_LONG_DOUBLE 0)
    if(GEOCOIN_LONG_DOUBLE_SIZE GREATER 8)
        set(GEOGRAPHICLIB_HAVE_LONG_DOUBLE 1)
    endif()
    include(TestBigEndian)
    test_big_endian(GEOGRAPHICLIB_WORDS_BIGENDIAN)
    set(config_dir "${CMAKE_CURRENT_BINARY_DIR}/geographiclib/include")
    configure_file("${source_dir}/include/GeographicLib/Config.h.in"
        "${config_dir}/GeographicLib/Config.h" @ONLY)

    # Match the SOURCES list in the pinned upstream src/CMakeLists.txt.
    set(sources Accumulator AlbersEqualArea AuxAngle AuxLatitude
        AzimuthalEquidistant CassiniSoldner CircularEngine DAuxLatitude DMS DST
        Ellipsoid EllipticFunction GARS GeoCoords Geocentric Geodesic
        GeodesicExact GeodesicLine GeodesicLineExact Geohash Geoid Georef
        Gnomonic GravityCircle GravityModel Intersect LambertConformalConic
        LocalCartesian MGRS MagneticCircle MagneticModel Math NormalGravity
        OSGB PolarStereographic PolygonArea Rhumb SphericalEngine
        TransverseMercator TransverseMercatorExact UTMUPS Utility)
    list(TRANSFORM sources PREPEND "${source_dir}/src/")
    list(TRANSFORM sources APPEND ".cpp")
    add_library(geocoin_geographiclib STATIC ${sources})
    add_library(GeographicLib::GeographicLib ALIAS geocoin_geographiclib)
    target_include_directories(geocoin_geographiclib SYSTEM PUBLIC
        "${config_dir}" "${source_dir}/include")
    target_compile_definitions(geocoin_geographiclib PUBLIC GEOGRAPHICLIB_SHARED_LIB=0)
    set_target_properties(geocoin_geographiclib PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON)
    message(STATUS "GeographicLib: 2.3.0 (${GEOCOIN_GEOGRAPHICLIB_REVISION}), static, binary64")
endfunction()
geocoin_build_geographiclib("${_geocoin_geographiclib_dir}")

function(geocoin_strict_geodesy target_name)
    # Do not allow reassociation, contraction, or fast-math to change consensus.
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /fp:strict)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -fno-fast-math -ffp-contract=off -frounding-math)
    endif()
endfunction()
geocoin_strict_geodesy(geocoin_geographiclib)
