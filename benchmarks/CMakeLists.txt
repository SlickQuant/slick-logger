cmake_minimum_required(VERSION 3.20)

# Find required packages for benchmarks
find_package(Threads REQUIRED)

# Try to find external libraries for comparison
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SPDLOG QUIET spdlog)
    pkg_check_modules(FMT QUIET fmt)
endif()

# Download and setup spdlog if not found
if(NOT SPDLOG_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.12.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(spdlog)
    set(SPDLOG_LIBRARIES spdlog::spdlog)
endif()

# Download and setup fmt if not found
if(NOT FMT_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG        10.1.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(fmt)
    set(FMT_LIBRARIES fmt::fmt)
endif()

# Benchmark infrastructure
add_library(benchmark_utils INTERFACE)
target_include_directories(benchmark_utils INTERFACE .)
target_link_libraries(benchmark_utils INTERFACE Threads::Threads)

if(WIN32)
    target_link_libraries(benchmark_utils INTERFACE psapi)
endif()

# Simple working benchmark  
add_executable(simple_benchmark simple_benchmark.cpp)
target_link_libraries(simple_benchmark slick::logger spdlog::spdlog fmt::fmt)

# Quick benchmark for testing
add_executable(quick_benchmark quick_benchmark.cpp)
target_link_libraries(quick_benchmark slick::logger spdlog::spdlog fmt::fmt)

# Main benchmark executable
add_executable(slick_logger_benchmark
    benchmark_main.cpp
    benchmark_utils.hpp
    system_monitor.hpp
    test_scenarios.hpp
)

target_link_libraries(slick_logger_benchmark
    slick::logger
    benchmark_utils
    ${SPDLOG_LIBRARIES}
    ${FMT_LIBRARIES}
    Threads::Threads
)

# Enable optimizations for accurate benchmarking
if(MSVC)
    target_compile_options(slick_logger_benchmark PRIVATE /O2 /DNDEBUG)
else()
    target_compile_options(slick_logger_benchmark PRIVATE -O3 -march=native -DNDEBUG)
endif()

# Copy test data files if they exist
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_data)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/test_data 
         DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Individual micro-benchmarks
add_executable(latency_benchmark latency_benchmark.cpp)
target_link_libraries(latency_benchmark 
    slick::logger
    benchmark_utils
    ${SPDLOG_LIBRARIES}
)

add_executable(throughput_benchmark throughput_benchmark.cpp)
target_link_libraries(throughput_benchmark 
    slick::logger
    benchmark_utils
    ${SPDLOG_LIBRARIES}
)

add_executable(memory_benchmark memory_benchmark.cpp)
target_link_libraries(memory_benchmark 
    slick::logger
    benchmark_utils
    ${SPDLOG_LIBRARIES}
)

# Set release flags for all benchmark executables
if(MSVC)
    target_compile_options(latency_benchmark PRIVATE /O2 /DNDEBUG)
    target_compile_options(throughput_benchmark PRIVATE /O2 /DNDEBUG)
    target_compile_options(memory_benchmark PRIVATE /O2 /DNDEBUG)
else()
    target_compile_options(latency_benchmark PRIVATE -O3 -march=native -DNDEBUG)
    target_compile_options(throughput_benchmark PRIVATE -O3 -march=native -DNDEBUG)
    target_compile_options(memory_benchmark PRIVATE -O3 -march=native -DNDEBUG)
endif()