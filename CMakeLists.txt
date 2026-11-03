cmake_minimum_required(VERSION 3.20)
project(RTES VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags (portable by default; see RTES_NATIVE)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g")

option(RTES_NATIVE "Build with -march=native (binaries only run on this CPU)" OFF)
option(RTES_WERROR "Treat compiler warnings as errors" OFF)

# Warning and arch flags applied to every RTES target (not to third-party code)
add_library(rtes_project_options INTERFACE)
target_compile_options(rtes_project_options INTERFACE -Wall -Wextra -Wpedantic)
if(RTES_WERROR)
    target_compile_options(rtes_project_options INTERFACE -Werror)
endif()
if(RTES_NATIVE)
    target_compile_options(rtes_project_options INTERFACE -march=native)
endif()

# Find packages
find_package(Threads REQUIRED)

# Include directories
include_directories(include)

# --------------------------------------------------------------------------
# Core library — explicit list so cmake doesn't need re-running on new files
# --------------------------------------------------------------------------
set(CORE_SOURCES
    src/order_book.cpp
    src/error_handling.cpp
    src/config.cpp
    src/risk_manager.cpp
    src/matching_engine.cpp
    src/gateway_core.cpp
    src/tcp_gateway.cpp
    src/udp_publisher.cpp
    src/exchange.cpp
    src/strategies.cpp
)

add_library(rtes_core ${CORE_SOURCES})
target_link_libraries(rtes_core PUBLIC Threads::Threads PRIVATE rtes_project_options)

# --------------------------------------------------------------------------
# Main exchange executable
# --------------------------------------------------------------------------
add_executable(trading_exchange src/main.cpp)
target_link_libraries(trading_exchange PRIVATE rtes_core rtes_project_options)

# --------------------------------------------------------------------------
# Benchmark tools
# --------------------------------------------------------------------------
add_executable(bench_memory_pool tools/bench_memory_pool.cpp)
target_link_libraries(bench_memory_pool PRIVATE rtes_core rtes_project_options)

add_executable(bench_book tools/bench_book.cpp)
target_link_libraries(bench_book PRIVATE rtes_core rtes_project_options)

add_executable(bench_exchange tools/bench_exchange.cpp)
target_link_libraries(bench_exchange PRIVATE rtes_core rtes_project_options)

# --------------------------------------------------------------------------
# Client tools
# --------------------------------------------------------------------------
add_executable(tcp_client tools/tcp_client.cpp)
target_link_libraries(tcp_client PRIVATE rtes_core rtes_project_options)

add_executable(client_simulator tools/client_simulator.cpp)
target_link_libraries(client_simulator PRIVATE rtes_core rtes_project_options)

add_executable(udp_receiver tools/udp_receiver.cpp)
target_link_libraries(udp_receiver PRIVATE rtes_core rtes_project_options)

# --------------------------------------------------------------------------
# Unit tests
# --------------------------------------------------------------------------
enable_testing()
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
endif()

if(GTest_FOUND OR TARGET gtest)
    file(GLOB_RECURSE TEST_SOURCES "tests/*.cpp")
    add_executable(rtes_tests ${TEST_SOURCES})
    if(TARGET GTest::gtest)
        target_link_libraries(rtes_tests PRIVATE rtes_core rtes_project_options GTest::gtest GTest::gtest_main)
    else()
        target_link_libraries(rtes_tests PRIVATE rtes_core rtes_project_options gtest gtest_main)
    endif()
    include(GoogleTest)
    gtest_discover_tests(rtes_tests)

    # Keep the benchmark working: a tiny run, no numbers checked.
    add_test(NAME bench_book_smoke COMMAND bench_book --ops 2000 --depth 200)
endif()
