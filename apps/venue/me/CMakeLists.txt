# hft_me
add_executable(hft_me main.cpp matching_engine.cpp order_book.cpp)
target_link_libraries(hft_me PRIVATE hft::common hft::ipc hft::protocol)
hft_set_warnings(hft_me)
