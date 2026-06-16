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
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)

# Include directories
include_directories(
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
    ${ZMQ_LIBRARIES}
)
target_include_directories(${PROJECT_NAME} PRIVATE ${ZMQ_INCLUDE_DIRS})

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

# ============================================================================
# Middleware Shared Library - C API for Unreal Engine
# ============================================================================

set(MIDDLEWARE_SOURCES
    src/MiddlewareCWrapper.cpp
    src/HpcMeshConverter.cpp
    src/UsdProcessorHpc.cpp
    src/CollisionProcessorHpc.cpp
)

add_library(anari_usd_middleware SHARED ${MIDDLEWARE_SOURCES})

# Symbol visibility
set_target_properties(anari_usd_middleware PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_compile_definitions(anari_usd_middleware PRIVATE ANARI_USD_MIDDLEWARE_EXPORTS)

# Link middleware libraries
target_link_libraries(anari_usd_middleware 
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${ZMQ_LIBRARIES}
)
target_include_directories(anari_usd_middleware PRIVATE ${ZMQ_INCLUDE_DIRS})

# Compiler options (no -Werror for middleware - stubs are intentional)
if(MSVC)
    target_compile_options(anari_usd_middleware PRIVATE /wd4251 /wd4458 /wd4100)
else()
    target_compile_options(anari_usd_middleware PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)
endif()

target_compile_definitions(anari_usd_middleware PRIVATE 
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
