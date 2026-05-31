# hft::appkit — application bootstrap: CLI parsing (CLI11) + TOML config (toml++) + fmt output.
# This is the "buy, don't build" plumbing; apps link it for their main()s. NOT on the hot path.
add_library(hft_appkit STATIC
    src/cli.cpp
    src/config.cpp)
add_library(hft::appkit ALIAS hft_appkit)
target_include_directories(hft_appkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hft_appkit PUBLIC cxx_std_20)
target_link_libraries(hft_appkit PUBLIC
    fmt::fmt
    CLI11::CLI11
    tomlplusplus::tomlplusplus)
hft_set_warnings(hft_appkit)
