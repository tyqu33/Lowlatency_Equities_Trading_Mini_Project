# Dependencies.cmake
# External dependencies are fetched only when the feature that needs them is
# enabled, so a plain `cmake -S . -B build` (skeleton) builds offline with no deps.

include(FetchContent)

# --- GoogleTest (unit tests) ------------------------------------------------
if(HFT_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# --- Google Benchmark (microbenchmarks) ------------------------------------
if(HFT_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.0)
    FetchContent_MakeAvailable(benchmark)
endif()

# --- Planned for later milestones (intentionally not wired yet) -------------
#   Boost.Interprocess   shared-memory segments        -> libs/ipc
#   Quill                low-latency async logging      -> hot-path logging
#   HdrHistogram         p50/p99/p99.9 latency stats    -> benchmarks
#   QuickFIX / Hffix     FIX 4.4 order entry (V2)        -> participant gateway
