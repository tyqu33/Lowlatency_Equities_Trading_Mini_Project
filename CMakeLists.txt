cmake_minimum_required(VERSION 3.20)

project(hft
    VERSION 0.1.0
    DESCRIPTION "Low-latency US-equities trading stack (simulated venue + participant)"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(HFT_BUILD_TESTS      "Build unit tests (GoogleTest)"        OFF)
option(HFT_BUILD_BENCHMARKS "Build benchmarks (Google Benchmark)"  OFF)
option(HFT_ENABLE_WARNINGS  "Enable a strict warning set"          ON)
option(HFT_ENABLE_SANITIZERS "Enable ASan/UBSan"                   OFF)
option(HFT_NATIVE_ARCH      "Compile with -march=native"           OFF)

# ---------------------------------------------------------------------------
# Global C++ settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# All executables land in build/bin/ for predictable run paths.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Default to an optimized-with-debug-info build for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CompilerWarnings)   # provides hft_set_warnings(<target>)
include(Sanitizers)         # applies ASan/UBSan flags when enabled
include(Dependencies)       # fetches gtest/benchmark when tests/benchmarks are on

# ---------------------------------------------------------------------------
# Common libraries (shared by both sides)
# ---------------------------------------------------------------------------
add_subdirectory(libs/common)     # hft::common   — Price, symbol, types, time, cpu
add_subdirectory(libs/ipc)        # hft::ipc       — SPSC/SPMC rings + shared memory
add_subdirectory(libs/protocol)   # hft::protocol  — OUCH / ITCH / risk wire structs
add_subdirectory(libs/matching)   # hft::matching  — price-time-priority limit order book
add_subdirectory(libs/appkit)     # hft::appkit    — CLI (CLI11) + TOML (toml++) + fmt plumbing

# ---------------------------------------------------------------------------
# VENUE side (the simulated exchange)
# ---------------------------------------------------------------------------
add_subdirectory(apps/venue/me)   # ME  — matching engine + central book
add_subdirectory(apps/venue/mdp)  # MDP — ITCH market-data publisher

# ---------------------------------------------------------------------------
# PARTICIPANT side (broker + buy-side; maps to UFT / UFR / refdata node)
# ---------------------------------------------------------------------------
add_subdirectory(apps/participant/fh)     # FH    — feed handler + local book
add_subdirectory(apps/participant/strat)  # STRAT — strategy (market maker)
add_subdirectory(apps/participant/risk)   # RISK  — SEC 15c3-5 pre-trade risk (= UFR)
add_subdirectory(apps/participant/og)     # OG    — order gateway + OMS (= UFT)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
add_subdirectory(tools/sim_client)        # background order-flow generator
add_subdirectory(tools/config_demo)       # smoke test for the CLI / TOML / fmt tooling
add_subdirectory(tools/book_demo)         # replay an order script through the book, print each step

# ---------------------------------------------------------------------------
# Tests / benchmarks (opt-in; pull external deps only when enabled)
# ---------------------------------------------------------------------------
if(HFT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(HFT_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
message(STATUS "hft ${PROJECT_VERSION}  |  tests=${HFT_BUILD_TESTS}  "
               "benchmarks=${HFT_BUILD_BENCHMARKS}  sanitizers=${HFT_ENABLE_SANITIZERS}")
