# hft_og
add_executable(hft_og main.cpp order_gateway.cpp oms.cpp)
target_link_libraries(hft_og PRIVATE hft::common hft::ipc hft::protocol)
hft_set_warnings(hft_og)
