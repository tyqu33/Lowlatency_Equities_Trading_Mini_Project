# hft_sim_client
add_executable(hft_sim_client main.cpp flow_generator.cpp)
target_link_libraries(hft_sim_client PRIVATE hft::common hft::ipc hft::protocol)
hft_set_warnings(hft_sim_client)
