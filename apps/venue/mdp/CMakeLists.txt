# hft_mdp
add_executable(hft_mdp main.cpp md_publisher.cpp itch_encoder.cpp)
target_link_libraries(hft_mdp PRIVATE hft::common hft::ipc hft::protocol)
hft_set_warnings(hft_mdp)
