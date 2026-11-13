cmake_minimum_required(VERSION 3.15)
project(GeoCoin VERSION 0.1.1 LANGUAGES C CXX)

# Keep policy/options before dependencies; each included module uses root scope.
include(cmake/Options.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Libraries.cmake)
include(cmake/Tools.cmake)

if(BUILD_QT_GUI)
    add_subdirectory(src/qt)
endif()

if(BUILD_TESTS)
    include(cmake/Tests.cmake)
endif()

if(BUILD_FUZZERS)
    include(cmake/Fuzzers.cmake)
endif()
