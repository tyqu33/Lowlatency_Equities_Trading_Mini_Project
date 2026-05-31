# config_demo — smoke test proving the CLI11 / toml++ / fmt tooling is wired correctly.
add_executable(hft_config_demo main.cpp)
target_link_libraries(hft_config_demo PRIVATE hft::appkit fmt::fmt)
hft_set_warnings(hft_config_demo)
