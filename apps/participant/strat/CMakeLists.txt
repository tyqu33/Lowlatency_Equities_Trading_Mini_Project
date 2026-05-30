# hft_strat
add_executable(hft_strat main.cpp market_maker.cpp)
target_link_libraries(hft_strat PRIVATE hft::common hft::ipc hft::protocol)
hft_set_warnings(hft_strat)
