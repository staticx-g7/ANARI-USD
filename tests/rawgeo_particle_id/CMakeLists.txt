cmake_minimum_required(VERSION 3.16)
project(rawgeo_particle_id_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(test_rawgeo_particle_id test_rawgeo_particle_id.cpp)
target_include_directories(test_rawgeo_particle_id PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

enable_testing()
add_test(NAME rawgeo_particle_id COMMAND test_rawgeo_particle_id)
