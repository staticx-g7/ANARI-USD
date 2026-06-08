# CMakeLists.txt for HPC Mesh Converter
cmake_minimum_required(VERSION 3.16)
project(HpcMeshConverter)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Find required packages
find_package(Threads REQUIRED)
find_package(OpenMP REQUIRED)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/cppzmq
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/ZeroMQ/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/include
)

# Source files
set(SOURCES
    src/main.cpp
    src/HpcMeshConverter.cpp
    src/UsdProcessorHpc.cpp
    src/CollisionProcessorHpc.cpp
)

# Header files
set(HEADERS
    src/HpcMeshConverter.h
    src/UsdProcessorHpc.h
    src/CollisionProcessorHpc.h
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Disable warnings treated as errors
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /wd4251 /wd4458 /wd4100)
endif()

# Link libraries
target_link_libraries(${PROJECT_NAME} 
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# Enable OpenMP
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
if(OPENMP_FOUND)
    target_compile_options(${PROJECT_NAME} PRIVATE ${OpenMP_CXX_FLAGS})
endif()

# Set compiler options
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Add definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE 
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)