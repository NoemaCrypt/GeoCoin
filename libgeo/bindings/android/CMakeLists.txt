cmake_minimum_required(VERSION 3.15)
project(libgeo_android_jni LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/libgeo)

find_package(JNI REQUIRED)

add_library(geo_jni SHARED src/main/cpp/libgeo_jni.cpp)

target_include_directories(geo_jni PRIVATE ${JNI_INCLUDE_DIRS})
target_link_libraries(geo_jni PRIVATE geo)
