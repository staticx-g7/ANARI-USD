cmake_minimum_required(VERSION 3.16)
project(rawgeo_point_gate_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(test_rawgeo_point_gate test_rawgeo_point_gate.cpp)
target_include_directories(test_rawgeo_point_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
