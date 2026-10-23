cmake_minimum_required(VERSION 3.16)
project(rawgeo_curve_pack_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(test_rawgeo_curve_pack test_rawgeo_curve_pack.cpp)
target_include_directories(test_rawgeo_curve_pack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
