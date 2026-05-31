# Dependencies.cmake
# Application plumbing (fmt / toml++ / CLI11) is fetched unconditionally — these are core to
# running any process. Tests (GoogleTest+GoogleMock) and benchmarks (Google Benchmark) are
# fetched only when their option is ON.
#
# NOTE: the first `cmake -S . -B build` needs network access to clone these (then cached under
# build/_deps). This is no longer a zero-dependency offline build.

include(FetchContent)

# --- Always-on application plumbing: formatting / config / CLI --------------
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.0.2)
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0)
FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2)
FetchContent_MakeAvailable(fmt tomlplusplus cli11)

# --- GoogleTest + GoogleMock (unit tests) — only when enabled ---------------
if(HFT_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)  # provides GTest::gtest, GTest::gmock, GTest::gmock_main
endif()

# --- Google Benchmark (microbenchmarks) — only when enabled -----------------
if(HFT_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.0)
    FetchContent_MakeAvailable(benchmark)   # provides benchmark::benchmark, benchmark::benchmark_main
endif()

# --- Planned for later milestones (intentionally not wired yet) -------------
#   Boost.Interprocess   shared-memory segments        -> libs/ipc
#   Quill                low-latency async logging      -> hot-path logging
#   HdrHistogram         p50/p99/p99.9 latency stats    -> benchmarks
#   QuickFIX / Hffix     FIX 4.4 order entry (V2)        -> participant gateway
